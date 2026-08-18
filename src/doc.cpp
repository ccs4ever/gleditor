#include <algorithm>                      // for min, max
#include <cmath>                          // for ceil, lround
#include <cstddef>                        // for byte
#include <cstdint>                        // for uint32_t
#include <format>                         // for format
#include <gleditor/animation.hpp>         // for docArrival, docArrivalDepth
#include <gleditor/doc.hpp>               // IWYU pragma: associated
#include <gleditor/document_observer.hpp> // for DocumentObserver
#include <gleditor/render/device.hpp>     // for RenderDevice
#include <gleditor/render_state.hpp>      // for RenderState
#include <gleditor/renderer.hpp>          // for Renderer, RendererRef
#include <gleditor/text_source.hpp>       // for TextSource
#include <gleditor/utf8.hpp>              // for alignToCharacterStart
#include <glm/detail/qualifier.hpp>       // for qualifier
#include <glm/ext/matrix_float4x4.hpp>    // for mat4
#include <glm/ext/vector_float3.hpp>      // for vec3
#include <glm/gtc/type_ptr.hpp>
#include <iostream> // for basic_ostream, operator<<
#include <limits>
#include <memory> // for __shared_ptr_access, shared...
#include <mutex>
#include <pango/pangoft2.h> // for pango_ft2_font_map_new
#include <span>             // for span
#include <stdexcept>        // for logic_error
#include <string>           // for char_traits, basic_string
#include <string_view>      // for string_view
#include <utility>          // for move
#include <vector>           // for vector

#include "glibmm/convert.h"          // for get_charset
#include "glibmm/fileutils.h"        // for file_get_contents
#include "glibmm/refptr.h"           // for RefPtr
#include "glibmm/ustring.h"          // for ustring, operator==, UStrin...
#include "pango/pango-layout.h"      // for pango_layout_set_text
#include "pango/pango-types.h"       // for PANGO_SCALE
#include "pango/pangofc-fontmap.h"   // for pango_fc_font_map_set_config
#include "pangomm/attributes.h"      // for AttrFontDesc, Attribute
#include "pangomm/attrlist.h"        // for AttrList
#include "pangomm/fontdescription.h" // for FontDescription
#include "pangomm/fontmap.h"         // for FontMap
#include "pangomm/layout.h"          // for Layout, EllipsizeMode
#include "pangomm/layoutiter.h"      // for LayoutIter
#include "pangomm/layoutline.h"      // for LayoutLine
#include "pangomm/rectangle.h"       // for Rectangle
#include <fontconfig/fontconfig.h>
#include <gleditor/caret.hpp>            // for Caret
#include <gleditor/drawable.hpp>         // for Drawable
#include <gleditor/glyphcache/cache.hpp> // for GlyphCache
#include <gleditor/glyphcache/types.hpp> // for TextureCoords, PointF, Rect
#include <glm/gtx/string_cast.hpp>

namespace {

/**
 * @brief Rows the document's vertex buffer starts out with.
 *
 * A page's worth. A document that is about to load text says how much it needs
 * before it lays anything out, so this only has to cover an empty document
 * being typed into.
 */
constexpr std::uint32_t initialPoolRows = 4096;

/**
 * @brief Rows a document of @p characters will want, near enough to allocate.
 *
 * One quad per cluster, and a cluster is a character except where several
 * combine into one -- so the character count is an over-estimate of the glyphs
 * and the pages add a background and a bar per line on top. An eighth covers
 * the bars and the room the pool leaves around each page to grow into, and
 * being a little over is the point: the buffer is allocated once at this size
 * instead of being grown through every size on the way there, and whatever is
 * left over is given back by a trim when the document has finished loading.
 *
 * Characters rather than bytes, so that text outside ASCII is not over-counted
 * threefold.
 */
std::uint32_t rowsFor(const std::size_t characters) {
  const auto estimate = characters + (characters / 8) + initialPoolRows;
  return static_cast<std::uint32_t>(std::min<std::size_t>(
      estimate, std::numeric_limits<std::uint32_t>::max()));
}

/// Margin in layout pixels between the page edge and its text.
constexpr float pageMargin = 24.0F;

/// How far in front of the page background its glyphs and bars sit, in the
/// same layout-pixel space. Small enough to be a depth tie-break rather than a
/// visible offset, and part of the box the frustum test uses. A quad carries
/// which step it is on, not the distance itself.
constexpr float glyphDepth = Doc::VBORow::depthStep;

/**
 * @brief Shade a line's bar takes, given how much of its ink box the glyphs
 *        cover.
 *
 * White paper darkened in proportion to the ink it would have carried. Nothing
 * here is tuned: the coverage comes from the glyph boxes the detailed path
 * places and from the mean coverage the glyph cache measured when it rasterised
 * each cluster, so a change of font or size carries through on its own. Two
 * earlier attempts did have a constant in them -- a flat shade, then a flat
 * assumption about how much of its box a glyph inks -- and each was tuned right
 * on one sample and ten to forty levels out on the other.
 */
unsigned char greekedShade(const float coverage) {
  const auto inked = coverage < 0.0F ? 0.0F : std::min(1.0F, coverage);
  return static_cast<unsigned char>(std::lround(255.0F * (1.0F - inked)));
}

/// Convert Pango units to pixels.
double toPixels(const int pangoUnits) {
  return static_cast<double>(pangoUnits) / PANGO_SCALE;
}

/// Byte offset of the character after the one starting at @p pos. Always
/// advances, so a caller stepping through text cannot get stuck on a malformed
/// byte.
std::size_t nextCharacter(const std::string_view text, std::size_t pos) {
  pos = std::min(pos + 1, text.size());
  while (pos < text.size() &&
         0x80 == (static_cast<unsigned char>(text[pos]) & 0xC0)) {
    pos++;
  }
  return pos;
}

/// Number of characters in a UTF-8 range, counting lead bytes.
std::size_t utf8Length(const std::string_view text) {
  return static_cast<std::size_t>(
      std::ranges::count_if(text, [](const char chr) {
        return 0x80 != (static_cast<unsigned char>(chr) & 0xC0);
      }));
}

/// View a row vector as the raw bytes the buffer pool wants.
std::span<const std::byte> asBytes(const std::vector<Doc::VBORow> &rows) {
  return {reinterpret_cast<const std::byte *>(rows.data()),
          rows.size() * sizeof(Doc::VBORow)};
}

/// Copy a matrix into the flat array the device uniform structs carry.
std::array<float, 16> toArray(const glm::mat4 &mat) {
  std::array<float, 16> out{};
  const auto *src = glm::value_ptr(mat);
  std::copy_n(src, out.size(), out.begin());
  return out;
}

} // namespace

// Neither header can see the other, so the two ends of the layer limit are
// tied together here: the atlas refuses to grow past what the packing can name.
static_assert(GlyphCache::maxEncodableLayers ==
                  static_cast<int>(Doc::VBORow::maxAtlasLayers),
              "the glyph cache's layer ceiling and the vertex packing's layer "
              "field have drifted apart");

// A quad carries its kind in two bits and takes the rest of its picking
// identity from the draw, so a fifth kind would need a bit that is not there.
static_assert(render::tagKindGlyph < 4 && render::tagKindPage < 4 &&
                  render::tagKindOverlay < 4,
              "a tag kind no longer fits the two bits a quad carries");

render::VertexLayout Doc::vertexLayout() {
  using render::AttributeType;
  static_assert(sizeof(Doc::VBORow) == 24,
                "the instance record is the renderer's dominant memory cost; "
                "growing it costs a megabyte per twenty thousand characters");

  render::VertexLayout layout;
  layout.stride     = sizeof(VBORow);
  layout.attributes = {
      {"position", 0, AttributeType::Float, 2, offsetof(VBORow, pos)},
      {"foreground", 1, AttributeType::UnsignedInt, 1,
       offsetof(VBORow, foreground)},
      {"atlas", 2, AttributeType::UnsignedInt, 1, offsetof(VBORow, atlas)},
      {"quad", 3, AttributeType::UnsignedInt, 1, offsetof(VBORow, quad)},
      {"paper", 4, AttributeType::UnsignedInt, 1, offsetof(VBORow, paper)},
  };
  return layout;
}

PageShaping Doc::extractPageShaping(const Glib::RefPtr<Pango::Layout> &layout,
                                    const std::string_view text) {
  PageShaping shaping;
  layout->get_pixel_size(shaping.textWidthPx, shaping.textHeightPx);
  shaping.limit =
      std::min<std::size_t>(text.size(), Doc::consumedBytes(layout));
  shaping.lineCount =
      static_cast<std::size_t>(std::max(0, layout->get_line_count()));

  std::size_t lineOfCluster = 0;
  const auto lineStartAt    = [&layout](const std::size_t index) {
    const auto &line = layout->get_const_line(static_cast<int>(index));
    return line ? static_cast<std::size_t>(line->get_start_index())
                : std::numeric_limits<std::size_t>::max();
  };
  auto nextLineStart = shaping.lineCount > 1
                           ? lineStartAt(1)
                           : std::numeric_limits<std::size_t>::max();

  auto iter = layout->get_iter();
  while (true) {
    Pango::Rectangle clusterInk;
    Pango::Rectangle clusterLogical;
    iter.get_cluster_extents(clusterInk, clusterLogical);

    const auto start = static_cast<std::size_t>(std::max(0, iter.get_index()));
    const bool more  = iter.next_cluster();
    const auto end =
        std::min(shaping.limit,
                 more ? static_cast<std::size_t>(std::max(0, iter.get_index()))
                      : nextCharacter(text, start));

    if (start >= shaping.limit) {
      break;
    }
    if (end <= start) {
      if (!more) {
        break;
      }
      continue;
    }

    std::size_t drawEnd = end;
    while (drawEnd > start &&
           ('\n' == text[drawEnd - 1] || '\r' == text[drawEnd - 1])) {
      drawEnd--;
    }

    if (shaping.clusters.size() >= Doc::VBORow::maxClustersPerPage) {
      shaping.limit = start;
      break;
    }

    shaping.clusters.push_back(
        ClusterBox{static_cast<std::uint32_t>(start),
                   static_cast<std::uint32_t>(end - start),
                   static_cast<std::uint32_t>(
                       utf8Length(text.substr(start, end - start)))});

    if (drawEnd == start) {
      if (!more) {
        break;
      }
      continue;
    }

    if (drawEnd - start > GlyphCache::maxClusterBytes) {
      if (!more) {
        break;
      }
      continue;
    }

    while (start >= nextLineStart && lineOfCluster + 1 < shaping.lineCount) {
      lineOfCluster++;
      nextLineStart = lineOfCluster + 1 < shaping.lineCount
                          ? lineStartAt(lineOfCluster + 1)
                          : std::numeric_limits<std::size_t>::max();
    }

    shaping.glyphs.push_back(PageShaping::GlyphEntry{
        .chr          = std::string(text.substr(start, drawEnd - start)),
        .clusterLeft  = static_cast<float>(toPixels(clusterLogical.get_x())),
        .clusterTop   = static_cast<float>(toPixels(clusterLogical.get_y())),
        .clusterIndex = shaping.clusters.size() - 1,
        .lineIndex    = lineOfCluster,
    });

    if (!more) {
      break;
    }
  }

  {
    auto lineIter         = layout->get_iter();
    std::size_t lineIndex = 0;
    do {
      const auto &line = layout->get_const_line(static_cast<int>(lineIndex));
      if (!line ||
          static_cast<std::size_t>(line->get_start_index()) >= shaping.limit) {
        break;
      }
      const auto curIndex = lineIndex;
      lineIndex++;

      Pango::Rectangle ink;
      Pango::Rectangle logical;
      lineIter.get_line_extents(ink, logical);

      const auto barWidth  = static_cast<float>(toPixels(ink.get_width()));
      const auto barHeight = static_cast<float>(toPixels(ink.get_height()));
      if (0.0F >= barWidth || 0.0F >= barHeight) {
        continue;
      }
      shaping.lines.push_back(PageShaping::LineEntry{
          .barWidth  = barWidth,
          .barHeight = barHeight,
          .left      = static_cast<float>(toPixels(ink.get_x())),
          .top       = static_cast<float>(toPixels(ink.get_y())),
          .lineIndex = curIndex,
      });
    } while (lineIter.next_line());
  }

  return shaping;
}

Page::Page(std::shared_ptr<Doc> aDoc, RenderState &state, glm::mat4 &model,
           PageShaping aShaping, const std::uint32_t aTextOffset,
           const std::uint32_t aPageIndex,
           const BufferPool::Allocation &inherited)
    : Drawable(model), doc(std::move(aDoc)), pageBacking(inherited),
      textOffset(aTextOffset), pageIndex(aPageIndex) {
  const auto color = Doc::VBORow::color;
  const auto box   = Doc::VBORow::box;

  pageWidth  = static_cast<float>(aShaping.textWidthPx) + (2 * pageMargin);
  pageHeight = static_cast<float>(aShaping.textHeightPx) + (2 * pageMargin);

  originX = -pageWidth / 2.0F;
  originY = pageHeight / 2.0F;

  identity = render::packTagIdentity(0, this->doc->documentIndex(), aPageIndex);

  std::vector<Doc::VBORow> vertexData;
  const auto pushBackground = [&] {
    vertexData.push_back(
        Doc::VBORow{{0.0F, 0.0F},
                    Doc::VBORow::fill(color(255), Doc::VBORow::onPaper),
                    0,
                    box(0,
                        std::min(Doc::VBORow::maxQuadExtent,
                                 static_cast<unsigned int>(pageWidth)),
                        std::min(Doc::VBORow::maxQuadExtent,
                                 static_cast<unsigned int>(pageHeight)),
                        render::tagKindPage),
                    Doc::VBORow::paperAt(color(255), 0)});
  };
  pushBackground();

  thread_local static auto renderFontMap =
      Glib::wrap(PANGO_FONT_MAP(pango_ft2_font_map_new()));
  thread_local static auto renderFontCtx = renderFontMap->create_context();
  const auto fontDesc =
      Pango::FontDescription(this->doc->renderer->defaultFontName().data());
  const auto font = renderFontCtx->load_font(fontDesc);

  const auto extent = [](const float value) {
    return static_cast<unsigned int>(std::clamp(
        value, 0.0F, static_cast<float>(Doc::VBORow::maxQuadExtent)));
  };

  std::vector<float> lineInk(aShaping.lineCount, 0.0F);

  clusters = std::move(aShaping.clusters);

  for (const auto &g : aShaping.glyphs) {
    const auto glyph    = state.glyphCache.put(g.chr, font);
    const auto &coords  = glyph.texCoords;
    const auto &extents = glyph.dims;

    const auto glyphWidth = static_cast<float>(static_cast<int>(extents.width));
    const auto glyphHeight =
        static_cast<float>(static_cast<int>(extents.height));

    if (0.0F < glyphWidth && 0.0F < glyphHeight) {
      if (g.lineIndex < lineInk.size()) {
        lineInk[g.lineIndex] += glyphWidth * glyphHeight * glyph.ink;
      }
      const auto left = pageMargin + g.clusterLeft;
      const auto top  = pageMargin + g.clusterTop;

      vertexData.push_back(Doc::VBORow{
          {originX + left + (glyphWidth / 2.0F),
           originY - (top + (glyphHeight / 2.0F))},
          Doc::VBORow::ink(color(0), Doc::VBORow::onText, false),
          Doc::VBORow::atlasAt(static_cast<unsigned int>(coords.topLeft.x),
                               static_cast<unsigned int>(coords.topLeft.y)),
          box(static_cast<unsigned char>(glyph.layer), extent(glyphWidth),
              extent(glyphHeight), render::tagKindGlyph),
          Doc::VBORow::paperAt(color(255),
                               static_cast<unsigned int>(g.clusterIndex))});
    }
  }

  textBytes       = static_cast<std::uint32_t>(aShaping.limit);
  detailInstances = static_cast<std::uint32_t>(vertexData.size());

  pushBackground();
  for (const auto &line : aShaping.lines) {
    const auto inkArea =
        line.lineIndex < lineInk.size() ? lineInk[line.lineIndex] : 0.0F;
    const auto left  = pageMargin + line.left;
    const auto top   = pageMargin + line.top;
    const auto shade = greekedShade(inkArea / (line.barWidth * line.barHeight));
    vertexData.push_back(
        Doc::VBORow{{originX + left + (line.barWidth / 2.0F),
                     originY - (top + (line.barHeight / 2.0F))},
                    Doc::VBORow::fill(color(shade), Doc::VBORow::onText),
                    0,
                    box(0, extent(line.barWidth), extent(line.barHeight),
                        render::tagKindPage),
                    Doc::VBORow::paperAt(color(shade), 0)});
  }

  coarseInstances =
      static_cast<std::uint32_t>(vertexData.size()) - detailInstances;
  if (1 >= coarseInstances) {
    vertexData.resize(detailInstances);
    coarseInstances = 0;
  }

  const auto rows = static_cast<std::uint32_t>(vertexData.size());
  if (pageBacking.empty()) {
    pageBacking = this->doc->pool->reserve(rows);
  } else {
    this->doc->pool->resize(pageBacking, rows, BufferPool::Contents::Discard);
  }
  this->doc->pool->write(pageBacking, 0, asBytes(vertexData));
}

Page::Page(std::shared_ptr<Doc> aDoc, RenderState &state, glm::mat4 &model,
           Glib::RefPtr<Pango::Layout> aLayout, const std::uint32_t aTextOffset,
           const std::uint32_t aPageIndex,
           const BufferPool::Allocation &inherited)
    : Page(std::move(aDoc), state, model,
           Doc::extractPageShaping(aLayout, aLayout->get_text().raw()),
           aTextOffset, aPageIndex, inherited) {}

bool Page::contains(const std::uint32_t globalOffset) const {
  // The end of the last page is a valid caret position, so the upper bound is
  // inclusive there and exclusive everywhere else -- otherwise the caret could
  // not be put after the final character.
  return globalOffset >= textOffset && globalOffset <= textOffset + textBytes;
}

void Doc::keepLayoutOf(const std::uint32_t pageIndex) const {
  // Already the most recent, so there is nothing to record and nothing to
  // drop; a caret sitting still asks its page for a layout every frame.
  if (!liveLayouts.empty() && liveLayouts.back() == pageIndex) {
    return;
  }
  std::erase(liveLayouts, pageIndex);
  liveLayouts.push_back(pageIndex);

  while (liveLayouts.size() > maxLiveLayouts) {
    const auto oldest = liveLayouts.front();
    liveLayouts.pop_front();
    // Pages are renumbered by a reflow, so an index recorded before one may
    // now name a different page or none at all. Dropping the wrong page's
    // layout costs it a reshaping and nothing else, and dropping none is only
    // a page kept a little longer.
    if (oldest < pages.size()) {
      pages[oldest].dropLayout();
    }
  }
}

std::string_view Page::pageText() const {
  const std::string_view whole{doc->contents().raw()};
  if (textOffset >= whole.size()) {
    return {};
  }
  return whole.substr(textOffset, textBytes);
}

Glib::RefPtr<Pango::Layout> Page::ensureLayout() const {
  if (!layout) {
    // The same call, on the same bytes, that produced this page in the first
    // place, so what comes back is what was drawn.
    layout = doc->layoutFrom(textOffset);
    doc->keepLayoutOf(pageIndex);
  }
  return layout;
}

bool Page::caretGeometry(const std::uint32_t globalOffset, float &posX,
                         float &posY, float &height) const {
  // Whether the caret is on this page is answered before the page is shaped
  // again, or drawing a caret would shape every page of the document to find
  // the one page it is on.
  if (!contains(globalOffset)) {
    return false;
  }
  const auto shaped = ensureLayout();
  if (!shaped) {
    return false;
  }
  Pango::Rectangle strong;
  Pango::Rectangle weak;
  shaped->get_cursor_pos(static_cast<int>(globalOffset - textOffset), strong,
                         weak);

  const auto left = pageMargin + static_cast<float>(toPixels(strong.get_x()));
  const auto top  = pageMargin + static_cast<float>(toPixels(strong.get_y()));
  const auto tall = static_cast<float>(toPixels(strong.get_height()));

  posX   = originX + left + (Caret::widthPixels / 2.0F);
  posY   = originY - (top + (tall / 2.0F));
  height = tall;
  return true;
}

std::optional<std::uint32_t>
Page::offsetForCluster(const std::uint32_t clusterIndex,
                       const float fraction) const {
  if (clusterIndex >= clusters.size()) {
    return std::nullopt;
  }
  const auto &cluster = clusters[clusterIndex];

  const auto steps = render::clusterCharStep(cluster.charCount, fraction);

  // Walk that many characters into the cluster. The byte length of a
  // character varies, so the boundary cannot be computed arithmetically.
  auto offset = static_cast<std::size_t>(cluster.byteStart);
  const auto end =
      static_cast<std::size_t>(cluster.byteStart + cluster.byteLength);
  const auto text = pageText();
  for (std::uint32_t taken = 0; taken < steps && offset < end;) {
    offset++;
    while (offset < end &&
           0x80 == (static_cast<unsigned char>(text[offset]) & 0xC0)) {
      offset++;
    }
    taken++;
  }

  return textOffset + static_cast<std::uint32_t>(offset);
}

// Always called from the render thread
void Page::collect(std::vector<render::GlyphBatch> &batches,
                   const glm::mat4 &docTransform, const float opacity,
                   const DrawBudget &budget, DrawStats &stats) const {
  if (0 == detailInstances) {
    return;
  }
  stats.pages++;

  const auto mvp = docTransform * model;
  // The page is flat: a rectangle in x and y, with the glyphs sitting just in
  // front of the background in z.
  if (budget.cull &&
      outsideFrustum(mvp, pageWidth / 2.0F, pageHeight / 2.0F, glyphDepth)) {
    stats.culled++;
    return;
  }

  const bool coarse =
      0 != coarseInstances &&
      screenScaleAt(mvp, budget.screenWidth) < budget.coarseBelow;
  if (coarse) {
    stats.coarse++;
  } else {
    stats.detailed++;
  }

  // Both draws live in the one allocation, the coarse one straight after the
  // detailed one, so choosing between them is a matter of where the draw
  // starts and how many instances it covers.
  const auto first = coarse ? detailInstances : 0U;
  const auto count = coarse ? coarseInstances : detailInstances;
  batches.push_back(render::GlyphBatch{
      render::DrawUniforms{toArray(mvp), opacity, identity},
      doc->pool->buffer(),
      doc->pool->byteOffset(pageBacking) + (first * sizeof(Doc::VBORow)),
      count});
}

// Always called from the render thread
void Doc::collect(std::vector<render::GlyphBatch> &batches,
                  const glm::mat4 &viewProjection, const DrawBudget &budget,
                  DrawStats &stats) const {
  // Fully faded out contributes nothing, so it is skipped before any page is
  // transformed rather than submitted and blended away to nothing.
  if (opacity() <= 0.0F) {
    return;
  }
  const auto docTransform = viewProjection * modelMatrix();
  for (const auto &page : pages) {
    page.collect(batches, docTransform, opacity(), budget, stats);
  }
}

glm::mat4 Doc::modelMatrix() const {
  return glm::translate(glm::mat4(1.0F), position());
}

std::optional<Doc::Anchor>
Doc::anchorFor(const std::uint32_t globalOffset) const {
  for (std::size_t i = 0; i < pages.size(); i++) {
    // Asked page by page rather than by searching, because the same call
    // decides whether the offset is on the page and where -- and the deciding
    // half is answered without shaping anything.
    Anchor anchor{static_cast<std::uint32_t>(i), 0.0F, 0.0F, 0.0F};
    if (pages[i].caretGeometry(globalOffset, anchor.x, anchor.y,
                               anchor.height)) {
      return anchor;
    }
  }
  return std::nullopt;
}

std::optional<glm::vec3> Doc::worldPoint(const std::uint32_t pageIndex,
                                         const float posX,
                                         const float posY) const {
  if (pageIndex >= pages.size()) {
    return std::nullopt;
  }
  const auto point = modelMatrix() * pages[pageIndex].getModel() *
                     glm::vec4(posX, posY, 0.0F, 1.0F);
  return glm::vec3(point);
}

void Doc::animateArrival(ch::Timeline &timeline) {
  // The resting place is the translation the constructor was handed; the base
  // class keeps that matrix untouched, so it stays available as the target no
  // matter where the animation has got to.
  const glm::vec3 target(model[3]);
  position = target + glm::vec3(0.0F, 0.0F, gleditor::anim::docArrivalDepth);
  opacity  = 0.0F;

  // Eased out rather than linear: the document decelerates into place, which
  // is what makes it read as arriving somewhere rather than being dragged.
  timeline.apply(&position).then<ch::RampTo>(target, gleditor::anim::docArrival,
                                             ch::EaseOutCubic());
  timeline.apply(&opacity).then<ch::RampTo>(1.0F, gleditor::anim::docArrival,
                                            ch::EaseOutQuad());
}

void Doc::animateDeparture(ch::Timeline &timeline) {
  closing = true;
  const glm::vec3 away =
      position() + glm::vec3(0.0F, 0.0F, gleditor::anim::docArrivalDepth);
  // Eased in rather than out, so the document lingers a moment and then leaves
  // quickly: the opposite shape to the arrival, which is what tells the two
  // apart at a glance.
  timeline.apply(&position).then<ch::RampTo>(away, gleditor::anim::docArrival,
                                             ch::EaseInQuad());
  timeline.apply(&opacity).then<ch::RampTo>(0.0F, gleditor::anim::docArrival,
                                            ch::EaseInQuad());
}

void Doc::animateMoveTo(ch::Timeline &timeline, const glm::vec3 &target) {
  // The resting place is recorded as well as animated towards: a later
  // arrival or departure reads it back out of the base matrix.
  model = glm::translate(glm::mat4(1.0F), target);
  // apply() replaces whatever motion was on this output, so a move that
  // interrupts another one continues from where that one had got to rather
  // than restarting from the old target.
  timeline.apply(&position).then<ch::RampTo>(target, gleditor::anim::docArrival,
                                             ch::EaseInOutQuad());
}

std::optional<render::HighlightRange>
Page::highlightFor(const std::uint32_t selStart, const std::uint32_t selEnd,
                   const std::uint32_t colour) const {
  if (selEnd <= selStart || clusters.empty()) {
    return std::nullopt;
  }
  // Clip the span to this page, in page-local bytes.
  const auto pageStart = textOffset;
  const auto pageEnd   = textOffset + textBytes;
  if (selEnd <= pageStart || selStart >= pageEnd) {
    return std::nullopt;
  }
  const auto localStart = std::max(selStart, pageStart) - textOffset;
  const auto localEnd   = std::min(selEnd, pageEnd) - textOffset;

  const auto text = pageText();

  std::optional<std::size_t> first;
  std::size_t last = 0;
  for (std::size_t i = 0; i < clusters.size(); i++) {
    const auto &box  = clusters[i];
    const auto begin = box.byteStart;
    const auto end   = box.byteStart + box.byteLength;
    // Half-open overlap: a cluster is covered when any of its bytes are.
    if (end <= localStart || begin >= localEnd) {
      continue;
    }
    if (!first) {
      first = i;
    }
    last = i;
  }
  if (!first) {
    return std::nullopt;
  }

  // Where inside the edge clusters the span begins and ends, counted in
  // characters so the edge cannot land mid-glyph of a ligature.
  const auto fractionInto = [&text](const ClusterBox &box,
                                    const std::uint32_t offset) -> float {
    if (0 == box.charCount) {
      return 0.0F;
    }
    const auto clamped =
        std::clamp(offset, box.byteStart, box.byteStart + box.byteLength);
    const auto chars = utf8Length(
        std::string_view(text).substr(box.byteStart, clamped - box.byteStart));
    return static_cast<float>(chars) / static_cast<float>(box.charCount);
  };

  render::HighlightRange range;
  range.identity     = render::packTagIdentity(render::tagKindGlyph,
                                               doc->documentIndex(), pageIndex);
  range.firstCluster = static_cast<std::uint32_t>(*first);
  range.lastCluster  = static_cast<std::uint32_t>(last);
  range.colour       = colour;
  range.startFraction = fractionInto(clusters[*first], localStart);
  range.endFraction   = fractionInto(clusters[last], localEnd);
  return range;
}

void Doc::highlightsFor(const std::uint32_t selStart,
                        const std::uint32_t selEnd, const std::uint32_t colour,
                        std::vector<render::HighlightRange> &out) const {
  for (const auto &page : pages) {
    if (auto range = page.highlightFor(selStart, selEnd, colour)) {
      out.push_back(*range);
    }
  }
}

std::optional<std::uint32_t>
Doc::offsetForPick(const render::PickingTag &tag) const {
  const auto *target = page(tag.pageIndex);
  if (nullptr == target) {
    return std::nullopt;
  }
  // A glyph resolves through its cluster; anything else a page draws is its
  // background, which has no character beneath it.
  if (render::tagKindGlyph != tag.kind) {
    return target->baseOffset();
  }
  return target->offsetForCluster(tag.clusterIndex, tag.fraction);
}

void Doc::drawCaret(RenderState &state, const glm::mat4 &viewProjection,
                    Caret &caret) const {
  if (!caret.active() || caret.documentIndex() != docIndex) {
    return;
  }
  for (const auto &pageOn : pages) {
    float posX   = 0.0F;
    float posY   = 0.0F;
    float height = 0.0F;
    if (!pageOn.caretGeometry(caret.byteOffset(), posX, posY, height)) {
      continue;
    }
    caret.setGeometry(posX, posY, height);
    caret.draw(state, viewProjection * modelMatrix() * pageOn.getModel());
    return;
  }
}

const char *reflowScopeName(const ReflowScope scope) {
  switch (scope) {
  case ReflowScope::Line:
    return "line";
  case ReflowScope::Page:
    return "page";
  case ReflowScope::Document:
    return "document";
  }
  return "unknown";
}

Glib::RefPtr<Pango::Layout> Doc::layoutFrom(const std::uint32_t offset) const {
  thread_local struct ThreadFontContext {
    Glib::RefPtr<Pango::FontMap> fontMap;

    Glib::RefPtr<Pango::FontMap> getFontMap() {
      if (!fontMap) {
        fontMap = Glib::wrap(PANGO_FONT_MAP(pango_ft2_font_map_new()));
      }
      return fontMap;
    }
  } threadCtx;

  const auto fontName = renderer->defaultFontName();
  const auto fontDesc = Pango::FontDescription(fontName.data());
  const auto ctx      = threadCtx.getFontMap()->create_context();
  ctx->set_font_description(fontDesc);

  auto lay = Pango::Layout::create(ctx);
  lay->set_font_description(fontDesc);
  lay->set_single_paragraph_mode(false);
  lay->set_height(std::ceil(139.70 * 11 * PANGO_SCALE));
  lay->set_width(std::ceil(139.70 * 8.5 * PANGO_SCALE));
  lay->set_ellipsize(Pango::EllipsizeMode::END);

  const char *txt = text.raw().c_str();
  const auto size = text.bytes();
  if (offset >= size) {
    lay->set_text("");
    return lay;
  }
  // The same slice makePages() uses. This path is the one an edit reflows
  // through, so leaving it handing Pango the whole document would have left
  // the fault in place for every keystroke in a long one.
  fillPage(lay, txt + offset, size - offset);
  int precomputeW = 0;
  int precomputeH = 0;
  lay->get_pixel_size(precomputeW, precomputeH);
  return lay;
}

std::uint32_t Doc::consumedBytes(const Glib::RefPtr<Pango::Layout> &layout) {
  // How much of the text this page actually shows.
  //
  // A page bounds its layout by height, and Pango implements that by
  // ellipsizing: the final line is cut short and replaced by an ellipsis, while
  // still reporting its full logical length. Taking that length made the page
  // claim bytes it never drew -- the following page then started past them, and
  // the cluster walk handed the glyph cache one bitmap covering the whole
  // remaining run, hundreds of texels wide and one per page.
  //
  // So an ellipsized last line belongs to the next page, not this one. Asking
  // whether the layout ellipsized costs nothing; counting the clusters that
  // survived would mean shaping the text here, on the loader thread, while the
  // render thread is shaping through the same global font map -- which
  // corrupts the heap inside fontconfig often enough to be caught in a handful
  // of runs.
  const auto lines = layout->get_line_count();
  if (0 >= lines) {
    return 0;
  }
  const auto &last = layout->get_const_line(lines - 1);
  const int len    = last->get_length();
  const auto whole = static_cast<std::uint32_t>(last->get_start_index() +
                                                (0 == len ? 1 : len));
  if (!layout->is_ellipsized()) {
    return whole;
  }
  // Everything before the cut line. A single line that ellipsizes has nothing
  // before it, and a page that consumed nothing would never advance, so the
  // whole line is taken in that case and the overrun tolerated.
  const auto upToCut = static_cast<std::uint32_t>(last->get_start_index());
  return 0 == upToCut ? whole : upToCut;
}

namespace {

/**
 * @brief Whether an insertion left every line break where it was.
 *
 * Line starts before the insertion are untouched; those at or after it shift
 * by exactly the bytes inserted. Comparing the two lists directly would always
 * differ, since the tail moves -- what matters is whether it moved by the
 * insertion and nothing more.
 */
bool sameLineBreaks(const std::vector<int> &before,
                    const std::vector<int> &after, const int at,
                    const int delta) {
  if (before.size() != after.size()) {
    return false;
  }
  for (std::size_t i = 0; i < before.size(); i++) {
    const int expected = before[i] <= at ? before[i] : before[i] + delta;
    if (after[i] != expected) {
      return false;
    }
  }
  return true;
}

} // namespace

std::vector<int> Doc::lineStarts(const Glib::RefPtr<Pango::Layout> &layout) {
  std::vector<int> starts;
  const int count = layout->get_line_count();
  starts.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; i++) {
    starts.push_back(layout->get_const_line(i)->get_start_index());
  }
  return starts;
}

void Doc::addObserver(gleditor::DocumentObserver *const observer) {
  if (nullptr == observer) {
    return;
  }
  if (std::find(observers.begin(), observers.end(), observer) ==
      observers.end()) {
    observers.push_back(observer);
  }
}

void Doc::removeObserver(gleditor::DocumentObserver *const observer) {
  observers.erase(std::remove(observers.begin(), observers.end(), observer),
                  observers.end());
}

std::vector<int> Doc::lineBreaksAround(const std::uint32_t at) const {
  // The line breaks of the page the edit lands on, as they are *now*.
  //
  // Taken before the text is spliced, and that is the whole point. A page
  // shapes itself again on demand, so asking it afterwards asks about text
  // that already contains the edit: the answer came back equal to the line
  // breaks after the edit, and the comparison that is supposed to tell a
  // line-local change from one that moved a word onto another line said "it
  // moved" every time. Whether it did depended on whether the page happened to
  // still be holding its layout, which is not something the answer should turn
  // on.
  if (pages.empty()) {
    return {};
  }
  std::size_t firstPage = 0;
  for (std::size_t i = 0; i < pages.size(); i++) {
    if (at >= pages[i].baseOffset()) {
      firstPage = i;
    }
  }
  return lineStarts(pages[firstPage].ensureLayout());
}

void Doc::scheduleReflow(RenderState &state, const std::uint32_t at,
                         const std::int32_t delta,
                         const std::vector<int> &oldStarts) {
  // Which page holds the edit. Everything before it is untouched by
  // construction: text ahead of an edit cannot reflow.
  std::size_t firstPage = 0;
  for (std::size_t i = 0; i < pages.size(); i++) {
    if (at >= pages[i].baseOffset()) {
      firstPage = i;
    }
  }
  if (pages.empty()) {
    return;
  }

  const auto oldConsumed = pages[firstPage].textLength();

  auto self = getPtr();
  renderer->run([self, &state, firstPage, at, delta, oldStarts, oldConsumed] {
    self->reflowFrom(state, firstPage, at, delta, oldStarts, oldConsumed);
  });
}

void Doc::insert(RenderState &state, const std::uint32_t offset,
                 const std::string &utf8, Caret *caret) {
  if (utf8.empty()) {
    return;
  }
  const auto inserted = static_cast<std::uint32_t>(utf8.size());
  const auto at       = std::min<std::uint32_t>(offset, text.bytes());
  // Before the splice: see lineBreaksAround().
  const auto oldStarts = lineBreaksAround(at);

  // Splice first: the document is the source of truth and must be correct
  // before anything asynchronous looks at it.
  auto raw = text.raw();
  raw.insert(at, utf8);
  text = raw;
  edits++;

  if (nullptr != caret) {
    caret->shiftForInsertion(at, inserted);
  }

  for (auto *const observer : observers) {
    observer->textInserted(*this, at, utf8);
  }

  scheduleReflow(state, at, static_cast<std::int32_t>(inserted), oldStarts);
}

std::string Doc::erase(RenderState &state, const std::uint32_t offset,
                       const std::uint32_t bytes, Caret *caret) {
  auto raw = text.raw();
  if (0 == bytes || raw.empty() || offset >= raw.size()) {
    return {};
  }

  // Snap outwards, so a caller working in bytes cannot leave half a character
  // behind: the start moves back to a boundary and the end forward to the next
  // one. Both directions matter. Snapping the end backwards as well would
  // collapse a range naming part of one character to nothing, and would make
  // "delete one byte of a two-byte character" mean something other than
  // deleting that character.
  const auto start = gleditor::alignToCharacterStart(raw, offset);
  const auto end   = gleditor::alignToCharacterEnd(
      raw, std::min<std::uint32_t>(offset + bytes,
                                   static_cast<std::uint32_t>(raw.size())));
  if (end <= start) {
    return {};
  }
  const auto removed = raw.substr(start, end - start);
  // Before the erasure, for the same reason as in insert().
  const auto oldStarts = lineBreaksAround(start);

  raw.erase(start, removed.size());
  text = raw;
  edits++;

  const auto delta = -static_cast<std::int32_t>(removed.size());
  if (nullptr != caret) {
    caret->shiftForErasure(start, static_cast<std::uint32_t>(removed.size()));
  }

  for (auto *const observer : observers) {
    observer->textErased(*this, start, removed);
  }

  scheduleReflow(state, start, delta, oldStarts);
  return removed;
}

void Doc::reflowFrom(RenderState &state, const std::size_t firstPage,
                     const std::uint32_t at, const std::int32_t delta,
                     const std::vector<int> &oldStarts,
                     const std::uint32_t oldConsumed) {
  // Lay the edited page out again and see how far the damage reaches.
  //
  // Pagination re-syncs as soon as a page ends where it used to, shifted by
  // what the edit changed. From there on every later page holds byte-identical
  // text: its shaping, its glyphs and its vertex rows are all unchanged, and
  // only the offset it reports moves. That is what keeps a keystroke from
  // costing a relayout of the whole document.
  //
  // The comparisons are made in 64-bit signed arithmetic because a removal
  // makes the shift negative, and every offset in sight is unsigned: the
  // re-sync test would otherwise be an unsigned subtraction that wraps rather
  // than going below zero, and would match at a wildly wrong page.
  const auto shift = static_cast<std::int64_t>(delta);
  std::vector<std::pair<std::uint32_t, Glib::RefPtr<Pango::Layout>>> rebuilt;
  auto offset     = pages[firstPage].baseOffset();
  auto pageCursor = firstPage;
  auto scope      = ReflowScope::Document;

  while (offset < text.bytes()) {
    auto lay            = layoutFrom(offset);
    const auto consumed = consumedBytes(lay);
    rebuilt.emplace_back(offset, lay);

    if (pageCursor == firstPage) {
      // The edited page absorbed the change when it still ends where it did,
      // shifted by what the edit added or took away.
      if (static_cast<std::int64_t>(consumed) ==
          static_cast<std::int64_t>(oldConsumed) + shift) {
        const auto relativeAt =
            static_cast<int>(at - pages[firstPage].baseOffset());
        scope = sameLineBreaks(oldStarts, lineStarts(lay), relativeAt,
                               static_cast<int>(delta))
                    ? ReflowScope::Line
                    : ReflowScope::Page;
      }
    }

    offset += consumed;
    pageCursor++;

    if (ReflowScope::Document != scope) {
      break; // pagination re-synced: later pages are untouched.
    }
    if (pageCursor >= pages.size()) {
      break; // ran past the pages that existed; the tail is being rebuilt.
    }
    if (static_cast<std::int64_t>(offset) ==
        static_cast<std::int64_t>(pages[pageCursor].baseOffset()) + shift) {
      break; // re-synced further down.
    }
  }

  // The rows of the pages being replaced. A rebuilt page takes over the rows
  // of the page it stands in for -- nearly the same length, since it covers
  // nearly the same text -- rather than handing them back and asking for them
  // again. Given back, the pool would satisfy the request from the first hole
  // that fitted, which is how a page came to move across the buffer on every
  // keystroke.
  const auto lastRebuilt = firstPage + rebuilt.size();
  const auto replaced    = std::min(lastRebuilt, pages.size());
  std::vector<BufferPool::Allocation> inherited;
  inherited.reserve(replaced - firstPage);
  for (std::size_t i = firstPage; i < replaced; i++) {
    inherited.push_back(pages[i].allocation());
  }
  // Any page that has no successor gives its rows back for good.
  for (std::size_t i = rebuilt.size(); i < inherited.size(); i++) {
    pool->release(inherited[i]);
  }
  inherited.resize(std::min(inherited.size(), rebuilt.size()));

  const auto tailFrom = std::min(lastRebuilt, pages.size());
  std::vector<Page> tail;
  tail.reserve(pages.size() - tailFrom);
  for (std::size_t i = tailFrom; i < pages.size(); i++) {
    tail.push_back(std::move(pages[i]));
  }
  pages.erase(pages.begin() + static_cast<std::ptrdiff_t>(firstPage),
              pages.end());

  for (std::size_t i = 0; i < rebuilt.size(); i++) {
    auto &[base, lay] = rebuilt[i];
    const auto index  = firstPage + i;
    glm::mat4 trans =
        glm::translate(glm::mat4(1.0),
                       glm::vec3(0.0F, -100 * static_cast<float>(index), 0.0F));
    trans = glm::scale(trans, glm::vec3(pixelsToWorld, pixelsToWorld, 1.0F));
    pages.emplace_back(
        getPtr(), state, trans, lay, base, static_cast<std::uint32_t>(index),
        i < inherited.size() ? inherited[i] : BufferPool::Allocation{});
  }
  // Untouched pages keep their shaping and their vertex rows; only the offset
  // they report moves.
  for (auto &page : tail) {
    page.shiftBaseOffset(delta);
    pages.push_back(std::move(page));
  }

  reflowScope = scope;
  reflowPages = rebuilt.size();
  std::cout << std::format("reflow: scope {} pages rebuilt {} of {}\n",
                           reflowScopeName(scope), rebuilt.size(),
                           pages.size());
}

Doc::Doc(const RendererRef &renderer, render::RenderDevice *device,
         const glm::mat4 &model, [[maybe_unused]] const Private _priv)
    : Drawable(model), renderer(renderer),
      pool(std::make_unique<BufferPool>(device, sizeof(VBORow),
                                        initialPoolRows)) {}

Doc::Doc(const RendererRef &renderer, render::RenderDevice *device,
         const glm::mat4 &model, const gleditor::TextSource &source,
         [[maybe_unused]] const Private _priv)
    : Doc(renderer, device, model, _priv) {
  docName = source.name();
  std::cout << "NEW DOC: " << this << " " << docName << " "
            << glm::to_string(model) << "\n";
  text = source.text();

  // Validated here rather than by the source, because every source needs it
  // and none of them can promise otherwise: the bytes come from a file
  // somebody else wrote, or from a program that assembled them out of pieces.
  // A document holding invalid UTF-8 crashes Pango somewhere inside shaping,
  // a long way from whatever produced it.
  auto iter = text.begin();
  if (!text.validate(iter)) {
    // Only a failed validate() leaves `iter` on a real character; on success it
    // is the end iterator and must not be dereferenced.
    std::cout << "invalid utf-8 in " << docName
              << ", first bad offset: " << std::distance(text.begin(), iter)
              << "\n";
    text = text.make_valid();
  }

  // The whole buffer in one allocation, before a page of it is laid out. Doing
  // it by growth instead cost more than the buffer itself: each intermediate
  // size is an allocation the driver keeps rather than returns, so arriving at
  // twenty-five megabytes through seven of them was worse for peak memory than
  // arriving at forty-eight through four.
  pool->reserveCapacity(rowsFor(text.length()));
}

namespace {

/**
 * @brief Where a page starts guessing at how much text it can hold.
 *
 * A page of the default geometry and font holds about four and a half thousand
 * bytes, so one slice of this size fills it with room to spare and the guess
 * is right first time. A font small enough to fit more gets a second attempt;
 * see fillPage().
 */
constexpr std::size_t firstPageGuess = std::size_t{8} * 1024;

} // namespace

void Doc::fillPage(const Glib::RefPtr<Pango::Layout> &layout, const char *from,
                   const std::size_t remaining) {
  for (auto budget = firstPageGuess;; budget *= 4) {
    if (remaining <= budget) {
      pango_layout_set_text(layout->gobj(), from, static_cast<int>(remaining));
      return;
    }
    const auto offered = gleditor::alignToCharacterEnd(
        std::string_view{from, remaining}, static_cast<std::uint32_t>(budget));
    pango_layout_set_text(layout->gobj(), from, static_cast<int>(offered));
    if (layout->is_ellipsized()) {
      // Out of room, so the rest of the document could not have been shown on
      // this page however much of it Pango had been given.
      return;
    }
  }
}

void Doc::makePages(RenderState &state) {
  std::cout << "MAKING PAGES: " << this << " " << glm::to_string(model) << "\n";
  auto tSize = 0UL;
  const std::string_view fullText{text.raw()};
  while (tSize < text.bytes()) {
    auto lay            = layoutFrom(static_cast<std::uint32_t>(tSize));
    auto shaping        = extractPageShaping(lay, fullText.substr(tSize));
    const auto consumed = static_cast<std::uint32_t>(shaping.limit);
    newPage(state, std::move(shaping), static_cast<std::uint32_t>(tSize));
    tSize += consumed;
  }

  // The document is as long as it is going to get without an edit, so the room
  // growth reserved beyond it can go back. Queued rather than done here: the
  // pool belongs to the render thread, which is still building the last pages
  // this loop handed it, and it is those pages that say how much is in use.
  auto self = getPtr();
  renderer->run([self] { self->pool->trim(); });
}

void Doc::newPage(RenderState &state, PageShaping aShaping,
                  const std::uint32_t textOffset) {
  renderer->run([this, &state, shaping = std::move(aShaping), textOffset] {
    const auto numPages = this->pages.size();
    // Pages are laid out in pixels and scaled here, so the stacking distance is
    // in world units while everything inside the page is not.
    glm::mat4 trans = glm::translate(
        glm::mat4(1.0),
        glm::vec3(0.0F, -100 * static_cast<float>(numPages), 0.0F));
    trans = glm::scale(trans, glm::vec3(pixelsToWorld, pixelsToWorld, 1.0F));
    pages.emplace_back(this->getPtr(), state, trans, std::move(shaping),
                       textOffset, static_cast<std::uint32_t>(numPages));
  });
}

void Doc::newPage(RenderState &state, Glib::RefPtr<Pango::Layout> &layout,
                  const std::uint32_t textOffset) {
  auto shaping = extractPageShaping(layout, layout->get_text().raw());
  newPage(state, std::move(shaping), textOffset);
}

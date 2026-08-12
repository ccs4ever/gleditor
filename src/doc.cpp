#include <algorithm>                   // for min, max
#include <cmath>                       // for ceil, lround
#include <limits>
#include <cstddef>                     // for byte
#include <cstdint>                     // for uint32_t
#include <format>                      // for format
#include <gleditor/animation.hpp>      // for docArrival, docArrivalDepth
#include <gleditor/doc.hpp>            // IWYU pragma: associated
#include <gleditor/document_observer.hpp> // for DocumentObserver
#include <gleditor/text_source.hpp>    // for TextSource
#include <gleditor/utf8.hpp>           // for alignToCharacterStart
#include <gleditor/render/device.hpp>  // for RenderDevice
#include <gleditor/render_state.hpp>   // for RenderState
#include <gleditor/renderer.hpp>       // for Renderer, RendererRef
#include <glm/detail/qualifier.hpp>    // for qualifier
#include <glm/ext/matrix_float4x4.hpp> // for mat4
#include <glm/ext/vector_float3.hpp>   // for vec3
#include <glm/gtc/type_ptr.hpp>
#include <iostream>               // for basic_ostream, operator<<
#include <memory>                 // for __shared_ptr_access, shared...
#include <pangomm/cairofontmap.h> // for CairoFontMap
#include <span>                   // for span
#include <stdexcept>              // for logic_error
#include <string>                 // for char_traits, basic_string
#include <string_view>            // for string_view
#include <utility>                // for move
#include <vector>                 // for vector

#include "glibmm/convert.h"              // for get_charset
#include "glibmm/fileutils.h"            // for file_get_contents
#include "glibmm/refptr.h"               // for RefPtr
#include "glibmm/ustring.h"              // for ustring, operator==, UStrin...
#include "pango/pango-layout.h"          // for pango_layout_set_text
#include "pango/pango-types.h"           // for PANGO_SCALE
#include "pangomm/attributes.h"          // for AttrFontDesc, Attribute
#include "pangomm/attrlist.h"            // for AttrList
#include "pangomm/fontdescription.h"     // for FontDescription
#include "pangomm/layout.h"              // for Layout, EllipsizeMode
#include "pangomm/layoutiter.h"          // for LayoutIter
#include "pangomm/layoutline.h"          // for LayoutLine
#include "pangomm/rectangle.h"           // for Rectangle
#include <gleditor/caret.hpp>            // for Caret
#include <gleditor/drawable.hpp>         // for Drawable
#include <gleditor/glyphcache/cache.hpp> // for GlyphCache
#include <gleditor/glyphcache/types.hpp> // for TextureCoords, PointF, Rect
#include <glm/gtx/string_cast.hpp>

namespace {

/// Rows the document's vertex buffer starts out with. It grows on demand, so
/// this only needs to cover a first page or two without a reallocation.
constexpr std::uint32_t initialPoolRows = 1U << 16U;

/// Unpack the layer/width/height triple written by
/// Doc::VBORow::layerWidthHeight. The field widths must stay in step with that
/// function and with unpackLayerWH() in assets/shaders/glyph.vert.glsl.
[[maybe_unused]] glm::vec3 lwh(const std::uint32_t packed3DDims) {
  return {packed3DDims >> 26U, packed3DDims >> 13U & 8191U,
          packed3DDims & 8191U};
}

/// Margin in layout pixels between the page edge and its text.
constexpr float pageMargin = 24.0F;

/// How far in front of the page background its glyphs and bars sit, in the
/// same layout-pixel space. Small enough to be a depth tie-break rather than a
/// visible offset, and part of the box the frustum test uses.
constexpr float glyphDepth = 0.1F;

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
std::size_t nextCharacter(const std::string &text, std::size_t pos) {
  pos = std::min(pos + 1, text.size());
  while (pos < text.size() &&
         0x80 == (static_cast<unsigned char>(text[pos]) & 0xC0)) {
    pos++;
  }
  return pos;
}

/// Number of characters in a UTF-8 range, counting lead bytes.
std::size_t utf8Length(const std::string_view text) {
  return static_cast<std::size_t>(std::ranges::count_if(text, [](const char chr) {
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

render::VertexLayout Doc::vertexLayout() {
  using render::AttributeType;
  static_assert(sizeof(Doc::VBORow) == 48);

  render::VertexLayout layout;
  layout.stride = sizeof(VBORow);
  layout.attributes = {
      {"position", 0, AttributeType::Float, 3, offsetof(VBORow, pos)},
      {"fgcolor", 1, AttributeType::UnsignedInt, 1, offsetof(VBORow, fg)},
      {"bgcolor", 2, AttributeType::UnsignedInt, 1, offsetof(VBORow, bg)},
      {"texcoord", 3, AttributeType::Float, 2, offsetof(VBORow, texcoord)},
      {"texBox", 4, AttributeType::Float, 2, offsetof(VBORow, texBox)},
      {"layerWH", 5, AttributeType::UnsignedInt, 1, offsetof(VBORow, layer)},
      {"tag", 6, AttributeType::UnsignedInt, 2, offsetof(VBORow, tag)},
  };
  return layout;
}

// The constructor parameters are named with a leading `a` so that the body can
// refer to the members without ambiguity: the members are move-constructed from
// the parameters, which leaves the parameters empty.
Page::Page(std::shared_ptr<Doc> aDoc, RenderState &state, glm::mat4 &model,
           Glib::RefPtr<Pango::Layout> aLayout, const std::uint32_t aTextOffset,
           const std::uint32_t aPageIndex)
    : Drawable(model), doc(std::move(aDoc)), layout(std::move(aLayout)),
      textOffset(aTextOffset), pageIndex(aPageIndex) {
  const auto &layout = this->layout;

  const auto color   = Doc::VBORow::color;
  const auto layerWH = Doc::VBORow::layerWidthHeight;

  // Everything below is in layout pixels, the unit Pango reports positions and
  // sizes in. The page's model matrix scales them to world units, so a glyph's
  // quad and the advance that places it shrink together -- they did not before,
  // and the mismatch drew every glyph on top of its neighbours.
  int textWidthPx  = 0;
  int textHeightPx = 0;
  layout->get_pixel_size(textWidthPx, textHeightPx);
  pageWidth  = static_cast<float>(textWidthPx) + (2 * pageMargin);
  pageHeight = static_cast<float>(textHeightPx) + (2 * pageMargin);

  // The page is centred on its own origin, so that the model matrix placing it
  // in the scene positions its middle rather than its top left corner. Kept as
  // members so caret geometry lands in the same space as the glyphs.
  originX = -pageWidth / 2.0F;
  originY = pageHeight / 2.0F;

  // The page's own tag, shared by its background and by the solid bars that
  // stand in for its text at a distance: at that size there is no character
  // under the cursor to name, so a click resolves to the start of the page,
  // which is what a page-kind tag already means.
  const std::array<unsigned int, 2> pageTag = {
      render::packTagIdentity(render::tagKindPage, this->doc->documentIndex(),
                              aPageIndex),
      0};

  // The allocation holds two draws back to back: the full-detail one -- page
  // background followed by a glyph per cluster -- and then the coarse one,
  // which repeats the background and follows it with a solid bar per line.
  // Repeating the background costs one row and is what lets either draw be
  // aimed at with a byte offset and a count, with no second allocation and no
  // stitching of two ranges.
  std::vector<Doc::VBORow> vertexData;
  const auto pushBackground = [&] {
    vertexData.push_back(Doc::VBORow{
        {0.0F, 0.0F, 0.0F},
        color(255),
        color(255),
        {0, 0},
        {0, 0},
        layerWH(0, std::min(Doc::VBORow::maxQuadExtent, static_cast<unsigned int>(pageWidth)),
                std::min(Doc::VBORow::maxQuadExtent, static_cast<unsigned int>(pageHeight))),
        pageTag});
  };
  pushBackground();

  const auto text = layout->get_text().raw();
  const auto font =
      layout->get_context()->load_font(layout->get_font_description());

  // A page layout is handed the whole rest of the document and limited by
  // height, so its text runs far past what the page shows. Everything below is
  // bounded by what the page actually consumes -- without which the final
  // cluster's end ran to the end of the document, producing a "cluster" of
  // tens of kilobytes.
  const auto limit =
      std::min<std::size_t>(text.size(), Doc::consumedBytes(layout));

  // Glyph box area per line, accumulated as the clusters are placed. This is
  // what tells the coarse path how full each line is, so that its bar is as
  // dark as the glyphs it stands in for without anything having to be assumed
  // about the text.
  std::vector<float> lineInk(
      static_cast<std::size_t>(std::max(0, layout->get_line_count())), 0.0F);
  std::size_t lineOfCluster = 0;
  // Byte at which the next line begins, so the running cluster offset can be
  // attributed to a line without searching. Clusters arrive in text order.
  const auto lineStartAt = [&layout](const std::size_t index) {
    const auto &line = layout->get_const_line(static_cast<int>(index));
    return line ? static_cast<std::size_t>(line->get_start_index())
                : std::numeric_limits<std::size_t>::max();
  };
  auto nextLineStart = lineInk.size() > 1
                           ? lineStartAt(1)
                           : std::numeric_limits<std::size_t>::max();

  // Walk the clusters. A cluster is the smallest run Pango will not break
  // apart, so it is what one quad can represent: an "ffi" ligature or a letter
  // with its combining marks is one cluster covering several characters.
  auto iter = layout->get_iter();
  while (true) {
    Pango::Rectangle clusterInk;
    Pango::Rectangle clusterLogical;
    iter.get_cluster_extents(clusterInk, clusterLogical);

    const auto start = static_cast<std::size_t>(std::max(0, iter.get_index()));
    const bool more  = iter.next_cluster();
    // Where the next cluster begins is where this one ends. When there is no
    // next cluster the answer is one character, not "the rest of the page":
    // Pango stops producing clusters partway through the final line, and
    // taking the page's end instead handed the cache a bitmap of the whole
    // remaining run -- a "glyph" 848 texels wide, and hundreds of them across
    // a document. Everything past the last cluster was never shaped, so it
    // belongs to the next page rather than to this one; textBytes below is set
    // from what was actually consumed.
    const auto end = std::min(
        limit, more ? static_cast<std::size_t>(std::max(0, iter.get_index()))
                    : nextCharacter(text, start));

    if (start >= limit) {
      break;
    }
    if (end <= start) {
      if (!more) {
        break;
      }
      continue;
    }

    // A trailing newline is part of the cluster's byte range but has nothing
    // to draw; it still has to be counted so offsets stay in step with the
    // text.
    std::size_t drawEnd = end;
    while (drawEnd > start && ('\n' == text[drawEnd - 1] ||
                               '\r' == text[drawEnd - 1])) {
      drawEnd--;
    }

    clusters.push_back(
        ClusterBox{static_cast<std::uint32_t>(start),
                   static_cast<std::uint32_t>(end - start),
                   static_cast<std::uint32_t>(utf8Length(
                       std::string_view(text).substr(start, end - start)))});

    if (drawEnd == start) {
      if (!more) {
        break;
      }
      continue;
    }

    // The whole cluster is rasterised, not just its first codepoint. Taking
    // the leading sequence dropped the rest of every ligature and every
    // combining mark from the page. A cluster too long for the cache to key on
    // is skipped rather than allowed to stop the editor: it is pathological
    // input, not a reason to fail to open a file.
    if (drawEnd - start > GlyphCache::maxClusterBytes) {
      if (!more) {
        break;
      }
      continue;
    }
    const std::string_view chr(text.data() + start, drawEnd - start);
    const auto glyph    = state.glyphCache.put(chr, font);
    const auto &coords  = glyph.texCoords;
    const auto &extents = glyph.dims;

    const auto glyphWidth  = static_cast<float>(static_cast<int>(extents.width));
    const auto glyphHeight = static_cast<float>(static_cast<int>(extents.height));

    while (start >= nextLineStart && lineOfCluster + 1 < lineInk.size()) {
      lineOfCluster++;
      nextLineStart = lineOfCluster + 1 < lineInk.size()
                          ? lineStartAt(lineOfCluster + 1)
                          : std::numeric_limits<std::size_t>::max();
    }

    if (0.0F < glyphWidth && 0.0F < glyphHeight) {
      if (lineOfCluster < lineInk.size()) {
        lineInk[lineOfCluster] += glyphWidth * glyphHeight * glyph.ink;
      }
      // Pango measures from the top left of the text block downwards; the page
      // runs upwards from its own origin, hence the negated Y.
      const auto left =
          pageMargin + static_cast<float>(toPixels(clusterLogical.get_x()));
      const auto top =
          pageMargin + static_cast<float>(toPixels(clusterLogical.get_y()));

      vertexData.push_back(Doc::VBORow{
          {originX + left + (glyphWidth / 2.0F),
           originY - (top + (glyphHeight / 2.0F)), 0.1F},
          color(0),
          color(255),
          {coords.topLeft.x, coords.topLeft.y},
          {coords.box.width, coords.box.height},
          layerWH(static_cast<unsigned char>(glyph.layer),
                  static_cast<unsigned int>(glyphWidth),
                  static_cast<unsigned int>(glyphHeight)),
          // The cluster index into this page's cluster table, which is what
          // turns a picked fragment back into a text position. The identity
          // word says which document and page that table belongs to.
          {render::packTagIdentity(render::tagKindGlyph,
                                   this->doc->documentIndex(), aPageIndex),
           static_cast<unsigned int>(clusters.size() - 1)}});
    }

    if (!more) {
      break;
    }
  }

  textBytes      = static_cast<std::uint32_t>(limit);
  detailInstances = static_cast<std::uint32_t>(vertexData.size());

  // The coarse draw. One quad per line, covering the line's ink box -- the box
  // the glyphs actually mark, not the logical box, which runs to the wrapping
  // width whether or not there is text out there. At the size this is used at
  // a line is a few pixels tall and its glyphs are indistinguishable anyway,
  // so what matters is that the bar sits where the text sits and is about as
  // dark.
  pushBackground();
  {
    auto lineIter = layout->get_iter();
    std::size_t lineIndex = 0;
    do {
      const auto &line = layout->get_const_line(static_cast<int>(lineIndex));
      // Lines past what this page consumes belong to the next page; the layout
      // is handed the rest of the document and bounded by height, so it has
      // them and the page must not draw them.
      if (!line || static_cast<std::size_t>(line->get_start_index()) >= limit) {
        break;
      }
      const auto inkArea =
          lineIndex < lineInk.size() ? lineInk[lineIndex] : 0.0F;
      lineIndex++;

      Pango::Rectangle ink;
      Pango::Rectangle logical;
      lineIter.get_line_extents(ink, logical);

      const auto barWidth  = static_cast<float>(toPixels(ink.get_width()));
      const auto barHeight = static_cast<float>(toPixels(ink.get_height()));
      // A blank line has no ink and needs no bar.
      if (0.0F >= barWidth || 0.0F >= barHeight) {
        continue;
      }
      const auto left = pageMargin + static_cast<float>(toPixels(ink.get_x()));
      const auto top  = pageMargin + static_cast<float>(toPixels(ink.get_y()));

      // Foreground and background the same colour, so the atlas sample the
      // fragment stage takes cannot change the result: the quad is solid
      // whatever texture coordinate it carries. That is what lets the coarse
      // path share the glyph pipeline instead of needing one of its own.
      const auto shade = greekedShade(inkArea / (barWidth * barHeight));
      vertexData.push_back(Doc::VBORow{
          {originX + left + (barWidth / 2.0F),
           originY - (top + (barHeight / 2.0F)), 0.1F},
          color(shade),
          color(shade),
          {0, 0},
          {0, 0},
          layerWH(0, std::min(Doc::VBORow::maxQuadExtent, static_cast<unsigned int>(barWidth)),
                  std::min(Doc::VBORow::maxQuadExtent, static_cast<unsigned int>(barHeight))),
          pageTag});
    } while (lineIter.next_line());
  }
  coarseInstances =
      static_cast<std::uint32_t>(vertexData.size()) - detailInstances;
  // A page with no lines to bar would draw its background alone, which is not
  // what the page looks like; fall back to the detailed draw instead.
  if (1 >= coarseInstances) {
    vertexData.resize(detailInstances);
    coarseInstances = 0;
  }

  pageBacking = this->doc->pool->reserve(
      static_cast<std::uint32_t>(vertexData.size()));
  this->doc->pool->write(pageBacking, 0, asBytes(vertexData));
}

bool Page::contains(const std::uint32_t globalOffset) const {
  // The end of the last page is a valid caret position, so the upper bound is
  // inclusive there and exclusive everywhere else -- otherwise the caret could
  // not be put after the final character.
  return globalOffset >= textOffset && globalOffset <= textOffset + textBytes;
}

bool Page::caretGeometry(const std::uint32_t globalOffset, float &posX,
                         float &posY, float &height) const {
  if (!contains(globalOffset) || !layout) {
    return false;
  }
  Pango::Rectangle strong;
  Pango::Rectangle weak;
  layout->get_cursor_pos(static_cast<int>(globalOffset - textOffset), strong,
                         weak);

  const auto left   = pageMargin + static_cast<float>(toPixels(strong.get_x()));
  const auto top    = pageMargin + static_cast<float>(toPixels(strong.get_y()));
  const auto tall   = static_cast<float>(toPixels(strong.get_height()));

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
  const auto text = layout->get_text().raw();
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

  const bool coarse = 0 != coarseInstances &&
                      screenScaleAt(mvp, budget.screenWidth) <
                          budget.coarseBelow;
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
      render::DrawUniforms{toArray(mvp), opacity}, doc->pool->buffer(),
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

void Doc::animateArrival(ch::Timeline &timeline) {
  // The resting place is the translation the constructor was handed; the base
  // class keeps that matrix untouched, so it stays available as the target no
  // matter where the animation has got to.
  const glm::vec3 target(model[3]);
  position = target + glm::vec3(0.0F, 0.0F, gleditor::anim::docArrivalDepth);
  opacity  = 0.0F;

  // Eased out rather than linear: the document decelerates into place, which
  // is what makes it read as arriving somewhere rather than being dragged.
  timeline.apply(&position)
      .then<ch::RampTo>(target, gleditor::anim::docArrival, ch::EaseOutCubic());
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
  timeline.apply(&position)
      .then<ch::RampTo>(away, gleditor::anim::docArrival, ch::EaseInQuad());
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
  timeline.apply(&position)
      .then<ch::RampTo>(target, gleditor::anim::docArrival, ch::EaseInOutQuad());
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

  const auto text = layout->get_text().raw();

  std::optional<std::size_t> first;
  std::size_t last = 0;
  for (std::size_t i = 0; i < clusters.size(); i++) {
    const auto &box = clusters[i];
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
    const auto clamped = std::clamp(offset, box.byteStart,
                                    box.byteStart + box.byteLength);
    const auto chars   = utf8Length(std::string_view(text).substr(
        box.byteStart, clamped - box.byteStart));
    return static_cast<float>(chars) / static_cast<float>(box.charCount);
  };

  render::HighlightRange range;
  range.identity =
      render::packTagIdentity(render::tagKindGlyph, doc->documentIndex(),
                              pageIndex);
  range.firstCluster  = static_cast<std::uint32_t>(*first);
  range.lastCluster   = static_cast<std::uint32_t>(last);
  range.colour        = colour;
  range.startFraction = fractionInto(clusters[*first], localStart);
  range.endFraction   = fractionInto(clusters[last], localEnd);
  return range;
}

void Doc::highlightsFor(const std::uint32_t selStart, const std::uint32_t selEnd,
                        const std::uint32_t colour,
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
  const auto fontDesc =
      Pango::FontDescription(renderer->defaultFontName().data());
  const auto fonts = Pango::CairoFontMap::get_default();
  const auto ctx   = fonts->create_context();
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
  pango_layout_set_text(lay->gobj(), txt + offset,
                        static_cast<int>(size - offset));
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

void Doc::scheduleReflow(RenderState &state, const std::uint32_t at,
                         const std::int32_t delta) {
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

  // Line breaks of the edited page before the edit, to tell a line-local
  // change from one that moved a word onto another line.
  const auto oldStarts   = lineStarts(pages[firstPage].layoutRef());
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

  // Splice first: the document is the source of truth and must be correct
  // before anything asynchronous looks at it.
  auto raw = text.raw();
  raw.insert(at, utf8);
  text = raw;

  if (nullptr != caret) {
    caret->shiftForInsertion(at, inserted);
  }

  for (auto *const observer : observers) {
    observer->textInserted(*this, at, utf8);
  }

  scheduleReflow(state, at, static_cast<std::int32_t>(inserted));
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

  raw.erase(start, removed.size());
  text = raw;

  const auto delta = -static_cast<std::int32_t>(removed.size());
  if (nullptr != caret) {
    caret->shiftForErasure(start, static_cast<std::uint32_t>(removed.size()));
  }

  for (auto *const observer : observers) {
    observer->textErased(*this, start, removed);
  }

  scheduleReflow(state, start, delta);
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
  auto offset      = pages[firstPage].baseOffset();
  auto pageCursor  = firstPage;
  auto scope       = ReflowScope::Document;

  while (offset < text.bytes()) {
    auto lay = layoutFrom(offset);
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

  // Drop the pages being replaced, returning their rows to the pool.
  const auto lastRebuilt = firstPage + rebuilt.size();
  for (std::size_t i = firstPage; i < std::min(lastRebuilt, pages.size()); i++) {
    pool->release(pages[i].allocation());
  }
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
    glm::mat4 trans   = glm::translate(
        glm::mat4(1.0), glm::vec3(0.0F, -100 * static_cast<float>(index), 0.0F));
    trans = glm::scale(trans, glm::vec3(pixelsToWorld, pixelsToWorld, 1.0F));
    pages.emplace_back(getPtr(), state, trans, lay, base,
                       static_cast<std::uint32_t>(index));
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
    std::cout << "invalid utf-8 in " << docName << ", first bad offset: "
              << std::distance(text.begin(), iter) << "\n";
    text = text.make_valid();
  }
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
constexpr std::size_t firstPageGuess = 8 * 1024;

/**
 * @brief Give a layout the least text that still fills the page.
 *
 * A page cannot show more than its height allows, so the text past that point
 * changes nothing about what the page holds -- but Pango does not know it is
 * unwanted, and breaking lines through it is what asking for the line count
 * pays for. Handed a whole document, that made the loader quadratic: every
 * page broke lines through everything after it.
 *
 * The amount a page holds depends on the font and the geometry, so it cannot
 * be a constant. Instead the slice starts small and grows until the layout
 * ellipsizes, which is Pango saying it ran out of room -- at which point more
 * text provably could not change what the page shows. A slice that reaches the
 * end of the document is likewise complete. So this ends up handing over
 * exactly what the unbounded version would have shown, having laid out a page's
 * worth rather than a document's worth to find it.
 *
 * Each slice ends on a character boundary, since cutting through one would
 * hand Pango invalid UTF-8 that was never in the document.
 */
void fillPage(const Glib::RefPtr<Pango::Layout> &layout, const char *from,
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

} // namespace

void Doc::makePages(RenderState &state) {
  std::cout << "MAKING PAGES: " << this << " " << glm::to_string(model) << "\n";
  const auto fontDesc =
      Pango::FontDescription(renderer->defaultFontName().data());
  const auto fonts = Pango::CairoFontMap::get_default();
  const auto ctx   = fonts->create_context();
  ctx->set_font_description(fontDesc);
  Pango::AttrList attrs;
  auto fontAttr = Pango::Attribute::create_attr_font_desc(fontDesc);
  attrs.change(fontAttr);
  std::string loc;
  Glib::get_charset(loc);
  auto tSize      = 0UL;
  const char *txt = text.raw().c_str();
  while (tSize < text.bytes()) {
    auto lay = Pango::Layout::create(ctx);
    lay->set_font_description(fontDesc);
    lay->set_single_paragraph_mode(false);
    lay->set_height(std::ceil(139.70 * 11 * PANGO_SCALE));
    lay->set_width(std::ceil(139.70 * 8.5 * PANGO_SCALE));
    lay->set_ellipsize(Pango::EllipsizeMode::END);
    fillPage(lay, txt + tSize, text.bytes() - tSize);
    // Measured before the layout is handed over, never after. newPage() queues
    // the page onto the render thread and the layout goes with it, so from
    // that call on it belongs to two threads at once -- and a Pango layout
    // computes its lines lazily, so merely asking it a question mutates it.
    // Reading it here means this thread does that work while it is still the
    // only owner. The same measure the page itself will record, so that page
    // N+1 starts exactly where page N stopped drawing.
    const auto consumed = consumedBytes(lay);
    newPage(state, lay, static_cast<std::uint32_t>(tSize));
    tSize += consumed;
  }
}

void Doc::newPage(RenderState &state, Glib::RefPtr<Pango::Layout> &layout,
                  const std::uint32_t textOffset) {
  renderer->run([this, &state, layout, textOffset] {
    const auto numPages = this->pages.size();
    // Pages are laid out in pixels and scaled here, so the stacking distance is
    // in world units while everything inside the page is not.
    glm::mat4 trans = glm::translate(
        glm::mat4(1.0),
        glm::vec3(0.0F, -100 * static_cast<float>(numPages), 0.0F));
    trans = glm::scale(trans, glm::vec3(pixelsToWorld, pixelsToWorld, 1.0F));
    pages.emplace_back(this->getPtr(), state, trans, layout, textOffset,
                       static_cast<std::uint32_t>(numPages));
  });
}
// vi: set sw=2 sts=2 ts=2 et:

#include <algorithm>                   // for min, max
#include <cmath>                       // for ceil, lround
#include <limits>
#include <cstddef>                     // for byte
#include <format>                      // for format
#include <gleditor/doc.hpp>            // IWYU pragma: associated
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
#include <sys/types.h>            // for uint
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
[[maybe_unused]] glm::vec3 lwh(const uint packed3DDims) {
  return {packed3DDims >> static_cast<uint>(28),
          packed3DDims >> static_cast<uint>(14) & static_cast<uint>(16383),
          packed3DDims & static_cast<uint>(16383)};
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

/// Number of characters in a UTF-8 range, counting lead bytes.
std::size_t utf8Length(const std::string_view text) {
  return static_cast<std::size_t>(std::ranges::count_if(text, [](const char chr) {
    return 0x80 != (static_cast<unsigned char>(chr) & 0xC0);
  }));
}

/// Byte at @p pos widened without sign extension, for comparing against byte
/// order marks.
unsigned int byteAt(const std::string &str, const std::size_t pos) {
  return static_cast<unsigned char>(str[pos]);
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
        layerWH(0, std::min(16383U, static_cast<unsigned int>(pageWidth)),
                std::min(16383U, static_cast<unsigned int>(pageHeight))),
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
  const auto &lastLine = layout->get_const_line(layout->get_line_count() - 1);
  const auto lastLen   = lastLine->get_length();
  const auto limit     = std::min<std::size_t>(
      text.size(), static_cast<std::size_t>(lastLine->get_start_index() +
                                                (0 == lastLen ? 1 : lastLen)));

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
    const auto end   = std::min(
        limit, more ? static_cast<std::size_t>(std::max(0, iter.get_index()))
                      : limit);

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
          layerWH(0, std::min(16383U, static_cast<unsigned int>(barWidth)),
                  std::min(16383U, static_cast<unsigned int>(barHeight))),
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
                   const glm::mat4 &docTransform, const DrawBudget &budget,
                   DrawStats &stats) const {
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
      render::DrawUniforms{toArray(mvp)}, doc->pool->buffer(),
      doc->pool->byteOffset(pageBacking) + (first * sizeof(Doc::VBORow)),
      count});
}

// Always called from the render thread
void Doc::collect(std::vector<render::GlyphBatch> &batches,
                  const glm::mat4 &viewProjection, const DrawBudget &budget,
                  DrawStats &stats) const {
  const auto docTransform = viewProjection * model;
  for (const auto &page : pages) {
    page.collect(batches, docTransform, budget, stats);
  }
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
    caret.draw(state, viewProjection * model * pageOn.getModel());
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
  const auto &line = layout->get_const_line(layout->get_line_count() - 1);
  const int len    = line->get_length();
  return static_cast<std::uint32_t>(line->get_start_index() +
                                    (0 == len ? 1 : len));
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
                    const int inserted) {
  if (before.size() != after.size()) {
    return false;
  }
  for (std::size_t i = 0; i < before.size(); i++) {
    const int expected = before[i] <= at ? before[i] : before[i] + inserted;
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

  // Which page holds the edit. Everything before it is untouched by
  // construction: text ahead of an insertion cannot reflow.
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
  // insertion from one that pushed a word onto the next line.
  const auto oldStarts   = lineStarts(pages[firstPage].layoutRef());
  const auto oldConsumed = pages[firstPage].textLength();

  auto self = getPtr();
  renderer->run([self, &state, firstPage, at, inserted, oldStarts,
                 oldConsumed] {
    self->reflowFrom(state, firstPage, at, inserted, oldStarts, oldConsumed);
  });
}

void Doc::reflowFrom(RenderState &state, const std::size_t firstPage,
                     const std::uint32_t at, const std::uint32_t inserted,
                     const std::vector<int> &oldStarts,
                     const std::uint32_t oldConsumed) {
  // Lay the edited page out again and see how far the damage reaches.
  //
  // Pagination re-syncs as soon as a page ends where it used to, shifted by
  // what was inserted. From there on every later page holds byte-identical
  // text: its shaping, its glyphs and its vertex rows are all unchanged, and
  // only the offset it reports moves. That is what keeps a keystroke from
  // costing a relayout of the whole document.
  std::vector<std::pair<std::uint32_t, Glib::RefPtr<Pango::Layout>>> rebuilt;
  auto offset      = pages[firstPage].baseOffset();
  auto pageCursor  = firstPage;
  auto scope       = ReflowScope::Document;

  while (offset < text.bytes()) {
    auto lay = layoutFrom(offset);
    const auto consumed = consumedBytes(lay);
    rebuilt.emplace_back(offset, lay);

    if (pageCursor == firstPage) {
      // The edited page absorbed the insertion when it still ends where it
      // did, plus the bytes that were added.
      if (consumed == oldConsumed + inserted) {
        const auto relativeAt =
            static_cast<int>(at - pages[firstPage].baseOffset());
        scope = sameLineBreaks(oldStarts, lineStarts(lay), relativeAt,
                               static_cast<int>(inserted))
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
    if (offset == pages[pageCursor].baseOffset() + inserted) {
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
    page.shiftBaseOffset(inserted);
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
         const glm::mat4 &model, const std::string &fileName,
         [[maybe_unused]] const Private _priv)
    : Doc(renderer, device, model, _priv) {
  docFile = fileName;
  std::cout << "NEW DOC: " << this << " " << fileName << " "
            << glm::to_string(model) << "\n";
  std::string tmpText = Glib::file_get_contents(docFile);
  // Read no further than the file actually goes: a file shorter than three
  // bytes has no third byte to inspect.
  for (std::size_t i = 0; i < std::min<std::size_t>(3, tmpText.size()); i++) {
    std::cout << std::format("bom[{}]: {:02x}\n", i,
                             static_cast<unsigned char>(tmpText[i]));
  }
  if (tmpText.size() >= 3 && static_cast<unsigned char>(tmpText[0]) == 0xEF &&
      static_cast<unsigned char>(tmpText[1]) == 0xBB &&
      static_cast<unsigned char>(tmpText[2]) == 0xBF) {
    std::cout << "found utf8 bom: " << tmpText.size() << " "
              << tmpText.capacity() << "\n";
    text = Glib::ustring(tmpText.data() + 3, tmpText.data() + tmpText.size());
    // `char` is signed on most targets, so the BOM bytes have to be widened
    // through `unsigned char`; comparing the raw char against 0xEF/0xFF is
    // never true and silently disables the detection below.
  } else if (tmpText.size() >= 4 &&
             ((/*utf32BE*/ byteAt(tmpText, 0) == 0x00 &&
               byteAt(tmpText, 1) == 0x00 && byteAt(tmpText, 2) == 0xFE &&
               byteAt(tmpText, 3) == 0xFF) ||
              (/*utf32LE*/ byteAt(tmpText, 0) == 0xFF &&
               byteAt(tmpText, 1) == 0xFE && byteAt(tmpText, 2) == 0x00 &&
               byteAt(tmpText, 3) == 0x00))) {
    throw std::logic_error("utf32 not supported yet");
  } else if (tmpText.size() >= 2 &&
             ((/*utf16BE*/ byteAt(tmpText, 0) == 0xFE &&
               byteAt(tmpText, 1) == 0xFF) ||
              (/*utf16LE*/ byteAt(tmpText, 0) == 0xFF &&
               byteAt(tmpText, 1) == 0xFE))) {
    throw std::logic_error("utf16 not supported yet");
  } else {
    text = tmpText;
  }
  auto iter        = text.begin();
  const bool valid = text.validate(iter);
  std::cout << "validate: " << valid << "\n";
  if (!valid) {
    // Only a failed validate() leaves `iter` on a real character; on success it
    // is the end iterator and must not be dereferenced.
    std::cout << "first bad offset: " << std::distance(text.begin(), iter)
              << "\n";
    text = text.make_valid();
  }
}

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
    pango_layout_set_text(lay->gobj(), txt + tSize,
                          static_cast<int>(text.bytes() - tSize));
    const auto &line = lay->get_const_line(lay->get_line_count() - 1);
    newPage(state, lay, static_cast<std::uint32_t>(tSize));
    const int len = line->get_length();
    tSize += line->get_start_index() + (0 == len ? 1 : len);
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

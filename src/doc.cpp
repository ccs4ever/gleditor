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
#include <span>        // for span
#include <stdexcept>   // for logic_error
#include <string>      // for char_traits, basic_string
#include <string_view> // for string_view
#include <utility>     // for move
#include <vector>      // for vector

#include <fontconfig/fontconfig.h>
#include <gleditor/caret.hpp>            // for Caret
#include <gleditor/drawable.hpp>         // for Drawable
#include <gleditor/glyphcache/cache.hpp> // for GlyphCache
#include <gleditor/glyphcache/types.hpp> // for TextureCoords, PointF, Rect
#include <gleditor/text/font.hpp>        // for FontManager
#include <gleditor/text/layout.hpp>      // for TextLayout
#include <gleditor/utf8.hpp>             // for validateUtf8, makeValidUtf8
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
static_assert(gleditor::GlyphCache::maxEncodableLayers ==
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

  auto font = gleditor::text::FontManager::instance().getFont(
      std::string{this->doc->renderer->defaultFontName()});

  const auto extent = [](const float value) {
    return static_cast<unsigned int>(std::clamp(
        value, 0.0F, static_cast<float>(Doc::VBORow::maxQuadExtent)));
  };

  std::vector<float> lineInk(aShaping.lineCount, 0.0F);

  clusters = std::move(aShaping.clusters);

  for (const auto &g : aShaping.glyphs) {
    const auto glyph = state.glyphCache.put(
        g.chr, font, gleditor::decorationSetFor(g.decorations));
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

bool Page::contains(const std::uint32_t globalOffset) const {
  // The end of the last page is a valid caret position, so the upper bound is
  // inclusive there and exclusive everywhere else -- otherwise the caret could
  // not be put after the final character.
  return globalOffset >= textOffset && globalOffset <= textOffset + textBytes;
}

void Doc::keepLayoutOf(const std::uint32_t pageIndex) const { (void)pageIndex; }

std::string_view Page::pageText() const {
  const std::string_view whole{doc->contents()};
  if (textOffset >= whole.size()) {
    return {};
  }
  return whole.substr(textOffset, textBytes);
}

PageShaping Page::ensureShaping() const { return doc->layoutFrom(textOffset); }

bool Page::caretGeometry(const std::uint32_t globalOffset, float &posX,
                         float &posY, float &height) const {
  if (!contains(globalOffset)) {
    return false;
  }
  const auto shaped = ensureShaping();
  auto font         = gleditor::text::FontManager::instance().getFont(
      std::string{doc->renderer->defaultFontName()});
  height = font ? font->metrics().lineHeight : 16.0F;

  const auto relOffset = globalOffset - textOffset;
  float left           = pageMargin;
  float top            = pageMargin;

  bool found = false;
  for (const auto &g : shaped.glyphs) {
    if (g.clusterIndex < shaped.clusters.size()) {
      const auto &cl = shaped.clusters[g.clusterIndex];
      if (relOffset >= cl.byteStart &&
          relOffset <= cl.byteStart + cl.byteLength) {
        left = pageMargin + g.clusterLeft;
        if (g.lineIndex < shaped.lines.size()) {
          top = pageMargin + shaped.lines[g.lineIndex].top;
        }
        found = true;
        break;
      }
    }
  }
  if (!found && !shaped.lines.empty()) {
    const auto &lastLine = shaped.lines.back();
    left                 = pageMargin + lastLine.barWidth;
    top                  = pageMargin + lastLine.top;
  }

  posX = originX + left + (Caret::widthPixels / 2.0F);
  posY = originY - (top + (height / 2.0F));
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

PageShaping Doc::layoutFrom(const std::uint32_t offset) const {
  if (offset >= text.size()) {
    return PageShaping{};
  }
  auto font = gleditor::text::FontManager::instance().getFont(
      std::string{renderer->defaultFontName()});
  gleditor::text::LayoutOptions opts{
      .maxWidthPx      = 139.70F * 8.5F,
      .maxHeightPx     = 139.70F * 11.0F,
      .singleParagraph = false,
      .ellipsize       = true,
  };

  // Bounded to the nearest forced break past this page's start, if there is
  // one, so layoutPage() never sees text on the far side of it: the break's
  // whole point is that the remainder of a page's height goes unused rather
  // than being filled with what comes after. forcedBreaks is sorted
  // ascending (see TextSource::forcedBreaks()), so the first entry greater
  // than offset is the nearest one -- strictly greater, since a break sitting
  // exactly at offset already did its job ending the previous page and says
  // nothing about this one.
  auto available = text.size() - offset;
  for (const auto breakAt : forcedBreaks) {
    if (breakAt > offset) {
      available = std::min(available, static_cast<std::size_t>(breakAt) -
                                          static_cast<std::size_t>(offset));
      break;
    }
  }

  // decoratedRanges is in this document's own offsets, same as forcedBreaks,
  // but layoutPage() only ever sees the slice starting at offset -- so each
  // range is clipped to what this page actually covers and rebased to that
  // slice's own coordinates, the same translation forcedBreaks gets above.
  const auto pageEnd = offset + available;
  for (const auto &range : decoratedRanges) {
    const auto from = std::max<std::uint32_t>(range.start, offset);
    const auto to    = std::min<std::uint32_t>(range.end,
                                             static_cast<std::uint32_t>(pageEnd));
    if (from < to) {
      opts.decoratedRanges.push_back(gleditor::DecoratedRange{
          .start       = from - offset,
          .end         = to - offset,
          .decorations = range.decorations,
      });
    }
  }

  return gleditor::text::TextLayout::layoutPage(
      std::string_view{text.data() + offset, available}, font, opts);
}

namespace {

std::vector<int> lineStartsFromShaping(const PageShaping &shaping) {
  std::vector<int> starts;
  starts.reserve(shaping.lineCount);
  std::size_t curLine = 0;
  for (const auto &g : shaping.glyphs) {
    while (curLine <= g.lineIndex && curLine < shaping.lineCount) {
      if (g.clusterIndex < shaping.clusters.size()) {
        starts.push_back(
            static_cast<int>(shaping.clusters[g.clusterIndex].byteStart));
      } else {
        starts.push_back(0);
      }
      curLine++;
    }
  }
  return starts;
}

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
  if (pages.empty()) {
    return {};
  }
  std::size_t firstPage = 0;
  for (std::size_t i = 0; i < pages.size(); i++) {
    if (at >= pages[i].baseOffset()) {
      firstPage = i;
    }
  }
  return lineStartsFromShaping(pages[firstPage].ensureShaping());
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
  const auto at =
      std::min<std::uint32_t>(offset, static_cast<std::uint32_t>(text.size()));
  // Before the splice: see lineBreaksAround().
  const auto oldStarts = lineBreaksAround(at);

  // Splice first: the document is the source of truth and must be correct
  // before anything asynchronous looks at it.
  text.insert(at, utf8);
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
  if (0 == bytes || text.empty() || offset >= text.size()) {
    return {};
  }

  // Snap outwards, so a caller working in bytes cannot leave half a character
  // behind: the start moves back to a boundary and the end forward to the next
  // one. Both directions matter. Snapping the end backwards as well would
  // collapse a range naming part of one character to nothing, and would make
  // "delete one byte of a two-byte character" mean something other than
  // deleting that character.
  const auto start = gleditor::alignToCharacterStart(text, offset);
  const auto end   = gleditor::alignToCharacterEnd(
      text, std::min<std::uint32_t>(offset + bytes,
                                    static_cast<std::uint32_t>(text.size())));
  if (end <= start) {
    return {};
  }
  const auto removed = text.substr(start, end - start);
  // Before the erasure, for the same reason as in insert().
  const auto oldStarts = lineBreaksAround(start);

  text.erase(start, removed.size());
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
  std::vector<std::pair<std::uint32_t, PageShaping>> rebuilt;
  auto offset     = pages[firstPage].baseOffset();
  auto pageCursor = firstPage;
  auto scope      = ReflowScope::Document;

  while (offset < text.size()) {
    auto shaping        = layoutFrom(offset);
    const auto consumed = static_cast<std::uint32_t>(shaping.limit);
    if (consumed == 0) {
      break;
    }
    rebuilt.emplace_back(offset, shaping);

    if (pageCursor == firstPage) {
      // The edited page absorbed the change when it still ends where it did,
      // shifted by what the edit added or took away.
      if (static_cast<std::int64_t>(consumed) ==
          static_cast<std::int64_t>(oldConsumed) + shift) {
        const auto relativeAt =
            static_cast<int>(at - pages[firstPage].baseOffset());
        scope = sameLineBreaks(oldStarts, lineStartsFromShaping(shaping),
                               relativeAt, static_cast<int>(delta))
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
    auto &[base, shaping] = rebuilt[i];
    const auto index      = firstPage + i;
    glm::mat4 trans =
        glm::translate(glm::mat4(1.0),
                       glm::vec3(0.0F, -100 * static_cast<float>(index), 0.0F));
    trans = glm::scale(trans, glm::vec3(pixelsToWorld, pixelsToWorld, 1.0F));
    pages.emplace_back(getPtr(), state, trans, std::move(shaping), base,
                       static_cast<std::uint32_t>(index),
                       i < inherited.size() ? inherited[i]
                                            : BufferPool::Allocation{});
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
  text            = source.text();
  forcedBreaks    = source.forcedBreaks();
  decoratedRanges = source.decoratedRanges();

  // Validated here rather than by the source, because every source needs it
  // and none of them can promise otherwise: the bytes come from a file
  // somebody else wrote, or from a program that assembled them out of pieces.
  std::size_t badOffset = 0;
  if (!gleditor::validateUtf8(text, badOffset)) {
    std::cout << "invalid utf-8 in " << docName
              << ", first bad offset: " << badOffset << "\n";
    text = gleditor::makeValidUtf8(text);
  }

  // The whole buffer in one allocation, before a page of it is laid out. Doing
  // it by growth instead cost more than the buffer itself: each intermediate
  // size is an allocation the driver keeps rather than returns, so arriving at
  // twenty-five megabytes through seven of them was worse for peak memory than
  // arriving at forty-eight through four.
  pool->reserveCapacity(rowsFor(text.size()));
}

void Doc::makePages(RenderState &state) {
  std::cout << "MAKING PAGES: " << this << " " << glm::to_string(model) << "\n";
  auto tSize = 0UL;
  while (tSize < text.size()) {
    auto shaping        = layoutFrom(static_cast<std::uint32_t>(tSize));
    const auto consumed = static_cast<std::uint32_t>(shaping.limit);
    if (consumed == 0) {
      break;
    }
    newPage(state, std::move(shaping), static_cast<std::uint32_t>(tSize));
    tSize += consumed;
  }

  // The document is as long as it is going to get without an edit, so the room
  // growth reserved beyond it can go back. Queued rather than done here: the
  // pool belongs to the render thread, which is still building the last pages
  // this loop handed it, and it is those pages that say how much is in use.
  auto self = getPtr();
  renderer->run([self] {
    self->pool->trim();
    self->fullyLoaded = true;
  });
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

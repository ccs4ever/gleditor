#include <algorithm>                      // for min, max
#include <chrono>                         // for steady_clock, milliseconds
#include <cmath>                          // for ceil, lround
#include <cstddef>                        // for byte
#include <cstdint>                        // for uint32_t
#include <format>                         // for format
#include <gleditor/animation.hpp>         // for docArrival, docArrivalDepth
#include <gleditor/doc.hpp>               // IWYU pragma: associated
#include <gleditor/document_observer.hpp> // for DocumentObserver
#include <gleditor/render/constants.hpp>  // for kPageBuildFrameBudget
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
#include <span>          // for span
#include <stdexcept>     // for logic_error
#include <string>        // for char_traits, basic_string
#include <string_view>   // for string_view
#include <unordered_set> // for unordered_set
#include <utility>       // for move
#include <vector>        // for vector

#include <fontconfig/fontconfig.h>
#include <gleditor/caret.hpp>            // for Caret
#include <gleditor/drawable.hpp>         // for Drawable
#include <gleditor/glyphcache/cache.hpp> // for GlyphCache
#include <gleditor/glyphcache/types.hpp> // for TextureCoords, PointF, Rect
#include <gleditor/text/font.hpp>        // for FontManager
#include <gleditor/text/layout.hpp>      // for TextLayout
#include <glm/geometric.hpp>             // for dot, normalize
#include <glm/gtx/string_cast.hpp>
#include <glm/trigonometric.hpp> // for radians

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

/// Margin in layout pixels between the page edge and its text. The one
/// definition lives on Page, where anything drawing in a page's margin can
/// reach it; this is the short name the layout below reads it by.
constexpr float pageMargin = Page::marginPixels;

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

/// The size of the quad a page fills, in layout pixels.
///
/// Four places used to work this out for themselves -- the page constructor
/// and the three that position a page in the document by stacking it under
/// the one before -- and they had to agree, since a page drawn one size and
/// stacked as another either overlaps its neighbour or leaves a gap. Here so
/// that they cannot drift.
///
/// A Fixed page (shaping.page, from TextSource::pageSize()) answers with its
/// own author-chosen size regardless of how much text landed on it -- a
/// short page is still a whole page, with room to grow into. FitContent (or
/// a page nobody named) keeps the answer every page gave before a document
/// could choose its own size: exactly as big as its own content.
struct PageBox {
  float width{};
  float height{};
};

PageBox pageBoxFor(const PageShaping &shaping) {
  if (gleditor::PageSizing::Fixed == shaping.page.mode &&
      shaping.page.widthPx > 0.0F) {
    return {.width = shaping.page.widthPx, .height = shaping.page.heightPx};
  }
  return {
      .width  = static_cast<float>(shaping.textWidthPx) + (2.0F * pageMargin),
      .height = static_cast<float>(shaping.textHeightPx) + (2.0F * pageMargin)};
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
      {.name       = "position",
       .location   = 0,
       .type       = AttributeType::Float,
       .components = 2,
       .offset     = offsetof(VBORow, pos)},
      {.name       = "foreground",
       .location   = 1,
       .type       = AttributeType::UnsignedInt,
       .components = 1,
       .offset     = offsetof(VBORow, foreground)},
      {.name       = "atlas",
       .location   = 2,
       .type       = AttributeType::UnsignedInt,
       .components = 1,
       .offset     = offsetof(VBORow, atlas)},
      {.name       = "quad",
       .location   = 3,
       .type       = AttributeType::UnsignedInt,
       .components = 1,
       .offset     = offsetof(VBORow, quad)},
      {.name       = "paper",
       .location   = 4,
       .type       = AttributeType::UnsignedInt,
       .components = 1,
       .offset     = offsetof(VBORow, paper)},
  };
  return layout;
}

Page::Page(std::shared_ptr<Doc> aDoc, RenderState &state, glm::mat4 &model,
           PageShaping aShaping, const std::uint32_t aTextOffset,
           const std::uint32_t aPageIndex,
           const BufferPool::Allocation &inherited)
    : Drawable(model), doc(std::move(aDoc)), pageBacking(inherited),
      textOffset(aTextOffset), clusters(std::move(aShaping.clusters)),
      pageIndex(aPageIndex),
      identity(render::packTagIdentity(0, doc->documentIndex(), aPageIndex)) {
  const auto color = Doc::VBORow::color;
  const auto box   = Doc::VBORow::box;

  const auto pageBox = pageBoxFor(aShaping);
  pageWidth          = pageBox.width;
  pageHeight         = pageBox.height;

  // originX/originY depend on pageWidth/pageHeight above, which themselves
  // depend on the pageBoxFor() call just above them -- moving these into the
  // member initializer list (as clang-tidy's own -fix does) reorders them
  // ahead of that computation and reads pageWidth/pageHeight before they
  // exist. See the cppcoreguidelines-prefer-member-initializer note in
  // .clang-tidy for the same bug caught here previously.
  // NOLINTBEGIN(cppcoreguidelines-prefer-member-initializer)
  originX = -pageWidth / 2.0F;
  originY = pageHeight / 2.0F;
  // NOLINTEND(cppcoreguidelines-prefer-member-initializer)

  std::vector<Doc::VBORow> vertexData;
  const auto pushBackground = [&] {
    vertexData.push_back(Doc::VBORow{
        .pos        = {0.0F, 0.0F},
        .foreground = Doc::VBORow::fill(color(255), Doc::VBORow::onPaper),
        .atlas      = 0,
        .quad       = box(0,
                          std::min(Doc::VBORow::maxQuadExtent,
                                   static_cast<unsigned int>(pageWidth)),
                          std::min(Doc::VBORow::maxQuadExtent,
                                   static_cast<unsigned int>(pageHeight)),
                          render::tagKindPage),
        .paper      = Doc::VBORow::paperAt(color(255), 0)});
  };
  pushBackground();

  auto font = gleditor::text::FontManager::instance().getFont(
      std::string{this->doc->renderer->defaultFontName()});

  const auto extent = [](const float value) {
    return static_cast<unsigned int>(std::clamp(
        value, 0.0F, static_cast<float>(Doc::VBORow::maxQuadExtent)));
  };

  std::vector<float> lineInk(aShaping.lineCount, 0.0F);

  for (std::size_t gi = 0; gi < aShaping.glyphs.size(); ++gi) {
    const auto &g    = aShaping.glyphs[gi];
    const auto glyph = state.glyphCache.put(
        g.chr, font, gleditor::decorationSetFor(g.decorations));
    const auto &coords  = glyph.texCoords;
    const auto &extents = glyph.dims;

    auto glyphWidth = static_cast<float>(static_cast<int>(extents.width));
    const auto glyphHeight =
        static_cast<float>(static_cast<int>(extents.height));

    if (gi + 1 < aShaping.glyphs.size() &&
        aShaping.glyphs[gi + 1].lineIndex == g.lineIndex) {
      const float distanceToNext =
          aShaping.glyphs[gi + 1].clusterLeft - g.clusterLeft;
      if (distanceToNext > glyphWidth) {
        glyphWidth = std::ceil(distanceToNext);
      }
    }

    if (0.0F < glyphWidth && 0.0F < glyphHeight) {
      if (g.lineIndex < lineInk.size()) {
        lineInk[g.lineIndex] += glyphWidth * glyphHeight * glyph.ink;
      }
      const auto left = pageMargin + g.clusterLeft;
      const auto top  = pageMargin + g.clusterTop;

      vertexData.push_back(Doc::VBORow{
          .pos        = {originX + left + (glyphWidth / 2.0F),
                         originY - (top + (glyphHeight / 2.0F))},
          .foreground = Doc::VBORow::ink(color(0), Doc::VBORow::onText, false),
          .atlas =
              Doc::VBORow::atlasAt(static_cast<unsigned int>(coords.topLeft.x),
                                   static_cast<unsigned int>(coords.topLeft.y)),
          .quad =
              box(static_cast<unsigned char>(glyph.layer), extent(glyphWidth),
                  extent(glyphHeight), render::tagKindGlyph),
          .paper = Doc::VBORow::paperAt(
              color(255), static_cast<unsigned int>(g.clusterIndex))});
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
    vertexData.push_back(Doc::VBORow{
        .pos        = {originX + left + (line.barWidth / 2.0F),
                       originY - (top + (line.barHeight / 2.0F))},
        .foreground = Doc::VBORow::fill(color(shade), Doc::VBORow::onText),
        .atlas      = 0,
        .quad       = box(0, extent(line.barWidth), extent(line.barHeight),
                          render::tagKindPage),
        .paper      = Doc::VBORow::paperAt(color(shade), 0)});
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

void Doc::keepLayoutOf(const std::uint32_t pageIndex) { (void)pageIndex; }

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
      // Exclusive upper bound: relOffset sitting exactly on the boundary
      // between two clusters means "the start of the next one", not "the end
      // of the previous one". Inclusive matched the previous cluster instead,
      // because shaped.glyphs is walked in document order and the previous
      // cluster is seen first -- indistinguishable from the correct answer
      // for two adjacent visible clusters (same on-screen X either way), but
      // wrong whenever what follows has no cluster of its own to lose to, the
      // case the fallback below exists for. A media placeholder's reserved
      // blank lines are exactly that: relOffset landing on the byte where the
      // preceding paragraph's last cluster ends *is* where the placeholder
      // begins, and this loop must not claim it first.
      if (relOffset >= cl.byteStart &&
          relOffset < cl.byteStart + cl.byteLength) {
        left = pageMargin + g.clusterLeft;
        if (g.lineIndex < shaped.lines.size()) {
          top = pageMargin + shaped.lines[g.lineIndex].top;
        }
        found = true;
        break;
      }
    }
  }
  if (!found) {
    // No cluster covers this offset -- the common reason is a blank line, a
    // media placeholder's reserved newlines having no glyph of their own to
    // match above. Locate the *line* directly by its own byte range instead
    // of falling back to the page's last line regardless of where relOffset
    // actually falls: two placeholders on the same page would otherwise both
    // resolve to that one last line and land on top of each other.
    for (const auto &ln : shaped.lines) {
      // Same exclusive-upper-bound reasoning as the cluster loop above: the
      // line this offset ends still-inclusive would be the *previous* line,
      // not the blank one this offset actually names.
      if (relOffset >= ln.byteStart &&
          relOffset < ln.byteStart + ln.byteLength) {
        // Past the end of the line's own ink, which starts at ln.left rather
        // than at the text edge for anything not left-aligned.
        left  = pageMargin + ln.left + ln.barWidth;
        top   = pageMargin + ln.top;
        found = true;
        break;
      }
    }
  }
  if (!found && !shaped.lines.empty()) {
    const auto &lastLine = shaped.lines.back();
    left                 = pageMargin + lastLine.left + lastLine.barWidth;
    top                  = pageMargin + lastLine.top;
  }

  posX = originX + left + (Caret::widthPixels / 2.0F);
  posY = originY - (top + (height / 2.0F));
  return true;
}

bool Page::boxGeometry(const std::uint32_t globalOffset, float &posX,
                       float &posY, float &width, float &height) const {
  if (!contains(globalOffset)) {
    return false;
  }
  const auto shaped    = ensureShaping();
  const auto relOffset = globalOffset - textOffset;

  for (const auto &placed : shaped.boxes) {
    if (placed.anchorByteOffset == relOffset) {
      posX   = originX + pageMargin + placed.left;
      posY   = originY - (pageMargin + placed.top + placed.height);
      width  = placed.width;
      height = placed.height;
      return true;
    }
  }
  return false;
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
  auto offset     = static_cast<std::size_t>(cluster.byteStart);
  const auto end  = static_cast<std::size_t>(cluster.byteStart) +
                    static_cast<std::size_t>(cluster.byteLength);
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

std::uint32_t Page::offsetForPagePoint(const float xFraction,
                                       const float yFraction) const {
  const auto shaped = ensureShaping();
  if (shaped.lines.empty()) {
    return textOffset;
  }

  // Layout coordinates exclude the page border and grow down from the top;
  // the picking quad coordinates cover the complete page and grow up from
  // the bottom.
  const float x =
      (std::clamp(xFraction, 0.0F, 1.0F) * pageWidth) - Page::marginPixels;
  const float y = ((1.0F - std::clamp(yFraction, 0.0F, 1.0F)) * pageHeight) -
                  Page::marginPixels;

  const auto lineIt = std::ranges::min_element(
      shaped.lines, [y](const auto &left, const auto &right) {
        const auto leftCentre  = left.top + (left.barHeight * 0.5F);
        const auto rightCentre = right.top + (right.barHeight * 0.5F);
        return std::abs(leftCentre - y) < std::abs(rightCentre - y);
      });
  const auto &line = *lineIt;

  const auto lineEnd = line.byteStart + line.byteLength;
  for (std::size_t i = 0; i < shaped.glyphs.size(); ++i) {
    const auto &glyph = shaped.glyphs[i];
    if (glyph.lineIndex != line.lineIndex ||
        glyph.clusterIndex >= shaped.clusters.size()) {
      continue;
    }
    const auto &cluster = shaped.clusters[glyph.clusterIndex];
    float right         = line.left + line.barWidth;
    for (const auto &next : shaped.glyphs) {
      if (next.lineIndex == line.lineIndex &&
          next.clusterLeft > glyph.clusterLeft) {
        right = next.clusterLeft;
        break;
      }
    }
    if (x < (glyph.clusterLeft + right) * 0.5F) {
      return textOffset + cluster.byteStart;
    }
    if (i + 1 == shaped.glyphs.size() ||
        shaped.glyphs[i + 1].lineIndex != line.lineIndex) {
      return textOffset + cluster.byteStart + cluster.byteLength;
    }
  }
  return textOffset + lineEnd;
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
      .uniforms = render::DrawUniforms{.mvp      = toArray(mvp),
                                       .opacity  = opacity,
                                       .identity = identity},
      .vertices = doc->pool->buffer(),
      .vertexByteOffset =
          doc->pool->byteOffset(pageBacking) + (first * sizeof(Doc::VBORow)),
      .instanceCount = count});
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
  for (const auto &pageSlot : pages) {
    if (!pageSlot) {
      continue;
    }
    pageSlot->collect(batches, docTransform, opacity(), budget, stats);
  }
}

glm::mat4 Doc::modelMatrix() const {
  return glm::translate(glm::mat4(1.0F), position());
}

std::optional<Doc::Anchor>
Doc::anchorFor(const std::uint32_t globalOffset) const {
  for (std::size_t i = 0; i < pages.size(); i++) {
    if (!pages[i]) {
      continue;
    }
    // Asked page by page rather than by searching, because the same call
    // decides whether the offset is on the page and where -- and the deciding
    // half is answered without shaping anything.
    Anchor anchor{.pageIndex = static_cast<std::uint32_t>(i),
                  .x         = 0.0F,
                  .y         = 0.0F,
                  .height    = 0.0F};
    // pages[i] was just checked truthy above; re-indexing here reaches the
    // same slot since nothing in this loop body mutates pages.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    if (pages[i]->caretGeometry(globalOffset, anchor.x, anchor.y,
                                anchor.height)) {
      return anchor;
    }
  }
  return std::nullopt;
}

std::optional<Doc::BoxRect>
Doc::boxFor(const std::uint32_t globalOffset) const {
  for (std::size_t i = 0; i < pages.size(); i++) {
    if (!pages[i]) {
      continue;
    }
    BoxRect rect{.pageIndex = static_cast<std::uint32_t>(i),
                 .x         = 0.0F,
                 .y         = 0.0F,
                 .width     = 0.0F,
                 .height    = 0.0F};
    // pages[i] was just checked truthy above; re-indexing here reaches the
    // same slot since nothing in this loop body mutates pages.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    if (pages[i]->boxGeometry(globalOffset, rect.x, rect.y, rect.width,
                              rect.height)) {
      return rect;
    }
  }
  return std::nullopt;
}

void Doc::refreshPageIndexFilade() const {
  std::vector<gleditor::enfilade::LayoutEntry> entriesSnapshot;
  {
    std::scoped_lock lock(shapingMutex);
    if (pageEntries.size() > pageIndexFiladeBuiltFor) {
      entriesSnapshot = pageEntries;
    }
  }
  if (entriesSnapshot.empty()) {
    return;
  }
  pageIndexFiladeBuiltFor = entriesSnapshot.size();
  pageIndexFilade =
      gleditor::enfilade::Layoutfilade::buildFromEntries(entriesSnapshot);
}

std::optional<std::uint32_t>
Doc::pageIndexForOffset(const std::uint32_t globalOffset) const {
  refreshPageIndexFilade();
  const auto hit = pageIndexFilade.findEntryAtByte(globalOffset);
  if (!hit) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(hit->entryIndex);
}

std::optional<Doc::Anchor>
Doc::approximateAnchorFor(const std::uint32_t globalOffset) const {
  const auto pageIndex = pageIndexForOffset(globalOffset);
  if (!pageIndex) {
    return std::nullopt;
  }
  return Anchor{.pageIndex = *pageIndex, .x = 0.0F, .y = 0.0F, .height = 0.0F};
}

std::optional<glm::vec3> Doc::worldPoint(const std::uint32_t pageIndex,
                                         const float posX,
                                         const float posY) const {
  if (pageIndex < pages.size() && pages[pageIndex].has_value()) {
    // Re-indexing pages[pageIndex] here reaches the same slot just checked.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    const auto point = modelMatrix() * pages[pageIndex]->getModel() *
                       glm::vec4(posX, posY, 0.0F, 1.0F);
    return glm::vec3(point);
  }
  // Shaped but not yet built into a Page object -- approximate from
  // pageIndexFilade instead of failing outright, so a caller deciding which
  // pages are worth building sooner can still place a point near one that
  // has not been built yet. posX/posY are ignored: only the page's own
  // cumulative Y is known without shaping it (see approximateAnchorFor()).
  refreshPageIndexFilade();
  const auto hit = pageIndexFilade.findEntryByIndex(pageIndex);
  if (!hit) {
    return std::nullopt;
  }
  const glm::vec3 approxLocal(0.0F, -hit->startYPx * pixelsToWorld, 0.0F);
  return glm::vec3(modelMatrix() * glm::vec4(approxLocal, 1.0F));
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
  timeline.apply(&opacity).then<ch::RampTo>(
      restingOpacity, gleditor::anim::docArrival, ch::EaseOutQuad());
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

void Doc::animateMoveTo(ch::Timeline &timeline, const glm::vec3 &target,
                        const double seconds, const double delay) {
  // The resting place is recorded as well as animated towards: a later
  // arrival or departure reads it back out of the base matrix.
  model = glm::translate(glm::mat4(1.0F), target);
  // apply() replaces whatever motion was on this output, so a move that
  // interrupts another one continues from where that one had got to rather
  // than restarting from the old target.
  auto motion = timeline.apply(&position);
  if (delay > 0.0) {
    // Held at where the document is now rather than at where it was told to
    // go, so the wait is a wait and not a jump followed by one.
    motion.then<ch::Hold>(position(), delay);
  }
  motion.then<ch::RampTo>(target, seconds, ch::EaseInOutQuad());
}

void Doc::animateOpacity(ch::Timeline &timeline, const float target,
                         const double seconds, const double delay) {
  restingOpacity = target;
  auto motion    = timeline.apply(&opacity);
  if (delay > 0.0) {
    motion.then<ch::Hold>(opacity(), delay);
  }
  motion.then<ch::RampTo>(target, seconds, ch::EaseInOutQuad());
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
  for (const auto &pageSlot : pages) {
    if (!pageSlot) {
      continue;
    }
    if (auto range = pageSlot->highlightFor(selStart, selEnd, colour)) {
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
    return target->offsetForPagePoint(
        static_cast<float>(tag.clusterIndex) /
            static_cast<float>(render::tagFractionScale),
        tag.fraction);
  }
  return target->offsetForCluster(tag.clusterIndex, tag.fraction);
}

void Doc::drawCaret(RenderState &state, const glm::mat4 &viewProjection,
                    Caret &caret) const {
  if (!caret.active() || caret.documentIndex() != docIndex) {
    return;
  }
  for (const auto &pageSlot : pages) {
    if (!pageSlot) {
      continue;
    }
    float posX   = 0.0F;
    float posY   = 0.0F;
    float height = 0.0F;
    if (!pageSlot->caretGeometry(caret.byteOffset(), posX, posY, height)) {
      continue;
    }
    caret.setGeometry(posX, posY, height);
    caret.draw(state, viewProjection * modelMatrix() * pageSlot->getModel());
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
  // A Fixed page wraps text at its own text-area width and never past its
  // own text-area height; FitContent (or a page nobody named, the default
  // before this existed) keeps wrapping at the Letter geometry every page
  // used to, since a Doc has always needed *some* wrap width and nothing
  // yet asks a FitContent Doc page for a different one.
  const bool fixedPage = gleditor::PageSizing::Fixed == pageGeometry.mode &&
                         pageGeometry.widthPx > 0.0F;
  gleditor::text::LayoutOptions opts{
      .maxWidthPx = fixedPage ? pageGeometry.textWidthPx() : Doc::textWidthPx,
      .maxHeightPx =
          fixedPage ? pageGeometry.textHeightPx() : Doc::textHeightPx,
      .singleParagraph = false,
      .ellipsize       = true,
      .page            = pageGeometry,
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
    const auto to =
        std::min<std::uint32_t>(range.end, static_cast<std::uint32_t>(pageEnd));
    if (from < to) {
      opts.decoratedRanges.push_back(gleditor::DecoratedRange{
          .start       = from - offset,
          .end         = to - offset,
          .decorations = range.decorations,
      });
    }
  }

  // layoutBoxes is a single anchor byte each -- forwarded only when that
  // anchor falls within this page's own slice, the same "start falls on
  // this page or it does not" rule decoratedRanges' clipping does not
  // need, since a box has no length of its own to be truncated.
  for (const auto &box : layoutBoxes) {
    if (box.anchor >= offset &&
        box.anchor < static_cast<std::uint32_t>(pageEnd)) {
      auto rebased   = box;
      rebased.anchor = box.anchor - offset;
      opts.boxes.push_back(rebased);
    }
  }

  // blockStyles is clipped and rebased the same way decoratedRanges is
  // above: a paragraph style, unlike an atomic range's height, is not
  // wrong for being truncated to what this page covers.
  for (const auto &range : blockStyles) {
    const auto from = std::max<std::uint32_t>(range.start, offset);
    const auto to =
        std::min<std::uint32_t>(range.end, static_cast<std::uint32_t>(pageEnd));
    if (from < to) {
      auto rebased  = range;
      rebased.start = from - offset;
      rebased.end   = to - offset;
      opts.blockStyles.push_back(rebased);
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
  if (std::ranges::find(observers, observer) == observers.end()) {
    observers.push_back(observer);
  }
}

void Doc::removeObserver(gleditor::DocumentObserver *const observer) {
  // std::ranges::remove returns a subrange (its .begin() is the new logical
  // end), not the plain iterator std::remove returns -- erase() needs that
  // iterator, so this stays the pre-ranges algorithm. See the
  // modernize-use-ranges note in .clang-tidy.
  // NOLINTNEXTLINE(modernize-use-ranges)
  observers.erase(std::remove(observers.begin(), observers.end(), observer),
                  observers.end());
}

std::vector<int> Doc::lineBreaksAround(const std::uint32_t at) const {
  if (pages.empty()) {
    return {};
  }
  std::size_t firstPage = 0;
  for (std::size_t i = 0; i < pages.size(); i++) {
    // pages[i] is re-indexed after the truthiness check in the same &&
    // expression, reaching the same slot.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    if (pages[i] && at >= pages[i]->baseOffset()) {
      firstPage = i;
    }
  }
  if (!pages[firstPage]) {
    return {};
  }
  // Re-indexing pages[firstPage] here reaches the same slot just checked.
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  return lineStartsFromShaping(pages[firstPage]->ensureShaping());
}

void Doc::scheduleReflow(RenderState &state, const std::uint32_t at,
                         const std::int32_t delta,
                         const std::vector<int> &oldStarts) {
  // Which page holds the edit. Everything before it is untouched by
  // construction: text ahead of an edit cannot reflow.
  std::size_t firstPage = 0;
  for (std::size_t i = 0; i < pages.size(); i++) {
    // pages[i] is re-indexed after the truthiness check in the same &&
    // expression, reaching the same slot.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    if (pages[i] && at >= pages[i]->baseOffset()) {
      firstPage = i;
    }
  }
  if (pages.empty() || !pages[firstPage]) {
    return;
  }

  // Re-indexing pages[firstPage] here reaches the same slot just checked.
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  const auto oldConsumed = pages[firstPage]->textLength();

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
  // See ensurePagesBuiltThrough()'s own comment: everything below assumes
  // pages[firstPage] itself, and everything before it, already exist.
  ensurePagesBuiltThrough(state, firstPage + 1);

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
  // pages[firstPage] is guaranteed built by ensurePagesBuiltThrough() above.
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  auto offset     = pages[firstPage]->baseOffset();
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
        // pages[firstPage] is guaranteed built by ensurePagesBuiltThrough().
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        const auto pageBase   = pages[firstPage]->baseOffset();
        const auto relativeAt = static_cast<int>(at - pageBase);
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
    // pages[pageCursor] is expected to already be built here -- nothing
    // before Stage 3 builds out of order -- but a missing one simply never
    // re-syncs early rather than dereferencing a gap.
    if (pages[pageCursor]) {
      // Guarded by the truthiness check just above.
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      const auto cursorBase = pages[pageCursor]->baseOffset();
      if (static_cast<std::int64_t>(offset) ==
          static_cast<std::int64_t>(cursorBase) + shift) {
        break; // re-synced further down.
      }
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
    // pages[i] is re-indexed in the ternary's true branch, reaching the
    // same slot just checked truthy.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    inherited.push_back(pages[i] ? pages[i]->allocation()
                                 : BufferPool::Allocation{});
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
    // Guaranteed built: everything past firstPage was dense before this
    // reflow started (nothing before Stage 3 builds out of order), and
    // ensurePagesBuiltThrough() above only had to cover up to firstPage.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    tail.push_back(std::move(*pages[i]));
  }
  pages.erase(pages.begin() + static_cast<std::ptrdiff_t>(firstPage),
              pages.end());

  float currentTopY = 0.0F;
  if (firstPage > 0 && firstPage <= pages.size()) {
    // Guaranteed built: pages are dense from index 0, and
    // ensurePagesBuiltThrough() above covered up to firstPage.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    const auto &prevPage        = *pages[firstPage - 1];
    const float prevCenterY     = prevPage.getModel()[3][1];
    const float prevHeightWorld = prevPage.heightPixels() * pixelsToWorld;
    const float prevBottomY     = prevCenterY - (prevHeightWorld / 2.0F);
    currentTopY                 = prevBottomY - (pageGapPx * pixelsToWorld);
  }

  for (std::size_t i = 0; i < rebuilt.size(); i++) {
    auto &[base, shaping]       = rebuilt[i];
    const auto index            = firstPage + i;
    const float pageHeightPx    = pageBoxFor(shaping).height;
    const float pageHeightWorld = pageHeightPx * pixelsToWorld;
    const float centerY         = currentTopY - (pageHeightWorld / 2.0F);

    glm::mat4 trans =
        glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, centerY, 0.0F));
    trans = glm::scale(trans, glm::vec3(pixelsToWorld, pixelsToWorld, 1.0F));
    placePageAt(state, index, std::move(shaping), base, trans,
                i < inherited.size() ? inherited[i] : BufferPool::Allocation{});

    currentTopY =
        (centerY - (pageHeightWorld / 2.0F)) - (pageGapPx * pixelsToWorld);
  }
  // Untouched pages keep their shaping and their vertex rows; only the offset
  // they report moves. Update their model matrix with the new vertical
  // position.
  for (auto &page : tail) {
    page.shiftBaseOffset(delta);
    const float pageHeightWorld = page.heightPixels() * pixelsToWorld;
    const float centerY         = currentTopY - (pageHeightWorld / 2.0F);
    glm::mat4 trans =
        glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, centerY, 0.0F));
    trans = glm::scale(trans, glm::vec3(pixelsToWorld, pixelsToWorld, 1.0F));
    page.setModel(trans);
    pages.emplace_back(std::move(page));
    currentTopY =
        (centerY - (pageHeightWorld / 2.0F)) - (pageGapPx * pixelsToWorld);
  }

  reflowScope = scope;
  reflowPages = rebuilt.size();
  std::cout << std::format("reflow: scope {} pages rebuilt {} of {}\n",
                           reflowScopeName(scope), rebuilt.size(),
                           pages.size());
}

Doc::Doc(RendererRef renderer, render::RenderDevice *device,
         const glm::mat4 &model, [[maybe_unused]] const Private _priv)
    : Drawable(model), renderer(std::move(renderer)),
      pool(std::make_unique<BufferPool>(device, sizeof(VBORow),
                                        initialPoolRows)),
      // Matches Drawable's own model translation, the same resting place
      // animateArrival() will later use as its own target -- so a Doc that
      // never goes through the normal open-document arrival animation
      // (every test that calls Doc::create() directly, notably) still has a
      // real position rather than ch::Output<glm::vec3>'s uninitialized
      // default. animateArrival() overwrites this with its own starting
      // point regardless, so this changes nothing for a Doc opened the
      // normal way.
      position(glm::vec3(model[3])) {}

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
  layoutBoxes     = source.layoutBoxes();
  blockStyles     = source.blockStyles();
  pageGeometry    = source.pageSize();

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

void Doc::load(const gleditor::TextSource &source) {
  docName         = source.name();
  text            = source.text();
  forcedBreaks    = source.forcedBreaks();
  decoratedRanges = source.decoratedRanges();
  layoutBoxes     = source.layoutBoxes();
  blockStyles     = source.blockStyles();
  pageGeometry    = source.pageSize();

  std::size_t badOffset = 0;
  if (!gleditor::validateUtf8(text, badOffset)) {
    text = gleditor::makeValidUtf8(text);
  }
  // Splice first, as in insert()/erase(): the document is the source of
  // truth and must be correct before anything asynchronous looks at it.
  edits++;

  pages.clear();
  liveLayouts.clear();
  {
    std::scoped_lock lock(shapingMutex);
    pendingShapings.clear();
    pageEntries.clear();
  }
  pageIndexFilade         = gleditor::enfilade::Layoutfilade{};
  pageIndexFiladeBuiltFor = 0;
  // Byte offsets into the text this load() is replacing; a load wholesale
  // replaces that text, so an offset from before it means nothing (and may
  // not even be in bounds) against the new one.
  priorityOffsets.clear();
  // A load replaces the document wholesale, so whatever the previous content
  // had already finished laying out says nothing about this one: without
  // resetting this, isFullyLoaded() (which requires shapingComplete before
  // it will even look at wantedPageIndices()) would keep answering as of the
  // old text, and anything (LinkBeams::drawFrame among them) waiting on the
  // new content to finish paginating would never see it become pending.
  shapingComplete.store(false, std::memory_order_release);
  pool->reserveCapacity(rowsFor(text.size()));
  makePages();
}

void Doc::makePages([[maybe_unused]] RenderState &state) { makePages(); }

void Doc::makePages() {
  std::cout << "MAKING PAGES: " << this << " " << glm::to_string(model) << "\n";
  auto tSize = 0UL;
  while (tSize < text.size()) {
    auto shaping        = layoutFrom(static_cast<std::uint32_t>(tSize));
    const auto consumed = static_cast<std::uint32_t>(shaping.limit);
    if (consumed == 0) {
      break;
    }
    const auto heightPx = pageBoxFor(shaping).height;
    {
      std::scoped_lock lock(shapingMutex);
      // This page's true index is exactly how many entries already exist:
      // makePages() always shapes strictly in document order, and this
      // entry and its pendingShapings counterpart are recorded together
      // under the same lock, so the two never drift apart.
      const auto pageIndex = static_cast<std::uint32_t>(pageEntries.size());
      pendingShapings.emplace(
          pageIndex,
          PendingShaping{.shaping    = std::move(shaping),
                         .textOffset = static_cast<std::uint32_t>(tSize)});
      pageEntries.push_back(gleditor::enfilade::LayoutEntry{
          .byteLength = consumed,
          .heightPx   = heightPx + pageGapPx,
      });
    }
    tSize += consumed;
  }
  shapingComplete.store(true, std::memory_order_release);
}

std::optional<Doc::CameraInfo> Doc::cameraInfo() const {
  if (nullptr == renderer) {
    return std::nullopt;
  }
  const auto appState = renderer->appState();
  if (nullptr == appState) {
    return std::nullopt;
  }
  glm::vec3 pos{};
  glm::vec3 front{};
  float fov = 0.0F;
  {
    std::scoped_lock viewLock(appState->view);
    pos   = appState->view.pos;
    front = appState->view.front;
    fov   = appState->view.fov;
  }
  CameraInfo info;
  // The camera is one shared, global, free-moving 3D point, not a
  // per-document scroll offset -- projected into this document's own
  // stacking coordinate the same way page Y positions already are: relative
  // to this Doc's own world position, in the same pixelsToWorld-scaled units
  // buildPendingPages() stacks pages in.
  info.pagePixelY = (currentPosition().y - pos.y) / pixelsToWorld;
  // Perpendicular distance from the camera to this document, along the
  // camera's own look direction -- the same projection src/app.cpp's touch
  // panning uses to turn pixel motion into world units at the camera's
  // current zoom.
  info.distanceToDoc = glm::dot(currentPosition() - pos, glm::normalize(front));
  info.fovDegrees    = fov;
  return info;
}

std::optional<gleditor::enfilade::VisibleRange>
Doc::viewportPriorityRange() const {
  refreshPageIndexFilade();
  if (pageIndexFilade.empty()) {
    return std::nullopt;
  }
  const auto cam = cameraInfo();
  // Behind the camera (or coincident with it): the perspective formula below
  // divides by nothing useful, and there is no meaningful "visible height" to
  // report.
  if (!cam || cam->distanceToDoc <= 0.0F) {
    return std::nullopt;
  }

  // Perspective-correct viewport height at this document's distance from the
  // camera: the frustum is 2*distance*tan(fov/2) world units tall there.
  const float visibleHeightWorld =
      2.0F * cam->distanceToDoc *
      std::tan(glm::radians(cam->fovDegrees) * 0.5F);
  const float halfViewportPx = (visibleHeightWorld / pixelsToWorld) / 2.0F;
  const float marginPx =
      halfViewportPx * render::kViewportPriorityMarginFraction;

  return pageIndexFilade.visibleRange(
      cam->pagePixelY - halfViewportPx - marginPx,
      cam->pagePixelY + halfViewportPx + marginPx);
}

std::vector<std::uint32_t> Doc::wantedPageIndices() const {
  refreshPageIndexFilade();

  std::vector<std::uint32_t> ordered;
  std::unordered_set<std::uint32_t> seen;
  const auto want = [&](const std::uint32_t index) {
    if (seen.insert(index).second) {
      ordered.push_back(index);
    }
  };

  if (const auto viewport = viewportPriorityRange()) {
    for (auto index = viewport->firstEntryIndex;
         index <= viewport->lastEntryIndex; ++index) {
      want(static_cast<std::uint32_t>(index));
    }
  } else if (!pageIndexFilade.empty()) {
    // No usable camera signal (see cameraInfo()'s own comment) but something
    // has been shaped -- want page 0 rather than nothing, so a document
    // opened before its camera settles still shows something and
    // isFullyLoaded() cannot report done having built no pages.
    want(0);
  }

  for (const auto offset : priorityOffsets) {
    if (const auto hit = pageIndexFilade.findEntryAtByte(offset)) {
      want(static_cast<std::uint32_t>(hit->entryIndex));
    }
  }

  return ordered;
}

bool Doc::isFullyLoaded() const {
  if (!shapingComplete.load(std::memory_order_acquire)) {
    return false;
  }
  return std::ranges::all_of(wantedPageIndices(), [this](const auto index) {
    return nullptr != page(index);
  });
}

std::chrono::milliseconds Doc::buildBudgetForThisCall() {
  // pageEntries accumulates independently of pendingShapings (which
  // buildPendingPages() below drains), so it reflects every page shaped so
  // far regardless of GPU-build progress -- refreshing the filade from it is
  // how "which page is near a target" can stay current even while most of
  // that backlog is still waiting to become a Page.
  refreshPageIndexFilade();

  if (pageIndexFilade.empty()) {
    return render::kPageBuildFrameBudget;
  }

  std::optional<std::size_t> targetIndex;

  // x/z and view direction are not considered here: a document positioned
  // well outside the camera's actual view frustum simply "catches up" for no
  // visual benefit, which costs nothing else this function's own per-call
  // budget already bounds.
  if (const auto cam = cameraInfo()) {
    if (const auto hit = pageIndexFilade.findEntryAtY(cam->pagePixelY)) {
      targetIndex = hit->entryIndex;
    }
  }

  // Priority offsets -- pages a beam touches, pushed by an external caller
  // via setPriorityOffsets() -- hurry the loader towards whichever named
  // page is furthest behind build progress. Furthest rather than nearest, so
  // that widening the budget helps regardless of which tier of
  // buildPendingPages()'s own priority order ends up reaching it first.
  for (const auto offset : priorityOffsets) {
    if (const auto hit = pageIndexFilade.findEntryAtByte(offset)) {
      if (!targetIndex || hit->entryIndex > *targetIndex) {
        targetIndex = hit->entryIndex;
      }
    }
  }

  // Whether the target is already at or behind build progress -- measured
  // against builtPageCount() rather than pages.size() (Stage 2's container
  // can grow pages past any particular unbuilt index once something builds
  // out of order) and rather than "is the target itself built" (Stage 3's
  // own selection already reaches a P0/P1 target within its own call
  // regardless of budget size, since it is picked first either way; asking
  // "is it built yet" would answer no for the very next page about to be
  // built anyway -- exactly as far "ahead" as no page ever is -- and
  // trigger catch-up on every ordinary call). builtPageCount() is the
  // degenerate-case equivalent of the old pages.size() check when nothing
  // has built out of order, and a meaningful "how much real progress has
  // been made so far" once something has.
  if (!targetIndex || *targetIndex <= builtPageCount()) {
    return render::kPageBuildFrameBudget;
  }
  return render::kPageBuildFrameBudget * render::kPageBuildCatchUpMultiplier;
}

bool Doc::buildPendingPages(RenderState &state) {
  if (isFullyLoaded()) {
    return true;
  }

  std::map<std::uint32_t, PendingShaping> toBuild;
  {
    std::scoped_lock lock(shapingMutex);
    toBuild.swap(pendingShapings);
  }

  // Every key in toBuild was recorded alongside its pageEntries entry under
  // the same lock (see makePages()), so the filade already knows each one's
  // Y position without needing pages.back() -- which would not mean
  // anything to chain from once a page can be built out of order (Stage 3).
  refreshPageIndexFilade();

  // What this call wants (Stage 5 of design/priority-page-building.md): P0
  // (the viewport range) then P1 (priorityOffsets' pages), ascending,
  // deduplicated. A page not in this list is not built this call, whether
  // or not toBuild happens to already hold its shaping.
  const auto wanted = wantedPageIndices();
  const std::unordered_set<std::uint32_t> wantedSet(wanted.begin(),
                                                    wanted.end());

  // Bounded so that a backlog the background shaping thread got ahead on --
  // whether from a slow render-thread startup or simply outpacing this loop
  // -- gets spread back out over several frames instead of built in one
  // blocking call. See render::kPageBuildFrameBudget's own comment. Widened
  // to render::kPageBuildCatchUpMultiplier times that when the camera is
  // looking well past what has been built so far -- see
  // buildBudgetForThisCall() -- so scrolling ahead of load progress closes
  // that gap faster instead of waiting behind every page before it.
  const auto buildBudget = buildBudgetForThisCall();
  const auto buildStart  = std::chrono::steady_clock::now();
  for (const auto trueIndex : wanted) {
    if (nullptr != page(trueIndex)) {
      continue; // Already built.
    }
    const auto hit = pageIndexFilade.findEntryByIndex(trueIndex);
    if (!hit) {
      continue; // Background loader has not shaped this page yet.
    }

    PageShaping shaping;
    if (const auto found = toBuild.find(trueIndex); found != toBuild.end()) {
      shaping = std::move(found->second.shaping);
      toBuild.erase(found);
    } else {
      // Banked earlier, or wanted for the first time without ever having
      // passed through toBuild -- re-derive its shaping the same way
      // ensurePagesBuiltThrough() does for reflow. pageIndexFilade already
      // knows its start offset permanently, so this needs nothing
      // pendingShapings held onto.
      shaping = layoutFrom(hit->startByte);
    }

    const float pageHeightPx    = pageBoxFor(shaping).height;
    const float pageHeightWorld = pageHeightPx * pixelsToWorld;
    const float topYWorld       = -hit->startYPx * pixelsToWorld;
    const float centerY         = topYWorld - (pageHeightWorld / 2.0F);

    glm::mat4 trans =
        glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, centerY, 0.0F));
    trans = glm::scale(trans, glm::vec3(pixelsToWorld, pixelsToWorld, 1.0F));
    placePageAt(state, trueIndex, std::move(shaping), hit->startByte, trans);

    // Always build at least one page per call even if it alone exceeds the
    // budget, so a single expensive page (e.g. one that grows the glyph
    // atlas) cannot stall progress -- checked after building rather than
    // before, since the cost being budgeted is the build that just ran.
    if (std::chrono::steady_clock::now() - buildStart >= buildBudget) {
      break;
    }
  }

  // Bank what is left: a still-wanted page the budget did not reach goes
  // back to pendingShapings, same as before Stage 5. Everything else -- P2
  // in the old tiering, now simply "not currently wanted" -- is dropped
  // here instead: its heavy PageShaping goes with it, and
  // pageEntries/pageIndexFilade already remember its offset and length
  // permanently, which is all the re-shape path above needs if it becomes
  // wanted again.
  std::erase_if(toBuild, [&](const auto &entry) {
    return !wantedSet.contains(entry.first);
  });
  if (!toBuild.empty()) {
    // Keys never collide with what the background thread has appended since
    // the swap above -- every one names a page index this call either built
    // or never reached -- and merge() splices nodes rather than copying each
    // still-large PageShaping.
    std::scoped_lock lock(shapingMutex);
    pendingShapings.merge(toBuild);
  }

  const bool done = isFullyLoaded();
  if (done) {
    pool->trim();
  }
  return done;
}

void Doc::newPage(RenderState &state, PageShaping aShaping,
                  const std::uint32_t textOffset) {
  const auto index  = pages.size();
  float currentTopY = 0.0F;
  if (!pages.empty() && pages.back()) {
    // pages.back() is re-invoked after the truthiness check; nothing
    // mutates pages between the two calls.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    const auto &prevPage        = *pages.back();
    const float prevCenterY     = prevPage.getModel()[3][1];
    const float prevHeightWorld = prevPage.heightPixels() * pixelsToWorld;
    const float prevBottomY     = prevCenterY - (prevHeightWorld / 2.0F);
    currentTopY                 = prevBottomY - (pageGapPx * pixelsToWorld);
  }
  const float pageHeightPx    = pageBoxFor(aShaping).height;
  const float pageHeightWorld = pageHeightPx * pixelsToWorld;
  const float centerY         = currentTopY - (pageHeightWorld / 2.0F);

  glm::mat4 trans =
      glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, centerY, 0.0F));
  trans = glm::scale(trans, glm::vec3(pixelsToWorld, pixelsToWorld, 1.0F));
  placePageAt(state, index, std::move(aShaping), textOffset, trans);
}

void Doc::placePageAt(RenderState &state, const std::size_t index,
                      PageShaping shaping, const std::uint32_t textOffset,
                      const glm::mat4 &trans,
                      const BufferPool::Allocation &inherited) {
  if (pages.size() <= index) {
    pages.resize(index + 1);
  }
  // Page's constructor takes its model by mutable reference.
  glm::mat4 transCopy = trans;
  pages[index].emplace(getPtr(), state, transCopy, std::move(shaping),
                       textOffset, static_cast<std::uint32_t>(index),
                       inherited);
}

void Doc::ensurePagesBuiltThrough(RenderState &state,
                                  const std::size_t exclusiveEnd) {
  if (pages.size() < exclusiveEnd) {
    pages.resize(exclusiveEnd);
  }
  bool anyMissing = false;
  for (std::size_t i = 0; i < exclusiveEnd; i++) {
    if (!pages[i]) {
      anyMissing = true;
      break;
    }
  }
  if (!anyMissing) {
    return;
  }

  refreshPageIndexFilade();
  for (std::size_t i = 0; i < exclusiveEnd; i++) {
    if (pages[i]) {
      continue;
    }
    const auto hit = pageIndexFilade.findEntryByIndex(i);
    if (!hit) {
      // Not even shaped yet -- nothing this guard can do; buildPendingPages()
      // will fill it in once the background loader reaches it.
      continue;
    }
    auto shaping                = layoutFrom(hit->startByte);
    const float pageHeightPx    = pageBoxFor(shaping).height;
    const float pageHeightWorld = pageHeightPx * pixelsToWorld;
    const float topYWorld       = -hit->startYPx * pixelsToWorld;
    const float centerY         = topYWorld - (pageHeightWorld / 2.0F);

    glm::mat4 trans =
        glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, centerY, 0.0F));
    trans = glm::scale(trans, glm::vec3(pixelsToWorld, pixelsToWorld, 1.0F));
    placePageAt(state, i, std::move(shaping), hit->startByte, trans);
    {
      std::scoped_lock lock(shapingMutex);
      pendingShapings.erase(static_cast<std::uint32_t>(i));
    }
  }
}

std::size_t Doc::builtPageCount() const {
  return static_cast<std::size_t>(std::count_if(
      pages.begin(), pages.end(),
      [](const std::optional<Page> &slot) { return slot.has_value(); }));
}

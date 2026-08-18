/**
 * @file canvas.cpp
 * @brief Implementation of the rectangle-and-text drawing surface.
 */
#include <gleditor/canvas.hpp> // IWYU pragma: associated

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <glm/gtc/type_ptr.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/glyphcache/cache.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/text/font.hpp>
#include <gleditor/text/layout.hpp>

namespace {

/// Copy a matrix into the flat array the device uniform structs carry.
std::array<float, 16> toArray(const glm::mat4 &mat) {
  std::array<float, 16> out{};
  const auto *src = glm::value_ptr(mat);
  std::copy_n(src, out.size(), out.begin());
  return out;
}

} // namespace

namespace gleditor {

Canvas::Canvas(render::RenderDevice *const aDevice, std::string aFontName,
               const std::uint32_t initialRows)
    : device(aDevice), fontName(std::move(aFontName)),
      pool(std::make_unique<BufferPool>(aDevice, sizeof(Doc::VBORow),
                                        initialRows)),
      tagKind(render::tagKindOverlay) {}

Canvas::~Canvas() = default;

void Canvas::createPipeline(const render::PipelineDesc &documentDesc,
                            const bool depthTest) {
  render::PipelineDesc desc = documentDesc;
  desc.name                 = "canvas";
  desc.depthTest            = depthTest;
  pipeline                  = device->createPipeline(desc);
}

void Canvas::clear() {
  rows.clear();
  pendingInstances = 0;
}

void Canvas::setTag(const std::uint32_t kind, const std::uint32_t index) {
  tagKind  = kind;
  tagIndex = index;
}

void Canvas::setIdentity(const std::uint32_t docIndex,
                         const std::uint32_t pageIndex) {
  identity = render::packTagIdentity(0, docIndex, pageIndex);
}

void Canvas::pushQuad(const float centreX, const float centreY,
                      const float width, const float height,
                      const std::uint32_t foreground,
                      const std::uint32_t background, const std::uint32_t layer,
                      const float texX, const float texY, const bool solid) {
  // The width and height fields the vertex stage unpacks are 12 bits each, so
  // a quad larger than this cannot be described. Clamping rather than asserting
  // because the sizes come from whatever a caller is drawing, and a panel too
  // big is a visual mistake where a failed assertion is a crash.
  const auto clamp = [](const float value) {
    return static_cast<unsigned int>(std::clamp(
        value, 0.0F, static_cast<float>(Doc::VBORow::maxQuadExtent)));
  };

  const Doc::VBORow row{
      {centreX, centreY},
      Doc::VBORow::ink(foreground, Doc::VBORow::onPaper, solid),
      Doc::VBORow::atlasAt(static_cast<unsigned int>(texX),
                           static_cast<unsigned int>(texY)),
      Doc::VBORow::box(static_cast<unsigned char>(layer), clamp(width),
                       clamp(height), tagKind),
      Doc::VBORow::paperAt(background, tagIndex)};

  const auto *const bytes = reinterpret_cast<const std::byte *>(&row);
  rows.insert(rows.end(), bytes, bytes + sizeof(row));
  pendingInstances++;
}

void Canvas::addRect(const float left, const float bottom, const float width,
                     const float height, const std::uint32_t colour) {
  if (width <= 0.0F || height <= 0.0F) {
    return;
  }
  // Solid, so the fragment stage fills it with this colour and never samples
  // the atlas.
  pushQuad(left + (width / 2.0F), bottom + (height / 2.0F), width, height,
           colour, colour, 0, 0.0F, 0.0F, true);
}

void Canvas::addLine(const float fromX, const float fromY, const float toX,
                     const float toY, const float thickness,
                     const std::uint32_t colour) {
  const auto minX  = std::min(fromX, toX);
  const auto minY  = std::min(fromY, toY);
  const auto spanX = std::abs(toX - fromX);
  const auto spanY = std::abs(toY - fromY);
  // A segment with no extent in one axis is exactly a thin rectangle; one with
  // extent in both is covered by its bounding box, which is the closest an
  // axis-aligned quad gets.
  addRect(minX - (thickness / 2.0F), minY - (thickness / 2.0F),
          std::max(spanX, thickness), std::max(spanY, thickness), colour);
}

TextMetrics Canvas::measureText(const std::string_view utf8) const {
  auto font = text::FontManager::instance().getFont(fontName);
  text::LayoutOptions opts{
      .maxWidthPx      = textWidthLimit > 0 ? static_cast<float>(textWidthLimit) : 0.0F,
      .maxHeightPx     = 0.0F,
      .singleParagraph = true,
      .ellipsize       = textWidthLimit > 0,
  };
  auto shaping = text::TextLayout::layoutSingleLine(utf8, font, opts);
  return {static_cast<float>(shaping.textWidthPx),
          static_cast<float>(shaping.textHeightPx)};
}

TextMetrics Canvas::addText(RenderState &state, const float left,
                            const float top, const std::string_view utf8,
                            const std::uint32_t colour,
                            const std::uint32_t background) {
  auto font = text::FontManager::instance().getFont(fontName);
  if (!font) {
    return {};
  }

  text::LayoutOptions opts{
      .maxWidthPx      = textWidthLimit > 0 ? static_cast<float>(textWidthLimit) : 0.0F,
      .maxHeightPx     = 0.0F,
      .singleParagraph = true,
      .ellipsize       = textWidthLimit > 0,
  };
  auto shaping = text::TextLayout::layoutSingleLine(utf8, font, opts);
  if (shaping.textWidthPx <= 0 || shaping.textHeightPx <= 0) {
    return {};
  }

  for (const auto &g : shaping.glyphs) {
    const auto glyph  = state.glyphCache.put(g.chr, font);
    const auto width  = static_cast<float>(static_cast<int>(glyph.dims.width));
    const auto height = static_cast<float>(static_cast<int>(glyph.dims.height));
    if (0.0F == width || 0.0F == height) {
      continue;
    }

    const auto glyphLeft = left + g.clusterLeft;
    const auto glyphTop  = top - g.clusterTop;

    pushQuad(glyphLeft + (width / 2.0F), glyphTop - (height / 2.0F), width,
             height, colour, background,
             static_cast<std::uint32_t>(glyph.layer), glyph.texCoords.topLeft.x,
             glyph.texCoords.topLeft.y, false);
  }

  return {static_cast<float>(shaping.textWidthPx),
          static_cast<float>(shaping.textHeightPx)};
}

void Canvas::commit() {
  if (!backing.empty()) {
    pool->release(backing);
    backing = {};
  }
  committedInstances = pendingInstances;
  if (0 == committedInstances) {
    return;
  }
  backing = pool->reserve(committedInstances);
  pool->write(backing, 0, std::span<const std::byte>(rows));
}

void Canvas::draw(RenderState &state, const glm::mat4 &transform,
                  const float opacity) const {
  if (0 == committedInstances || !pipeline.valid()) {
    return;
  }
  state.device->bindPipeline(pipeline);
  state.device->bindGlyphTexture(state.glyphCache.textureHandle());
  const render::DrawUniforms uniforms{toArray(transform), opacity, identity};
  state.device->drawGlyphs(uniforms, pool->buffer(), pool->byteOffset(backing),
                           committedInstances);
}

} // namespace gleditor

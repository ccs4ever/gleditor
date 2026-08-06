/**
 * @file caret.cpp
 * @brief Implementation of the editing caret.
 */
#include <gleditor/caret.hpp> // IWYU pragma: associated

#include <array>
#include <cstddef>
#include <span>

#include <format>
#include <iostream>
#include <format>
#include <iostream>
#include <glm/gtc/type_ptr.hpp>

#include <gleditor/doc.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render_state.hpp>

namespace {

/// Rows the caret needs: one quad.
constexpr std::uint32_t caretRows = 1;

/// Copy a matrix into the flat array the device uniform structs carry.
std::array<float, 16> toArray(const glm::mat4 &mat) {
  std::array<float, 16> out{};
  const auto *src = glm::value_ptr(mat);
  std::copy_n(src, out.size(), out.begin());
  return out;
}

} // namespace

Caret::Caret(render::RenderDevice *aDevice)
    : device(aDevice),
      pool(std::make_unique<BufferPool>(aDevice, sizeof(Doc::VBORow),
                                        caretRows)) {
  backing = pool->reserve(caretRows);
}

Caret::~Caret() = default;

void Caret::createPipeline(const render::PipelineDesc &documentDesc) {
  render::PipelineDesc desc = documentDesc;
  desc.name                 = "caret";
  desc.depthTest            = false;
  pipeline                  = device->createPipeline(desc);
}

void Caret::placeAt(const std::uint32_t aDocIndex,
                    const std::uint32_t aByteOffset) {
  docIndex    = aDocIndex;
  byteOffset_ = aByteOffset;
  visible     = true;
}

void Caret::shiftForInsertion(const std::uint32_t at,
                              const std::uint32_t bytes) {
  // An insertion exactly at the caret pushes the caret after what was typed,
  // which is where the next character belongs.
  if (at <= byteOffset_) {
    byteOffset_ += bytes;
  }
}

void Caret::setGeometry(const float posX, const float posY,
                        const float height) {
  // Width and height are packed as unsigned integers, so a caret shorter than
  // a pixel would vanish rather than round down to a thin line.
  const auto pixelHeight = std::max(1U, static_cast<unsigned int>(height));
  constexpr auto pixelWidth = static_cast<unsigned int>(widthPixels);

  const auto colour = Doc::VBORow::color3(32, 96, 220);
  const Doc::VBORow row{
      {posX, posY, 0.2F},
      colour,
      colour,
      {0, 0},
      {0, 0},
      Doc::VBORow::layerWidthHeight(0, pixelWidth, pixelHeight),
      // Tag zero: the caret is not something to pick. Clicking where it sits
      // should report the glyph underneath, not the caret itself.
      {0, 0}};

  const std::span<const std::byte> bytes{
      reinterpret_cast<const std::byte *>(&row), sizeof(row)};
  pool->write(backing, 0, bytes);
  hasGeometry = true;
}

void Caret::draw(RenderState &state, const glm::mat4 &pageTransform) const {
  if (!visible || !hasGeometry || !pipeline.valid()) {
    return;
  }
  // The caret's own pipeline, which does not depth test. It sits in the same
  // plane as the text it marks, close enough that which of the two wins would
  // otherwise come down to rounding -- and the glyph backgrounds won, punching
  // white holes through it. Submitted after the documents, it is simply on top.
  state.device->bindPipeline(pipeline);
  state.device->bindGlyphTexture(state.glyphCache.textureHandle());

  const render::DrawUniforms uniforms{toArray(pageTransform)};
  state.device->drawGlyphs(uniforms, pool->buffer(), pool->byteOffset(backing),
                           caretRows);
}
// vi: set sw=2 sts=2 ts=2 et:

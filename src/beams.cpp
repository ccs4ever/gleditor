/**
 * @file beams.cpp
 * @brief Implementation of the beam batch.
 */
#include <gleditor/beams.hpp> // IWYU pragma: associated

#include <algorithm>
#include <cstddef>
#include <span>

#include <glm/gtc/type_ptr.hpp>

#include <gleditor/render/device.hpp>
#include <gleditor/render/shader_source.hpp>
#include <gleditor/render_state.hpp>

namespace gleditor {

namespace {

/// Copy a matrix into the flat array the device uniform structs carry.
std::array<float, 16> toArray(const glm::mat4 &mat) {
  std::array<float, 16> out{};
  const auto *src = glm::value_ptr(mat);
  std::copy_n(src, out.size(), out.begin());
  return out;
}

} // namespace

render::VertexLayout Beams::layout() {
  using render::AttributeType;
  static_assert(sizeof(Beams::Row) == 36,
                "the beam record is read by a shader that names its fields by "
                "offset; padding it would silently shift every attribute");

  render::VertexLayout out;
  out.stride     = sizeof(Row);
  out.attributes = {
      {"beamFrom", 0, AttributeType::Float, 3, offsetof(Row, from)},
      {"beamWidth", 1, AttributeType::Float, 1, offsetof(Row, width)},
      {"beamTo", 2, AttributeType::Float, 3, offsetof(Row, to)},
      {"beamColour", 3, AttributeType::UnsignedInt, 1, offsetof(Row, colour)},
      {"beamTag", 4, AttributeType::UnsignedInt, 1, offsetof(Row, tag)},
  };
  return out;
}

Beams::Beams(render::RenderDevice *const aDevice,
             const std::uint32_t initialRows)
    : device(aDevice),
      pool(std::make_unique<BufferPool>(aDevice, sizeof(Row), initialRows)) {}

Beams::~Beams() = default;

void Beams::createPipeline(const std::string &assetDir,
                           const std::string &spirvDir, const bool depthTest) {
  render::PipelineDesc desc;
  desc.name = "beam";
  // Which SPIR-V the Vulkan backend loads follows from this, so it has to be
  // the base name the shader files were written under.
  desc.shaderName     = "beam";
  desc.vertexSource   = render::readShaderBody(assetDir + "/beam.vert.glsl");
  desc.fragmentSource = render::readShaderBody(assetDir + "/beam.frag.glsl");
  desc.spirvDir       = spirvDir;
  desc.layout         = layout();
  desc.depthTest      = depthTest;
  pipeline            = device->createPipeline(desc);
}

void Beams::clear() { rows.clear(); }

void Beams::add(const glm::vec3 &from, const glm::vec3 &to, const float width,
                const std::uint32_t colour, const std::uint32_t tag) {
  rows.push_back(
      Row{{from.x, from.y, from.z}, width, {to.x, to.y, to.z}, colour, tag});
}

void Beams::commit() {
  if (!backing.empty()) {
    pool->release(backing);
    backing = {};
  }
  committedRows = static_cast<std::uint32_t>(rows.size());
  if (0 == committedRows) {
    return;
  }
  backing = pool->reserve(committedRows);
  pool->write(backing, 0,
              std::as_bytes(std::span<const Row>(rows.data(), rows.size())));
}

void Beams::draw(RenderState &state, const glm::mat4 &transform,
                 const float opacity, const std::uint32_t identity) const {
  if (0 == committedRows || !pipeline.valid()) {
    return;
  }
  state.device->bindPipeline(pipeline);
  // A beam samples nothing, but the pipeline's descriptor set is the same shape
  // as every other one -- the highlight block and the atlas -- and Vulkan wants
  // it filled in whether or not the shader reads from it. Binding the atlas is
  // what fills it.
  state.device->bindGlyphTexture(state.glyphCache.textureHandle());
  const render::DrawUniforms uniforms{toArray(transform), opacity, identity};
  state.device->drawGlyphs(uniforms, pool->buffer(), pool->byteOffset(backing),
                           committedRows);
}

} // namespace gleditor

// vi: set sw=2 sts=2 ts=2 et:

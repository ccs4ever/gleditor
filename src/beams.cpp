/**
 * @file beams.cpp
 * @brief Implementation of the beam batch.
 */
#include <gleditor/beams.hpp> // IWYU pragma: associated

#include <algorithm>
#include <cstddef>
#include <span>

#include <glm/geometric.hpp>
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
  static_assert(sizeof(Beams::Row) == 72,
                "the beam record is read by a shader that names its fields by "
                "offset; padding it would silently shift every attribute");

  render::VertexLayout out;
  out.stride     = sizeof(Row);
  out.attributes = {
      {.name       = "beamFrom",
       .location   = 0,
       .type       = AttributeType::Float,
       .components = 3,
       .offset     = offsetof(Row, from)},
      {.name       = "beamWidth",
       .location   = 1,
       .type       = AttributeType::Float,
       .components = 1,
       .offset     = offsetof(Row, width)},
      {.name       = "beamTo",
       .location   = 2,
       .type       = AttributeType::Float,
       .components = 3,
       .offset     = offsetof(Row, to)},
      {.name       = "beamColour",
       .location   = 3,
       .type       = AttributeType::UnsignedInt,
       .components = 1,
       .offset     = offsetof(Row, colour)},
      {.name       = "beamTag",
       .location   = 4,
       .type       = AttributeType::UnsignedInt,
       .components = 1,
       .offset     = offsetof(Row, tag)},
      {.name       = "beamAlong",
       .location   = 5,
       .type       = AttributeType::Float,
       .components = 2,
       .offset     = offsetof(Row, along)},
      {.name       = "beamFromNormal",
       .location   = 6,
       .type       = AttributeType::Float,
       .components = 3,
       .offset     = offsetof(Row, fromNormal)},
      {.name       = "beamToNormal",
       .location   = 7,
       .type       = AttributeType::Float,
       .components = 3,
       .offset     = offsetof(Row, toNormal)},
      {.name       = "beamSurface",
       .location   = 8,
       .type       = AttributeType::Float,
       .components = 1,
       .offset     = offsetof(Row, surface)},
  };
  return out;
}

Beams::Beams(render::RenderDevice *const aDevice,
             const std::uint32_t initialRows)
    : device(aDevice),
      pool(std::make_unique<BufferPool>(aDevice, sizeof(Row), initialRows)) {
  rows.reserve(initialRows);
  backing = pool->reserve(initialRows);
}

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
                const std::uint32_t colour, const std::uint32_t tag,
                const float alongFrom, const float alongTo) {
  rows.push_back(Row{.from   = {from.x, from.y, from.z},
                     .width  = width,
                     .to     = {to.x, to.y, to.z},
                     .colour = colour,
                     .tag    = tag,
                     .along  = {alongFrom, alongTo}});
}

void Beams::addPath(const std::span<const glm::vec3> through, const float width,
                    const std::uint32_t colour, const std::uint32_t tag) {
  if (through.size() < 2) {
    return;
  }
  // By arc length rather than by segment count, so a route made of one long
  // run and two short elbows fades over the run rather than spending a third
  // of the fade on each elbow.
  float total = 0.0F;
  for (std::size_t i = 1; i < through.size(); i++) {
    total += glm::distance(through[i - 1], through[i]);
  }
  if (total <= 0.0F) {
    return;
  }
  float travelled = 0.0F;
  for (std::size_t i = 1; i < through.size(); i++) {
    const float here = travelled;
    travelled += glm::distance(through[i - 1], through[i]);
    add(through[i - 1], through[i], width, colour, tag, here / total,
        travelled / total);
  }
}

void Beams::addNurbs(const NurbsPath &path, unsigned segments, float width,
                     std::uint32_t colour, std::uint32_t tag, float phase,
                     Surface surface) {
  const auto sampled = sampleNurbs(path, segments);
  if (sampled.length <= 0) return;
  const auto normal = [](glm::vec3 tangent, glm::vec3 fallback) {
    auto n = glm::cross(tangent, glm::vec3(0, 0, 1));
    if (glm::dot(n, n) < 1e-6F) n = glm::cross(fallback, glm::vec3(0, 0, 1));
    return glm::dot(n, n) > 1e-6F ? glm::normalize(n) : glm::vec3(0);
  };
  for (unsigned i = 1; i < sampled.count; ++i) {
    const auto &a = sampled.points[i - 1], &b = sampled.points[i];
    const auto fallback = b.position - a.position;
    const auto na       = normal(a.tangent, fallback),
               nb       = normal(b.tangent, fallback);
    add(a.position, b.position, width, colour, tag, a.along - phase,
        b.along - phase);
    rows.back().fromNormal = {na.x, na.y, na.z};
    rows.back().toNormal   = {nb.x, nb.y, nb.z};
    rows.back().surface    = surface == Surface::Glass ? 1.F : 0.F;
  }
}

void Beams::commit() {
  committedRows = static_cast<std::uint32_t>(rows.size());
  if (0 == committedRows) return;
  if (pool->rowCount(backing) < committedRows)
    pool->resize(backing, committedRows, BufferPool::Contents::Discard);
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
  state.device->bindAtlasTexture(state.glyphCache.textureHandle());
  const render::DrawUniforms uniforms{
      .mvp = toArray(transform), .opacity = opacity, .identity = identity};
  state.device->drawGlyphs(uniforms, pool->buffer(), pool->byteOffset(backing),
                           committedRows);
}

} // namespace gleditor

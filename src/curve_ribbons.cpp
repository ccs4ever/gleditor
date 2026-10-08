#include <algorithm>
#include <cmath>
#include <cstddef>
#include <gleditor/curve_ribbons.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render/shader_source.hpp>
#include <gleditor/render_state.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <stdexcept>

namespace gleditor {
render::VertexLayout CurveRibbons::layout() {
  using render::AttributeType;
  static_assert(sizeof(Row) == 84);
  render::VertexLayout result;
  result.stride     = sizeof(Row);
  result.attributes = {
      {"curveFrom", 0, AttributeType::Float, 2, offsetof(Row, from)},
      {"curveNormals", 1, AttributeType::Float, 4, offsetof(Row, normals)},
      {"curveTo", 2, AttributeType::Float, 2, offsetof(Row, to)},
      {"curveWidths", 3, AttributeType::Float, 2, offsetof(Row, widths)},
      {"curveInterval", 4, AttributeType::Float, 2, offsetof(Row, interval)},
      {"curveColour", 5, AttributeType::UnsignedInt, 1, offsetof(Row, colour)},
      {"curveTag", 6, AttributeType::UnsignedInt, 1, offsetof(Row, tag)},
      {"curveSurface", 7, AttributeType::Float, 4, offsetof(Row, surface)},
      {"curveHole", 8, AttributeType::Float, 3, offsetof(Row, hole)}};
  return result;
}
CurveRibbons::CurveRibbons(render::RenderDevice *device)
    : device_(device),
      pool_(std::make_unique<BufferPool>(device, sizeof(Row), maxSegments)) {
  rows_.reserve(maxSegments);
  backing_ = pool_->reserve(maxSegments);
}
CurveRibbons::~CurveRibbons() = default;
void CurveRibbons::createPipeline(const std::string &assets,
                                  const std::string &spirv) {
  render::PipelineDesc desc;
  desc.name = desc.shaderName = "curve_ribbon";
  desc.vertexSource =
      render::readShaderBody(assets + "/curve_ribbon.vert.glsl");
  desc.fragmentSource =
      render::readShaderBody(assets + "/curve_ribbon.frag.glsl");
  desc.spirvDir  = spirv;
  desc.layout    = layout();
  desc.depthTest = false;
  pipeline_      = device_->createPipeline(desc);
}
void CurveRibbons::clear() { rows_.clear(); }
void CurveRibbons::add(glm::vec2 from, glm::vec2 control, glm::vec2 to,
                       Style style, std::uint32_t colour, std::uint32_t tag,
                       glm::vec2 holeCentre, float holeRadius) {
  const std::array points{glm::vec3(from, 0), glm::vec3(control, 0),
                          glm::vec3(to, 0)};
  const std::array weights{1.F, 1.F, 1.F};
  const std::array knots{0.F, 0.F, 0.F, 1.F, 1.F, 1.F};
  addNurbs(NurbsPath(points, weights, knots, 2), style, colour, tag, holeCentre,
           holeRadius);
}
void CurveRibbons::addNurbs(const NurbsPath &path, Style style,
                            std::uint32_t colour, std::uint32_t tag,
                            glm::vec2 holeCentre, float holeRadius) {
  for (const float value :
       {style.startWidth, style.endWidth, style.edgeSoftness,
        style.texturePeriod, style.textureStrength, style.maxSegmentLength,
        holeCentre.x, holeCentre.y, holeRadius})
    if (!std::isfinite(value))
      throw std::invalid_argument("non-finite curve ribbon");
  if (style.startWidth <= 0 || style.endWidth <= 0 || style.edgeSoftness <= 0 ||
      style.texturePeriod <= 0 || style.textureStrength < 0 ||
      style.textureStrength > 1 || style.maxSegmentLength <= 0 ||
      holeRadius < 0)
    throw std::invalid_argument("invalid curve ribbon style");
  const float estimate = path.controlLength();
  if (estimate <= 0) return;
  const auto segments = static_cast<unsigned>(
      std::clamp(std::ceil(estimate / style.maxSegmentLength), 1.F,
                 static_cast<float>(maxSegments)));
  const auto sampled = sampleNurbs(path, segments);
  if (sampled.length <= 0) return;
  const auto normal = [](glm::vec3 tangent, glm::vec3 fallback) {
    glm::vec2 direction(tangent);
    if (glm::dot(direction, direction) < 1e-6F) direction = glm::vec2(fallback);
    if (glm::dot(direction, direction) < 1e-6F) direction = {1, 0};
    return glm::normalize(glm::vec2(-direction.y, direction.x));
  };
  for (unsigned i = 1; i < sampled.count; ++i) {
    const auto &a = sampled.points[i - 1], &b = sampled.points[i];
    const auto fallback = b.position - a.position;
    const auto na       = normal(a.tangent, fallback),
               nb       = normal(b.tangent, fallback);
    rows_.push_back({{a.position.x, a.position.y},
                     {b.position.x, b.position.y},
                     {style.startWidth, style.endWidth},
                     {a.along, b.along},
                     {na.x, na.y, nb.x, nb.y},
                     colour,
                     tag,
                     {style.edgeSoftness, style.texturePeriod,
                      style.textureStrength, sampled.length},
                     {holeCentre.x, holeCentre.y, holeRadius}});
  }
}
void CurveRibbons::commit() {
  if (pool_->rowCount(backing_) < rows_.size())
    pool_->resize(backing_, static_cast<std::uint32_t>(rows_.size()),
                  BufferPool::Contents::Discard);
  if (!rows_.empty())
    pool_->write(backing_, 0, std::as_bytes(std::span{rows_}));
  committed_ = static_cast<std::uint32_t>(rows_.size());
}
void CurveRibbons::draw(RenderState &state, const glm::mat4 &matrix) const {
  if (!committed_ || !pipeline_.valid()) return;
  render::DrawUniforms uniforms;
  std::copy_n(glm::value_ptr(matrix), 16, uniforms.mvp.begin());
  state.device->bindPipeline(pipeline_);
  state.device->bindAtlasTexture(state.glyphCache.textureHandle());
  state.device->drawGlyphs(uniforms, pool_->buffer(),
                           pool_->byteOffset(backing_), committed_);
}
} // namespace gleditor

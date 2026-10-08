#ifndef GLEDITOR_CURVE_RIBBONS_HPP
#define GLEDITOR_CURVE_RIBBONS_HPP

#include <array>
#include <gleditor/buffer_pool.hpp>
#include <gleditor/render/types.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <memory>
#include <vector>

struct RenderState;
namespace gleditor {
/// Screen-plane quadratic ribbons with continuous normals, tapered widths,
/// antialiased edges and stationary filament shading. No application semantics.
class CurveRibbons {
public:
  struct Style {
    float startWidth{4}, endWidth{1};
    float edgeSoftness{1}, texturePeriod{24}, textureStrength{.18F};
    float maxSegmentLength{4};
  };
  struct Row {
    std::array<float, 2> from, control, to, widths, interval;
    std::uint32_t colour{}, tag{};
    std::array<float, 4> surface;
    std::array<float, 3> hole;
  };
  explicit CurveRibbons(render::RenderDevice *device);
  ~CurveRibbons();
  static render::VertexLayout layout();
  void createPipeline(const std::string &assets, const std::string &spirv);
  void clear();
  /// holeRadius excludes a screen-plane circle from the entire ribbon, so
  /// transient pointer feedback never steals the target's picking pixel.
  void add(glm::vec2 from, glm::vec2 control, glm::vec2 to, Style style,
           std::uint32_t colour, std::uint32_t tag, glm::vec2 holeCentre = {},
           float holeRadius = 0);
  void commit();
  void draw(RenderState &, const glm::mat4 &) const;
  [[nodiscard]] const std::vector<Row> &pending() const { return rows_; }

private:
  // Four-pixel subdivision keeps curved silhouettes smooth at typical UI
  // distances; the cap bounds per-curve upload work even for extreme
  // coordinates.
  static constexpr std::uint32_t maxSegments = 256;
  render::RenderDevice *device_;
  std::unique_ptr<BufferPool> pool_;
  render::PipelineHandle pipeline_{};
  BufferPool::Allocation backing_{};
  std::vector<Row> rows_;
  std::uint32_t committed_{};
};
} // namespace gleditor
#endif

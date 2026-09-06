/**
 * @file tenuous_tether.hpp
 * @brief Tenuous elastic tether ribbons connecting flying documents to origin
 * context.
 *
 * When a linked passage or document flies forward out of the background plane
 * (Z = -40) into collinear reading alignment (Z = 0), it remains anchored to
 * its origin slot via a faint, semi-transparent elastic tether ribbon
 * (quadratic Bezier arc with alpha ~ 0.25), preventing visual disorientation
 * and maintaining Nelsonian deep provenance.
 */
#ifndef XUDU_TENUOUS_TETHER_HPP
#define XUDU_TENUOUS_TETHER_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <glm/ext/matrix_float4x4.hpp>
#include <glm/ext/vector_float3.hpp>

#include <gleditor/beams.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/renderer.hpp>

namespace xudu {

/**
 * @struct FlyingTetherAnchor
 * @brief Record of a document currently displaced from its background resting
 * position.
 */
struct FlyingTetherAnchor {
  std::size_t docIndex{0};
  glm::vec3 originPos{
      0.0F}; ///< Resting home coordinate in background (Z ~ -40)
  glm::vec3 currentPos{0.0F}; ///< Current physical coordinate (Z ~ 0)
  float width{50.0F};
  float height{70.0F};
  std::uint32_t colour{0x38BDF844}; ///< Faint ethereal cyan (alpha ~ 0.25)
  bool active{true};
};

/**
 * @class TenuousTetherOverlay
 * @brief FrameContributor that stages and draws quadratic Bezier ribbons and
 * origin footprints.
 */
class TenuousTetherOverlay : public gleditor::FrameContributor {
public:
  TenuousTetherOverlay(RendererRef renderer, render::RenderDevice *device);
  ~TenuousTetherOverlay() override;

  TenuousTetherOverlay(const TenuousTetherOverlay &)            = delete;
  TenuousTetherOverlay &operator=(const TenuousTetherOverlay &) = delete;

  /// Register or update a flying tether anchor.
  void setTether(FlyingTetherAnchor anchor);

  /// Remove tether for a specific document.
  void removeTether(std::size_t docIndex);

  /// Clear all registered tethers.
  void clear();

  /// Retrieve all active tethers.
  [[nodiscard]] const std::vector<FlyingTetherAnchor> &
  tethers() const noexcept {
    return tethers_;
  }

  /// Whether any tethers are active and visible.
  [[nodiscard]] bool hasActiveTethers() const noexcept;

  // -- FrameContributor implementation ---------------------------------------
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &documentPipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool busy() const override { return false; }

  /// Evaluate quadratic Bezier arc at parameter t in [0, 1].
  [[nodiscard]] static inline glm::vec3 evaluateBezier(const glm::vec3 &origin,
                                                       const glm::vec3 &control,
                                                       const glm::vec3 &flying,
                                                       const float t) noexcept {
    const float inv = 1.0F - t;
    return (inv * inv * origin) + (2.0F * inv * t * control) + (t * t * flying);
  }

  /// Compute arched 3D control point dipping into depth.
  [[nodiscard]] static inline glm::vec3
  computeControlPoint(const glm::vec3 &origin, const glm::vec3 &flying,
                      const float depthOffset = 15.0F) noexcept {
    glm::vec3 mid = 0.5F * (origin + flying);
    mid.z         = std::min(origin.z, flying.z) - depthOffset;
    return mid;
  }

private:
  RendererRef renderer_;
  render::RenderDevice *device_{nullptr};
  std::unique_ptr<gleditor::Beams> beams_;
  std::vector<FlyingTetherAnchor> tethers_;
  bool visible_{true};
};

} // namespace xudu

#endif // XUDU_TENUOUS_TETHER_HPP

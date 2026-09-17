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
#include <gleditor/spatial.hpp>

#include "common/xanadu/universal_link_endpoint.hpp"

namespace xudu {

using xanadu::LinkTargetKind;

/**
 * @struct FlyingTetherAnchor
 * @brief Record of a document or cell currently displaced from its background
 * resting position.
 */
struct FlyingTetherAnchor {
  union {
    std::size_t docIndex{0};
    std::size_t targetId;
  };
  LinkTargetKind targetKind{LinkTargetKind::Document};
  zigzag::CellRef cellRef{zigzag::noCell};

  glm::vec3 originPos{
      0.0F}; ///< Resting home coordinate in background (Z ~ -40)
  glm::vec3 currentPos{0.0F}; ///< Current physical coordinate (Z ~ 0)
  float width{50.0F};
  float height{70.0F};
  std::uint32_t colour{0x38BDF844}; ///< Faint ethereal cyan (alpha ~ 0.25)
  bool active{true};

  [[nodiscard]] constexpr bool isDocument() const noexcept {
    return targetKind == LinkTargetKind::Document;
  }
  [[nodiscard]] constexpr bool isCell() const noexcept {
    return targetKind == LinkTargetKind::ZigzagCell;
  }
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

  /// Remove tether for a specific document or cell.
  void removeTether(std::size_t targetId,
                    LinkTargetKind kind = LinkTargetKind::Document);

  /// Remove tether for a specific cell.
  void removeCellTether(zigzag::CellRef cell) {
    removeTether(static_cast<std::size_t>(cell), LinkTargetKind::ZigzagCell);
  }

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
    return gleditor::spatial::evaluateQuadraticBezier(origin, control, flying,
                                                      t);
  }

  /// Compute arched 3D control point dipping into depth.
  [[nodiscard]] static inline glm::vec3
  computeControlPoint(const glm::vec3 &origin, const glm::vec3 &flying,
                      const float depthOffset = 15.0F) noexcept {
    glm::vec3 mid = 0.5F * (origin + flying);
    mid.z         = std::min(origin.z, flying.z) - depthOffset;
    return mid;
  }

  void setTessellationSegments(const std::size_t segs) noexcept {
    segments_ = segs;
  }
  [[nodiscard]] std::size_t tessellationSegments() const noexcept {
    return segments_;
  }
  void setControlDepth(const float depth) noexcept { controlDepth_ = depth; }
  [[nodiscard]] float controlDepth() const noexcept { return controlDepth_; }

private:
  RendererRef renderer_;
  render::RenderDevice *device_{nullptr};
  std::unique_ptr<gleditor::Beams> beams_;
  std::vector<FlyingTetherAnchor> tethers_;
  bool visible_{true};
  std::size_t segments_{16};
  float controlDepth_{18.0F};
};

} // namespace xudu

#endif // XUDU_TENUOUS_TETHER_HPP

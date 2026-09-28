/**
 * @file satelloid.hpp
 * @brief Flying Cell Satelloids and focus ring overlays for cross-domain
 * hypertext.
 *
 * Implements Ted Nelson's "Seeing Both Ends" invariant across disparate
 * data models: when a link or transclusion to a multidimensional Zigzag cell
 * is activated, a lightweight proxy quad glides into collinear reading
 * alignment beside the text line rather than teleporting the reader.
 */
#ifndef XUDU_SATELLOID_HPP
#define XUDU_SATELLOID_HPP

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/common.hpp>
#include <glm/ext/matrix_float4x4.hpp>
#include <glm/ext/vector_float2.hpp>
#include <glm/ext/vector_float3.hpp>

#include <gleditor/canvas.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/renderer.hpp>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/link_occurrences.hpp"
#include "common/xanadu/universal_link_endpoint.hpp"

namespace xudu {
using namespace ::xanadu;

class LinkContext;

struct SatelloidNeighbor {
  zigzag::CellRef cell{zigzag::noCell};
  std::string text;
  glm::vec2 place{};
  std::uint32_t depth{};
};

struct SatelloidOccurrence {
  std::uint64_t link{};
  std::uint32_t side{};
  std::uint32_t member{};
  std::uint32_t occurrence{};
  bool operator==(const SatelloidOccurrence &) const = default;
};

/// Selected occurrence is the front page; the others recede in endset order.
[[nodiscard]] constexpr std::size_t
satelloidLayer(const std::size_t index, const std::size_t selected,
               const std::size_t count) noexcept {
  return count == 0 ? 0 : (index + count - selected % count) % count;
}

/**
 * @struct CellSatelloid
 * @brief Dynamic proxy quad of a multidimensional Zigzag cell.
 */
struct CellSatelloid {
  zigzag::CellRef cellRef{zigzag::noCell};
  std::uint64_t linkId{};
  std::optional<SatelloidOccurrence> occurrence;
  std::vector<SatelloidNeighbor> neighborhood;
  std::uint64_t neighborhoodRevision{};

  glm::vec3 originPos{0.0F, 0.0F, -40.0F}; ///< Native lattice pos (Z ~ -40)
  glm::vec3 currentPos{0.0F, 0.0F, 0.0F};  ///< Dynamic position (Z ~ 0)
  glm::vec3 targetPos{0.0F, 0.0F, 0.0F};   ///< Collinear target position

  float width{24.0F};
  float height{14.0F};

  std::string text;
  std::string dimName{"d.sequence"};
  std::uint32_t accentColor{0x38BDF8FF}; ///< Primary dimension accent (Cyan)

  float alpha{0.0F};       ///< Visual card opacity [0.0, 1.0]
  float targetAlpha{1.0F}; ///< Target opacity

  float pulseRadius{0.0F}; ///< Focus ring expansion radius in px
  float pulseAlpha{0.0F};  ///< Focus ring opacity [0.0, 1.0]

  bool active{false};
  bool selected{false};
  glm::vec2 screenPos{};
  glm::vec2 screenTarget{};
  bool screenPlaced{false};
  float depth{};
  float targetDepth{};
  std::size_t stackLayer{};

  void triggerPulse() noexcept {
    pulseRadius = 6.0F;
    pulseAlpha  = 1.0F;
  }

  void updateDynamics(const float dt) noexcept {
    const float lerpFactor = std::clamp(dt * 15.0F, 0.0F, 1.0F);
    alpha                  = glm::mix(alpha, targetAlpha, lerpFactor);
    depth                  = glm::mix(depth, targetDepth, lerpFactor);
    if (screenPlaced) {
      screenPos = glm::mix(screenPos, screenTarget, lerpFactor);
      if (glm::distance(screenPos, screenTarget) < 0.5F) {
        screenPos = screenTarget;
      }
    }

    if (pulseAlpha > 0.01F) {
      pulseRadius += dt * 65.0F;
      pulseAlpha = std::max(0.0F, pulseAlpha - dt * 2.2F);
    }
  }
};

/**
 * @class SatelloidOverlay
 * @brief FrameContributor & PickObserver managing flying Cell Satelloids,
 *        dimensional focus rings, and provenance badges.
 */
class SatelloidOverlay : public gleditor::FrameContributor,
                         public gleditor::PickObserver {
public:
  static constexpr std::uint32_t kTagSatelloidBase = 15000U;

  using NavigationCallback =
      std::function<void(zigzag::CellRef cellRef, bool altHeld)>;
  using NeighborhoodResolver =
      std::function<std::vector<SatelloidNeighbor>(zigzag::CellRef)>;
  using NeighborhoodRevision = std::function<std::uint64_t()>;
  using AxisNameResolver     = std::function<std::string()>;
  using AnchorResolver =
      std::function<std::optional<glm::vec3>(zigzag::CellRef)>;
  using SiteFilter = std::function<bool(const xanadu::CellSite &)>;

  explicit SatelloidOverlay(RendererRef renderer,
                            std::string fontName = "Sans 9");
  ~SatelloidOverlay() override;

  SatelloidOverlay(const SatelloidOverlay &)            = delete;
  SatelloidOverlay &operator=(const SatelloidOverlay &) = delete;

  /// Register or update a cell satelloid.
  void setSatelloid(CellSatelloid satelloid);

  /// Remove a satelloid by cell reference.
  void removeSatelloid(zigzag::CellRef cellRef);

  /// Clear all registered satelloids.
  void clear();

  /// Retrieve all current satelloids.
  [[nodiscard]] const std::vector<CellSatelloid> &satelloids() const noexcept {
    return satelloids_;
  }
  [[nodiscard]] std::vector<CellSatelloid> &satelloids() noexcept {
    return satelloids_;
  }

  /// Lookup satelloid by cell reference.
  [[nodiscard]] gleditor::cpp26::optional<const CellSatelloid &>
  findSatelloid(zigzag::CellRef cellRef) const;
  [[nodiscard]] gleditor::cpp26::optional<CellSatelloid &>
  findSatelloid(zigzag::CellRef cellRef);

  /// Trigger an outward dimensional ring pulse animation.
  void triggerPulse(zigzag::CellRef cellRef);

  /// Set navigation callback invoked upon click or Alt+Click.
  void setNavigationCallback(NavigationCallback cb) {
    navigationCb_ = std::move(cb);
  }
  void setLinkContext(LinkContext *context) noexcept { linkContext_ = context; }
  void setNeighborhoodResolver(NeighborhoodResolver resolver) {
    neighborhoodResolver_ = std::move(resolver);
  }
  void setNeighborhoodRevision(NeighborhoodRevision revision) {
    neighborhoodRevision_ = std::move(revision);
  }
  void setAxisNameResolver(AxisNameResolver resolver) {
    axisNameResolver_ = std::move(resolver);
  }
  void setAnchorResolver(AnchorResolver resolver) {
    anchorResolver_ = std::move(resolver);
  }
  void setSiteFilter(SiteFilter filter) { siteFilter_ = std::move(filter); }

  // -- FrameContributor implementation ---------------------------------------
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &documentPipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool busy() const override;

  // -- PickObserver implementation -------------------------------------------
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;

private:
  RendererRef renderer_;
  std::string fontName_{"Sans 9"};
  std::unique_ptr<gleditor::Canvas> canvas_;
  std::vector<std::unique_ptr<gleditor::Canvas>> extraCanvases_;
  render::RenderDevice *device_{};
  render::PipelineDesc pipeline_;

  std::vector<CellSatelloid> satelloids_;
  NavigationCallback navigationCb_;
  LinkContext *linkContext_{};
  NeighborhoodResolver neighborhoodResolver_;
  NeighborhoodRevision neighborhoodRevision_;
  AxisNameResolver axisNameResolver_;
  AnchorResolver anchorResolver_;
  SiteFilter siteFilter_;
  std::vector<std::size_t> drawnIndices_;

  void synchronizeSelection();

  std::chrono::steady_clock::time_point lastFrameTime_{
      std::chrono::steady_clock::now()};
  bool visible_{true};
};

} // namespace xudu

#endif // XUDU_SATELLOID_HPP

/**
 * @file stretch_vanishing_view.hpp
 * @brief Stretch vanishing: every cell at the size its content needs, packed
 *        almost touching, fading towards the pane's edge
 *        (design/view-system.md §9.1; plan E9, G5, D1, D2).
 *
 * From the cell under the cursor the view walks the dimensions the placed
 * axes show, breadth first, and places what it reaches by anchored slide:
 * the focus's immediate neighbours exactly on its axes, everything further
 * out slid against the cell that reached it, until the pane is tiled. Only
 * the first two spatial binding points lie in the plane, as x and y; the
 * third is depth, in planes stretch.layerDepth apart; any further spatial
 * point is on the compass and moves the cursor but has no direction here
 * (G5). A cell the pane would cut is never drawn with its content: it is a
 * ghost (D1).
 *
 * The view shows real cells only, so prepare() mints nothing; it caches the
 * axes' directions and sizes the workspace layout() reuses, so a layout that
 * has reached its size allocates nothing (V-R2).
 *
 * Edge heat, the view's second sub-view, is deferred beyond the spine (plan
 * §4.1); the breadcrumb strip is chrome and the presenter's, as is the
 * clip margin's hysteresis. Their settings are here, with the view's others,
 * so they are seeded where the view's tunables are.
 */
#ifndef COMMON_XANADU_VIEW_SLICE_STRETCH_VANISHING_VIEW_HPP
#define COMMON_XANADU_VIEW_SLICE_STRETCH_VANISHING_VIEW_HPP

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <glm/vec3.hpp>

#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/view/slice_view.hpp"
#include "common/xanadu/view/view.hpp"
#include "common/xanadu/view/view_error.hpp"
#include "common/xanadu/view/view_ids.hpp"
#include "common/xanadu/view/view_manifold.hpp"
#include "common/xanadu/view/view_records.hpp"

namespace xanadu::view {

namespace stretch_settings {
inline constexpr std::string_view kGap             = "stretch.gap";
inline constexpr std::string_view kMinContact      = "stretch.minContact";
inline constexpr std::string_view kOverfill        = "stretch.overfill";
inline constexpr std::string_view kLayerDepth      = "stretch.layerDepth";
inline constexpr std::string_view kFadeBand        = "stretch.fadeBand";
inline constexpr std::string_view kFadeFloor       = "stretch.fadeFloor";
inline constexpr std::string_view kGhostShare      = "stretch.ghostShare";
inline constexpr std::string_view kAbbreviateBelow = "stretch.abbreviateBelow";
inline constexpr std::string_view kCoarseBelow     = "stretch.coarseBelow";
inline constexpr std::string_view kClipMargin      = "stretch.clipMargin";
inline constexpr std::string_view kBreadcrumbs     = "stretch.breadcrumbs";
} // namespace stretch_settings

/**
 * @brief The stretch.* settings (§12.3; plan D1, D2). Lengths are logical
 *        pixels; shares and opacities are fractions.
 *
 * The defaults here are the ones stretchSettingSpecs() seeds, so the two
 * cannot drift.
 */
struct StretchConfig {
  /// Space between neighbouring boxes. Small and constant by design.
  float gap{4.0F};
  /// Least overlap a slid box keeps with the cell that reached it.
  float minContact{12.0F};
  /// Multiple of the pane, about its centre, whose cells the walk expands.
  float overfill{1.3F};
  /// Distance between depth planes.
  float layerDepth{120.0F};
  /// Outer fraction of the pane over which opacity falls.
  float fadeBand{0.35F};
  /// Opacity at the pane's edge.
  float fadeFloor{0.15F};
  /// A ghost's opacity as a share of fadeFloor (D1).
  float ghostShare{0.6F};
  /// Below this opacity a cell's content gives way to its shorter form (D2).
  float abbreviateBelow{0.75F};
  /// Below this opacity a cell's text gives way to bars (D2). At 0.5 the
  /// default theme's text over its surface is about 4.6:1 (§15); the test
  /// that says so reads the theme.
  float coarseBelow{0.5F};
  /// Pixels a hidden cell must clear the edge by before it is shown again;
  /// the presenter's, which tests animated boxes (§9.1.5).
  float clipMargin{6.0F};
  /// Cells in the breadcrumb strip; the chrome's (§9.1.6).
  std::uint32_t breadcrumbs{6};

  bool operator==(const StretchConfig &) const = default;

  /// What a ghost is drawn at: ghostShare of fadeFloor.
  [[nodiscard]] float ghostOpacity() const noexcept;

  /**
   * @brief The stretch.* values @p settings holds, each falling back to the
   *        default when absent or outside its range.
   *
   * A range is what keeps the placement meaningful: a negative gap or a
   * band of nothing would not fail loudly, it would draw nonsense.
   */
  [[nodiscard]] static StretchConfig
  fromSettings(const SystemStoreModel &settings);
};

/// Every stretch.* setting, with StretchConfig's defaults.
[[nodiscard]] std::vector<SettingSpec> stretchSettingSpecs();

/**
 * @brief §9.1.5's fade: @p margin is a box's least distance from the pane's
 *        edge, as a fraction of the pane, from 0 at the edge to 0.5 at the
 *        centre.
 *
 * With e = min(1, 2 margin), opacity is floor + (1 - floor) smoothstep(0,
 * band, e): constant over the middle, falling through the outer band to the
 * floor. A margin outside the pane is the floor.
 */
[[nodiscard]] float stretchOpacity(float margin,
                                   const StretchConfig &config) noexcept;

/// Everything stretch vanishing reads: its own settings, and the existing
/// zigzag.* cell padding and content width it uses rather than copies
/// (§12.3).
struct StretchVanishingConfig {
  StretchConfig stretch;
  ZigzagPresentationConfig cells;

  bool operator==(const StretchVanishingConfig &) const = default;
};

class StretchVanishingView final : public SliceView {
public:
  static constexpr std::string_view kKind = "slice.stretch-vanishing";

  explicit StretchVanishingView(StretchVanishingConfig config = {});
  StretchVanishingView(const StretchVanishingView &)            = delete;
  StretchVanishingView &operator=(const StretchVanishingView &) = delete;
  StretchVanishingView(StretchVanishingView &&)                 = delete;
  StretchVanishingView &operator=(StretchVanishingView &&)      = delete;
  ~StretchVanishingView() override;

  /// Settings changed: the next layout reads these.
  StretchVanishingView *configure(const StretchVanishingConfig &config);
  [[nodiscard]] const StretchVanishingConfig &config() const noexcept {
    return config_;
  }

  [[nodiscard]] std::string_view kind() const noexcept override {
    return kKind;
  }

  /// +x for the first spatial binding point, +y for the second, away from
  /// the viewer (-z) for the third, nothing for any other: as of the last
  /// prepare() (G5).
  [[nodiscard]] std::optional<glm::vec3>
  axisDirection(ViewAxisId axis) const noexcept override;

  /// Mints nothing. Caches the axes' directions and sizes the workspace to
  /// the slice, so layout() does not allocate.
  std::expected<SliceView *, ViewError> prepare(ViewManifold &space,
                                                const SliceCursor &cursor,
                                                const PaneFrame &frame,
                                                Measure measure) override;

  /**
   * @brief §9.1.3 to §9.1.6 into @p out.
   *
   * Items in placement order, the focus first: each cell drawn with its
   * content, or its ghost, or nothing when it lies wholly outside the pane;
   * then the ticks on the focus's immediate neighbours; then the edges
   * between placed cells, each once. The same input gives the same records.
   */
  void layout(const SliceLayoutInput &in,
              LayoutSink &out) const noexcept override;

private:
  struct Workspace;

  StretchVanishingConfig config_;
  std::vector<std::optional<glm::vec3>> directions_;
  /// Reused by every layout(); its contents mean nothing between calls.
  std::unique_ptr<Workspace> workspace_;
};

/// The view as a registry installs it: kind, sub-views, settings. It brings
/// no chords: movement keeps the existing step actions (§12.1), and the
/// actions every view shares are the host's (U7a).
[[nodiscard]] ViewDescriptor stretchVanishingDescriptor();

} // namespace xanadu::view

#endif // COMMON_XANADU_VIEW_SLICE_STRETCH_VANISHING_VIEW_HPP

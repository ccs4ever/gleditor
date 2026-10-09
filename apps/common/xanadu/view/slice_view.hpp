/**
 * @file slice_view.hpp
 * @brief A slice view's cursor, its inputs, and how the cursor moves
 *        (design/view-system.md §8.5; invariant I6).
 *
 * The cursor names real cells and numbers only: an origin, an axis, a signed
 * count of packs and the lanes entered. It names no view cell, so a toss
 * cannot invalidate it, and re-deriving after a toss finds the same real cell
 * under it. Every cursor motion is a pure move(): a view never mutates to
 * move, and a move that would change the origin says so, for the host to
 * toss and re-prepare.
 */
#ifndef COMMON_XANADU_VIEW_SLICE_VIEW_HPP
#define COMMON_XANADU_VIEW_SLICE_VIEW_HPP

#include <cstdint>
#include <expected>
#include <optional>
#include <vector>

#include <glm/vec3.hpp>

#include "common/xanadu/view/dimension_ranking.hpp"
#include "common/xanadu/view/view.hpp"
#include "common/xanadu/view/view_binding.hpp"
#include "common/xanadu/view/view_error.hpp"
#include "common/xanadu/view/view_ids.hpp"
#include "common/xanadu/view/view_manifold.hpp"
#include "common/xanadu/view/view_records.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace xanadu::view {

/// I6: real cells and numbers only. step == 0 is the origin itself.
struct SliceCursor {
  zigzag::CellRef origin;         ///< always a real cell
  std::optional<ViewAxisId> axis; ///< the group axis stepped along, if any
  std::int32_t step{};            ///< signed packs from the origin on it
  /// Into nested packs, outermost first; empty: the pack as a whole.
  std::vector<std::uint32_t> lanes;
  /// All-dim walk: the selected spoke, a dimension and a direction (§9.2.6).
  std::optional<zigzag::DirectedDim> spoke;

  bool operator==(const SliceCursor &) const = default;
};

/**
 * @brief The real cell under the cursor.
 *
 * The origin at step 0. Otherwise the pack @p cursor.step along the axis's
 * d.axis-step rank from the occurrence of the origin on it (§9.3.3), then one
 * lane per entry of @p cursor.lanes, and that cell's real cell: an
 * occurrence's target, or a pack's first present constituent's. Nothing when
 * the packs there are not derived in this epoch, or a lane is an empty place.
 */
[[nodiscard]] std::optional<zigzag::CellRef>
cellAt(const ViewManifold &space, const SliceCursor &cursor) noexcept;

/// "As if this axis showed that": for a drag in flight.
struct BindingPreview {
  ViewAxisId axis{};
  BindTarget target;
};

struct SliceLayoutInput {
  const ViewManifold &space;
  const SliceCursor &cursor;
  PaneFrame frame;
  Measure measure;
  std::optional<BindingPreview> preview;
  std::uint32_t subview{};        ///< index into the descriptor's subviews
  std::optional<SubjectId> hover; ///< what the pointer is over, if anything
};

enum class MoveKind : std::uint8_t {
  AlongAxis,  ///< one step on `axis` in `direction`
  AlongSpoke, ///< one step to the selected spoke's neighbour
  NextSpoke,
  PreviousSpoke,
  EnterPack,
  LeavePack,
  NextLane,
  PreviousLane,
  Retrieve, ///< the cell under the cursor becomes the origin
};

struct MoveRequest {
  MoveKind kind{MoveKind::AlongAxis};
  std::optional<ViewAxisId> axis; ///< for AlongAxis
  zigzag::DimVector direction{zigzag::DimVector::POS};
};

/// "Nothing further that way" is not an error: it is moved == false.
struct MoveOutcome {
  SliceCursor cursor;
  bool moved{};
  bool originChanged{}; ///< the host tosses and re-prepares
  /// The step through the slice the move took, when it was one: what the
  /// walk recorder is fed, and nothing else is (plan G6).
  std::optional<SliceStep> step;
};

class SliceView : public View {
public:
  /// The local direction posward on an axis points, or nothing if this view
  /// does not place that axis (§7.3).
  [[nodiscard]] virtual std::optional<glm::vec3>
  axisDirection(ViewAxisId axis) const noexcept = 0;

  /// The only phase that may mint, extend the ring order or fill caches.
  /// Runs when the cursor, the epoch, the store or the frame changed.
  virtual std::expected<SliceView *, ViewError>
  prepare(ViewManifold &space, const SliceCursor &cursor,
          const PaneFrame &frame, Measure measure);

  /// Pure (V-R2).
  virtual void layout(const SliceLayoutInput &in,
                      LayoutSink &out) const noexcept = 0;

  /**
   * @brief Pure: where the cursor goes.
   *
   * The default steps from cellAt(cursor) along the real dimension an axis
   * shows (AlongAxis) or the selected spoke names (AlongSpoke), making the
   * neighbour the origin; for an axis showing a group that is the group's
   * first leaf dimension, as a group resolves (§7.2), until a pack view
   * overrides it with steps along packs. Retrieve makes the cell under the
   * cursor the origin. Every other kind is a view's own and does not move.
   */
  [[nodiscard]] virtual MoveOutcome move(const ViewManifold &space,
                                         const SliceCursor &cursor,
                                         MoveRequest request) const noexcept;
};

} // namespace xanadu::view

#endif // COMMON_XANADU_VIEW_SLICE_VIEW_HPP

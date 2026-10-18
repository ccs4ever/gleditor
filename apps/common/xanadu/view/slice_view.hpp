/**
 * @file slice_view.hpp
 * @brief Slice view interfaces and cursor navigation across multidimensional
 * ZigZag space.
 *
 * Implements pure spatial layout abstractions (SliceView, SliceCursor,
 * SliceLayoutInput) separating the view model from physical rendering.
 */
#ifndef XANADU_VIEW_SLICE_VIEW_HPP
#define XANADU_VIEW_SLICE_VIEW_HPP

#include <cstdint>
#include <optional>
#include <vector>

#include "common/xanadu/view/view.hpp"
#include "common/xanadu/view/view_manifold.hpp"
#include "common/xanadu/view/view_records.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace xanadu::view {

struct SliceCursor {
  std::optional<zigzag::CellRef> origin{std::nullopt};
  std::optional<ViewAxisId> axis{std::nullopt};
  std::int32_t step{0};
  std::vector<std::uint32_t> lanes{};
  std::optional<zigzag::DirectedDim> spoke{std::nullopt};

  bool operator==(const SliceCursor &) const = default;
};

[[nodiscard]] inline std::optional<zigzag::CellRef>
cellAt(const ViewManifold &space, const SliceCursor &cursor) noexcept {
  if (!cursor.origin.has_value()) {
    return std::nullopt;
  }
  return space.resolveReal(*cursor.origin);
}

struct SliceLayoutInput {
  const ViewManifold &space;
  const SliceCursor &cursor;
  PaneFrame frame{};
  Measure measure;
  std::uint32_t subview{0};
  std::optional<SubjectId> hover{std::nullopt};
};

enum class MoveKind : std::uint8_t {
  AlongAxis,
  AlongSpoke,
  NextSpoke,
  PreviousSpoke,
  EnterPack,
  LeavePack,
  NextLane,
  PreviousLane,
  Retrieve
};

struct MoveRequest {
  MoveKind kind{MoveKind::AlongAxis};
  std::optional<ViewAxisId> axis{std::nullopt};
  zigzag::DimVector direction{zigzag::DimVector::POS};
};

struct MoveOutcome {
  SliceCursor cursor{};
  bool moved{false};
  bool originChanged{false};
};

class SliceView : public View {
public:
  [[nodiscard]] virtual std::optional<glm::vec3>
  axisDirection(ViewAxisId axis) const noexcept = 0;

  virtual std::expected<SliceView *, ViewError>
  prepare(ViewManifold &space, const SliceCursor &cursor,
          const PaneFrame &frame, Measure measure) {
    std::ignore = space;
    std::ignore = cursor;
    std::ignore = frame;
    std::ignore = measure;
    return this;
  }

  virtual void layout(const SliceLayoutInput &in,
                      LayoutSink &out) const noexcept = 0;

  [[nodiscard]] virtual MoveOutcome move(const ViewManifold &space,
                                         const SliceCursor &cursor,
                                         MoveRequest request) const noexcept {
    std::ignore = space;
    MoveOutcome outcome{
        .cursor = cursor, .moved = false, .originChanged = false};
    if (request.kind == MoveKind::AlongAxis && request.axis.has_value()) {
      // Step along axis
      outcome.cursor.step +=
          (request.direction == zigzag::DimVector::POS ? 1 : -1);
      outcome.moved = true;
    }
    return outcome;
  }
};

} // namespace xanadu::view

#endif // XANADU_VIEW_SLICE_VIEW_HPP

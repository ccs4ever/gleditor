/**
 * @file slice_view.cpp
 * @brief The cursor's real cell and the default move (design/view-system.md
 *        §8.5, §9.3.3, §9.3.5).
 */
#include "common/xanadu/view/slice_view.hpp"

#include <cstdlib>

namespace xanadu::view {

namespace {

/// @p cursor's place made the origin: what every move that leaves the packs
/// it was in produces. The spoke is the reader's selection, kept across.
SliceCursor originAt(const zigzag::CellRef cell, const SliceCursor &cursor) {
  return SliceCursor{.origin = cell,
                     .axis   = std::nullopt,
                     .step   = 0,
                     .lanes  = {},
                     .spoke  = cursor.spoke};
}

/// One step from the cell under the cursor along a real dimension. A step
/// from inside a pack acts on the cell under the cursor, as a retrieve
/// followed by the step (§9.3.5).
MoveOutcome stepAlong(const ViewManifold &space, const SliceCursor &cursor,
                      const zigzag::DimRef dimension,
                      const zigzag::DimVector direction) {
  MoveOutcome out{.cursor = cursor};
  const auto here = cellAt(space, cursor);
  if (!here.has_value()) {
    return out;
  }
  const auto there = space.base().linked(*here, dimension, direction);
  if (!there.has_value()) {
    return out;
  }
  out.cursor        = originAt(*there, cursor);
  out.moved         = true;
  out.originChanged = *there != cursor.origin;
  out.step          = SliceStep::of(*here, *there, dimension);
  return out;
}

} // namespace

std::optional<zigzag::CellRef> cellAt(const ViewManifold &space,
                                      const SliceCursor &cursor) noexcept {
  if (0 == cursor.step || !cursor.axis.has_value()) {
    return cursor.origin;
  }
  const auto axisStep = space.findAxisStepDim(*cursor.axis);
  if (!axisStep.has_value()) {
    return std::nullopt;
  }
  auto at = space.findOccurrence(*axisStep, cursor.origin);
  const auto direction =
      cursor.step > 0 ? zigzag::DimVector::POS : zigzag::DimVector::NEG;
  for (auto left = std::abs(static_cast<std::int64_t>(cursor.step));
       left > 0 && at.has_value(); --left) {
    at = space.linked(*at, *axisStep, direction);
  }
  if (!at.has_value()) {
    return std::nullopt;
  }
  if (!cursor.lanes.empty()) {
    const auto pack    = space.findPackDim();
    const auto packing = space.findPackingDim();
    if (!pack.has_value() || !packing.has_value()) {
      return std::nullopt;
    }
    for (const auto lane : cursor.lanes) {
      at = space.linked(*at, *pack, zigzag::DimVector::POS);
      for (auto left = lane; left > 0 && at.has_value(); --left) {
        at = space.linked(*at, *packing, zigzag::DimVector::POS);
      }
      if (!at.has_value()) {
        return std::nullopt;
      }
    }
  }
  const auto real = space.resolveReal(*at);
  return real.has_value() ? std::optional{*real} : std::nullopt;
}

std::expected<SliceView *, ViewError>
SliceView::prepare(ViewManifold & /*space*/, const SliceCursor & /*cursor*/,
                   const PaneFrame & /*frame*/, Measure /*measure*/) {
  return this;
}

MoveOutcome SliceView::move(const ViewManifold &space,
                            const SliceCursor &cursor,
                            const MoveRequest request) const noexcept {
  switch (request.kind) {
  case MoveKind::AlongAxis: {
    if (!request.axis.has_value()) {
      break;
    }
    const auto shows = space.axes().shown(*request.axis);
    if (!shows.has_value()) {
      break;
    }
    std::optional<zigzag::DimRef> first;
    space.axes().forEachLeaf(*shows, [&](const zigzag::DimRef dim) {
      if (!first.has_value()) {
        first = dim;
      }
    });
    if (!first.has_value()) {
      break;
    }
    return stepAlong(space, cursor, *first, request.direction);
  }
  case MoveKind::AlongSpoke:
    if (!cursor.spoke.has_value()) {
      break;
    }
    return stepAlong(space, cursor, cursor.spoke->dim, cursor.spoke->dir);
  case MoveKind::Retrieve: {
    const auto here = cellAt(space, cursor);
    if (!here.has_value()) {
      break;
    }
    auto next = originAt(*here, cursor);
    MoveOutcome out{.cursor        = next,
                    .moved         = next != cursor,
                    .originChanged = *here != cursor.origin,
                    .step          = std::nullopt};
    return out;
  }
  case MoveKind::NextSpoke:
  case MoveKind::PreviousSpoke:
  case MoveKind::EnterPack:
  case MoveKind::LeavePack:
  case MoveKind::NextLane:
  case MoveKind::PreviousLane:
    break;
  }
  return MoveOutcome{.cursor = cursor};
}

} // namespace xanadu::view

#include "common/xanadu/view/transductive_view.hpp"
#include <algorithm>

namespace xanadu::view {

CycleBoundedRankIterator::CycleBoundedRankIterator(
    const ViewManifold &viewManifold, std::optional<zigzag::CellRef> start,
    std::optional<zigzag::DimRef> dim, std::size_t maxSteps)
    : viewManifold_(viewManifold), current_(start), dim_(dim),
      maxSteps_(maxSteps) {
  if (current_.has_value() && *current_ != zigzag::noCell) {
    visited_.insert(*current_);
  } else {
    current_ = std::nullopt;
  }
}

std::optional<zigzag::CellRef> CycleBoundedRankIterator::next() noexcept {
  if (!current_.has_value() || cycleDetected_ || !dim_.has_value()) {
    return std::nullopt;
  }
  const auto ret = current_;
  ++stepsTaken_;
  if (stepsTaken_ >= maxSteps_) {
    cycleDetected_ = true;
    current_       = std::nullopt;
    return ret;
  }

  const auto nextCell =
      viewManifold_.baseLinked(*current_, *dim_, zigzag::DimVector::POS);
  if (!nextCell.has_value() || *nextCell == zigzag::noCell) {
    current_ = std::nullopt;
  } else {
    if (visited_.contains(*nextCell)) {
      cycleDetected_ = true;
      current_       = std::nullopt;
    } else {
      current_ = *nextCell;
      visited_.insert(*nextCell);
    }
  }
  return ret;
}

std::optional<ViewCellRef> TransductiveView::materializeFilter(
    std::optional<zigzag::CellRef> head,
    std::optional<zigzag::DimRef> sourceDim,
    gleditor::cpp26::function_ref<bool(zigzag::CellRef)> predicate,
    std::string_view dimName) {
  if (!head.has_value() || !sourceDim.has_value() || *head == zigzag::noCell ||
      *sourceDim == zigzag::noCell) {
    return std::nullopt;
  }

  // Mint derived dimension cell in derived arena
  const auto derivedDim = viewManifold_.mint(Layer::Derived, dimName);

  std::vector<zigzag::CellRef> matchingReal;
  CycleBoundedRankIterator it(viewManifold_, head, sourceDim);
  while (it.hasNext()) {
    const auto c = it.next();
    if (c.has_value() && predicate(*c)) {
      matchingReal.push_back(*c);
    }
  }

  if (matchingReal.empty()) {
    return std::nullopt;
  }

  std::optional<ViewCellRef> firstOcc;
  std::optional<ViewCellRef> prevOcc;

  for (const auto realCell : matchingReal) {
    const auto occ = viewManifold_.mintOccurrence(Layer::Derived, realCell);
    if (!firstOcc.has_value()) {
      firstOcc = occ;
    }
    if (prevOcc.has_value()) {
      // Link prevOcc -> occ along derivedDim
      const auto res = viewManifold_.link(Layer::Derived, *prevOcc, derivedDim,
                                          zigzag::DimVector::POS, occ);
      if (!res.has_value()) {
        return std::nullopt;
      }
    }
    prevOcc = occ;
  }

  return firstOcc;
}

std::optional<ViewCellRef> TransductiveView::materializeSorted(
    std::optional<zigzag::CellRef> head,
    std::optional<zigzag::DimRef> sourceDim,
    gleditor::cpp26::function_ref<bool(zigzag::CellRef, zigzag::CellRef)>
        comparator,
    std::string_view dimName) {
  if (!head.has_value() || !sourceDim.has_value() || *head == zigzag::noCell ||
      *sourceDim == zigzag::noCell) {
    return std::nullopt;
  }

  const auto derivedDim = viewManifold_.mint(Layer::Derived, dimName);

  std::vector<zigzag::CellRef> cells;
  CycleBoundedRankIterator it(viewManifold_, head, sourceDim);
  while (it.hasNext()) {
    const auto c = it.next();
    if (c.has_value()) {
      cells.push_back(*c);
    }
  }

  if (cells.empty()) {
    return std::nullopt;
  }

  std::stable_sort(cells.begin(), cells.end(), comparator);

  std::optional<ViewCellRef> firstOcc;
  std::optional<ViewCellRef> prevOcc;

  for (const auto realCell : cells) {
    const auto occ = viewManifold_.mintOccurrence(Layer::Derived, realCell);
    if (!firstOcc.has_value()) {
      firstOcc = occ;
    }
    if (prevOcc.has_value()) {
      const auto res = viewManifold_.link(Layer::Derived, *prevOcc, derivedDim,
                                          zigzag::DimVector::POS, occ);
      if (!res.has_value()) {
        return std::nullopt;
      }
    }
    prevOcc = occ;
  }

  return firstOcc;
}

std::optional<ViewCellRef> TransductiveView::materializeUnion(
    std::span<const std::pair<zigzag::CellRef, zigzag::DimRef>> ranks,
    std::string_view dimName, std::size_t maxStepsPerRank) {
  if (ranks.empty()) {
    return std::nullopt;
  }

  const auto derivedDim = viewManifold_.mint(Layer::Derived, dimName);

  std::vector<zigzag::CellRef> unionCells;
  std::unordered_set<zigzag::CellRef> seen;

  for (const auto &[head, dim] : ranks) {
    if (head == zigzag::noCell || dim == zigzag::noCell) {
      continue;
    }
    CycleBoundedRankIterator it(viewManifold_, head, dim, maxStepsPerRank);
    while (it.hasNext()) {
      const auto c = it.next();
      if (c.has_value() && seen.insert(*c).second) {
        unionCells.push_back(*c);
      }
    }
  }

  if (unionCells.empty()) {
    return std::nullopt;
  }

  std::optional<ViewCellRef> firstOcc;
  std::optional<ViewCellRef> prevOcc;

  for (const auto realCell : unionCells) {
    const auto occ = viewManifold_.mintOccurrence(Layer::Derived, realCell);
    if (!firstOcc.has_value()) {
      firstOcc = occ;
    }
    if (prevOcc.has_value()) {
      const auto res = viewManifold_.link(Layer::Derived, *prevOcc, derivedDim,
                                          zigzag::DimVector::POS, occ);
      if (!res.has_value()) {
        return std::nullopt;
      }
    }
    prevOcc = occ;
  }

  return firstOcc;
}

} // namespace xanadu::view

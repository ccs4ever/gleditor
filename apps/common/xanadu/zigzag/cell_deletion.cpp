/**
 * @file cell_deletion.cpp
 * @brief The shared cell-deletion plan and cursor move-off rule (P7).
 */
#include "common/xanadu/zigzag/cell_deletion.hpp"

#include <algorithm>
#include <array>
#include <ranges>

namespace zigzag {

namespace {

constexpr std::array<DimVector, 2> moveOffOrder{DimVector::POS, DimVector::NEG};

} // namespace

Neighbourhood Neighbourhood::of(const CellRef cell,
                                const std::span<const DimLink> run) {
  Neighbourhood out{.cell = cell, .ranks = {}};
  for (const auto &link : run) {
    const RankNeighbours ward{.dim     = link.dim,
                              .negward = link.neighbor(DimVector::NEG),
                              .posward = link.neighbor(DimVector::POS)};
    if (ward.negward || ward.posward) out.ranks.push_back(ward);
  }
  return out;
}

std::optional<RankNeighbours>
Neighbourhood::along(const DimRef dim) const noexcept {
  const auto found = std::ranges::find(ranks, dim, &RankNeighbours::dim);
  return found == ranks.end() ? std::nullopt : std::optional{*found};
}

bool orphansViews(const Neighbourhood &broken,
                  const std::optional<CloneGuard> &guard) noexcept {
  if (!guard || guard->promoteFirstView) return false;
  const auto ward = broken.along(guard->dim);
  return ward && ward->posward && !ward->negward &&
         *ward->posward != broken.cell;
}

void planRankDeletion(
    const CellRef cell, const RankNeighbours &ward, const DeletionMode mode,
    const gleditor::cpp26::function_ref<void(const LinkEdit &)> emit) {
  const bool bothSides = ward.negward && ward.posward;
  if (mode == DeletionMode::Repair && bothSides &&
      *ward.negward != *ward.posward && *ward.negward != cell &&
      *ward.posward != cell) {
    emit({*ward.negward, ward.dim, DimVector::POS, ward.posward});
    return;
  }
  if (ward.posward) emit({cell, ward.dim, DimVector::POS, std::nullopt});
  // A one-cell loop is one edge: clearing it once clears both its sides.
  const bool loop = ward.posward == cell && ward.negward == cell;
  if (ward.negward && !loop) {
    emit({cell, ward.dim, DimVector::NEG, std::nullopt});
  }
}

void planDeletion(
    const CellRef cell, const Neighbourhood &ranks, const DeletionMode mode,
    const gleditor::cpp26::function_ref<void(const LinkEdit &)> emit) {
  for (const auto &ward : ranks.ranks) planRankDeletion(cell, ward, mode, emit);
}

std::optional<CellRef> cursorMoveOff(
    const Neighbourhood &broken, const std::span<const DimRef> boundDimensions,
    const std::optional<CellRef> home,
    const gleditor::cpp26::function_ref<bool(DimRef, CellRef)> eligible) {
  const auto landing =
      [&](const RankNeighbours &ward) -> std::optional<CellRef> {
    for (const auto dir : moveOffOrder) {
      if (const auto next = ward.toward(dir);
          next && *next != broken.cell && eligible(ward.dim, *next))
        return next;
    }
    return std::nullopt;
  };
  for (const auto dim : boundDimensions) {
    if (const auto ward = broken.along(dim)) {
      if (const auto next = landing(*ward)) return next;
    }
  }
  for (const auto &ward : broken.ranks) {
    if (std::ranges::contains(boundDimensions, ward.dim)) continue;
    if (const auto next = landing(ward)) return next;
  }
  return home;
}

} // namespace zigzag

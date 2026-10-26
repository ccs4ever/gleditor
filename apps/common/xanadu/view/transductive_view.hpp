/**
 * @file transductive_view.hpp
 * @brief Materialized derived transductive views (filter, sort, union) over
 * ZigZag ranks.
 *
 * Implements pure ephemeral rank derivations into ViewManifold derived arenas:
 * - CycleBoundedRankIterator: Cycle-safe rank traversal with bound
 * verification.
 * - TransductiveView: Materializes filtered, sorted, and union ranks with zero
 * base shadowing.
 */
#ifndef XANADU_VIEW_TRANSDUCTIVE_VIEW_HPP
#define XANADU_VIEW_TRANSDUCTIVE_VIEW_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/view/view_manifold.hpp"

namespace xanadu::view {

/**
 * @class CycleBoundedRankIterator
 * @brief Cycle-safe rank iterator preventing infinite loops in cyclic or
 * malformed ranks.
 */
class CycleBoundedRankIterator {
public:
  CycleBoundedRankIterator(const ViewManifold &viewManifold,
                           std::optional<zigzag::CellRef> start,
                           std::optional<zigzag::DimRef> dim,
                           std::size_t maxSteps = 100000);

  [[nodiscard]] bool hasNext() const noexcept {
    return current_.has_value() && !cycleDetected_;
  }
  [[nodiscard]] std::optional<zigzag::CellRef> next() noexcept;
  [[nodiscard]] bool cycleDetected() const noexcept { return cycleDetected_; }
  [[nodiscard]] std::size_t stepsTaken() const noexcept { return stepsTaken_; }

private:
  const ViewManifold &viewManifold_;
  std::optional<zigzag::CellRef> current_{std::nullopt};
  std::optional<zigzag::DimRef> dim_{std::nullopt};
  std::size_t maxSteps_{100000};
  std::size_t stepsTaken_{0};
  bool cycleDetected_{false};
  std::unordered_set<zigzag::CellRef> visited_;
};

/**
 * @class TransductiveView
 * @brief Materialized derived ephemeral dimensions (d.filtered, d.sorted,
 * d.union) in ViewManifold's derived arena during prepare().
 */
class TransductiveView {
public:
  explicit TransductiveView(ViewManifold &viewManifold)
      : viewManifold_(viewManifold) {}

  /// Materialize a filtered rank from @p head along @p sourceDim into @p
  /// dimName. Walks the base rank, mints derived occurrences for matches, and
  /// links them into CSR runs. @p predicate is borrowed and called only before
  /// this function returns; it is not retained.
  [[nodiscard]] std::optional<ViewCellRef> materializeFilter(
      std::optional<zigzag::CellRef> head,
      std::optional<zigzag::DimRef> sourceDim,
      gleditor::cpp26::function_ref<bool(zigzag::CellRef)> predicate,
      std::string_view dimName = "d.filtered");

  /// Materialize a sorted rank from @p head along @p sourceDim using @p
  /// comparator. @p comparator is borrowed and used only before this function
  /// returns; it is not retained.
  [[nodiscard]] std::optional<ViewCellRef> materializeSorted(
      std::optional<zigzag::CellRef> head,
      std::optional<zigzag::DimRef> sourceDim,
      gleditor::cpp26::function_ref<bool(zigzag::CellRef, zigzag::CellRef)>
          comparator,
      std::string_view dimName = "d.sorted");

  /// Materialize a union rank across multiple rank heads into @p dimName,
  /// cycle-safely.
  [[nodiscard]] std::optional<ViewCellRef> materializeUnion(
      std::span<const std::pair<zigzag::CellRef, zigzag::DimRef>> ranks,
      std::string_view dimName    = "d.union",
      std::size_t maxStepsPerRank = 100000);

private:
  ViewManifold &viewManifold_;
};

} // namespace xanadu::view

#endif // XANADU_VIEW_TRANSDUCTIVE_VIEW_HPP

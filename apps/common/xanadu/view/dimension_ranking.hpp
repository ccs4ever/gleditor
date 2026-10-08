/**
 * @file dimension_ranking.hpp
 * @brief Movement through a slice condensed into walk summaries, kept in the
 *        reader's activity store, and the "most used" order computed from
 *        them.
 *
 * design/view-system.md §7.8. A reader crosses cells many times a second, so
 * steps are never recorded one by one: a WalkRecorder holds the run in
 * progress in memory and answers one WalkSummary when the run settles, and
 * only that summary is filed (V-R53). The summaries go to the private
 * system://activity store and nowhere else: moving through a slice is not a
 * change to it and appends nothing to it (store-slice-convergence.md R8).
 *
 * Only movement through the slice feeds the recorder (implementation plan
 * G6): a step from one real cell to another along a real dimension. A
 * SliceStep cannot be made from a view-arena ref, so entering or leaving a
 * pack, a lane move or any other view-only motion cannot be recorded by
 * mistake; the host passes only its AlongAxis and AlongSpoke steps.
 *
 * "Most likely", the first-order Markov order over the same summaries, is
 * deferred beyond the first release (implementation plan §4.1). The
 * summaries already count which dimension followed which, so it will be a
 * new reading of the records already kept, not a new kind of record.
 */
#ifndef COMMON_XANADU_VIEW_DIMENSION_RANKING_HPP
#define COMMON_XANADU_VIEW_DIMENSION_RANKING_HPP

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/document_id.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace xanadu {
class Store;
} // namespace xanadu

namespace xanadu::view {

struct DimensionSteps {
  zigzag::DimRef dimension{};
  std::uint32_t steps{};

  bool operator==(const DimensionSteps &) const = default;
};

/// How many times a step along @p to directly followed one along @p from.
/// @p from may equal @p to: going on along one dimension is the commonest
/// thing a reader does, and the Markov order needs to know how common.
struct DimensionChange {
  zigzag::DimRef from{}, to{};
  std::uint32_t times{};

  bool operator==(const DimensionChange &) const = default;
};

/// One settled run of movement, condensed. Owns its counts, because a
/// summary outlives the run that made it and the recorder starts another.
struct WalkSummary {
  /// Position among the reader's runs, oldest first, from 1. The activity
  /// store assigns it; a summary fresh from a recorder has 0.
  std::uint64_t ordinal{};
  zigzag::CellRef began{}, ended{};
  /// Ascending by dimension; no zero counts.
  std::vector<DimensionSteps> steps;
  /// Ascending by (from, to); no zero counts.
  std::vector<DimensionChange> changes;

  bool operator==(const WalkSummary &) const = default;
};

/// One step through the slice: between real cells, along a real dimension.
class SliceStep {
public:
  /// Nothing unless all three refs are real cells: not noCell, and none
  /// carrying ephemeralBit.
  [[nodiscard]] static std::optional<SliceStep>
  of(zigzag::CellRef from, zigzag::CellRef to,
     zigzag::DimRef dimension) noexcept;

  [[nodiscard]] zigzag::CellRef from() const noexcept { return from_; }
  [[nodiscard]] zigzag::CellRef to() const noexcept { return to_; }
  [[nodiscard]] zigzag::DimRef dimension() const noexcept { return dimension_; }

  bool operator==(const SliceStep &) const = default;

private:
  SliceStep(zigzag::CellRef from, zigzag::CellRef to,
            zigzag::DimRef dimension) noexcept
      : from_(from), to_(to), dimension_(dimension) {}

  zigzag::CellRef from_;
  zigzag::CellRef to_;
  zigzag::DimRef dimension_;
};

/**
 * @brief Accumulates the run in progress. Pure state: time comes in as an
 *        argument, so a test drives it with any clock.
 *
 * A run settles in one of three ways, and each call that can settle one
 * answers the summary: step() when the step comes after a pause of
 * settleMs (the summary is the run before the pause, and the step begins the
 * next); poll() when the host's clock shows such a pause with no step; and
 * settle() when the reader does anything that is not a step. A run with no
 * steps left once slips are dropped answers nothing.
 */
class WalkRecorder {
public:
  explicit WalkRecorder(const WalkRankingConfig &config) noexcept
      : settleMs(config.settleMs), bounceMs(config.bounceMs) {}

  /// A step straight back along the step before it, within bounceMs, is a
  /// slip: the two cancel and neither is counted.
  std::optional<WalkSummary> step(SliceStep step, std::uint64_t atMs);
  std::optional<WalkSummary> poll(std::uint64_t nowMs);
  std::optional<WalkSummary> settle();

  /// Steps held for the run in progress, after slips.
  [[nodiscard]] std::size_t pending() const noexcept { return run.size(); }

private:
  struct Taken {
    SliceStep step;
    std::uint64_t atMs{};
  };

  [[nodiscard]] bool quietAt(std::uint64_t nowMs) const noexcept;

  std::uint64_t settleMs;
  std::uint64_t bounceMs;
  std::vector<Taken> run;
  /// The latest step offered, slip or not: a pause is measured from it.
  std::optional<std::uint64_t> lastAtMs;
};

struct RankedDimension {
  zigzag::DimRef dimension{};
  /// Steps along it, each run's halved every halfLifeRuns runs of age.
  double used{};

  bool operator==(const RankedDimension &) const = default;
};

/**
 * @brief "Most used": every dimension @p history moved along, most used
 *        first, ties by ascending dimension.
 *
 * Pure. A run's age is how many runs after it the newest in @p history is,
 * by ordinal, so the same history in any order ranks the same. A dimension
 * with no steps in @p history is not emitted. @p config's halfLifeRuns must
 * be positive and finite, as WalkRankingConfig::fromStore() guarantees.
 */
void rankDimensions(
    std::span<const WalkSummary> history, const WalkRankingConfig &config,
    gleditor::cpp26::function_ref<void(const RankedDimension &)> out);

/**
 * @brief Walk summaries kept in the reader's activity store
 *        (system://activity), one run cell per settled run.
 *
 * A summary is cells and dimensions, never text to parse (view-system.md
 * §7.8 draws the shape): the run cell on d.activity-walks, its two ends on
 * d.walk-ends, a cell per dimension on d.walk-steps and per pair on
 * d.walk-pairs, each with its count on d.walk-count. Refs into the visited
 * slice are integer scalar cells, because a link cannot reach into another
 * store.
 *
 * Summaries are kept per slice: a slice cell on d.activity-slices names the
 * slice store's DocumentId and heads that slice's runs on d.walk-runs,
 * because a dimension is a cell of one store and means nothing in another.
 * Ordinals are global, a run's place on d.activity-walks: a run in another
 * slice still ages this one's, since "lately" is the reader's time.
 *
 * With no store the log keeps summaries in memory only, as the activity log
 * does. The constructor throws std::runtime_error on a shape it cannot read,
 * rather than drop part of the reader's history.
 */
class WalkSummaryLog {
public:
  WalkSummaryLog(Store *activity, std::filesystem::path directory);

  /// File @p summary, whose ordinal is ignored, and answer the ordinal it
  /// now has. Saves the activity store. Throws std::runtime_error, writing
  /// nothing, for a summary no recorder could have settled.
  std::uint64_t append(const DocumentId &slice, WalkSummary summary);

  /// @p slice's summaries, oldest first. Invalidated by append().
  [[nodiscard]] std::span<const WalkSummary>
  history(const DocumentId &slice) const;

  /// Summaries of every slice.
  [[nodiscard]] std::uint64_t size() const noexcept { return count; }

private:
  using SliceKey = std::array<std::uint8_t, 16>;

  Store *store;
  std::filesystem::path directory;
  std::map<SliceKey, std::vector<WalkSummary>> bySlice;
  std::uint64_t count{};
};

} // namespace xanadu::view

#endif // COMMON_XANADU_VIEW_DIMENSION_RANKING_HPP

#include "common/xanadu/view/dimension_ranking.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include <gleditor/logging.hpp>

#include "common/xanadu/store.hpp"
#include "common/xanadu/store_activity_log.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace xanadu::view {
namespace {

constexpr const char *kRankingCategory = "view.ranking";

// The shape of a walk summary in system://activity (view-system.md §7.8).
// Every part is a cell on a dimension named for what it is, so nothing in a
// summary is read by parsing text.
constexpr std::string_view kWalks  = "d.activity-walks";  // home -> runs
constexpr std::string_view kSlices = "d.activity-slices"; // home -> slices
constexpr std::string_view kRuns   = "d.walk-runs";       // slice -> its runs
constexpr std::string_view kEnds   = "d.walk-ends";  // run -> began -> ended
constexpr std::string_view kSteps  = "d.walk-steps"; // run -> dimensions
constexpr std::string_view kPairs  = "d.walk-pairs"; // run -> pairs
constexpr std::string_view kTo     = "d.walk-to";    // pair -> to
constexpr std::string_view kCount  = "d.walk-count"; // part -> its count
constexpr std::array kShape{kWalks, kSlices, kRuns, kEnds,
                            kSteps, kPairs,  kTo,   kCount};

/// A run cell's content: what the cell is, for a reader browsing the store.
constexpr std::string_view kRunText = "walk";

using ShapeDims = std::map<std::string_view, zigzag::DimRef>;

[[noreturn]] void malformed(const std::string_view why) {
  throw std::runtime_error("activity: malformed walk summary: " +
                           std::string(why));
}

bool isCell(const zigzag::CellRef ref) noexcept {
  return ref != zigzag::noCell && !zigzag::isEphemeral(ref);
}

/// What a recorder can settle: real refs; counts non-zero and ascending; one
/// pair between each two consecutive steps.
bool wellFormed(const WalkSummary &summary) noexcept {
  if (!isCell(summary.began) || !isCell(summary.ended) ||
      summary.steps.empty()) {
    return false;
  }
  std::uint64_t total{};
  for (std::size_t i = 0; i < summary.steps.size(); ++i) {
    const auto &steps = summary.steps[i];
    if (!isCell(steps.dimension) || steps.steps == 0 ||
        (i > 0 && summary.steps[i - 1].dimension >= steps.dimension)) {
      return false;
    }
    total += steps.steps;
  }
  std::uint64_t followed{};
  for (std::size_t i = 0; i < summary.changes.size(); ++i) {
    const auto &change = summary.changes[i];
    if (!isCell(change.from) || !isCell(change.to) || change.times == 0 ||
        (i > 0 &&
         std::pair{summary.changes[i - 1].from, summary.changes[i - 1].to} >=
             std::pair{change.from, change.to})) {
      return false;
    }
    followed += change.times;
  }
  return followed + 1 == total;
}

zigzag::CellRef tailOf(const zigzag::Manifold &manifold, zigzag::CellRef from,
                       const zigzag::DimRef dim) {
  for (auto next = manifold.linked(from, dim); next.has_value();
       next      = manifold.linked(from, dim)) {
    from = *next;
  }
  return from;
}

/// A ref into the visited slice, held as an integer: a link cannot reach
/// into another store.
zigzag::CellRef refIn(const zigzag::Manifold &manifold,
                      const zigzag::CellRef cell) {
  const auto value = manifold.asInt64(cell);
  if (!value || *value <= 0 ||
      *value > std::numeric_limits<zigzag::CellRef>::max() ||
      zigzag::isEphemeral(static_cast<zigzag::CellRef>(*value))) {
    malformed("a ref that is not a real cell");
  }
  return static_cast<zigzag::CellRef>(*value);
}

std::uint32_t countIn(const zigzag::Manifold &manifold,
                      const zigzag::OptionalCell cell) {
  if (!cell) malformed("a part without its count");
  const auto value = manifold.asInt64(*cell);
  if (!value || *value <= 0 ||
      *value > std::numeric_limits<std::uint32_t>::max()) {
    malformed("a count that is not a positive integer");
  }
  return static_cast<std::uint32_t>(*value);
}

WalkSummary readRun(const zigzag::Manifold &manifold, const Store &store,
                    const ShapeDims &dims, const zigzag::CellRef run) {
  if (manifold.textOf(run, store) != kRunText) malformed("not a run cell");
  const auto began = manifold.linked(run, dims.at(kEnds));
  const auto ended =
      began ? manifold.linked(*began, dims.at(kEnds)) : zigzag::OptionalCell{};
  if (!ended || manifold.linked(*ended, dims.at(kEnds))) {
    malformed("a run without exactly two ends");
  }
  WalkSummary summary{.began = refIn(manifold, *began),
                      .ended = refIn(manifold, *ended)};
  // A ring along either rank repeats a part, so the order checks refuse it
  // before the walk could go round for ever.
  for (auto part = manifold.linked(run, dims.at(kSteps)); part;
       part      = manifold.linked(*part, dims.at(kSteps))) {
    summary.steps.push_back(
        {refIn(manifold, *part),
         countIn(manifold, manifold.linked(*part, dims.at(kCount)))});
    if (summary.steps.size() > 1 &&
        summary.steps[summary.steps.size() - 2].dimension >=
            summary.steps.back().dimension) {
      malformed("steps out of order");
    }
  }
  for (auto part = manifold.linked(run, dims.at(kPairs)); part;
       part      = manifold.linked(*part, dims.at(kPairs))) {
    const auto to = manifold.linked(*part, dims.at(kTo));
    if (!to) malformed("a pair without its second dimension");
    summary.changes.push_back(
        {refIn(manifold, *part), refIn(manifold, *to),
         countIn(manifold, manifold.linked(*part, dims.at(kCount)))});
    const auto n = summary.changes.size();
    if (n > 1 &&
        std::pair{summary.changes[n - 2].from, summary.changes[n - 2].to} >=
            std::pair{summary.changes[n - 1].from, summary.changes[n - 1].to}) {
      malformed("pairs out of order");
    }
  }
  if (!wellFormed(summary)) malformed("counts that no run could make");
  return summary;
}

/// Mints a summary's cells and links, threading the state through.
class RunWriter {
public:
  RunWriter(Store &store, MicroversionId at, ShapeDims dims)
      : store(store), at(std::move(at)), dims(std::move(dims)) {}

  zigzag::CellRef cell(const std::string_view text) {
    at = store.makeCell(at, text);
    return store.cellRefOf(at);
  }
  zigzag::CellRef integer(const std::int64_t value) {
    at = store.makeScalarCell(at, value);
    return store.cellRefOf(at);
  }
  RunWriter *link(const zigzag::CellRef from, const std::string_view dim,
                  const zigzag::CellRef to) {
    at = store.setLink(at, from, dims.at(dim), zigzag::DimVector::POS, to);
    return this;
  }

private:
  Store &store;
  MicroversionId at;
  ShapeDims dims;
};

} // namespace

std::optional<SliceStep>
SliceStep::of(const zigzag::CellRef from, const zigzag::CellRef to,
              const zigzag::DimRef dimension) noexcept {
  if (!isCell(from) || !isCell(to) || !isCell(dimension)) {
    return std::nullopt;
  }
  return SliceStep{from, to, dimension};
}

bool WalkRecorder::quietAt(const std::uint64_t nowMs) const noexcept {
  // A clock that runs backwards reads as no pause at all.
  return lastAtMs && nowMs >= *lastAtMs && nowMs - *lastAtMs >= settleMs;
}

std::optional<WalkSummary> WalkRecorder::step(const SliceStep step,
                                              const std::uint64_t atMs) {
  auto settled = quietAt(atMs) ? settle() : std::nullopt;
  lastAtMs     = atMs;
  if (!run.empty()) {
    const auto &previous = run.back();
    const bool back      = previous.step.dimension() == step.dimension() &&
                      previous.step.from() == step.to() &&
                      previous.step.to() == step.from();
    if (back && atMs >= previous.atMs && atMs - previous.atMs <= bounceMs) {
      GLEDITOR_LOG_TRACE(kRankingCategory, "slip along {} dropped",
                         step.dimension());
      run.pop_back();
      return settled;
    }
  }
  run.push_back({step, atMs});
  return settled;
}

std::optional<WalkSummary> WalkRecorder::poll(const std::uint64_t nowMs) {
  if (!quietAt(nowMs)) return std::nullopt;
  return settle();
}

std::optional<WalkSummary> WalkRecorder::settle() {
  if (run.empty()) return std::nullopt;
  std::map<zigzag::DimRef, std::uint32_t> steps;
  std::map<std::pair<zigzag::DimRef, zigzag::DimRef>, std::uint32_t> changes;
  for (std::size_t i = 0; i < run.size(); ++i) {
    ++steps[run[i].step.dimension()];
    if (i > 0) {
      ++changes[{run[i - 1].step.dimension(), run[i].step.dimension()}];
    }
  }
  WalkSummary summary{.began = run.front().step.from(),
                      .ended = run.back().step.to()};
  summary.steps.reserve(steps.size());
  for (const auto &[dimension, count] : steps) {
    summary.steps.push_back({dimension, count});
  }
  summary.changes.reserve(changes.size());
  for (const auto &[pair, times] : changes) {
    summary.changes.push_back({pair.first, pair.second, times});
  }
  GLEDITOR_LOG_DEBUG(kRankingCategory, "run of {} steps settled", run.size());
  run.clear();
  return summary;
}

void rankDimensions(
    const std::span<const WalkSummary> history, const WalkRankingConfig &config,
    const gleditor::cpp26::function_ref<void(const RankedDimension &)> out) {
  if (history.empty()) return;
  const auto newest =
      std::ranges::max_element(history, {}, &WalkSummary::ordinal)->ordinal;
  std::map<zigzag::DimRef, double> used;
  for (const auto &summary : history) {
    const auto age    = static_cast<double>(newest - summary.ordinal);
    const auto weight = std::exp2(-age / config.halfLifeRuns);
    for (const auto &steps : summary.steps) {
      used[steps.dimension] += weight * steps.steps;
    }
  }
  std::vector<RankedDimension> ranked;
  ranked.reserve(used.size());
  for (const auto &[dimension, amount] : used) {
    ranked.push_back({dimension, amount});
  }
  // The map already ordered ties by dimension; a stable sort keeps that.
  std::ranges::stable_sort(ranked, std::ranges::greater{},
                           &RankedDimension::used);
  for (const auto &entry : ranked) out(entry);
}

WalkSummaryLog::WalkSummaryLog(Store *activity,
                               std::filesystem::path aDirectory)
    : store(activity), directory(std::move(aDirectory)) {
  if (!store || store->homeCell() == zigzag::noCell) return;
  const auto home     = store->homeCell();
  const auto manifold = store->rebuildManifold(store->latest());
  ShapeDims dims;
  for (const auto name : kShape) {
    if (const auto dim = manifold.dimensionNamed(name, *store)) {
      dims.emplace(name, *dim);
    }
  }
  if (!dims.contains(kWalks)) return;
  if (dims.size() != kShape.size()) malformed("a dimension of its shape gone");

  std::map<zigzag::CellRef, std::uint64_t> ordinals;
  for (auto run = manifold.linked(home, dims.at(kWalks)); run;
       run      = manifold.linked(*run, dims.at(kWalks))) {
    if (!ordinals.emplace(*run, ordinals.size() + 1).second) {
      malformed("the runs form a ring");
    }
  }
  std::set<std::array<std::uint8_t, 16>> seen;
  std::uint64_t found{};
  for (auto slice = manifold.linked(home, dims.at(kSlices)); slice;
       slice      = manifold.linked(*slice, dims.at(kSlices))) {
    const auto id = parseActivityId(manifold.textOf(*slice, *store));
    if (!seen.insert(id.bytes()).second) malformed("a slice filed twice");
    auto &runs = bySlice[id.bytes()];
    for (auto run = manifold.linked(*slice, dims.at(kRuns)); run;
         run      = manifold.linked(*run, dims.at(kRuns))) {
      const auto ordinal = ordinals.find(*run);
      if (ordinal == ordinals.end()) malformed("a slice's run not in order");
      if (!runs.empty() && runs.back().ordinal >= ordinal->second) {
        malformed("a slice's runs out of order");
      }
      auto summary    = readRun(manifold, *store, dims, *run);
      summary.ordinal = ordinal->second;
      runs.push_back(std::move(summary));
      ++found;
    }
  }
  if (found != ordinals.size()) malformed("a run filed under no slice");
  count = ordinals.size();
  GLEDITOR_LOG_DEBUG(kRankingCategory, "read {} walk summaries", count);
}

std::uint64_t WalkSummaryLog::append(const DocumentId &slice,
                                     WalkSummary summary) {
  // Refused before anything is written: a summary the constructor cannot
  // read back would stop every later session reading the reader's history.
  if (!wellFormed(summary)) malformed("counts that no run could make");
  summary.ordinal = count + 1;
  if (store) {
    auto at = store->latest();
    if (store->homeCell() == zigzag::noCell) at = store->sliceGenesis(at);
    ShapeDims dims;
    {
      const auto before = store->rebuildManifold(at);
      for (const auto name : kShape) {
        if (const auto dim = before.dimensionNamed(name, *store)) {
          dims.emplace(name, *dim);
        } else {
          const auto made = store->makeDimension(at, name);
          at              = made.version;
          dims.emplace(name, made.dim);
        }
      }
    }
    const auto manifold = store->rebuildManifold(at);
    const auto home     = store->homeCell();
    const auto idText   = activityIdText(slice);
    std::optional<zigzag::CellRef> sliceCell;
    for (auto next = manifold.linked(home, dims.at(kSlices)); next;
         next      = manifold.linked(*next, dims.at(kSlices))) {
      if (manifold.textOf(*next, *store) == idText) {
        sliceCell = *next;
        break;
      }
    }
    const auto walksTail = tailOf(manifold, home, dims.at(kWalks));
    RunWriter writer(*store, at, dims);
    // A slice cell minted now has no runs, and the manifold predates it.
    const auto runsTail =
        sliceCell ? tailOf(manifold, *sliceCell, dims.at(kRuns)) : [&] {
          const auto minted = writer.cell(idText);
          writer.link(tailOf(manifold, home, dims.at(kSlices)), kSlices,
                      minted);
          return minted;
        }();
    const auto run = writer.cell(kRunText);
    writer.link(walksTail, kWalks, run)->link(runsTail, kRuns, run);
    const auto began = writer.integer(summary.began);
    const auto ended = writer.integer(summary.ended);
    writer.link(run, kEnds, began)->link(began, kEnds, ended);
    auto previous = run;
    for (const auto &steps : summary.steps) {
      const auto part  = writer.integer(steps.dimension);
      const auto times = writer.integer(steps.steps);
      writer.link(previous, kSteps, part)->link(part, kCount, times);
      previous = part;
    }
    previous = run;
    for (const auto &change : summary.changes) {
      const auto part  = writer.integer(change.from);
      const auto to    = writer.integer(change.to);
      const auto times = writer.integer(change.times);
      writer.link(previous, kPairs, part)
          ->link(part, kTo, to)
          ->link(part, kCount, times);
      previous = part;
    }
    std::filesystem::create_directories(directory);
    store->save(directory.string());
  }
  ++count;
  bySlice[slice.bytes()].push_back(std::move(summary));
  return count;
}

std::span<const WalkSummary>
WalkSummaryLog::history(const DocumentId &slice) const {
  const auto found = bySlice.find(slice.bytes());
  if (found == bySlice.end()) return {};
  return found->second;
}

} // namespace xanadu::view

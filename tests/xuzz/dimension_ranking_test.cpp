#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/store_activity_log.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/view/dimension_ranking.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace {

using xanadu::view::DimensionChange;
using xanadu::view::DimensionSteps;
using xanadu::view::RankedDimension;
using xanadu::view::SliceStep;
using xanadu::view::WalkRecorder;
using xanadu::view::WalkSummary;
using xanadu::view::WalkSummaryLog;

constexpr zigzag::DimRef d1 = 101;
constexpr zigzag::DimRef d2 = 102;
constexpr zigzag::DimRef d3 = 103;

SliceStep realStep(const zigzag::CellRef from, const zigzag::CellRef to,
                   const zigzag::DimRef dimension) {
  const auto step = SliceStep::of(from, to, dimension);
  if (!step) throw std::logic_error("test step is not real");
  return *step;
}

std::vector<RankedDimension>
ranked(const std::vector<WalkSummary> &history,
       const xanadu::WalkRankingConfig &config = {}) {
  std::vector<RankedDimension> out;
  xanadu::view::rankDimensions(
      history, config,
      [&out](const RankedDimension &entry) { out.push_back(entry); });
  return out;
}

std::vector<WalkSummary> copyOf(const std::span<const WalkSummary> history) {
  return {history.begin(), history.end()};
}

/// 1 to 2 along d1, then 2 to 3 along d2.
WalkSummary twoDimensionRun() {
  WalkRecorder recorder({});
  recorder.step(realStep(1, 2, d1), 0);
  recorder.step(realStep(2, 3, d2), 10);
  return *recorder.settle();
}

WalkSummary summaryOf(const std::uint64_t ordinal,
                      std::vector<DimensionSteps> steps) {
  WalkSummary summary{.ordinal = ordinal, .began = 1, .ended = 2};
  summary.steps = std::move(steps);
  return summary;
}

/// A temporary directory with one permascroll, removed on destruction.
struct ActivityPlace {
  std::filesystem::path base =
      std::filesystem::temp_directory_path() /
      ("xuzz-walks-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::shared_ptr<xanadu::UserPermascroll> scroll;
  std::filesystem::path directory = base / "activity";

  ActivityPlace() {
    std::filesystem::remove_all(base);
    xanadu::UserPermascroll::Config config;
    config.storageDir = base / "permascroll";
    scroll            = std::make_shared<xanadu::UserPermascroll>(config);
  }
  ActivityPlace(const ActivityPlace &)            = delete;
  ActivityPlace &operator=(const ActivityPlace &) = delete;
  ActivityPlace(ActivityPlace &&)                 = delete;
  ActivityPlace &operator=(ActivityPlace &&)      = delete;
  ~ActivityPlace() {
    scroll.reset();
    std::filesystem::remove_all(base);
  }
};

} // namespace

TEST(SliceStepTest, OnlyRealCellsAlongARealDimensionAreMovementThroughASlice) {
  EXPECT_TRUE(SliceStep::of(1, 2, d1));
  EXPECT_FALSE(SliceStep::of(zigzag::ephemeralBit | 1U, 2, d1));
  EXPECT_FALSE(SliceStep::of(1, zigzag::ephemeralBit | 2U, d1));
  EXPECT_FALSE(SliceStep::of(1, 2, zigzag::ephemeralBit | d1));
  EXPECT_FALSE(SliceStep::of(zigzag::noCell, 2, d1));
  EXPECT_FALSE(SliceStep::of(1, 2, zigzag::noCell));
}

TEST(WalkRecorderTest, CondensesARunIntoStepsAndWhatFollowedWhat) {
  WalkRecorder recorder({});
  EXPECT_FALSE(recorder.step(realStep(1, 2, d1), 0));
  EXPECT_FALSE(recorder.step(realStep(2, 3, d1), 100));
  EXPECT_FALSE(recorder.step(realStep(3, 4, d2), 200));
  EXPECT_FALSE(recorder.step(realStep(4, 5, d1), 300));
  EXPECT_EQ(recorder.pending(), 4U);

  const auto summary = recorder.settle();
  ASSERT_TRUE(summary);
  EXPECT_EQ(summary->ordinal, 0U) << "the activity store assigns it";
  EXPECT_EQ(summary->began, 1U);
  EXPECT_EQ(summary->ended, 5U);
  EXPECT_EQ(summary->steps, (std::vector<DimensionSteps>{{d1, 3}, {d2, 1}}));
  // The input the deferred Markov order reads: which dimension followed
  // which, going on along one dimension included.
  EXPECT_EQ(summary->changes, (std::vector<DimensionChange>{
                                  {d1, d1, 1}, {d1, d2, 1}, {d2, d1, 1}}));
  EXPECT_EQ(recorder.pending(), 0U);
  EXPECT_FALSE(recorder.settle()) << "nothing is left to settle";
}

TEST(WalkRecorderTest, AStepStraightBackSoonIsASlipAndCountsNeither) {
  const xanadu::WalkRankingConfig config;
  WalkRecorder recorder(config);
  recorder.step(realStep(1, 2, d1), 0);
  recorder.step(realStep(2, 1, d1), config.bounceMs);
  EXPECT_EQ(recorder.pending(), 0U);
  recorder.step(realStep(1, 3, d2), config.bounceMs + 1);
  const auto summary = recorder.settle();
  ASSERT_TRUE(summary);
  EXPECT_EQ(summary->began, 1U);
  EXPECT_EQ(summary->ended, 3U);
  EXPECT_EQ(summary->steps, (std::vector<DimensionSteps>{{d2, 1}}));
  EXPECT_TRUE(summary->changes.empty());
}

TEST(WalkRecorderTest, SlipsUnwindOneStepAtATime) {
  WalkRecorder recorder({});
  recorder.step(realStep(1, 2, d1), 0);
  recorder.step(realStep(2, 3, d2), 400);
  recorder.step(realStep(3, 2, d2), 500);
  const auto summary = recorder.settle();
  ASSERT_TRUE(summary);
  EXPECT_EQ(summary->ended, 2U);
  EXPECT_EQ(summary->steps, (std::vector<DimensionSteps>{{d1, 1}}));
}

TEST(WalkRecorderTest, ReturningAfterTheBounceTimeIsTwoSteps) {
  const xanadu::WalkRankingConfig config;
  WalkRecorder recorder(config);
  recorder.step(realStep(1, 2, d1), 0);
  recorder.step(realStep(2, 1, d1), config.bounceMs + 1);
  const auto summary = recorder.settle();
  ASSERT_TRUE(summary);
  EXPECT_EQ(summary->steps, (std::vector<DimensionSteps>{{d1, 2}}));
  EXPECT_EQ(summary->began, summary->ended);
}

TEST(WalkRecorderTest, BackAlongAnotherDimensionIsNotASlip) {
  WalkRecorder recorder({});
  recorder.step(realStep(1, 2, d1), 0);
  recorder.step(realStep(2, 1, d2), 10);
  EXPECT_EQ(recorder.pending(), 2U);
}

TEST(WalkRecorderTest, ARunOfOnlySlipsSettlesToNothing) {
  WalkRecorder recorder({});
  recorder.step(realStep(1, 2, d1), 0);
  recorder.step(realStep(2, 1, d1), 50);
  EXPECT_FALSE(recorder.settle());
}

TEST(WalkRecorderTest, APauseSettlesTheRunBeforeIt) {
  const xanadu::WalkRankingConfig config;
  WalkRecorder recorder(config);
  recorder.step(realStep(1, 2, d1), 1000);
  recorder.step(realStep(2, 3, d1), 1100);
  EXPECT_FALSE(recorder.poll(1100 + config.settleMs - 1));
  const auto polled = recorder.poll(1100 + config.settleMs);
  ASSERT_TRUE(polled);
  EXPECT_EQ(polled->steps, (std::vector<DimensionSteps>{{d1, 2}}));

  recorder.step(realStep(3, 4, d2), 5000);
  const auto stepped = recorder.step(realStep(4, 5, d3), 5000 + 10000);
  ASSERT_TRUE(stepped) << "a step after a pause closes the run before it";
  EXPECT_EQ(stepped->steps, (std::vector<DimensionSteps>{{d2, 1}}));
  EXPECT_EQ(recorder.pending(), 1U) << "and begins the next";
}

TEST(WalkRecorderTest, TheSlipItselfKeepsTheRunAwake) {
  const xanadu::WalkRankingConfig config;
  WalkRecorder recorder(config);
  recorder.step(realStep(1, 2, d1), 0);
  recorder.step(realStep(2, 3, d2), config.settleMs - 10);
  recorder.step(realStep(3, 2, d2), config.settleMs);
  EXPECT_FALSE(recorder.poll(config.settleMs + 10))
      << "a pause is measured from the latest step offered";
}

TEST(DimensionRankingTest, UseDecaysByHalfLifeInRuns) {
  const std::vector history{summaryOf(1, {{d1, 10}}),
                            summaryOf(401, {{d2, 3}})};
  const auto fast = ranked(history, {.halfLifeRuns = 200.0});
  ASSERT_EQ(fast.size(), 2U);
  EXPECT_EQ(fast[0].dimension, d2);
  EXPECT_DOUBLE_EQ(fast[0].used, 3.0);
  EXPECT_EQ(fast[1].dimension, d1);
  EXPECT_DOUBLE_EQ(fast[1].used, 2.5) << "two half-lives old";

  const auto slow = ranked(history, {.halfLifeRuns = 1000.0});
  ASSERT_EQ(slow.size(), 2U);
  EXPECT_EQ(slow[0].dimension, d1) << "lately, without forgetting the past";
  EXPECT_DOUBLE_EQ(slow[0].used, 10.0 * std::exp2(-0.4));
}

TEST(DimensionRankingTest, TiesGoByDimensionAndTheOrderIsDeterministic) {
  std::vector history{summaryOf(1, {{d3, 2}, {d1, 1}}), summaryOf(2, {{d2, 1}}),
                      summaryOf(3, {{d1, 1}})};
  const auto first = ranked(history);
  EXPECT_EQ(first, ranked(history));
  std::ranges::reverse(history);
  const auto reversed = ranked(history);
  ASSERT_EQ(first.size(), reversed.size());
  for (std::size_t i = 0; i < first.size(); ++i) {
    EXPECT_EQ(first[i].dimension, reversed[i].dimension);
    EXPECT_DOUBLE_EQ(first[i].used, reversed[i].used);
  }
  const auto equal = ranked({summaryOf(1, {{d3, 1}, {d2, 1}, {d1, 1}})});
  ASSERT_EQ(equal.size(), 3U);
  EXPECT_EQ(equal[0].dimension, d1);
  EXPECT_EQ(equal[1].dimension, d2);
  EXPECT_EQ(equal[2].dimension, d3);
  EXPECT_TRUE(ranked({}).empty());
}

TEST(WalkSummaryLogTest, SummariesSurviveARestartPerSlice) {
  ActivityPlace place;
  const xanadu::DocumentId sliceA, sliceB;
  std::vector<RankedDimension> before;
  {
    xanadu::Store activity(place.scroll);
    WalkSummaryLog log(&activity, place.directory);
    WalkRecorder recorder({});
    recorder.step(realStep(1, 2, d1), 0);
    recorder.step(realStep(2, 3, d2), 10);
    EXPECT_EQ(log.append(sliceA, *recorder.settle()), 1U);
    recorder.step(realStep(7, 8, d3), 20);
    EXPECT_EQ(log.append(sliceB, *recorder.settle()), 2U);
    recorder.step(realStep(3, 4, d2), 30);
    EXPECT_EQ(log.append(sliceA, *recorder.settle()), 3U);
    before = ranked(copyOf(log.history(sliceA)));
  }
  xanadu::Store reopened(place.scroll);
  reopened.load(place.directory.string());
  const WalkSummaryLog log(&reopened, place.directory);
  EXPECT_EQ(log.size(), 3U);
  const auto a = log.history(sliceA);
  ASSERT_EQ(a.size(), 2U);
  EXPECT_EQ(a[0].ordinal, 1U);
  EXPECT_EQ(a[0].began, 1U);
  EXPECT_EQ(a[0].ended, 3U);
  EXPECT_EQ(a[0].changes, (std::vector<DimensionChange>{{d1, d2, 1}}));
  EXPECT_EQ(a[1].ordinal, 3U);
  ASSERT_EQ(log.history(sliceB).size(), 1U);
  EXPECT_EQ(log.history(sliceB)[0].steps,
            (std::vector<DimensionSteps>{{d3, 1}}));
  EXPECT_EQ(ranked(copyOf(a)), before);
  EXPECT_TRUE(log.history(xanadu::DocumentId{}).empty());
}

TEST(WalkSummaryLogTest, MovementAppendsNothingToTheVisitedSlice) {
  ActivityPlace place;
  xanadu::Store slice(place.scroll);
  auto version         = slice.sliceGenesis({});
  const auto dimension = slice.makeDimension(version, "d.1");
  version              = slice.makeCell(dimension.version, "first");
  const auto first     = slice.cellRefOf(version);
  version              = slice.makeCell(version, "second");
  const auto second    = slice.cellRefOf(version);
  version = slice.setLink(version, first, dimension.dim, zigzag::DimVector::POS,
                          second);
  const auto sliceOps = slice.opCount();

  xanadu::Store activity(place.scroll);
  WalkSummaryLog log(&activity, place.directory);
  WalkRecorder recorder({});
  recorder.step(realStep(first, second, dimension.dim), 0);
  recorder.step(realStep(second, first, dimension.dim), 1000);
  recorder.step(realStep(first, second, dimension.dim), 2000);
  EXPECT_EQ(activity.opCount(), 0U) << "a step is never recorded alone";

  log.append(slice.documentId(), *recorder.settle());
  EXPECT_GT(activity.opCount(), 0U);
  EXPECT_EQ(slice.opCount(), sliceOps);
  EXPECT_EQ(log.history(slice.documentId())[0].steps,
            (std::vector<DimensionSteps>{{dimension.dim, 3}}));
}

TEST(WalkSummaryLogTest, ASummaryNoRecorderCouldSettleIsRefusedUnwritten) {
  ActivityPlace place;
  xanadu::Store activity(place.scroll);
  WalkSummaryLog log(&activity, place.directory);
  const xanadu::DocumentId slice;
  EXPECT_THROW(log.append(slice, WalkSummary{}), std::runtime_error);
  auto inconsistent    = summaryOf(0, {{d1, 2}});
  inconsistent.changes = {{d1, d1, 5}};
  EXPECT_THROW(log.append(slice, inconsistent), std::runtime_error);
  EXPECT_EQ(activity.opCount(), 0U);
  EXPECT_EQ(log.size(), 0U);
}

TEST(WalkSummaryLogTest, ASummaryIsCellsAndDimensionsNotText) {
  ActivityPlace place;
  xanadu::Store activity(place.scroll);
  const xanadu::DocumentId slice;
  WalkSummaryLog(&activity, place.directory).append(slice, twoDimensionRun());

  const auto manifold = activity.rebuildManifold(activity.latest());
  const auto dim      = [&](const std::string_view name) {
    const auto found = manifold.dimensionNamed(name, activity);
    if (!found) throw std::logic_error("no such dimension");
    return *found;
  };
  const auto home   = activity.homeCell();
  const auto slices = manifold.linked(home, dim("d.activity-slices"));
  ASSERT_TRUE(slices);
  EXPECT_EQ(manifold.textOf(*slices, activity), xanadu::activityIdText(slice));
  const auto run = manifold.linked(home, dim("d.activity-walks"));
  ASSERT_TRUE(run);
  EXPECT_TRUE(manifold.linked(*slices, dim("d.walk-runs")) == *run);

  const auto began = manifold.linked(*run, dim("d.walk-ends"));
  ASSERT_TRUE(began);
  EXPECT_EQ(manifold.asInt64(*began), 1);
  const auto ended = manifold.linked(*began, dim("d.walk-ends"));
  ASSERT_TRUE(ended);
  EXPECT_EQ(manifold.asInt64(*ended), 3);

  const auto first = manifold.linked(*run, dim("d.walk-steps"));
  ASSERT_TRUE(first);
  EXPECT_EQ(manifold.asInt64(*first), std::int64_t{d1});
  const auto firstCount = manifold.linked(*first, dim("d.walk-count"));
  ASSERT_TRUE(firstCount);
  EXPECT_EQ(manifold.asInt64(*firstCount), 1);
  const auto second = manifold.linked(*first, dim("d.walk-steps"));
  ASSERT_TRUE(second);
  EXPECT_EQ(manifold.asInt64(*second), std::int64_t{d2});

  const auto pair = manifold.linked(*run, dim("d.walk-pairs"));
  ASSERT_TRUE(pair);
  EXPECT_EQ(manifold.asInt64(*pair), std::int64_t{d1});
  const auto to = manifold.linked(*pair, dim("d.walk-to"));
  ASSERT_TRUE(to);
  EXPECT_EQ(manifold.asInt64(*to), std::int64_t{d2});
  const auto times = manifold.linked(*pair, dim("d.walk-count"));
  ASSERT_TRUE(times);
  EXPECT_EQ(manifold.asInt64(*times), 1);
  EXPECT_FALSE(manifold.linked(*pair, dim("d.walk-pairs")));

  // A further run of the same slice: two dimensions and one pair.
  const auto before = activity.opCount();
  WalkSummaryLog(&activity, place.directory).append(slice, twoDimensionRun());
  EXPECT_EQ(activity.opCount() - before, 7U + (4U * 2U) + (6U * 1U))
      << "the price §7.8 states";
}

TEST(WalkSummaryLogTest, AnUnreadableShapeIsRefusedLoudly) {
  ActivityPlace place;
  {
    // A run cell filed under no slice, its parts' dimensions never made.
    xanadu::Store activity(place.scroll);
    xanadu::appendActivityRecord(activity, place.base / "orphan",
                                 "d.activity-walks", "walk");
    EXPECT_THROW(
        static_cast<void>(WalkSummaryLog{&activity, place.base / "orphan"}),
        std::runtime_error);
  }
  // Each case below starts from one good summary and breaks one thing.
  const auto broken = [&place](const std::string &name, auto &&breakIt) {
    xanadu::Store activity(place.scroll);
    const xanadu::DocumentId slice;
    WalkSummaryLog(&activity, place.base / name)
        .append(slice, twoDimensionRun());
    const auto manifold = activity.rebuildManifold(activity.latest());
    const auto dim      = [&](const std::string_view dimension) {
      return *manifold.dimensionNamed(dimension, activity);
    };
    breakIt(activity, manifold, dim, slice);
    EXPECT_THROW(
        static_cast<void>(WalkSummaryLog{&activity, place.base / name}),
        std::runtime_error)
        << name;
  };
  broken("third-end", [](xanadu::Store &activity, const auto &manifold,
                         const auto &dim, const xanadu::DocumentId &) {
    const auto run =
        *manifold.linked(activity.homeCell(), dim("d.activity-walks"));
    const auto began = *manifold.linked(run, dim("d.walk-ends"));
    const auto ended = *manifold.linked(began, dim("d.walk-ends"));
    const auto at = activity.makeScalarCell(activity.latest(), std::int64_t{9});
    static_cast<void>(activity.setLink(at, ended, dim("d.walk-ends"),
                                       zigzag::DimVector::POS,
                                       activity.cellRefOf(at)));
  });
  broken("slice-twice", [](xanadu::Store &activity, const auto &manifold,
                           const auto &dim, const xanadu::DocumentId &slice) {
    const auto first =
        *manifold.linked(activity.homeCell(), dim("d.activity-slices"));
    const auto at =
        activity.makeCell(activity.latest(), xanadu::activityIdText(slice));
    static_cast<void>(activity.setLink(at, first, dim("d.activity-slices"),
                                       zigzag::DimVector::POS,
                                       activity.cellRefOf(at)));
  });
  broken("zero-count", [](xanadu::Store &activity, const auto &manifold,
                          const auto &dim, const xanadu::DocumentId &) {
    // A third step dimension with no steps along it.
    const auto run =
        *manifold.linked(activity.homeCell(), dim("d.activity-walks"));
    const auto first  = *manifold.linked(run, dim("d.walk-steps"));
    const auto second = *manifold.linked(first, dim("d.walk-steps"));
    auto at = activity.makeScalarCell(activity.latest(), std::int64_t{d3});
    const auto part = activity.cellRefOf(at);
    at              = activity.setLink(at, second, dim("d.walk-steps"),
                                       zigzag::DimVector::POS, part);
    at              = activity.makeScalarCell(at, std::int64_t{0});
    static_cast<void>(activity.setLink(at, part, dim("d.walk-count"),
                                       zigzag::DimVector::POS,
                                       activity.cellRefOf(at)));
  });
}

TEST(WalkRankingConfigTest, SettingsSeedTheDefaultsAndOverridesAreRead) {
  const xanadu::WalkRankingConfig defaults;
  const auto specs =
      xanadu::defaultSettingSpecs(xanadu::SystemDocKind::Settings);
  const auto defaultOf = [&specs](const std::string_view name) {
    const auto found =
        std::ranges::find(specs, name, &xanadu::SettingSpec::name);
    if (found == specs.end()) throw std::logic_error("no such setting");
    return found->schemas.front().defaultValues.front();
  };
  EXPECT_EQ(
      std::get<std::int64_t>(defaultOf(xanadu::settings::kActivitySettleMs)),
      defaults.settleMs);
  EXPECT_EQ(
      std::get<std::int64_t>(defaultOf(xanadu::settings::kActivityBounceMs)),
      defaults.bounceMs);
  EXPECT_DOUBLE_EQ(std::get<double>(defaultOf(xanadu::settings::kRankHalfLife)),
                   defaults.halfLifeRuns);

  xanadu::Store store;
  store.setSystem(true);
  xanadu::initializeSystemStore(store, xanadu::SystemDocKind::Settings);
  EXPECT_EQ(xanadu::WalkRankingConfig::fromStore(store), defaults);

  auto version = xanadu::setSetting(store, store.primaryCurrentVersion(),
                                    xanadu::settings::kActivitySettleMs,
                                    std::int64_t{900});
  version =
      xanadu::setSetting(store, version, xanadu::settings::kRankHalfLife, 50.0);
  store.repointCurrentVersion(version);
  auto read = xanadu::WalkRankingConfig::fromStore(store);
  EXPECT_EQ(read.settleMs, 900U);
  EXPECT_EQ(read.bounceMs, defaults.bounceMs);
  EXPECT_DOUBLE_EQ(read.halfLifeRuns, 50.0);

  version =
      xanadu::setSetting(store, version, xanadu::settings::kRankHalfLife, -1.0);
  store.repointCurrentVersion(version);
  read = xanadu::WalkRankingConfig::fromStore(store);
  EXPECT_DOUBLE_EQ(read.halfLifeRuns, defaults.halfLifeRuns)
      << "a half-life that is not positive keeps the default";
}

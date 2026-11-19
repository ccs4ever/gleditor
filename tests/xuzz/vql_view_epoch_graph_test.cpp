// EPOCHREAD consumer: the handle-resolution subroutine of a dynamic view step
// compares, in the graph, the epoch witness recorded beside a handle with
// EpochOfCell of the source and view cells it names. The host refuses a stale
// handle it can see while compiling; these are the routes it cannot see (a
// def argument, a for source, a stream a def returns), where only the graph
// stands between a reused cell number and a wrong answer.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/view_cells.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using xanadu::vql::VQLEngine;

struct VqlViewEpochGraphTest : testing::Test {
  zigzag::ArenaManifold arena;
  VQLEngine engine{arena};
  zigzag::vortex::VortexVM vm{engine.core()};
  xanadu::vql::QueryExecutor executor{engine, vm};
  zigzag::CellRef source{};
  zigzag::CellRef handle{};

  void SetUp() override {
    const auto priority = engine.resolveDimension("d.priority");
    const auto high     = arena.makeCell("high");
    const auto urgent   = arena.makeCell("urgent");
    const auto home     = engine.core().home();
    zigzag::expectWritten(arena.link(home, priority, false, high));
    zigzag::expectWritten(arena.link(high, priority, false, urgent));
    source = arena.makeCell("task");
    zigzag::expectWritten(
        arena.link(source, priority, false, arena.makeCell("low")));
    engine.setVariable("task", source);
    // Names the view dimensions, so the programs below differ only in what is
    // under test.
    const auto view = engine.execute("$task{ d.1 <=> d.priority }");
    EXPECT_EQ(view.size(), 1u);
    handle = view.front();
    engine.setVariable("v", view);
    // The handle is also stored in the data, where a walk can reach it.
    zigzag::expectWritten(
        arena.link(source, engine.resolveDimension("d.held"), false, handle));
  }

  std::string textsOf(const std::string &query) {
    std::string joined;
    for (const auto cell : engine.execute(query))
      joined += (joined.empty() ? "" : ",") + arena.textOf(cell);
    return joined;
  }

  std::string refusalOf(const std::string &query) {
    try {
      (void)engine.execute(query);
    } catch (const std::exception &error) {
      return error.what();
    }
    ADD_FAILURE() << "expected a refusal: " << query;
    return {};
  }

  /// Repoints one of the handle's boxes at @p number, as a handle made over a
  /// cell that was released and whose number was handed out again.
  void repoint(const std::string &dimension, const std::int64_t number) {
    const auto box = arena.linked(handle, engine.resolveDimension(dimension),
                                  zigzag::DimVector::POS);
    ASSERT_TRUE(box.has_value());
    ASSERT_TRUE(engine.core()
                    .writeValue(*box, 0, std::nullopt,
                                zigzag::vortex::CellValue{number})
                    .has_value());
  }

  /// A number a released cell held and a later cell now holds.
  std::int64_t reusedNumber() {
    const auto mark  = arena.mark();
    const auto freed = static_cast<std::int64_t>(arena.makeCell("short"));
    EXPECT_TRUE(arena.release(mark).has_value());
    EXPECT_EQ(static_cast<std::int64_t>(arena.makeCell("reused")), freed);
    return freed;
  }

  /// A number no cell holds and the running query will not mint into: a
  /// number that was released and not reused would be handed out again by the
  /// query's own cells, which is the reuse case.
  static std::int64_t absentNumber() {
    return static_cast<std::int64_t>(zigzag::ArenaManifold::refOf(1U << 20));
  }
};

constexpr const char *sourceReused = "source cell was released and its number "
                                     "reused";
constexpr const char *viewReused   = "view cell was released and its number "
                                     "reused";

// A handle named by a variable is seen while compiling and refused where a def
// or a for would carry it away. One reached by walking data (a handle stored
// in a cell, found at run time) is a run-time fact, and only the graph sees it.
const std::vector<std::pair<const char *, const char *>> blindRoutes{
    {"step", "$task/+d.held/+d.1/value()"},
    {"def argument", "def f($c) := $c/+d.1/value(); f($task/+d.held)"},
    {"for source", "for $c in $task/+d.held return $c/+d.1/value()"},
};

TEST_F(VqlViewEpochGraphTest, AReusedSourceNumberRefusesOnEveryBlindRoute) {
  repoint("d.vql.view", reusedNumber());
  for (const auto &[route, query] : blindRoutes)
    EXPECT_THAT(refusalOf(query), testing::HasSubstr(sourceReused)) << route;
}

TEST_F(VqlViewEpochGraphTest, AReusedViewNumberRefusesOnEveryBlindRoute) {
  repoint("d.vql.of", reusedNumber());
  for (const auto &[route, query] : blindRoutes)
    EXPECT_THAT(refusalOf(query), testing::HasSubstr(viewReused)) << route;
}

TEST_F(VqlViewEpochGraphTest, ANumberNamingNoCellSaysSoOnEveryBlindRoute) {
  repoint("d.vql.view", absentNumber());
  for (const auto &[route, query] : blindRoutes) {
    const auto message = refusalOf(query);
    EXPECT_THAT(message, testing::HasSubstr("no longer a cell")) << route;
    EXPECT_THAT(message, testing::Not(testing::HasSubstr("reused"))) << route;
  }
}

TEST_F(VqlViewEpochGraphTest, ARefusalLeavesNoCellsOrMarks) {
  repoint("d.vql.view", reusedNumber());
  const auto cells = arena.cellCount();
  const auto marks = arena.outstandingMarks();
  (void)refusalOf("def f($c) := $c/+d.1/value(); f($v)");
  EXPECT_EQ(arena.cellCount(), cells);
  EXPECT_EQ(arena.outstandingMarks(), marks);
}

TEST_F(VqlViewEpochGraphTest, ALiveHandleStillStepsAcrossReleasesAboveIt) {
  for (int round = 0; round < 3; ++round) {
    const auto mark = arena.mark();
    arena.makeCell("scratch");
    ASSERT_TRUE(arena.release(mark).has_value());
  }
  for (const auto &[route, query] : blindRoutes)
    EXPECT_EQ(textsOf(query), "low") << route;
}

TEST_F(VqlViewEpochGraphTest, AHandleMintedByTheRunSteps) {
  // The wrap step writes the handle's witness, so a handle made by the
  // running program passes the same check as any other.
  EXPECT_EQ(textsOf("def f($c) := $c/+d.1/value(); "
                    "f(##{ d.1 <=> d.priority })"),
            "high,urgent");
  EXPECT_EQ(textsOf("for $c in ##{ d.1 <=> d.priority } return "
                    "$c/+d.1/+d.1/value()"),
            "urgent");
}

TEST_F(VqlViewEpochGraphTest, AHandleMintedByTheRunLeavesTheViewAlone) {
  // The wrap step writes nothing onto the view or the source: the in-graph
  // check is a read, so a view stepped inside the run leaves them as found.
  const auto before = arena.cellCount();
  (void)textsOf("def f($c) := $c/+d.1/value(); f($task{ d.1 <=> d.priority })");
  EXPECT_LE(arena.cellCount(), before);
}

constexpr const char *noWitness = "no valid epoch witness";

TEST_F(VqlViewEpochGraphTest, AHandleWithoutAWitnessRefusesByNameOnEveryRoute) {
  // Nothing says such a handle was made by the running program: a handle a
  // host linked into data, copied, or loaded carries no witness either, and
  // its number may name a cell that was released and minted again.
  const auto dims = *xanadu::vql::findViewDimensions(engine.core());
  ASSERT_TRUE(
      arena.unlink(handle, dims.epoch, zigzag::DimVector::POS).has_value());
  for (const auto &[route, query] : blindRoutes)
    EXPECT_THAT(refusalOf(query), testing::HasSubstr(noWitness)) << route;
}

TEST_F(VqlViewEpochGraphTest, AStaleHandleWithoutAWitnessIsRefusedNotAnswered) {
  const auto dims = *xanadu::vql::findViewDimensions(engine.core());
  ASSERT_TRUE(
      arena.unlink(handle, dims.epoch, zigzag::DimVector::POS).has_value());
  repoint("d.vql.view", reusedNumber());
  for (const auto &[route, query] : blindRoutes)
    EXPECT_THAT(refusalOf(query), testing::HasSubstr(noWitness)) << route;
}

TEST_F(VqlViewEpochGraphTest, ASecondWitnessMissingIsNotReadAsEpochZero) {
  const auto dims  = *xanadu::vql::findViewDimensions(engine.core());
  const auto first = arena.linked(handle, dims.epoch, zigzag::DimVector::POS);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(
      arena.unlink(*first, dims.epoch, zigzag::DimVector::POS).has_value());
  for (const auto &[route, query] : blindRoutes)
    EXPECT_THAT(refusalOf(query), testing::HasSubstr(noWitness)) << route;
}

TEST_F(VqlViewEpochGraphTest, AWitnessThatIsNotAnInt64IsInvalidNotReused) {
  const auto dims  = *xanadu::vql::findViewDimensions(engine.core());
  const auto first = arena.linked(handle, dims.epoch, zigzag::DimVector::POS);
  ASSERT_TRUE(first.has_value());
  for (const zigzag::vortex::CellValue &junk :
       {zigzag::vortex::CellValue{std::string{"junk"}},
        zigzag::vortex::CellValue{2.5}, zigzag::vortex::CellValue{true}}) {
    ASSERT_TRUE(engine.core().writeValue(*first, 0, std::nullopt, junk));
    for (const auto &[route, query] : blindRoutes) {
      const auto message = refusalOf(query);
      EXPECT_THAT(message, testing::HasSubstr(noWitness)) << route;
      EXPECT_THAT(message, testing::Not(testing::HasSubstr("reused"))) << route;
    }
  }
}

TEST_F(VqlViewEpochGraphTest, TheWitnessOfAMintedHandleIsItsCellsEpochs) {
  // A handle made in the graph carries the epochs its two boxes name, whether
  // it was wrapped in the program or its view was carried out of one.
  const auto dims  = *xanadu::vql::findViewDimensions(engine.core());
  const auto check = [&](const zigzag::CellRef minted, const char *what) {
    const auto parts = xanadu::vql::handleParts(arena, dims, minted);
    ASSERT_TRUE(parts.has_value()) << what;
    const auto first = arena.linked(minted, dims.epoch, zigzag::DimVector::POS);
    ASSERT_TRUE(first.has_value()) << what;
    const auto second =
        arena.linked(*first, dims.epoch, zigzag::DimVector::POS);
    ASSERT_TRUE(second.has_value()) << what;
    EXPECT_EQ(arena.asInt64(*first),
              std::optional{static_cast<std::int64_t>(
                  arena.epochOfCell(parts->source).value())})
        << what;
    EXPECT_EQ(arena.asInt64(*second),
              std::optional{static_cast<std::int64_t>(
                  arena.epochOfCell(parts->view).value())})
        << what;
  };
  const auto carried = engine.execute("##{ d.1 <=> d.priority }");
  ASSERT_EQ(carried.size(), 1u);
  check(carried.front(), "view carried out of its program");
  const auto wrapped = engine.execute("$task/+d.held/+d.1");
  ASSERT_EQ(wrapped.size(), 1u);
  check(wrapped.front(), "handle wrapped in the graph");
}

TEST_F(VqlViewEpochGraphTest, WrappingAHandleWritesNoOlderCell) {
  const auto dims  = *xanadu::vql::findViewDimensions(engine.core());
  const auto parts = xanadu::vql::handleParts(arena, dims, handle);
  ASSERT_TRUE(parts.has_value());
  const auto edges = [&](const zigzag::CellRef cell) {
    return arena.dimensionsOf(cell).size();
  };
  const auto viewEdges   = edges(parts->view);
  const auto sourceEdges = edges(parts->source);
  const auto before      = std::pair{arena.cellCount(), arena.deadLinks()};
  const auto read        = executor.execute("$task/+d.held/+d.1/+d.1/value()");
  ASSERT_TRUE(read.has_value()) << read.error().message;
  EXPECT_FALSE(executor.programRetainedBecause());
  EXPECT_EQ(edges(parts->view), viewEdges);
  EXPECT_EQ(edges(parts->source), sourceEdges);
  EXPECT_EQ(std::pair(arena.cellCount(), arena.deadLinks()), before);
}

} // namespace

// V12: a dynamic view is data. A view is a cell with its rules as cells; a
// handle is a cell naming its source and its view as Int64 ref-boxes; a step
// on a handle is a guarded fold over those cells in the graph. So a view can
// be taken over a source known only at run time (the home cell, a let-bound
// path, a function's argument), and the compile-time handle table no longer
// holds views.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <set>
#include <string>
#include <vector>

#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using xanadu::vql::VQLEngine;
using zigzag::vortex::OpcodeKind;

struct VQLDynamicViewRuntimeTest : testing::Test {
  zigzag::ArenaManifold arena;
  VQLEngine engine{arena};
  zigzag::vortex::VortexVM vm{engine.core()};
  xanadu::vql::QueryExecutor executor{engine, vm};
  zigzag::CellRef source{};

  void SetUp() override {
    const auto priority = engine.resolveDimension("d.priority");
    const auto assignee = engine.resolveDimension("d.assignee");
    const auto high     = arena.makeCell("high");
    const auto urgent   = arena.makeCell("urgent");
    // The home cell is the source `##` names; it is not a session variable,
    // so its view is over a source the compiler cannot know.
    const auto home = engine.core().home();
    zigzag::expectWritten(arena.link(home, priority, false, high));
    zigzag::expectWritten(arena.link(high, priority, false, urgent));
    zigzag::expectWritten(
        arena.link(home, assignee, false, arena.makeCell("Alice")));
    source = arena.makeCell("task");
    zigzag::expectWritten(
        arena.link(source, priority, false, arena.makeCell("low")));
    engine.setVariable("task", source);
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

  struct Footprint {
    std::size_t cells;
    std::size_t links;
    std::size_t trail;
    std::uint32_t marks;
    std::size_t handles;
    bool operator==(const Footprint &) const = default;
  };
  [[nodiscard]] Footprint footprint() const {
    return {arena.cellCount(), arena.deadLinks(), arena.trailSize(),
            arena.outstandingMarks(), engine.shapeCount()};
  }

  void expectRefusal(const std::string &query, const std::string &fragment) {
    const auto before = footprint();
    EXPECT_THAT(refusalOf(query), testing::HasSubstr(fragment)) << query;
    EXPECT_EQ(footprint(), before) << query;
  }

  /// Names the view dimensions once, so a later program neither retains
  /// itself for minting them nor differs in what it emits.
  void warm() { (void)engine.execute("$task{ d.1 <=> d.priority }"); }
};

TEST_F(VQLDynamicViewRuntimeTest, AViewOverTheHomeCellSteps) {
  EXPECT_EQ(textsOf("##{ d.1 <=> d.priority }/+d.1/value()"), "high,urgent");
  EXPECT_EQ(textsOf("##{ d.1 <=> d.priority }/+d.assignee/value()"), "Alice");
}

TEST_F(VQLDynamicViewRuntimeTest, AViewOverALetBoundPathSteps) {
  EXPECT_EQ(textsOf("let $t := $task/+d.priority return "
                    "$t{ d.1 <=> d.priority }/+d.1/value()"),
            "");
  // `high` has `urgent` behind it on d.priority; the pivot reads it as d.1.
  EXPECT_EQ(textsOf("let $h := ##/+d.priority return $h{ d.1 <=> d.priority }"
                    "/+d.1/value()"),
            "urgent");
}

TEST_F(VQLDynamicViewRuntimeTest, ARunTimeViewBoundInOneQueryStepsInAnother) {
  const auto view = engine.execute("##{ d.1 <=> d.priority }");
  ASSERT_EQ(view.size(), 1u);
  EXPECT_EQ(arena.textOf(view.front()), arena.textOf(engine.core().home()));
  engine.setVariable("v", view);
  EXPECT_EQ(textsOf("$v/+d.1/value()"), "high,urgent");
  EXPECT_EQ(textsOf("$v/+d.1/+d.1/value()"), "urgent");
}

TEST_F(VQLDynamicViewRuntimeTest, AViewProjectedInsideADefSteps) {
  EXPECT_EQ(
      textsOf("def v($s) := $s{ d.1 <=> d.priority }; v($task)/+d.1/value()"),
      "low");
  EXPECT_EQ(textsOf("def v($s) := $s{ d.1 <=> d.priority }; "
                    "let $w := v(##) return $w/+d.1/value()"),
            "high,urgent");
}

TEST_F(VQLDynamicViewRuntimeTest, ADefStepsAHandleItIsHandedAtRunTime) {
  // The shared fold is called from a function body as from the query's own.
  EXPECT_EQ(textsOf("def f($c) := $c/+d.1/value(); "
                    "f(##{ d.1 <=> d.priority })"),
            "high,urgent");
}

TEST_F(VQLDynamicViewRuntimeTest, AChainThroughTwoStepsKeepsTheView) {
  EXPECT_EQ(textsOf("##{ d.1 <=> d.priority }/+d.1/+d.1/value()"), "urgent");
  // A third step finds nothing behind `urgent`; it must not fall back to d.1.
  EXPECT_EQ(textsOf("##{ d.1 <=> d.priority }/+d.1/+d.1/+d.1/value()"), "");
}

TEST_F(VQLDynamicViewRuntimeTest, MaskWildcardAndPivotRulesFoldInSourceOrder) {
  EXPECT_EQ(textsOf("##{ !*, d.1 <=> d.priority }/+d.1/value()"),
            "high,urgent");
  EXPECT_EQ(textsOf("##{ !*, d.1 <=> d.priority }/+d.assignee/value()"), "");
  EXPECT_EQ(textsOf("##{ *, d.1 <=> d.priority, !+d.assignee }"
                    "/+d.assignee/value()"),
            "");
  EXPECT_EQ(textsOf("##{ *, d.1 <=> d.priority, !+d.assignee }"
                    "/+d.priority/value()"),
            "high,urgent");
  // A positive mask rule hides every dimension it does not name.
  EXPECT_EQ(textsOf("##{ +d.assignee, d.1 <=> d.priority }"
                    "/+d.assignee/value()"),
            "Alice");
  EXPECT_EQ(textsOf("##{ +d.assignee, d.1 <=> d.priority }"
                    "/+d.priority/value()"),
            "");
}

TEST_F(VQLDynamicViewRuntimeTest, AHiddenStepAnswersNothingAndCreatesNothing) {
  warm();
  const auto before = arena.cellCount();
  EXPECT_EQ(textsOf("##{ !*, d.1 <=> d.fresh }/+d.assignee/value()"), "");
  EXPECT_TRUE(
      engine.execute("##{ !*, d.1 <=> d.fresh }/+d.assignee%\"x\"").empty());
  EXPECT_EQ(textsOf("##/+d.assignee/value()"), "Alice");
  EXPECT_GE(arena.cellCount(), before);
}

TEST_F(VQLDynamicViewRuntimeTest,
       ACreationThroughARunTimeViewReachesTheSource) {
  (void)engine.execute("##/+d.fresh/value()");
  const auto made = engine.execute("##{ d.1 <=> d.fresh }/+d.1%\"made\"");
  ASSERT_EQ(made.size(), 1u);
  EXPECT_EQ(arena.textOf(made.front()), "made");
  EXPECT_EQ(textsOf("##/+d.fresh/value()"), "made");
  EXPECT_FALSE(arena.linkedMinting(engine.core().home(),
                                   engine.resolveDimension("d.1"),
                                   zigzag::DimVector::POS));
}

TEST_F(VQLDynamicViewRuntimeTest, APlainCellStillStepsWhenViewsExist) {
  warm();
  // Variable-rooted streams carry the guard once the session has views; a
  // plain cell takes the unguarded branch and answers as it always did.
  EXPECT_EQ(textsOf("$task/+d.priority/value()"), "low");
  EXPECT_EQ(textsOf("##/+d.priority/value()"), "high,urgent");
  EXPECT_EQ(textsOf("for $c in ##/+d.priority return $c/+d.priority/value()"),
            "urgent");
}

TEST_F(VQLDynamicViewRuntimeTest, APlainAndAViewCellMixInOneStream) {
  const auto view = engine.execute("##{ d.1 <=> d.priority }");
  ASSERT_EQ(view.size(), 1u);
  engine.setVariable("v", view);
  engine.setVariable("both", {source, view.front()});
  // Each element is read by what it is: the plain cell by its own d.priority,
  // the handle through its pivot.
  EXPECT_EQ(textsOf("$both/+d.priority/value()"), "low,high,urgent");
  EXPECT_EQ(textsOf("$both/+d.1/value()"), "high,urgent");
}

TEST_F(VQLDynamicViewRuntimeTest, AHiddenFormOnAHandleRefusesByName) {
  warm();
  const auto view = engine.execute("##{ d.1 <=> d.priority }");
  engine.setVariable("v", view);
  expectRefusal("$v/+d.1[./value() = \"high\"]", "predicate");
  // first() is not compiled to Vortex at all, so its own refusal would come
  // before the view's; a user function is compiled for plain cells and is
  // what refuses here.
  expectRefusal("def f($x) := 1; $v/+d.1/f()", "function f()");
  expectRefusal("$v/+d.1!keep", "yield mode");
  expectRefusal("$v{*}", "dynamic view handle");
  expectRefusal("$v{ d.2 <=> d.assignee }", "dynamic view handle");
  expectRefusal("let $s := $v + 1 return $s", "dynamic view");
}

TEST_F(VQLDynamicViewRuntimeTest, AHandleIsNeitherALensNorAClosure) {
  warm();
  engine.setVariable("v", engine.execute("$task{ d.1 <=> d.priority }"));
  expectRefusal("$task/$v", "dynamic view handle");
  expectRefusal("let $s := $v return $s{ d.2 <=> d.priority }",
                "dynamic view handle");
  expectRefusal("let $s := { *, $v } return $task{ $s }", "dynamic view");
}

TEST_F(VQLDynamicViewRuntimeTest, AHandleOfAnotherEngineStepsLikeItsOwn) {
  VQLEngine other{engine.core()};
  other.setVariable("task", source);
  const auto view = other.execute("$task{ d.1 <=> d.priority }");
  ASSERT_EQ(view.size(), 1u);
  engine.setVariable("v", view);
  EXPECT_EQ(textsOf("$v/+d.1/value()"), "low");
  // It is still not a loop source or an operand: those sites refuse by name.
  expectRefusal("for $c in $v return $c", "dynamic view handle");
}

TEST_F(VQLDynamicViewRuntimeTest, AHandleHeldAfterClearVariablesStillSteps) {
  const auto view = engine.execute("$task{ d.1 <=> d.priority }");
  ASSERT_EQ(view.size(), 1u);
  engine.clearVariables();
  engine.setVariable("v", view);
  EXPECT_EQ(textsOf("$v/+d.1/value()"), "low");
}

TEST_F(VQLDynamicViewRuntimeTest,
       AComputedRuleOverARunTimeSourceRefusesByName) {
  expectRefusal("##{ +d.label := ./value() }", "source computed at run time");
  expectRefusal("let $t := $task/+d.priority return "
                "$t{ d.1 <=> d.x, +d.label := ./value() }",
                "source computed at run time");
}

TEST_F(VQLDynamicViewRuntimeTest, AComputedHandleOfAnotherEngineRefusesByName) {
  VQLEngine other{engine.core()};
  other.setVariable("task", source);
  const auto view = other.execute("$task{ +d.label := ./value() }");
  ASSERT_EQ(view.size(), 1u);
  engine.setVariable("v", view);
  // The expression lives in the other engine's compile-time table, so this
  // engine can read the view's other rules but not evaluate that one.
  expectRefusal("$v/+d.label/value()", "dynamic view handle");
}

// -- program release ---------------------------------------------------------

TEST_F(VQLDynamicViewRuntimeTest, AViewOverAnOlderSourceReleasesTheProgram) {
  warm();
  const auto first = executor.execute("##{ d.1 <=> d.priority }");
  ASSERT_TRUE(first) << first.error().message;
  EXPECT_FALSE(executor.programRetainedBecause())
      << *executor.programRetainedBecause();
  // The handle is a copy the session keeps; stepping it needs the copy's view
  // and not the released program's.
  engine.setVariable("v", *first);
  const auto read = executor.execute("$v/+d.1/value()");
  ASSERT_TRUE(read) << read.error().message;
  EXPECT_FALSE(executor.programRetainedBecause());
  ASSERT_EQ(read->size(), 2u);
  EXPECT_EQ(arena.textOf(read->front()), "high");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST_F(VQLDynamicViewRuntimeTest, AViewRetainedWithItsProgramStillSteps) {
  // The first run names the view dimensions, which the session keeps, so it
  // retains its program and the handle's boxes still name live cells.
  const auto first = executor.execute("##{ d.1 <=> d.priority }");
  ASSERT_TRUE(first) << first.error().message;
  ASSERT_TRUE(executor.programRetainedBecause());
  engine.setVariable("v", *first);
  EXPECT_EQ(textsOf("$v/+d.1/value()"), "high,urgent");
}

TEST_F(VQLDynamicViewRuntimeTest, ASourceTheProgramMintedKeepsTheProgram) {
  warm();
  // count() answers a fresh cell the program minted; a handle over it would
  // name a cell the release is about to take.
  const auto made =
      executor.execute("##/+d.priority/count(){ d.1 <=> d.priority }");
  ASSERT_TRUE(made) << made.error().message;
  ASSERT_EQ(made->size(), 1u);
  ASSERT_TRUE(executor.programRetainedBecause());
  EXPECT_THAT(*executor.programRetainedBecause(),
              testing::HasSubstr("dynamic view handle"));
  EXPECT_THAT(*executor.programRetainedBecause(), testing::HasSubstr("minted"));
  engine.setVariable("v", *made);
  EXPECT_TRUE(arena.contains(made->front()));
  EXPECT_TRUE(engine.execute("$v/+d.1/value()").empty());
}

TEST_F(VQLDynamicViewRuntimeTest, SteppingAStoredViewWritesNoOlderCell) {
  warm();
  const auto view = executor.execute("$task{ d.1 <=> d.priority }");
  ASSERT_TRUE(view) << view.error().message;
  engine.setVariable("v", *view);
  const auto before = footprint();
  const auto read   = executor.execute("$v/+d.1/value()");
  ASSERT_TRUE(read) << read.error().message;
  // An older-cell write retains the program with this reason; there is none,
  // so the source gained no handles rank, no marker and no link of any kind.
  EXPECT_FALSE(executor.programRetainedBecause());
  EXPECT_EQ(footprint(), before);
  EXPECT_FALSE(arena.linkedMinting(
      source, engine.resolveDimension("d.vql.view"), zigzag::DimVector::POS));
  EXPECT_FALSE(arena.linkedMinting(source, engine.resolveDimension("d.vql.of"),
                                   zigzag::DimVector::POS));
  EXPECT_FALSE(arena.linkedMinting(source,
                                   engine.resolveDimension("d.vql.handles"),
                                   zigzag::DimVector::POS));
}

TEST_F(VQLDynamicViewRuntimeTest, AFailedQueryLeavesNoCellsOrMarks) {
  warm();
  const auto view = engine.execute("##{ d.1 <=> d.priority }");
  engine.setVariable("v", view);
  const auto before = footprint();
  EXPECT_THAT(refusalOf("$v/+d.1[./value() = \"high\"]"),
              testing::HasSubstr("predicate"));
  EXPECT_THAT(refusalOf("def f($x) := 1; ##{ d.1 <=> d.priority }/+d.1/f()"),
              testing::HasSubstr("function f()"));
  EXPECT_EQ(footprint(), before);
}

TEST_F(VQLDynamicViewRuntimeTest, ACompactionBetweenQueriesKeepsAHandleValid) {
  warm();
  const auto view = engine.execute("##{ d.1 <=> d.priority }");
  engine.setVariable("v", view);
  ASSERT_TRUE(arena.compact().has_value());
  EXPECT_EQ(textsOf("$v/+d.1/value()"), "high,urgent");
}

// -- guards -------------------------------------------------------------------

TEST_F(VQLDynamicViewRuntimeTest, NoGuardIsEmittedWhileNoViewWasEverMade) {
  const auto read = executor.execute("$task/+d.priority/value()");
  ASSERT_TRUE(read) << read.error().message;
  EXPECT_THAT(std::string{executor.disassembly()},
              testing::Not(testing::HasSubstr("VIEW_")));
}

TEST_F(VQLDynamicViewRuntimeTest, AStepBeforeTheFirstViewOfAQueryIsGuarded) {
  // The first statement is compiled before anything names the view
  // dimensions, yet it runs before a handle of the second exists only by
  // luck of order; the compile names them first so it is guarded all the same.
  const auto both = executor.execute(
      "$task/+d.priority/value(); ##{ d.1 <=> d.priority }/+d.1/value()");
  ASSERT_TRUE(both) << both.error().message;
  EXPECT_THAT(std::string{executor.disassembly()},
              testing::HasSubstr("VIEW_IS_PLAIN"));
  ASSERT_EQ(both->size(), 2u);
  EXPECT_EQ(arena.textOf(both->front()), "high");
}

TEST_F(VQLDynamicViewRuntimeTest, ManyHandlesOverAProgramMintedViewSettle) {
  // Settle collects the views of the handles it carries while it walks them;
  // enough handles to make that vector grow more than once, each over the one
  // program-minted view.
  constexpr int reached = 40;
  const auto home       = engine.core().home();
  const auto priority   = engine.resolveDimension("d.many");
  auto tail             = home;
  for (int i = 0; i < reached; ++i) {
    const auto next = arena.makeCell("n" + std::to_string(i));
    zigzag::expectWritten(arena.link(tail, priority, false, next));
    tail = next;
  }
  warm();
  const auto handles = executor.execute("##{ d.1 <=> d.many }/+d.1");
  ASSERT_TRUE(handles) << handles.error().message;
  ASSERT_EQ(handles->size(), static_cast<std::size_t>(reached));
  EXPECT_FALSE(executor.programRetainedBecause());
  engine.setVariable("h", *handles);
  // Each handle kept its view: the second step reads one cell further on.
  EXPECT_EQ(engine.execute("$h/+d.1/value()").size(),
            static_cast<std::size_t>(reached - 1) * reached / 2);
}

TEST_F(VQLDynamicViewRuntimeTest, TwoReachesOfOneSourceAreTwoHandles) {
  // A handle is minted per reach and no VQL operation exposes cell identity,
  // so two reaches of the same source being two cells is not observable as a
  // wrong answer; equality of reaches is not claimed. This records the shape
  // so a change to sharing handles is a decision and not an accident.
  const auto view = engine.execute("##{ d.1 <=> d.priority }");
  ASSERT_EQ(view.size(), 1u);
  engine.setVariable("two", {view.front(), view.front()});
  const auto reached = engine.execute("$two/+d.1");
  ASSERT_EQ(reached.size(), 4u);
  std::set<zigzag::CellRef> distinct(reached.begin(), reached.end());
  EXPECT_EQ(distinct.size(), 4u);
  EXPECT_EQ(textsOf("$two/+d.1/value()"), "high,urgent,high,urgent");
}

TEST_F(VQLDynamicViewRuntimeTest, AHandleOfASeparateCoreRefusesByName) {
  const auto view = engine.execute("$task{ d.1 <=> d.priority }");
  ASSERT_EQ(view.size(), 1u);
  // Its own dimension directory knows nothing of the marker; the handle is
  // recognised by the marker's name on the handle's own links, so it is never
  // walked as a plain cell.
  VQLEngine other{arena};
  other.setVariable("v", view);
  other.setVariable("task", source);
  const auto before = footprint();
  std::string message;
  try {
    (void)other.execute("$v/+d.1/value()");
  } catch (const std::exception &error) {
    message = error.what();
  }
  EXPECT_THAT(message, testing::HasSubstr("dynamic view handle"));
  EXPECT_THAT(message, testing::HasSubstr("another core"));
  EXPECT_EQ(footprint(), before);
  EXPECT_THAT(std::string{[&] {
                try {
                  (void)other.execute("for $c in $v return $c");
                } catch (const std::exception &error) {
                  return std::string{error.what()};
                }
                return std::string{};
              }()},
              testing::HasSubstr("dynamic view handle"));
}

TEST_F(VQLDynamicViewRuntimeTest, TheStepProgramGrowsByABoundedAmountPerStep) {
  warm();
  engine.setVariable("v", engine.execute("$task{ d.1 <=> d.priority }"));
  const auto opsFor = [&](const int steps) {
    std::string query = "$v";
    for (int i = 0; i < steps; ++i) query += "/+d.1";
    query += "/value()";
    xanadu::vql::VQLCompiler compiler(engine.coordinator(), vm);
    xanadu::vql::CompilationOptions options;
    options.initialBindings = engine.variables();
    const auto result       = compiler.compile(query, options);
    EXPECT_TRUE(result.success) << result.errorMessage;
    return result.generatedOpcodes.size();
  };
  // The fold, the handle wrap and the handle's resolution are subroutines
  // built once per program. What a site still emits is the map harness, its
  // dispatch, the plain branch (the step as it always compiled, about the
  // size of a step without views), its arguments and calls, and the walk that
  // wraps what is reached. Measured 57 per site; the bound leaves a little
  // room for the plain branch to grow. Before the routines were shared a
  // site cost 102. The in-graph epoch check of a handle (EpochOfCell of the
  // source and the view against the recorded witness) is part of the shared
  // resolve routine: about a dozen ops once per program, none per site, so
  // 57 stands (re-measured with the check in place: 57 at 2, 3 and 8 steps).
  constexpr std::size_t maxOpsPerStep = 60;
  const auto one                      = opsFor(1);
  std::size_t previous                = one;
  for (const int steps : {2, 3, 8}) {
    const auto now = opsFor(steps);
    const auto perStep =
        (now - previous) / static_cast<std::size_t>(steps == 8 ? 5 : 1);
    EXPECT_LE(perStep, maxOpsPerStep) << steps << " steps: " << now;
    previous = now;
  }
}

// -- stale numbers
// --------------------------------------------------------------

namespace {
/// Repoints a box at a cell number the arena has since reused, as a handle
/// made over a cell that was released before the number was handed out again.
void repoint(zigzag::ArenaManifold &arena, VQLEngine &engine,
             const zigzag::CellRef handle, const std::string &dimension,
             const std::int64_t number) {
  const auto box = arena.linked(handle, engine.resolveDimension(dimension),
                                zigzag::DimVector::POS);
  ASSERT_TRUE(box.has_value());
  ASSERT_TRUE(
      engine.core()
          .writeValue(*box, 0, std::nullopt, zigzag::vortex::CellValue{number})
          .has_value());
}
} // namespace

TEST_F(VQLDynamicViewRuntimeTest, ALiteralIdNamingAHandleStepsIt) {
  const auto view = engine.execute("##{ d.1 <=> d.priority }");
  ASSERT_EQ(view.size(), 1u);
  const auto id = "#" + std::to_string(view.front());
  EXPECT_EQ(textsOf(id + "/+d.1/value()"), "high,urgent");
  EXPECT_EQ(textsOf(id + "/+d.1/+d.1/value()"), "urgent");
}

TEST_F(VQLDynamicViewRuntimeTest, AStaleHandleNamedByIdRefuses) {
  warm();
  const auto view = engine.execute("$task{ d.1 <=> d.priority }");
  ASSERT_EQ(view.size(), 1u);
  const auto mark  = arena.mark();
  const auto freed = static_cast<std::int64_t>(arena.makeCell("short lived"));
  ASSERT_TRUE(arena.release(mark).has_value());
  ASSERT_EQ(static_cast<std::int64_t>(arena.makeCell("reused")), freed);
  repoint(arena, engine, view.front(), "d.vql.view", freed);
  EXPECT_THAT(refusalOf("#" + std::to_string(view.front()) + "/+d.1/value()"),
              testing::HasSubstr("number reused"));
}

TEST_F(VQLDynamicViewRuntimeTest, AForeignHandleNamedByIdRefuses) {
  const auto view = engine.execute("$task{ d.1 <=> d.priority }");
  ASSERT_EQ(view.size(), 1u);
  VQLEngine other{arena};
  std::string message;
  try {
    (void)other.execute("#" + std::to_string(view.front()) + "/+d.1/value()");
  } catch (const std::exception &error) {
    message = error.what();
  }
  EXPECT_THAT(message, testing::HasSubstr("another core"));
}

TEST_F(VQLDynamicViewRuntimeTest, ANumberNamingNoCellSaysSo) {
  warm();
  const auto view = engine.execute("$task{ d.1 <=> d.priority }");
  ASSERT_EQ(view.size(), 1u);
  const auto mark  = arena.mark();
  const auto freed = static_cast<std::int64_t>(arena.makeCell("short lived"));
  ASSERT_TRUE(arena.release(mark).has_value());
  repoint(arena, engine, view.front(), "d.vql.view", freed);
  engine.setVariable("v", view);
  const auto message = refusalOf("$v/+d.1/value()");
  EXPECT_THAT(message, testing::HasSubstr("no longer a cell"));
  EXPECT_THAT(message, testing::Not(testing::HasSubstr("number reused")));
}

TEST_F(VQLDynamicViewRuntimeTest, AHandleWithoutAValidWitnessRefuses) {
  warm();
  const auto view = engine.execute("$task{ d.1 <=> d.priority }");
  ASSERT_EQ(view.size(), 1u);
  const auto box =
      arena.linked(view.front(), engine.resolveDimension("d.vql.of"),
                   zigzag::DimVector::POS);
  ASSERT_TRUE(box.has_value());
  // A forged handle: both boxes, no witness.
  const auto forged = arena.makeCell("forged");
  zigzag::expectWritten(
      arena.link(forged, engine.resolveDimension("d.vql.view"), false,
                 arena.makeScalarCell(static_cast<std::int64_t>(source))));
  zigzag::expectWritten(arena.link(forged, engine.resolveDimension("d.vql.of"),
                                   false,
                                   arena.makeScalarCell(*arena.asInt64(*box))));
  engine.setVariable("f", forged);
  EXPECT_THAT(refusalOf("$f/+d.1/value()"),
              testing::HasSubstr("epoch witness"));
  // A real handle whose witness was overwritten with text.
  const auto witness =
      arena.linked(view.front(), engine.resolveDimension("d.vql.epoch"),
                   zigzag::DimVector::POS);
  ASSERT_TRUE(witness.has_value());
  ASSERT_TRUE(engine.core()
                  .writeValue(*witness, 0, std::nullopt,
                              zigzag::vortex::CellValue{std::string{"x"}})
                  .has_value());
  engine.setVariable("v", view);
  EXPECT_THAT(refusalOf("$v/+d.1/value()"),
              testing::HasSubstr("epoch witness"));
}

TEST_F(VQLDynamicViewRuntimeTest, AReusedSourceNumberRefusesByName) {
  // A handle names its source by number and records, beside it, the epoch the
  // source was minted in. A number reused by a later cell answers another
  // epoch, so the step refuses instead of reading the reusing cell.
  warm();
  const auto view = engine.execute("$task{ d.1 <=> d.priority }");
  ASSERT_EQ(view.size(), 1u);
  const auto mark  = arena.mark();
  const auto gone  = arena.makeCell("minted after the mark");
  const auto freed = static_cast<std::int64_t>(gone);
  ASSERT_TRUE(arena.release(mark).has_value());
  const auto reused = arena.makeCell("reused");
  ASSERT_EQ(static_cast<std::int64_t>(reused), freed);
  repoint(arena, engine, view.front(), "d.vql.view", freed);
  engine.setVariable("v", view);
  EXPECT_THAT(refusalOf("$v/+d.1/value()"),
              testing::HasSubstr("source cell was released and its number "
                                 "reused"));
}

TEST_F(VQLDynamicViewRuntimeTest, AReusedViewNumberRefusesByName) {
  warm();
  const auto view = engine.execute("$task{ d.1 <=> d.priority }");
  ASSERT_EQ(view.size(), 1u);
  const auto mark  = arena.mark();
  const auto gone  = arena.makeCell("minted after the mark");
  const auto freed = static_cast<std::int64_t>(gone);
  ASSERT_TRUE(arena.release(mark).has_value());
  ASSERT_EQ(static_cast<std::int64_t>(arena.makeCell("reused")), freed);
  repoint(arena, engine, view.front(), "d.vql.of", freed);
  engine.setVariable("v", view);
  EXPECT_THAT(refusalOf("$v/+d.1/value()"),
              testing::HasSubstr("view cell was released and its number "
                                 "reused"));
}

TEST_F(VQLDynamicViewRuntimeTest, AHandleSurvivesReleasesAboveItAndAClear) {
  warm();
  const auto view = engine.execute("$task{ d.1 <=> d.priority }");
  ASSERT_EQ(view.size(), 1u);
  for (int round = 0; round < 3; ++round) {
    const auto mark = arena.mark();
    arena.makeCell("scratch");
    ASSERT_TRUE(arena.release(mark).has_value());
  }
  engine.clearVariables();
  engine.setVariable("v", view);
  EXPECT_EQ(textsOf("$v/+d.1/value()"), "low");
}

TEST_F(VQLDynamicViewRuntimeTest, ARunTimeViewOfTheHomeCellKeepsItsWitness) {
  const auto view = engine.execute("##{ d.1 <=> d.priority }");
  ASSERT_EQ(view.size(), 1u);
  engine.setVariable("v", view);
  EXPECT_EQ(textsOf("$v/+d.1/value()"), "high,urgent");
  const auto mark = arena.mark();
  arena.makeCell("scratch");
  ASSERT_TRUE(arena.release(mark).has_value());
  EXPECT_EQ(textsOf("$v/+d.1/value()"), "high,urgent");
}

// -- the shape of what is emitted -------------------------------------------

std::size_t countOps(const xanadu::vql::CompilationResult &result,
                     zigzag::vortex::VortexVM &vm, OpcodeKind kind) {
  return static_cast<std::size_t>(
      std::ranges::count_if(result.generatedOpcodes, [&](auto op) {
        return vm.getOpcodeKind(op) == kind;
      }));
}

TEST_F(VQLDynamicViewRuntimeTest, TheFoldIsBuiltOncePerProgram) {
  warm();
  const auto view = engine.execute("##{ d.1 <=> d.priority }");
  engine.setVariable("v", view);
  const auto compile = [&](const std::string &query) {
    xanadu::vql::VQLCompiler compiler(engine.coordinator(), vm);
    xanadu::vql::CompilationOptions options;
    options.initialBindings = engine.variables();
    auto result             = compiler.compile(query, options);
    EXPECT_TRUE(result.success) << result.errorMessage;
    return result;
  };
  const auto one   = compile("$v/+d.1/value()");
  const auto three = compile("$v/+d.1/+d.1/+d.1/value()");
  const auto dims1 = countOps(one, vm, OpcodeKind::DimByName);
  const auto dims3 = countOps(three, vm, OpcodeKind::DimByName);
  // The fold is one subroutine per program, so a step site resolves no
  // dimension of its own: the step's dimension, a rule's name and a pivot's
  // target are looked up inside the shared fold, whatever the number of sites.
  EXPECT_EQ(dims3, dims1) << dims1 << " " << dims3;
}

TEST_F(VQLDynamicViewRuntimeTest, RuleCountDoesNotChangeTheStepProgram) {
  warm();
  const auto compile = [&](const std::string &query) {
    xanadu::vql::VQLCompiler compiler(engine.coordinator(), vm);
    xanadu::vql::CompilationOptions options;
    options.initialBindings = engine.variables();
    auto result             = compiler.compile(query, options);
    EXPECT_TRUE(result.success) << result.errorMessage;
    return result.generatedOpcodes.size();
  };
  engine.setVariable("one", engine.execute("$task{ d.1 <=> d.priority }"));
  engine.setVariable(
      "many", engine.execute("$task{ d.1 <=> d.priority, d.2 <=> d.assignee, "
                             "d.3 <=> d.priority, !+d.x, +d.y }"));
  EXPECT_EQ(compile("$one/+d.1/value()"), compile("$many/+d.1/value()"));
}

} // namespace

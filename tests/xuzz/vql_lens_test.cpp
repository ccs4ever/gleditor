// Q-LENS: lens(get:, put:) lowers to a Vortex graph. A lens is a pair of
// callbacks the compiler keeps under a handle cell; binding one runs neither,
// and the step that applies it compiles the callback it needs.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using xanadu::vql::VQLEngine;

struct VQLLensTest : testing::Test {
  zigzag::ArenaManifold arena;
  VQLEngine engine{arena};

  [[nodiscard]] std::string textOf(const std::string &query) {
    const auto cells = engine.execute(query);
    EXPECT_EQ(cells.size(), 1u) << query;
    return cells.empty() ? std::string{} : arena.textOf(cells.front());
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

  /// What a failed query must leave exactly as it found it.
  struct Footprint {
    std::size_t cells;
    std::size_t links;
    std::size_t trail;
    std::uint32_t marks;
    std::size_t lenses;
    bool operator==(const Footprint &) const = default;
  };
  [[nodiscard]] Footprint footprint() const {
    return {arena.cellCount(), arena.deadLinks(), arena.trailSize(),
            arena.outstandingMarks(), engine.shapeCount()};
  }
};

constexpr const char *valueLens = R"(
  let $editable := lens(
    get: \(.) ./value(),
    put: \(cell, val) $cell/value($val)
  )
  return $editable
)";

TEST_F(VQLLensTest, GetOnlyLensReadsAndRefusesToWrite) {
  engine.setVariable("item", arena.makeCell("before"));
  (void)engine.execute(R"(let $r := lens(get: \(.) ./value()) return $r)");
  EXPECT_EQ(textOf("$item/$r"), "before");
  EXPECT_THAT(refusalOf("weave { $item/$r%\"x\" }"),
              testing::HasSubstr("no put: callback"));
  EXPECT_EQ(textOf("$item/value()"), "before");
}

TEST_F(VQLLensTest, PutWithACallbackThatWritesSeveralCells) {
  const auto item = arena.makeCell("old");
  engine.setVariable("item", item);
  (void)engine.execute(R"(
    let $both := lens(
      get: \(.) ./value(),
      put: \(cell, val) $cell/value($val)/+d.note%"touched"
    )
    return $both
  )");
  engine.execute("weave { $item/$both%\"new\" }");
  EXPECT_EQ(textOf("$item/value()"), "new");
  EXPECT_EQ(textOf("$item/+d.note/value()"), "touched");
}

TEST_F(VQLLensTest, PutReceivesEveryReplacementInOrder) {
  engine.setVariable("item", arena.makeCell("0"));
  (void)engine.execute(valueLens);
  engine.execute("weave { $item/$editable%\"1\"%\"2\" }");
  EXPECT_EQ(textOf("$item/value()"), "2");
}

TEST_F(VQLLensTest, PutAcceptsAReplacementComputedFromAVariable) {
  engine.setVariable("item", arena.makeCell("0"));
  engine.setVariable("fresh", arena.makeCell("computed"));
  (void)engine.execute(valueLens);
  engine.execute("weave { $item/$editable%$fresh }");
  EXPECT_EQ(textOf("$item/value()"), "computed");
}

TEST_F(VQLLensTest, FailingPutLeavesNothingHalfWritten) {
  engine.setVariable("item", arena.makeCell("kept"));
  (void)engine.execute(R"(
    let $bad := lens(
      get: \(.) ./value(),
      put: \(cell, val) $cell/value($val)/+d.note%"half"/no_such_function()
    )
    return $bad
  )");
  const auto before = footprint();
  EXPECT_ANY_THROW(engine.execute("weave { $item/$bad%\"lost\" }"));
  EXPECT_EQ(footprint(), before);
  EXPECT_EQ(textOf("$item/value()"), "kept");
  EXPECT_TRUE(engine.execute("$item/+d.note").empty());
}

TEST_F(VQLLensTest, RunTimeFailureInPutWritesNothing) {
  engine.setVariable("item", arena.makeCell("kept"));
  // The contract on must_int fails when put runs, after the first two writes
  // of the callback have been issued.
  (void)engine.execute(R"(
    def must_int($x): Int := $x;
    let $bad := lens(
      get: \(.) ./value(),
      put: \(cell, val) $cell/value($val)/+d.note%"half"/value(must_int("text"))
    )
    return $bad
  )");
  const auto before = footprint();
  EXPECT_ANY_THROW(engine.execute("weave { $item/$bad%\"lost\" }"));
  EXPECT_EQ(footprint(), before);
  EXPECT_EQ(textOf("$item/value()"), "kept");
  EXPECT_TRUE(engine.execute("$item/+d.note").empty());
}

TEST_F(VQLLensTest, LensBoundInOneExecuteIsUsedInALaterOne) {
  engine.setVariable("item", arena.makeCell("before"));
  (void)engine.execute(valueLens);
  EXPECT_EQ(engine.shapeCount(), 1u);
  engine.execute("weave { $item/$editable%\"after\" }");
  EXPECT_EQ(textOf("$item/$editable"), "after");
}

TEST_F(VQLLensTest, LensPassedAsAFunctionArgument) {
  engine.setVariable("item", arena.makeCell("through"));
  (void)engine.execute(valueLens);
  EXPECT_EQ(textOf(R"(
    def read($l, $x) := $x/$l;
    read($editable, $item)
  )"),
            "through");
}

TEST_F(VQLLensTest, LensesComposeAsPathSteps) {
  const auto outer = arena.makeCell("outer");
  const auto inner = arena.makeCell("inner");
  zigzag::expectWritten(arena.link(outer, engine.resolveDimension("d.in"),
                                   zigzag::DimVector::POS, inner));
  engine.setVariable("item", outer);
  (void)engine.execute(R"(
    let $down := lens(get: \(.) ./+d.in)
    let $read := lens(get: \(.) ./value())
    return $read
  )");
  EXPECT_EQ(textOf("$item/$down/$read"), "inner");
}

TEST_F(VQLLensTest, GetThatReturnsNothingYieldsAnEmptyStream) {
  engine.setVariable("item", arena.makeCell("alone"));
  (void)engine.execute(
      R"(let $none := lens(get: \(.) ./+d.absent) return $none)");
  EXPECT_TRUE(engine.execute("$item/$none").empty());
  EXPECT_EQ(textOf("$item/value()"), "alone");
}

TEST_F(VQLLensTest, ProviderLensesRefuseByTheirOwnNames) {
  const auto files = refusalOf("files(\"x\")");
  EXPECT_THAT(files, testing::HasSubstr("files(...)"));
  EXPECT_THAT(files, testing::HasSubstr("host lens provider"));
  EXPECT_THAT(files, testing::Not(testing::HasSubstr("couples")));

  const auto named = refusalOf("let $l := lens(\"xanadoc\", \"a\") return $l");
  EXPECT_THAT(named, testing::HasSubstr("lens(\"xanadoc\", ...)"));
  EXPECT_THAT(named, testing::HasSubstr("host lens provider"));
  EXPECT_THAT(named, testing::Not(testing::HasSubstr("couples")));
}

TEST_F(VQLLensTest, MalformedLensesAreRefusedByName) {
  EXPECT_THAT(refusalOf(R"(let $l := lens(put: \(c, v) $c) return $l)"),
              testing::HasSubstr("needs a get: callback"));
  EXPECT_THAT(refusalOf(R"(let $l := lens(get: \(a, b) $a) return $l)"),
              testing::HasSubstr("get: takes exactly one parameter"));
  EXPECT_THAT(
      refusalOf(
          R"(let $l := lens(get: \(.) ./value(), put: \(c) $c) return $l)"),
      testing::HasSubstr("put: takes exactly two parameters"));
  EXPECT_THAT(
      refusalOf(
          R"(let $l := lens(get: \(.) ./value(), get: \(.) .) return $l)"),
      testing::HasSubstr("given twice"));
}

TEST_F(VQLLensTest, ALensIsNotAShapeOrAnOperand) {
  engine.setVariable("item", arena.makeCell("x"));
  (void)engine.execute(valueLens);
  EXPECT_THAT(refusalOf("##{ $editable }"), testing::HasSubstr("lens"));
  EXPECT_THAT(refusalOf("let $t := $editable + 1 return $t"),
              testing::HasSubstr("lens value"));
}

TEST_F(VQLLensTest, ALensFromAnotherEngineIsNotStaticallyKnown) {
  (void)engine.execute(valueLens);
  const auto handle = engine.getVariable("editable");
  ASSERT_EQ(handle.size(), 1u);
  VQLEngine other{arena};
  other.setVariable("editable", handle);
  other.setVariable("item", arena.makeCell("x"));
  EXPECT_EQ(other.shapeCount(), 0u);
  EXPECT_ANY_THROW(other.execute("$item/$editable"));
}

TEST_F(VQLLensTest, TableIsBoundedByTheLensesHeldAndSurvivesNoFailure) {
  engine.setVariable("item", arena.makeCell("x"));
  EXPECT_ANY_THROW(engine.execute(
      R"(let $l := lens(get: \(.) ./value()); let $t := $nosuch + 1; return $t)"));
  EXPECT_EQ(engine.shapeCount(), 0u);
  EXPECT_TRUE(engine.getVariable("l").empty());

  constexpr int rounds = 20;
  for (int round = 0; round < rounds; ++round) (void)engine.execute(valueLens);
  EXPECT_EQ(engine.shapeCount(), 1u);
  (void)engine.execute(R"(let $other := lens(get: \(.) .) return $other)");
  EXPECT_EQ(engine.shapeCount(), 2u);
  (void)engine.execute("let $editable := 1 return $editable");
  EXPECT_EQ(engine.shapeCount(), 1u);
  engine.clearVariables();
  EXPECT_EQ(engine.shapeCount(), 0u);
}

TEST(VQLLensProgramReleaseTest, ProgramIsReleasedAfterALensQuery) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  xanadu::MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};
  xanadu::vql::QueryExecutor executor{coordinator, vm};
  const auto item = arena.makeCell("before");
  for (int run = 0; run < 3; ++run) {
    const auto result =
        executor.execute(R"(let $r := lens(get: \(.) ./value()) return $r)");
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_FALSE(executor.programRetainedBecause())
        << *executor.programRetainedBecause();
  }
  (void)item;
}

} // namespace

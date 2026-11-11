/**
 * @file vql_path_functions_vm_test.cpp
 * @brief VQL path functions lowered to Vortex graphs (Q1): count, contains and
 * sort_into in their call and value positions, and the refusals for find and
 * for count as a path step.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using xanadu::vql::VQLEngine;
using zigzag::DimRef;
using zigzag::DimVector;

struct PathFunctionFixture : ::testing::Test {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  VQLEngine engine{core};
  DimRef items{engine.resolveDimension("d.items")};

  // Links n fresh cells in order along d.items from the home cell.
  void linkItems(const std::size_t n) {
    auto previous = core.home();
    for (std::size_t i = 0; i < n; ++i) {
      const auto item = arena.makeCell("item " + std::to_string(i));
      zigzag::expectWritten(arena.link(previous, items, DimVector::POS, item));
      previous = item;
    }
  }

  std::optional<std::int64_t> countOf(const std::string &query) {
    const auto result = engine.execute(query);
    EXPECT_EQ(result.size(), 1U) << query;
    if (result.size() != 1U) return std::nullopt;
    return arena.asInt64(result.front());
  }

  // A refusal is a runtime_error whose message contains `text`.
  void expectRefusal(const std::string &query, std::string_view text) {
    SCOPED_TRACE(query);
    try {
      std::ignore = engine.execute(query);
      FAIL() << "query was not refused";
    } catch (const std::runtime_error &error) {
      EXPECT_NE(std::string_view(error.what()).find(text),
                std::string_view::npos)
          << error.what();
    }
  }

  // A malformed call (a caller's mistake) surfaces as invalid_argument.
  void expectMalformed(const std::string &query, std::string_view text) {
    SCOPED_TRACE(query);
    try {
      std::ignore = engine.execute(query);
      FAIL() << "query was not refused";
    } catch (const std::invalid_argument &error) {
      EXPECT_NE(std::string_view(error.what()).find(text),
                std::string_view::npos)
          << error.what();
    }
  }
};

TEST_F(PathFunctionFixture, CountInValuePositionCountsTheWalk) {
  linkItems(3);
  EXPECT_EQ(countOf("count(##/d.items)"), 3);
  EXPECT_EQ(countOf("count(##/d.items/d.absent)"), 0);
}

TEST_F(PathFunctionFixture, CountOfAnAbsentWalkIsZeroNotCellZero) {
  EXPECT_EQ(countOf("count($absent)"), 0);
  EXPECT_EQ(countOf("count(##/d.absent)"), 0);
}

TEST_F(PathFunctionFixture, CountIsRunAgainOnEveryEvaluation) {
  linkItems(2);
  EXPECT_EQ(countOf("count(##/d.items)"), 2);
  EXPECT_EQ(countOf("count(##/d.items)"), 2);
  EXPECT_EQ(countOf("count($absent)"), 0);
}

TEST_F(PathFunctionFixture, CountInCallPositionIsAnArgumentToAnotherFunction) {
  // The inner count answers one scalar cell, which the outer count walks once.
  EXPECT_EQ(countOf("count(count($absent))"), 1);
}

TEST_F(PathFunctionFixture, CountArityIsCheckedBeforeArgumentsRun) {
  expectRefusal("count()", "count");
  expectRefusal("count(##/d.items, ##/+d.unexpected%)", "count");
  expectRefusal(R"(count(##/d.items, "x"))", "count");
  expectRefusal(R"(##/d.items/count(##/d.items, "x"))", "count");
}

TEST_F(PathFunctionFixture, ArgumentFreeCountAsPathStepIsRefusedByName) {
  linkItems(2);
  expectRefusal("##/d.items/count()", "count");
  expectRefusal("##/d.absent/count()", "count");
}

TEST_F(PathFunctionFixture, CountWithArgumentRunsPerCurrentCell) {
  linkItems(2);
  // The first item has two cells along d.sub, the second none, so a per-cell
  // count answers 2 then 0 where a whole-walk count would answer one number.
  const auto sub  = engine.resolveDimension("d.sub");
  const auto head = arena.linked(core.home(), items, DimVector::POS);
  ASSERT_TRUE(head);
  const auto firstSub = arena.makeCell("s0");
  zigzag::expectWritten(arena.link(*head, sub, DimVector::POS, firstSub));
  zigzag::expectWritten(
      arena.link(firstSub, sub, DimVector::POS, arena.makeCell("s1")));
  const auto perCell = engine.execute("##/d.items/count(+d.sub)");
  ASSERT_EQ(perCell.size(), 2U);
  EXPECT_EQ(arena.asInt64(perCell[0]), 2);
  EXPECT_EQ(arena.asInt64(perCell[1]), 0);
}

TEST_F(PathFunctionFixture, NamedArgumentsToCountAndContainsAreMalformed) {
  expectMalformed("count(path: ##/d.items)", "count");
  expectMalformed(R"(contains(haystack: "abc", "b"))", "contains");
}

TEST_F(PathFunctionFixture, ContainsSearchesAComputedScalarAsItsText) {
  linkItems(1);
  using zigzag::vortex::CellValue;
  // count() mints an Int64 cell at run time; it is searched as "1".
  auto result = engine.execute(R"(contains("a1b", count(##/d.items)))");
  ASSERT_EQ(result.size(), 1U);
  EXPECT_EQ(core.render(result.front()), CellValue(true));
  result = engine.execute(R"(contains("a2b", count(##/d.items)))");
  ASSERT_EQ(result.size(), 1U);
  EXPECT_EQ(core.render(result.front()), CellValue(false));
}

TEST_F(PathFunctionFixture, ContainsArityIsCheckedBeforeArgumentsRun) {
  expectRefusal(R"(contains("abc"))", "contains");
  expectRefusal(R"(contains("abc", "a", "extra"))", "contains");
  expectRefusal("contains()", "contains");
}

TEST_F(PathFunctionFixture, ContainsSearchesEveryOffset) {
  const auto answer = [&](const std::string &query) {
    const auto result = engine.execute(query);
    EXPECT_EQ(result.size(), 1U) << query;
    return result.empty() ? std::optional<zigzag::vortex::CellValue>{}
                          : core.render(result.front());
  };
  using zigzag::vortex::CellValue;
  EXPECT_EQ(answer(R"(contains("abc", "b"))"), CellValue(true));
  EXPECT_EQ(answer(R"(contains("abc", "a"))"), CellValue(true));
  EXPECT_EQ(answer(R"(contains("abc", "c"))"), CellValue(true));
  EXPECT_EQ(answer(R"(contains("abc", "abc"))"), CellValue(true));
  EXPECT_EQ(answer(R"(contains("abc", "bc"))"), CellValue(true));
  EXPECT_EQ(answer(R"(contains("abc", "z"))"), CellValue(false));
  EXPECT_EQ(answer(R"(contains("abc", "abcd"))"), CellValue(false));
  EXPECT_EQ(answer(R"(contains("abc", "ac"))"), CellValue(false));
  // A partial match at the tail must not count, and a repeated prefix must
  // not hide the real match behind it.
  EXPECT_EQ(answer(R"(contains("aab", "ab"))"), CellValue(true));
  EXPECT_EQ(answer(R"(contains("abab", "bb"))"), CellValue(false));
}

TEST_F(PathFunctionFixture, ContainsEmptyNeedleAndHaystackFollowFind) {
  const auto answer = [&](const std::string &query) {
    const auto result = engine.execute(query);
    EXPECT_EQ(result.size(), 1U) << query;
    return result.empty() ? std::optional<zigzag::vortex::CellValue>{}
                          : core.render(result.front());
  };
  using zigzag::vortex::CellValue;
  EXPECT_EQ(answer(R"(contains("abc", ""))"), CellValue(true));
  EXPECT_EQ(answer(R"(contains("", ""))"), CellValue(true));
  EXPECT_EQ(answer(R"(contains("", "a"))"), CellValue(false));
}

TEST_F(PathFunctionFixture, ContainsReadsNumbersAsTheirText) {
  using zigzag::vortex::CellValue;
  const auto result = engine.execute("contains(12345, 34)");
  ASSERT_EQ(result.size(), 1U);
  EXPECT_EQ(core.render(result.front()), CellValue(true));
}

TEST_F(PathFunctionFixture, ContainsOfAnAbsentArgumentIsFalse) {
  using zigzag::vortex::CellValue;
  for (const auto *query :
       {R"(contains($absent, "a"))", R"(contains("a", $absent))",
        R"(contains(##/d.absent, ##/d.absent))"}) {
    const auto result = engine.execute(query);
    ASSERT_EQ(result.size(), 1U) << query;
    EXPECT_EQ(core.render(result.front()), CellValue(false)) << query;
  }
}

TEST_F(PathFunctionFixture, ContainsRunsOnCellsAndAgainOnEveryEvaluation) {
  using zigzag::vortex::CellValue;
  linkItems(2);
  for (int round = 0; round < 2; ++round) {
    const auto hit = engine.execute(R"(contains(##/d.items, "item"))");
    ASSERT_EQ(hit.size(), 1U);
    EXPECT_EQ(core.render(hit.front()), CellValue(true));
    const auto miss = engine.execute(R"(contains(##/d.items, "zzz"))");
    ASSERT_EQ(miss.size(), 1U);
    EXPECT_EQ(core.render(miss.front()), CellValue(false));
  }
}

TEST_F(PathFunctionFixture, ContainsInALetBindingIsTheBooleanScalar) {
  const auto bound =
      engine.execute(R"(let $hit := contains("abc", "b"); return $hit)");
  ASSERT_EQ(bound.size(), 1U);
  EXPECT_EQ(core.render(bound.front()), zigzag::vortex::CellValue(true));
}

TEST_F(PathFunctionFixture, FindIsRefusedUntilProseSearchExists) {
  expectRefusal(R"(find("needle"))", "find");
  expectRefusal("find()", "find");
  expectRefusal(R"(find("a", "b"))", "find");
}

// A rank of keyed cells: n cells along d.videos, each with a +d.tag key.
struct SortFixture : PathFunctionFixture {
  DimRef videos{engine.resolveDimension("d.videos")};
  DimRef tag{engine.resolveDimension("d.tag")};
  std::vector<zigzag::CellRef> cells;

  void build(const std::vector<std::int64_t> &keys) {
    auto previous = core.home();
    for (const auto key : keys) {
      const auto cell = arena.makeCell("cell " + std::to_string(cells.size()));
      zigzag::expectWritten(
          arena.link(cell, tag, DimVector::POS, arena.makeScalarCell(key)));
      zigzag::expectWritten(arena.link(previous, videos, DimVector::POS, cell));
      cells.push_back(cell);
      previous = cell;
    }
  }

  // The rank after home, read back off the arena.
  std::vector<zigzag::CellRef> rank() const {
    std::vector<zigzag::CellRef> out;
    auto at = arena.linked(core.home(), videos, DimVector::POS);
    while (at) {
      out.push_back(*at);
      at = arena.linked(*at, videos, DimVector::POS);
    }
    return out;
  }

  std::vector<zigzag::CellRef> pick(std::initializer_list<std::size_t> order) {
    std::vector<zigzag::CellRef> out;
    for (const auto index : order) out.push_back(cells[index]);
    return out;
  }

  // Every link is reciprocal and the rank is a simple chain.
  void expectReciprocal() const {
    auto previous = core.home();
    for (const auto cell : rank()) {
      EXPECT_EQ(arena.linked(cell, videos, DimVector::NEG), previous);
      previous = cell;
    }
  }
};

TEST_F(SortFixture, SortsStablyAndRelinksReciprocally) {
  build({5, 3, 5, 1, 3});
  const auto sorted = engine.execute("sort_into(##/d.videos, by: +d.tag)");
  EXPECT_EQ(sorted, pick({3, 1, 4, 0, 2}));
  EXPECT_EQ(rank(), pick({3, 1, 4, 0, 2}));
  expectReciprocal();
}

TEST_F(SortFixture, SortingTwiceAndSortedInputAreIdempotent) {
  build({1, 2, 3});
  EXPECT_EQ(engine.execute("sort_into(##/d.videos, by: +d.tag)"),
            pick({0, 1, 2}));
  EXPECT_EQ(engine.execute("sort_into(##/d.videos, by: +d.tag)"),
            pick({0, 1, 2}));
  expectReciprocal();
}

TEST_F(SortFixture, ReverseOrderMovesEveryCell) {
  build({4, 3, 2, 1});
  EXPECT_EQ(engine.execute("sort_into(##/d.videos, by: +d.tag)"),
            pick({3, 2, 1, 0}));
  expectReciprocal();
}

TEST_F(SortFixture, EmptyAndSingleRanksAreReturnedUnchanged) {
  EXPECT_TRUE(engine.execute("sort_into(##/d.videos, by: +d.tag)").empty());
  build({7});
  EXPECT_EQ(engine.execute("sort_into(##/d.videos, by: +d.tag)"), pick({0}));
  expectReciprocal();
}

TEST_F(SortFixture, ASectionAfterAnAnchorKeepsTheCellsAroundIt) {
  build({9, 4, 8, 1});
  engine.setVariable("anchor", cells[0]);
  const auto sorted =
      engine.execute("sort_into($anchor/+d.videos, by: +d.tag)");
  EXPECT_EQ(sorted, pick({3, 1, 2}));
  EXPECT_EQ(rank(), pick({0, 3, 1, 2}));
  expectReciprocal();
}

TEST_F(SortFixture, LambdaKeyReceivesEachCandidate) {
  build({3, 1, 2});
  const auto sorted =
      engine.execute(R"(sort_into(##/d.videos, by: \($c) $c/+d.tag))");
  EXPECT_EQ(sorted, pick({1, 2, 0}));
}

TEST_F(SortFixture, ALetBindingHoldsTheWholeSortedSection) {
  build({3, 1, 2});
  const auto bound = engine.execute(
      "let $sorted := sort_into(##/d.videos, by: +d.tag); return $sorted");
  EXPECT_EQ(bound, pick({1, 2, 0}));
  EXPECT_EQ(rank(), pick({1, 2, 0}));
}

TEST_F(SortFixture, ACandidateWithoutAKeyFailsAndLeavesTheRankAlone) {
  build({3, 1, 2});
  const auto bare = arena.makeCell("no key");
  zigzag::expectWritten(arena.link(cells.back(), videos, DimVector::POS, bare));
  cells.push_back(bare);
  const auto before = rank();
  expectRefusal("sort_into(##/d.videos, by: +d.tag)", "exactly one scalar");
  EXPECT_EQ(rank(), before);
  expectReciprocal();
}

TEST_F(SortFixture, ARankPathThatMayNotBeOneSectionIsRefused) {
  build({2, 1});
  const auto before = rank();
  for (const auto *query :
       {"sort_into(##/d.videos/+d.videos, by: +d.tag)",
        "sort_into(##/-d.videos, by: +d.tag)",
        "sort_into(##/+d.videos[1], by: +d.tag)",
        "sort_into(##/+d.videos[? true], by: +d.tag)",
        "sort_into(##/~d.videos, by: +d.tag)", "sort_into($absent, by: +d.tag)",
        "sort_into(##, by: +d.tag)"}) {
    expectRefusal(query, "sort_into");
  }
  EXPECT_EQ(rank(), before);
}

TEST_F(SortFixture, SortIntoArityIsUnsupportedAndNamingMistakesAreMalformed) {
  build({2, 1});
  // Arity stays a runtime refusal: VQLQueryErrorTest pins it Unsupported.
  expectRefusal("sort_into(##/d.videos)", "sort_into");
  expectRefusal("sort_into(##/d.videos, by: +d.tag, by: +d.tag)", "sort_into");
  expectMalformed("sort_into(##/d.videos, +d.tag)", "by:");
  expectMalformed("sort_into(##/d.videos, key: +d.tag)", "by:");
  expectMalformed("sort_into(rank: ##/d.videos, by: +d.tag)", "by:");
}

// Key classes. Gt orders text byte-wise and everything else through double, so
// the lowering refuses what that cannot order correctly.
struct SortKeyFixture : SortFixture {
  void buildKeyCells(const std::vector<zigzag::CellRef> &keys) {
    auto previous = core.home();
    for (const auto key : keys) {
      const auto cell = arena.makeCell("cell " + std::to_string(cells.size()));
      zigzag::expectWritten(arena.link(cell, tag, DimVector::POS, key));
      zigzag::expectWritten(arena.link(previous, videos, DimVector::POS, cell));
      cells.push_back(cell);
      previous = cell;
    }
  }
  void expectRefusedAndUnmoved(std::string_view text) {
    const auto before = rank();
    expectRefusal("sort_into(##/d.videos, by: +d.tag)", text);
    EXPECT_EQ(rank(), before);
    expectReciprocal();
  }
};

TEST_F(SortKeyFixture, TextKeysIncludingEmptyAndNumeralsSortLexically) {
  buildKeyCells({arena.makeCell("b"), arena.makeCell("10"), arena.makeCell(""),
                 arena.makeCell("9"), arena.makeCell("a")});
  EXPECT_EQ(engine.execute("sort_into(##/d.videos, by: +d.tag)"),
            pick({2, 1, 3, 4, 0}));
  expectReciprocal();
}

TEST_F(SortKeyFixture, DecimalKeysSortNumerically) {
  buildKeyCells({arena.makeScalarCell(2.5), arena.makeScalarCell(-1.5),
                 arena.makeScalarCell(10.25)});
  EXPECT_EQ(engine.execute("sort_into(##/d.videos, by: +d.tag)"),
            pick({1, 0, 2}));
}

TEST_F(SortKeyFixture, MixedTextAndNumberKeysAreRefusedAndRolledBack) {
  buildKeyCells({arena.makeCell("a"), arena.makeScalarCell(std::int64_t{1}),
                 arena.makeCell("b")});
  expectRefusedAndUnmoved("sort_into");
  expectRefusedAndUnmoved("cannot mix");
}

TEST_F(SortKeyFixture, NumberFirstThenTextKeysAreRefused) {
  buildKeyCells({arena.makeScalarCell(std::int64_t{3}),
                 arena.makeScalarCell(std::int64_t{2}), arena.makeCell("a")});
  expectRefusedAndUnmoved("cannot mix");
}

TEST_F(SortKeyFixture, IntegerAgainstDecimalKeysAreRefused) {
  // 2^53+1 against 2^53: through double they compare equal and would sort
  // silently wrong, so the pair is refused instead.
  buildKeyCells({arena.makeScalarCell(std::int64_t{9007199254740993}),
                 arena.makeScalarCell(9007199254740992.0)});
  expectRefusedAndUnmoved("cannot mix");
}

TEST_F(SortKeyFixture, NaNKeysAreRefused) {
  buildKeyCells({arena.makeScalarCell(1.5),
                 arena.makeScalarCell(std::numeric_limits<double>::quiet_NaN()),
                 arena.makeScalarCell(0.5)});
  expectRefusedAndUnmoved("NaN");
}

TEST_F(SortKeyFixture, BoolKeysAreRefused) {
  buildKeyCells({arena.makeScalarCell(true), arena.makeScalarCell(false)});
  expectRefusedAndUnmoved("Bool");
}

TEST_F(SortKeyFixture, ALambdaKeyIsEvaluatedForEveryComparisonAndStaysCorrect) {
  buildKeyCells({arena.makeScalarCell(std::int64_t{6}),
                 arena.makeScalarCell(std::int64_t{2}),
                 arena.makeScalarCell(std::int64_t{5}),
                 arena.makeScalarCell(std::int64_t{2}),
                 arena.makeScalarCell(std::int64_t{9}),
                 arena.makeScalarCell(std::int64_t{1})});
  EXPECT_EQ(engine.execute(R"(sort_into(##/d.videos, by: \($c) $c/+d.tag))"),
            pick({5, 1, 3, 2, 0, 4}));
  expectReciprocal();
}

// A ring has no head: the anchored walk ends back at the anchor. The sort
// detects that before moving anything and refuses by name.
TEST_F(SortKeyFixture, ACyclicRankTerminatesWithoutRelinking) {
  buildKeyCells({arena.makeScalarCell(std::int64_t{3}),
                 arena.makeScalarCell(std::int64_t{1}),
                 arena.makeScalarCell(std::int64_t{2})});
  zigzag::expectWritten(arena.unlink(cells[0], videos, DimVector::NEG));
  zigzag::expectWritten(arena.link(cells[2], videos, DimVector::POS, cells[0]));
  // The walk from c1 is c2, c0 and stops at the anchor, so c1 is both the
  // section's predecessor and its successor.
  engine.setVariable("ring", cells[1]);
  expectRefusal("sort_into($ring/+d.videos, by: +d.tag)", "cyclic rank");
  EXPECT_EQ(arena.linked(cells[0], videos, DimVector::POS), cells[1]);
  EXPECT_EQ(arena.linked(cells[1], videos, DimVector::POS), cells[2]);
  EXPECT_EQ(arena.linked(cells[2], videos, DimVector::POS), cells[0]);
}

TEST_F(SortFixture, ExhaustingTheCycleBudgetFailsAndRollsTheRankBack) {
  std::vector<std::int64_t> keys;
  for (std::int64_t key = 40; key > 0; --key) keys.push_back(key);
  build(keys);
  const auto before = rank();

  zigzag::vortex::VortexVM vm{core};
  xanadu::MultiStoreCoordinator coordinator{core};
  xanadu::vql::QueryExecutor tight(coordinator, vm, {.maxCycles = 200});
  const auto refused = tight.execute("sort_into(##/d.videos, by: +d.tag)");
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().category, xanadu::vql::QueryErrorCategory::Budget)
      << refused.error().message;
  EXPECT_EQ(rank(), before);

  xanadu::vql::QueryExecutor roomy(coordinator, vm, {.maxCycles = 5'000'000});
  const auto sorted = roomy.execute("sort_into(##/d.videos, by: +d.tag)");
  ASSERT_TRUE(sorted.has_value()) << sorted.error().message;
  EXPECT_EQ(sorted->size(), cells.size());
  EXPECT_EQ(rank().front(), cells.back());
  EXPECT_EQ(rank().back(), cells.front());
  expectReciprocal();
}

} // namespace

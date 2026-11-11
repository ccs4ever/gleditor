/**
 * @file vql_path_functions_vm_test.cpp
 * @brief VQL path functions lowered to Vortex graphs (Q1): count in its call
 * and value positions, and the refusals for sort_into, find, contains, and
 * count as a path step.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

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

  // A refusal is a runtime_error that names the function.
  void expectRefusal(const std::string &query, std::string_view name) {
    SCOPED_TRACE(query);
    try {
      std::ignore = engine.execute(query);
      FAIL() << "query was not refused";
    } catch (const std::runtime_error &error) {
      EXPECT_NE(std::string_view(error.what()).find(name),
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
  // Each cell of the walk answers its own count, as a step with an argument
  // always does; the argument here is empty at every cell.
  const auto perCell = engine.execute("##/d.items/count($absent)");
  ASSERT_EQ(perCell.size(), 2U);
  EXPECT_EQ(arena.asInt64(perCell[0]), 0);
  EXPECT_EQ(arena.asInt64(perCell[1]), 0);
}

TEST_F(PathFunctionFixture, ContainsIsRefusedUntilASubstringOpcodeExists) {
  expectRefusal(R"(contains("abc", "b"))", "contains");
  expectRefusal(R"(contains("abc"))", "contains");
  expectRefusal(R"(contains("abc", "a", "extra"))", "contains");
}

TEST_F(PathFunctionFixture, FindIsRefusedUntilProseSearchExists) {
  expectRefusal(R"(find("needle"))", "find");
  expectRefusal("find()", "find");
  expectRefusal(R"(find("a", "b"))", "find");
}

TEST_F(PathFunctionFixture, SortIntoIsRefusedWithItsContract) {
  linkItems(3);
  expectRefusal("sort_into(##/d.items)", "sort_into");
  expectRefusal("sort_into(##/d.items, +d.tag)", "sort_into");
  expectRefusal("##/d.items/sort_into(##/d.items, by: +d.tag)", "sort_into");
}

} // namespace

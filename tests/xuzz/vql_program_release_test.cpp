/**
 * @file vql_program_release_test.cpp
 * @brief A VQL run leaves no program cells behind (coordinator plan section 5,
 * "Program cells after a VQL run"): the arena is marked before compile and
 * released after the run, and the result cells the program minted are carried
 * out first.
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::DimVector;

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};
  QueryExecutor executor{coordinator, vm};
  zigzag::DimRef values{coordinator.resolveDimension("d.values")};

  CellRef append(CellRef cell) {
    const auto from = zigzag::rankTail(arena, core.home(), values);
    zigzag::expectWritten(arena.link(from, values, DimVector::POS, cell));
    return cell;
  }
};

TEST(VqlProgramReleaseTest, ARunOverExistingCellsLeavesTheArenaAsItWas) {
  Harness h;
  h.append(h.arena.makeScalarCell(std::int64_t{1}));
  h.append(h.arena.makeScalarCell(std::int64_t{2}));
  const auto before = h.arena.cellCount();
  for (int run = 0; run < 5; ++run) {
    const auto result = h.executor.execute("for $x in ##/d.values return $x");
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->size(), 2U);
    EXPECT_FALSE(h.executor.programRetainedBecause());
    EXPECT_EQ(h.arena.cellCount(), before) << "after run " << run;
  }
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VqlProgramReleaseTest, AResultTheProgramMintedSurvivesTheRelease) {
  Harness h;
  h.append(h.arena.makeScalarCell(std::int64_t{40}));
  const auto before  = h.arena.cellCount();
  constexpr int runs = 4;
  std::vector<CellRef> held;
  for (int run = 0; run < runs; ++run) {
    const auto result =
        h.executor.execute("for $x in ##/d.values return ($x + 2)");
    ASSERT_TRUE(result) << result.error().message;
    ASSERT_EQ(result->size(), 1U);
    EXPECT_FALSE(h.executor.programRetainedBecause())
        << *h.executor.programRetainedBecause();
    held.push_back(result->front());
  }
  // Only the caller's results remain; every program cell is gone.
  EXPECT_EQ(h.arena.cellCount(), before + held.size());
  for (const auto cell : held) {
    ASSERT_TRUE(h.arena.contains(cell));
    EXPECT_EQ(h.arena.asInt64(cell), std::int64_t{42});
  }
}

TEST(VqlProgramReleaseTest, ATextResultKeepsItsBytes) {
  Harness h;
  h.append(h.arena.makeCell(std::string_view{"alpha"}));
  const auto result = h.executor.execute("for $x in ##/d.values return $x");
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 1U);
  EXPECT_EQ(h.arena.textOf(result->front()), "alpha");
}

TEST(VqlProgramReleaseTest, AFailedCompileLeavesNothing) {
  Harness h;
  h.append(h.arena.makeScalarCell(std::int64_t{1}));
  const auto before = h.arena.cellCount();
  const auto result =
      h.executor.execute("for $x in ##/d.values return nosuchfunction($x)");
  ASSERT_FALSE(result);
  EXPECT_EQ(h.arena.cellCount(), before);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VqlProgramReleaseTest, AMutatingRunKeepsItsProgramAndSaysWhy) {
  Harness h;
  const auto first  = h.append(h.arena.makeScalarCell(std::int64_t{2}));
  const auto second = h.append(h.arena.makeScalarCell(std::int64_t{1}));
  const auto tag    = h.coordinator.resolveDimension("d.tag");
  zigzag::expectWritten(
      h.arena.link(first, tag, DimVector::POS, h.arena.makeCell("b")));
  zigzag::expectWritten(
      h.arena.link(second, tag, DimVector::POS, h.arena.makeCell("a")));
  const auto result = h.executor.execute("sort_into(##/d.values, by: +d.tag)");
  ASSERT_TRUE(result) << result.error().message;
  // release() would undo the relink the sort made to older cells.
  ASSERT_TRUE(h.executor.programRetainedBecause());
  EXPECT_NE(h.executor.programRetainedBecause()->find("writes cells"),
            std::string::npos);
  EXPECT_EQ(h.arena.linked(h.core.home(), h.values, DimVector::POS), second);
}

} // namespace
} // namespace xanadu::vql

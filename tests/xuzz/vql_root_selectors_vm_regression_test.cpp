#include <gtest/gtest.h>

#include <string_view>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
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

  CompilationResult compile(const std::string_view source) {
    VQLCompiler compiler(coordinator, vm);
    return compiler.compile(source);
  }

  CellRef namedCell(const std::string_view name) {
    const CellRef cell     = arena.makeCell();
    const CellRef nameCell = arena.makeCell(name);
    zigzag::expectWritten(
        arena.link(cell, core.dims().name, DimVector::POS, nameCell));
    return cell;
  }

  void appendCursor(const CellRef cursor, const bool pinned = false) {
    const auto dimension =
        pinned ? core.dims().pinningCursors : core.dims().cursors;
    const CellRef tail = zigzag::rankTail(arena, core.home(), dimension);
    zigzag::expectWritten(arena.link(tail, dimension, DimVector::POS, cursor));
  }

  void appendNameAlias(const CellRef previousName,
                       const std::string_view name) {
    const CellRef alias = arena.makeCell(name);
    zigzag::expectWritten(
        arena.link(previousName, core.dims().name, DimVector::POS, alias));
  }

  std::vector<CellRef> run(CompilationResult &compiled) {
    EXPECT_TRUE(compiled.success) << compiled.errorMessage;
    EXPECT_TRUE(compiled.cursor);
    if (!compiled.success || !compiled.cursor) return {};

    const auto execution = vm.run(*compiled.cursor, 100'000);
    EXPECT_TRUE(execution.success) << execution.errorMessage;
    vm.setCursorOpcode(*compiled.cursor, std::nullopt);

    std::vector<CellRef> result;
    for (const CellRef slot : compiled.resultSlots) {
      const auto cells = vm.resultCells(slot);
      result.insert(result.end(), cells.begin(), cells.end());
    }
    return result;
  }
};

TEST(VQLRootSelectorsVmRegressionTest,
     ActiveCursorRankIsReadAfterCompilationAndKeepsEveryMember) {
  Harness h;
  auto compiled = h.compile("##/^");
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  ASSERT_TRUE(compiled.cursor);

  const CellRef first  = h.arena.makeCell("first-active");
  const CellRef second = h.arena.makeCell("second-active");
  h.appendCursor(first);
  h.appendCursor(second);

  EXPECT_EQ(h.run(compiled),
            (std::vector<CellRef>{*compiled.cursor, first, second}));
}

TEST(VQLRootSelectorsVmRegressionTest,
     AllCursorsConcatenatesActiveThenPinnedRuntimeRanks) {
  Harness h;
  auto compiled = h.compile("##/^^");
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  ASSERT_TRUE(compiled.cursor);

  const CellRef firstActive  = h.arena.makeCell("first-active");
  const CellRef secondActive = h.arena.makeCell("second-active");
  const CellRef firstPinned  = h.arena.makeCell("first-pinned");
  const CellRef secondPinned = h.arena.makeCell("second-pinned");
  h.appendCursor(firstActive);
  h.appendCursor(secondActive);
  h.appendCursor(firstPinned, true);
  h.appendCursor(secondPinned, true);

  EXPECT_EQ(h.run(compiled),
            (std::vector<CellRef>{*compiled.cursor, firstActive, secondActive,
                                  firstPinned, secondPinned}));
}

TEST(VQLRootSelectorsVmRegressionTest,
     NamedCursorReadsEveryNameAliasAndScansOnlyItsSpecifiedRanks) {
  Harness h;
  auto activeCompiled = h.compile("##/^chosen");
  ASSERT_TRUE(activeCompiled.success) << activeCompiled.errorMessage;
  ASSERT_TRUE(activeCompiled.cursor);

  const CellRef active    = h.arena.makeCell("active");
  const CellRef firstName = h.arena.makeCell("other");
  zigzag::expectWritten(
      h.arena.link(active, h.core.dims().name, DimVector::POS, firstName));
  h.appendNameAlias(firstName, "chosen");
  h.appendCursor(active);

  EXPECT_EQ(h.run(activeCompiled), (std::vector<CellRef>{active}));

  auto allNamedCompiled = h.compile("##/^^chosen");
  ASSERT_TRUE(allNamedCompiled.success) << allNamedCompiled.errorMessage;
  ASSERT_TRUE(allNamedCompiled.cursor);

  const CellRef pinned = h.namedCell("chosen");
  h.appendCursor(pinned, true);
  EXPECT_EQ(h.run(allNamedCompiled), (std::vector<CellRef>{active, pinned}));
}

TEST(VQLRootSelectorsVmRegressionTest,
     PerspectiveReadsQualifiedAxisAfterCompileAndPreservesFilterAndClamp) {
  Harness h;
  auto compiled = h.compile("##/@alice:+d.items[. = \"keep\"][-1]");
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;

  const auto qualified   = h.coordinator.resolveDimension("d.items@alice");
  const auto canonical   = h.coordinator.resolveDimension("d.items");
  const CellRef discard  = h.arena.makeCell("discard");
  const CellRef keep     = h.arena.makeCell("keep");
  const CellRef fallback = h.arena.makeCell("fallback");
  zigzag::expectWritten(
      h.arena.link(h.core.home(), qualified, DimVector::POS, discard));
  zigzag::expectWritten(h.arena.link(discard, qualified, DimVector::POS, keep));
  zigzag::expectWritten(
      h.arena.link(h.core.home(), canonical, DimVector::POS, fallback));

  EXPECT_EQ(h.run(compiled), (std::vector<CellRef>{keep}));
}

TEST(VQLRootSelectorsVmRegressionTest,
     PerspectiveFallsBackWhenQualifiedPathIsEmptyAtExecution) {
  Harness h;
  auto compiled = h.compile("##/@alice:+d.items");
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;

  const auto canonical   = h.coordinator.resolveDimension("d.items");
  const CellRef fallback = h.arena.makeCell("fallback");
  zigzag::expectWritten(
      h.arena.link(h.core.home(), canonical, DimVector::POS, fallback));

  EXPECT_EQ(h.run(compiled), (std::vector<CellRef>{fallback}));
}

TEST(VQLRootSelectorsVmRegressionTest,
     PerspectiveFallsBackWhenQualifiedTargetFiltersToEmpty) {
  Harness h;
  auto compiled = h.compile("##/@alice:+d.items[. = \"keep\"]");
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;

  const auto qualified  = h.coordinator.resolveDimension("d.items@alice");
  const auto canonical  = h.coordinator.resolveDimension("d.items");
  const CellRef discard = h.arena.makeCell("discard");
  const CellRef keep    = h.arena.makeCell("keep");
  zigzag::expectWritten(
      h.arena.link(h.core.home(), qualified, DimVector::POS, discard));
  zigzag::expectWritten(
      h.arena.link(h.core.home(), canonical, DimVector::POS, keep));

  EXPECT_EQ(h.run(compiled), (std::vector<CellRef>{keep}));
}

TEST(VQLRootSelectorsVmRegressionTest,
     RootMetacellsRefuseWithoutGraphVisibleRegistry) {
  Harness h;
  const CellRef source = h.arena.makeCell("source");
  const CellRef child  = h.arena.makeCell("child");
  const auto dimension = h.coordinator.resolveDimension("d.items");
  zigzag::expectWritten(h.arena.link(source, dimension, DimVector::POS, child));

  const auto compiled = h.compile("##/#");
  EXPECT_FALSE(compiled.success);
  EXPECT_FALSE(compiled.cursor);

  EXPECT_EQ(h.arena.linked(source, dimension, DimVector::POS), child);
}

} // namespace
} // namespace xanadu::vql

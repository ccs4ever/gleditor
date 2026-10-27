#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {
using zigzag::ArenaManifold;
using zigzag::CellRef;
using namespace zigzag::vortex;

TEST(VagentPointIntegration, OverlayBudgetRefusalWritesNoOperations) {
  ArenaManifold arena;
  const auto root = arena.makeCell("state");
  xanadu::Store store;
  const auto before = store.segmentedOps().size();
  EXPECT_FALSE(zigzag::promoteOverlay(arena, root, store, {},
                                      zigzag::PromotionBudget{0}));
  EXPECT_EQ(store.segmentedOps().size(), before);
}

TEST(VagentPointIntegration, PersistedOpcodeIsDecodedByAFreshVM) {
  ArenaManifold original;
  VortexCore originalCore{original};
  VortexVM originalVM{originalCore};
  const auto point = originalVM.mintOpcode(OpcodeKind::Point);

  xanadu::Store store;
  const auto genesis   = store.sliceGenesis({});
  const auto persisted = zigzag::promote(store, genesis, original, point);
  ASSERT_TRUE(persisted);
  const auto fold = store.rebuildManifold(persisted->version);
  ArenaManifold resumed{&fold, store};
  VortexCore core{resumed};
  VortexVM vm{core};
  const auto opcode = persisted->mapping.at(point);
  ASSERT_EQ(vm.getOpcodeKind(opcode), OpcodeKind::Point);

  const auto target = resumed.makeScalarCell(std::int64_t{42});
  const auto handle = resumed.makeCell();
  const auto reference =
      resumed.makeScalarCell(static_cast<std::int64_t>(target));
  core.bindInput(opcode, reference);
  core.bindOutput(opcode, handle);
  const auto cursor = vm.spawnCursor(opcode);
  ASSERT_TRUE(vm.step(cursor).success);
  EXPECT_EQ(resumed.handleTarget(handle), target);
}

TEST(VagentPointIntegration, InvalidReferencesFailWithoutRebindingTheHandle) {
  const std::vector<CellValue> invalid{std::int64_t{-1},
                                       std::int64_t{0},
                                       std::numeric_limits<std::int64_t>::max(),
                                       std::numeric_limits<double>::infinity(),
                                       1.5,
                                       true,
                                       std::string{"1"}};
  for (const auto &value : invalid) {
    ArenaManifold arena;
    VortexCore core{arena};
    VortexVM vm{core};
    const auto point     = vm.mintOpcode(OpcodeKind::Point);
    const auto reference = arena.makeCell();
    const auto handle    = arena.makeCell("unchanged");
    core.writeValue(reference, 0, std::nullopt, value);
    core.bindInput(point, reference);
    core.bindOutput(point, handle);
    const auto result = vm.step(vm.spawnCursor(point));
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.failingClause, point);
    EXPECT_FALSE(arena.handleTarget(handle));
    EXPECT_EQ(arena.textOf(handle), "unchanged");
  }
}
} // namespace

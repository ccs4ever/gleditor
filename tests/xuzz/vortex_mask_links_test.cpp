#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "common/xanadu/vortex/vortex_vm.hpp"

namespace zigzag::vortex {
namespace {
struct MaskHarness {
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  CellRef owner{arena.makeCell("owner")};
  CellRef neighbor{arena.makeCell("neighbor")};

  MaskHarness() {
    zigzag::expectWritten(
        arena.link(owner, core.dims().role, DimVector::POS, neighbor));
  }

  CellRef mask() {
    const auto opcode = vm.mintOpcode(OpcodeKind::VqlMaskLinks);
    core.bindInput(opcode, owner);
    core.bindInput(opcode, arena.makeScalarCell(false));
    return opcode;
  }

  void pair(const CellRef opcode, const std::int64_t dimension,
            const std::int64_t bits) {
    core.bindInput(opcode, arena.makeScalarCell(dimension));
    core.bindInput(opcode, arena.makeScalarCell(bits));
  }

  void expectUnchanged() {
    EXPECT_EQ(arena.linked(owner, core.dims().role, DimVector::POS), neighbor);
    EXPECT_EQ(arena.linked(neighbor, core.dims().role, DimVector::NEG), owner);
    EXPECT_EQ(arena.outstandingMarks(), 0U);
  }
};

TEST(VortexMaskLinksTest, MalformedLateOperandsRefuseBeforeRemovingAnyEdge) {
  for (const auto invalid : {std::int64_t{-1}, std::int64_t{4},
                             std::numeric_limits<std::int64_t>::max()}) {
    MaskHarness h;
    const auto opcode = h.mask();
    h.pair(opcode, h.core.dims().role, 0);
    h.pair(opcode, h.core.dims().step, invalid);
    const auto result = h.vm.step(h.vm.spawnCursor(opcode));
    EXPECT_FALSE(result.success);
    h.expectUnchanged();
  }
}

TEST(VortexMaskLinksTest, DuplicateAndOutOfRangeDimensionsRefuseAtomically) {
  for (const bool duplicate : {false, true}) {
    MaskHarness h;
    const auto opcode = h.mask();
    h.pair(opcode, h.core.dims().role, 0);
    h.pair(opcode,
           duplicate
               ? static_cast<std::int64_t>(h.core.dims().role)
               : static_cast<std::int64_t>(std::numeric_limits<DimRef>::max()) +
                     1,
           3);
    const auto result = h.vm.step(h.vm.spawnCursor(opcode));
    EXPECT_FALSE(result.success);
    h.expectUnchanged();
  }
}

TEST(VortexMaskLinksTest, DeclaredOutputIsRejectedBeforeMutation) {
  MaskHarness h;
  const auto opcode = h.mask();
  const auto output = h.arena.makeScalarCell(std::int64_t{91});
  h.core.bindOutput(opcode, output);
  const auto result = h.vm.step(h.vm.spawnCursor(opcode));
  EXPECT_FALSE(result.success);
  EXPECT_EQ(h.arena.asInt64(output), 91);
  h.expectUnchanged();
}

TEST(VortexMaskLinksTest, RuleLimitRefusesBeforeMutation) {
  MaskHarness h;
  h.vm.setProjectionLimits({.maxRules = 0});
  const auto opcode = h.mask();
  h.pair(opcode, h.core.dims().role, 0);
  const auto result = h.vm.step(h.vm.spawnCursor(opcode));
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.errorMessage.find("limit"), std::string::npos);
  h.expectUnchanged();
}

TEST(VortexMaskLinksTest, CellReferenceOwnerResolvesBeforeMasking) {
  MaskHarness h;
  const auto slot = h.arena.makeCell();
  const auto copy = h.vm.mintOpcode(OpcodeKind::CopyCellRefs);
  h.core.bindInput(copy, h.owner);
  h.core.bindOutput(copy, slot);
  const auto mask = h.vm.mintOpcode(OpcodeKind::VqlMaskLinks);
  h.core.bindInput(mask, slot);
  h.core.bindInput(mask, h.arena.makeScalarCell(false));
  zigzag::expectWritten(
      h.arena.link(copy, h.core.dims().spin, DimVector::POS, mask));
  const auto cursor = h.vm.spawnCursor(copy);
  ASSERT_TRUE(h.vm.step(cursor).success);
  const auto result = h.vm.step(cursor);
  ASSERT_TRUE(result.success) << result.errorMessage;
  EXPECT_FALSE(h.arena.linked(h.owner, h.core.dims().role, DimVector::POS));
  EXPECT_FALSE(h.arena.linked(h.neighbor, h.core.dims().role, DimVector::NEG));
}
} // namespace
} // namespace zigzag::vortex

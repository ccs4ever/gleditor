#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <string>

#include "common/xanadu/scalar.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {
using zigzag::CellRef;
using zigzag::vortex::OpcodeKind;

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
};

TEST(VortexCurrentTimestampTest, ReturnsFreshTypedCellWithCanonicalUtcText) {
  Harness h;
  const CellRef output = h.arena.makeCell();
  const CellRef opcode = h.vm.mintOpcode(OpcodeKind::CurrentTimestamp);
  h.core.bindOutput(opcode, output);
  const CellRef cursor = h.vm.spawnCursor(opcode);

  ASSERT_TRUE(h.vm.step(cursor).success);
  const auto first = h.vm.resultCell(output);
  ASSERT_TRUE(first);
  EXPECT_EQ(h.arena.valueKindOf(*first), xanadu::ValueKind::Timestamp);
  const auto firstNanos =
      std::bit_cast<std::int64_t>(h.arena.slot(*first)->valueBits);
  EXPECT_EQ(h.arena.textOf(*first),
            xanadu::formatUtcTimestampIso8601(firstNanos));

  h.vm.setCursorOpcode(cursor, opcode);
  ASSERT_TRUE(h.vm.step(cursor).success);
  const auto second = h.vm.resultCell(output);
  ASSERT_TRUE(second);
  EXPECT_NE(*first, *second);
  EXPECT_EQ(h.arena.valueKindOf(*second), xanadu::ValueKind::Timestamp);
  const auto secondNanos =
      std::bit_cast<std::int64_t>(h.arena.slot(*second)->valueBits);
  EXPECT_EQ(h.arena.textOf(*second),
            xanadu::formatUtcTimestampIso8601(secondNanos));
}

TEST(VortexCurrentTimestampTest,
     RefusesMalformedOperandsWithoutChangingOutput) {
  Harness h;
  const CellRef output = h.arena.makeCell();
  const CellRef opcode = h.vm.mintOpcode(OpcodeKind::CurrentTimestamp);
  h.core.bindOutput(opcode, output);
  const CellRef cursor = h.vm.spawnCursor(opcode);
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_TRUE(h.vm.resultCell(output));
  const auto oldResult = h.vm.resultCell(output);

  const CellRef input = h.arena.makeCell("unexpected");
  h.core.bindInput(opcode, input);
  h.vm.setCursorOpcode(cursor, opcode);
  const auto failed = h.vm.step(cursor);
  EXPECT_FALSE(failed.success);
  EXPECT_EQ(h.vm.resultCell(output), oldResult);
}

TEST(VortexCurrentTimestampTest,
     WorkBudgetRefusalLeavesOutputAndArenaRetryable) {
  Harness h;
  const CellRef output = h.arena.makeCell("sentinel");
  const CellRef opcode = h.vm.mintOpcode(OpcodeKind::CurrentTimestamp);
  h.core.bindOutput(opcode, output);
  zigzag::expectWritten(
      h.arena.link(opcode, h.core.dims().spin, zigzag::DimVector::POS, opcode));
  const CellRef cursor    = h.vm.spawnCursor(opcode);
  const auto initialCells = h.arena.cells().size();

  h.arena.setExecutionLimits(1000, 0, 1000);
  const auto refused = h.vm.step(cursor);
  EXPECT_FALSE(refused.success);
  EXPECT_EQ(h.arena.cells().size(), initialCells);
  EXPECT_EQ(h.arena.textOf(output), "sentinel");
  EXPECT_FALSE(h.vm.cellReferenceOf(output));

  h.arena.setExecutionLimits(1000, 100, 1000);
  const auto retried = h.vm.step(cursor);
  ASSERT_TRUE(retried.success) << retried.errorMessage;
  EXPECT_TRUE(h.vm.cellReferenceOf(output));
  EXPECT_EQ(h.arena.valueKindOf(*h.vm.cellReferenceOf(output)),
            xanadu::ValueKind::Timestamp);
}
} // namespace

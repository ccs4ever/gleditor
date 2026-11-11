/**
 * @file vortex_dim_by_name_test.cpp
 * @brief DimByName: a run-time dimension name to the dimension's number, with
 *        or without minting.
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace zigzag::vortex {
namespace {

struct Harness {
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};

  struct Outcome {
    ExecutionResult result;
    CellRef output;
  };

  Outcome run(const CellRef name, const CellRef flag) {
    const auto op     = vm.mintOpcode(OpcodeKind::DimByName);
    const auto output = arena.makeCell("dimension");
    core.bindInput(op, name);
    core.bindInput(op, flag);
    core.bindOutput(op, output);
    return {vm.step(vm.spawnCursor(op)), output};
  }

  CellRef text(const std::string &value) { return arena.makeCell(value); }
};

TEST(VortexDimByNameTest, MintsAnUnknownNameWhenTheFlagSaysSo) {
  Harness h;
  ASSERT_FALSE(h.arena.dimensionNamed("d.lace"));
  const auto outcome = h.run(h.text("d.lace"), h.arena.makeScalarCell(true));
  ASSERT_TRUE(outcome.result.success) << outcome.result.errorMessage;
  const auto named = h.arena.dimensionNamed("d.lace");
  ASSERT_TRUE(named);
  EXPECT_EQ(h.core.render(outcome.output),
            CellValue{static_cast<std::int64_t>(*named)});
}

TEST(VortexDimByNameTest, FindsAKnownNameWithoutMintingAgain) {
  Harness h;
  const auto existing = h.arena.ensureDimension("d.lace");
  const auto before   = h.arena.dimensions().size();
  const auto outcome  = h.run(h.text("d.lace"), h.arena.makeScalarCell(false));
  ASSERT_TRUE(outcome.result.success) << outcome.result.errorMessage;
  EXPECT_EQ(h.core.render(outcome.output),
            CellValue{static_cast<std::int64_t>(existing)});
  EXPECT_EQ(h.arena.dimensions().size(), before);
  EXPECT_EQ(h.arena.dimensionNamed("d.lace"), existing);
}

TEST(VortexDimByNameTest, MintFlagOnAKnownNameReturnsTheSameDimension) {
  Harness h;
  const auto existing = h.arena.ensureDimension("d.lace");
  const auto outcome =
      h.run(h.text("d.lace"), h.arena.makeScalarCell(std::int64_t{1}));
  ASSERT_TRUE(outcome.result.success) << outcome.result.errorMessage;
  EXPECT_EQ(h.core.render(outcome.output),
            CellValue{static_cast<std::int64_t>(existing)});
}

TEST(VortexDimByNameTest, UnknownNameWithoutMintReportsAbsenceAndMintsNothing) {
  Harness h;
  const auto before = h.arena.dimensions().size();
  const auto outcome =
      h.run(h.text("d.absent"), h.arena.makeScalarCell(std::int64_t{0}));
  ASSERT_TRUE(outcome.result.success) << outcome.result.errorMessage;
  // The Int64 box carries noCell, the way Deref and ProbeLink report absence.
  EXPECT_EQ(h.core.render(outcome.output),
            CellValue{static_cast<std::int64_t>(noCell)});
  EXPECT_FALSE(h.arena.dimensionNamed("d.absent"));
  EXPECT_EQ(h.arena.dimensions().size(), before);
}

TEST(VortexDimByNameTest, OutputFeedsLinkAsADimensionOperand) {
  Harness h;
  const auto from    = h.arena.makeCell("from");
  const auto to      = h.arena.makeCell("to");
  const auto dimOp   = h.vm.mintOpcode(OpcodeKind::DimByName);
  const auto dimSlot = h.arena.makeCell("dim slot");
  h.core.bindInput(dimOp, h.text("d.made"));
  h.core.bindInput(dimOp, h.arena.makeScalarCell(true));
  h.core.bindOutput(dimOp, dimSlot);
  const auto link = h.vm.mintOpcode(OpcodeKind::Link);
  h.core.bindInput(link, from);
  h.core.bindInput(link, dimSlot);
  h.core.bindInput(link, h.arena.makeScalarCell(std::int64_t{1}));
  h.core.bindInput(link, to);
  h.core.bindOutput(link, h.arena.makeCell("link result"));
  ASSERT_TRUE(h.arena.link(dimOp, h.core.dims().spin, DimVector::POS, link));
  const auto halt = h.vm.mintOpcode(OpcodeKind::Halt);
  ASSERT_TRUE(h.arena.link(link, h.core.dims().spin, DimVector::POS, halt));
  const auto executed = h.vm.run(h.vm.spawnCursor(dimOp), 100);
  ASSERT_TRUE(executed.success) << executed.errorMessage;
  const auto made = h.arena.dimensionNamed("d.made");
  ASSERT_TRUE(made);
  EXPECT_EQ(h.arena.linked(from, *made, DimVector::POS), to);
}

TEST(VortexDimByNameTest, RefusesAnEmptyName) {
  Harness h;
  const auto outcome = h.run(h.text(""), h.arena.makeScalarCell(true));
  EXPECT_FALSE(outcome.result.success);
  EXPECT_NE(outcome.result.errorMessage.find("DimByName"), std::string::npos);
}

TEST(VortexDimByNameTest, RefusesANonTextName) {
  Harness h;
  const auto outcome = h.run(h.arena.makeScalarCell(std::int64_t{7}),
                             h.arena.makeScalarCell(true));
  EXPECT_FALSE(outcome.result.success);
  EXPECT_NE(outcome.result.errorMessage.find("DimByName"), std::string::npos);
}

TEST(VortexDimByNameTest, RefusesAMintFlagThatIsNotBoolOrInt) {
  Harness h;
  const auto outcome = h.run(h.text("d.lace"), h.text("yes"));
  EXPECT_FALSE(outcome.result.success);
  EXPECT_NE(outcome.result.errorMessage.find("mint flag"), std::string::npos);
  EXPECT_FALSE(h.arena.dimensionNamed("d.lace"));
}

TEST(VortexDimByNameTest, RefusesTheWrongNumberOfInputs) {
  Harness h;
  const auto op     = h.vm.mintOpcode(OpcodeKind::DimByName);
  const auto output = h.arena.makeCell("dimension");
  h.core.bindInput(op, h.text("d.lace"));
  h.core.bindOutput(op, output);
  const auto result = h.vm.step(h.vm.spawnCursor(op));
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.errorMessage.find("DimByName"), std::string::npos);
}

TEST(VortexDimByNameTest, CannotRunAsACondition) {
  Harness h;
  const auto condition = h.vm.mintOpcode(OpcodeKind::DimByName);
  h.core.bindInput(condition, h.text("d.lace"));
  h.core.bindInput(condition, h.arena.makeScalarCell(true));
  const auto guarded = h.vm.mintOpcode(OpcodeKind::Nop);
  h.core.attachPrecondition(guarded, condition);
  const auto result = h.vm.step(h.vm.spawnCursor(guarded));
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.errorMessage.find("cannot run as conditions"),
            std::string::npos);
}

} // namespace
} // namespace zigzag::vortex

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
  // The core files it, so the core sees it; the arena's own table does not.
  const auto named = h.core.findDimension("d.lace");
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
  const auto made = h.core.findDimension("d.made");
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

TEST(VortexDimByNameTest, CoreMintedNamesAreFoundWithoutMinting) {
  Harness h;
  const auto before = h.arena.cellCount();
  for (const auto &[name, expected] :
       {std::pair{std::string{"d.grab"}, h.core.dims().grab},
        std::pair{std::string{"d.spin"}, h.core.dims().spin}}) {
    const auto outcome = h.run(h.text(name), h.arena.makeScalarCell(false));
    ASSERT_TRUE(outcome.result.success) << outcome.result.errorMessage;
    EXPECT_EQ(h.core.render(outcome.output),
              CellValue{static_cast<std::int64_t>(expected)})
        << name;
  }
  EXPECT_GT(h.arena.cellCount(), before);
}

TEST(VortexDimByNameTest, MintFlagNeverDuplicatesACoreDimension) {
  Harness h;
  const auto outcome = h.run(h.text("d.grab"), h.arena.makeScalarCell(true));
  ASSERT_TRUE(outcome.result.success) << outcome.result.errorMessage;
  EXPECT_EQ(h.core.render(outcome.output),
            CellValue{static_cast<std::int64_t>(h.core.dims().grab)});
  EXPECT_EQ(h.core.findDimension("d.grab"),
            std::optional<DimRef>{h.core.dims().grab});
}

TEST(VortexDimByNameTest, ADimensionMintedHereIsOneDimensionForTheCore) {
  Harness h;
  const auto first = h.run(h.text("d.fresh"), h.arena.makeScalarCell(true));
  ASSERT_TRUE(first.result.success) << first.result.errorMessage;
  const auto found = h.core.findDimension("d.fresh");
  ASSERT_TRUE(found);
  EXPECT_EQ(h.core.render(first.output),
            CellValue{static_cast<std::int64_t>(*found)});
  const auto again = h.run(h.text("d.fresh"), h.arena.makeScalarCell(false));
  ASSERT_TRUE(again.result.success) << again.result.errorMessage;
  EXPECT_EQ(h.core.render(again.output),
            CellValue{static_cast<std::int64_t>(*found)});
}

TEST(VortexDimByNameTest, AMintUnderAChoiceIsUndoneByFail) {
  Harness h;
  const auto choice = h.vm.mintOpcode(OpcodeKind::Choice);
  const auto mint   = h.vm.mintOpcode(OpcodeKind::DimByName);
  const auto fail   = h.vm.mintOpcode(OpcodeKind::Fail);
  const auto alt    = h.vm.mintOpcode(OpcodeKind::Halt);
  h.core.bindInput(choice,
                   h.arena.makeScalarCell(static_cast<std::int64_t>(alt)));
  h.core.bindInput(mint, h.text("d.undone"));
  h.core.bindInput(mint, h.arena.makeScalarCell(true));
  h.core.bindOutput(mint, h.arena.makeCell("dimension"));
  ASSERT_TRUE(h.arena.link(choice, h.core.dims().spin, DimVector::POS, mint));
  ASSERT_TRUE(h.arena.link(mint, h.core.dims().spin, DimVector::POS, fail));
  const auto cursor   = h.vm.spawnCursor(choice);
  const auto cells    = h.arena.cellCount();
  const auto executed = h.vm.run(cursor, 20);
  ASSERT_TRUE(executed.success) << executed.errorMessage;
  EXPECT_FALSE(h.core.findDimension("d.undone"));
  EXPECT_FALSE(h.arena.dimensionNamed("d.undone"));
  EXPECT_EQ(h.arena.cellCount(), cells);
}
} // namespace
} // namespace zigzag::vortex

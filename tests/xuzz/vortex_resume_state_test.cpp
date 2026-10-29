#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <vector>

#include "common/xanadu/vortex/vortex_vm.hpp"

namespace {
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
};

void linkSequence(Harness &h, const std::vector<CellRef> &opcodes) {
  for (std::size_t i = 1; i < opcodes.size(); ++i) {
    zigzag::expectWritten(h.arena.link(opcodes[i - 1], h.core.dims().spin,
                                       DimVector::POS, opcodes[i]));
  }
}

TEST(VortexResumeStateTest,
     ResumeValueSurvivesBacktrackWhileArenaSlotsRollback) {
  Harness h;
  const auto initial      = h.arena.makeScalarCell(std::int64_t{1});
  const auto ten          = h.arena.makeScalarCell(std::int64_t{10});
  const auto two          = h.arena.makeScalarCell(std::int64_t{2});
  const auto three        = h.arena.makeScalarCell(std::int64_t{3});
  const auto zero         = h.arena.makeScalarCell(std::int64_t{0});
  const auto token        = h.arena.makeCell();
  const auto ordinarySlot = h.arena.makeScalarCell(std::int64_t{91});
  const auto observed     = h.arena.makeCell();

  const auto init        = h.vm.mintOpcode(OpcodeKind::ResumeInit);
  const auto choice      = h.vm.mintOpcode(OpcodeKind::Choice);
  const auto set         = h.vm.mintOpcode(OpcodeKind::ResumeSet);
  const auto ordinaryAdd = h.vm.mintOpcode(OpcodeKind::Add);
  const auto fail        = h.vm.mintOpcode(OpcodeKind::Fail);
  const auto resumeAdd   = h.vm.mintOpcode(OpcodeKind::Add);
  const auto halt        = h.vm.mintOpcode(OpcodeKind::Halt);
  linkSequence(h, {init, choice, set, ordinaryAdd, fail, resumeAdd, halt});
  h.core.bindInput(
      choice, h.arena.makeScalarCell(static_cast<std::int64_t>(resumeAdd)));
  h.core.bindInput(init, initial);
  h.core.bindOutput(init, token);
  h.core.bindInput(set, token);
  h.core.bindInput(set, ten);
  h.core.bindInput(ordinaryAdd, two);
  h.core.bindInput(ordinaryAdd, three);
  h.core.bindOutput(ordinaryAdd, ordinarySlot);
  h.core.bindInput(resumeAdd, token);
  h.core.bindInput(resumeAdd, zero);
  h.core.bindOutput(resumeAdd, observed);

  const auto cursor = h.vm.spawnCursor(init, "resume-backtrack");
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_TRUE(h.vm.resultCell(token));
  EXPECT_EQ(h.arena.valueKindOf(*h.vm.resultCell(token)),
            xanadu::ValueKind::ExternRef);
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_EQ(h.arena.asInt64(ordinarySlot), std::optional{std::int64_t{91}});
  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_EQ(h.arena.asInt64(ordinarySlot), std::optional{std::int64_t{5}});
  const auto failed = h.vm.step(cursor);
  EXPECT_TRUE(failed.success) << failed.errorMessage;
  EXPECT_EQ(h.arena.asInt64(ordinarySlot), std::optional{std::int64_t{91}});
  ASSERT_EQ(h.vm.getCursorOpcode(cursor), resumeAdd);

  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_EQ(h.arena.asInt64(observed), std::optional{std::int64_t{10}});
}

TEST(VortexResumeStateTest, ReinitializationRestoresTheOlderTokenOnBacktrack) {
  Harness h;
  const auto outerInitial = h.arena.makeScalarCell(std::int64_t{3});
  const auto innerInitial = h.arena.makeScalarCell(std::int64_t{7});
  const auto outerValue   = h.arena.makeScalarCell(std::int64_t{30});
  const auto innerValue   = h.arena.makeScalarCell(std::int64_t{70});
  const auto zero         = h.arena.makeScalarCell(std::int64_t{0});
  const auto token        = h.arena.makeCell();
  const auto innerSeen    = h.arena.makeCell();
  const auto outerSeen    = h.arena.makeCell();

  const auto outerInit = h.vm.mintOpcode(OpcodeKind::ResumeInit);
  const auto choice    = h.vm.mintOpcode(OpcodeKind::Choice);
  const auto outerSet  = h.vm.mintOpcode(OpcodeKind::ResumeSet);
  const auto innerInit = h.vm.mintOpcode(OpcodeKind::ResumeInit);
  const auto readInner = h.vm.mintOpcode(OpcodeKind::Add);
  const auto innerSet  = h.vm.mintOpcode(OpcodeKind::ResumeSet);
  const auto fail      = h.vm.mintOpcode(OpcodeKind::Fail);
  const auto readOuter = h.vm.mintOpcode(OpcodeKind::Add);
  const auto halt      = h.vm.mintOpcode(OpcodeKind::Halt);
  linkSequence(h, {outerInit, choice, outerSet, innerInit, readInner, innerSet,
                   fail, readOuter, halt});
  h.core.bindInput(
      choice, h.arena.makeScalarCell(static_cast<std::int64_t>(readOuter)));
  h.core.bindInput(outerInit, outerInitial);
  h.core.bindOutput(outerInit, token);
  h.core.bindInput(outerSet, token);
  h.core.bindInput(outerSet, outerValue);
  h.core.bindInput(innerInit, innerInitial);
  h.core.bindOutput(innerInit, token);
  h.core.bindInput(readInner, token);
  h.core.bindInput(readInner, zero);
  h.core.bindOutput(readInner, innerSeen);
  h.core.bindInput(innerSet, token);
  h.core.bindInput(innerSet, innerValue);
  h.core.bindInput(readOuter, token);
  h.core.bindInput(readOuter, zero);
  h.core.bindOutput(readOuter, outerSeen);

  const auto cursor = h.vm.spawnCursor(outerInit, "resume-generation");
  for (int step = 0; step < 5; ++step) {
    const auto result = h.vm.step(cursor);
    ASSERT_TRUE(result.success) << result.errorMessage;
  }
  EXPECT_EQ(h.arena.asInt64(innerSeen), std::optional{std::int64_t{7}});
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_EQ(h.vm.getCursorOpcode(cursor), readOuter);
  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_EQ(h.arena.asInt64(outerSeen), std::optional{std::int64_t{30}});
}

TEST(VortexResumeStateTest, ResumeValuesAreIsolatedByCursorAndReleasedOnClose) {
  Harness h;
  const auto one     = h.arena.makeScalarCell(std::int64_t{1});
  const auto two     = h.arena.makeScalarCell(std::int64_t{2});
  const auto ten     = h.arena.makeScalarCell(std::int64_t{10});
  const auto twenty  = h.arena.makeScalarCell(std::int64_t{20});
  const auto zero    = h.arena.makeScalarCell(std::int64_t{0});
  const auto tokenA  = h.arena.makeCell();
  const auto tokenB  = h.arena.makeCell();
  const auto resultA = h.arena.makeCell();
  const auto resultB = h.arena.makeCell();

  const auto initA = h.vm.mintOpcode(OpcodeKind::ResumeInit);
  const auto setA  = h.vm.mintOpcode(OpcodeKind::ResumeSet);
  const auto addA  = h.vm.mintOpcode(OpcodeKind::Add);
  const auto initB = h.vm.mintOpcode(OpcodeKind::ResumeInit);
  const auto setB  = h.vm.mintOpcode(OpcodeKind::ResumeSet);
  const auto addB  = h.vm.mintOpcode(OpcodeKind::Add);
  linkSequence(h, {initA, setA, addA});
  linkSequence(h, {initB, setB, addB});
  h.core.bindInput(initA, one);
  h.core.bindOutput(initA, tokenA);
  h.core.bindInput(setA, tokenA);
  h.core.bindInput(setA, ten);
  h.core.bindInput(addA, tokenA);
  h.core.bindInput(addA, zero);
  h.core.bindOutput(addA, resultA);
  h.core.bindInput(initB, two);
  h.core.bindOutput(initB, tokenB);
  h.core.bindInput(setB, tokenB);
  h.core.bindInput(setB, twenty);
  h.core.bindInput(addB, tokenB);
  h.core.bindInput(addB, zero);
  h.core.bindOutput(addB, resultB);

  const auto cursorA = h.vm.spawnCursor(initA, "resume-A");
  const auto cursorB = h.vm.spawnCursor(initB, "resume-B");
  ASSERT_TRUE(h.vm.step(cursorA).success);
  ASSERT_TRUE(h.vm.step(cursorB).success);
  EXPECT_TRUE(h.vm.hasResumeState(cursorA));
  EXPECT_TRUE(h.vm.hasResumeState(cursorB));
  ASSERT_TRUE(h.vm.step(cursorA).success);
  ASSERT_TRUE(h.vm.step(cursorB).success);
  ASSERT_TRUE(h.vm.step(cursorA).success);
  EXPECT_FALSE(h.vm.hasResumeState(cursorA));
  EXPECT_TRUE(h.vm.hasResumeState(cursorB));
  ASSERT_TRUE(h.vm.step(cursorB).success);
  EXPECT_EQ(h.arena.asInt64(resultA), std::optional{std::int64_t{10}});
  EXPECT_EQ(h.arena.asInt64(resultB), std::optional{std::int64_t{20}});
  EXPECT_FALSE(h.vm.hasResumeState(cursorB));
}

TEST(VortexResumeStateTest, ForeignCursorAndClosedTokenAreRejected) {
  Harness h;
  const auto initial = h.arena.makeScalarCell(std::int64_t{4});
  const auto zero    = h.arena.makeScalarCell(std::int64_t{0});
  const auto token   = h.arena.makeCell();
  const auto output  = h.arena.makeCell();
  const auto init    = h.vm.mintOpcode(OpcodeKind::ResumeInit);
  const auto add     = h.vm.mintOpcode(OpcodeKind::Add);
  const auto halt    = h.vm.mintOpcode(OpcodeKind::Halt);
  zigzag::expectWritten(
      h.arena.link(init, h.core.dims().spin, DimVector::POS, halt));
  h.core.bindInput(init, initial);
  h.core.bindOutput(init, token);
  h.core.bindInput(add, token);
  h.core.bindInput(add, zero);
  h.core.bindOutput(add, output);
  const auto owner = h.vm.spawnCursor(init, "token-owner");
  ASSERT_TRUE(h.vm.step(owner).success);
  ASSERT_TRUE(h.vm.hasResumeState(owner));
  const auto ownerToken = h.vm.cellReferenceOf(token);
  ASSERT_TRUE(ownerToken);

  const auto sameSite    = h.vm.spawnCursor(init, "same-output-site");
  const auto refusedInit = h.vm.step(sameSite);
  EXPECT_FALSE(refusedInit.success);
  EXPECT_TRUE(h.vm.hasResumeState(owner));
  EXPECT_FALSE(h.vm.hasResumeState(sameSite));
  EXPECT_EQ(h.vm.cellReferenceOf(token), ownerToken);

  const auto foreign  = h.vm.spawnCursor(add, "foreign-token-user");
  const auto rejected = h.vm.step(foreign);
  EXPECT_FALSE(rejected.success);
  EXPECT_FALSE(h.vm.hasResumeState(foreign));

  h.vm.setCursorOpcode(owner, std::nullopt);
  EXPECT_FALSE(h.vm.hasResumeState(owner));
  const auto stale = h.vm.spawnCursor(add, "closed-token-user");
  EXPECT_FALSE(h.vm.step(stale).success);

  const auto five         = h.arena.makeScalarCell(std::int64_t{5});
  const auto zeroForReuse = h.arena.makeScalarCell(std::int64_t{0});
  const auto write        = h.vm.mintOpcode(OpcodeKind::Add);
  const auto read         = h.vm.mintOpcode(OpcodeKind::Add);
  const auto result       = h.arena.makeCell();
  h.core.bindInput(write, five);
  h.core.bindInput(write, zeroForReuse);
  h.core.bindOutput(write, token);
  h.core.bindInput(read, token);
  h.core.bindInput(read, zeroForReuse);
  h.core.bindOutput(read, result);
  const auto writer = h.vm.spawnCursor(write, "reuse-token-slot");
  ASSERT_TRUE(h.vm.step(writer).success);
  const auto reader = h.vm.spawnCursor(read, "read-reused-slot");
  ASSERT_TRUE(h.vm.step(reader).success);
  EXPECT_EQ(h.arena.asInt64(result), std::optional{std::int64_t{5}});
}

TEST(VortexResumeStateTest,
     ResumeOpcodesRefusePipelinesConditionsAndMemoization) {
  const auto checkRefused = [](const bool pipeline, const bool condition,
                               const bool memoized) {
    Harness h;
    const auto init    = h.vm.mintOpcode(OpcodeKind::ResumeInit);
    const auto initial = h.arena.makeScalarCell(std::int64_t{1});
    const auto token   = h.arena.makeCell();
    h.core.bindInput(init, initial);
    h.core.bindOutput(init, token);
    if (pipeline) {
      h.core.attachPipeline(initial, h.vm.mintOpcode(OpcodeKind::Nop));
    }
    if (condition) {
      h.core.attachPrecondition(init, h.vm.mintOpcode(OpcodeKind::Nop));
    }
    if (memoized) h.vm.enableMemoization(init, "resume-init");

    const auto cursor = h.vm.spawnCursor(init, "resume-refusal");
    const auto result = h.vm.step(cursor);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(h.vm.hasResumeState(cursor));
    EXPECT_EQ(h.vm.choiceDepth(), 0U);
  };

  checkRefused(true, false, false);
  checkRefused(false, true, false);
  checkRefused(false, false, true);
}

TEST(VortexResumeStateTest, ResumeSetRefusesPipelinesConditionsAndMemoization) {
  const auto checkRefused = [](const bool pipeline, const bool condition,
                               const bool memoized) {
    Harness h;
    const auto initial = h.arena.makeScalarCell(std::int64_t{1});
    const auto value   = h.arena.makeScalarCell(std::int64_t{2});
    const auto token   = h.arena.makeCell();
    const auto init    = h.vm.mintOpcode(OpcodeKind::ResumeInit);
    const auto set     = h.vm.mintOpcode(OpcodeKind::ResumeSet);
    const auto halt    = h.vm.mintOpcode(OpcodeKind::Halt);
    linkSequence(h, {init, set, halt});
    h.core.bindInput(init, initial);
    h.core.bindOutput(init, token);
    h.core.bindInput(set, token);
    h.core.bindInput(set, value);
    if (pipeline) {
      h.core.attachPipeline(value, h.vm.mintOpcode(OpcodeKind::Nop));
    }
    if (condition) {
      h.core.attachPrecondition(set, h.vm.mintOpcode(OpcodeKind::Nop));
    }
    if (memoized) h.vm.enableMemoization(set, "resume-set");

    const auto cursor = h.vm.spawnCursor(init, "resume-set-refusal");
    ASSERT_TRUE(h.vm.step(cursor).success);
    ASSERT_TRUE(h.vm.hasResumeState(cursor));
    const auto result = h.vm.step(cursor);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(h.vm.hasResumeState(cursor));
  };

  checkRefused(true, false, false);
  checkRefused(false, true, false);
  checkRefused(false, false, true);
}

} // namespace

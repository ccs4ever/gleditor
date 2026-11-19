// What these tests assert: the size of the undo journal a frame opcode keeps,
// read through VortexVM::frameTableEntriesCopied(), is the same on every call
// of the same program, however many runs finished before it and however large
// the VM's unrelated tables are. The tests need that counter, so they do not
// compile against the code from before the journal (which had no such counter);
// the failing-first evidence for the old behaviour is the same program with the
// counter added to the old whole-table copies (16 entries on the first run,
// 488 by run 60 in the nested case), recorded in commit 19e2c3cb. Exactness of
// the rollback itself is vortex_frame_rollback_test.cpp. Counts only; nothing
// here asserts on time.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "vortex_frame_test_access.hpp"

namespace zigzag::vortex {
namespace {

using zigzag::CellRef;
using zigzag::DimVector;

/// Runs of the same tiny program; large enough that a copy that grows with the
/// run count is far above the per-run constant.
constexpr std::size_t finishedRuns = 200;
/// Runs the frame depth tests make, each through a nested call.
constexpr std::size_t nestedRuns     = 60;
constexpr std::int64_t firstArgument = 40;

struct Harness {
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  CellRef one{arena.makeScalarCell(std::int64_t{1})};

  CellRef op(const OpcodeKind kind) { return vm.mintOpcode(kind); }
  void next(const CellRef from, const CellRef to) {
    ASSERT_TRUE(arena.link(from, core.dims().spin, DimVector::POS, to));
  }

  CellRef descriptor(const CellRef body, const CellRef argument,
                     const CellRef result, std::vector<CellRef> extra = {}) {
    std::vector<CellRef> locals{argument, result};
    locals.insert(locals.end(), extra.begin(), extra.end());
    const auto made = vm.makeFrameDescriptor(FrameDescriptorSpec{
        .entry         = body,
        .locals        = std::move(locals),
        .argumentPorts = {argument},
        .resultPorts   = {result},
    });
    EXPECT_TRUE(made);
    return made.value_or(noCell);
  }

  CellRef addOneFrame() {
    const auto argument = arena.makeCell("argument");
    const auto result   = arena.makeCell("result");
    const auto add      = op(OpcodeKind::Add);
    core.bindInput(add, argument);
    core.bindInput(add, one);
    core.bindOutput(add, result);
    const auto ret = op(OpcodeKind::ReturnFrame);
    next(add, ret);
    const auto desc = descriptor(add, argument, result);
    core.bindInput(ret, desc);
    return desc;
  }

  /// A frame that calls addOneFrame and adds one to its answer.
  CellRef addTwoFrame(const CellRef inner) {
    const auto argument  = arena.makeCell("outer argument");
    const auto temporary = arena.makeCell("outer temporary");
    const auto result    = arena.makeCell("outer result");
    const auto nested    = op(OpcodeKind::InvokeFrame);
    core.bindInput(nested, inner);
    core.bindInput(nested, argument);
    core.bindOutput(nested, temporary);
    const auto add = op(OpcodeKind::Add);
    core.bindInput(add, temporary);
    core.bindInput(add, one);
    core.bindOutput(add, result);
    const auto ret = op(OpcodeKind::ReturnFrame);
    next(nested, add);
    next(add, ret);
    const auto desc = descriptor(nested, argument, result, {temporary});
    core.bindInput(ret, desc);
    return desc;
  }

  /// One fresh host program: call the frame once, then halt, on its own cursor
  /// so that every run leaves cells, references and a cursor behind as a host
  /// that runs the VM once per step does.
  std::pair<CellRef, CellRef> freshRun(const CellRef frame,
                                       const std::int64_t value) {
    const auto call = op(OpcodeKind::InvokeFrame);
    const auto out  = arena.makeCell("run result");
    core.bindInput(call, frame);
    core.bindInput(call, arena.makeScalarCell(value));
    core.bindOutput(call, out);
    const auto halt = op(OpcodeKind::Halt);
    next(call, halt);
    return {vm.spawnCursor(call, "journal_test"), out};
  }
};

std::vector<std::size_t> copiedPerRun(Harness &h, const CellRef frame,
                                      const std::size_t runs,
                                      const std::int64_t expectedOffset) {
  std::vector<std::size_t> perRun;
  for (std::size_t run = 0; run < runs; ++run) {
    const auto value         = firstArgument + static_cast<std::int64_t>(run);
    const auto before        = h.vm.frameTableEntriesCopied();
    const auto [cursor, out] = h.freshRun(frame, value);
    const auto execution     = h.vm.run(cursor, 100);
    EXPECT_TRUE(execution.success) << execution.errorMessage;
    const auto answer = h.vm.resultCell(cursor, out);
    EXPECT_TRUE(answer.has_value());
    if (answer)
      EXPECT_EQ(h.core.render(*answer),
                CellValue{std::int64_t{value + expectedOffset}});
    perRun.push_back(h.vm.frameTableEntriesCopied() - before);
  }
  return perRun;
}

TEST(VortexFrameJournalTest, TheCostOfACallDoesNotGrowWithFinishedRuns) {
  Harness h;
  const auto frame  = h.addOneFrame();
  const auto perRun = copiedPerRun(h, frame, finishedRuns, 1);
  ASSERT_EQ(perRun.size(), finishedRuns);
  const auto [lowest, highest] = std::ranges::minmax(perRun);
  EXPECT_EQ(lowest, highest)
      << "first run copied " << perRun.front() << " entries, run "
      << finishedRuns << " copied " << perRun.back();
}

TEST(VortexFrameJournalTest, TheCostOfANestedCallDoesNotGrowWithFinishedRuns) {
  Harness h;
  const auto frame  = h.addTwoFrame(h.addOneFrame());
  const auto perRun = copiedPerRun(h, frame, nestedRuns, 2);
  ASSERT_EQ(perRun.size(), nestedRuns);
  const auto [lowest, highest] = std::ranges::minmax(perRun);
  EXPECT_EQ(lowest, highest)
      << "first run copied " << perRun.front() << " entries, run " << nestedRuns
      << " copied " << perRun.back();
}

TEST(VortexFrameJournalTest, ARerunCursorCostsTheSameAsAFreshOne) {
  Harness h;
  const auto frame = h.addOneFrame();
  const auto call  = h.op(OpcodeKind::InvokeFrame);
  const auto out   = h.arena.makeCell("rerun result");
  const auto arg   = h.arena.makeScalarCell(std::int64_t{firstArgument});
  h.core.bindInput(call, frame);
  h.core.bindInput(call, arg);
  h.core.bindOutput(call, out);
  const auto halt = h.op(OpcodeKind::Halt);
  h.next(call, halt);
  const auto cursor = h.vm.spawnCursor(call, "rerun");
  std::vector<std::size_t> perRun;
  for (std::size_t run = 0; run < finishedRuns; ++run) {
    const auto before = h.vm.frameTableEntriesCopied();
    h.vm.setCursorOpcode(cursor, call);
    const auto execution = h.vm.run(cursor, 100);
    ASSERT_TRUE(execution.success) << execution.errorMessage;
    perRun.push_back(h.vm.frameTableEntriesCopied() - before);
  }
  const auto [lowest, highest] = std::ranges::minmax(perRun);
  EXPECT_EQ(lowest, highest);
}

TEST(VortexFrameJournalTest, TheCostOfCreatingAClosureDoesNotGrowWithRuns) {
  Harness h;
  const auto ret = h.op(OpcodeKind::ReturnFrame);
  const auto frame =
      h.vm.makeFrameDescriptor(FrameDescriptorSpec{.entry = ret});
  ASSERT_TRUE(frame);
  h.core.bindInput(ret, *frame);
  const auto closure = h.vm.makeClosureDescriptor(ClosureDescriptorSpec{
      .frameDescriptor = *frame,
  });
  ASSERT_TRUE(closure);
  std::vector<std::size_t> perRun;
  for (std::size_t run = 0; run < finishedRuns; ++run) {
    const auto before = h.vm.frameTableEntriesCopied();
    const auto create = h.op(OpcodeKind::CreateClosure);
    const auto handle = h.arena.makeCell("closure handle");
    h.core.bindInput(create, *closure);
    h.core.bindOutput(create, handle);
    const auto halt = h.op(OpcodeKind::Halt);
    h.next(create, halt);
    const auto cursor    = h.vm.spawnCursor(create, "closure_journal");
    const auto execution = h.vm.run(cursor, 20);
    ASSERT_TRUE(execution.success) << execution.errorMessage;
    perRun.push_back(h.vm.frameTableEntriesCopied() - before);
  }
  const auto [lowest, highest] = std::ranges::minmax(perRun);
  EXPECT_EQ(lowest, highest);
}

/// Entries one call of a program costs after @p unrelated entries were put in
/// the reference and guard-scope tables, none of them named by the program.
std::size_t costWithUnrelatedEntries(const std::size_t unrelated) {
  Harness h;
  const auto frame = h.addOneFrame();
  // Keys far above every cell the program mints, so none is its own.
  constexpr CellRef firstUnrelated = 1U << 24;
  FrameTestAccess::seedUnrelated(h.vm, unrelated, firstUnrelated);
  const auto before        = h.vm.frameTableEntriesCopied();
  const auto [cursor, out] = h.freshRun(frame, firstArgument);
  const auto execution     = h.vm.run(cursor, 100);
  EXPECT_TRUE(execution.success) << execution.errorMessage;
  return h.vm.frameTableEntriesCopied() - before;
}

TEST(VortexFrameJournalTest, TheCostOfACallDoesNotDependOnUnrelatedTables) {
  constexpr std::size_t manyUnrelated = 10'000;
  EXPECT_EQ(costWithUnrelatedEntries(0),
            costWithUnrelatedEntries(manyUnrelated));
}

} // namespace
} // namespace zigzag::vortex

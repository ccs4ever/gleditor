// The frame opcodes' rollback must cost what a frame changes, not what the VM
// has accumulated. Before the journal, InvokeFrame and ReturnFrame copied every
// VM table on each call, so a host that runs the VM once per step (VLace's lazy
// render and IO loop, the VQL executor) paid for every earlier run on every
// call. These tests assert on counts (table entries copied), never on time.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace zigzag::vortex {
namespace {

using zigzag::CellRef;
using zigzag::DimVector;

/// Runs of the same tiny program; large enough that a copy that grows with the
/// run count is far above the per-run constant.
constexpr std::size_t finishedRuns = 200;
/// Runs the frame depth tests make, each through a nested call.
constexpr std::size_t nestedRuns = 60;
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
    const auto value  = firstArgument + static_cast<std::int64_t>(run);
    const auto before = h.vm.frameTableEntriesCopied();
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
  const auto frame   = h.addOneFrame();
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

} // namespace
} // namespace zigzag::vortex

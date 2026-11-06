// Opt-in timing probe for active runtime guard validation. Run explicitly with
// --gtest_also_run_disabled_tests --gtest_filter='*DISABLED_Scaling'.
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace zigzag::vortex {
namespace {

using Clock = std::chrono::steady_clock;
using zigzag::CellRef;
using zigzag::DimVector;

struct GuardProfileHarness {
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};

  void next(const CellRef from, const CellRef to) {
    ASSERT_TRUE(arena.link(from, core.dims().spin, DimVector::POS, to));
  }

  CellRef makeBoolValidator(const CellRef capturedStream,
                            const CellRef equalValue) {
    const CellRef left    = arena.makeCell("left");
    const CellRef right   = arena.makeCell("right");
    const CellRef capture = arena.makeCell("captures");
    const CellRef result  = arena.makeCell("bool result");
    const CellRef compare = vm.mintOpcode(OpcodeKind::VqlCompare);
    const CellRef ret     = vm.mintOpcode(OpcodeKind::ReturnFrame);
    core.bindInput(compare, equalValue);
    core.bindInput(compare, left);
    core.bindInput(compare, right);
    core.bindOutput(compare, result);
    next(compare, ret);

    FrameDescriptorSpec spec{
        .entry         = compare,
        .locals        = {left, right, capture, result},
        .argumentPorts = {{left, FramePortKind::Cell},
                          {right, FramePortKind::Cell},
                          {capture, FramePortKind::Stream}},
        .resultPorts   = {{result, FramePortKind::Cell}},
    };
    EXPECT_TRUE(arena.contains(capturedStream));
    const auto descriptor = vm.makeFrameDescriptor(spec);
    EXPECT_TRUE(descriptor);
    if (!descriptor) throw std::runtime_error(descriptor.error().message);
    core.bindInput(ret, *descriptor);
    return *descriptor;
  }

  CellRef buildGraph(const CellRef descriptor, const CellRef diagnostic,
                     const CellRef subject, const CellRef capturedStream,
                     const std::size_t guardCount, const bool checkGuards) {
    const CellRef clear = vm.mintOpcode(OpcodeKind::StreamClear);
    core.bindOutput(clear, capturedStream);
    CellRef first    = clear;
    CellRef previous = clear;
    if (captureValues.size() > 0) {
      const CellRef append = vm.mintOpcode(OpcodeKind::StreamAppend);
      for (const CellRef value : captureValues) core.bindInput(append, value);
      core.bindOutput(append, capturedStream);
      next(previous, append);
      previous = append;
    }

    std::vector<CellRef> tokens;
    tokens.reserve(guardCount);
    for (std::size_t i = 0; i < guardCount; ++i) {
      const CellRef push  = vm.mintOpcode(OpcodeKind::PushGuard);
      const CellRef token = arena.makeCell("guard token");
      core.bindInput(push, descriptor);
      core.bindInput(push, diagnostic);
      core.bindInput(push, subject);
      core.bindInput(push, subject);
      core.bindInput(push, capturedStream);
      core.bindOutput(push, token);
      next(previous, push);
      previous = push;
      tokens.push_back(token);
    }

    if (checkGuards) {
      const CellRef check = vm.mintOpcode(OpcodeKind::CheckGuards);
      next(previous, check);
      previous = check;
    }

    for (auto token = tokens.rbegin(); token != tokens.rend(); ++token) {
      const CellRef pop = vm.mintOpcode(OpcodeKind::PopGuard);
      core.bindInput(pop, *token);
      next(previous, pop);
      previous = pop;
    }
    const CellRef halt = vm.mintOpcode(OpcodeKind::Halt);
    next(previous, halt);
    return first;
  }

  std::vector<CellRef> captureValues;
};

void runProfileCase(const std::size_t guardCount,
                    const std::size_t capturedRefsPerGuard,
                    const bool checkGuards) {
  GuardProfileHarness h;
  h.vm.setGuardLimits(GuardLimits{.maxGuards          = 128,
                                  .maxCapturedCells   = 1'048'576,
                                  .maxDiagnosticBytes = 1024});
  h.captureValues.reserve(capturedRefsPerGuard);
  const CellRef captured = h.arena.makeCell("repeated captured cell");
  for (std::size_t i = 0; i < capturedRefsPerGuard; ++i)
    h.captureValues.push_back(captured);

  const CellRef subject    = h.arena.makeCell("validator subject");
  const CellRef stream     = h.arena.makeCell("captured stream register");
  const CellRef equalValue = h.arena.makeScalarCell(
      static_cast<std::int64_t>(VqlComparisonKind::Equal));
  const CellRef diagnostic = h.arena.makeCell("profile guard");
  const CellRef descriptor = h.makeBoolValidator(stream, equalValue);
  const CellRef entry = h.buildGraph(descriptor, diagnostic, subject, stream,
                                     guardCount, checkGuards);

  const CellRef cursor = h.vm.spawnCursor(entry, "guard-profile");
  ASSERT_TRUE(h.vm.step(cursor).success) << "captured stream setup";
  if (capturedRefsPerGuard > 0)
    ASSERT_TRUE(h.vm.step(cursor).success) << "captured stream append";

  const auto cellsBefore  = h.arena.cellCount();
  const auto cyclesBefore = h.vm.totalCycles();
  const auto start        = Clock::now();
  const auto execution    = h.vm.run(cursor, 1'000'000);
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      Clock::now() - start);
  const auto cycleDelta     = h.vm.totalCycles() - cyclesBefore;
  const auto arenaCellDelta = h.arena.cellCount() - cellsBefore;

  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
  std::cout << "VORTEX_GUARD_PROFILE mode="
            << (checkGuards ? "check" : "control") << " guards=" << guardCount
            << " captured_refs_per_guard=" << capturedRefsPerGuard
            << " captured_refs_total=" << guardCount * capturedRefsPerGuard
            << " expected_frame_backing_cells="
            << (checkGuards ? guardCount * 4 : 0) << " cycles=" << cycleDelta
            << " elapsed_us=" << elapsed.count()
            << " arena_cell_delta=" << arenaCellDelta << '\n';
}

TEST(VortexGuardProfileTest, DISABLED_Scaling) {
  for (const std::size_t guards : {1U, 16U, 64U}) {
    for (const std::size_t captures : {0U, 1000U}) {
      runProfileCase(guards, captures, true);
      runProfileCase(guards, captures, false);
    }
  }
}

} // namespace
} // namespace zigzag::vortex

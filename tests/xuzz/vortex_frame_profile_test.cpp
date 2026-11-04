// Opt-in timing probe for native Vortex frame invocation. Run explicitly with
// --gtest_also_run_disabled_tests
// --gtest_filter='*DISABLED_NativeInvokeScaling'.
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace zigzag::vortex {
namespace {

using Clock = std::chrono::steady_clock;
using zigzag::CellRef;
using zigzag::DimVector;

struct ProfileHarness {
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};

  void next(const CellRef from, const CellRef to) {
    ASSERT_TRUE(arena.link(from, core.dims().spin, DimVector::POS, to));
  }

  CellRef makeDescriptor(const std::size_t localCount) {
    const auto argument = arena.makeCell("argument");
    const auto result   = arena.makeCell("result");
    const auto copy     = vm.mintOpcode(OpcodeKind::CopyCellRefs);
    core.bindInput(copy, argument);
    core.bindOutput(copy, result);
    const auto ret = vm.mintOpcode(OpcodeKind::ReturnFrame);
    next(copy, ret);

    std::vector<CellRef> locals{argument, result};
    locals.reserve(localCount);
    while (locals.size() < localCount) locals.push_back(arena.makeCell());
    const auto made = vm.makeFrameDescriptor(FrameDescriptorSpec{
        .entry         = copy,
        .locals        = std::move(locals),
        .argumentPorts = {argument},
        .resultPorts   = {result},
    });
    EXPECT_TRUE(made);
    if (!made) throw std::runtime_error(made.error().message);
    core.bindInput(ret, *made);
    return *made;
  }

  CellRef appendCalls(const CellRef descriptor, const std::size_t count,
                      const CellRef input, const CellRef first) {
    if (!count)
      throw std::invalid_argument("profile call count must be positive");
    std::optional<CellRef> initial;
    std::optional<CellRef> previous;
    for (std::size_t i = 0; i < count; ++i) {
      const auto call = vm.mintOpcode(OpcodeKind::InvokeFrame);
      const auto out  = i == 0 ? first : arena.makeCell("result register");
      core.bindInput(call, descriptor);
      core.bindInput(call, input);
      core.bindOutput(call, out);
      if (previous)
        next(*previous, call);
      else
        initial = call;
      previous = call;
    }
    const auto halt = vm.mintOpcode(OpcodeKind::Halt);
    next(*previous, halt);
    return *initial;
  }

  CellRef cursorAt(const CellRef first, const std::string_view name) {
    return vm.spawnCursor(first, name);
  }
};

TEST(VortexFrameProfileTest, DISABLED_NativeInvokeScaling) {
  constexpr std::size_t measuredCalls = 32;
  const auto inputText                = "native frame profile input";

  for (const auto locals : {2U, 32U, 256U}) {
    for (const auto unrelatedRegisters : {0U, 100U}) {
      ProfileHarness h;
      const auto descriptor = h.makeDescriptor(locals);
      const auto input      = h.arena.makeCell(inputText);

      if (unrelatedRegisters != 0) {
        const auto firstSetupOutput = h.arena.makeCell("setup result");
        const auto setupFirst  = h.appendCalls(descriptor, unrelatedRegisters,
                                               input, firstSetupOutput);
        const auto setupCursor = h.cursorAt(setupFirst, "frame-profile-setup");
        const auto setup       = h.vm.run(setupCursor, 100'000);
        ASSERT_TRUE(setup.success) << setup.errorMessage;
      }

      const auto firstOutput = h.arena.makeCell("profile result");
      const auto firstCall   = h.vm.mintOpcode(OpcodeKind::InvokeFrame);
      h.core.bindInput(firstCall, descriptor);
      h.core.bindInput(firstCall, input);
      h.core.bindOutput(firstCall, firstOutput);
      auto previous = firstCall;
      for (std::size_t i = 1; i < measuredCalls; ++i) {
        const auto call = h.vm.mintOpcode(OpcodeKind::InvokeFrame);
        h.core.bindInput(call, descriptor);
        h.core.bindInput(call, input);
        h.core.bindOutput(call, h.arena.makeCell("profile result"));
        h.next(previous, call);
        previous = call;
      }
      const auto halt = h.vm.mintOpcode(OpcodeKind::Halt);
      h.next(previous, halt);
      const auto cursor = h.cursorAt(firstCall, "frame-profile-measured");

      const auto cellsBefore = h.arena.cellCount();
      const auto start       = Clock::now();
      const auto execution   = h.vm.run(cursor, 100'000);
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                                start);
      ASSERT_TRUE(execution.success) << execution.errorMessage;
      const auto allocatedCells = h.arena.cellCount() - cellsBefore;

      std::cout << "VORTEX_FRAME_PROFILE locals=" << locals
                << " unrelated_ref_registers=" << unrelatedRegisters
                << " calls=" << measuredCalls
                << " elapsed_us=" << elapsed.count()
                << " arena_cells_allocated=" << allocatedCells << '\n';
    }
  }
}

} // namespace
} // namespace zigzag::vortex

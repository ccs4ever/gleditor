// Rollback of InvokeFrame and ReturnFrame, exactly, at every statement.
//
// The frame opcodes restore a failed call from a per-key undo log
// (vortex/table_undo.hpp). No ordinary failure reaches most of that code (the
// body after the frame is built does no metered work), so these tests arm the
// VM's inert frame failpoint to throw after the Nth journalled statement, for
// every N the call has, and require that the failed step leaves the VM exactly
// as it found it: all twelve tables (down to the identity of every shared
// object they hold), the backing-cell and activation counters, the stream
// cursor, the retained-groups counter and the arena's cell count. Stale keys
// are seeded where the call will write or erase so that a restore of a prior
// value, and not only of an absent key, is checked.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "vortex_frame_test_access.hpp"

namespace zigzag::vortex {

namespace {

using zigzag::CellRef;
using zigzag::DimVector;

/// More than any one frame opcode has statements; a sweep that reaches it
/// means the failpoint never stopped firing.
constexpr std::size_t sweepLimit = 512;
/// Fewest failpoint stops a sweep of one frame opcode must find, so that a
/// seam that stopped being hit cannot pass unnoticed.
constexpr std::size_t fewestStops        = 4;
constexpr std::int64_t argumentValue     = 40;
constexpr std::string_view failpointText = "failpoint";

struct Harness {
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  CellRef one{arena.makeScalarCell(std::int64_t{1})};

  CellRef op(const OpcodeKind kind) { return vm.mintOpcode(kind); }
  void next(const CellRef from, const CellRef to) {
    ASSERT_TRUE(arena.link(from, core.dims().spin, DimVector::POS, to));
  }
  CellRef entry(const CellRef start) {
    const auto halt = op(OpcodeKind::Halt);
    auto tail       = start;
    while (const auto following =
               arena.linked(tail, core.dims().spin, DimVector::POS))
      tail = *following;
    next(tail, halt);
    return vm.spawnCursor(start, "rollback_test");
  }
  CellRef descriptor(const CellRef body, std::vector<CellRef> locals,
                     std::vector<FramePort> arguments,
                     std::vector<FramePort> results) {
    const auto made = vm.makeFrameDescriptor(
        FrameDescriptorSpec{.entry         = body,
                            .locals        = std::move(locals),
                            .argumentPorts = std::move(arguments),
                            .resultPorts   = std::move(results)});
    EXPECT_TRUE(made);
    return made.value_or(noCell);
  }
  /// Argument plus one, with a scratch local the body never touches.
  CellRef addOneFrame() {
    const auto argument = arena.makeCell("argument");
    const auto result   = arena.makeCell("result");
    const auto scratch  = arena.makeCell("scratch");
    const auto add      = op(OpcodeKind::Add);
    core.bindInput(add, argument);
    core.bindInput(add, one);
    core.bindOutput(add, result);
    const auto ret = op(OpcodeKind::ReturnFrame);
    next(add, ret);
    const auto desc = descriptor(add, {argument, result, scratch},
                                 {{argument, FramePortKind::Cell}},
                                 {{result, FramePortKind::Cell}});
    core.bindInput(ret, desc);
    return desc;
  }
  /// Calls @p inner and adds one to its answer.
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
    const auto desc = descriptor(nested, {argument, temporary, result},
                                 {{argument, FramePortKind::Cell}},
                                 {{result, FramePortKind::Cell}});
    core.bindInput(ret, desc);
    return desc;
  }
  CellRef call(const CellRef frame, const CellRef argument,
               const CellRef output) {
    const auto invoke = op(OpcodeKind::InvokeFrame);
    core.bindInput(invoke, frame);
    core.bindInput(invoke, argument);
    core.bindOutput(invoke, output);
    return invoke;
  }
};

struct Sweeps {
  std::size_t invokes{};
  std::size_t returns{};
};

struct Options {
  bool seedRegisters{false};
  bool seedGuard{false};
  bool seedStaleArguments{false};
  /// Backing cells of locals that are stream arguments, which the invoke
  /// sweep seeds with stale stream entries; cell offsets from the next id.
  std::vector<std::size_t> streamArgumentOffsets;
};

/// Whether sections named in @p expected all differ between two digests.
void expectSectionsChanged(const FrameTestAccess::Sections &before,
                           const FrameTestAccess::Sections &after,
                           const std::vector<std::string_view> &expected) {
  for (const auto name : expected) {
    const auto find = [&](const FrameTestAccess::Sections &sections) {
      return std::ranges::find_if(
          sections, [&](const auto &entry) { return entry.first == name; });
    };
    const auto left = find(before), right = find(after);
    ASSERT_NE(left, before.end()) << name;
    ASSERT_NE(right, after.end()) << name;
    EXPECT_NE(left->second, right->second)
        << "the call never changed " << name << ", so its rollback is untested";
  }
}

/// Fails the next frame opcode after each of its statements in turn, requires
/// the VM to be exactly as it was, then lets it succeed. Returns how many
/// failures it injected.
std::size_t sweepFrameOpcode(Harness &h, const CellRef cursor,
                             const std::vector<std::string_view> &changes) {
  const auto before       = FrameTestAccess::sections(h.vm);
  const auto beforeDigest = FrameTestAccess::digest(h.vm);
  const auto beforeCells  = h.arena.cellCount();
  std::size_t failures    = 0;
  for (std::size_t nth = 1; nth <= sweepLimit; ++nth) {
    FrameTestAccess::arm(h.vm, nth);
    const auto result = h.vm.step(cursor);
    if (result.success) {
      FrameTestAccess::arm(h.vm, std::nullopt);
      expectSectionsChanged(before, FrameTestAccess::sections(h.vm), changes);
      return failures;
    }
    EXPECT_NE(result.errorMessage.find(failpointText), std::string::npos)
        << "statement " << nth << ": " << result.errorMessage;
    EXPECT_EQ(FrameTestAccess::digest(h.vm), beforeDigest)
        << "failing after statement " << nth << " did not restore the tables";
    EXPECT_EQ(h.arena.cellCount(), beforeCells)
        << "failing after statement " << nth << " leaked arena cells";
    ++failures;
  }
  ADD_FAILURE() << "the failpoint never stopped firing";
  return failures;
}

Sweeps driveWithSweeps(Harness &h, const CellRef cursor, const Options &opt) {
  Sweeps sweeps;
  for (std::size_t guard = 0; guard < sweepLimit; ++guard) {
    const auto pc = h.vm.getCursorOpcode(cursor);
    if (!pc) break;
    const auto kind = h.vm.getOpcodeKind(*pc);
    if (!kind || *kind == OpcodeKind::Halt) break;
    if (*kind == OpcodeKind::InvokeFrame) {
      const auto nextId = static_cast<CellRef>(h.arena.cellCount());
      if (opt.seedStaleArguments)
        for (CellRef offset = 0; offset < 4; ++offset)
          FrameTestAccess::seedStaleReference(h.vm, nextId + offset);
      for (const auto offset : opt.streamArgumentOffsets)
        FrameTestAccess::seedStaleStreamArgument(
            h.vm, nextId + static_cast<CellRef>(offset));
      const auto failures = sweepFrameOpcode(
          h, cursor, {"cellReferences", "activationFrames", "cursorPC"});
      EXPECT_GE(failures, fewestStops) << "InvokeFrame stops";
      ++sweeps.invokes;
    } else if (*kind == OpcodeKind::ReturnFrame) {
      std::vector<std::string_view> changes{"cellReferences",
                                            "activationFrames", "cursorPC"};
      const auto frame = FrameTestAccess::top(h.vm, cursor);
      if (opt.seedRegisters) {
        const auto backings = FrameTestAccess::seedableBackings(h.vm, cursor);
        for (const auto backing : backings)
          FrameTestAccess::seedRegisters(h.vm, backing, frame, true);
        for (const auto destination :
             FrameTestAccess::destinations(h.vm, cursor))
          FrameTestAccess::seedRegisters(h.vm, destination, frame, false);
        for (const auto name :
             {"streams", "iterators", "groupIterators", "streamSlots",
              "iteratorSlots", "groupIteratorSlots"})
          changes.push_back(name);
        // Group owners are only seeded on backing cells; a frame whose every
        // local is a result port has none.
        if (!backings.empty()) changes.push_back("groupOwners");
      }
      if (opt.seedGuard) {
        FrameTestAccess::seedGuard(h.vm, cursor);
        changes.push_back("guardStacks");
        changes.push_back("guardScopes");
      }
      const auto failures = sweepFrameOpcode(h, cursor, changes);
      EXPECT_GE(failures, fewestStops) << "ReturnFrame stops";
      ++sweeps.returns;
    } else {
      const auto result = h.vm.step(cursor);
      EXPECT_TRUE(result.success) << result.errorMessage;
      if (!result.success) break;
    }
  }
  return sweeps;
}

void expectResult(Harness &h, const CellRef cursor, const CellRef output,
                  const std::int64_t expected) {
  const auto answer = h.vm.resultCell(cursor, output);
  ASSERT_TRUE(answer);
  EXPECT_EQ(h.core.render(*answer), CellValue{std::int64_t{expected}});
}

TEST(VortexFrameRollbackTest, PlainCallRollsBackAtEveryStatement) {
  Harness h;
  const auto frame  = h.addOneFrame();
  const auto output = h.arena.makeCell("result");
  const auto call   = h.call(
      frame, h.arena.makeScalarCell(std::int64_t{argumentValue}), output);
  const auto cursor = h.entry(call);
  const auto sweeps = driveWithSweeps(
      h, cursor, {.seedRegisters = true, .seedStaleArguments = true});
  EXPECT_EQ(sweeps.invokes, 1u);
  EXPECT_EQ(sweeps.returns, 1u);
  expectResult(h, cursor, output, argumentValue + 1);
}

TEST(VortexFrameRollbackTest, RetainedGuardStackAndScopeRollBackOnReturn) {
  Harness h;
  const auto frame  = h.addOneFrame();
  const auto output = h.arena.makeCell("result");
  const auto call   = h.call(
      frame, h.arena.makeScalarCell(std::int64_t{argumentValue}), output);
  const auto cursor = h.entry(call);
  const auto sweeps = driveWithSweeps(h, cursor, {.seedGuard = true});
  EXPECT_EQ(sweeps.returns, 1u);
  expectResult(h, cursor, output, argumentValue + 1);
}

TEST(VortexFrameRollbackTest, NestedInnerAndOuterCallsRollBack) {
  Harness h;
  const auto inner  = h.addOneFrame();
  const auto outer  = h.addTwoFrame(inner);
  const auto output = h.arena.makeCell("result");
  const auto call   = h.call(
      outer, h.arena.makeScalarCell(std::int64_t{argumentValue}), output);
  const auto cursor = h.entry(call);
  const auto sweeps = driveWithSweeps(
      h, cursor,
      {.seedRegisters = true, .seedGuard = true, .seedStaleArguments = true});
  EXPECT_EQ(sweeps.invokes, 2u);
  EXPECT_EQ(sweeps.returns, 2u);
  expectResult(h, cursor, output, argumentValue + 2);
}

/// The stream program: a stream argument and a stream result on the same
/// local, called once with a stream and once with a scalar.
struct StreamProgram {
  CellRef append, streamCall, singletonCall, streamOutput, singletonOutput;
};

StreamProgram buildStreamProgram(Harness &h) {
  const auto argument = h.arena.makeCell();
  const auto ret      = h.op(OpcodeKind::ReturnFrame);
  const auto descriptor =
      h.descriptor(ret, {argument}, {{argument, FramePortKind::Stream}},
                   {{argument, FramePortKind::Stream}});
  h.core.bindInput(ret, descriptor);
  const auto a      = h.arena.makeCell("a");
  const auto b      = h.arena.makeCell("b");
  const auto stream = h.arena.makeCell();
  const auto append = h.op(OpcodeKind::StreamAppend);
  h.core.bindInput(append, a);
  h.core.bindInput(append, b);
  h.core.bindInput(append, a);
  h.core.bindOutput(append, stream);
  StreamProgram program{.append          = append,
                        .streamOutput    = h.arena.makeCell(),
                        .singletonOutput = h.arena.makeCell()};
  program.streamCall    = h.call(descriptor, stream, program.streamOutput);
  program.singletonCall = h.call(descriptor, b, program.singletonOutput);
  h.next(append, program.streamCall);
  h.next(program.streamCall, program.singletonCall);
  return program;
}

TEST(VortexFrameRollbackTest, StreamArgumentAndStreamResultRollBack) {
  Harness h;
  const auto program = buildStreamProgram(h);
  const auto cursor  = h.entry(program.singletonCall);
  h.vm.setCursorOpcode(cursor, program.append);
  const auto sweeps = driveWithSweeps(
      h, cursor,
      {.seedRegisters = true, .seedGuard = true, .streamArgumentOffsets = {0}});
  EXPECT_EQ(sweeps.invokes, 2u);
  EXPECT_EQ(sweeps.returns, 2u);
  EXPECT_EQ(h.vm.resultCells(cursor, program.singletonOutput).size(), 1u);
}

TEST(VortexFrameRollbackTest, StreamCursorHandoverIsUndoneByTheSnapshot) {
  Harness h;
  const auto program = buildStreamProgram(h);
  const auto first   = h.entry(program.singletonCall);
  h.vm.setCursorOpcode(first, program.append);
  const auto ran = h.vm.run(first, 100);
  ASSERT_TRUE(ran.success) << ran.errorMessage;
  // The first cursor has finished and owns the stream tables. The second
  // cursor's stream-port call takes the role over, which clears four tables;
  // failing after that must put them, and the owner, back.
  const auto second = h.vm.spawnCursor(program.singletonCall, "second");
  const auto before = FrameTestAccess::sections(h.vm);
  const auto sweeps = driveWithSweeps(h, second, {.seedRegisters = true});
  EXPECT_EQ(sweeps.invokes, 1u);
  expectSectionsChanged(before, FrameTestAccess::sections(h.vm), {"scalars"});
}

} // namespace
} // namespace zigzag::vortex

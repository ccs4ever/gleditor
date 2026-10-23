/**
 * @file vagent_core_changes_test.cpp
 * @brief Tests for the Vortex VM and arena changes vagent depends on:
 * #ChoiceDepth (P1), optional Dewey tracking (P12) and trail clearing (P13).
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::noCell;
using namespace zigzag::vortex;

// Bound on steps per program so a wiring mistake fails the test, not hangs it.
constexpr int maxTestSteps = 64;

struct Harness {
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  VortexStdLib stdlib{core, vm};

  CellRef chain(std::initializer_list<CellRef> ops) {
    CellRef prev = noCell;
    CellRef head = noCell;
    for (const CellRef op : ops) {
      if (head == noCell) {
        head = op;
      }
      if (prev != noCell) {
        EXPECT_TRUE(arena.link(prev, core.dims().spin, false, op));
      }
      prev = op;
    }
    return head;
  }

  CellRef depthOp(CellRef out) {
    const CellRef op = vm.mintOpcode(OpcodeKind::ChoiceDepth);
    core.bindOutput(op, out);
    return op;
  }

  CellRef choiceOp(CellRef alt) {
    const CellRef op = vm.mintOpcode(OpcodeKind::Choice);
    core.bindInput(op, arena.makeScalarCell(static_cast<std::int64_t>(alt)));
    return op;
  }

  CellRef cutOp(CellRef depthCell) {
    const CellRef op = vm.mintOpcode(OpcodeKind::Cut);
    core.bindInput(op, depthCell);
    return op;
  }

  CellRef unifyOp(CellRef var, CellRef value) {
    const CellRef op = vm.mintOpcode(OpcodeKind::Unify);
    core.bindInput(op, var);
    core.bindInput(op, value);
    return op;
  }

  void runAll(CellRef cursor) {
    for (int i = 0; i < maxTestSteps; ++i) {
      if (vm.getCursorOpcode(cursor) == noCell || !vm.step(cursor).success) {
        break;
      }
    }
  }
};

std::int64_t asInt(const CellValue &v) { return std::get<std::int64_t>(v); }

TEST(VagentCoreChanges, ChoiceDepthReportsNestingAndCutRestoresIt) {
  Harness h;
  const CellRef d0 = h.arena.makeCell();
  const CellRef d1 = h.arena.makeCell();
  const CellRef d2 = h.arena.makeCell();
  const CellRef x  = h.stdlib.makeVar();
  const CellRef v  = h.arena.makeCell("kept");

  const CellRef alt = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef one = h.arena.makeScalarCell(static_cast<std::int64_t>(1));
  const CellRef head =
      h.chain({h.depthOp(d0), h.choiceOp(alt), h.depthOp(d1), h.choiceOp(alt),
               h.depthOp(d2), h.unifyOp(x, v), h.cutOp(one)});

  h.runAll(h.vm.spawnCursor(head));

  EXPECT_EQ(asInt(h.core.render(d0)), 0);
  EXPECT_EQ(asInt(h.core.render(d1)), 1);
  EXPECT_EQ(asInt(h.core.render(d2)), 2);
  EXPECT_EQ(h.vm.choiceDepth(), 1u);
  EXPECT_EQ(h.arena.textOf(h.stdlib.deref(x)), "kept");
}

TEST(VagentCoreChanges, BoundedStatementLeavesNoChoicePointAndKeepsWrites) {
  Harness h;
  const CellRef d  = h.arena.makeCell();
  const CellRef x  = h.stdlib.makeVar();
  const CellRef v  = h.arena.makeCell("one");
  const CellRef sf = h.vm.mintOpcode(OpcodeKind::Halt);

  const CellRef head = h.chain({h.depthOp(d), h.choiceOp(sf), h.unifyOp(x, v),
                                h.cutOp(d), h.vm.mintOpcode(OpcodeKind::Halt)});
  h.runAll(h.vm.spawnCursor(head));

  EXPECT_EQ(h.vm.choiceDepth(), 0u);
  EXPECT_EQ(h.arena.textOf(h.stdlib.deref(x)), "one");
}

TEST(VagentCoreChanges, FailedBoundedStatementUndoesOnlyItsOwnWrites) {
  Harness h;
  const CellRef d     = h.arena.makeCell();
  const CellRef early = h.stdlib.makeVar();
  const CellRef own   = h.stdlib.makeVar();
  const CellRef a     = h.arena.makeCell("a");
  const CellRef b     = h.arena.makeCell("b");
  const CellRef sf    = h.vm.mintOpcode(OpcodeKind::Halt);

  const CellRef head =
      h.chain({h.unifyOp(early, a), h.depthOp(d), h.choiceOp(sf),
               h.unifyOp(own, b), h.vm.mintOpcode(OpcodeKind::Fail)});
  const CellRef cur = h.vm.spawnCursor(head);
  h.runAll(cur);

  EXPECT_EQ(h.vm.getCursorOpcode(cur), sf);
  EXPECT_EQ(h.vm.choiceDepth(), 0u);
  EXPECT_EQ(h.arena.textOf(h.stdlib.deref(early)), "a");
  EXPECT_TRUE(h.stdlib.isVar(own));
}

TEST(VagentCoreChanges, DeweyPathStaysOutOfChoicePointsWhenTrackingIsOff) {
  Harness h;
  const CellRef alt = h.vm.mintOpcode(OpcodeKind::Nop);
  const CellRef cur = h.vm.spawnCursor(alt);

  EXPECT_EQ(h.vm.setDeweyTracking(false), &h.vm);
  constexpr std::size_t pushes = 5;
  for (std::size_t i = 0; i < pushes; ++i) {
    h.vm.pushChoicePoint(cur, alt);
  }

  EXPECT_TRUE(h.vm.currentDeweyPath().empty());
  ASSERT_EQ(h.vm.choiceDepth(), pushes);
  for (const auto &cp : h.vm.choiceStack()) {
    EXPECT_TRUE(cp.deweyPath.empty());
  }
  EXPECT_TRUE(h.vm.backtrack(cur));
  EXPECT_TRUE(h.vm.currentDeweyPath().empty());
}

TEST(VagentCoreChanges, DeweyPathIsMaintainedByDefault) {
  Harness h;
  const CellRef alt = h.vm.mintOpcode(OpcodeKind::Nop);
  const CellRef cur = h.vm.spawnCursor(alt);

  EXPECT_TRUE(h.vm.deweyTracking());
  constexpr std::size_t pushes = 3;
  for (std::size_t i = 0; i < pushes; ++i) {
    h.vm.pushChoicePoint(cur, alt);
  }

  EXPECT_EQ(h.vm.currentDeweyPath().size(), pushes);
  EXPECT_EQ(h.vm.choiceStack().back().deweyPath.size(), pushes);
  EXPECT_EQ(h.vm.currentDeweyKey(), "0.0.0");
}

TEST(VagentCoreChanges, DiscardingTheOnlyMarkClearsTheTrail) {
  ArenaManifold arena;
  const CellRef a  = arena.makeCell("a");
  const CellRef b  = arena.makeCell("b");
  const DimRef dim = arena.ensureDimension("d.test");
  const auto mark  = arena.mark();
  EXPECT_TRUE(arena.link(a, dim, false, b));
  EXPECT_GT(arena.trailSize(), 0u);

  arena.discard(mark);

  EXPECT_EQ(arena.outstandingMarks(), 0u);
  EXPECT_EQ(arena.trailSize(), 0u);
  EXPECT_EQ(arena.linked(a, dim, false), b);
}

TEST(VagentCoreChanges, InnerDiscardKeepsTrailForTheOuterRelease) {
  ArenaManifold arena;
  const CellRef a  = arena.makeCell("a");
  const CellRef b  = arena.makeCell("b");
  const DimRef dim = arena.ensureDimension("d.test");

  const auto outer = arena.mark();
  const auto inner = arena.mark();
  EXPECT_TRUE(arena.link(a, dim, false, b));
  const std::size_t trailed = arena.trailSize();
  EXPECT_GT(trailed, 0u);

  arena.discard(inner);
  EXPECT_EQ(arena.trailSize(), trailed);

  arena.release(outer);
  EXPECT_EQ(arena.linked(a, dim, false), noCell);
  EXPECT_EQ(arena.trailSize(), 0u);
}

} // namespace

/**
 * @file vortex_logic_chaining_probe_test.cpp
 * @brief Graph-level probes: can the outputs of MakeVar, MakeTerm and Deref be
 *        fed by a graph into Unify, MakeTerm and an InvokeFrame argument port
 *        and still denote the variable / term / dereferenced cell, and does
 *        MakeTerm survive a shared argument cell.
 *
 * Every probe wires hand-built ops through slots (never through direct cells),
 * which is how a compiler emits them. The Int64 box a producer writes is the
 * only thing a consumer may see unless the producer publishes a cell
 * reference, so each probe asks the VM, not the producer, what it denotes.
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace zigzag::vortex {
namespace {

using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::noCell;

struct Probe {
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  VortexStdLib stdlib{core, vm};
  std::vector<CellRef> chain;

  CellRef slot(const std::string &name = {}) {
    return name.empty() ? arena.makeCell() : arena.makeCell(name);
  }

  // Appends an op to the straight-line program under construction.
  CellRef emit(const OpcodeKind kind, const std::vector<CellRef> &inputs,
               const std::vector<CellRef> &outputs) {
    const auto op = vm.mintOpcode(kind);
    for (const auto in : inputs) core.bindInput(op, in);
    for (const auto out : outputs) core.bindOutput(op, out);
    if (!chain.empty()) {
      EXPECT_TRUE(
          arena.link(chain.back(), core.dims().spin, DimVector::POS, op));
    }
    chain.push_back(op);
    return op;
  }

  // Runs the program built so far to its end.
  ExecutionResult run() {
    const auto halt = vm.mintOpcode(OpcodeKind::Halt);
    EXPECT_TRUE(
        arena.link(chain.back(), core.dims().spin, DimVector::POS, halt));
    const auto cursor = vm.spawnCursor(chain.front(), "chaining_probe");
    lastCursor        = cursor;
    return vm.run(cursor, 1000);
  }

  CellRef lastCursor{noCell};

  // The cell a producer's Int64 box names.
  std::optional<CellRef> boxed(const CellRef producerSlot) {
    const auto value = core.render(producerSlot);
    if (!value || !std::holds_alternative<std::int64_t>(*value))
      return std::nullopt;
    return static_cast<CellRef>(std::get<std::int64_t>(*value));
  }

  // A frame that copies its single argument reference to its result.
  CellRef identityFrame() {
    const auto argument = arena.makeCell("formal argument");
    const auto result   = arena.makeCell("formal result");
    const auto copy     = vm.mintOpcode(OpcodeKind::CopyCellRefs);
    core.bindInput(copy, argument);
    core.bindOutput(copy, result);
    const auto ret = vm.mintOpcode(OpcodeKind::ReturnFrame);
    EXPECT_TRUE(arena.link(copy, core.dims().spin, DimVector::POS, ret));
    const auto desc = vm.makeFrameDescriptor(FrameDescriptorSpec{
        .entry         = copy,
        .locals        = {argument, result},
        .argumentPorts = {argument},
        .resultPorts   = {result},
    });
    EXPECT_TRUE(desc);
    core.bindInput(ret, *desc);
    return *desc;
  }

  // Passes `producerSlot` through the identity frame; returns the cell the
  // call's result slot denotes afterwards.
  std::optional<CellRef> throughFrame(const CellRef producerSlot) {
    const auto desc   = identityFrame();
    const auto output = arena.makeCell("call result");
    emit(OpcodeKind::InvokeFrame, {desc, producerSlot}, {output});
    const auto executed = run();
    EXPECT_TRUE(executed.success) << executed.errorMessage;
    return vm.resultCell(lastCursor, output);
  }
};

// (a) MakeVar ------------------------------------------------------------

TEST(VortexLogicChainingProbe, MakeVarFeedsUnify) {
  Probe p;
  const auto var  = p.slot("var slot");
  const auto atom = p.arena.makeCell("cat");
  const auto ok   = p.slot("ok");
  p.emit(OpcodeKind::MakeVar, {}, {var});
  p.emit(OpcodeKind::Unify, {var, atom}, {ok});
  const auto executed = p.run();
  ASSERT_TRUE(executed.success) << executed.errorMessage;
  const auto varCell = p.boxed(var);
  ASSERT_TRUE(varCell);
  EXPECT_EQ(p.core.render(ok), CellValue{true});
  const auto bound = p.stdlib.deref(*varCell);
  ASSERT_TRUE(bound);
  EXPECT_EQ(p.arena.textOf(*bound), "cat");
}

TEST(VortexLogicChainingProbe, MakeVarFeedsMakeTermArgument) {
  Probe p;
  const auto var     = p.slot("var slot");
  const auto functor = p.arena.makeCell("f");
  const auto term    = p.slot("term slot");
  p.emit(OpcodeKind::MakeVar, {}, {var});
  p.emit(OpcodeKind::MakeTerm, {functor, var}, {term});
  const auto executed = p.run();
  ASSERT_TRUE(executed.success) << executed.errorMessage;
  const auto varCell  = p.boxed(var);
  const auto termCell = p.boxed(term);
  ASSERT_TRUE(varCell && termCell);
  const auto args = p.stdlib.argumentsOf(*termCell);
  ASSERT_EQ(args.size(), 1u);
  EXPECT_EQ(args[0], *varCell);
  EXPECT_TRUE(p.stdlib.isVar(args[0]));
}

TEST(VortexLogicChainingProbe, MakeVarThroughInvokeFrameArgumentPort) {
  Probe p;
  const auto var = p.slot("var slot");
  p.emit(OpcodeKind::MakeVar, {}, {var});
  const auto denoted = p.throughFrame(var);
  const auto varCell = p.boxed(var);
  ASSERT_TRUE(varCell);
  ASSERT_TRUE(denoted);
  EXPECT_EQ(*denoted, *varCell);
}

// (b) MakeTerm and Deref -------------------------------------------------

TEST(VortexLogicChainingProbe, MakeTermFeedsUnify) {
  Probe p;
  const auto x       = p.stdlib.makeVar();
  const auto functor = p.arena.makeCell("f");
  const auto arg     = p.arena.makeCell("a");
  const auto term    = p.slot("term slot");
  const auto ok      = p.slot("ok");
  p.emit(OpcodeKind::MakeTerm, {functor, arg}, {term});
  p.emit(OpcodeKind::Unify, {x, term}, {ok});
  const auto executed = p.run();
  ASSERT_TRUE(executed.success) << executed.errorMessage;
  const auto termCell = p.boxed(term);
  ASSERT_TRUE(termCell);
  EXPECT_EQ(p.core.render(ok), CellValue{true});
  EXPECT_EQ(p.stdlib.deref(x), std::optional<CellRef>{*termCell});
}

TEST(VortexLogicChainingProbe, MakeTermFeedsMakeTermArgument) {
  Probe p;
  const auto inner  = p.slot("inner slot");
  const auto outer  = p.slot("outer slot");
  const auto fInner = p.arena.makeCell("g");
  const auto fOuter = p.arena.makeCell("f");
  const auto leaf   = p.arena.makeCell("a");
  p.emit(OpcodeKind::MakeTerm, {fInner, leaf}, {inner});
  p.emit(OpcodeKind::MakeTerm, {fOuter, inner}, {outer});
  const auto executed = p.run();
  ASSERT_TRUE(executed.success) << executed.errorMessage;
  const auto innerCell = p.boxed(inner);
  const auto outerCell = p.boxed(outer);
  ASSERT_TRUE(innerCell && outerCell);
  const auto args = p.stdlib.argumentsOf(*outerCell);
  ASSERT_EQ(args.size(), 1u);
  EXPECT_EQ(args[0], *innerCell);
}

TEST(VortexLogicChainingProbe, MakeTermThroughInvokeFrameArgumentPort) {
  Probe p;
  const auto term    = p.slot("term slot");
  const auto functor = p.arena.makeCell("f");
  const auto leaf    = p.arena.makeCell("a");
  p.emit(OpcodeKind::MakeTerm, {functor, leaf}, {term});
  const auto denoted  = p.throughFrame(term);
  const auto termCell = p.boxed(term);
  ASSERT_TRUE(termCell);
  ASSERT_TRUE(denoted);
  EXPECT_EQ(*denoted, *termCell);
}

TEST(VortexLogicChainingProbe, DerefFeedsUnify) {
  Probe p;
  const auto x    = p.stdlib.makeVar();
  const auto atom = p.arena.makeCell("cat");
  ASSERT_TRUE(p.stdlib.unify(x, atom));
  const auto again   = p.arena.makeCell("cat");
  const auto derefed = p.slot("deref slot");
  const auto ok      = p.slot("ok");
  p.emit(OpcodeKind::Deref, {x}, {derefed});
  p.emit(OpcodeKind::Unify, {derefed, again}, {ok});
  const auto executed = p.run();
  ASSERT_TRUE(executed.success) << executed.errorMessage;
  EXPECT_EQ(p.boxed(derefed), std::optional<CellRef>{atom});
  EXPECT_EQ(p.core.render(ok), CellValue{true});
}

TEST(VortexLogicChainingProbe, DerefFeedsMakeTermArgument) {
  Probe p;
  const auto x    = p.stdlib.makeVar();
  const auto atom = p.arena.makeCell("cat");
  ASSERT_TRUE(p.stdlib.unify(x, atom));
  const auto derefed = p.slot("deref slot");
  const auto term    = p.slot("term slot");
  const auto functor = p.arena.makeCell("f");
  p.emit(OpcodeKind::Deref, {x}, {derefed});
  p.emit(OpcodeKind::MakeTerm, {functor, derefed}, {term});
  const auto executed = p.run();
  ASSERT_TRUE(executed.success) << executed.errorMessage;
  const auto termCell = p.boxed(term);
  ASSERT_TRUE(termCell);
  const auto args = p.stdlib.argumentsOf(*termCell);
  ASSERT_EQ(args.size(), 1u);
  EXPECT_EQ(p.arena.textOf(args[0]), "cat");
}

TEST(VortexLogicChainingProbe, DerefThroughInvokeFrameArgumentPort) {
  Probe p;
  const auto x    = p.stdlib.makeVar();
  const auto atom = p.arena.makeCell("cat");
  ASSERT_TRUE(p.stdlib.unify(x, atom));
  const auto derefed = p.slot("deref slot");
  p.emit(OpcodeKind::Deref, {x}, {derefed});
  const auto denoted = p.throughFrame(derefed);
  ASSERT_TRUE(denoted);
  EXPECT_EQ(*denoted, atom);
}

// (c) MakeTerm and shared argument cells ---------------------------------

TEST(VortexLogicChainingProbe, MakeTermSameVariableTwiceBuildsFofXX) {
  Probe p;
  const auto x       = p.stdlib.makeVar();
  const auto functor = p.arena.makeCell("f");
  const auto term    = p.slot("term slot");
  p.emit(OpcodeKind::MakeTerm, {functor, x, x}, {term});
  const auto executed = p.run();
  ASSERT_TRUE(executed.success) << executed.errorMessage;
  const auto termCell = p.boxed(term);
  ASSERT_TRUE(termCell);
  const auto args = p.stdlib.argumentsOf(*termCell);
  ASSERT_EQ(args.size(), 2u);
  // Both positions denote X, and X itself is still a free variable that no
  // term chain has stolen.
  EXPECT_EQ(p.stdlib.deref(args[0]), std::optional<CellRef>{x});
  EXPECT_EQ(p.stdlib.deref(args[1]), std::optional<CellRef>{x});
  EXPECT_TRUE(p.stdlib.isVar(x));
  // Binding X binds both positions.
  const auto atom = p.arena.makeCell("a");
  ASSERT_TRUE(p.stdlib.unify(x, atom));
  EXPECT_EQ(p.stdlib.deref(args[0]), std::optional<CellRef>{atom});
  EXPECT_EQ(p.stdlib.deref(args[1]), std::optional<CellRef>{atom});
}

TEST(VortexLogicChainingProbe, MakeTermSubTermAlreadyAnArgumentIsNotStolen) {
  Probe p;
  const auto leaf   = p.arena.makeCell("a");
  const auto fInner = p.arena.makeCell("g");
  const auto fOne   = p.arena.makeCell("f");
  const auto fTwo   = p.arena.makeCell("h");
  const auto inner  = p.slot("inner slot");
  const auto first  = p.slot("first slot");
  const auto second = p.slot("second slot");
  p.emit(OpcodeKind::MakeTerm, {fInner, leaf}, {inner});
  p.emit(OpcodeKind::MakeTerm, {fOne, inner}, {first});
  p.emit(OpcodeKind::MakeTerm, {fTwo, inner}, {second});
  const auto executed = p.run();
  ASSERT_TRUE(executed.success) << executed.errorMessage;
  const auto innerCell  = p.boxed(inner);
  const auto firstCell  = p.boxed(first);
  const auto secondCell = p.boxed(second);
  ASSERT_TRUE(innerCell && firstCell && secondCell);
  const auto firstArgs  = p.stdlib.argumentsOf(*firstCell);
  const auto secondArgs = p.stdlib.argumentsOf(*secondCell);
  ASSERT_EQ(firstArgs.size(), 1u);
  ASSERT_EQ(secondArgs.size(), 1u);
  EXPECT_EQ(firstArgs[0], *innerCell);
  EXPECT_EQ(p.stdlib.deref(secondArgs[0]), std::optional<CellRef>{*innerCell});
  // The inner term is still intact.
  const auto innerArgs = p.stdlib.argumentsOf(*innerCell);
  ASSERT_EQ(innerArgs.size(), 1u);
  EXPECT_EQ(innerArgs[0], leaf);
}

} // namespace
} // namespace zigzag::vortex

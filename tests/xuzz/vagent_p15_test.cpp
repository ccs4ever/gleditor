// Phase-4 P15 tests (design/vagent-metaprogramming-implementation.md
// :322-335; spec §6.5/M10, the §10.2 P15 row): opcode-kind
// immutability at the write funnel — a VM-installed predicate testing
// `opcodeMap_.contains(masterOf(cell))`, refusing program-value writes
// into registered op cells and surfacing a typed `success=false`
// ExecutionResult at every funnel site — plus the registration pass
// that repopulates a fresh context's map from the recorded
// instruction enumeration (never a value scan).
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "common/xanadu/vagent/compiler.hpp"
#include "common/xanadu/vagent/fragment.hpp"
#include "common/xanadu/vagent/parser.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace {

using namespace xanadu::vagent;
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::Posward;
using zigzag::vortex::CellValue;
using zigzag::vortex::ContractViolationKind;
using zigzag::vortex::ExecutionResult;
using zigzag::vortex::OpcodeKind;
using zigzag::vortex::WriteFault;

// One arena/core/vm plus the scalar compiler — the shape the sibling's
// phase-4 mutation suite shares. `run` executes a single-op program
// (spawn -> run until the op's d.spin chain ends or a Halt) and hands
// back the step's ExecutionResult verbatim.
struct P15Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  Compiler compiler{core, vm};

  CellRef scalar(std::int64_t value) { return arena.makeScalarCell(value); }

  ExecutionResult run(CellRef entry) {
    const auto cursor = vm.spawnCursor(entry);
    return vm.run(cursor);
  }

  // The four-input Value/Splice operand convention: target, offset,
  // length, replacement — each as an operand cell on the +d.grab wing.
  void bindWriteOperands(CellRef op, CellRef target, std::int64_t offset,
                         std::int64_t length, CellRef replacement) {
    core.bindInput(op, target);
    core.bindInput(op, scalar(offset));
    core.bindInput(op, scalar(length));
    core.bindInput(op, replacement);
  }

  // The +d.vars record shape fragment.cpp's varsRecord mints — used to
  // hand-build the malformed-record refusal arms.
  CellRef varsRecord(CellRef owner, std::string_view key, CellRef value) {
    const auto record = arena.makeCell(key);
    zigzag::expectWritten(
        arena.link(record, core.dims().values, Posward, value));
    zigzag::expectWritten(
        arena.link(zigzag::rankTail(arena, owner, core.dims().vars),
                   core.dims().vars, Posward, record));
    return record;
  }
};

void expectKindRefusal(const ExecutionResult &result) {
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.contractViolation, ContractViolationKind::KindImmutability);
}

// ---- the funnel sites -------------------------------------------------

TEST(VagentP15Test, ValueBodyRefusesARegisteredOpCellTarget) {
  P15Harness h;
  const CellRef target = h.vm.mintOpcode(OpcodeKind::Add);
  ASSERT_TRUE(h.vm.hasOpcodeKind(target));
  const CellRef op = h.vm.mintOpcode(OpcodeKind::Value);
  h.bindWriteOperands(op, target, 0, 1, h.scalar(99));
  expectKindRefusal(h.run(op));
  // The registered kind survives — no bits moved.
  EXPECT_EQ(h.vm.getOpcodeKind(target), OpcodeKind::Add);
}

TEST(VagentP15Test, SpliceBodyRefusesARegisteredOpCellTarget) {
  P15Harness h;
  const CellRef target = h.vm.mintOpcode(OpcodeKind::Add);
  const CellRef op     = h.vm.mintOpcode(OpcodeKind::Splice);
  h.bindWriteOperands(op, target, 0, 1, h.scalar(99));
  expectKindRefusal(h.run(op));
}

TEST(VagentP15Test, GenericOutputWritebackRefusesARegisteredOpCell) {
  P15Harness h;
  // Every non-structural op restates its bound outputs' content — a
  // wing re-point aims that writeback at an op cell with no
  // Value/Splice involvement, which is the desync P15 exists to stop.
  const CellRef target = h.vm.mintOpcode(OpcodeKind::Nop);
  const CellRef op     = h.vm.mintOpcode(OpcodeKind::Add);
  h.core.bindInput(op, h.scalar(1));
  h.core.bindInput(op, h.scalar(2));
  h.core.bindOutput(op, target);
  expectKindRefusal(h.run(op));
  EXPECT_EQ(h.vm.getOpcodeKind(target), OpcodeKind::Nop);
}

TEST(VagentP15Test, MemoHitWritebackRefusesARegisteredOpCell) {
  P15Harness h;
  const CellRef in1 = h.scalar(1);
  const CellRef in2 = h.scalar(2);
  const CellRef op  = h.vm.mintOpcode(OpcodeKind::Add);
  h.core.bindInput(op, in1);
  h.core.bindInput(op, in2);
  // Pre-populate the memo entry so the run takes the memo-hit
  // writeback arm rather than the body-execution arm.
  h.vm.enableMemoization(op, "p15-memo");
  const CellRef pin = h.core.getOrCreateMemoPin("p15-memo");
  h.core.recordMemo(pin,
                    {CellValue(std::int64_t{1}), CellValue(std::int64_t{2})},
                    {CellValue(std::int64_t{3})});
  const CellRef target = h.vm.mintOpcode(OpcodeKind::Nop);
  h.core.bindOutput(op, target);
  expectKindRefusal(h.run(op));
}

TEST(VagentP15Test, PipelineParamWritebackRefusesARegisteredParamCell) {
  P15Harness h;
  // A registered op heading a parameter pipeline — deliberately
  // pathological, and exactly what the funnel must catch.
  const CellRef param = h.vm.mintOpcode(OpcodeKind::Nop);
  const CellRef neg   = h.vm.mintOpcode(OpcodeKind::Neg);
  zigzag::expectWritten(h.arena.link(param, h.core.dims().spin, Posward, neg));
  const CellRef driver = h.vm.mintOpcode(OpcodeKind::Nop);
  h.core.bindInput(driver, param);
  expectKindRefusal(h.run(driver));
}

TEST(VagentP15Test, PipelineBoundOutputWritebackRefusesARegisteredOpCell) {
  P15Harness h;
  const CellRef param   = h.scalar(4);
  const CellRef neg     = h.vm.mintOpcode(OpcodeKind::Neg);
  const CellRef outCell = h.vm.mintOpcode(OpcodeKind::Nop);
  zigzag::expectWritten(h.arena.link(param, h.core.dims().spin, Posward, neg));
  h.core.bindOutput(neg, outCell);
  const CellRef driver = h.vm.mintOpcode(OpcodeKind::Nop);
  h.core.bindInput(driver, param);
  expectKindRefusal(h.run(driver));
}

// ---- the exempt arm and the carve-outs --------------------------------

TEST(VagentP15Test, ValueReadArmStaysExempt) {
  P15Harness h;
  // The guard is the write arm only — reading an op cell's content is
  // what validate's kind check does.
  const CellRef target = h.vm.mintOpcode(OpcodeKind::Add);
  const CellRef op     = h.vm.mintOpcode(OpcodeKind::Value);
  const CellRef out    = h.arena.makeCell();
  h.core.bindInput(op, target);
  h.core.bindInput(op, h.scalar(0));
  h.core.bindOutput(op, out);
  const auto result = h.run(op);
  EXPECT_TRUE(result.success) << result.errorMessage;
  EXPECT_EQ(h.arena.asInt64(out).value_or(-1),
            static_cast<std::int64_t>(target));
}

TEST(VagentP15Test, FreshAndUnregisteredCellsWriteFreely) {
  P15Harness h;
  // A fresh cell carries no map entry — nothing to desync.
  const CellRef fresh = h.arena.makeCell();
  WriteFault fault;
  EXPECT_TRUE(h.core.writeValue(fresh, 0, std::nullopt,
                                CellValue(std::int64_t{5}), &fault));
  EXPECT_EQ(fault, WriteFault::None);

  // Membership, not decodability: a data cell whose small int64
  // *decodes* as a kind still writes freely — this is exactly how
  // New+Value mints runtime ops.
  EXPECT_TRUE(h.vm.getOpcodeKind(fresh).has_value());
  EXPECT_FALSE(h.vm.hasOpcodeKind(fresh));
  EXPECT_TRUE(h.core.writeValue(fresh, 0, std::nullopt,
                                CellValue(std::int64_t{42}), &fault));
  EXPECT_EQ(fault, WriteFault::None);
  EXPECT_EQ(h.arena.asInt64(fresh).value_or(-1), 42);
}

TEST(VagentP15Test, NewValueRuntimeMintingStaysLegal) {
  P15Harness h;
  // The spec-named minting shape: #NEW links a fresh cell, #VALUE
  // restates its content with a kind int — both free, because an
  // unregistered cell's content *is* its kind.
  const CellRef anchor  = h.arena.makeCell();
  const CellRef fresh   = h.arena.makeCell();
  const CellRef newOp   = h.vm.mintOpcode(OpcodeKind::New);
  const CellRef valueOp = h.vm.mintOpcode(OpcodeKind::Value);
  const CellRef stepDim =
      h.scalar(static_cast<std::int64_t>(h.core.dims().step));
  h.core.bindInput(newOp, anchor);
  h.core.bindInput(newOp, stepDim);
  h.core.bindInput(newOp, h.scalar(0));
  h.core.bindOutput(newOp, fresh);
  EXPECT_TRUE(h.run(newOp).success);
  h.bindWriteOperands(valueOp, fresh, 0, 1,
                      h.scalar(static_cast<std::int64_t>(OpcodeKind::Value)));
  const auto result = h.run(valueOp);
  EXPECT_TRUE(result.success) << result.errorMessage;
  // …and the runtime-minted op rewrites freely — it decodes but is not
  // registered, so no map can desync.
  EXPECT_EQ(h.vm.getOpcodeKind(fresh), OpcodeKind::Value);
  EXPECT_FALSE(h.vm.hasOpcodeKind(fresh));
  WriteFault fault;
  EXPECT_TRUE(h.core.writeValue(
      fresh, 0, std::nullopt,
      CellValue(static_cast<std::int64_t>(OpcodeKind::Add)), &fault));
  EXPECT_EQ(fault, WriteFault::None);
  EXPECT_EQ(h.vm.getOpcodeKind(fresh), OpcodeKind::Add);
}

TEST(VagentP15Test, CloneOfRegisteredOpResolvesToRegisteredMaster) {
  P15Harness h;
  // A Clone of a registered op is itself unregistered, but writes the
  // registered master's bits — the predicate tests masterOf(cell).
  const CellRef target = h.vm.mintOpcode(OpcodeKind::Add);
  const CellRef clone  = h.arena.makeCell();
  h.core.setLink(target, h.core.dims().clone, Posward, clone);
  WriteFault fault;
  EXPECT_FALSE(h.core.writeValue(clone, 0, std::nullopt,
                                 CellValue(std::int64_t{7}), &fault));
  EXPECT_EQ(fault, WriteFault::OpcodeKind);
  EXPECT_EQ(h.vm.getOpcodeKind(target), OpcodeKind::Add);
  // …while a clone of a *data* cell resolves to an unregistered master.
  const CellRef data      = h.arena.makeCell("data");
  const CellRef dataClone = h.arena.makeCell();
  h.core.setLink(data, h.core.dims().clone, Posward, dataClone);
  EXPECT_TRUE(h.core.writeValue(dataClone, 0, std::nullopt,
                                CellValue(std::string("rewritten")), &fault));
  EXPECT_EQ(fault, WriteFault::None);
}

TEST(VagentP15Test, CallFrameWritesStayExempt) {
  P15Harness h;
  // #CALL's frame traffic writes a per-activation fresh cell directly —
  // the frame-cell carve-out.
  const CellRef halt = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef call = h.vm.mintOpcode(OpcodeKind::Call);
  h.core.bindInput(call, h.scalar(static_cast<std::int64_t>(halt)));
  const auto result = h.run(call);
  EXPECT_TRUE(result.success) << result.errorMessage;
}

TEST(VagentP15Test, EveryFunnelVerbReportsTheTypedFault) {
  P15Harness h;
  // The WriteFault channel the sibling's graft/prune/learn_* verbs get
  // for free: writeValue/splice and their insert/append/erase aliases
  // all refuse with OpcodeKind on a registered master.
  const CellRef target = h.vm.mintOpcode(OpcodeKind::Add);
  for (const int write : {0, 1, 2, 3, 4}) {
    WriteFault fault = WriteFault::None;
    std::optional<CellRef> res;
    switch (write) {
    case 0:
      res = h.core.writeValue(target, 0, std::nullopt,
                              CellValue(std::int64_t{9}), &fault);
      break;
    case 1:
      res = h.core.splice(target, 0, 1, CellValue(std::int64_t{9}), &fault);
      break;
    case 2:
      res = h.core.insert(target, 0, CellValue(std::int64_t{9}), &fault);
      break;
    case 3:
      res = h.core.append(target, CellValue(std::int64_t{9}), &fault);
      break;
    default:
      res = h.core.erase(target, 0, 1, &fault);
      break;
    }
    EXPECT_FALSE(res.has_value()) << "verb " << write;
    EXPECT_EQ(fault, WriteFault::OpcodeKind) << "verb " << write;
  }
  EXPECT_EQ(h.vm.getOpcodeKind(target), OpcodeKind::Add);
}

TEST(VagentP15Test, OrdinaryProgramExecutionIsUnaffected) {
  P15Harness h;
  // The guard refuses only registered-cell writes: a compiled program
  // still mints, binds, writes and halts.
  auto expression = Parser("1 + 2 * 3").parseExpression();
  ASSERT_TRUE(expression);
  auto statement = h.compiler.compileStatement(**expression);
  ASSERT_TRUE(statement) << formatDiagnostic(statement.error());
  const auto cursor = h.vm.spawnCursor(statement->entry);
  const auto result = h.vm.run(cursor);
  EXPECT_TRUE(result.success) << result.errorMessage;
  EXPECT_EQ(h.core.render(statement->result), CellValue(std::int64_t{7}));
}

// ---- the registration pass ---------------------------------------------

TEST(VagentP15Test, RegisterProgramImageRepopulatesAFreshContext) {
  P15Harness h;
  auto expression = Parser("1 + 2").parseExpression();
  ASSERT_TRUE(expression);
  auto statement = h.compiler.compileStatement(**expression);
  ASSERT_TRUE(statement) << formatDiagnostic(statement.error());
  ASSERT_FALSE(h.compiler.programOps().empty());
  const auto record = finalizeProgramImage(h.compiler);
  ASSERT_TRUE(record.has_value());

  // A fresh context's image bind: a second VM over the same core — its
  // opcodeMap_ is empty (the per-WorkerContext fact spec :2335-2343
  // names), and constructing it re-installs the funnel guard onto its
  // own map's view.
  zigzag::vortex::VortexVM fresh{h.core};
  for (const auto op : h.compiler.programOps()) {
    EXPECT_TRUE(fresh.getOpcodeKind(op).has_value())
        << "recorded op " << op << " still decodes through asInt64";
    EXPECT_FALSE(fresh.hasOpcodeKind(op))
        << "recorded op " << op << " is absent from the fresh map";
  }

  // The hole the pass exists to close: a surgical write into a
  // recorded op cell lands while the fresh map holds nothing.
  WriteFault fault    = WriteFault::None;
  const CellRef stray = h.vm.mintOpcode(OpcodeKind::Nop);
  EXPECT_TRUE(fresh.getOpcodeKind(stray).has_value());
  EXPECT_FALSE(fresh.hasOpcodeKind(stray));
  EXPECT_TRUE(h.core.writeValue(stray, 0, std::nullopt,
                                CellValue(std::int64_t{7}), &fault));
  EXPECT_EQ(fault, WriteFault::None);

  // The pass: exactly the recorded enumeration re-registers.
  const auto registered = fresh.registerProgramImage(*record);
  ASSERT_TRUE(registered.has_value()) << registered.error();
  EXPECT_EQ(*registered, h.compiler.programOps().size());
  for (const auto op : h.compiler.programOps()) {
    EXPECT_TRUE(fresh.hasOpcodeKind(op));
    EXPECT_EQ(fresh.getOpcodeKind(op), h.vm.getOpcodeKind(op));
    fault = WriteFault::None;
    EXPECT_FALSE(h.core.writeValue(op, 0, std::nullopt,
                                   CellValue(std::int64_t{9}), &fault));
    EXPECT_EQ(fault, WriteFault::OpcodeKind);
  }

  // …and only the enumeration: the stray minted op stays unregistered
  // (minting is not recording — no value scan can claim it) — and so
  // does honest small-int data.
  EXPECT_FALSE(fresh.hasOpcodeKind(stray));
  EXPECT_TRUE(h.core.writeValue(stray, 0, std::nullopt,
                                CellValue(std::int64_t{8}), &fault));
  EXPECT_EQ(fault, WriteFault::None);
  const CellRef data = h.scalar(static_cast<std::int64_t>(OpcodeKind::Mod));
  EXPECT_TRUE(fresh.getOpcodeKind(data).has_value());
  EXPECT_FALSE(fresh.hasOpcodeKind(data));
  EXPECT_TRUE(h.core.writeValue(data, 0, std::nullopt,
                                CellValue(std::int64_t{1}), &fault));
  EXPECT_EQ(fault, WriteFault::None);

  // The first VM's map never lost the entries — re-asserting its guard
  // view restores its own refusals (one VM per core in production;
  // sharing is a test shape).
  h.vm.installWriteGuard();
  fault = WriteFault::None;
  EXPECT_FALSE(h.core.writeValue(h.compiler.programOps().front(), 0,
                                 std::nullopt, CellValue(std::int64_t{9}),
                                 &fault));
  EXPECT_EQ(fault, WriteFault::OpcodeKind);
}

TEST(VagentP15Test, RegisterProgramOpsNamesBadEnumerationCells) {
  P15Harness h;
  zigzag::vortex::VortexVM fresh{h.core};
  // A cell the arena cannot reach.
  const std::vector<CellRef> alien{
      static_cast<CellRef>(h.arena.cellCount() + 1000)};
  EXPECT_FALSE(fresh.registerProgramOps(alien).has_value());
  // A contained cell whose content is not a kind int.
  const std::vector<CellRef> text{h.arena.makeCell("not an op")};
  EXPECT_FALSE(fresh.registerProgramOps(text).has_value());
}

TEST(VagentP15Test, RegisterProgramImageNamesMalformedRecords) {
  P15Harness h;
  zigzag::vortex::VortexVM fresh{h.core};
  // Not an arena cell at all.
  EXPECT_FALSE(fresh
                   .registerProgramImage(
                       static_cast<CellRef>(h.arena.cellCount() + 1000))
                   .has_value());
  // A record carrying no "ops" rank.
  EXPECT_FALSE(fresh.registerProgramImage(h.arena.makeCell()).has_value());
  // An "ops" rank whose member is not an int64 position.
  const CellRef image   = h.arena.makeCell();
  const CellRef opsHead = h.arena.makeCell();
  zigzag::expectWritten(h.arena.link(opsHead, h.core.dims().step, Posward,
                                     h.arena.makeCell("text, not a position")));
  h.varsRecord(image, "ops", opsHead);
  EXPECT_FALSE(fresh.registerProgramImage(image).has_value());
}

} // namespace

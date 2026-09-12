/**
 * @file vortex_core_test.cpp
 * @brief Unit test suite for the Vortex Hyperstructural Runtime Core (Stage 1).
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "xudu/core/vortex.hpp"
#include "zigzag/core/arena_manifold.hpp"

namespace {

using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::noCell;
using namespace zigzag::vortex;

struct TestHarness {
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
};

TEST(VortexCoreTest, SystemGenesisMintsAllDimensionsOffHome) {
  TestHarness h;
  EXPECT_NE(h.core.home(), noCell);
  EXPECT_EQ(h.core.arena().textOf(h.core.home()), "home");

  const auto &d = h.core.dims();
  EXPECT_NE(d.dims, noCell);
  EXPECT_NE(d.grab, noCell);
  EXPECT_NE(d.step, noCell);
  EXPECT_NE(d.spin, noCell);
  EXPECT_NE(d.stack, noCell);
  EXPECT_NE(d.contract, noCell);
  EXPECT_NE(d.clone, noCell);
  EXPECT_NE(d.cursors, noCell);
  EXPECT_NE(d.cache, noCell);
  EXPECT_NE(d.vars, noCell);
  EXPECT_NE(d.values, noCell);
  EXPECT_NE(d.pinningCursors, noCell);
  EXPECT_NE(d.name, noCell);
  EXPECT_NE(d.stdlib, noCell);

  // Each dimension is linked along d.dims
  EXPECT_EQ(h.core.arena().linked(h.core.home(), d.dims, false), d.dims);
}

TEST(VortexCoreTest, LinkPrimitiveLifecycle) {
  TestHarness h;
  CellRef a        = h.arena.makeCell("nodeA");
  DimRef customDim = h.core.dims().step;

  // 1. Read unlinked -> nullopt
  EXPECT_EQ(h.core.link(a, customDim, false), std::nullopt);

  // 2. Allocate (target == -1)
  auto created = h.core.link(a, customDim, false, static_cast<CellRef>(-1));
  ASSERT_TRUE(created.has_value());
  EXPECT_NE(*created, noCell);
  EXPECT_NE(*created, a);

  // 3. Read linked
  auto readRes = h.core.link(a, customDim, false);
  ASSERT_TRUE(readRes.has_value());
  EXPECT_EQ(*readRes, *created);

  // Bidirectional link check
  EXPECT_EQ(h.arena.linked(*created, customDim, true), a);

  // 4. Isolate (target == 0)
  auto isolated = h.core.link(a, customDim, false, 0);
  ASSERT_TRUE(isolated.has_value());
  EXPECT_EQ(*isolated, *created);

  // Now unlinked
  EXPECT_EQ(h.core.link(a, customDim, false), std::nullopt);
  EXPECT_EQ(h.arena.linked(*created, customDim, true), noCell);

  // 5. Literal target
  CellRef b    = h.arena.makeCell("nodeB");
  auto literal = h.core.link(a, customDim, false, b);
  ASSERT_TRUE(literal.has_value());
  EXPECT_EQ(*literal, b);
  EXPECT_EQ(h.core.link(a, customDim, false), b);
  EXPECT_EQ(h.arena.linked(b, customDim, true), a);
}

TEST(VortexCoreTest, ValuePrimitiveReadWholeSliceAndSplice) {
  TestHarness h;
  CellRef c = h.arena.makeCell("The quick brown fox");

  // 1. Read Whole Content -> returns cell itself
  auto whole = h.core.value(c, 0, -1);
  ASSERT_TRUE(whole.has_value());
  EXPECT_EQ(*whole, c);
  EXPECT_EQ(h.core.render(c), CellValue(std::string("The quick brown fox")));

  // 2. Read Slice -> returns ephemeral cell quoting range
  auto slice = h.core.value(c, 4, 5); // "quick"
  ASSERT_TRUE(slice.has_value());
  EXPECT_NE(*slice, c);
  EXPECT_EQ(h.core.render(*slice), CellValue(std::string("quick")));

  // Negative indexing: offset -3, length 3 -> "fox"
  auto foxSlice = h.core.value(c, -3, 3);
  ASSERT_TRUE(foxSlice.has_value());
  EXPECT_EQ(h.core.render(*foxSlice), CellValue(std::string("fox")));

  // 3. Splice -> splices in-place on clone master, returns cell
  auto spliced = h.core.value(c, 10, 5, CellValue(std::string("silver")));
  ASSERT_TRUE(spliced.has_value());
  EXPECT_EQ(*spliced, c);
  EXPECT_EQ(h.core.render(c), CellValue(std::string("The quick silver fox")));
}

TEST(VortexCoreTest, StructuralIdentitySharingViaDClone) {
  TestHarness h;
  CellRef master = h.arena.makeCell("original text");

  // Mint clone posward
  CellRef cloneA = h.arena.makeCell();
  h.core.link(master, h.core.dims().clone, false, cloneA);

  CellRef cloneB = h.arena.makeCell();
  h.core.link(cloneA, h.core.dims().clone, false, cloneB);

  // All resolve to master's content
  EXPECT_EQ(h.core.render(master), CellValue(std::string("original text")));
  EXPECT_EQ(h.core.render(cloneA), CellValue(std::string("original text")));
  EXPECT_EQ(h.core.render(cloneB), CellValue(std::string("original text")));

  // Modifying via cloneA updates master and cloneB
  h.core.value(cloneA, 0, 8, CellValue(std::string("updated")));
  EXPECT_EQ(h.core.render(master), CellValue(std::string("updated text")));
  EXPECT_EQ(h.core.render(cloneA), CellValue(std::string("updated text")));
  EXPECT_EQ(h.core.render(cloneB), CellValue(std::string("updated text")));

  // Linking a new cell negward of master makes it the new head
  CellRef newHead = h.arena.makeCell("overriding authority");
  h.core.link(master, h.core.dims().clone, true, newHead);

  // Instantly all cells resolve through the new head
  EXPECT_EQ(h.core.render(newHead),
            CellValue(std::string("overriding authority")));
  EXPECT_EQ(h.core.render(master),
            CellValue(std::string("overriding authority")));
  EXPECT_EQ(h.core.render(cloneA),
            CellValue(std::string("overriding authority")));
  EXPECT_EQ(h.core.render(cloneB),
            CellValue(std::string("overriding authority")));
}

TEST(VortexCoreTest, DualWingCallingConventionAndSnapshotBeforeWrite) {
  TestHarness h;
  // #ADD out out out -> in-place accumulator: out = out + out
  CellRef outCell = h.arena.makeScalarCell(static_cast<std::int64_t>(10));

  CellRef addOp = h.vm.mintOpcode(OpcodeKind::Add, "#ADD");
  // Bind outCell as in0 and in1
  h.core.bindInput(addOp, outCell);
  h.core.bindInput(addOp, outCell);
  // Bind outCell as output destination
  h.core.bindOutput(addOp, outCell);

  std::vector<CellRef> ins = h.core.inputsOf(addOp);
  ASSERT_EQ(ins.size(), 2u);
  EXPECT_EQ(ins[0], outCell);
  EXPECT_EQ(ins[1], outCell);

  std::vector<CellRef> outs = h.core.outputsOf(addOp);
  ASSERT_EQ(outs.size(), 1u);
  EXPECT_EQ(outs[0], outCell);

  CellRef cursor = h.vm.spawnCursor(addOp, "calc");
  auto res       = h.vm.step(cursor);
  EXPECT_TRUE(res.success);

  // Snapshot-before-write ensures 10 + 10 = 20
  EXPECT_EQ(h.core.render(outCell), CellValue(static_cast<std::int64_t>(20)));
}

TEST(VortexCoreTest, ParameterPipelinesAlongDSpin) {
  TestHarness h;
  // Test input preprocessing pipeline
  CellRef inParam = h.arena.makeScalarCell(static_cast<std::int64_t>(5));

  // Pipeline on inParam: #ADD 10 to inParam before opcode consumes it
  CellRef preOp = h.vm.mintOpcode(OpcodeKind::Add, "#PRE_ADD");
  CellRef ten   = h.arena.makeScalarCell(static_cast<std::int64_t>(10));
  h.core.bindInput(preOp, inParam);
  h.core.bindInput(preOp, ten);
  h.core.attachPipeline(inParam, preOp);

  // Main opcode: #MUL inParam * 2
  CellRef mainOp = h.vm.mintOpcode(OpcodeKind::Mul, "#MAIN_MUL");
  CellRef two    = h.arena.makeScalarCell(static_cast<std::int64_t>(2));
  h.core.bindInput(mainOp, inParam);
  h.core.bindInput(mainOp, two);

  CellRef outTarget = h.arena.makeCell();
  h.core.bindOutput(mainOp, outTarget);

  // Postprocessing pipeline on outTarget: #ADD 1 to result
  CellRef postOp = h.vm.mintOpcode(OpcodeKind::Add, "#POST_ADD");
  CellRef one    = h.arena.makeScalarCell(static_cast<std::int64_t>(1));
  h.core.bindInput(postOp, outTarget);
  h.core.bindInput(postOp, one);
  h.core.attachPipeline(outTarget, postOp);

  CellRef cursor = h.vm.spawnCursor(mainOp, "pipeline_test");
  auto res       = h.vm.step(cursor);
  EXPECT_TRUE(res.success);

  // inParam was 5 -> preprocessed to 5 + 10 = 15
  // mainOp: 15 * 2 = 30
  // outTarget postprocessed: 30 + 1 = 31!
  EXPECT_EQ(h.core.render(outTarget), CellValue(static_cast<std::int64_t>(31)));
}

TEST(VortexCoreTest,
     DesignByContractPreconditionFailureAbortsWithoutModifyingOutput) {
  TestHarness h;
  CellRef inParam   = h.arena.makeScalarCell(static_cast<std::int64_t>(-5));
  CellRef outTarget = h.arena.makeScalarCell(static_cast<std::int64_t>(999));

  CellRef op  = h.vm.mintOpcode(OpcodeKind::Add, "#ADD");
  CellRef one = h.arena.makeScalarCell(static_cast<std::int64_t>(1));
  h.core.bindInput(op, inParam);
  h.core.bindInput(op, one);
  h.core.bindOutput(op, outTarget);

  // Precondition: inParam must be > 0 (#GT inParam 0)
  CellRef zero = h.arena.makeScalarCell(static_cast<std::int64_t>(0));
  CellRef req  = h.vm.mintOpcode(OpcodeKind::Gt, "#REQUIRE_POSITIVE");
  h.core.bindInput(req, inParam);
  h.core.bindInput(req, zero);
  h.core.attachPrecondition(op, req);

  CellRef cursor = h.vm.spawnCursor(op, "contract_test");
  auto res       = h.vm.step(cursor);

  // Execution must fail with PreconditionViolation
  EXPECT_FALSE(res.success);
  EXPECT_EQ(res.contractViolation, ContractViolationKind::Precondition);
  EXPECT_EQ(res.failingClause, req);

  // Output destination MUST remain completely untouched (999)
  EXPECT_EQ(h.core.render(outTarget),
            CellValue(static_cast<std::int64_t>(999)));
}

TEST(VortexCoreTest, DesignByContractPostconditionVerification) {
  TestHarness h;
  CellRef inParam   = h.arena.makeScalarCell(static_cast<std::int64_t>(10));
  CellRef outTarget = h.arena.makeCell();

  CellRef op   = h.vm.mintOpcode(OpcodeKind::Add, "#ADD");
  CellRef five = h.arena.makeScalarCell(static_cast<std::int64_t>(5));
  h.core.bindInput(op, inParam);
  h.core.bindInput(op, five);
  h.core.bindOutput(op, outTarget);

  // Postcondition: outTarget >= inParam (15 >= 10)
  CellRef ensureOp = h.vm.mintOpcode(OpcodeKind::Gte, "#ENSURE_GROWTH");
  h.core.bindInput(ensureOp, outTarget);
  h.core.bindInput(ensureOp, inParam);
  h.core.attachPostcondition(op, ensureOp);

  CellRef cursor = h.vm.spawnCursor(op, "postcond_pass");
  auto res       = h.vm.step(cursor);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(h.core.render(outTarget), CellValue(static_cast<std::int64_t>(15)));
}

TEST(VortexCoreTest, VortexNativeMemoizationLifecycle) {
  TestHarness h;
  // Define an expensive operation: #MUL x x
  CellRef op      = h.vm.mintOpcode(OpcodeKind::Mul, "#SQUARE");
  CellRef inParam = h.arena.makeScalarCell(static_cast<std::int64_t>(7));
  h.core.bindInput(op, inParam);
  h.core.bindInput(op, inParam);
  CellRef outTarget = h.arena.makeCell();
  h.core.bindOutput(op, outTarget);

  // Opt-in to memoization with key "memo:square"
  h.vm.enableMemoization(op, "memo:square");
  EXPECT_TRUE(h.vm.isMemoized(op));

  // First execution: Cache Miss -> evaluates and records to d.cache
  CellRef cursor1 = h.vm.spawnCursor(op, "calc1");
  auto res1       = h.vm.step(cursor1);
  EXPECT_TRUE(res1.success);
  EXPECT_EQ(h.core.render(outTarget), CellValue(static_cast<std::int64_t>(49)));

  // Verify memo entry exists on pinning cursor
  auto pinOpt = h.core.findPin("memo:square");
  ASSERT_TRUE(pinOpt.has_value());
  CellRef pin = *pinOpt;
  auto cached = h.core.lookupMemo(
      pin, {static_cast<std::int64_t>(7), static_cast<std::int64_t>(7)});
  ASSERT_TRUE(cached.has_value());
  ASSERT_EQ(cached->size(), 1u);
  EXPECT_EQ((*cached)[0], CellValue(static_cast<std::int64_t>(49)));

  // Change outTarget to 0 to verify Cache Hit overwrites it from cache
  h.core.value(outTarget, 0, -1, static_cast<std::int64_t>(0));
  EXPECT_EQ(h.core.render(outTarget), CellValue(static_cast<std::int64_t>(0)));

  // Second execution: Cache Hit!
  CellRef cursor2 = h.vm.spawnCursor(op, "calc2");
  auto res2       = h.vm.step(cursor2);
  EXPECT_TRUE(res2.success);
  EXPECT_EQ(h.core.render(outTarget), CellValue(static_cast<std::int64_t>(49)));

  // Atomic flush of memoization cache
  EXPECT_TRUE(h.core.flushMemo("memo:square"));
  auto cachedAfterFlush = h.core.lookupMemo(
      pin, {static_cast<std::int64_t>(7), static_cast<std::int64_t>(7)});
  EXPECT_FALSE(cachedAfterFlush.has_value());

  // Retirement of memoization cache
  EXPECT_TRUE(h.core.retireMemo("memo:square"));
  EXPECT_FALSE(h.core.findPin("memo:square").has_value());
}

TEST(VortexCoreTest, MultiOutputDivModInstruction) {
  TestHarness h;
  CellRef divmodOp = h.vm.mintOpcode(OpcodeKind::DivMod, "#DIVMOD");
  CellRef a        = h.arena.makeScalarCell(static_cast<std::int64_t>(17));
  CellRef b        = h.arena.makeScalarCell(static_cast<std::int64_t>(5));
  h.core.bindInput(divmodOp, a);
  h.core.bindInput(divmodOp, b);

  CellRef quotient  = h.arena.makeCell();
  CellRef remainder = h.arena.makeCell();
  h.core.bindOutput(divmodOp, quotient);
  h.core.bindOutput(divmodOp, remainder);

  CellRef cursor = h.vm.spawnCursor(divmodOp, "divmod_test");
  auto res       = h.vm.step(cursor);
  EXPECT_TRUE(res.success);

  // 17 / 5 = 3 remainder 2
  EXPECT_EQ(h.core.render(quotient), CellValue(static_cast<std::int64_t>(3)));
  EXPECT_EQ(h.core.render(remainder), CellValue(static_cast<std::int64_t>(2)));
}

TEST(VortexCoreTest, ControlFlowCallAndReturnOnDStack) {
  TestHarness h;
  // Subroutine: #ADD 100 to input
  CellRef subOp   = h.vm.mintOpcode(OpcodeKind::Add, "#SUBROUTINE_ADD");
  CellRef inCell  = h.arena.makeScalarCell(static_cast<std::int64_t>(20));
  CellRef hundred = h.arena.makeScalarCell(static_cast<std::int64_t>(100));
  CellRef outCell = h.arena.makeCell();
  h.core.bindInput(subOp, inCell);
  h.core.bindInput(subOp, hundred);
  h.core.bindOutput(subOp, outCell);

  CellRef retOp = h.vm.mintOpcode(OpcodeKind::Return, "#RETURN");
  h.core.arena().link(subOp, h.core.dims().spin, false, retOp);

  // Caller: #CALL subOp, then #MUL outCell * 2
  CellRef callOp    = h.vm.mintOpcode(OpcodeKind::Call, "#CALL");
  CellRef subTarget = h.arena.makeScalarCell(static_cast<std::int64_t>(subOp));
  h.core.bindInput(callOp, subTarget);

  CellRef afterCall = h.vm.mintOpcode(OpcodeKind::Mul, "#AFTER_CALL");
  CellRef two       = h.arena.makeScalarCell(static_cast<std::int64_t>(2));
  h.core.bindInput(afterCall, outCell);
  h.core.bindInput(afterCall, two);
  h.core.bindOutput(afterCall, outCell);

  h.core.arena().link(callOp, h.core.dims().spin, false, afterCall);

  CellRef cursor = h.vm.spawnCursor(callOp, "main_thread");
  auto res       = h.vm.run(cursor, 10);
  EXPECT_TRUE(res.success);

  // Subroutine: 20 + 100 = 120
  // After call: 120 * 2 = 240
  EXPECT_EQ(h.core.render(outCell), CellValue(static_cast<std::int64_t>(240)));
}

TEST(VortexCoreTest, CursorAssociativeScopes) {
  TestHarness h;
  CellRef bindOp  = h.vm.mintOpcode(OpcodeKind::Bind, "#BIND");
  CellRef varName = h.arena.makeCell("counter");
  CellRef varVal  = h.arena.makeScalarCell(static_cast<std::int64_t>(42));
  h.core.bindInput(bindOp, varName);
  h.core.bindInput(bindOp, varVal);

  CellRef resolveOp = h.vm.mintOpcode(OpcodeKind::Resolve, "#RESOLVE");
  h.core.bindInput(resolveOp, varName);
  CellRef outCell = h.arena.makeCell();
  h.core.bindOutput(resolveOp, outCell);

  h.core.arena().link(bindOp, h.core.dims().spin, false, resolveOp);

  CellRef cursor = h.vm.spawnCursor(bindOp, "scope_test");
  auto res       = h.vm.run(cursor, 5);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(h.core.render(outCell), CellValue(static_cast<std::int64_t>(42)));
}

} // namespace

/**
 * @file vortex_core_test.cpp
 * @brief Unit test suite for the Vortex Hyperstructural Runtime Core (Stage 1).
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <limits>
#include <memory>
#include <ranges>
#include <string>
#include <type_traits>
#include <vector>

#include "common/xanadu/relocation_marks.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
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
  EXPECT_EQ(h.core.arena().linked(h.core.home(), d.dims, DimVector::POS),
            d.dims);
}

TEST(VortexCoreTest, TruthinessKeepsItsExplicitWordsAndProseFallback) {
  EXPECT_TRUE(VortexCore::evaluateTruthiness(CellValue{std::string{"YeS"}}));
  EXPECT_TRUE(VortexCore::evaluateTruthiness(CellValue{std::string{"ON"}}));
  EXPECT_FALSE(VortexCore::evaluateTruthiness(CellValue{std::string{"Off"}}));
  EXPECT_FALSE(VortexCore::evaluateTruthiness(CellValue{std::string{}}));
  EXPECT_TRUE(
      VortexCore::evaluateTruthiness(CellValue{std::string{"a report title"}}));
}

TEST(VortexCoreTest, LinkPrimitiveLifecycle) {
  TestHarness h;
  CellRef a        = h.arena.makeCell("nodeA");
  DimRef customDim = h.core.dims().step;

  // 1. Read unlinked -> nullopt
  EXPECT_EQ(h.core.readLink(a, customDim, DimVector::POS), std::nullopt);

  // 2. Allocate (target == -1)
  auto created = h.core.allocateLink(a, customDim, DimVector::POS);
  ASSERT_TRUE(created.has_value());
  EXPECT_NE(*created, noCell);
  EXPECT_NE(*created, a);

  // 3. Read linked
  auto readRes = h.core.readLink(a, customDim, DimVector::POS);
  ASSERT_TRUE(readRes.has_value());
  EXPECT_EQ(*readRes, *created);

  // Bidirectional link check
  EXPECT_EQ(h.arena.linked(*created, customDim, DimVector::NEG), a);

  // 4. Isolate (target == 0)
  auto isolated = h.core.isolateLink(a, customDim, DimVector::POS);
  ASSERT_TRUE(isolated.has_value());
  EXPECT_EQ(*isolated, *created);

  // Now unlinked
  EXPECT_EQ(h.core.readLink(a, customDim, DimVector::POS), std::nullopt);
  EXPECT_EQ(h.arena.linked(*created, customDim, DimVector::NEG), std::nullopt);

  // 5. Literal target
  CellRef b    = h.arena.makeCell("nodeB");
  auto literal = h.core.setLink(a, customDim, DimVector::POS, b);
  ASSERT_TRUE(literal.has_value());
  EXPECT_EQ(*literal, b);
  EXPECT_EQ(h.core.readLink(a, customDim, DimVector::POS), b);
  EXPECT_EQ(h.arena.linked(b, customDim, DimVector::NEG), a);
}

TEST(VortexCoreTest, ValuePrimitiveReadWholeSliceAndSplice) {
  TestHarness h;
  CellRef c = h.arena.makeCell("The quick brown fox");

  // 1. Read Whole Content -> returns cell itself
  auto whole = h.core.readValue(c, 0, std::nullopt);
  ASSERT_TRUE(whole.has_value());
  EXPECT_EQ(*whole, c);
  EXPECT_EQ(h.core.render(c), CellValue(std::string("The quick brown fox")));

  // 2. Read Slice -> returns ephemeral cell quoting range
  auto slice = h.core.readValue(c, 4, 5); // "quick"
  ASSERT_TRUE(slice.has_value());
  EXPECT_NE(*slice, c);
  EXPECT_EQ(h.core.render(*slice), CellValue(std::string("quick")));

  // Negative indexing: offset -3, length 3 -> "fox"
  auto foxSlice = h.core.readValue(c, -3, 3);
  ASSERT_TRUE(foxSlice.has_value());
  EXPECT_EQ(h.core.render(*foxSlice), CellValue(std::string("fox")));

  // 3. Splice -> splices in-place on clone master, returns cell
  auto spliced = h.core.writeValue(c, 10, 5, CellValue(std::string("silver")));
  ASSERT_TRUE(spliced.has_value());
  EXPECT_EQ(*spliced, c);
  EXPECT_EQ(h.core.render(c), CellValue(std::string("The quick silver fox")));
}

TEST(VortexCoreTest, ValueLengthAbsenceAndZeroAreDistinct) {
  TestHarness h;
  const CellRef cell = h.arena.makeCell("abcdef");

  const auto whole = h.core.readValue(cell);
  ASSERT_TRUE(whole);
  EXPECT_EQ(*whole, cell);

  const auto empty = h.core.readValue(cell, 2, 0);
  ASSERT_TRUE(empty);
  EXPECT_EQ(h.core.render(*empty), CellValue(std::string{}));

  const auto remaining = h.core.readValue(cell, 2, std::nullopt);
  ASSERT_TRUE(remaining);
  EXPECT_EQ(h.core.render(*remaining), CellValue(std::string("cdef")));

  ASSERT_TRUE(h.core.writeValue(cell, 2, 0, std::string("X")));
  EXPECT_EQ(h.core.render(cell), CellValue(std::string("abXcdef")));
  ASSERT_TRUE(h.core.writeValue(cell, 2, std::nullopt, std::string("Y")));
  EXPECT_EQ(h.core.render(cell), CellValue(std::string("abY")));
}

TEST(VortexCoreTest, GetDistinguishesMissingCellFromFalseyValues) {
  TestHarness h;
  const auto falseCell = h.arena.makeScalarCell(false);
  const auto emptyCell = h.arena.makeCell("");
  const auto zeroCell  = h.arena.makeScalarCell(static_cast<std::int64_t>(0));

  EXPECT_EQ(h.core.get(noCell), std::nullopt);

  const auto falseValue = h.core.get(falseCell);
  ASSERT_TRUE(falseValue);
  ASSERT_TRUE(std::holds_alternative<bool>(*falseValue));
  EXPECT_FALSE(std::get<bool>(*falseValue));

  const auto emptyValue = h.core.get(emptyCell);
  ASSERT_TRUE(emptyValue);
  ASSERT_TRUE(std::holds_alternative<std::string>(*emptyValue));
  EXPECT_TRUE(std::get<std::string>(*emptyValue).empty());

  const auto zeroValue = h.core.get(zeroCell);
  ASSERT_TRUE(zeroValue);
  ASSERT_TRUE(std::holds_alternative<std::int64_t>(*zeroValue));
  EXPECT_EQ(std::get<std::int64_t>(*zeroValue), 0);
}

TEST(VortexCoreTest, RenderDistinguishesMissingCellFromFalseyValues) {
  TestHarness h;
  const auto falseCell = h.arena.makeScalarCell(false);
  const auto emptyCell = h.arena.makeCell("");
  const auto zeroCell  = h.arena.makeScalarCell(std::int64_t{0});

  EXPECT_EQ(h.core.render(noCell), std::nullopt);
  EXPECT_EQ(h.core.render(falseCell), std::optional<CellValue>{false});
  EXPECT_EQ(h.core.render(emptyCell), std::optional<CellValue>{std::string{}});
  EXPECT_EQ(h.core.render(zeroCell), std::optional<CellValue>{std::int64_t{0}});
  EXPECT_EQ(h.core.isTruthy(noCell), std::nullopt);
  EXPECT_EQ(h.core.isTruthy(falseCell), std::optional<bool>{false});
}

TEST(VortexCoreTest, StructuralIdentitySharingViaDClone) {
  TestHarness h;
  CellRef master = h.arena.makeCell("original text");

  // Mint clone posward
  CellRef cloneA = h.arena.makeCell();
  h.core.setLink(master, h.core.dims().clone, DimVector::POS, cloneA);

  CellRef cloneB = h.arena.makeCell();
  h.core.setLink(cloneA, h.core.dims().clone, DimVector::POS, cloneB);

  // All resolve to master's content
  EXPECT_EQ(h.core.render(master), CellValue(std::string("original text")));
  EXPECT_EQ(h.core.render(cloneA), CellValue(std::string("original text")));
  EXPECT_EQ(h.core.render(cloneB), CellValue(std::string("original text")));

  // Modifying via cloneA updates master and cloneB
  h.core.writeValue(cloneA, 0, 8, CellValue(std::string("updated")));
  EXPECT_EQ(h.core.render(master), CellValue(std::string("updated text")));
  EXPECT_EQ(h.core.render(cloneA), CellValue(std::string("updated text")));
  EXPECT_EQ(h.core.render(cloneB), CellValue(std::string("updated text")));

  // Linking a new cell negward of master makes it the new head
  CellRef newHead = h.arena.makeCell("overriding authority");
  h.core.setLink(master, h.core.dims().clone, DimVector::NEG, newHead);

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

TEST(VortexCoreTest, WingSlotsUseUniqueMarkedOpHandlesAndPromoteByTwin) {
  TestHarness h;
  const auto firstInput   = h.arena.makeCell("first input");
  const auto secondInput  = h.arena.makeCell("second input");
  const auto firstOutput  = h.arena.makeCell("first output");
  const auto secondOutput = h.arena.makeCell("second output");
  const auto opcode       = h.vm.mintOpcode(OpcodeKind::Nop, "#WINGS");

  h.core.bindInput(opcode, firstInput);
  h.core.bindInput(opcode, secondInput);
  h.core.bindOutput(opcode, firstOutput);
  h.core.bindOutput(opcode, secondOutput);

  EXPECT_EQ(h.core.inputsOf(opcode),
            (std::vector<CellRef>{firstInput, secondInput}));
  EXPECT_EQ(h.core.outputsOf(opcode),
            (std::vector<CellRef>{firstOutput, secondOutput}));

  const auto relocation = h.core.findDimension(xanadu::cellRelocationDimension);
  ASSERT_TRUE(relocation);
  EXPECT_FALSE(h.arena.dimensionNamed(xanadu::cellRelocationDimension));

  const auto inputSlots  = h.core.rawWingSlotsOf(opcode);
  const auto outputSlots = h.core.rawWingSlotsOf(opcode, true);
  ASSERT_EQ(inputSlots.size(), 2u);
  ASSERT_EQ(outputSlots.size(), 2u);
  std::vector<CellRef> markers;
  const auto checkWing = [&](const std::vector<CellRef> &slots,
                             const std::vector<CellRef> &targets) {
    for (std::size_t i = 0; i < slots.size(); ++i) {
      EXPECT_EQ(h.arena.valueKindOf(slots[i]), xanadu::ValueKind::OpHandle);
      EXPECT_EQ(h.arena.handleTarget(slots[i]), targets[i]);
      const auto marker = h.arena.linked(slots[i], *relocation, DimVector::POS);
      ASSERT_TRUE(marker);
      EXPECT_EQ(h.arena.textOf(*marker), xanadu::cellRelocationMarker);
      markers.push_back(*marker);
    }
  };
  checkWing(inputSlots, {firstInput, secondInput});
  checkWing(outputSlots, {firstOutput, secondOutput});
  EXPECT_EQ(std::ranges::count_if(markers,
                                  [&](const CellRef marker) {
                                    return std::ranges::count(markers,
                                                              marker) == 1;
                                  }),
            4);

  xanadu::Store store;
  const auto promoted = zigzag::promote(store, store.latest(), h.arena, opcode);
  ASSERT_TRUE(promoted);
  const auto twinOf = [&](const CellRef ref) -> std::optional<CellRef> {
    const auto found = std::ranges::find(promoted->arena, ref);
    if (found == promoted->arena.end()) return std::nullopt;
    return promoted->cells[static_cast<std::size_t>(
        std::distance(promoted->arena.begin(), found))];
  };
  const auto fold = store.rebuildManifold(promoted->version);
  for (const auto &[slot, target] : {std::pair{inputSlots[0], firstInput},
                                     std::pair{inputSlots[1], secondInput},
                                     std::pair{outputSlots[0], firstOutput},
                                     std::pair{outputSlots[1], secondOutput}}) {
    const auto slotTwin   = twinOf(slot);
    const auto targetTwin = twinOf(target);
    ASSERT_TRUE(slotTwin);
    ASSERT_TRUE(targetTwin);
    EXPECT_EQ(fold.valueKindOf(*slotTwin), xanadu::ValueKind::OpHandle);
    const auto persistedSlot = fold.slot(*slotTwin);
    ASSERT_TRUE(persistedSlot);
    EXPECT_EQ(persistedSlot->valueBits, *targetTwin);
  }
}

TEST(VortexCoreTest, LegacyInt64WingsDecodeAndUnmarkedIntegersStayUnharvested) {
  TestHarness h;
  const auto target = h.arena.makeCell("legacy target");
  const auto opcode = h.vm.mintOpcode(OpcodeKind::Nop, "#LEGACY_WING");
  const auto legacy = h.arena.makeScalarCell(static_cast<std::int64_t>(target));
  ASSERT_TRUE(h.arena.link(opcode, h.core.dims().grab, DimVector::POS, legacy));
  EXPECT_EQ(h.core.inputsOf(opcode), (std::vector<CellRef>{target}));

  const auto unmarked =
      h.arena.makeScalarCell(static_cast<std::int64_t>(target));
  EXPECT_THAT(xanadu::harvestRelocationTargets(h.arena),
              ::testing::Not(::testing::Contains(target)));
  EXPECT_THAT(xanadu::markedRelocationSlots(h.arena),
              ::testing::Not(::testing::Contains(unmarked)));
}

TEST(VortexCoreTest, RelocationTargetsRejectTruncatedInt64References) {
  TestHarness h;
  const auto target = h.arena.makeCell("valid target");
  const auto overflowing =
      static_cast<std::uint64_t>(std::numeric_limits<CellRef>::max()) + 1U +
      target;
  const auto tooLarge =
      h.arena.makeScalarCell(static_cast<std::int64_t>(overflowing));
  const auto negative = h.arena.makeScalarCell(std::int64_t{-1});
  EXPECT_FALSE(xanadu::relocationTarget(h.arena, tooLarge));
  EXPECT_FALSE(xanadu::relocationTarget(h.arena, negative));
}

TEST(VortexCoreTest, RankOnlyRelocationDimensionHarvestsMarkedInt64Targets) {
  TestHarness h;
  const auto target  = h.arena.makeCell("relocation target");
  const auto refCell = xanadu::refCell(h.arena, h.core, target);

  EXPECT_FALSE(h.arena.dimensionNamed(xanadu::cellRelocationDimension));
  ASSERT_TRUE(h.core.findDimension(xanadu::cellRelocationDimension));
  EXPECT_THAT(xanadu::markedRelocationSlots(h.arena),
              ::testing::Contains(refCell));
  EXPECT_THAT(xanadu::harvestRelocationTargets(h.arena),
              ::testing::Contains(target));

  xanadu::Store store;
  const auto promoted =
      zigzag::promote(store, store.latest(), h.arena, refCell, {},
                      xanadu::harvestRelocationTargets(h.arena));
  ASSERT_TRUE(promoted);
  const auto twinOf = [&](const CellRef ref) -> std::optional<CellRef> {
    const auto found = std::ranges::find(promoted->arena, ref);
    if (found == promoted->arena.end()) return std::nullopt;
    return promoted->cells[static_cast<std::size_t>(
        std::distance(promoted->arena.begin(), found))];
  };
  const auto slotTwin   = twinOf(refCell);
  const auto targetTwin = twinOf(target);
  ASSERT_TRUE(slotTwin);
  ASSERT_TRUE(targetTwin);
  const auto version = store.setValue(
      promoted->version, *slotTwin, xanadu::PrimediaSpan{},
      xanadu::ValueKind::Int64,
      std::bit_cast<std::uint64_t>(static_cast<std::int64_t>(*targetTwin)));
  const auto fold          = store.rebuildManifold(version);
  const auto persistedSlot = fold.slot(*slotTwin);
  ASSERT_TRUE(persistedSlot);
  EXPECT_EQ(std::bit_cast<std::int64_t>(persistedSlot->valueBits),
            static_cast<std::int64_t>(*targetTwin));
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
  h.core.writeValue(outTarget, 0, std::nullopt, static_cast<std::int64_t>(0));
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
  EXPECT_TRUE(
      h.core.arena().link(subOp, h.core.dims().spin, DimVector::POS, retOp));

  // Caller: #CALL subOp, then #MUL outCell * 2
  CellRef callOp    = h.vm.mintOpcode(OpcodeKind::Call, "#CALL");
  CellRef subTarget = h.arena.makeScalarCell(static_cast<std::int64_t>(subOp));
  h.core.bindInput(callOp, subTarget);

  CellRef afterCall = h.vm.mintOpcode(OpcodeKind::Mul, "#AFTER_CALL");
  CellRef two       = h.arena.makeScalarCell(static_cast<std::int64_t>(2));
  h.core.bindInput(afterCall, outCell);
  h.core.bindInput(afterCall, two);
  h.core.bindOutput(afterCall, outCell);

  EXPECT_TRUE(h.core.arena().link(callOp, h.core.dims().spin, DimVector::POS,
                                  afterCall));

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

  EXPECT_TRUE(h.core.arena().link(bindOp, h.core.dims().spin, DimVector::POS,
                                  resolveOp));

  CellRef cursor = h.vm.spawnCursor(bindOp, "scope_test");
  auto res       = h.vm.run(cursor, 5);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(h.core.render(outCell), CellValue(static_cast<std::int64_t>(42)));
}

} // namespace

namespace {

struct NumericExecution {
  CellRef opcode;
  std::vector<CellRef> outputs;
  ExecutionResult result;
};

NumericExecution executeNumeric(TestHarness &h, OpcodeKind kind,
                                const std::vector<CellValue> &inputs) {
  NumericExecution execution{h.vm.mintOpcode(kind), {}, {}};
  for (const auto &input : inputs) {
    const auto cell = h.arena.makeCell();
    h.core.writeValue(cell, 0, std::nullopt, input);
    h.core.bindInput(execution.opcode, cell);
  }
  const auto outputCount = kind == OpcodeKind::DivMod ? 2 : 1;
  for (int i = 0; i < outputCount; ++i) {
    const auto output = h.arena.makeScalarCell(std::int64_t{123});
    h.core.bindOutput(execution.opcode, output);
    execution.outputs.push_back(output);
  }
  execution.result = h.vm.step(h.vm.spawnCursor(execution.opcode));
  return execution;
}

TEST(VortexCoreTest, Int64RelationsPreserveAdjacentLargeValuesAndExtremes) {
  constexpr auto min = std::numeric_limits<std::int64_t>::min();
  constexpr auto max = std::numeric_limits<std::int64_t>::max();
  const std::vector<std::pair<std::int64_t, std::int64_t>> ordered{
      {9007199254740992LL, 9007199254740993LL},
      {min, min + 1},
      {max - 1, max},
      {min, max}};
  for (const auto &[left, right] : ordered) {
    for (const auto kind :
         {OpcodeKind::Lt, OpcodeKind::Lte, OpcodeKind::Gt, OpcodeKind::Gte}) {
      for (const bool reverse : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << static_cast<int>(kind) << ": " << left << ", " << right
                     << ", reversed=" << reverse);
        TestHarness h;
        auto execution = executeNumeric(
            h, kind, {reverse ? right : left, reverse ? left : right});
        ASSERT_TRUE(execution.result.success);
        const bool less = kind == OpcodeKind::Lt || kind == OpcodeKind::Lte;
        EXPECT_EQ(h.core.render(execution.outputs[0]),
                  CellValue{less != reverse});
      }
      TestHarness h;
      auto equal = executeNumeric(h, kind, {left, left});
      ASSERT_TRUE(equal.result.success);
      EXPECT_EQ(h.core.render(equal.outputs[0]),
                CellValue{kind == OpcodeKind::Lte || kind == OpcodeKind::Gte});
    }
  }
}

TEST(VortexCoreTest, SignedArithmeticOverflowRejectsWithoutWritingAnyOutput) {
  constexpr auto min = std::numeric_limits<std::int64_t>::min();
  constexpr auto max = std::numeric_limits<std::int64_t>::max();
  const std::vector<std::pair<OpcodeKind, std::vector<CellValue>>> cases{
      {OpcodeKind::Add, {max, std::int64_t{1}}},
      {OpcodeKind::Add, {min, std::int64_t{-1}}},
      {OpcodeKind::Sub, {min, std::int64_t{1}}},
      {OpcodeKind::Sub, {max, std::int64_t{-1}}},
      {OpcodeKind::Sub, {min}},
      {OpcodeKind::Mul, {max, std::int64_t{2}}},
      {OpcodeKind::Mul, {min, std::int64_t{-1}}},
      {OpcodeKind::Neg, {min}},
      {OpcodeKind::Abs, {min}},
      {OpcodeKind::Div, {min, std::int64_t{-1}}},
      {OpcodeKind::Mod, {min, std::int64_t{-1}}},
      {OpcodeKind::DivMod, {min, std::int64_t{-1}}}};
  for (const auto &[kind, inputs] : cases) {
    SCOPED_TRACE(static_cast<int>(kind));
    TestHarness h;
    auto execution = executeNumeric(h, kind, inputs);
    EXPECT_FALSE(execution.result.success);
    EXPECT_EQ(execution.result.contractViolation,
              ContractViolationKind::Precondition);
    EXPECT_EQ(execution.result.failingClause, execution.opcode);
    EXPECT_THAT(execution.result.errorMessage,
                ::testing::HasSubstr("overflow"));
    for (const auto output : execution.outputs) {
      EXPECT_EQ(h.core.render(output), CellValue{std::int64_t{123}});
    }
  }
}

TEST(VortexCoreTest, SignedArithmeticSafeEdgesAndUnaryDoubleRemainValid) {
  constexpr auto min = std::numeric_limits<std::int64_t>::min();
  constexpr auto max = std::numeric_limits<std::int64_t>::max();
  struct Case {
    OpcodeKind kind;
    std::vector<CellValue> inputs;
    std::vector<CellValue> outputs;
  };
  const std::vector<Case> cases{
      {OpcodeKind::Add, {max, std::int64_t{0}}, {max}},
      {OpcodeKind::Add, {min, std::int64_t{0}}, {min}},
      {OpcodeKind::Sub, {min, min}, {std::int64_t{0}}},
      {OpcodeKind::Sub, {max, max}, {std::int64_t{0}}},
      {OpcodeKind::Sub, {min + 1}, {max}},
      {OpcodeKind::Sub, {1.25}, {-1.25}},
      {OpcodeKind::Mul, {min, std::int64_t{1}}, {min}},
      {OpcodeKind::Mul, {max, std::int64_t{1}}, {max}},
      {OpcodeKind::Neg, {-max}, {max}},
      {OpcodeKind::Abs, {-max}, {max}},
      {OpcodeKind::Div, {min, std::int64_t{1}}, {min}},
      {OpcodeKind::Mod, {min, std::int64_t{1}}, {std::int64_t{0}}},
      {OpcodeKind::DivMod, {min, std::int64_t{1}}, {min, std::int64_t{0}}},
      {OpcodeKind::Div, {min, std::int64_t{0}}, {0.0}},
      {OpcodeKind::Mod, {min, std::int64_t{0}}, {std::int64_t{0}}},
      {OpcodeKind::DivMod,
       {min, std::int64_t{0}},
       {std::int64_t{0}, std::int64_t{0}}},
      {OpcodeKind::Add, {std::int64_t{2}, 0.5}, {2.5}},
      {OpcodeKind::Sub, {std::int64_t{2}, 0.5}, {1.5}},
      {OpcodeKind::Mul, {std::int64_t{2}, 0.5}, {1.0}},
      {OpcodeKind::Div, {std::int64_t{2}, 0.5}, {4.0}}};
  for (const auto &[kind, inputs, outputs] : cases) {
    SCOPED_TRACE(static_cast<int>(kind));
    TestHarness h;
    auto execution = executeNumeric(h, kind, inputs);
    ASSERT_TRUE(execution.result.success) << execution.result.errorMessage;
    for (std::size_t i = 0; i < outputs.size(); ++i) {
      EXPECT_EQ(h.core.render(execution.outputs[i]), outputs[i]);
    }
  }
}

TEST(VortexCoreTest, RelationsRetainMixedNumericAndLexicographicSemantics) {
  struct Case {
    OpcodeKind kind;
    CellValue left;
    CellValue right;
    bool result;
  };
  const std::vector<Case> cases{
      {OpcodeKind::Lt, std::int64_t{2}, 2.5, true},
      {OpcodeKind::Lte, 2.0, std::int64_t{2}, true},
      {OpcodeKind::Gt, 2.5, std::int64_t{2}, true},
      {OpcodeKind::Gte, std::int64_t{2}, 2.0, true},
      {OpcodeKind::Lt, std::int64_t{9007199254740993LL}, 9007199254740992.0,
       false},
      {OpcodeKind::Lt, std::string{"10"}, std::string{"2"}, true},
      {OpcodeKind::Lte, std::string{"same"}, std::string{"same"}, true},
      {OpcodeKind::Gt, std::string{"b"}, std::string{"aa"}, true},
      {OpcodeKind::Gte, std::string{"same"}, std::string{"same"}, true}};
  for (const auto &[kind, left, right, expected] : cases) {
    TestHarness h;
    auto execution = executeNumeric(h, kind, {left, right});
    ASSERT_TRUE(execution.result.success);
    EXPECT_EQ(h.core.render(execution.outputs[0]), CellValue{expected});
  }
}

} // namespace

namespace {

TEST(VortexCoreTest,
     OverflowingArithmeticContractCannotFallBackToTruthyOpcode) {
  TestHarness h;
  const auto operation = h.vm.mintOpcode(OpcodeKind::Add);
  const auto left      = h.arena.makeScalarCell(std::int64_t{1});
  const auto right     = h.arena.makeScalarCell(std::int64_t{2});
  const auto output    = h.arena.makeScalarCell(std::int64_t{123});
  h.core.bindInput(operation, left);
  h.core.bindInput(operation, right);
  h.core.bindOutput(operation, output);

  const auto clause =
      h.vm.mintOpcode(OpcodeKind::Mul, "overflowing precondition");
  const auto max =
      h.arena.makeScalarCell(std::numeric_limits<std::int64_t>::max());
  const auto two = h.arena.makeScalarCell(std::int64_t{2});
  h.core.bindInput(clause, max);
  h.core.bindInput(clause, two);
  h.core.attachPrecondition(operation, clause);
  ASSERT_TRUE(h.core.isTruthy(clause));

  const auto result = h.vm.step(h.vm.spawnCursor(operation));
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.contractViolation, ContractViolationKind::Precondition);
  EXPECT_EQ(result.failingClause, clause);
  EXPECT_EQ(h.core.render(output), CellValue{std::int64_t{123}});
}

static_assert(!std::is_copy_constructible_v<VortexVM>);
static_assert(!std::is_move_constructible_v<VortexVM>);

TEST(VortexCoreTest, EmptyWriteGuardRemovesOnlyItsOwner) {
  TestHarness h;
  const int extraOwner = 0;
  const auto data      = h.arena.makeCell("data");
  const auto op        = h.vm.mintOpcode(OpcodeKind::Nop, "#GUARDED");
  h.core.setWriteGuard(&extraOwner,
                       [data](const CellRef cell) { return cell == data; });
  EXPECT_FALSE(
      h.core.writeValue(data, 0, std::nullopt, std::string{"changed"}));
  h.core.setWriteGuard(&extraOwner, {});
  EXPECT_TRUE(h.core.writeValue(data, 0, std::nullopt, std::string{"changed"}));
  EXPECT_FALSE(h.core.writeValue(op, 0, std::nullopt, std::string{"changed"}));
}

TEST(VortexCoreTest, WriteGuardsAreOwnedByEachLiveVM) {
  ArenaManifold arena;
  VortexCore core{arena};

  const auto expectRefused = [&](const CellRef cell) {
    WriteFault fault = WriteFault::None;
    EXPECT_FALSE(
        core.writeValue(cell, 0, std::nullopt, std::string{"changed"}, &fault));
    EXPECT_EQ(fault, WriteFault::OpcodeKind);
  };
  const auto expectWritable = [&](const CellRef cell) {
    WriteFault fault = WriteFault::OpcodeKind;
    ASSERT_TRUE(
        core.writeValue(cell, 0, std::nullopt, std::string{"changed"}, &fault));
    EXPECT_EQ(fault, WriteFault::None);
    EXPECT_EQ(core.render(cell), CellValue{std::string{"changed"}});
  };
  const auto makeClone = [&](const CellRef master) {
    const auto clone = arena.makeCell("clone");
    EXPECT_TRUE(core.setLink(master, core.dims().clone, DimVector::POS, clone));
    return clone;
  };

  auto persistent = std::make_unique<VortexVM>(core);
  const auto persistentOp =
      persistent->mintOpcode(OpcodeKind::Add, "#PERSISTENT");
  const auto persistentClone = makeClone(persistentOp);

  auto transient         = std::make_unique<VortexVM>(core);
  const auto transientOp = transient->mintOpcode(OpcodeKind::Mul, "#TRANSIENT");
  const auto transientClone = makeClone(transientOp);
  ASSERT_EQ(transient->installWriteGuard(), transient.get());
  ASSERT_EQ(persistent->installWriteGuard(), persistent.get());
  ASSERT_EQ(persistent->installWriteGuard(), persistent.get());

  expectRefused(persistentOp);
  expectRefused(persistentClone);
  expectRefused(transientOp);
  expectRefused(transientClone);

  const auto ordinary = arena.makeCell("ordinary");
  expectWritable(ordinary);

  transient.reset();
  expectRefused(persistentOp);
  expectRefused(persistentClone);

  auto later            = std::make_unique<VortexVM>(core);
  const auto laterOp    = later->mintOpcode(OpcodeKind::Sub, "#LATER");
  const auto laterClone = makeClone(laterOp);
  expectRefused(persistentOp);
  expectRefused(laterOp);
  expectRefused(laterClone);

  // Destroying the older VM first removes only its entry; the newer VM keeps
  // protecting its own registered opcode and clone master.
  persistent.reset();
  expectWritable(persistentOp);
  expectWritable(persistentClone);
  expectRefused(laterOp);
  expectRefused(laterClone);

  later.reset();
  expectWritable(laterOp);
  expectWritable(laterClone);
}

TEST(VortexCoreTest, ByteAtAndStrSliceFollowOneBasedSemantics) {
  TestHarness h;
  // ByteAt: 1-based indexing
  {
    auto resFirst = executeNumeric(h, OpcodeKind::ByteAt,
                                   {std::string{"hello"}, std::int64_t{1}});
    ASSERT_TRUE(resFirst.result.success);
    EXPECT_EQ(h.core.render(resFirst.outputs[0]),
              CellValue{static_cast<std::int64_t>('h')});

    auto resLast = executeNumeric(h, OpcodeKind::ByteAt,
                                  {std::string{"hello"}, std::int64_t{-1}});
    ASSERT_TRUE(resLast.result.success);
    EXPECT_EQ(h.core.render(resLast.outputs[0]),
              CellValue{static_cast<std::int64_t>('o')});

    auto resSecondLast = executeNumeric(
        h, OpcodeKind::ByteAt, {std::string{"hello"}, std::int64_t{-2}});
    ASSERT_TRUE(resSecondLast.result.success);
    EXPECT_EQ(h.core.render(resSecondLast.outputs[0]),
              CellValue{static_cast<std::int64_t>('l')});

    auto resZero = executeNumeric(h, OpcodeKind::ByteAt,
                                  {std::string{"hello"}, std::int64_t{0}});
    EXPECT_FALSE(resZero.result.success);
    EXPECT_THAT(resZero.result.errorMessage,
                ::testing::HasSubstr("out of bounds"));

    auto resOob = executeNumeric(h, OpcodeKind::ByteAt,
                                 {std::string{"hello"}, std::int64_t{6}});
    EXPECT_FALSE(resOob.result.success);
    EXPECT_THAT(resOob.result.errorMessage,
                ::testing::HasSubstr("out of bounds"));
  }

  // StrSlice: 1-based closed interval [start, end]
  {
    auto resSlice = executeNumeric(
        h, OpcodeKind::StrSlice,
        {std::string{"hello"}, std::int64_t{1}, std::int64_t{3}});
    ASSERT_TRUE(resSlice.result.success);
    EXPECT_EQ(h.core.render(resSlice.outputs[0]),
              CellValue{std::string{"hel"}});

    auto resEmpty = executeNumeric(
        h, OpcodeKind::StrSlice,
        {std::string{"hello"}, std::int64_t{2}, std::int64_t{1}});
    ASSERT_TRUE(resEmpty.result.success);
    EXPECT_EQ(h.core.render(resEmpty.outputs[0]), CellValue{std::string{""}});

    auto resNeg = executeNumeric(
        h, OpcodeKind::StrSlice,
        {std::string{"hello"}, std::int64_t{2}, std::int64_t{-1}});
    ASSERT_TRUE(resNeg.result.success);
    EXPECT_EQ(h.core.render(resNeg.outputs[0]), CellValue{std::string{"ello"}});

    auto resZero = executeNumeric(
        h, OpcodeKind::StrSlice,
        {std::string{"hello"}, std::int64_t{0}, std::int64_t{3}});
    EXPECT_FALSE(resZero.result.success);
    EXPECT_THAT(resZero.result.errorMessage,
                ::testing::HasSubstr("out of bounds"));
  }
}

} // namespace

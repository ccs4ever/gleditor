/**
 * @file vpl_graph_emit_test.cpp
 * @brief Proves each VPL graph-emit helper on the real Vortex VM.
 *
 * The helpers were designed from a reading of the VM; these tests are the
 * first time their graphs run. Each test hand-drives a compile through a
 * LowerContext, ends the chain with Halt, runs it and reads cells back.
 */
#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <vector>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vpl/execution.hpp"
#include "common/xanadu/vpl/graph_emit.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vpl {
namespace {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;
using zigzag::vortex::VortexCore;
using zigzag::vortex::VortexVM;

constexpr std::size_t kCycleBudget = 200000;

class GraphEmitTest : public ::testing::Test {
protected:
  void SetUp() override {
    dim_   = core_.findOrMintDimension("d.emit-test");
    other_ = core_.findOrMintDimension("d.emit-other");
    // The first op of the chain, so the program has an entry to start at.
    entry_ = ctx_.emitOp(OpcodeKind::Nop, "#TEST_ENTRY");
  }

  CellRef constant(std::int64_t value) { return ctx_.emitConstant(value); }

  // A counter born as an integer by arithmetic, like the helpers' own.
  CellRef counter() {
    const auto cell = arena_.makeCell();
    const auto op   = ctx_.emitOp(OpcodeKind::Add, "#TEST_COUNTER");
    ctx_.emitInputConstant(op, std::int64_t{0});
    ctx_.emitInputConstant(op, std::int64_t{0});
    ctx_.emitOutput(op, cell);
    return cell;
  }

  void add(CellRef counter, CellRef amount) {
    const auto op = ctx_.emitOp(OpcodeKind::Add, "#TEST_ADD");
    ctx_.emitInput(op, counter);
    ctx_.emitInput(op, amount);
    ctx_.emitOutput(op, counter);
  }

  void bump(CellRef counter) { add(counter, constant(1)); }

  // A register that holds no cell at run time.
  CellRef absent() {
    const auto reg = arena_.makeCell();
    const auto op  = ctx_.emitOp(OpcodeKind::ClearCellRefs, "#TEST_ABSENT");
    ctx_.emitOutput(op, reg);
    return reg;
  }

  // A rank built by the host on `dim_`, head first.
  std::vector<CellRef> makeRank(const std::vector<std::int64_t> &values,
                                DimRef dimension) {
    std::vector<CellRef> cells;
    for (const auto value : values) {
      const auto cell = arena_.makeScalarCell(value);
      if (!cells.empty()) {
        zigzag::expectWritten(
            arena_.link(cells.back(), dimension, false, cell));
      }
      cells.push_back(cell);
    }
    return cells;
  }

  // The values along `dimension` from `head`, read by the host.
  std::vector<std::int64_t> readRank(CellRef head, DimRef dimension) {
    std::vector<std::int64_t> values;
    std::optional<CellRef> at = head;
    while (at) {
      values.push_back(arena_.asInt64(*at).value_or(-999));
      at = arena_.linked(*at, dimension, DimVector::POS);
    }
    return values;
  }

  void run() {
    const auto halt = ctx_.emitOp(OpcodeKind::Halt, "#TEST_HALT");
    (void)halt;
    const auto unplaced = unplacedBranchTarget(vm_, programOps());
    ASSERT_FALSE(unplaced.has_value()) << *unplaced;
    const auto cursor = vm_.spawnCursor(entry_, "emit-test");
    const auto result = vm_.run(cursor, kCycleBudget);
    ASSERT_TRUE(result.success) << result.errorMessage;
  }

  // Walks +d.spin from the entry: the ops of the assembled chain.
  std::vector<CellRef> programOps() {
    std::vector<CellRef> ops;
    std::optional<CellRef> at = entry_;
    while (at) {
      ops.push_back(*at);
      at = arena_.linked(*at, core_.dims().spin, DimVector::POS);
    }
    return ops;
  }

  std::int64_t valueOf(CellRef cell) {
    return arena_.asInt64(cell).value_or(-999);
  }

  struct Walked {
    std::vector<std::int64_t> values;
    CellRef hits;
    CellRef sum;
    CellRef out;
  };
  std::vector<Walked> walked_;

  zigzag::ArenaManifold arena_;
  VortexCore core_{arena_};
  VortexVM vm_{core_};
  VPLCompiler compiler_{core_, vm_};
  LowerContext ctx_{compiler_};
  DimRef dim_{};
  DimRef other_{};
  CellRef entry_{};
};

// --- labels and branches ----------------------------------------------------

TEST_F(GraphEmitTest, ForwardBranchSkipsTheInstructionsBetween) {
  const auto hits = counter();
  const auto skip = emit::mintLabel(ctx_, "#SKIP");
  emit::branchAlways(ctx_, skip, "#JUMP");
  bump(hits);
  bump(hits);
  emit::placeLabel(ctx_, skip);
  bump(hits);
  run();
  EXPECT_EQ(valueOf(hits), 1);
}

TEST_F(GraphEmitTest, BackwardBranchLoopsThroughAPlacedLabel) {
  const auto hits = counter();
  const auto top  = emit::mintLabel(ctx_, "#TOP");
  const auto end  = emit::mintLabel(ctx_, "#END");
  emit::placeLabel(ctx_, top);
  const auto more = arena_.makeCell();
  const auto test = ctx_.emitOp(OpcodeKind::Lt, "#TEST");
  ctx_.emitInput(test, hits);
  ctx_.emitInputConstant(test, std::int64_t{3});
  ctx_.emitOutput(test, more);
  emit::branchIfFalse(ctx_, more, end, "#EXIT");
  bump(hits);
  emit::branchAlways(ctx_, top, "#AGAIN");
  emit::placeLabel(ctx_, end);
  run();
  EXPECT_EQ(valueOf(hits), 3);
}

TEST_F(GraphEmitTest,
       BranchIfFalseTreatsAbsentAndFalseAsFalseAndTrueFallsThrough) {
  const auto hits    = counter();
  const auto missing = absent();
  const auto falsy   = constant(0);
  const auto truthy  = constant(1);
  const auto label1  = emit::mintLabel(ctx_, "#L1");
  const auto label2  = emit::mintLabel(ctx_, "#L2");
  const auto label3  = emit::mintLabel(ctx_, "#L3");
  emit::branchIfFalse(ctx_, missing, label1, "#B1");
  bump(hits); // skipped: absent jumps
  emit::placeLabel(ctx_, label1);
  emit::branchIfFalse(ctx_, falsy, label2, "#B2");
  bump(hits); // skipped: false jumps
  emit::placeLabel(ctx_, label2);
  emit::branchIfFalse(ctx_, truthy, label3, "#B3");
  bump(hits); // runs: true falls through
  emit::placeLabel(ctx_, label3);
  run();
  EXPECT_EQ(valueOf(hits), 1);
}

TEST_F(GraphEmitTest, PlacingALabelTwiceIsRefused) {
  const auto label = emit::mintLabel(ctx_, "#ONCE");
  emit::placeLabel(ctx_, label);
  EXPECT_THROW(emit::placeLabel(ctx_, label), std::invalid_argument);
}

TEST_F(GraphEmitTest, ALabelBranchedToButNeverPlacedIsRefusedAtAssembly) {
  const auto label = emit::mintLabel(ctx_, "#NEVER");
  emit::branchAlways(ctx_, label, "#JUMP");
  (void)ctx_.emitOp(OpcodeKind::Halt, "#HALT");
  const auto message = unplacedBranchTarget(vm_, programOps());
  ASSERT_TRUE(message.has_value());
  EXPECT_NE(message->find("never placed"), std::string::npos) << *message;
}

TEST_F(GraphEmitTest, ALoopBodyThatBranchesToAnUnplacedLabelIsRefused) {
  const auto count = constant(2);
  EXPECT_THROW(emit::countedLoop(ctx_, count,
                                 [&](CellRef) {
                                   const auto stray =
                                       emit::mintLabel(ctx_, "#STRAY");
                                   emit::branchAlways(ctx_, stray, "#OUT");
                                 }),
               std::invalid_argument);
}

// --- counted loops -----------------------------------------------------------

TEST_F(GraphEmitTest, CountedLoopRunsExactlyNTimes) {
  struct Observed {
    std::int64_t n;
    CellRef hits;
    CellRef index;
  };
  std::vector<Observed> observed;
  for (const auto n :
       {std::int64_t{0}, std::int64_t{1}, std::int64_t{5}, std::int64_t{-3}}) {
    const auto hits  = counter();
    const auto count = constant(n);
    const auto index =
        emit::countedLoop(ctx_, count, [&](CellRef) { bump(hits); });
    const auto kept = counter();
    add(kept, index);
    observed.push_back({n, hits, kept});
  }
  run();
  for (const auto &[n, hits, kept] : observed) {
    const auto expected = n < 0 ? 0 : n;
    EXPECT_EQ(valueOf(hits), expected) << "n=" << n;
    EXPECT_EQ(valueOf(kept), expected) << "index after the loop, n=" << n;
  }
}

TEST_F(GraphEmitTest, CountedLoopIndexCountsFromZeroInsideTheBody) {
  const auto sum   = counter();
  const auto count = constant(5);
  emit::countedLoop(ctx_, count, [&](CellRef index) { add(sum, index); });
  run();
  EXPECT_EQ(valueOf(sum), 0 + 1 + 2 + 3 + 4);
}

TEST_F(GraphEmitTest, NestedCountedLoopsRestartTheInnerIndex) {
  const auto hits  = counter();
  const auto outer = constant(3);
  const auto inner = constant(4);
  emit::countedLoop(ctx_, outer, [&](CellRef) {
    emit::countedLoop(ctx_, inner, [&](CellRef) { bump(hits); });
  });
  run();
  EXPECT_EQ(valueOf(hits), 12);
}

TEST_F(GraphEmitTest, ALoopBodyCanGuardItsTailWithALabelOfItsOwn) {
  const auto hits  = counter();
  const auto count = constant(6);
  emit::countedLoop(ctx_, count, [&](CellRef index) {
    // Only odd indices count: index % 2 via DivMod is overkill, so compare.
    const auto big  = arena_.makeCell();
    const auto test = ctx_.emitOp(OpcodeKind::Gt, "#BIG");
    ctx_.emitInput(test, index);
    ctx_.emitInputConstant(test, std::int64_t{2});
    ctx_.emitOutput(test, big);
    const auto skip = emit::mintLabel(ctx_, "#SKIP_SMALL");
    emit::branchIfFalse(ctx_, big, skip, "#GUARD");
    bump(hits);
    emit::placeLabel(ctx_, skip);
  });
  run();
  EXPECT_EQ(valueOf(hits), 3); // indices 3, 4, 5
}

// R2: a counter passing through values equal to live cell ids stays a number.
TEST_F(GraphEmitTest, CounterEqualToLiveCellIdsStaysAnInteger) {
  // Cells 1..N all exist, so every counter value below is a live cell id.
  const auto live  = static_cast<std::int64_t>(arena_.cellCount());
  const auto count = constant(live + 10);
  const auto hits  = counter();
  const auto head  = arena_.makeScalarCell(std::int64_t{-1});
  const auto tail =
      emit::extreme(ctx_, head, dim_, DimVector::POS, std::nullopt);
  const auto index = emit::countedLoop(ctx_, count, [&](CellRef i) {
    bump(hits);
    const auto fresh = emit::mintAfter(ctx_, tail, dim_, DimVector::POS);
    emit::copyValue(ctx_, i, fresh);
  });
  const auto kept  = counter();
  add(kept, index);
  run();
  EXPECT_EQ(valueOf(hits), live + 10);
  EXPECT_EQ(valueOf(kept), live + 10);
  const auto values = readRank(head, dim_);
  ASSERT_EQ(values.size(), static_cast<std::size_t>(live + 11));
  for (std::int64_t i = 0; i <= live + 9; ++i) {
    EXPECT_EQ(values[static_cast<std::size_t>(i) + 1], i) << "i=" << i;
  }
}

// R3: arithmetic over a reference register reads the cell's value, never its
// id.
TEST_F(GraphEmitTest, ArithmeticOnAReferenceRegisterReadsTheCellValueNotItsId) {
  const auto cell   = arena_.makeScalarCell(std::int64_t{7});
  const auto result = counter();
  const auto holder = emit::aliasRegister(ctx_, cell);
  add(result, holder);
  run();
  ASSERT_NE(static_cast<std::int64_t>(cell), 7);
  EXPECT_EQ(valueOf(result), 7);
}

// --- rank walks
// ----------------------------------------------------------------

TEST_F(GraphEmitTest, WalkRankVisitsTheAnchorAndEverySuccessorInOrder) {
  for (const std::size_t length : {std::size_t{1}, std::size_t{4}}) {
    std::vector<std::int64_t> values;
    for (std::size_t i = 0; i < length; ++i)
      values.push_back(static_cast<std::int64_t>(10 + i));
    const auto cells = makeRank(values, dim_);

    const auto hits = counter();
    const auto sum  = counter();
    const auto out  = arena_.makeScalarCell(std::int64_t{-1});
    const auto tail =
        emit::extreme(ctx_, out, other_, DimVector::POS, std::nullopt);
    emit::walkRank(
        ctx_, cells.front(), dim_, DimVector::POS,
        [&](CellRef item) {
          bump(hits);
          add(sum, item);
          const auto fresh =
              emit::mintAfter(ctx_, tail, other_, DimVector::POS);
          emit::copyValue(ctx_, item, fresh);
        },
        std::nullopt);
    walked_.push_back({values, hits, sum, out});
  }
  run();
  for (const auto &w : walked_) {
    std::int64_t total = 0;
    for (const auto v : w.values) total += v;
    EXPECT_EQ(valueOf(w.hits), static_cast<std::int64_t>(w.values.size()));
    EXPECT_EQ(valueOf(w.sum), total);
    auto expected = std::vector<std::int64_t>{-1};
    expected.insert(expected.end(), w.values.begin(), w.values.end());
    EXPECT_EQ(readRank(w.out, other_), expected);
  }
}

TEST_F(GraphEmitTest, WalkRankOverAnAbsentAnchorRunsTheBodyZeroTimes) {
  const auto hits = counter();
  const auto gone = absent();
  emit::walkRank(
      ctx_, gone, dim_, DimVector::POS, [&](CellRef) { bump(hits); },
      std::nullopt);
  emit::walkRankAfter(
      ctx_, gone, dim_, DimVector::POS, [&](CellRef) { bump(hits); },
      std::nullopt);
  run();
  EXPECT_EQ(valueOf(hits), 0);
}

TEST_F(GraphEmitTest, WalkRankAfterSkipsTheAnchor) {
  const auto cells = makeRank({1, 2, 3, 4}, dim_);
  const auto sum   = counter();
  const auto hits  = counter();
  emit::walkRankAfter(
      ctx_, cells.front(), dim_, DimVector::POS,
      [&](CellRef item) {
        bump(hits);
        add(sum, item);
      },
      std::nullopt);
  const auto lone     = makeRank({9}, dim_);
  const auto loneHits = counter();
  emit::walkRankAfter(
      ctx_, lone.front(), dim_, DimVector::POS,
      [&](CellRef) { bump(loneHits); }, std::nullopt);
  run();
  EXPECT_EQ(valueOf(hits), 3);
  EXPECT_EQ(valueOf(sum), 2 + 3 + 4);
  EXPECT_EQ(valueOf(loneHits), 0);
}

TEST_F(GraphEmitTest, WalkRankNegwardFollowsTheOtherDirection) {
  const auto cells = makeRank({1, 2, 3}, dim_);
  const auto sum   = counter();
  emit::walkRank(
      ctx_, cells.back(), dim_, DimVector::NEG,
      [&](CellRef item) { add(sum, item); }, std::nullopt);
  run();
  EXPECT_EQ(valueOf(sum), 6);
}

TEST_F(GraphEmitTest, NestedWalksVisitEveryPair) {
  const auto a    = makeRank({1, 2, 3}, dim_);
  const auto b    = makeRank({10, 20, 30, 40}, other_);
  const auto hits = counter();
  const auto sum  = counter();
  emit::walkRank(
      ctx_, a.front(), dim_, DimVector::POS,
      [&](CellRef x) {
        emit::walkRank(
            ctx_, b.front(), other_, DimVector::POS,
            [&](CellRef y) {
              bump(hits);
              add(sum, x);
              add(sum, y);
            },
            std::nullopt);
      },
      std::nullopt);
  run();
  EXPECT_EQ(valueOf(hits), 12);
  EXPECT_EQ(valueOf(sum), (1 + 2 + 3) * 4 + (10 + 20 + 30 + 40) * 3);
}

TEST_F(GraphEmitTest, IteratorLoopConsumesAnIteratorTheCallerBegan) {
  const auto cells = makeRank({5, 6, 7}, dim_);
  const auto sum   = counter();
  const auto begin = ctx_.emitOp(OpcodeKind::RankBegin, "#BEGIN");
  ctx_.emitInput(begin, cells.front());
  ctx_.emitInputConstant(begin, static_cast<std::int64_t>(dim_));
  ctx_.emitInputConstant(begin, std::int64_t{1});
  const auto iterator = arena_.makeCell();
  ctx_.emitOutput(begin, iterator);
  emit::iteratorLoop(ctx_, iterator, [&](CellRef item) { add(sum, item); });
  run();
  EXPECT_EQ(valueOf(sum), 6 + 7); // RankBegin alone skips the anchor
}

TEST_F(GraphEmitTest, LockstepStopsAtTheShorterRank) {
  struct Shape {
    std::size_t a;
    std::size_t b;
  };
  struct Watch {
    Shape shape;
    CellRef hits;
    CellRef sum;
  };
  std::vector<Watch> watches;
  for (const auto shape :
       {Shape{5, 3}, Shape{3, 5}, Shape{4, 4}, Shape{1, 1}}) {
    std::vector<std::int64_t> va(shape.a, 1);
    std::vector<std::int64_t> vb(shape.b, 100);
    const auto a    = makeRank(va, dim_);
    const auto b    = makeRank(vb, other_);
    const auto hits = counter();
    const auto sum  = counter();
    emit::walkRanksLockstep(
        ctx_, a.front(), dim_, DimVector::POS, b.front(), other_,
        DimVector::POS,
        [&](CellRef x, CellRef y) {
          bump(hits);
          add(sum, x);
          add(sum, y);
        },
        std::nullopt, std::nullopt);
    watches.push_back({shape, hits, sum});
  }
  const auto gone   = absent();
  const auto full   = makeRank({1, 2}, dim_);
  const auto emptyA = counter();
  const auto emptyB = counter();
  emit::walkRanksLockstep(
      ctx_, gone, dim_, DimVector::POS, full.front(), dim_, DimVector::POS,
      [&](CellRef, CellRef) { bump(emptyA); }, std::nullopt, std::nullopt);
  emit::walkRanksLockstep(
      ctx_, full.front(), dim_, DimVector::POS, gone, dim_, DimVector::POS,
      [&](CellRef, CellRef) { bump(emptyB); }, std::nullopt, std::nullopt);
  run();
  for (const auto &w : watches) {
    const auto n = static_cast<std::int64_t>(std::min(w.shape.a, w.shape.b));
    EXPECT_EQ(valueOf(w.hits), n) << w.shape.a << "," << w.shape.b;
    EXPECT_EQ(valueOf(w.sum), n * 101) << w.shape.a << "," << w.shape.b;
  }
  EXPECT_EQ(valueOf(emptyA), 0);
  EXPECT_EQ(valueOf(emptyB), 0);
}

// --- extreme, extent
// ---------------------------------------------------------------

TEST_F(GraphEmitTest,
       ExtremeIsTheLastCellAndAliasesTheAnchorWhenTheWalkIsEmpty) {
  const auto cells = makeRank({1, 2, 3}, dim_);
  const auto lone  = makeRank({9}, dim_);
  const auto gone  = absent();
  const auto last =
      emit::extreme(ctx_, cells.front(), dim_, DimVector::POS, std::nullopt);
  const auto first =
      emit::extreme(ctx_, cells.back(), dim_, DimVector::NEG, std::nullopt);
  const auto same =
      emit::extreme(ctx_, lone.front(), dim_, DimVector::POS, std::nullopt);
  const auto none =
      emit::extreme(ctx_, gone, dim_, DimVector::POS, std::nullopt);
  run();
  EXPECT_EQ(vm_.resultCell(last), cells.back());
  EXPECT_EQ(vm_.resultCell(first), cells.front());
  EXPECT_EQ(vm_.resultCell(same), lone.front());
  EXPECT_FALSE(vm_.resultCell(none).has_value());
}

TEST_F(GraphEmitTest, ExtentCountsTheAnchorAndIsZeroForAnAbsentAnchor) {
  const auto cells = makeRank({1, 2, 3, 4}, dim_);
  const auto lone  = makeRank({9}, dim_);
  const auto gone  = absent();
  const auto four =
      emit::extentOf(ctx_, cells.front(), dim_, DimVector::POS, std::nullopt);
  const auto three =
      emit::extentOf(ctx_, cells[1], dim_, DimVector::POS, std::nullopt);
  const auto back =
      emit::extentOf(ctx_, cells.back(), dim_, DimVector::NEG, std::nullopt);
  const auto one =
      emit::extentOf(ctx_, lone.front(), dim_, DimVector::POS, std::nullopt);
  const auto zero =
      emit::extentOf(ctx_, gone, dim_, DimVector::POS, std::nullopt);
  run();
  EXPECT_EQ(valueOf(four), 4);
  EXPECT_EQ(valueOf(three), 3);
  EXPECT_EQ(valueOf(back), 4);
  EXPECT_EQ(valueOf(one), 1);
  EXPECT_EQ(valueOf(zero), 0);
}

// --- minting
// ------------------------------------------------------------------------

TEST_F(GraphEmitTest, MintAfterBuildsARankWhoseLinksAreReciprocal) {
  const auto head = arena_.makeScalarCell(std::int64_t{-1});
  const auto tail =
      emit::extreme(ctx_, head, dim_, DimVector::POS, std::nullopt);
  const auto n = constant(5);
  emit::countedLoop(ctx_, n, [&](CellRef index) {
    const auto fresh = emit::mintAfter(ctx_, tail, dim_, DimVector::POS);
    emit::copyValue(ctx_, index, fresh);
  });
  const auto length =
      emit::extentOf(ctx_, head, dim_, DimVector::POS, std::nullopt);
  const auto none  = arena_.makeScalarCell(std::int64_t{-1});
  const auto zeroN = constant(0);
  const auto noneTail =
      emit::extreme(ctx_, none, dim_, DimVector::POS, std::nullopt);
  emit::countedLoop(ctx_, zeroN, [&](CellRef) {
    (void)emit::mintAfter(ctx_, noneTail, dim_, DimVector::POS);
  });
  run();
  EXPECT_EQ(valueOf(length), 6);
  EXPECT_EQ(readRank(head, dim_),
            (std::vector<std::int64_t>{-1, 0, 1, 2, 3, 4}));
  EXPECT_EQ(readRank(none, dim_), (std::vector<std::int64_t>{-1}));
  std::optional<CellRef> at = head;
  while (const auto next = arena_.linked(*at, dim_, DimVector::POS)) {
    EXPECT_EQ(arena_.linked(*next, dim_, DimVector::NEG), at);
    at = next;
  }
  EXPECT_EQ(vm_.resultCell(tail), at)
      << "tail advances to the last minted cell";
}

TEST_F(GraphEmitTest, MintAfterNegwardGrowsTheRankTheOtherWay) {
  const auto head = arena_.makeScalarCell(std::int64_t{-1});
  const auto tail = emit::aliasRegister(ctx_, head);
  const auto n    = constant(3);
  emit::countedLoop(ctx_, n, [&](CellRef index) {
    const auto fresh = emit::mintAfter(ctx_, tail, dim_, DimVector::NEG);
    emit::copyValue(ctx_, index, fresh);
  });
  run();
  std::vector<std::int64_t> walked;
  std::optional<CellRef> at = head;
  while (at) {
    walked.push_back(valueOf(*at));
    at = arena_.linked(*at, dim_, DimVector::NEG);
  }
  EXPECT_EQ(walked, (std::vector<std::int64_t>{-1, 0, 1, 2}));
}

TEST_F(GraphEmitTest, CopyValueKeepsIntegerDoubleAndTextTypes) {
  const auto source = arena_.makeCell();
  const auto text   = arena_.makeCell("label");
  const auto number = arena_.makeScalarCell(std::int64_t{42});
  const auto real   = arena_.makeScalarCell(3.5);
  const auto head   = arena_.makeScalarCell(std::int64_t{0});
  (void)source;
  const auto tail =
      emit::extreme(ctx_, head, dim_, DimVector::POS, std::nullopt);
  const auto a = emit::mintAfter(ctx_, tail, dim_, DimVector::POS);
  emit::copyValue(ctx_, number, a);
  const auto b = emit::mintAfter(ctx_, tail, dim_, DimVector::POS);
  emit::copyValue(ctx_, real, b);
  const auto c = emit::mintAfter(ctx_, tail, dim_, DimVector::POS);
  emit::copyValue(ctx_, text, c);
  run();
  const auto cells = [&] {
    std::vector<CellRef> out;
    std::optional<CellRef> at = head;
    while (at) {
      out.push_back(*at);
      at = arena_.linked(*at, dim_, DimVector::POS);
    }
    return out;
  }();
  ASSERT_EQ(cells.size(), 4U);
  EXPECT_EQ(arena_.asInt64(cells[1]), 42);
  EXPECT_EQ(arena_.asDouble(cells[2]), 3.5);
  EXPECT_EQ(arena_.textOf(cells[3]), "label");
  // The sources are untouched and no cell was created by the copies.
  EXPECT_EQ(arena_.asInt64(number), 42);
  EXPECT_EQ(arena_.textOf(text), "label");
}

TEST_F(GraphEmitTest, CopyValueIntoAnAbsentTargetNeverCreatesACell) {
  const auto number = arena_.makeScalarCell(std::int64_t{42});
  const auto gone   = absent();
  emit::copyValue(ctx_, number, gone);
  (void)ctx_.emitOp(OpcodeKind::Halt, "#HALT");
  const auto cursor = vm_.spawnCursor(entry_, "absent-target");
  const auto before = arena_.cellCount();
  const auto result = vm_.run(cursor, kCycleBudget);
  // The helper emits no guard and the Value op is a silent no-op on an absent
  // target: the run succeeds, mints no cell and writes nothing.
  EXPECT_TRUE(result.success) << result.errorMessage;
  EXPECT_EQ(arena_.cellCount(), before);
  EXPECT_EQ(arena_.asInt64(number), 42);
  EXPECT_FALSE(vm_.resultCell(gone).has_value());
}

TEST_F(GraphEmitTest, AliasRegisterIsIndependentOfItsSourceAfterwards) {
  const auto one    = arena_.makeScalarCell(std::int64_t{1});
  const auto two    = arena_.makeScalarCell(std::int64_t{2});
  const auto reg    = emit::aliasRegister(ctx_, one);
  const auto alias  = emit::aliasRegister(ctx_, reg);
  const auto rebind = ctx_.emitOp(OpcodeKind::CopyCellRefs, "#REBIND");
  ctx_.emitInput(rebind, two);
  ctx_.emitOutput(rebind, reg);
  run();
  EXPECT_EQ(vm_.resultCell(reg), two);
  EXPECT_EQ(vm_.resultCell(alias), one);
}

// --- compile-time bookkeeping
// -------------------------------------------------------

TEST_F(GraphEmitTest, MergeRankInputDependenciesMergesAndNeverReplaces) {
  const auto a                    = arena_.makeCell();
  const auto b                    = arena_.makeCell();
  const auto result               = arena_.makeCell();
  const auto x                    = arena_.makeCell();
  const auto y                    = arena_.makeCell();
  const auto z                    = arena_.makeCell();
  ctx_.rankDependencies()[a]      = {x, y};
  ctx_.rankDependencies()[b]      = {y, z};
  ctx_.rankDependencies()[result] = {a};
  const std::vector<CellRef> inputs{a, b};
  emit::mergeRankInputDependencies(ctx_, result, inputs);
  EXPECT_EQ(ctx_.rankDependencies()[result],
            (std::vector<CellRef>{a, x, y, z}));
  emit::mergeRankInputDependencies(ctx_, result, inputs);
  EXPECT_EQ(ctx_.rankDependencies()[result],
            (std::vector<CellRef>{a, x, y, z}));
}

TEST_F(GraphEmitTest, ALoopBodyCannotBranchToALabelOutsideItsLoop) {
  const auto outside = emit::mintLabel(ctx_, "#OUTSIDE");
  const auto count   = constant(2);
  EXPECT_THROW(emit::countedLoop(
                   ctx_, count,
                   [&](CellRef) { emit::branchAlways(ctx_, outside, "#OUT"); }),
               std::invalid_argument);
  emit::placeLabel(ctx_, outside);
}

TEST_F(GraphEmitTest, AnInnerLoopCannotBranchToALabelOfTheOuterBody) {
  const auto count = constant(2);
  EXPECT_THROW(emit::countedLoop(ctx_, count,
                                 [&](CellRef) {
                                   const auto mine =
                                       emit::mintLabel(ctx_, "#MINE");
                                   emit::countedLoop(ctx_, count, [&](CellRef) {
                                     emit::branchAlways(ctx_, mine, "#OUT");
                                   });
                                   emit::placeLabel(ctx_, mine);
                                 }),
               std::invalid_argument);
}

TEST_F(GraphEmitTest, JumpAndCallToALabelNeverPlacedAreRefusedAtAssembly) {
  for (const auto kind : {OpcodeKind::Jump, OpcodeKind::Call}) {
    const auto label = emit::mintLabel(ctx_, "#NEVER");
    const auto op    = ctx_.emitOp(kind, "#TRANSFER");
    ctx_.emitInput(op, label);
    (void)ctx_.emitOp(OpcodeKind::Halt, "#HALT");
    const auto message = unplacedBranchTarget(vm_, programOps());
    ASSERT_TRUE(message.has_value()) << static_cast<int>(kind);
    EXPECT_NE(message->find("never placed"), std::string::npos);
  }
}

TEST_F(GraphEmitTest, JumpToAPlacedLabelPassesTheAssemblyCheck) {
  const auto label = emit::mintLabel(ctx_, "#THERE");
  const auto op    = ctx_.emitOp(OpcodeKind::Jump, "#TRANSFER");
  ctx_.emitInput(op, label);
  emit::placeLabel(ctx_, label);
  (void)ctx_.emitOp(OpcodeKind::Halt, "#HALT");
  EXPECT_FALSE(unplacedBranchTarget(vm_, programOps()).has_value());
}

// --- Executor: results the lowering cards hand back -------------------------

class ExecutorResultTest : public GraphEmitTest {
protected:
  // Ends the chain, spawns the cursor and describes the program as a compiler
  // would: one result slot, the facts the test sets, the assembled chain.
  CompilationResult describe(CellRef slot) {
    const auto halt = ctx_.emitOp(OpcodeKind::Halt, "#TEST_HALT");
    CompilationResult compiled;
    compiled.generatedOpcodes = programOps();
    compiled.entryOpcode      = entry_;
    compiled.terminalOpcode   = halt;
    compiled.cursor           = vm_.spawnCursor(entry_, "executor-test");
    compiled.resultSlots      = {slot};
    return compiled;
  }

  std::vector<zigzag::DirectedDim> axes() const {
    return {{dim_, DimVector::POS}};
  }

  Executor executor_{core_, vm_};
};

TEST_F(ExecutorResultTest, ARankResultCarriesItsExtentsAndFirstValueAsPayload) {
  const auto cells    = makeRank({5, 6, 7}, dim_);
  const auto slot     = emit::aliasRegister(ctx_, cells.front());
  auto compiled       = describe(slot);
  compiled.resultAxes = axes();
  const auto result   = executor_.run(std::move(compiled));
  ASSERT_TRUE(result.has_value()) << result.error().message;
  EXPECT_FALSE(result->view.isScalar());
  EXPECT_EQ(result->view.valence(), 1U);
  EXPECT_EQ(result->view.scalarInt(), 5);
  EXPECT_EQ(result->view.extents(), (std::vector<std::size_t>{3}));
  EXPECT_EQ(result->view.origin(), cells.front());
}

TEST_F(ExecutorResultTest,
       AClearedRegisterWithAxesIsAnEmptyArrayWithoutAnOrigin) {
  const auto slot     = absent();
  auto compiled       = describe(slot);
  compiled.resultAxes = axes();
  const auto result   = executor_.run(std::move(compiled));
  ASSERT_TRUE(result.has_value()) << result.error().message;
  EXPECT_FALSE(result->view.isScalar());
  EXPECT_EQ(result->view.valence(), 1U);
  EXPECT_FALSE(result->view.origin().has_value());
  EXPECT_EQ(result->view.shape(arena_), (std::vector<std::size_t>{0}));
  EXPECT_TRUE(result->view.collectCells(arena_).empty());
}

TEST_F(ExecutorResultTest, AClearedRegisterWithoutAxesIsStillAFailure) {
  const auto slot   = absent();
  const auto result = executor_.run(describe(slot));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().stage, ExecutionError::Stage::Runtime);
}

TEST_F(ExecutorResultTest,
       AnEnclosedResultIsABoxWhoseContentHangsOffTheEnclosureDimension) {
  const auto content   = makeRank({1, 2, 3}, dim_);
  const auto box       = arena_.makeCell("box");
  const auto enclosure = core_.findOrMintDimension(enclosureDimensionName);
  zigzag::expectWritten(arena_.link(box, enclosure, false, content.front()));
  const auto slot         = emit::aliasRegister(ctx_, box);
  auto compiled           = describe(slot);
  compiled.resultAxes     = axes();
  compiled.resultEnclosed = true;
  const auto result       = executor_.run(std::move(compiled));
  ASSERT_TRUE(result.has_value()) << result.error().message;
  const auto &view = result->view;
  EXPECT_TRUE(view.isEnclosed());
  ASSERT_NE(view.enclosedView(), nullptr);
  EXPECT_FALSE(view.enclosedView()->isEnclosed());
  EXPECT_EQ(view.enclosedView()->origin(), content.front());
  EXPECT_EQ(view.enclosedView()->shape(arena_), (std::vector<std::size_t>{3}));
}

TEST_F(ExecutorResultTest, ADimensionResultHasAxesAndNoCells) {
  const auto slot                = absent();
  auto compiled                  = describe(slot);
  compiled.resultDimensionValues = std::vector<zigzag::DirectedDim>{
      {dim_, DimVector::POS}, {other_, DimVector::NEG}};
  const auto result = executor_.run(std::move(compiled));
  ASSERT_TRUE(result.has_value()) << result.error().message;
  EXPECT_FALSE(result->view.origin().has_value());
  EXPECT_EQ(result->view.valence(), 2U);
  EXPECT_EQ(result->view.axes()[1].dim, other_);
  EXPECT_EQ(result->view.axes()[1].dir, DimVector::NEG);
}

TEST_F(ExecutorResultTest,
       AssignedBindingsKeepEnclosureEmptinessAndDimensions) {
  const auto content = makeRank({1, 2}, dim_);
  const auto box     = arena_.makeCell("box");
  zigzag::expectWritten(
      arena_.link(box, core_.findOrMintDimension(enclosureDimensionName), false,
                  content.front()));
  const auto boxSlot             = emit::aliasRegister(ctx_, box);
  const auto emptySlot           = absent();
  const auto dimSlot             = absent();
  const auto result              = emit::aliasRegister(ctx_, content.front());
  auto compiled                  = describe(result);
  compiled.resultAxes            = axes();
  compiled.assignedBindings["B"] = {
      .cell = boxSlot, .axes = axes(), .enclosed = true};
  compiled.assignedBindings["E"] = {.cell = emptySlot, .axes = axes()};
  compiled.assignedBindings["D"] = {
      .cell = dimSlot,
      .dimensionValues =
          std::vector<zigzag::DirectedDim>{{other_, DimVector::POS}}};
  const auto run = executor_.run(std::move(compiled));
  ASSERT_TRUE(run.has_value()) << run.error().message;
  ASSERT_TRUE(executor_.getVariable("B").has_value());
  EXPECT_TRUE(executor_.getVariable("B")->isEnclosed());
  ASSERT_TRUE(executor_.getVariable("E").has_value());
  EXPECT_FALSE(executor_.getVariable("E")->origin().has_value());
  EXPECT_EQ(executor_.getVariable("E")->valence(), 1U);
  ASSERT_TRUE(executor_.getVariable("D").has_value());
  EXPECT_EQ(executor_.getVariable("D")->axes().front().dim, other_);
}

TEST_F(ExecutorResultTest,
       ResultCellsWithoutFactsOfTheirOwnTakeTheResultsOperands) {
  const auto cells            = makeRank({1, 2, 3}, dim_);
  const auto source           = arena_.makeCell();
  const auto own              = arena_.makeCell();
  const auto slot             = emit::aliasRegister(ctx_, cells.front());
  auto compiled               = describe(slot);
  compiled.resultAxes         = axes();
  compiled.resultDependencies = {source};
  compiled.cellDependencies[cells[1]] = {own};
  const auto result                   = executor_.run(std::move(compiled));
  ASSERT_TRUE(result.has_value()) << result.error().message;
  ASSERT_EQ(executor_.dependenciesOf(cells[0]).size(), 1U);
  EXPECT_EQ(executor_.dependenciesOf(cells[0])[0], source);
  ASSERT_EQ(executor_.dependenciesOf(cells[1]).size(), 1U);
  EXPECT_EQ(executor_.dependenciesOf(cells[1])[0], own)
      << "a compile-time fact is never overwritten";
  EXPECT_EQ(executor_.dependenciesOf(cells[2])[0], source);
}

TEST_F(ExecutorResultTest,
       PrintMarkerReachesTheHostAndABranchToNowhereIsRefused) {
  const auto cells     = makeRank({1}, dim_);
  const auto slot      = emit::aliasRegister(ctx_, cells.front());
  auto compiled        = describe(slot);
  compiled.resultAxes  = axes();
  compiled.resultPrint = true;
  const auto printed   = executor_.run(std::move(compiled));
  ASSERT_TRUE(printed.has_value()) << printed.error().message;
  EXPECT_TRUE(printed->print);
}

TEST_F(ExecutorResultTest,
       ABranchToALabelNeverPlacedStopsTheProgramBeforeItRuns) {
  const auto cells = makeRank({1}, dim_);
  const auto label = emit::mintLabel(ctx_, "#NEVER");
  emit::branchAlways(ctx_, label, "#JUMP");
  const auto slot     = emit::aliasRegister(ctx_, cells.front());
  const auto compiled = describe(slot);
  const auto refused  = executor_.run(compiled);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().stage, ExecutionError::Stage::Compile);
  EXPECT_NE(refused.error().message.find("never placed"), std::string::npos);
}

} // namespace
} // namespace xanadu::vpl

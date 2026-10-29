#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

namespace {

using namespace zigzag;
using namespace zigzag::vortex;

struct Harness {
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
};

void bindSequence(Harness &h, std::span<const CellRef> opcodes) {
  for (std::size_t i = 1; i < opcodes.size(); ++i) {
    ASSERT_TRUE(h.arena.link(opcodes[i - 1], h.core.dims().spin, DimVector::POS,
                             opcodes[i]));
  }
}

TEST(VortexStreamTest, StreamAppendAccumulatesAndSequenceIterationResumes) {
  Harness h;
  const CellRef first    = h.arena.makeCell("first");
  const CellRef second   = h.arena.makeCell("second");
  const CellRef stream   = h.arena.makeCell();
  const CellRef iterator = h.arena.makeCell();
  const CellRef item     = h.arena.makeCell();
  const CellRef present  = h.arena.makeCell();

  const CellRef clear        = h.vm.mintOpcode(OpcodeKind::StreamClear);
  const CellRef appendFirst  = h.vm.mintOpcode(OpcodeKind::StreamAppend);
  const CellRef appendSecond = h.vm.mintOpcode(OpcodeKind::StreamAppend);
  const CellRef begin        = h.vm.mintOpcode(OpcodeKind::StreamBegin);
  const CellRef nextFirst    = h.vm.mintOpcode(OpcodeKind::IteratorNext);
  const CellRef nextSecond   = h.vm.mintOpcode(OpcodeKind::IteratorNext);
  const CellRef halt         = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef code[]{clear,     appendFirst, appendSecond, begin,
                       nextFirst, nextSecond,  halt};
  bindSequence(h, code);

  h.core.bindOutput(clear, stream);
  h.core.bindInput(appendFirst, first);
  h.core.bindOutput(appendFirst, stream);
  h.core.bindInput(appendSecond, second);
  h.core.bindOutput(appendSecond, stream);
  h.core.bindInput(begin, stream);
  h.core.bindOutput(begin, iterator);
  h.core.bindInput(nextFirst, iterator);
  h.core.bindOutput(nextFirst, item);
  h.core.bindOutput(nextFirst, present);
  h.core.bindInput(nextSecond, iterator);
  h.core.bindOutput(nextSecond, item);
  h.core.bindOutput(nextSecond, present);

  const CellRef cursor = h.vm.spawnCursor(clear, "stream-sequence");
  const auto partial   = h.vm.run(cursor, 4);
  EXPECT_FALSE(partial.success);
  const auto run = h.vm.run(cursor, 2);
  EXPECT_FALSE(run.success);
  EXPECT_EQ(h.vm.resultCells(stream), (std::vector<CellRef>{first, second}));
  EXPECT_EQ(h.vm.cellReferenceOf(item), second);
  EXPECT_EQ(h.arena.asBool(present), std::optional{true});
}

TEST(VortexStreamTest, ReverseSequenceSnapshotsChunksAndRestoresPosition) {
  Harness h;
  const auto a          = h.arena.makeCell("a");
  const auto b          = h.arena.makeCell("b");
  const auto c          = h.arena.makeCell("c");
  const auto d          = h.arena.makeCell("d");
  const auto empty      = h.arena.makeCell();
  const auto stream     = h.arena.makeCell();
  const auto iterator   = h.arena.makeCell();
  const auto item       = h.arena.makeCell();
  const auto present    = h.arena.makeCell();
  const auto clearEmpty = h.vm.mintOpcode(OpcodeKind::StreamClear);
  const auto clear      = h.vm.mintOpcode(OpcodeKind::StreamClear);
  const auto append     = h.vm.mintOpcode(OpcodeKind::StreamAppend);
  const auto begin      = h.vm.mintOpcode(OpcodeKind::StreamReverseBegin);
  const auto change     = h.vm.mintOpcode(OpcodeKind::StreamAppend);
  const auto next       = h.vm.mintOpcode(OpcodeKind::IteratorNext);
  const auto halt       = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef code[]{clearEmpty, clear, append, begin, change, next, halt};
  bindSequence(h, code);
  h.core.bindOutput(clearEmpty, empty);
  h.core.bindOutput(clear, stream);
  h.core.bindInput(append, b);
  h.core.bindInput(append, c);
  h.core.bindOutput(append, stream);
  h.core.bindInput(begin, a);
  h.core.bindInput(begin, empty);
  h.core.bindInput(begin, stream);
  h.core.bindOutput(begin, iterator);
  h.core.bindInput(change, d);
  h.core.bindOutput(change, stream);
  h.core.bindInput(next, iterator);
  h.core.bindOutput(next, item);
  h.core.bindOutput(next, present);
  const auto cursor = h.vm.spawnCursor(clearEmpty, "reverse-sequence");
  EXPECT_FALSE(h.vm.run(cursor, 6).success);
  EXPECT_EQ(h.vm.resultCell(item), c);
  h.vm.pushChoicePoint(cursor, next);
  h.vm.setCursorOpcode(cursor, next);
  EXPECT_FALSE(h.vm.run(cursor, 1).success);
  EXPECT_EQ(h.vm.resultCell(item), b);
  ASSERT_TRUE(h.vm.backtrack(cursor));
  EXPECT_FALSE(h.vm.run(cursor, 1).success);
  EXPECT_EQ(h.vm.resultCell(item), b);
  h.vm.setCursorOpcode(cursor, next);
  EXPECT_FALSE(h.vm.run(cursor, 1).success);
  EXPECT_EQ(h.vm.resultCell(item), a);
  h.vm.setCursorOpcode(cursor, next);
  EXPECT_FALSE(h.vm.run(cursor, 1).success);
  EXPECT_FALSE(h.vm.resultCell(item));
  EXPECT_EQ(h.arena.asBool(present), false);
}

TEST(VortexStreamTest, EmptyStreamProducesNoSequenceItems) {
  Harness h;
  const CellRef stream   = h.arena.makeCell();
  const CellRef iterator = h.arena.makeCell();
  const CellRef item     = h.arena.makeCell();
  const CellRef present  = h.arena.makeCell();
  const CellRef clear    = h.vm.mintOpcode(OpcodeKind::StreamClear);
  const CellRef begin    = h.vm.mintOpcode(OpcodeKind::StreamBegin);
  const CellRef next     = h.vm.mintOpcode(OpcodeKind::IteratorNext);
  const CellRef halt     = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef code[]{clear, begin, next, halt};
  bindSequence(h, code);
  h.core.bindOutput(clear, stream);
  h.core.bindInput(begin, stream);
  h.core.bindOutput(begin, iterator);
  h.core.bindInput(next, iterator);
  h.core.bindOutput(next, item);
  h.core.bindOutput(next, present);

  const CellRef cursor = h.vm.spawnCursor(clear, "empty-stream");
  const auto run       = h.vm.run(cursor, 10);
  ASSERT_TRUE(run.success) << run.errorMessage;
  EXPECT_TRUE(h.vm.resultCells(stream).empty());
  EXPECT_EQ(h.arena.asBool(present), std::optional{false});
}

TEST(VortexStreamTest, ResultCellVisitorMatchesScalarAndTypedRegisters) {
  Harness h;
  const CellRef first       = h.arena.makeCell("first");
  const CellRef second      = h.arena.makeCell("second");
  const CellRef emptyStream = h.arena.makeCell();
  const CellRef stream      = h.arena.makeCell();
  const CellRef iterator    = h.arena.makeCell();
  const CellRef item        = h.arena.makeCell();
  const CellRef present     = h.arena.makeCell();
  const CellRef nextStream  = h.arena.makeCell();

  const CellRef clearEmpty   = h.vm.mintOpcode(OpcodeKind::StreamClear);
  const CellRef clear        = h.vm.mintOpcode(OpcodeKind::StreamClear);
  const CellRef appendFirst  = h.vm.mintOpcode(OpcodeKind::StreamAppend);
  const CellRef appendSecond = h.vm.mintOpcode(OpcodeKind::StreamAppend);
  const CellRef begin        = h.vm.mintOpcode(OpcodeKind::StreamBegin);
  const CellRef next         = h.vm.mintOpcode(OpcodeKind::IteratorNext);
  const CellRef halt         = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef clearNext    = h.vm.mintOpcode(OpcodeKind::StreamClear);
  const CellRef haltNext     = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef code[]{clearEmpty, clear, appendFirst, appendSecond, begin,
                       next,       halt,  clearNext,   haltNext};
  bindSequence(h, code);
  h.core.bindOutput(clearEmpty, emptyStream);
  h.core.bindOutput(clear, stream);
  h.core.bindInput(appendFirst, first);
  h.core.bindOutput(appendFirst, stream);
  h.core.bindInput(appendSecond, second);
  h.core.bindOutput(appendSecond, stream);
  h.core.bindInput(begin, stream);
  h.core.bindOutput(begin, iterator);
  h.core.bindInput(next, iterator);
  h.core.bindOutput(next, item);
  h.core.bindOutput(next, present);
  h.core.bindOutput(clearNext, nextStream);

  const CellRef cursor = h.vm.spawnCursor(clearEmpty, "result-cell-visitor");
  EXPECT_FALSE(h.vm.run(cursor, 6).success);

  const auto visit = [&](const CellRef slot) {
    std::vector<CellRef> cells;
    h.vm.forEachResultCell(slot,
                           [&](const CellRef cell) { cells.push_back(cell); });
    return cells;
  };
  EXPECT_EQ(visit(first), (std::vector<CellRef>{first}));
  EXPECT_EQ(visit(emptyStream), std::vector<CellRef>{});
  EXPECT_EQ(visit(stream), (std::vector<CellRef>{first, second}));
  EXPECT_EQ(visit(iterator), std::vector<CellRef>{});
  EXPECT_EQ(visit(item), (std::vector<CellRef>{first}));
  EXPECT_EQ(visit(present), (std::vector<CellRef>{present}));

  ASSERT_TRUE(h.vm.run(cursor, 1).success);
  const CellRef nextCursor = h.vm.spawnCursor(clearNext, "result-cell-expired");
  EXPECT_FALSE(h.vm.run(nextCursor, 1).success);
  EXPECT_EQ(visit(stream), std::vector<CellRef>{});
  EXPECT_EQ(visit(iterator), std::vector<CellRef>{});
}

TEST(VortexStreamTest, ContractBudgetChargesBeforeCopyAndCanRecover) {
  Harness h;
  xanadu::MultiStoreCoordinator coordinator(h.core);
  h.vm.setContractCoordinator(coordinator);
  const CellRef first       = h.arena.makeCell("first");
  const CellRef second      = h.arena.makeCell("second");
  const CellRef firstTitle  = h.arena.makeCell("first title");
  const CellRef secondTitle = h.arena.makeCell("second title");
  const auto title          = coordinator.resolveDimension("d.title");
  zigzag::expectWritten(h.arena.link(first, title, DimVector::POS, firstTitle));
  zigzag::expectWritten(
      h.arena.link(second, title, DimVector::POS, secondTitle));
  const CellRef stream     = h.arena.makeCell();
  const CellRef descriptor = h.arena.makeCell(
      "VQL_CONTRACT/3\nStream<Cell { +d.title: $title: String }>");
  const CellRef matched      = h.arena.makeCell();
  const CellRef capture      = h.arena.makeCell();
  const CellRef clear        = h.vm.mintOpcode(OpcodeKind::StreamClear);
  const CellRef appendFirst  = h.vm.mintOpcode(OpcodeKind::StreamAppend);
  const CellRef appendSecond = h.vm.mintOpcode(OpcodeKind::StreamAppend);
  const CellRef contract     = h.vm.mintOpcode(OpcodeKind::MatchContract);
  const CellRef halt         = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef code[]{clear, appendFirst, appendSecond, contract, halt};
  bindSequence(h, code);
  h.core.bindOutput(clear, stream);
  h.core.bindInput(appendFirst, first);
  h.core.bindOutput(appendFirst, stream);
  h.core.bindInput(appendSecond, second);
  h.core.bindOutput(appendSecond, stream);
  h.core.bindInput(contract, descriptor);
  h.core.bindInput(contract, h.arena.makeScalarCell(std::int64_t{1}));
  h.core.bindInput(contract, stream);
  h.core.bindOutput(contract, matched);
  h.core.bindOutput(contract, capture);

  const CellRef cursor = h.vm.spawnCursor(clear, "contract-subject-budget");
  EXPECT_FALSE(h.vm.run(cursor, 3).success);
  EXPECT_EQ(h.vm.resultCells(stream), (std::vector<CellRef>{first, second}));

  constexpr std::size_t descriptorWork =
      std::string_view(
          "VQL_CONTRACT/3\nStream<Cell { +d.title: $title: String }>")
          .size();
  h.arena.setExecutionLimits(100'000, descriptorWork + 2, 100'000);
  const auto failed = h.vm.run(cursor, 1);
  EXPECT_FALSE(failed.success);
  EXPECT_NE(failed.errorMessage.find("work budget exhausted"),
            std::string::npos);
  EXPECT_FALSE(h.arena.asBool(matched));
  EXPECT_FALSE(h.vm.cellReferenceOf(capture));

  h.arena.setExecutionLimits(100'000, 100'000, 100'000);
  const auto recovered = h.vm.run(cursor, 2);
  ASSERT_TRUE(recovered.success) << recovered.errorMessage;
  EXPECT_EQ(h.arena.asBool(matched), std::optional{true});
  EXPECT_FALSE(h.vm.cellReferenceOf(capture));
  EXPECT_EQ(h.vm.resultCells(stream), (std::vector<CellRef>{first, second}));
}

TEST(VortexStreamTest, ContractRejectsMalformedSubjectCounts) {
  for (const auto count :
       {std::int64_t{-1}, std::int64_t{0}, std::int64_t{2}}) {
    Harness h;
    const auto descriptor = h.arena.makeCell("VQL_CONTRACT/3\nCell");
    const auto contract   = h.vm.mintOpcode(OpcodeKind::MatchContract);
    const auto halt       = h.vm.mintOpcode(OpcodeKind::Halt);
    const CellRef code[]{contract, halt};
    bindSequence(h, code);
    h.core.bindInput(contract, descriptor);
    h.core.bindInput(contract, h.arena.makeScalarCell(count));
    h.core.bindInput(contract, h.core.home());
    h.core.bindOutput(contract, h.arena.makeCell());
    const auto cursor = h.vm.spawnCursor(contract, "invalid-contract-layout");
    const auto execution = h.vm.run(cursor, 10);
    EXPECT_FALSE(execution.success) << count;
    EXPECT_NE(execution.errorMessage.find("contract"), std::string::npos);
  }
}

TEST(VortexStreamTest, ContractRejectsCaptureLayoutBeforeAllocatingDefaults) {
  Harness h;
  const auto descriptor =
      h.arena.makeCell("VQL_CONTRACT/3\nCell { +d.missing: $value: Int := 7 }");
  const auto contract = h.vm.mintOpcode(OpcodeKind::MatchContract);
  const auto halt     = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef code[]{contract, halt};
  bindSequence(h, code);
  h.core.bindInput(contract, descriptor);
  h.core.bindInput(contract, h.arena.makeScalarCell(std::int64_t{1}));
  h.core.bindInput(contract, h.core.home());
  h.core.bindOutput(contract, h.arena.makeCell());
  const auto cursor      = h.vm.spawnCursor(contract, "invalid-capture-layout");
  const auto cellsBefore = h.arena.cellCount();
  const auto execution   = h.vm.run(cursor, 10);
  EXPECT_FALSE(execution.success);
  EXPECT_NE(execution.errorMessage.find("capture slots"), std::string::npos);
  EXPECT_EQ(h.arena.cellCount(), cellsBefore);
}

TEST(VortexStreamTest, AppendCanSnapshotAndAppendItsOwnPrefix) {
  Harness h;
  const CellRef value       = h.arena.makeCell("value");
  const CellRef stream      = h.arena.makeCell();
  const CellRef clear       = h.vm.mintOpcode(OpcodeKind::StreamClear);
  const CellRef appendValue = h.vm.mintOpcode(OpcodeKind::StreamAppend);
  const CellRef appendSelf  = h.vm.mintOpcode(OpcodeKind::StreamAppend);
  const CellRef halt        = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef code[]{clear, appendValue, appendSelf, halt};
  bindSequence(h, code);
  h.core.bindOutput(clear, stream);
  h.core.bindInput(appendValue, value);
  h.core.bindOutput(appendValue, stream);
  h.core.bindInput(appendSelf, stream);
  h.core.bindOutput(appendSelf, stream);

  const CellRef cursor = h.vm.spawnCursor(clear, "stream-self-append");
  const auto run       = h.vm.run(cursor, 10);
  ASSERT_TRUE(run.success) << run.errorMessage;
  EXPECT_EQ(h.vm.resultCells(stream), (std::vector<CellRef>{value, value}));
}

TEST(VortexStreamTest, AppendUsesCopyOnWriteForSharedStreamRegisters) {
  Harness h;
  const CellRef first        = h.arena.makeCell("first");
  const CellRef second       = h.arena.makeCell("second");
  const CellRef original     = h.arena.makeCell();
  const CellRef alias        = h.arena.makeCell();
  const CellRef clear        = h.vm.mintOpcode(OpcodeKind::StreamClear);
  const CellRef appendFirst  = h.vm.mintOpcode(OpcodeKind::StreamAppend);
  const CellRef copy         = h.vm.mintOpcode(OpcodeKind::CopyCellRefs);
  const CellRef appendSecond = h.vm.mintOpcode(OpcodeKind::StreamAppend);
  const CellRef halt         = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef code[]{clear, appendFirst, copy, appendSecond, halt};
  bindSequence(h, code);
  h.core.bindOutput(clear, original);
  h.core.bindInput(appendFirst, first);
  h.core.bindOutput(appendFirst, original);
  h.core.bindInput(copy, original);
  h.core.bindOutput(copy, alias);
  h.core.bindInput(appendSecond, second);
  h.core.bindOutput(appendSecond, original);

  const CellRef cursor = h.vm.spawnCursor(clear, "stream-copy-on-write");
  const auto run       = h.vm.run(cursor, 10);
  ASSERT_TRUE(run.success) << run.errorMessage;
  EXPECT_EQ(h.vm.resultCells(original), (std::vector<CellRef>{first, second}));
  EXPECT_EQ(h.vm.resultCells(alias), (std::vector<CellRef>{first}));
}

TEST(VortexStreamTest, AppendChecksWholeInputBudgetBeforeChangingAccumulator) {
  Harness h;
  const CellRef kept             = h.arena.makeCell("kept");
  const CellRef first            = h.arena.makeCell("first");
  const CellRef second           = h.arena.makeCell("second");
  const CellRef accumulator      = h.arena.makeCell();
  const CellRef source           = h.arena.makeCell();
  const CellRef clearAccumulator = h.vm.mintOpcode(OpcodeKind::StreamClear);
  const CellRef appendKept       = h.vm.mintOpcode(OpcodeKind::StreamAppend);
  const CellRef clearSource      = h.vm.mintOpcode(OpcodeKind::StreamClear);
  const CellRef appendFirst      = h.vm.mintOpcode(OpcodeKind::StreamAppend);
  const CellRef appendSecond     = h.vm.mintOpcode(OpcodeKind::StreamAppend);
  const CellRef appendSource     = h.vm.mintOpcode(OpcodeKind::StreamAppend);
  const CellRef halt             = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef code[]{clearAccumulator, appendKept,   clearSource, appendFirst,
                       appendSecond,     appendSource, halt};
  bindSequence(h, code);
  h.core.bindOutput(clearAccumulator, accumulator);
  h.core.bindInput(appendKept, kept);
  h.core.bindOutput(appendKept, accumulator);
  h.core.bindOutput(clearSource, source);
  h.core.bindInput(appendFirst, first);
  h.core.bindOutput(appendFirst, source);
  h.core.bindInput(appendSecond, second);
  h.core.bindOutput(appendSecond, source);
  h.core.bindInput(appendSource, source);
  h.core.bindOutput(appendSource, accumulator);
  const CellRef cursor = h.vm.spawnCursor(clearAccumulator, "append-budget");

  const auto prepared = h.vm.run(cursor, 5);
  EXPECT_FALSE(prepared.success);
  ASSERT_EQ(h.vm.resultCells(accumulator), (std::vector<CellRef>{kept}));
  h.arena.setExecutionLimits(100'000, 1, 100'000);
  const auto failed = h.vm.run(cursor, 1);
  EXPECT_FALSE(failed.success);
  EXPECT_NE(failed.errorMessage.find("work budget exhausted"),
            std::string::npos);
  EXPECT_EQ(h.vm.resultCells(accumulator), (std::vector<CellRef>{kept}));
}

TEST(VortexStreamTest, CopyCellRefsSnapshotsAllSourcesBeforeWritingTargets) {
  Harness h;
  const CellRef firstValue   = h.arena.makeCell("first");
  const CellRef secondValue  = h.arena.makeCell("second");
  const CellRef firstStream  = h.arena.makeCell();
  const CellRef secondStream = h.arena.makeCell();
  const CellRef clearFirst   = h.vm.mintOpcode(OpcodeKind::StreamClear);
  const CellRef appendFirst  = h.vm.mintOpcode(OpcodeKind::StreamAppend);
  const CellRef clearSecond  = h.vm.mintOpcode(OpcodeKind::StreamClear);
  const CellRef appendSecond = h.vm.mintOpcode(OpcodeKind::StreamAppend);
  const CellRef swap         = h.vm.mintOpcode(OpcodeKind::CopyCellRefs);
  const CellRef halt         = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef code[]{clearFirst,   appendFirst, clearSecond,
                       appendSecond, swap,        halt};
  bindSequence(h, code);
  h.core.bindOutput(clearFirst, firstStream);
  h.core.bindInput(appendFirst, firstValue);
  h.core.bindOutput(appendFirst, firstStream);
  h.core.bindOutput(clearSecond, secondStream);
  h.core.bindInput(appendSecond, secondValue);
  h.core.bindOutput(appendSecond, secondStream);
  h.core.bindInput(swap, firstStream);
  h.core.bindInput(swap, secondStream);
  h.core.bindOutput(swap, secondStream);
  h.core.bindOutput(swap, firstStream);

  const CellRef cursor = h.vm.spawnCursor(clearFirst, "stream-copy-swap");
  const auto run       = h.vm.run(cursor, 10);
  ASSERT_TRUE(run.success) << run.errorMessage;
  EXPECT_EQ(h.vm.resultCells(firstStream), (std::vector<CellRef>{secondValue}));
  EXPECT_EQ(h.vm.resultCells(secondStream), (std::vector<CellRef>{firstValue}));
}

TEST(VortexStreamTest, IteratorNextRejectsOutputAliasingItsState) {
  Harness h;
  const CellRef anchor   = h.arena.makeCell("anchor");
  const CellRef iterator = h.arena.makeCell();
  const CellRef present  = h.arena.makeCell();
  const CellRef begin    = h.vm.mintOpcode(OpcodeKind::RankBegin);
  const CellRef next     = h.vm.mintOpcode(OpcodeKind::IteratorNext);
  const CellRef halt     = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef code[]{begin, next, halt};
  bindSequence(h, code);
  h.core.bindInput(begin, anchor);
  h.core.bindInput(begin, h.arena.makeScalarCell(
                              static_cast<std::int64_t>(h.core.dims().step)));
  h.core.bindInput(begin, h.arena.makeScalarCell(std::int64_t{1}));
  h.core.bindOutput(begin, iterator);
  h.core.bindInput(next, iterator);
  h.core.bindOutput(next, iterator);
  h.core.bindOutput(next, present);

  const CellRef cursor = h.vm.spawnCursor(begin, "iterator-output-alias");
  const auto run       = h.vm.run(cursor, 10);
  EXPECT_FALSE(run.success);
  EXPECT_NE(run.errorMessage.find("distinct iterator"), std::string::npos);
}

TEST(VortexStreamTest, RankIteratorYieldsForwardNeighborsThenStops) {
  Harness h;
  const CellRef a = h.arena.makeCell("a");
  const CellRef b = h.arena.makeCell("b");
  const CellRef c = h.arena.makeCell("c");
  ASSERT_TRUE(h.arena.link(a, h.core.dims().step, DimVector::POS, b));
  ASSERT_TRUE(h.arena.link(b, h.core.dims().step, DimVector::POS, c));
  ASSERT_TRUE(h.arena.link(c, h.core.dims().step, DimVector::POS, a));

  const CellRef iterator  = h.arena.makeCell();
  const CellRef item      = h.arena.makeCell();
  const CellRef present   = h.arena.makeCell();
  const CellRef begin     = h.vm.mintOpcode(OpcodeKind::RankBegin);
  const CellRef nextOne   = h.vm.mintOpcode(OpcodeKind::IteratorNext);
  const CellRef nextTwo   = h.vm.mintOpcode(OpcodeKind::IteratorNext);
  const CellRef nextThree = h.vm.mintOpcode(OpcodeKind::IteratorNext);
  const CellRef nextEnd   = h.vm.mintOpcode(OpcodeKind::IteratorNext);
  const CellRef halt      = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef code[]{begin, nextOne, nextTwo, nextThree, nextEnd, halt};
  bindSequence(h, code);
  h.core.bindInput(begin, a);
  h.core.bindInput(begin, h.arena.makeScalarCell(
                              static_cast<std::int64_t>(h.core.dims().step)));
  h.core.bindInput(begin, h.arena.makeScalarCell(std::int64_t{1}));
  h.core.bindOutput(begin, iterator);
  for (const CellRef next : {nextOne, nextTwo, nextThree, nextEnd}) {
    h.core.bindInput(next, iterator);
    h.core.bindOutput(next, item);
    h.core.bindOutput(next, present);
  }

  const CellRef cursor = h.vm.spawnCursor(begin, "rank-iterator");
  const auto run       = h.vm.run(cursor, 20);
  ASSERT_TRUE(run.success) << run.errorMessage;
  EXPECT_EQ(h.arena.asBool(present), std::optional{false});
}

TEST(VortexStreamTest, ChoicePointRestoresStreamAccumulatorSnapshot) {
  Harness h;
  const CellRef first       = h.arena.makeCell("first");
  const CellRef later       = h.arena.makeCell("later");
  const CellRef stream      = h.arena.makeCell();
  const CellRef clear       = h.vm.mintOpcode(OpcodeKind::StreamClear);
  const CellRef appendFirst = h.vm.mintOpcode(OpcodeKind::StreamAppend);
  const CellRef appendLater = h.vm.mintOpcode(OpcodeKind::StreamAppend);
  const CellRef halt        = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef code[]{clear, appendFirst, appendLater, halt};
  bindSequence(h, code);
  h.core.bindOutput(clear, stream);
  h.core.bindInput(appendFirst, first);
  h.core.bindOutput(appendFirst, stream);
  h.core.bindInput(appendLater, later);
  h.core.bindOutput(appendLater, stream);
  const CellRef cursor = h.vm.spawnCursor(clear, "stream-choice");

  const auto partial = h.vm.run(cursor, 2);
  EXPECT_FALSE(partial.success);
  ASSERT_EQ(h.vm.resultCells(stream), (std::vector<CellRef>{first}));
  h.vm.pushChoicePoint(cursor, halt);
  EXPECT_FALSE(h.vm.stealNearRootChoicePoint());
  const auto continued = h.vm.run(cursor, 1);
  EXPECT_FALSE(continued.success);
  EXPECT_EQ(h.vm.resultCells(stream), (std::vector<CellRef>{first, later}));

  ASSERT_TRUE(h.vm.backtrack(cursor));
  EXPECT_EQ(h.vm.resultCells(stream), (std::vector<CellRef>{first}));
}

TEST(VortexStreamTest, ScalarOpcodeRejectsStreamRegister) {
  Harness h;
  const CellRef stream = h.arena.makeCell();
  const CellRef result = h.arena.makeCell();
  const CellRef clear  = h.vm.mintOpcode(OpcodeKind::StreamClear);
  const CellRef add    = h.vm.mintOpcode(OpcodeKind::Add);
  const CellRef halt   = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef code[]{clear, add, halt};
  bindSequence(h, code);
  h.core.bindOutput(clear, stream);
  h.core.bindInput(add, stream);
  h.core.bindInput(add, h.arena.makeScalarCell(std::int64_t{1}));
  h.core.bindOutput(add, result);
  const CellRef cursor = h.vm.spawnCursor(clear, "stream-scalar-refusal");

  const auto run = h.vm.run(cursor, 4);
  EXPECT_FALSE(run.success);
  EXPECT_NE(run.errorMessage.find("stream register"), std::string::npos);
}

TEST(VortexStreamTest, StreamInstructionsRejectParameterPipelines) {
  Harness h;
  const CellRef stream   = h.arena.makeCell();
  const CellRef source   = h.arena.makeCell("source");
  const CellRef append   = h.vm.mintOpcode(OpcodeKind::StreamAppend);
  const CellRef pipeline = h.vm.mintOpcode(OpcodeKind::Nop);
  const CellRef halt     = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef code[]{append, halt};
  bindSequence(h, code);
  h.core.bindInput(append, source);
  h.core.bindOutput(append, stream);
  h.core.attachPipeline(source, pipeline);
  const CellRef cursor = h.vm.spawnCursor(append, "stream-pipeline-refusal");

  const auto run = h.vm.run(cursor, 2);
  EXPECT_FALSE(run.success);
  EXPECT_NE(run.errorMessage.find("do not support pipelines"),
            std::string::npos);
}

TEST(VortexStreamTest, ChoicePointRestoresIteratorPosition) {
  Harness h;
  const CellRef a = h.arena.makeCell("a");
  const CellRef b = h.arena.makeCell("b");
  const CellRef c = h.arena.makeCell("c");
  ASSERT_TRUE(h.arena.link(a, h.core.dims().step, DimVector::POS, b));
  ASSERT_TRUE(h.arena.link(b, h.core.dims().step, DimVector::POS, c));
  const CellRef iterator = h.arena.makeCell();
  const CellRef item     = h.arena.makeCell();
  const CellRef present  = h.arena.makeCell();
  const CellRef begin    = h.vm.mintOpcode(OpcodeKind::RankBegin);
  const CellRef nextOne  = h.vm.mintOpcode(OpcodeKind::IteratorNext);
  const CellRef nextTwo  = h.vm.mintOpcode(OpcodeKind::IteratorNext);
  const CellRef halt     = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef code[]{begin, nextOne, nextTwo, halt};
  bindSequence(h, code);
  h.core.bindInput(begin, a);
  h.core.bindInput(begin, h.arena.makeScalarCell(
                              static_cast<std::int64_t>(h.core.dims().step)));
  h.core.bindInput(begin, h.arena.makeScalarCell(std::int64_t{1}));
  h.core.bindOutput(begin, iterator);
  for (const CellRef next : {nextOne, nextTwo}) {
    h.core.bindInput(next, iterator);
    h.core.bindOutput(next, item);
    h.core.bindOutput(next, present);
  }
  const CellRef cursor = h.vm.spawnCursor(begin, "iterator-choice");

  EXPECT_FALSE(h.vm.run(cursor, 2).success);
  ASSERT_EQ(h.vm.cellReferenceOf(item), b);
  h.vm.pushChoicePoint(cursor, nextTwo);
  EXPECT_FALSE(h.vm.run(cursor, 1).success);
  ASSERT_EQ(h.vm.cellReferenceOf(item), c);
  ASSERT_TRUE(h.vm.backtrack(cursor));
  EXPECT_FALSE(h.vm.run(cursor, 1).success);
  EXPECT_EQ(h.vm.cellReferenceOf(item), c);
}

TEST(VortexStreamTest, RejectsOverlappingActiveStreamCursors) {
  Harness h;
  const CellRef streamOne  = h.arena.makeCell();
  const CellRef first      = h.vm.mintOpcode(OpcodeKind::StreamClear);
  const CellRef keepActive = h.vm.mintOpcode(OpcodeKind::Nop);
  const CellRef haltFirst  = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef second     = h.vm.mintOpcode(OpcodeKind::Add);
  const CellRef haltSecond = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef firstCode[]{first, keepActive, haltFirst};
  const CellRef secondCode[]{second, haltSecond};
  bindSequence(h, firstCode);
  bindSequence(h, secondCode);
  h.core.bindOutput(first, streamOne);
  h.core.bindInput(second, h.arena.makeScalarCell(std::int64_t{1}));
  h.core.bindInput(second, h.arena.makeScalarCell(std::int64_t{2}));
  h.core.bindOutput(second, h.arena.makeCell());
  const CellRef firstCursor  = h.vm.spawnCursor(first, "first-stream-cursor");
  const CellRef secondCursor = h.vm.spawnCursor(second, "second-stream-cursor");

  EXPECT_FALSE(h.vm.run(firstCursor, 1).success);
  const auto result = h.vm.run(secondCursor, 1);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.errorMessage.find("one active stream cursor"),
            std::string::npos);
}

TEST(VortexStreamTest, FinishedOwnerCannotTransferWhileChoicePointsRemain) {
  Harness h;
  const CellRef firstStream  = h.arena.makeCell();
  const CellRef secondStream = h.arena.makeCell();
  const CellRef first        = h.vm.mintOpcode(OpcodeKind::StreamClear);
  const CellRef firstHalt    = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef second       = h.vm.mintOpcode(OpcodeKind::StreamClear);
  const CellRef secondHalt   = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef firstCode[]{first, firstHalt};
  const CellRef secondCode[]{second, secondHalt};
  bindSequence(h, firstCode);
  bindSequence(h, secondCode);
  h.core.bindOutput(first, firstStream);
  h.core.bindOutput(second, secondStream);
  const CellRef firstCursor  = h.vm.spawnCursor(first, "finished-stream-owner");
  const CellRef secondCursor = h.vm.spawnCursor(second, "next-stream-owner");

  ASSERT_TRUE(h.vm.run(firstCursor, 10).success);
  h.vm.pushChoicePoint(firstCursor, firstHalt);
  const auto blocked = h.vm.run(secondCursor, 1);
  EXPECT_FALSE(blocked.success);
  EXPECT_NE(blocked.errorMessage.find("choice points remain"),
            std::string::npos);

  h.vm.clearChoicePoints();
  ASSERT_TRUE(h.vm.run(secondCursor, 10).success);
  EXPECT_TRUE(h.vm.resultCells(firstStream).empty());
}

TEST(VortexStreamTest, ScalarWriteReusesPreviouslyTypedStreamRegister) {
  Harness h;
  const CellRef registerCell = h.arena.makeCell();
  const CellRef resultCell   = h.arena.makeCell();
  const CellRef clear        = h.vm.mintOpcode(OpcodeKind::StreamClear);
  const CellRef firstHalt    = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef write        = h.vm.mintOpcode(OpcodeKind::Add);
  const CellRef read         = h.vm.mintOpcode(OpcodeKind::Add);
  const CellRef secondHalt   = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef firstCode[]{clear, firstHalt};
  const CellRef secondCode[]{write, read, secondHalt};
  bindSequence(h, firstCode);
  bindSequence(h, secondCode);
  h.core.bindOutput(clear, registerCell);
  h.core.bindInput(write, h.arena.makeScalarCell(std::int64_t{2}));
  h.core.bindInput(write, h.arena.makeScalarCell(std::int64_t{3}));
  h.core.bindOutput(write, registerCell);
  h.core.bindInput(read, registerCell);
  h.core.bindInput(read, h.arena.makeScalarCell(std::int64_t{1}));
  h.core.bindOutput(read, resultCell);
  const CellRef firstCursor =
      h.vm.spawnCursor(clear, "typed-register-producer");
  const CellRef secondCursor = h.vm.spawnCursor(write, "typed-register-reuse");

  ASSERT_TRUE(h.vm.run(firstCursor, 10).success);
  const auto run = h.vm.run(secondCursor, 10);
  ASSERT_TRUE(run.success) << run.errorMessage;
  EXPECT_EQ(h.core.render(resultCell), CellValue{static_cast<std::int64_t>(6)});
}

} // namespace

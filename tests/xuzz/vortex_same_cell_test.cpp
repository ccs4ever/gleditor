/**
 * @file vortex_same_cell_test.cpp
 * @brief SameCell: identity of two cells, never their content.
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace zigzag::vortex {
namespace {

struct Harness {
  explicit Harness(zigzag::ArenaManifold &target)
      : arena(target), core(target), vm(core) {}

  zigzag::ArenaManifold &arena;
  VortexCore core;
  VortexVM vm;

  struct Outcome {
    ExecutionResult result;
    CellValue value;
  };

  // Registers are the cells themselves unless a reference was published, so
  // binding a cell twice or two cells that hold one reference both read well.
  Outcome same(const CellRef left, const CellRef right) {
    const auto op     = vm.mintOpcode(OpcodeKind::SameCell);
    const auto output = arena.makeCell("same");
    core.bindInput(op, left);
    core.bindInput(op, right);
    core.bindOutput(op, output);
    const auto result = vm.step(vm.spawnCursor(op));
    return {result, core.render(output).value_or(CellValue{std::string{"unset"}})};
  }

  bool isSame(const CellRef left, const CellRef right) {
    const auto outcome = same(left, right);
    EXPECT_TRUE(outcome.result.success) << outcome.result.errorMessage;
    const auto *const flag = std::get_if<bool>(&outcome.value);
    EXPECT_NE(flag, nullptr);
    return flag != nullptr && *flag;
  }

  // A register that holds a reference to @p target.
  CellRef registerFor(const CellRef target) {
    const auto copy = vm.mintOpcode(OpcodeKind::CopyCellRefs);
    const auto slot = arena.makeCell("register");
    core.bindInput(copy, target);
    core.bindOutput(copy, slot);
    const auto result = vm.step(vm.spawnCursor(copy));
    EXPECT_TRUE(result.success) << result.errorMessage;
    EXPECT_EQ(vm.cellReferenceOf(slot), std::optional<CellRef>{target});
    return slot;
  }

  // A register that holds no cell: a Link read along a dimension nothing uses.
  CellRef absentRegister() {
    const auto anchor = arena.makeCell("anchor");
    const auto dim    = core.findOrMintDimension("d.same-cell-absent");
    const auto link   = vm.mintOpcode(OpcodeKind::Link);
    const auto slot   = arena.makeCell("absent");
    core.bindInput(link, anchor);
    core.bindInput(link, arena.makeScalarCell(static_cast<std::int64_t>(dim)));
    core.bindInput(link, arena.makeScalarCell(std::int64_t{1}));
    core.bindOutput(link, slot);
    const auto result = vm.step(vm.spawnCursor(link));
    EXPECT_TRUE(result.success) << result.errorMessage;
    EXPECT_EQ(vm.cellReferenceOf(slot), std::optional<CellRef>{});
    EXPECT_TRUE(vm.cellReferenceOf(slot) == std::nullopt);
    return slot;
  }
};

struct OwnedArena {
  zigzag::ArenaManifold owned;
};

struct ArenaHarness : OwnedArena, Harness {
  ArenaHarness() : Harness(owned) {}
};

// A store with three cells, folded, and an arena over it. The folded manifold
// is attached as a space on demand so proxies of its cells can be minted.
struct StoreFixture {
  xanadu::Store store;
  zigzag::CellRef first{};
  zigzag::CellRef second{};
  zigzag::Manifold base;
  zigzag::ArenaManifold arena;

  static zigzag::Manifold fold(xanadu::Store &store, zigzag::CellRef &first,
                               zigzag::CellRef &second) {
    auto at = store.sliceGenesis(xanadu::MicroversionId{});
    at      = store.makeCell(at, "twin");
    first   = store.cellRefOf(at).value();
    at      = store.makeCell(at, "twin");
    second  = store.cellRefOf(at).value();
    return store.rebuildManifold(at);
  }

  StoreFixture() : base(fold(store, first, second)), arena(&base, &store) {}

  std::uint32_t attach(const std::string &label) {
    return arena.attach(zigzag::Space{
        .manifold = &base, .store = &store, .reader = &store, .label = label});
  }
};

TEST(VortexSameCellTest, TheSameRegisterTwiceIsTheSameCell) {
  ArenaHarness h;
  const auto cell = h.arena.makeCell("only");
  EXPECT_TRUE(h.isSame(cell, cell));
}

TEST(VortexSameCellTest, TwoRegistersReferencingOneCellAreTheSameCell) {
  ArenaHarness h;
  const auto cell  = h.arena.makeCell("target");
  const auto one   = h.registerFor(cell);
  const auto other = h.registerFor(cell);
  ASSERT_NE(one, other);
  EXPECT_TRUE(h.isSame(one, other));
  // A register and the cell it references are one cell as well.
  EXPECT_TRUE(h.isSame(one, cell));
}

TEST(VortexSameCellTest, DistinctCellsWithEqualContentAreNotTheSameCell) {
  ArenaHarness h;
  const auto left  = h.arena.makeScalarCell(std::int64_t{7});
  const auto right = h.arena.makeScalarCell(std::int64_t{7});
  EXPECT_FALSE(h.isSame(left, right));
  const auto text      = h.arena.makeCell("same text");
  const auto otherText = h.arena.makeCell("same text");
  EXPECT_FALSE(h.isSame(text, otherText));
  EXPECT_FALSE(h.isSame(h.registerFor(left), h.registerFor(right)));
}

TEST(VortexSameCellTest, AnAbsentInputMakesTheResultFalseNotAnError) {
  ArenaHarness h;
  const auto cell   = h.arena.makeCell("present");
  const auto absent = h.absentRegister();
  EXPECT_FALSE(h.isSame(absent, cell));
  EXPECT_FALSE(h.isSame(cell, absent));
  // There is no cell to be the same.
  EXPECT_FALSE(h.isSame(absent, absent));
  EXPECT_FALSE(h.isSame(absent, h.absentRegister()));
}

TEST(VortexSameCellTest, AProxyAndItsSourceCellAreTheSameCell) {
  StoreFixture f;
  const auto space = f.attach("live");
  Harness h(f.arena);
  const auto proxy = f.arena.proxyFor(space, f.first);
  ASSERT_NE(proxy, f.first);
  EXPECT_TRUE(h.isSame(proxy, f.first));
  EXPECT_TRUE(h.isSame(f.first, proxy));
  EXPECT_FALSE(h.isSame(proxy, f.second));
  EXPECT_FALSE(h.isSame(proxy, f.arena.proxyFor(space, f.second)));
}

TEST(VortexSameCellTest, TwoProxiesOfOneSourceAreTheSameCell) {
  StoreFixture f;
  const auto early = f.attach("early");
  const auto late  = f.attach("late");
  Harness h(f.arena);
  const auto one   = f.arena.proxyFor(early, f.first);
  const auto other = f.arena.proxyFor(late, f.first);
  ASSERT_NE(one, other);
  EXPECT_TRUE(h.isSame(one, other));
  EXPECT_FALSE(h.isSame(one, f.arena.proxyFor(late, f.second)));
}

TEST(VortexSameCellTest, ProxiesOfTheSameIndexInTwoStoresAreDifferentCells) {
  StoreFixture f;
  StoreFixture g;
  ASSERT_EQ(f.first, g.first);
  const auto own     = f.attach("own");
  const auto foreign = f.arena.attach(zigzag::Space{.manifold = &g.base,
                                                    .store    = &g.store,
                                                    .reader   = &g.store,
                                                    .label    = "foreign"});
  Harness h(f.arena);
  EXPECT_FALSE(h.isSame(f.arena.proxyFor(own, f.first),
                        f.arena.proxyFor(foreign, g.first)));
}

TEST(VortexSameCellTest, AnEphemeralCellIsNeverAStoreCell) {
  StoreFixture f;
  const auto space = f.attach("live");
  Harness h(f.arena);
  const auto scratch = f.arena.makeCell("twin");
  EXPECT_FALSE(h.isSame(scratch, f.first));
  EXPECT_FALSE(h.isSame(f.first, scratch));
  EXPECT_FALSE(h.isSame(scratch, f.arena.proxyFor(space, f.first)));
  EXPECT_TRUE(h.isSame(scratch, scratch));
}

TEST(VortexSameCellTest, RefusesAnyArityButTwoInputsAndOneOutput) {
  ArenaHarness h;
  const auto a           = h.arena.makeCell("a");
  const auto b           = h.arena.makeCell("b");
  const auto bindAndStep = [&](const std::vector<CellRef> &inputs,
                               const std::vector<CellRef> &outputs) {
    const auto op = h.vm.mintOpcode(OpcodeKind::SameCell);
    for (const auto input : inputs) h.core.bindInput(op, input);
    for (const auto output : outputs) h.core.bindOutput(op, output);
    return h.vm.step(h.vm.spawnCursor(op));
  };
  const auto out = h.arena.makeCell("out");
  for (const auto &result : {bindAndStep({a}, {out}), bindAndStep({}, {out}),
                             bindAndStep({a, b, a}, {out})}) {
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find("SameCell expects exactly two"),
              std::string::npos)
        << result.errorMessage;
  }
  const auto missing = bindAndStep({a, b}, {});
  EXPECT_FALSE(missing.success);
  EXPECT_NE(missing.errorMessage.find("exactly one output"), std::string::npos)
      << missing.errorMessage;
}

TEST(VortexSameCellTest, IsPureAndNeverMemoisedByContent) {
  ArenaHarness h;
  // Four cells of equal content: identity must not share a memo entry between
  // two ops asking about different pairs.
  const auto a     = h.arena.makeCell("x");
  const auto b     = h.arena.makeCell("x");
  const auto c     = h.arena.makeCell("x");
  const auto asked = [&](const CellRef left, const CellRef right) {
    const auto op     = h.vm.mintOpcode(OpcodeKind::SameCell);
    const auto output = h.arena.makeCell("same");
    h.core.bindInput(op, left);
    h.core.bindInput(op, right);
    h.core.bindOutput(op, output);
    h.vm.enableMemoization(op, "same-cell-shared-key");
    const auto result = h.vm.step(h.vm.spawnCursor(op));
    EXPECT_TRUE(result.success) << result.errorMessage;
    return h.core.render(output).value_or(CellValue{std::string{"unset"}});
  };
  EXPECT_EQ(asked(a, a), CellValue{true});
  EXPECT_EQ(asked(b, c), CellValue{false});
  EXPECT_EQ(asked(c, c), CellValue{true});
  EXPECT_EQ(asked(a, b), CellValue{false});
}

TEST(VortexSameCellTest, RunsAsAParameterPipelineStage) {
  ArenaHarness h;
  const auto left  = h.arena.makeCell("left");
  const auto right = h.arena.makeCell("right");
  const auto probe = h.arena.makeCell("probe");
  // The stage has no output of its own: it writes the parameter it filters.
  const auto stage = h.vm.mintOpcode(OpcodeKind::SameCell);
  h.core.bindInput(stage, left);
  h.core.bindInput(stage, right);
  h.core.attachPipeline(probe, stage);
  const auto consumer = h.vm.mintOpcode(OpcodeKind::Not);
  const auto sink     = h.arena.makeCell("sink");
  h.core.bindInput(consumer, probe);
  h.core.bindOutput(consumer, sink);
  const auto result = h.vm.step(h.vm.spawnCursor(consumer));
  ASSERT_TRUE(result.success) << result.errorMessage;
  // left and right are distinct, so the stage wrote false and Not saw it.
  EXPECT_EQ(h.core.render(sink), std::optional<CellValue>{true});
}

// The pattern a bounded rank walk needs: step along a rank and stop when the
// item is a given cell, by identity.
struct RankWalk {
  ArenaHarness h;
  std::vector<CellRef> cells;
  CellRef anchor{};
  DimRef dim{};

  RankWalk() {
    dim           = h.core.findOrMintDimension("d.walk");
    anchor        = h.arena.makeCell("anchor");
    auto previous = anchor;
    for (int index = 0; index < 5; ++index) {
      // Equal content throughout: only identity can tell the cells apart.
      const auto cell = h.arena.makeCell("item");
      zigzag::expectWritten(h.arena.link(previous, dim, DimVector::POS, cell));
      cells.push_back(cell);
      previous = cell;
    }
  }

  // Walks until the item is @p stop; the cell the walk ended on, if it ended.
  std::optional<CellRef> walkUntil(const CellRef stop,
                                   const std::size_t steps) {
    const auto iterator = h.arena.makeCell("iterator");
    const auto item     = h.arena.makeCell("item register");
    const auto present  = h.arena.makeCell("present");
    const auto hit      = h.arena.makeCell("hit");
    const auto begin    = h.vm.mintOpcode(OpcodeKind::RankBegin);
    const auto next     = h.vm.mintOpcode(OpcodeKind::IteratorNext);
    const auto compare  = h.vm.mintOpcode(OpcodeKind::SameCell);
    const auto branch   = h.vm.mintOpcode(OpcodeKind::BranchFalseCell);
    const auto halt     = h.vm.mintOpcode(OpcodeKind::Halt);
    const CellRef code[]{begin, next, compare, branch, halt};
    for (std::size_t i = 1; i < std::size(code); ++i)
      zigzag::expectWritten(h.arena.link(code[i - 1], h.core.dims().spin,
                                         DimVector::POS, code[i]));
    h.core.bindInput(begin, anchor);
    h.core.bindInput(begin,
                     h.arena.makeScalarCell(static_cast<std::int64_t>(dim)));
    h.core.bindInput(begin, h.arena.makeScalarCell(std::int64_t{1}));
    h.core.bindOutput(begin, iterator);
    h.core.bindInput(next, iterator);
    h.core.bindOutput(next, item);
    h.core.bindOutput(next, present);
    h.core.bindInput(compare, item);
    h.core.bindInput(compare, stop);
    h.core.bindOutput(compare, hit);
    h.core.bindInput(branch, hit);
    h.core.bindInput(branch, next); // not the stop cell yet: take the next one
    const auto cursor = h.vm.spawnCursor(begin);
    const auto run    = h.vm.run(cursor, steps);
    if (!run.success) return std::nullopt;
    return h.vm.resultCell(item);
  }
};

TEST(VortexSameCellTest, EndsARankWalkAtTheStopCellAsABranchCondition) {
  RankWalk walk;
  const auto stop = walk.cells[2];
  ASSERT_EQ(walk.h.arena.textOf(walk.cells[0]),
            walk.h.arena.textOf(walk.cells[2]));
  EXPECT_EQ(walk.walkUntil(stop, 64), std::optional<CellRef>{stop});
}

TEST(VortexSameCellTest, ARankWalkWhoseStopIsNeverMetDoesNotEndAtAnItem) {
  RankWalk walk;
  const auto elsewhere = walk.h.arena.makeCell("item");
  EXPECT_EQ(walk.walkUntil(elsewhere, 64), std::nullopt);
}

} // namespace
} // namespace zigzag::vortex

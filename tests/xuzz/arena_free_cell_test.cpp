/**
 * @file arena_free_cell_test.cpp
 * @brief Per-cell release (P4): freeCell() frees one cell of an arena, the next
 * mint reuses its slot under a new incarnation, and the choice-point machinery
 * keeps its meaning around both.
 *
 * What each test pins down is a way the feature could silently go wrong: a
 * stale handle freeing the new tenant of a slot, a freed cell still answering
 * reads, a backtrack that forgets a free (or a reuse), a base cell freed
 * through its shadow, a promotion that carries a freed cell.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using xanadu::MicroversionId;
using xanadu::Store;
using zigzag::ArenaManifold;
using zigzag::ArenaRefusal;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::noCell;

struct Arena {
  ArenaManifold m;
  DimRef step{noCell};

  Arena() { step = m.makeCell("d.step"); }

  struct Named {
    CellRef cell{noCell};
    ArenaManifold::AllocationIncarnation incarnation{0};
  };
  Named mint(const std::string_view text) {
    const auto cell = m.makeCell(text);
    return {cell, m.incarnationOf(cell).value()};
  }
  void free(const Named &named) {
    ASSERT_TRUE(m.freeCell(named.cell, named.incarnation));
  }
};

TEST(ArenaFreeCellTest, aFreedCellAnswersAsIfItWereNeverMinted) {
  Arena arena;
  const auto kept  = arena.mint("kept");
  const auto freed = arena.mint("freed");
  const auto live  = arena.m.liveCells();
  arena.free(freed);
  EXPECT_EQ(arena.m.liveCells(), live - 1U);
  EXPECT_EQ(arena.m.freeSlotCount(), 1U);
  EXPECT_FALSE(arena.m.contains(freed.cell));
  EXPECT_FALSE(arena.m.incarnationOf(freed.cell));
  EXPECT_FALSE(arena.m.matches(freed.cell, freed.incarnation));
  EXPECT_FALSE(arena.m.linked(freed.cell, arena.step, DimVector::POS));
  EXPECT_FALSE(arena.m.link(freed.cell, arena.step, DimVector::POS, kept.cell));
  EXPECT_EQ(
      arena.m.link(kept.cell, arena.step, DimVector::POS, freed.cell).error(),
      ArenaRefusal::UnknownTarget);
  EXPECT_EQ(arena.m.textOf(kept.cell), "kept");
}

TEST(ArenaFreeCellTest,
     aReusedSlotGetsANewIncarnationAndStaleHandlesAreRefused) {
  Arena arena;
  const auto old        = arena.mint("old");
  const auto sizeBefore = arena.m.cellCount();
  auto lease            = std::make_shared<int>(7);
  ASSERT_TRUE(arena.m.setOwnerLease(old.cell, old.incarnation, lease));
  ASSERT_TRUE(arena.m.markRuntimeOnly(old.cell));
  ASSERT_TRUE(arena.m.setRuntimeTag(old.cell, old.incarnation, 9));
  EXPECT_EQ(lease.use_count(), 2);
  arena.free(old);
  EXPECT_EQ(lease.use_count(), 1) << "the arena's hold goes with the cell";

  const auto fresh = arena.mint("fresh");
  EXPECT_EQ(fresh.cell, old.cell) << "the freed slot is reused";
  EXPECT_NE(fresh.incarnation, old.incarnation);
  EXPECT_EQ(arena.m.cellCount(), sizeBefore) << "no growth";
  EXPECT_EQ(arena.m.freeSlotCount(), 0U);
  EXPECT_EQ(arena.m.textOf(fresh.cell), "fresh");

  // Every owner-side accessor refuses the old handle, and none of the old
  // cell's state leaks into the new tenant.
  EXPECT_FALSE(arena.m.matches(old.cell, old.incarnation));
  EXPECT_EQ(arena.m.setOwnerLease(old.cell, old.incarnation, lease).error(),
            ArenaRefusal::UnknownCell);
  EXPECT_FALSE(arena.m.ownerLeaseOf(old.cell, old.incarnation));
  EXPECT_FALSE(arena.m.ownerLeaseOf(fresh.cell, fresh.incarnation));
  EXPECT_FALSE(arena.m.isRuntimeOnly(old.cell, old.incarnation));
  EXPECT_FALSE(arena.m.isRuntimeOnly(fresh.cell, fresh.incarnation));
  EXPECT_FALSE(arena.m.runtimeTagOf(fresh.cell, fresh.incarnation));
  EXPECT_FALSE(arena.m.runtimeTagOf(old.cell, old.incarnation));
  EXPECT_EQ(arena.m.setRuntimeTag(old.cell, old.incarnation, 1).error(),
            ArenaRefusal::UnknownCell);

  // The stale handle cannot free the new tenant.
  EXPECT_EQ(arena.m.freeCell(old.cell, old.incarnation).error(),
            ArenaRefusal::UnknownCell);
  EXPECT_TRUE(arena.m.matches(fresh.cell, fresh.incarnation));
  // And a freed cell cannot be freed twice.
  arena.free(fresh);
  EXPECT_EQ(arena.m.freeCell(fresh.cell, fresh.incarnation).error(),
            ArenaRefusal::UnknownCell);
  EXPECT_EQ(arena.m.freeSlotCount(), 1U);
}

TEST(ArenaFreeCellTest, aCellWithALiveLinkIsRefusedUntilItIsDetached) {
  Arena arena;
  const auto a = arena.mint("a");
  const auto b = arena.mint("b");
  const auto c = arena.mint("c");
  ASSERT_TRUE(arena.m.link(a.cell, arena.step, DimVector::POS, b.cell));
  ASSERT_TRUE(arena.m.link(b.cell, arena.step, DimVector::POS, c.cell));

  // Both ends of an edge, and a middle cell with two, are refused.
  EXPECT_EQ(arena.m.freeCell(a.cell, a.incarnation).error(),
            ArenaRefusal::CellLinked);
  EXPECT_EQ(arena.m.freeCell(b.cell, b.incarnation).error(),
            ArenaRefusal::CellLinked);
  EXPECT_EQ(arena.m.freeCell(c.cell, c.incarnation).error(),
            ArenaRefusal::CellLinked);
  EXPECT_EQ(arena.m.freeSlotCount(), 0U);
  EXPECT_EQ(arena.m.linked(a.cell, arena.step, DimVector::POS), b.cell);

  // Detach the middle the way a reaper does: relink around it, then free.
  ASSERT_TRUE(arena.m.unlink(b.cell, arena.step, DimVector::NEG));
  ASSERT_TRUE(arena.m.unlink(b.cell, arena.step, DimVector::POS));
  ASSERT_TRUE(arena.m.link(a.cell, arena.step, DimVector::POS, c.cell));
  arena.free(b);
  EXPECT_EQ(arena.m.linked(a.cell, arena.step, DimVector::POS), c.cell);
  EXPECT_EQ(arena.m.linked(c.cell, arena.step, DimVector::NEG), a.cell);
}

TEST(ArenaFreeCellTest, aDimensionAProxyAndAShadowAreNotReleasable) {
  Arena arena;
  const auto dim = arena.m.ensureDimension("d.named");
  EXPECT_EQ(arena.m.freeCell(dim, arena.m.incarnationOf(dim).value()).error(),
            ArenaRefusal::NotReleasable);
  const auto space = arena.m.attach(zigzag::Space{.label = "elsewhere"});
  const auto proxy = arena.m.proxyFor(space, 42);
  EXPECT_EQ(
      arena.m.freeCell(proxy, arena.m.incarnationOf(proxy).value()).error(),
      ArenaRefusal::NotReleasable);
  EXPECT_TRUE(arena.m.contains(dim));
  EXPECT_TRUE(arena.m.contains(proxy));
}

TEST(ArenaFreeCellTest, aBaseCellIsNeverFreedNeitherItselfNorThroughItsShadow) {
  Store store;
  const auto genesis = store.sliceGenesis(MicroversionId{});
  const auto base    = store.rebuildManifold(genesis);
  ArenaManifold arena(&base, &store);
  const auto home = base.home();
  const auto dims = arena.dimensionNamed("d.dims");
  ASSERT_TRUE(home && dims);

  // Read through, not held: not a cell of this arena.
  EXPECT_FALSE(arena.holdsOwn(*home));
  EXPECT_EQ(arena.freeCell(*home, 1).error(), ArenaRefusal::UnknownCell);

  // A write shadows it; the shadow carries the base cell's own ref and an
  // incarnation, and freeing it is refused rather than dropping the base's
  // cell.
  const auto extra = arena.makeCell("extra");
  ASSERT_TRUE(arena.link(*home, *dims, DimVector::POS, extra));
  ASSERT_TRUE(arena.holdsOwn(*home));
  const auto shadowIncarnation = arena.incarnationOf(*home).value();
  EXPECT_EQ(arena.freeCell(*home, shadowIncarnation).error(),
            ArenaRefusal::NotReleasable);
  EXPECT_TRUE(arena.holdsOwn(*home));
  EXPECT_EQ(arena.linked(*home, *dims, DimVector::POS), extra);

  // An arena cell of the same arena is freed as usual, and the base is whole.
  ASSERT_TRUE(arena.unlink(*home, *dims, DimVector::POS));
  ASSERT_TRUE(arena.freeCell(extra, arena.incarnationOf(extra).value()));
  EXPECT_TRUE(base.contains(*home));
  EXPECT_FALSE(arena.contains(extra));
}

TEST(ArenaFreeCellTest, aFreeInsideAMarkIsUndoneByReleasingTheMark) {
  Arena arena;
  const auto old = arena.mint("old");
  auto lease     = std::make_shared<int>(1);
  ASSERT_TRUE(arena.m.setOwnerLease(old.cell, old.incarnation, lease));
  ASSERT_TRUE(arena.m.setRuntimeTag(old.cell, old.incarnation, 5));
  const auto cells = arena.m.cellCount();
  const auto live  = arena.m.liveCells();

  const auto mark = arena.m.mark();
  arena.free(old);
  EXPECT_FALSE(arena.m.contains(old.cell));
  EXPECT_EQ(arena.m.freeSlotCount(), 1U);
  ASSERT_TRUE(arena.m.release(mark));

  // Backtracking brings the cell back exactly: identity, content, lease, tag.
  EXPECT_EQ(arena.m.freeSlotCount(), 0U);
  EXPECT_EQ(arena.m.liveCells(), live);
  EXPECT_EQ(arena.m.cellCount(), cells);
  EXPECT_TRUE(arena.m.matches(old.cell, old.incarnation));
  EXPECT_EQ(arena.m.textOf(old.cell), "old");
  EXPECT_EQ(arena.m.ownerLeaseOf(old.cell, old.incarnation), lease);
  EXPECT_EQ(arena.m.runtimeTagOf(old.cell, old.incarnation),
            std::optional<std::uint32_t>{5});
  EXPECT_TRUE(arena.m.isRuntimeOnly(old.cell, old.incarnation));
  // And it frees again afterwards.
  arena.free(old);
  EXPECT_EQ(lease.use_count(), 1);
}

TEST(ArenaFreeCellTest, aFreeKeptByDiscardOutlivesTheMark) {
  Arena arena;
  const auto old  = arena.mint("old");
  const auto mark = arena.m.mark();
  arena.free(old);
  ASSERT_TRUE(arena.m.discard(mark));
  EXPECT_FALSE(arena.m.contains(old.cell));
  EXPECT_EQ(arena.m.freeSlotCount(), 1U);
  EXPECT_EQ(arena.mint("again").cell, old.cell);
}

TEST(ArenaFreeCellTest, aReuseInsideAMarkIsUndoneAndTheSlotIsFreeAgain) {
  Arena arena;
  const auto old = arena.mint("old");
  arena.free(old);
  const auto cells = arena.m.cellCount();

  const auto mark  = arena.m.mark();
  const auto fresh = arena.mint("fresh");
  EXPECT_EQ(fresh.cell, old.cell);
  auto lease = std::make_shared<int>(2);
  ASSERT_TRUE(arena.m.setOwnerLease(fresh.cell, fresh.incarnation, lease));
  ASSERT_TRUE(arena.m.setRuntimeTag(fresh.cell, fresh.incarnation, 3));
  ASSERT_TRUE(arena.m.release(mark));

  // The branch's cell is gone, its lease and tag with it, and the slot is on
  // the free list as it was: the next mint takes it under yet another
  // incarnation.
  EXPECT_FALSE(arena.m.contains(old.cell));
  EXPECT_EQ(arena.m.freeSlotCount(), 1U);
  EXPECT_EQ(arena.m.cellCount(), cells);
  EXPECT_EQ(lease.use_count(), 1);
  const auto again = arena.mint("again");
  EXPECT_EQ(again.cell, old.cell);
  EXPECT_NE(again.incarnation, fresh.incarnation);
  EXPECT_NE(again.incarnation, old.incarnation);
  EXPECT_FALSE(arena.m.runtimeTagOf(again.cell, again.incarnation));
  EXPECT_FALSE(arena.m.matches(fresh.cell, fresh.incarnation));
}

TEST(ArenaFreeCellTest, aCellMintedAndFreedInsideAMarkLeavesNoTrace) {
  Arena arena;
  const auto keep  = arena.mint("keep");
  const auto cells = arena.m.cellCount();
  const auto mark  = arena.m.mark();
  const auto born  = arena.mint("born");
  arena.free(born);
  const auto reborn = arena.mint("reborn");
  EXPECT_EQ(reborn.cell, born.cell);
  ASSERT_TRUE(arena.m.release(mark));
  EXPECT_EQ(arena.m.cellCount(), cells);
  EXPECT_EQ(arena.m.freeSlotCount(), 0U);
  EXPECT_TRUE(arena.m.matches(keep.cell, keep.incarnation));
  // No dangling free slot: the next mint extends the arena, not a ghost.
  EXPECT_EQ(arena.mint("next").cell, ArenaManifold::refOf(cells));
}

TEST(ArenaFreeCellTest, nestedMarksUnwindFreesAndReusesInOrder) {
  Arena arena;
  const auto a     = arena.mint("a");
  const auto b     = arena.mint("b");
  const auto outer = arena.m.mark();
  arena.free(a);
  const auto inner  = arena.m.mark();
  const auto reused = arena.mint("reused");
  EXPECT_EQ(reused.cell, a.cell);
  arena.free(b);
  EXPECT_EQ(arena.m.freeSlotCount(), 1U);
  ASSERT_TRUE(arena.m.release(inner));
  // Back to the state after the first free: a freed, b live.
  EXPECT_FALSE(arena.m.contains(a.cell));
  EXPECT_TRUE(arena.m.matches(b.cell, b.incarnation));
  EXPECT_EQ(arena.m.freeSlotCount(), 1U);
  ASSERT_TRUE(arena.m.release(outer));
  EXPECT_TRUE(arena.m.matches(a.cell, a.incarnation));
  EXPECT_TRUE(arena.m.matches(b.cell, b.incarnation));
  EXPECT_EQ(arena.m.freeSlotCount(), 0U);
}

TEST(ArenaFreeCellTest, aFreedSlotBelowAMarkIsNotTruncatedWithTheBranch) {
  // The sweep that forgets a branch's runtime state used to compare against
  // the newest surviving slot; a reused low slot is newer than that.
  Arena arena;
  const auto low = arena.mint("low");
  const auto top = arena.mint("top");
  arena.free(low);
  const auto mark  = arena.m.mark();
  const auto fresh = arena.mint("fresh");
  EXPECT_EQ(fresh.cell, low.cell);
  ASSERT_TRUE(arena.m.setRuntimeTag(fresh.cell, fresh.incarnation, 4));
  ASSERT_TRUE(arena.m.setRuntimeTag(top.cell, top.incarnation, 6));
  ASSERT_TRUE(arena.m.discard(mark));
  // Kept by discard, state and all.
  EXPECT_EQ(arena.m.runtimeTagOf(fresh.cell, fresh.incarnation),
            std::optional<std::uint32_t>{4});
  const auto inside = arena.m.mark();
  const auto extra  = arena.mint("extra");
  ASSERT_TRUE(arena.m.setRuntimeTag(extra.cell, extra.incarnation, 8));
  ASSERT_TRUE(arena.m.release(inside));
  EXPECT_EQ(arena.m.runtimeTagOf(fresh.cell, fresh.incarnation),
            std::optional<std::uint32_t>{4});
  EXPECT_EQ(arena.m.runtimeTagOf(top.cell, top.incarnation),
            std::optional<std::uint32_t>{6});
}

TEST(ArenaFreeCellTest, compactionReclaimsAFreedCellsRunsAndKeepsTheRest) {
  Arena arena;
  const auto a = arena.mint("alpha");
  const auto b = arena.mint("beta");
  const auto c = arena.mint("gamma");
  ASSERT_TRUE(arena.m.link(a.cell, arena.step, DimVector::POS, c.cell));
  ASSERT_TRUE(arena.m.unlink(a.cell, arena.step, DimVector::POS));
  arena.free(b);
  arena.free(a);
  EXPECT_GT(arena.m.deadLinks(), 0U);
  ASSERT_TRUE(arena.m.compact());
  EXPECT_EQ(arena.m.deadLinks(), 0U);
  EXPECT_EQ(arena.m.textOf(c.cell), "gamma");
  const auto mark = arena.m.mark();
  EXPECT_EQ(arena.m.compact().error(), ArenaRefusal::MarksOutstanding);
  ASSERT_TRUE(arena.m.release(mark));
}

TEST(ArenaFreeCellTest, promotionNeitherCarriesNorAcceptsAFreedCell) {
  Store store;
  auto at = store.sliceGenesis(MicroversionId{});
  Arena arena;
  const auto answer = arena.mint("answer");
  const auto tail   = arena.mint("tail");
  const auto gone   = arena.mint("gone");
  ASSERT_TRUE(arena.m.link(answer.cell, arena.step, DimVector::POS, tail.cell));
  arena.free(gone);

  EXPECT_FALSE(zigzag::promote(store, at, arena.m, gone.cell).has_value());
  const auto promoted = zigzag::promote(store, at, arena.m, answer.cell);
  ASSERT_TRUE(promoted.has_value());
  EXPECT_EQ(promoted->cells.size(), 3U) << "answer, tail and the d.step cell";
  const auto manifold = store.rebuildManifold(promoted->version);
  EXPECT_EQ(manifold.textOf(promoted->cells.front(), store), "answer");

  // A slot reused after the free promotes as the cell it now is.
  const auto reused = arena.mint("reused");
  EXPECT_EQ(reused.cell, gone.cell);
  const auto again =
      zigzag::promote(store, promoted->version, arena.m, reused.cell);
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(
      store.rebuildManifold(again->version).textOf(again->cells.front(), store),
      "reused");
}

TEST(ArenaFreeCellTest, aPrefixOfAnArenaThatFreedIsRefused) {
  Arena arena;
  const auto cell = arena.mint("x");
  const auto mark = arena.m.mark();
  EXPECT_TRUE(arena.m.exportPrefix(mark).has_value());
  arena.free(cell);
  EXPECT_EQ(arena.m.exportPrefix(mark).error(),
            ArenaRefusal::UnsupportedPrefixState);
  ASSERT_TRUE(arena.m.release(mark));
}

TEST(ArenaFreeCellTest, anArenaServingAStreamStaysTheSizeOfItsLiveCells) {
  Arena arena;
  const auto resident  = arena.mint("resident");
  const auto cells     = arena.m.cellCount();
  const auto links     = arena.m.deadLinks();
  constexpr int stream = 20'000;
  for (int i = 0; i < stream; ++i) {
    const auto a = arena.mint("request");
    const auto b = arena.mint("outcome");
    ASSERT_TRUE(arena.m.link(a.cell, arena.step, DimVector::POS, b.cell));
    ASSERT_TRUE(arena.m.unlink(a.cell, arena.step, DimVector::POS));
    arena.free(b);
    arena.free(a);
  }
  EXPECT_EQ(arena.m.cellCount(), cells + 2U) << "peak live cells, not history";
  EXPECT_EQ(arena.m.liveCells(), cells);
  EXPECT_LT(arena.m.deadLinks(), 1'000U + links)
      << "dead link runs are reclaimed as the arena goes";
  EXPECT_TRUE(arena.m.matches(resident.cell, resident.incarnation));
}

// A model of what is live, driven by a random walk over mint, free, link-free
// detach, mark, release and discard. After every step the arena and the model
// agree on which refs are cells, their text and their incarnations.
TEST(ArenaFreeCellTest, aRandomWalkOverMintFreeAndMarksMatchesAModel) {
  struct Live {
    ArenaManifold::AllocationIncarnation incarnation;
    std::string text;
  };
  using Model = std::map<CellRef, Live>;
  for (const std::uint32_t seed : {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U}) {
    std::mt19937 random(seed);
    Arena arena;
    Model model;
    struct Frame {
      zigzag::Mark mark;
      Model saved;
    };
    std::vector<Frame> frames;
    std::set<ArenaManifold::AllocationIncarnation> seen;
    for (int step = 0; step < 400; ++step) {
      switch (random() % 6) {
      case 0:
      case 1: {
        const auto text  = "t" + std::to_string(step);
        const auto named = arena.mint(text);
        ASSERT_TRUE(seen.insert(named.incarnation).second)
            << "an incarnation is never handed out twice";
        ASSERT_FALSE(model.contains(named.cell));
        model[named.cell] = {named.incarnation, text};
        break;
      }
      case 2:
      case 3: {
        if (model.empty()) break;
        auto it = model.begin();
        std::advance(it, random() % model.size());
        ASSERT_TRUE(arena.m.freeCell(it->first, it->second.incarnation));
        model.erase(it);
        break;
      }
      case 4:
        if (frames.size() < 3) {
          frames.push_back({arena.m.mark(), model});
        }
        break;
      default:
        if (frames.empty()) break;
        if (random() % 2 == 0) {
          ASSERT_TRUE(arena.m.release(frames.back().mark));
          model = frames.back().saved;
        } else {
          ASSERT_TRUE(arena.m.discard(frames.back().mark));
        }
        frames.pop_back();
        break;
      }
      ASSERT_EQ(arena.m.liveCells() - 1U, model.size()) << "seed " << seed;
      for (const auto &[ref, live] : model) {
        ASSERT_TRUE(arena.m.matches(ref, live.incarnation)) << "seed " << seed;
        ASSERT_EQ(arena.m.textOf(ref), live.text) << "seed " << seed;
      }
    }
    while (!frames.empty()) {
      ASSERT_TRUE(arena.m.discard(frames.back().mark));
      frames.pop_back();
    }
  }
}

} // namespace

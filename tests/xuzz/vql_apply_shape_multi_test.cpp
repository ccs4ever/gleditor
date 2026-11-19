/**
 * @file vql_apply_shape_multi_test.cpp
 * @brief apply_shape with target assignments over a dynamic or multi-cell
 * subject: the run-time reciprocal-collision check, and the guarantee that a
 * refused run leaves no effect and releases its program.
 */
#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using xanadu::vql::VQLEngine;
using zigzag::CellRef;
using zigzag::DimVector;

constexpr std::string_view collisionMessage = "shape assignment collision";
constexpr std::string_view retainedReason =
    "the run writes cells that existed before it";

class ApplyShapeMultiTest : public ::testing::Test {
protected:
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  VQLEngine engine{core};
  zigzag::vortex::VortexVM vm{core};
  xanadu::vql::QueryExecutor executor{engine, vm};

  zigzag::DimRef items = engine.resolveDimension("d.items");
  zigzag::DimRef keep  = engine.resolveDimension("d.keep");
  zigzag::DimRef edge  = engine.resolveDimension("d.edge");
  zigzag::DimRef fwd   = engine.resolveDimension("d.fwd");
  zigzag::DimRef bwd   = engine.resolveDimension("d.bwd");

  // Subjects hang off home along d.items so `##/+d.items` streams them.
  std::vector<CellRef> subjects;
  std::vector<CellRef> tracked;

  CellRef make(const std::string &text) {
    const auto cell = arena.makeCell(text);
    tracked.push_back(cell);
    return cell;
  }
  CellRef addSubject(const std::string &text) {
    const auto cell = make(text);
    EXPECT_TRUE(arena.link(subjects.empty() ? core.home() : subjects.back(),
                           items, DimVector::POS, cell));
    subjects.push_back(cell);
    return cell;
  }
  CellRef attach(CellRef from, zigzag::DimRef dim, const std::string &text) {
    const auto cell = make(text);
    EXPECT_TRUE(arena.link(from, dim, DimVector::POS, cell));
    return cell;
  }

  /// What a failed run must not change: the cell count, the trail, and every
  /// link of every cell the test knows, along every dimension it uses.
  struct Snapshot {
    std::size_t cells;
    std::size_t trail;
    std::vector<std::optional<CellRef>> links;
    bool operator==(const Snapshot &) const = default;
  };
  Snapshot snapshot() {
    Snapshot result{arena.cellCount(), arena.trailSize(), {}};
    for (const auto cell : tracked)
      for (const auto dim : {items, keep, edge, fwd, bwd})
        for (const auto direction : {DimVector::POS, DimVector::NEG})
          result.links.push_back(arena.linked(cell, dim, direction));
    return result;
  }

  std::vector<CellRef> succeeds(const std::string &query) {
    const auto result = executor.execute(query);
    EXPECT_TRUE(result) << query << ": "
                        << (result ? "" : result.error().message);
    // Writing a subject that existed before the run is exactly what keeps the
    // program's cells; nothing else may be the reason.
    EXPECT_THAT(
        executor.programRetainedBecause().value_or(std::string(retainedReason)),
        ::testing::Eq(std::string(retainedReason)))
        << query;
    EXPECT_EQ(arena.outstandingMarks(), 0U);
    return result ? *result : std::vector<CellRef>{};
  }

  /// A failed run names the collision, changes nothing, and leaves no mark.
  void expectCollision(const std::string &query) {
    const auto before = snapshot();
    const auto result = executor.execute(query);
    ASSERT_FALSE(result) << query << " was not refused";
    EXPECT_THAT(result.error().message, ::testing::HasSubstr(collisionMessage))
        << query;
    EXPECT_EQ(snapshot(), before) << query;
    EXPECT_EQ(arena.outstandingMarks(), 0U);
  }

  void expectNamedFailure(const std::string &query, std::string_view text) {
    const auto before = snapshot();
    const auto result = executor.execute(query);
    ASSERT_FALSE(result) << query << " was not refused";
    EXPECT_THAT(result.error().message, ::testing::HasSubstr(std::string(text)))
        << query;
    EXPECT_EQ(snapshot(), before) << query;
    EXPECT_EQ(arena.outstandingMarks(), 0U);
  }
};

TEST_F(ApplyShapeMultiTest, DynamicStreamSubjectsEachGetTheirOwnTarget) {
  const auto a      = addSubject("a");
  const auto b      = addSubject("b");
  const auto result = succeeds("apply_shape(##/+d.items, {*, +d.edge: \"t\"})");
  EXPECT_EQ(result, (std::vector<CellRef>{a, b}));
  const auto ta = arena.linked(a, edge, DimVector::POS);
  const auto tb = arena.linked(b, edge, DimVector::POS);
  ASSERT_TRUE(ta && tb);
  EXPECT_NE(*ta, *tb);
  EXPECT_EQ(arena.textOf(*ta), "t");
  EXPECT_EQ(arena.linked(*ta, edge, DimVector::NEG), a);
  EXPECT_EQ(arena.linked(*tb, edge, DimVector::NEG), b);
}

TEST_F(ApplyShapeMultiTest, ReverseReverseCollisionOnASharedTarget) {
  addSubject("a");
  addSubject("b");
  engine.setVariable("target", make("shared"));
  expectCollision("apply_shape(##/+d.items, {+d.edge: $target})");
  expectCollision("apply_shape(##/+d.items, {-d.edge: $target})");
}

TEST_F(ApplyShapeMultiTest, ForwardReverseCollisionBetweenTwoSubjects) {
  const auto a = addSubject("a");
  const auto b = addSubject("b");
  attach(a, fwd, "f_a");
  attach(a, bwd, "g_a");
  attach(b, fwd, "f_b");
  // b's reverse target is a, so b claims a's forward slot while a's own rule
  // claims it for f_a.
  ASSERT_TRUE(arena.link(b, bwd, DimVector::POS, a));
  expectCollision(
      "apply_shape(##/+d.items, {+d.edge: ./+d.fwd[1], -d.edge: ./+d.bwd[1]})");
}

TEST_F(ApplyShapeMultiTest, ForwardForwardCollisionOnARepeatedSubject) {
  const auto a = addSubject("a");
  // The literal mints a fresh target per evaluation, so the one subject asks
  // for two different partners on the same slot.
  engine.setVariable("twice", std::vector<CellRef>{a, a});
  expectCollision("apply_shape($twice, {+d.edge: \"t\"})");
}

TEST_F(ApplyShapeMultiTest, DistinctSubjectsWithDistinctTargetsDoNotCollide) {
  const auto a      = addSubject("a");
  const auto b      = addSubject("b");
  const auto fa     = attach(a, fwd, "f_a");
  const auto ga     = attach(a, bwd, "g_a");
  const auto fb     = attach(b, fwd, "f_b");
  const auto gb     = attach(b, bwd, "g_b");
  const auto result = succeeds("apply_shape(##/+d.items, {*, +d.edge: "
                               "./+d.fwd[1], -d.edge: ./+d.bwd[1]})");
  EXPECT_EQ(result, (std::vector<CellRef>{a, b}));
  EXPECT_EQ(arena.linked(a, edge, DimVector::POS), fa);
  EXPECT_EQ(arena.linked(a, edge, DimVector::NEG), ga);
  EXPECT_EQ(arena.linked(b, edge, DimVector::POS), fb);
  EXPECT_EQ(arena.linked(b, edge, DimVector::NEG), gb);
  EXPECT_EQ(arena.linked(fa, edge, DimVector::NEG), a);
  EXPECT_EQ(arena.linked(gb, edge, DimVector::POS), b);
}

TEST_F(ApplyShapeMultiTest, ARepeatedSubjectWithOneTargetIsCompatible) {
  const auto a      = addSubject("a");
  const auto target = make("shared");
  engine.setVariable("twice", std::vector<CellRef>{a, a});
  engine.setVariable("target", target);
  succeeds("apply_shape($twice, {*, +d.edge: $target})");
  EXPECT_EQ(arena.linked(a, edge, DimVector::POS), target);
  EXPECT_EQ(arena.linked(target, edge, DimVector::NEG), a);
}

TEST_F(ApplyShapeMultiTest, OneDynamicSubject) {
  const auto a = addSubject("a");
  const auto result =
      succeeds("apply_shape(##/+d.items[1], {*, +d.edge: \"processed\"})");
  EXPECT_EQ(result, std::vector<CellRef>{a});
  const auto t = arena.linked(a, edge, DimVector::POS);
  ASSERT_TRUE(t);
  EXPECT_EQ(arena.textOf(*t), "processed");
}

TEST_F(ApplyShapeMultiTest, AnEmptySubjectStreamHasNoEffectAndNoError) {
  addSubject("a");
  const auto before = snapshot();
  const auto result =
      succeeds("apply_shape(##/+d.absent, {*, +d.edge: \"t\"})");
  EXPECT_TRUE(result.empty());
  EXPECT_EQ(snapshot(), before);
}

TEST_F(ApplyShapeMultiTest, ATargetOfZeroCellsFailsNamedBeforeAnyWrite) {
  addSubject("a");
  const auto b = addSubject("b");
  attach(subjects.front(), fwd, "f_a");
  (void)b; // b has no d.fwd target, so its target resolves to zero cells.
  expectNamedFailure("apply_shape(##/+d.items, {+d.edge: ./+d.fwd})",
                     "shape assignment target");
}

TEST_F(ApplyShapeMultiTest, ATargetOfTwoCellsFailsNamedBeforeAnyWrite) {
  const auto a  = addSubject("a");
  const auto fa = attach(a, fwd, "f_a");
  attach(fa, fwd, "g_a");
  addSubject("b");
  expectNamedFailure("apply_shape(##/+d.items, {+d.edge: ./+d.fwd})",
                     "shape assignment target");
}

TEST_F(ApplyShapeMultiTest, AFailureAfterTheFirstSubjectsMaskWritesNothing) {
  const auto a        = addSubject("a");
  const auto b        = addSubject("b");
  const auto original = attach(a, keep, "original");
  engine.setVariable("target", make("shared"));
  // Without the check the first subject's d.keep link would already be cut
  // when the second subject's claim is found to collide.
  expectCollision("apply_shape(##/+d.items, {+d.edge: $target})");
  EXPECT_EQ(arena.linked(a, keep, DimVector::POS), original);
  EXPECT_EQ(arena.linked(original, keep, DimVector::NEG), a);
  EXPECT_FALSE(arena.linked(b, edge, DimVector::POS));
}

TEST_F(ApplyShapeMultiTest, SubjectsGivenThroughAVariable) {
  const auto a      = addSubject("a");
  const auto b      = addSubject("b");
  const auto result = succeeds(
      "let $s := ##/+d.items return apply_shape($s, {*, +d.edge: \"t\"})");
  EXPECT_EQ(result, (std::vector<CellRef>{a, b}));
  EXPECT_TRUE(arena.linked(a, edge, DimVector::POS));
  EXPECT_TRUE(arena.linked(b, edge, DimVector::POS));
}

TEST_F(ApplyShapeMultiTest, SubjectsGivenThroughAFunction) {
  const auto a      = addSubject("a");
  const auto b      = addSubject("b");
  const auto result = succeeds("def all_items() := ##/+d.items; "
                               "apply_shape(all_items(), {*, +d.edge: \"t\"})");
  EXPECT_EQ(result, (std::vector<CellRef>{a, b}));
  EXPECT_TRUE(arena.linked(a, edge, DimVector::POS));
  EXPECT_TRUE(arena.linked(b, edge, DimVector::POS));
}

TEST_F(ApplyShapeMultiTest, SubjectsGivenThroughAForLoop) {
  const auto a      = addSubject("a");
  const auto b      = addSubject("b");
  const auto result = succeeds(
      "for $x in ##/+d.items return apply_shape($x, {*, +d.edge: \"t\"})");
  EXPECT_EQ(result, (std::vector<CellRef>{a, b}));
  EXPECT_TRUE(arena.linked(a, edge, DimVector::POS));
  EXPECT_TRUE(arena.linked(b, edge, DimVector::POS));
}

TEST_F(ApplyShapeMultiTest, SubjectsGivenByIndexing) {
  addSubject("a");
  const auto b = addSubject("b");
  const auto result =
      succeeds("apply_shape(##/+d.items[2], {*, +d.edge: \"t\"})");
  EXPECT_EQ(result, std::vector<CellRef>{b});
  EXPECT_TRUE(arena.linked(b, edge, DimVector::POS));
  EXPECT_FALSE(arena.linked(subjects.front(), edge, DimVector::POS));
}

// A successful run writes subjects that existed before it, so it keeps its
// program cells (O2c); a refused run changes nothing and keeps none. Either
// order of the two leaves the arena consistent.
TEST_F(ApplyShapeMultiTest, ARefusedRunReleasesAndALaterSuccessfulRunRetains) {
  addSubject("a");
  addSubject("b");
  engine.setVariable("target", make("shared"));
  expectCollision("apply_shape(##/+d.items, {+d.edge: $target})");
  EXPECT_FALSE(executor.programRetainedBecause());
  const auto before = snapshot();
  succeeds("apply_shape(##/+d.items, {*, +d.edge: \"t\"})");
  EXPECT_NE(snapshot(), before);
  expectCollision("apply_shape(##/+d.items, {+d.edge: $target})");
  EXPECT_FALSE(executor.programRetainedBecause());
}

TEST_F(ApplyShapeMultiTest, FiftySubjectsStayWithinTheBudget) {
  constexpr std::size_t small = 3;
  constexpr std::size_t count = 50;
  // The program is retained, so a run's growth is its program plus what it
  // minted; the difference between two sizes is the cost of the extra subjects.
  const auto grow = [&](const std::string &query) {
    const auto cellsBefore  = arena.cellCount();
    const auto cyclesBefore = vm.totalCycles();
    EXPECT_EQ(succeeds(query).size(), subjects.size());
    return std::pair{arena.cellCount() - cellsBefore,
                     vm.totalCycles() - cyclesBefore};
  };
  for (std::size_t index = 0; index < small; ++index)
    addSubject("s" + std::to_string(index));
  const auto [cellsSmall, cyclesSmall] =
      grow("apply_shape(##/+d.items, {*, +d.edge: \"t\"})");
  for (std::size_t index = small; index < count; ++index)
    addSubject("s" + std::to_string(index));
  const auto [cellsBig, cyclesBig] =
      grow("apply_shape(##/+d.items, {*, +d.edge: \"u\"})");
  std::cerr << "[apply_shape x" << count << "] cells: " << cellsBig << " (x"
            << small << ": " << cellsSmall << "), VM cycles: " << cyclesBig
            << " (x" << small << ": " << cyclesSmall << ")\n";
  // One box cell per subject, one reciprocal link each, nothing per pair.
  constexpr std::size_t cellsPerSubject = 3;
  EXPECT_LE(cellsBig - cellsSmall, (count - small) * cellsPerSubject);
  constexpr std::size_t cycleBudget =
      100'000; // QueryExecutionOptions::maxCycles
  EXPECT_LT(cyclesBig, cycleBudget);
}

} // namespace

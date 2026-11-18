/**
 * @file vortex_project_topology_test.cpp
 * @brief copyTopology and the ProjectTopology opcode behind VQL `project`: a
 * detached copy of the selected topology that shares content and quote
 * bindings, links only to itself, and leaves its source untouched.
 */
#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/arena_topology_copy.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace zigzag {
namespace {

using xanadu::vql::VQLEngine;

/// Every (dimension, direction, neighbour) of @p cell, for byte-equal
/// before/after comparison of a source.
using Edges =
    std::vector<std::tuple<DimRef, DimVector, std::optional<CellRef>>>;

Edges edgesOf(const ArenaManifold &arena, const CellRef cell) {
  Edges edges;
  for (const auto &link : arena.dimensionsOf(cell))
    for (const DimVector dir : {DimVector::POS, DimVector::NEG})
      edges.emplace_back(link.dim, dir, link.neighbor(dir));
  return edges;
}

struct Fixture {
  ArenaManifold arena;
  DimRef x{arena.makeCell()};
  DimRef y{arena.makeCell()};
  DimRef clone{arena.makeCell()};

  EdgeSelection only(std::initializer_list<DirectedDim> edges) const {
    return EdgeSelection{.everything = false, .add = edges, .remove = {}};
  }
  CellRef chain(const DimRef dim,
                std::initializer_list<std::string_view> text) {
    CellRef previous = noCell;
    CellRef first    = noCell;
    for (const auto word : text) {
      const auto cell = arena.makeCell(word);
      if (first == noCell)
        first = cell;
      else
        expectWritten(arena.link(previous, dim, DimVector::POS, cell));
      previous = cell;
    }
    return first;
  }
  std::vector<CellRef> walk(const CellRef from, const DimRef dim) const {
    std::vector<CellRef> cells{from};
    while (const auto next = arena.linked(cells.back(), dim, DimVector::POS))
      cells.push_back(*next);
    return cells;
  }
};

TEST(ProjectTopologyTest, EmptySelectionCopiesOneUnlinkedCellPerSubject) {
  Fixture f;
  const auto head   = f.chain(f.x, {"a", "b", "c"});
  const auto before = edgesOf(f.arena, head);
  const std::array subjects{head};
  const auto copied = copyTopology(f.arena, subjects, EdgeSelection{}, f.clone);
  ASSERT_TRUE(copied);
  ASSERT_EQ(copied->size(), 1U);
  const auto copy = copied->front();
  EXPECT_NE(copy, head);
  EXPECT_EQ(f.arena.textOf(copy), "a");
  EXPECT_TRUE(f.arena.dimensionsOf(copy).empty());
  EXPECT_EQ(edgesOf(f.arena, head), before);
}

TEST(ProjectTopologyTest, ARankIsCopiedUnderNewRefsAndNeverLinksBack) {
  Fixture f;
  const auto head   = f.chain(f.x, {"a", "b", "c"});
  const auto source = f.walk(head, f.x);
  const std::array subjects{head};
  const auto copied = copyTopology(
      f.arena, subjects, f.only({DirectedDim{f.x, DimVector::POS}}), f.clone);
  ASSERT_TRUE(copied);
  const auto copy = f.walk(copied->front(), f.x);
  ASSERT_EQ(copy.size(), 3U);
  for (std::size_t i = 0; i < copy.size(); ++i) {
    EXPECT_EQ(f.arena.textOf(copy[i]), f.arena.textOf(source[i]));
    EXPECT_EQ(std::ranges::count(source, copy[i]), 0);
    // The reciprocal exists inside the copy, and points at no source cell.
    if (i > 0)
      EXPECT_EQ(f.arena.linked(copy[i], f.x, DimVector::NEG), copy[i - 1]);
  }
  EXPECT_FALSE(f.arena.linked(copy.front(), f.x, DimVector::NEG));
  for (const auto cell : source)
    for (const auto &[dim, dir, neighbour] : edgesOf(f.arena, cell))
      if (neighbour) EXPECT_EQ(std::ranges::count(copy, *neighbour), 0);
}

TEST(ProjectTopologyTest, SelectionFollowsSignAndWildcardRules) {
  const DirectedDim xPos{DimRef{10}, DimVector::POS};
  const DirectedDim xNeg{DimRef{10}, DimVector::NEG};
  const DirectedDim yPos{DimRef{11}, DimVector::POS};
  const EdgeSelection all{.everything = true, .add = {}, .remove = {yPos}};
  EXPECT_TRUE(all.selects(xPos));
  EXPECT_TRUE(all.selects(xNeg));
  EXPECT_FALSE(all.selects(yPos));
  const EdgeSelection none{.everything = false, .add = {xPos}, .remove = {}};
  EXPECT_TRUE(none.selects(xPos));
  EXPECT_FALSE(none.selects(xNeg));
  EXPECT_FALSE(none.selects(yPos));
}

TEST(ProjectTopologyTest, ACycleTerminatesAndASharedCellIsCopiedOnce) {
  Fixture f;
  const auto a = f.arena.makeCell("a");
  const auto b = f.arena.makeCell("b");
  const auto c = f.arena.makeCell("c");
  // a -> b -> c -> a on x (a cycle), and a -> c on y (c reached twice).
  expectWritten(f.arena.link(a, f.x, DimVector::POS, b));
  expectWritten(f.arena.link(b, f.x, DimVector::POS, c));
  expectWritten(f.arena.link(c, f.x, DimVector::POS, a));
  expectWritten(f.arena.link(a, f.y, DimVector::POS, c));
  const auto cellsBefore = f.arena.cellCount();
  const std::array subjects{a};
  const auto copied = copyTopology(f.arena, subjects,
                                   f.only({DirectedDim{f.x, DimVector::POS},
                                           DirectedDim{f.y, DimVector::POS}}),
                                   f.clone);
  ASSERT_TRUE(copied);
  EXPECT_EQ(f.arena.cellCount(), cellsBefore + 3U);
  const auto ca = copied->front();
  const auto cb = *f.arena.linked(ca, f.x, DimVector::POS);
  const auto cc = *f.arena.linked(cb, f.x, DimVector::POS);
  EXPECT_EQ(f.arena.linked(cc, f.x, DimVector::POS), ca);
  EXPECT_EQ(f.arena.linked(ca, f.y, DimVector::POS), cc);
}

TEST(ProjectTopologyTest, ScratchTextAndTypedValuesSurviveBitExactly) {
  Fixture f;
  const auto text  = f.arena.makeCell("scratch bytes");
  const auto whole = f.arena.makeScalarCell(std::int64_t{-42});
  const auto real  = f.arena.makeScalarCell(-0.0);
  const auto flag  = f.arena.makeScalarCell(true);
  const std::array subjects{text, whole, real, flag};
  const auto copied = copyTopology(f.arena, subjects, EdgeSelection{}, f.clone);
  ASSERT_TRUE(copied);
  EXPECT_EQ(f.arena.textOf((*copied)[0]), "scratch bytes");
  EXPECT_EQ(f.arena.asInt64((*copied)[1]), std::int64_t{-42});
  ASSERT_TRUE(f.arena.asDouble((*copied)[2]));
  EXPECT_EQ(std::bit_cast<std::uint64_t>(*f.arena.asDouble((*copied)[2])),
            std::bit_cast<std::uint64_t>(*f.arena.asDouble(real)));
  EXPECT_EQ(f.arena.asBool((*copied)[3]), std::optional<bool>{true});
}

TEST(ProjectTopologyTest, ACopyTakesItsContentFromTheCloneMaster) {
  Fixture f;
  const auto master = f.arena.makeCell("master text");
  const auto clone  = f.arena.makeCell("own text");
  expectWritten(f.arena.link(master, f.clone, DimVector::POS, clone));
  const std::array subjects{clone};
  const auto copied = copyTopology(f.arena, subjects, EdgeSelection{}, f.clone);
  ASSERT_TRUE(copied);
  EXPECT_EQ(f.arena.textOf(copied->front()), "master text");
}

TEST(ProjectTopologyTest, AnUnknownSubjectIsRefusedByNameAndMintsNothing) {
  Fixture f;
  const auto before = f.arena.cellCount();
  const std::array subjects{static_cast<CellRef>(ephemeralBit | 0x00ffffffU)};
  const auto copied = copyTopology(f.arena, subjects, EdgeSelection{}, f.clone);
  ASSERT_FALSE(copied);
  EXPECT_EQ(copied.error().reason, TopologyCopyRefusal::UnknownSubject);
  EXPECT_NE(toString(copied.error()).find("not a cell"), std::string::npos);
  EXPECT_EQ(f.arena.cellCount(), before);
}

TEST(ProjectTopologyTest, AHandleValueIsRefusedByName) {
  Fixture f;
  const auto cell = f.arena.makeCell();
  expectWritten(f.arena.setValueBits(cell, xanadu::ValueKind::OpHandle, 7));
  const auto before = f.arena.cellCount();
  const std::array subjects{cell};
  const auto copied = copyTopology(f.arena, subjects, EdgeSelection{}, f.clone);
  ASSERT_FALSE(copied);
  EXPECT_EQ(copied.error().reason, TopologyCopyRefusal::HandleValue);
  EXPECT_EQ(f.arena.cellCount(), before);
}

TEST(ProjectTopologyTest, ABudgetRefusalLeavesTheSourceUntouched) {
  Fixture f;
  const auto head   = f.chain(f.x, {"a", "b", "c", "d", "e", "f"});
  const auto before = f.walk(head, f.x);
  std::vector<Edges> edges;
  for (const auto cell : before) edges.push_back(edgesOf(f.arena, cell));
  const auto cellsBefore = f.arena.cellCount();
  f.arena.setExecutionLimits(ephemeralBit, 3, 1U << 20);
  const std::array subjects{head};
  EXPECT_THROW(std::ignore = copyTopology(
                   f.arena, subjects,
                   f.only({DirectedDim{f.x, DimVector::POS}}), f.clone),
               std::runtime_error);
  EXPECT_EQ(f.arena.cellCount(), cellsBefore);
  for (std::size_t i = 0; i < before.size(); ++i)
    EXPECT_EQ(edgesOf(f.arena, before[i]), edges[i]);
}

TEST(ProjectTopologyTest, AllocationLimitIsRefusedBeforeAnythingIsMinted) {
  Fixture f;
  const auto head = f.chain(f.x, {"a", "b", "c"});
  f.arena.setAllocationLimitForTesting(
      static_cast<std::uint32_t>(f.arena.cellCount() + 2));
  const auto before = f.arena.cellCount();
  const std::array subjects{head};
  const auto copied = copyTopology(
      f.arena, subjects, f.only({DirectedDim{f.x, DimVector::POS}}), f.clone);
  ASSERT_FALSE(copied);
  EXPECT_EQ(copied.error().reason, TopologyCopyRefusal::AllocationLimit);
  EXPECT_EQ(f.arena.cellCount(), before);
}

// -- through VQL: the opcode, its operands, and identity ---------------------

struct Query {
  ArenaManifold arena;
  vortex::VortexCore core{arena};
  VQLEngine engine{core};
};

TEST(ProjectTopologyOpcodeTest, WildcardRulesFoldInOrder) {
  Query q;
  const auto subject = q.arena.makeCell("s");
  const auto foo     = q.engine.resolveDimension("d.foo");
  const auto bar     = q.engine.resolveDimension("d.bar");
  const auto a       = q.arena.makeCell("a");
  const auto b       = q.arena.makeCell("b");
  expectWritten(q.arena.link(subject, foo, DimVector::POS, a));
  expectWritten(q.arena.link(subject, bar, DimVector::POS, b));
  q.engine.setVariable("s", subject);
  const auto edgesBefore = edgesOf(q.arena, subject);

  const auto everythingButBar = q.engine.execute("project($s, {*, !d.bar})");
  ASSERT_EQ(everythingButBar.size(), 1U);
  EXPECT_TRUE(q.arena.linked(everythingButBar.front(), foo, DimVector::POS));
  EXPECT_FALSE(q.arena.linked(everythingButBar.front(), bar, DimVector::POS));

  const auto resetMidway =
      q.engine.execute("project($s, {+d.foo, !*, +d.bar})");
  ASSERT_EQ(resetMidway.size(), 1U);
  EXPECT_FALSE(q.arena.linked(resetMidway.front(), foo, DimVector::POS));
  EXPECT_TRUE(q.arena.linked(resetMidway.front(), bar, DimVector::POS));

  const auto laterRuleWins = q.engine.execute("project($s, {+d.foo, !d.foo})");
  ASSERT_EQ(laterRuleWins.size(), 1U);
  EXPECT_FALSE(q.arena.linked(laterRuleWins.front(), foo, DimVector::POS));

  const auto bothSigns = q.engine.execute("project($s, {!*, ~d.foo})");
  ASSERT_EQ(bothSigns.size(), 1U);
  EXPECT_TRUE(q.arena.linked(bothSigns.front(), foo, DimVector::POS));
  EXPECT_FALSE(q.arena.linked(bothSigns.front(), bar, DimVector::POS));
  EXPECT_EQ(edgesOf(q.arena, subject), edgesBefore);
}

TEST(ProjectTopologyOpcodeTest, TargetAssignmentsRefuseNamingProject) {
  Query q;
  q.arena.makeCell("s");
  q.engine.setVariable("s", q.arena.makeCell("s"));
  try {
    q.engine.execute("project($s, {+d.foo: $s})");
    FAIL() << "project with an assignment must refuse";
  } catch (const std::exception &error) {
    EXPECT_NE(std::string{error.what()}.find("project"), std::string::npos);
  }
}

TEST(ProjectTopologyOpcodeTest, TheCopyIsADistinctIdentityFromItsSource) {
  Query q;
  const auto subject = q.arena.makeCell("s");
  q.engine.setVariable("s", subject);
  const auto copy = q.engine.execute("project($s, {})");
  ASSERT_EQ(copy.size(), 1U);
  q.engine.setVariable("c", copy);
  const auto differs = q.engine.execute("same_cell($s, $c)");
  ASSERT_EQ(differs.size(), 1U);
  EXPECT_EQ(q.core.isTruthy(differs.front()), std::optional<bool>{false});
  const auto itself = q.engine.execute("same_cell($c, $c)");
  ASSERT_EQ(itself.size(), 1U);
  EXPECT_EQ(q.core.isTruthy(itself.front()), std::optional<bool>{true});
}

TEST(ProjectTopologyOpcodeTest, AnIsolatedCopyAnswersWithoutTouchingHome) {
  Query q;
  const auto result = q.engine.execute(
      "let $isolated := project(##, {}) where same_cell($isolated, ##) = false "
      "return ##");
  ASSERT_EQ(result.size(), 1U);
  EXPECT_EQ(result.front(), q.core.home());
}

TEST(ProjectTopologyOpcodeTest, AQuoteCopyKeepsItsSpansAndSpace) {
  const auto directory =
      std::filesystem::temp_directory_path() / "project_topology_quote_test";
  std::filesystem::remove_all(directory);
  {
    xanadu::UserPermascroll::Config config;
    config.storageDir  = directory;
    auto scroll        = std::make_shared<xanadu::UserPermascroll>(config);
    auto store         = std::make_shared<xanadu::Store>(scroll);
    const auto genesis = store->sliceGenesis(xanadu::MicroversionId{});
    const auto dim     = store->makeDimension(genesis, "d.text");
    const auto cell    = store->makeCell(dim.version, "quoted bytes");
    const auto linked = store->setLink(cell, store->homeCell().value(), dim.dim,
                                       false, store->cellRefOf(cell).value());
    xanadu::MultiStoreCoordinator coordinator;
    coordinator.addStore("source", "primary", store, linked);
    VQLEngine engine(coordinator);
    const auto original = engine.execute("##source/+d.text");
    ASSERT_EQ(original.size(), 1U);
    const auto copied = engine.execute("project(##source/+d.text, {})");
    ASSERT_EQ(copied.size(), 1U);
    const auto &arena = coordinator.arena();
    const auto theirs = arena.quotedContent(original.front());
    const auto ours   = arena.quotedContent(copied.front());
    ASSERT_TRUE(theirs);
    ASSERT_TRUE(ours);
    EXPECT_EQ(ours->store, theirs->store);
    EXPECT_EQ(ours->spans, theirs->spans);
    EXPECT_EQ(arena.textOf(copied.front()), "quoted bytes");
  }
  std::filesystem::remove_all(directory);
}

} // namespace
} // namespace zigzag

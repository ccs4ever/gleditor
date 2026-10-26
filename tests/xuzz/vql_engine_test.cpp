/**
 * @file vql_engine_test.cpp
 * @brief Unit tests for VQL Direct Evaluator (VQLEngine).
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vql;
using xanadu::MultiStoreCoordinator;
using zigzag::DimVector;

TEST(VQLEngineTest, BarePathNavigation) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::DimRef dPeople = engine.resolveDimension("d.people");
  zigzag::CellRef alice  = arena.makeCell("Alice");
  zigzag::CellRef bob    = arena.makeCell("Bob");

  EXPECT_TRUE(arena.link(core.home(), dPeople, DimVector::POS, alice));
  EXPECT_TRUE(arena.link(alice, dPeople, DimVector::POS, bob));

  auto results = engine.execute("##/d.people");
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0], alice);
  EXPECT_EQ(results[1], bob);
}

TEST(VQLEngineTest, MultiStoreAndNamedStoreSugar) {
  MultiStoreCoordinator coordinator;
  auto &arena = coordinator.core().arena();

  // Mint slice 1: users
  zigzag::CellRef usersHome = arena.makeCell("users_home");
  zigzag::CellRef uAlice    = arena.makeCell("Alice");
  zigzag::DimRef dUsers     = coordinator.core().mintDimension("d.users");
  EXPECT_TRUE(arena.link(usersHome, dUsers, DimVector::POS, uAlice));

  // Mint slice 2: products
  zigzag::CellRef prodHome = arena.makeCell("prod_home");
  zigzag::CellRef pBook    = arena.makeCell("HypertextBook");
  zigzag::DimRef dProds    = coordinator.core().mintDimension("d.items");
  EXPECT_TRUE(arena.link(prodHome, dProds, DimVector::POS, pBook));

  coordinator.addSlice("users", "data", usersHome);
  coordinator.addSlice("products", "catalog", prodHome);

  VQLEngine engine(coordinator);

  // ##users should desugar to ##/d.stores>[d.name = "users"]
  auto resUsers = engine.execute("##users/d.users");
  ASSERT_EQ(resUsers.size(), 1u);
  EXPECT_EQ(resUsers[0], uAlice);

  auto resProds = engine.execute("##products/d.items");
  ASSERT_EQ(resProds.size(), 1u);
  EXPECT_EQ(resProds[0], pBook);
}

TEST(VQLEngineTest, UniversalDerefMaster) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::DimRef dClone = core.dims().clone;

  zigzag::CellRef master = arena.makeCell("MasterDoc");
  zigzag::CellRef clone1 = arena.makeCell("Clone1");
  zigzag::CellRef clone2 = arena.makeCell("Clone2");

  EXPECT_TRUE(arena.link(master, dClone, DimVector::POS, clone1));
  EXPECT_TRUE(arena.link(clone1, dClone, DimVector::NEG, master));

  EXPECT_TRUE(arena.link(clone1, dClone, DimVector::POS, clone2));
  EXPECT_TRUE(arena.link(clone2, dClone, DimVector::NEG, clone1));

  engine.setVariable("c2", clone2);

  // $c2> should seek negward to master
  auto results = engine.execute("$c2>");
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0], master);
}

TEST(VQLEngineTest, CreationSugarAndYieldModes) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  // 1. Create with !new (default)
  auto res1 = engine.execute("##/d.fruits%\"Apple\"%\"Banana\"");
  ASSERT_EQ(res1.size(), 2u);
  EXPECT_EQ(arena.textOf(res1[0]), "Apple");
  EXPECT_EQ(arena.textOf(res1[1]), "Banana");

  // Verify they are linked on d.fruits off home
  zigzag::DimRef dFruits = engine.resolveDimension("d.fruits");
  const auto first       = arena.linked(core.home(), dFruits, DimVector::POS);
  ASSERT_TRUE(first);
  EXPECT_EQ(*first, res1[0]);
  const auto second = arena.linked(*first, dFruits, DimVector::POS);
  ASSERT_TRUE(second);
  EXPECT_EQ(*second, res1[1]);

  // 2. Create with !keep
  auto res2 = engine.execute("##/d.colors%\"Red\"!");
  ASSERT_EQ(res2.size(), 1u);
  EXPECT_EQ(res2[0], core.home());

  // 3. Create with !last
  auto res3 = engine.execute("##/d.cities%\"Paris\"%\"Tokyo\"!last");
  ASSERT_EQ(res3.size(), 1u);
  EXPECT_EQ(arena.textOf(res3[0]), "Tokyo");

  auto res4 = engine.execute("##/d.seasons%\"Spring\"%\"Summer\"!both");
  ASSERT_EQ(res4.size(), 3u);
  EXPECT_EQ(res4[0], core.home());
  EXPECT_EQ(arena.textOf(res4[1]), "Spring");
  EXPECT_EQ(arena.textOf(res4[2]), "Summer");
}

TEST(VQLEngineTest, LinkYieldBothPreservesContextsBeforeTargetClones) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  const zigzag::CellRef first  = arena.makeCell("First");
  const zigzag::CellRef second = arena.makeCell("Second");
  const zigzag::CellRef target = arena.makeCell("Target");
  engine.setVariable("contexts", std::vector<zigzag::CellRef>{first, second});
  engine.setVariable("target", target);

  const auto result = engine.execute("$contexts/link(+d.fanout, $target)!both");
  ASSERT_EQ(result.size(), 4u);
  EXPECT_EQ(result[0], first);
  EXPECT_EQ(result[1], second);
  EXPECT_NE(result[2], target);
  EXPECT_NE(result[3], target);
  EXPECT_NE(result[2], result[3]);
  EXPECT_EQ(core.render(result[2]), core.render(target));
  EXPECT_EQ(core.render(result[3]), core.render(target));

  const zigzag::DimRef dim = engine.resolveDimension("d.fanout");
  EXPECT_EQ(arena.linked(first, dim, DimVector::POS), result[2]);
  EXPECT_EQ(arena.linked(second, dim, DimVector::POS), result[3]);
}

TEST(VQLEngineTest, PredicatesAndQuantifiers) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::DimRef dNums = engine.resolveDimension("d.numbers");

  zigzag::CellRef c10 = arena.makeScalarCell(static_cast<std::int64_t>(10));
  zigzag::CellRef c25 = arena.makeScalarCell(static_cast<std::int64_t>(25));
  zigzag::CellRef c40 = arena.makeScalarCell(static_cast<std::int64_t>(40));
  zigzag::CellRef c50 = arena.makeScalarCell(static_cast<std::int64_t>(50));

  EXPECT_TRUE(arena.link(core.home(), dNums, DimVector::POS, c10));
  EXPECT_TRUE(arena.link(c10, dNums, DimVector::POS, c25));
  EXPECT_TRUE(arena.link(c25, dNums, DimVector::POS, c40));
  EXPECT_TRUE(arena.link(c40, dNums, DimVector::POS, c50));

  // Filter: [. > 20 and . < 45]
  auto results = engine.execute("##/d.numbers[. > 20 and . < 45]");
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0], c25);
  EXPECT_EQ(results[1], c40);

  // Equality filter: [. = 50]
  auto resEq = engine.execute("##/d.numbers[. = 50]");
  ASSERT_EQ(resEq.size(), 1u);
  EXPECT_EQ(resEq[0], c50);
}

TEST(VQLEngineTest, MissingComparisonOperandIsFalseBeforeFactorNegation) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);
  const auto dim  = engine.resolveDimension("d.missing_operand");
  const auto cell = arena.makeScalarCell(std::int64_t{0});
  ASSERT_TRUE(arena.link(core.home(), dim, DimVector::POS, cell));
  engine.setVariable("unset", std::vector<zigzag::CellRef>{});

  EXPECT_TRUE(engine.execute("##/d.missing_operand[$unset != false]").empty());
  const auto negated =
      engine.execute("##/d.missing_operand[not $unset != false]");
  ASSERT_EQ(negated.size(), 1U);
  EXPECT_EQ(negated.front(), cell);
}

TEST(VQLEngineTest, RangeClamps) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::DimRef dItems = engine.resolveDimension("d.items");
  zigzag::CellRef a     = arena.makeCell("A");
  zigzag::CellRef b     = arena.makeCell("B");
  zigzag::CellRef c     = arena.makeCell("C");
  zigzag::CellRef d     = arena.makeCell("D");

  EXPECT_TRUE(arena.link(core.home(), dItems, DimVector::POS, a));
  EXPECT_TRUE(arena.link(a, dItems, DimVector::POS, b));
  EXPECT_TRUE(arena.link(b, dItems, DimVector::POS, c));
  EXPECT_TRUE(arena.link(c, dItems, DimVector::POS, d));

  // [1]: First
  auto rFirst = engine.execute("##/d.items[1]");
  ASSERT_EQ(rFirst.size(), 1u);
  EXPECT_EQ(rFirst[0], a);

  // [-1]: Last
  auto rLast = engine.execute("##/d.items[-1]");
  ASSERT_EQ(rLast.size(), 1u);
  EXPECT_EQ(rLast[0], d);

  // [2, 3]: Slice
  auto rSlice = engine.execute("##/d.items[2, 3]");
  ASSERT_EQ(rSlice.size(), 2u);
  EXPECT_EQ(rSlice[0], b);
  EXPECT_EQ(rSlice[1], c);
}

TEST(VQLEngineTest, FLWORQueryExecution) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::DimRef dPeople = engine.resolveDimension("d.people");
  zigzag::DimRef dAge    = engine.resolveDimension("d.age");
  zigzag::DimRef dName   = engine.resolveDimension("d.name");

  // Person 1: Alice, 30
  zigzag::CellRef p1  = arena.makeCell("person1");
  zigzag::CellRef p1a = arena.makeScalarCell(static_cast<std::int64_t>(30));
  zigzag::CellRef p1n = arena.makeCell("Alice");
  EXPECT_TRUE(arena.link(p1, dAge, DimVector::POS, p1a));
  EXPECT_TRUE(arena.link(p1, dName, DimVector::POS, p1n));

  // Person 2: Bob, 17
  zigzag::CellRef p2  = arena.makeCell("person2");
  zigzag::CellRef p2a = arena.makeScalarCell(static_cast<std::int64_t>(17));
  zigzag::CellRef p2n = arena.makeCell("Bob");
  EXPECT_TRUE(arena.link(p2, dAge, DimVector::POS, p2a));
  EXPECT_TRUE(arena.link(p2, dName, DimVector::POS, p2n));

  // Person 3: Charlie, 45
  zigzag::CellRef p3  = arena.makeCell("person3");
  zigzag::CellRef p3a = arena.makeScalarCell(static_cast<std::int64_t>(45));
  zigzag::CellRef p3n = arena.makeCell("Charlie");
  EXPECT_TRUE(arena.link(p3, dAge, DimVector::POS, p3a));
  EXPECT_TRUE(arena.link(p3, dName, DimVector::POS, p3n));

  EXPECT_TRUE(arena.link(core.home(), dPeople, DimVector::POS, p1));
  EXPECT_TRUE(arena.link(p1, dPeople, DimVector::POS, p2));
  EXPECT_TRUE(arena.link(p2, dPeople, DimVector::POS, p3));

  // for $p in ##/d.people where $p/d.age >= 18 return $p/d.name
  auto results = engine.execute(
      "for $p in ##/d.people where $p/d.age >= 18 return $p/d.name");
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(arena.textOf(results[0]), "Alice");
  EXPECT_EQ(arena.textOf(results[1]), "Charlie");
}

TEST(VQLEngineTest, WeaveEffectBlock) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  engine.execute("weave { ##/d.tasks%\"Task1\"%\"Task2\" }");

  zigzag::DimRef dTasks = engine.resolveDimension("d.tasks");
  const auto t1         = arena.linked(core.home(), dTasks, DimVector::POS);
  ASSERT_TRUE(t1);
  EXPECT_EQ(arena.textOf(*t1), "Task1");

  const auto t2 = arena.linked(*t1, dTasks, DimVector::POS);
  ASSERT_TRUE(t2);
  EXPECT_EQ(arena.textOf(*t2), "Task2");
}

TEST(VQLEngineTest, SlicingSugar) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::CellRef header = arena.makeCell("XanaduOSMICDoc");
  engine.setVariable("h", header);

  // $h.[0, 6] should return ephemeral slice cell "Xanadu"
  auto res = engine.execute("$h.[0, 6]");
  ASSERT_EQ(res.size(), 1u);
  EXPECT_EQ(arena.textOf(res[0]), "Xanadu");
}

TEST(VQLEngineTest, ExtendedShapePredicates) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::DimRef dItems = engine.resolveDimension("d.items");
  zigzag::DimRef dTag   = engine.resolveDimension("d.tag");
  zigzag::DimRef dArch  = engine.resolveDimension("d.archived");

  zigzag::CellRef item1 = arena.makeCell("Item1");
  zigzag::CellRef tag1  = arena.makeCell("urgent");
  EXPECT_TRUE(arena.link(item1, dTag, DimVector::POS, tag1));

  zigzag::CellRef item2 = arena.makeCell("Item2");
  zigzag::CellRef tag2  = arena.makeCell("low");
  EXPECT_TRUE(arena.link(item2, dTag, DimVector::POS, tag2));

  zigzag::CellRef item3 = arena.makeCell("Item3");
  zigzag::CellRef tag3  = arena.makeCell("urgent");
  zigzag::CellRef arch3 = arena.makeCell("true");
  EXPECT_TRUE(arena.link(item3, dTag, DimVector::POS, tag3));
  EXPECT_TRUE(arena.link(item3, dArch, DimVector::POS, arch3));

  EXPECT_TRUE(arena.link(core.home(), dItems, DimVector::POS, item1));
  EXPECT_TRUE(arena.link(item1, dItems, DimVector::POS, item2));
  EXPECT_TRUE(arena.link(item2, dItems, DimVector::POS, item3));

  // Match urgent items: ##/d.items[{ +d.tag: "urgent" }]
  auto urgentItems = engine.execute("##/d.items[{ +d.tag: \"urgent\" }]");
  ASSERT_EQ(urgentItems.size(), 2u);
  EXPECT_EQ(urgentItems[0], item1);
  EXPECT_EQ(urgentItems[1], item3);

  // Match urgent AND not archived: ##/d.items[{ +d.tag: "urgent", !d.archived
  // }]
  auto activeUrgent =
      engine.execute("##/d.items[{ +d.tag: \"urgent\", !d.archived }]");
  ASSERT_EQ(activeUrgent.size(), 1u);
  EXPECT_EQ(activeUrgent[0], item1);
}

TEST(VQLEngineTest, BidirectionalTraversal) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::DimRef dTag         = engine.resolveDimension("d.tag");
  zigzag::CellRef center      = arena.makeCell("Center");
  zigzag::CellRef posNeighbor = arena.makeCell("PosNeighbor");
  zigzag::CellRef negNeighbor = arena.makeCell("NegNeighbor");

  EXPECT_TRUE(arena.link(center, dTag, DimVector::POS, posNeighbor));
  EXPECT_TRUE(arena.link(center, dTag, DimVector::NEG, negNeighbor));

  engine.setVariable("c", center);

  // Bidirectional step: $c/~d.tag
  auto results = engine.execute("$c/~d.tag");
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0], posNeighbor);
  EXPECT_EQ(results[1], negNeighbor);
}

TEST(VQLEngineTest, DestructuringInForClause) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::DimRef dPeople = engine.resolveDimension("d.people");
  zigzag::DimRef dTag    = engine.resolveDimension("d.tag");

  zigzag::CellRef p1 = arena.makeCell("Person1");
  zigzag::CellRef t1 = arena.makeCell("developer");
  EXPECT_TRUE(arena.link(p1, dTag, DimVector::POS, t1));

  zigzag::CellRef p2 = arena.makeCell("Person2");
  zigzag::CellRef t2 = arena.makeCell("designer");
  EXPECT_TRUE(arena.link(p2, dTag, DimVector::POS, t2));

  EXPECT_TRUE(arena.link(core.home(), dPeople, DimVector::POS, p1));
  EXPECT_TRUE(arena.link(p1, dPeople, DimVector::POS, p2));

  // for { ., +d.tag: $t } in ##/d.people return $t
  auto results =
      engine.execute("for { ., +d.tag: $t } in ##/d.people return $t");
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(arena.textOf(results[0]), "developer");
  EXPECT_EQ(arena.textOf(results[1]), "designer");
}

TEST(VQLEngineTest, SproutMorphism) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::CellRef root = arena.makeCell("Root");
  engine.setVariable("r", root);

  auto sprouts = engine.execute("$r/=> [ \"Alpha\", \"Beta\" ]");
  ASSERT_EQ(sprouts.size(), 2u);
  EXPECT_EQ(arena.textOf(sprouts[0]), "Alpha");
  EXPECT_EQ(arena.textOf(sprouts[1]), "Beta");
}

TEST(VQLEngineTest, RootStepsOnSlices) {
  MultiStoreCoordinator coordinator;
  auto &arena = coordinator.core().arena();

  zigzag::CellRef workerHome = arena.makeCell("worker_home");
  zigzag::DimRef dCursors    = coordinator.core().dims().cursors;

  zigzag::CellRef defaultCursor = arena.makeCell("default");
  zigzag::CellRef workerCursor  = arena.makeCell("worker");

  EXPECT_TRUE(arena.link(workerHome, dCursors, DimVector::POS, defaultCursor));
  EXPECT_TRUE(
      arena.link(defaultCursor, dCursors, DimVector::POS, workerCursor));

  coordinator.addSlice("worker_slice", "worker", workerHome);

  VQLEngine engine(coordinator);
  engine.setVariable("slice", workerHome);

  // 1. /$slice/## -> workerHome
  auto genesisRes = engine.execute("/$slice/##");
  ASSERT_EQ(genesisRes.size(), 1u);
  EXPECT_EQ(genesisRes[0], workerHome);

  // 2. /$slice/^ -> defaultCursor
  auto activeCursorRes = engine.execute("/$slice/^");
  ASSERT_EQ(activeCursorRes.size(), 1u);
  EXPECT_EQ(activeCursorRes[0], defaultCursor);

  // 3. /$slice/^worker -> workerCursor
  auto namedCursorRes = engine.execute("/$slice/^worker");
  ASSERT_EQ(namedCursorRes.size(), 1u);
  EXPECT_EQ(namedCursorRes[0], workerCursor);
}

} // namespace

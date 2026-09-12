/**
 * @file vql_engine_test.cpp
 * @brief Unit tests for VQL Direct Evaluator (VQLEngine).
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "common/xanadu/vql/multi_store.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vql;
using zigzag::DimVector;

TEST(VQLEngineTest, BarePathNavigation) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::DimRef dPeople = engine.resolveDimension("d.people");
  zigzag::CellRef alice  = arena.makeCell("Alice");
  zigzag::CellRef bob    = arena.makeCell("Bob");

  arena.link(core.home(), dPeople, DimVector::POS, alice);
  arena.link(alice, dPeople, DimVector::POS, bob);

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
  arena.link(usersHome, dUsers, DimVector::POS, uAlice);

  // Mint slice 2: products
  zigzag::CellRef prodHome = arena.makeCell("prod_home");
  zigzag::CellRef pBook    = arena.makeCell("HypertextBook");
  zigzag::DimRef dProds    = coordinator.core().mintDimension("d.items");
  arena.link(prodHome, dProds, DimVector::POS, pBook);

  coordinator.addSlice("users", "data", usersHome);
  coordinator.addSlice("products", "catalog", prodHome);

  VQLEngine engine(coordinator);

  // ##users should desugar to ##/d.stores>[d.name == "users"]
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

  arena.link(master, dClone, DimVector::POS, clone1);
  arena.link(clone1, dClone, DimVector::NEG, master);

  arena.link(clone1, dClone, DimVector::POS, clone2);
  arena.link(clone2, dClone, DimVector::NEG, clone1);

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
  zigzag::CellRef first  = arena.linked(core.home(), dFruits, DimVector::POS);
  EXPECT_EQ(first, res1[0]);
  zigzag::CellRef second = arena.linked(first, dFruits, DimVector::POS);
  EXPECT_EQ(second, res1[1]);

  // 2. Create with !keep
  auto res2 = engine.execute("##/d.colors%\"Red\"!");
  ASSERT_EQ(res2.size(), 1u);
  EXPECT_EQ(res2[0], core.home());

  // 3. Create with !last
  auto res3 = engine.execute("##/d.cities%\"Paris\"%\"Tokyo\"!last");
  ASSERT_EQ(res3.size(), 1u);
  EXPECT_EQ(arena.textOf(res3[0]), "Tokyo");
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

  arena.link(core.home(), dNums, DimVector::POS, c10);
  arena.link(c10, dNums, DimVector::POS, c25);
  arena.link(c25, dNums, DimVector::POS, c40);
  arena.link(c40, dNums, DimVector::POS, c50);

  // Filter: [. > 20 and . < 45]
  auto results = engine.execute("##/d.numbers[. > 20 and . < 45]");
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0], c25);
  EXPECT_EQ(results[1], c40);

  // Equality filter: [. == 50]
  auto resEq = engine.execute("##/d.numbers[. == 50]");
  ASSERT_EQ(resEq.size(), 1u);
  EXPECT_EQ(resEq[0], c50);
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

  arena.link(core.home(), dItems, DimVector::POS, a);
  arena.link(a, dItems, DimVector::POS, b);
  arena.link(b, dItems, DimVector::POS, c);
  arena.link(c, dItems, DimVector::POS, d);

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
  arena.link(p1, dAge, DimVector::POS, p1a);
  arena.link(p1, dName, DimVector::POS, p1n);

  // Person 2: Bob, 17
  zigzag::CellRef p2  = arena.makeCell("person2");
  zigzag::CellRef p2a = arena.makeScalarCell(static_cast<std::int64_t>(17));
  zigzag::CellRef p2n = arena.makeCell("Bob");
  arena.link(p2, dAge, DimVector::POS, p2a);
  arena.link(p2, dName, DimVector::POS, p2n);

  // Person 3: Charlie, 45
  zigzag::CellRef p3  = arena.makeCell("person3");
  zigzag::CellRef p3a = arena.makeScalarCell(static_cast<std::int64_t>(45));
  zigzag::CellRef p3n = arena.makeCell("Charlie");
  arena.link(p3, dAge, DimVector::POS, p3a);
  arena.link(p3, dName, DimVector::POS, p3n);

  arena.link(core.home(), dPeople, DimVector::POS, p1);
  arena.link(p1, dPeople, DimVector::POS, p2);
  arena.link(p2, dPeople, DimVector::POS, p3);

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
  zigzag::CellRef t1    = arena.linked(core.home(), dTasks, DimVector::POS);
  ASSERT_NE(t1, zigzag::noCell);
  EXPECT_EQ(arena.textOf(t1), "Task1");

  zigzag::CellRef t2 = arena.linked(t1, dTasks, DimVector::POS);
  ASSERT_NE(t2, zigzag::noCell);
  EXPECT_EQ(arena.textOf(t2), "Task2");
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

} // namespace

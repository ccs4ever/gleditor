/**
 * @file vql_multi_store_test.cpp
 * @brief Unit tests for Stage 0 MultiStoreCoordinator: d.stores rank,
 *        clone master dereference '>', and '##NAME' store selection shorthand.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vql/multi_store.hpp"

namespace {

namespace fs = std::filesystem;
using xanadu::Store;
using xanadu::UserPermascroll;
using xanadu::vql::MultiStoreCoordinator;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::noCell;

fs::path tempStoreDir(const std::string &name) {
  const auto dir = fs::temp_directory_path() / ("vql_store_test_" + name);
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

TEST(VQLMultiStoreTest, CoordinatorGenesis) {
  MultiStoreCoordinator coord;

  EXPECT_NE(coord.coordinatorHome(), noCell);
  EXPECT_NE(coord.dimStores(), noCell);
  EXPECT_NE(coord.dimClone(), noCell);
  EXPECT_NE(coord.dimName(), noCell);
  EXPECT_NE(coord.dimRole(), noCell);
  EXPECT_EQ(coord.storeCount(), 0u);
}

TEST(VQLMultiStoreTest, SingleSliceRegistration) {
  MultiStoreCoordinator coord;
  auto &arena = coord.arena();

  CellRef mySliceHome = arena.makeCell("home_slice_1");
  CellRef storeCell   = coord.addSlice("primary_slice", "primary", mySliceHome);

  EXPECT_EQ(coord.storeCount(), 1u);
  EXPECT_EQ(coord.homeAnchor(), mySliceHome);
  EXPECT_NE(storeCell, noCell);

  // The representative cell on d.stores clones mySliceHome
  EXPECT_EQ(coord.derefCloneMaster(storeCell), mySliceHome);

  // Resolves via ##NAME shorthand
  EXPECT_EQ(coord.resolveNamedStore("primary_slice"), mySliceHome);
  EXPECT_EQ(coord.resolveNamedStore("non_existent"), noCell);
}

TEST(VQLMultiStoreTest, MultiSliceTopologyAndDerefMaster) {
  MultiStoreCoordinator coord;
  auto &arena = coord.arena();

  // Create three slice homes in the arena
  CellRef homeUsers = arena.makeCell("users_home");
  CellRef homeMath  = arena.makeCell("math_home");
  CellRef homeGeo   = arena.makeCell("geo_home");

  CellRef storeUsers = coord.addSlice("users", "data", homeUsers);
  CellRef storeMath  = coord.addSlice("math", "library", homeMath);
  CellRef storeGeo   = coord.addSlice("geo", "library", homeGeo);

  EXPECT_EQ(coord.storeCount(), 3u);
  EXPECT_EQ(coord.coordinatorHome(), coord.homeAnchor());

  // Verify d.stores rank posward walk: coordHome -> storeUsers -> storeMath ->
  // storeGeo
  CellRef step1 =
      arena.linked(coord.coordinatorHome(), coord.dimStores(), false);
  EXPECT_EQ(step1, storeUsers);

  CellRef step2 = arena.linked(step1, coord.dimStores(), false);
  EXPECT_EQ(step2, storeMath);

  CellRef step3 = arena.linked(step2, coord.dimStores(), false);
  EXPECT_EQ(step3, storeGeo);

  EXPECT_EQ(arena.linked(step3, coord.dimStores(), false), noCell);

  // Test the universal '>' clone master dereference on each storeCell
  EXPECT_EQ(coord.derefCloneMaster(storeUsers), homeUsers);
  EXPECT_EQ(coord.derefCloneMaster(storeMath), homeMath);
  EXPECT_EQ(coord.derefCloneMaster(storeGeo), homeGeo);

  // Verify metadata on slice home cells
  CellRef nameUsers = arena.linked(homeUsers, coord.dimName(), false);
  EXPECT_NE(nameUsers, noCell);
  EXPECT_EQ(arena.textOf(nameUsers), "users");

  CellRef roleUsers = arena.linked(homeUsers, coord.dimRole(), false);
  EXPECT_NE(roleUsers, noCell);
  EXPECT_EQ(arena.textOf(roleUsers), "data");

  CellRef nameMath = arena.linked(homeMath, coord.dimName(), false);
  EXPECT_NE(nameMath, noCell);
  EXPECT_EQ(arena.textOf(nameMath), "math");

  // Test ##NAME shorthand resolution
  EXPECT_EQ(coord.resolveNamedStore("users"), homeUsers);
  EXPECT_EQ(coord.resolveNamedStore("math"), homeMath);
  EXPECT_EQ(coord.resolveNamedStore("geo"), homeGeo);
  EXPECT_EQ(coord.resolveNamedStore("missing"), noCell);
}

TEST(VQLMultiStoreTest, UniversalCloneMasterDereferenceChain) {
  MultiStoreCoordinator coord;
  auto &arena = coord.arena();

  // Create chain: C >< B >< A where A is master
  CellRef cellA = arena.makeCell("MasterA");
  CellRef cellB = arena.makeCell();
  CellRef cellC = arena.makeCell();

  // Link B to A along d.clone: A pos to B, B neg to A
  arena.link(cellA, coord.dimClone(), DimVector::POS, cellB);
  arena.link(cellB, coord.dimClone(), DimVector::NEG, cellA);

  // Link C to B along d.clone: B pos to C, C neg to B
  arena.link(cellB, coord.dimClone(), DimVector::POS, cellC);
  arena.link(cellC, coord.dimClone(), DimVector::NEG, cellB);

  // Dereferencing any cell in the chain via '>' lands on Master A
  EXPECT_EQ(coord.derefCloneMaster(cellA), cellA);
  EXPECT_EQ(coord.derefCloneMaster(cellB), cellA);
  EXPECT_EQ(coord.derefCloneMaster(cellC), cellA);
}

TEST(VQLMultiStoreTest, StoreImportAndCrossStoreNavigation) {
  const auto dirA = tempStoreDir("store_a");
  const auto dirB = tempStoreDir("store_b");

  UserPermascroll::Config configA;
  configA.storageDir = dirA / "permascroll";
  auto scrollA       = std::make_shared<UserPermascroll>(configA);
  auto storeA        = std::make_shared<Store>(scrollA);
  const auto vA1     = storeA->sliceGenesis(xanadu::MicroversionId{});

  // Add custom dimension and cell to Store A
  auto dimStep = storeA->makeDimension(vA1, "d.step");
  auto cellA1  = storeA->makeCell(dimStep.version, "hello_from_A");
  auto vA2     = storeA->setLink(cellA1, storeA->homeCell(), dimStep.dim, false,
                                 storeA->cellRefOf(cellA1));

  UserPermascroll::Config configB;
  configB.storageDir = dirB / "permascroll";
  auto scrollB       = std::make_shared<UserPermascroll>(configB);
  auto storeB        = std::make_shared<Store>(scrollB);
  const auto vB1     = storeB->sliceGenesis(xanadu::MicroversionId{});

  MultiStoreCoordinator coord;
  coord.addStore("docA", "primary", storeA, vA2);
  coord.addStore("docB", "library", storeB, vB1);

  EXPECT_EQ(coord.storeCount(), 2u);

  CellRef resolvedA = coord.resolveNamedStore("docA");
  CellRef resolvedB = coord.resolveNamedStore("docB");

  EXPECT_NE(resolvedA, noCell);
  EXPECT_NE(resolvedB, noCell);
  EXPECT_NE(resolvedA, resolvedB);

  // Verify that Store A's structure is preserved in the composite arena
  // Find d.step on resolvedA
  auto &arena    = coord.arena();
  bool foundStep = false;
  for (const auto &dimLink : arena.dimensionsOf(resolvedA)) {
    CellRef next = dimLink.pos;
    if (next != noCell) {
      foundStep = true;
    }
  }
  EXPECT_TRUE(foundStep);
}

} // namespace

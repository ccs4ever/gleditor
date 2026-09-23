/**
 * @file test_dimension_registry.cpp
 * @brief Unit tests for DimensionRegistry and InternedDimName.
 */
#include <gtest/gtest.h>

#include "common/xanadu/store.hpp"
#include "common/xanadu/zigzag/dimension_registry.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

using namespace zigzag;
using xanadu::MicroversionId;
using xanadu::Store;

TEST(DimensionRegistryTest, StringInterningAndFastComparison) {
  auto &reg = DimensionRegistry::instance();

  const auto d1_a = reg.intern("d.1");
  const auto d1_b = reg.intern("d.1");
  const auto d2   = reg.intern("d.2");

  EXPECT_FALSE(d1_a.empty());
  EXPECT_TRUE(static_cast<bool>(d1_a));
  EXPECT_EQ(d1_a.name(), "d.1");
  EXPECT_EQ(d1_a.string(), "d.1");

  // Identical strings produce identical interned IDs
  EXPECT_EQ(d1_a.id(), d1_b.id());
  EXPECT_EQ(d1_a, d1_b);

  // Distinct strings produce distinct interned IDs
  EXPECT_NE(d1_a.id(), d2.id());
  EXPECT_NE(d1_a, d2);

  // Three-way comparison consistency
  EXPECT_TRUE((d1_a <=> d1_b) == 0);
  EXPECT_TRUE((d1_a <=> d2) != 0);

  // Empty string handling
  const auto empty = reg.intern("");
  EXPECT_TRUE(empty.empty());
  EXPECT_EQ(empty.id(), 0U);

  // findInterned lookup without insertion
  EXPECT_EQ(reg.findInterned("d.1"), d1_a);
  EXPECT_TRUE(reg.findInterned("d.nonexistent_unknown_dimension").empty());
}

TEST(DimensionRegistryTest, GetOrCreateWithStoreAndManifold) {
  Store store;
  auto manifold = store.rebuildManifold(store.latest());

  auto &reg = DimensionRegistry::instance();

  // Create dimension on demand in empty store
  const auto dimX = reg.getOrCreate(store, manifold, "d.coord_x");
  EXPECT_NE(dimX, noCell);
  EXPECT_TRUE(manifold.contains(dimX));
  EXPECT_EQ(manifold.textOf(dimX, store), "d.coord_x");

  // Repeated call returns the exact same cell without modifying store
  const auto headBefore  = store.primaryCurrentVersion();
  const auto countBefore = manifold.cellCount();

  const auto dimX2 = reg.getOrCreate(store, manifold, "d.coord_x");
  EXPECT_EQ(dimX, dimX2);
  EXPECT_EQ(store.primaryCurrentVersion(), headBefore);
  EXPECT_EQ(manifold.cellCount(), countBefore);

  // Direct get() lookup from store and manifold
  EXPECT_EQ(reg.get(store, "d.coord_x"), dimX);
  EXPECT_EQ(reg.get(manifold, "d.coord_x"), dimX);

  // Lookup unknown dimension returns noCell
  EXPECT_EQ(reg.get(store, "d.does_not_exist"), std::nullopt);
}

TEST(DimensionRegistryTest, GetOrCreateGivenOnlyManifold) {
  Store store;
  auto manifold = store.rebuildManifold(store.latest());
  EXPECT_EQ(manifold.store(), &store);

  auto &reg = DimensionRegistry::instance();

  // Given only manifold and string name, creates dimension cell in store
  const auto dimY = reg.getOrCreate(manifold, "d.coord_y");
  EXPECT_NE(dimY, noCell);
  EXPECT_TRUE(manifold.contains(dimY));
  EXPECT_EQ(manifold.textOf(dimY, store), "d.coord_y");

  // Subsequent getOrCreate on manifold returns the same cell
  const auto dimY2 = reg.getOrCreate(manifold, "d.coord_y");
  EXPECT_EQ(dimY, dimY2);
}

TEST(DimensionRegistryTest, MultiStoreIsolation) {
  Store storeA;
  Store storeB;

  auto manifoldA = storeA.rebuildManifold(storeA.latest());
  auto manifoldB = storeB.rebuildManifold(storeB.latest());

  auto &reg = DimensionRegistry::instance();

  const auto dimA = reg.getOrCreate(storeA, manifoldA, "d.shared_name");
  const auto dimB = reg.getOrCreate(storeB, manifoldB, "d.shared_name");

  EXPECT_NE(dimA, noCell);
  EXPECT_NE(dimB, noCell);

  // Both stores resolve "d.shared_name" to their own store cells
  EXPECT_EQ(reg.get(storeA, "d.shared_name"), dimA);
  EXPECT_EQ(reg.get(storeB, "d.shared_name"), dimB);
}

TEST(DimensionRegistryTest, ExplicitHeadOverload) {
  Store store;
  auto ver      = store.sliceGenesis({});
  auto manifold = store.rebuildManifold(ver);

  auto &reg = DimensionRegistry::instance();

  const auto verBefore = ver;
  const auto dimZ      = reg.getOrCreate(store, ver, manifold, "d.coord_z");
  EXPECT_NE(dimZ, noCell);
  EXPECT_NE(ver, verBefore); // Head advanced
  EXPECT_TRUE(manifold.contains(dimZ));
  EXPECT_EQ(manifold.textOf(dimZ, store), "d.coord_z");

  // Subsequent call does not advance head
  const auto verAfter = ver;
  const auto dimZ2    = reg.getOrCreate(store, ver, manifold, "d.coord_z");
  EXPECT_EQ(dimZ, dimZ2);
  EXPECT_EQ(ver, verAfter);
}

TEST(DimensionRegistryTest, StoreDestructionUnregisters) {
  auto &reg             = DimensionRegistry::instance();
  const Store *storePtr = nullptr;

  {
    Store scopedStore;
    storePtr       = &scopedStore;
    auto manifold  = scopedStore.rebuildManifold(scopedStore.latest());
    const auto dim = reg.getOrCreate(scopedStore, manifold, "d.temp_dim");
    EXPECT_NE(dim, noCell);
    EXPECT_EQ(reg.get(scopedStore, "d.temp_dim"), dim);
  }

  // After scopedStore is destructed, storeDims_ entry must be removed
  EXPECT_EQ(reg.get(*storePtr, "d.temp_dim"), std::nullopt);
}

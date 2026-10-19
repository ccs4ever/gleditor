/**
 * @file vql_multi_store_test.cpp
 * @brief Unit tests for Stage 0 MultiStoreCoordinator: d.stores rank,
 *        clone master dereference '>', and '##NAME' store selection shorthand.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/store_loader.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vql/vql_engine.hpp"

namespace {

namespace fs = std::filesystem;
using xanadu::MultiStoreCoordinator;
using xanadu::Store;
using xanadu::UserPermascroll;
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

  EXPECT_TRUE(coord.coordinatorHome().has_value());
  EXPECT_NE(coord.dimStores(), 0u);
  EXPECT_NE(coord.dimClone(), 0u);
  EXPECT_NE(coord.dimName(), 0u);
  EXPECT_NE(coord.dimRole(), 0u);
  EXPECT_EQ(coord.storeCount(), 0u);
}

TEST(VQLMultiStoreTest, SingleSliceRegistration) {
  MultiStoreCoordinator coord;
  auto &arena = coord.arena();

  CellRef mySliceHome = arena.makeCell("home_slice_1");
  CellRef storeCell   = coord.addSlice("primary_slice", "primary", mySliceHome);

  EXPECT_EQ(coord.storeCount(), 1u);
  EXPECT_EQ(coord.homeAnchor(), std::optional<CellRef>{mySliceHome});
  EXPECT_NE(storeCell, noCell);

  // The representative cell on d.stores clones mySliceHome
  EXPECT_EQ(coord.derefCloneMaster(storeCell),
            std::optional<CellRef>{mySliceHome});

  // Resolves via ##NAME shorthand
  EXPECT_EQ(coord.resolveNamedStore("primary_slice"),
            std::optional<CellRef>{mySliceHome});
  EXPECT_EQ(coord.resolveNamedStore("non_existent"), std::nullopt);
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
  CellRef step1 = arena.linked(coord.coordinatorHome().value_or(noCell),
                               coord.dimStores(), false);
  EXPECT_EQ(step1, storeUsers);

  CellRef step2 = arena.linked(step1, coord.dimStores(), false);
  EXPECT_EQ(step2, storeMath);

  CellRef step3 = arena.linked(step2, coord.dimStores(), false);
  EXPECT_EQ(step3, storeGeo);

  EXPECT_EQ(arena.linked(step3, coord.dimStores(), false), noCell);

  // Test the universal '>' clone master dereference on each storeCell
  EXPECT_EQ(coord.derefCloneMaster(storeUsers),
            std::optional<CellRef>{homeUsers});
  EXPECT_EQ(coord.derefCloneMaster(storeMath),
            std::optional<CellRef>{homeMath});
  EXPECT_EQ(coord.derefCloneMaster(storeGeo), std::optional<CellRef>{homeGeo});

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
  EXPECT_EQ(coord.resolveNamedStore("users"),
            std::optional<CellRef>{homeUsers});
  EXPECT_EQ(coord.resolveNamedStore("math"), std::optional<CellRef>{homeMath});
  EXPECT_EQ(coord.resolveNamedStore("geo"), std::optional<CellRef>{homeGeo});
  EXPECT_EQ(coord.resolveNamedStore("missing"), std::nullopt);
}

TEST(VQLMultiStoreTest, UniversalCloneMasterDereferenceChain) {
  MultiStoreCoordinator coord;
  auto &arena = coord.arena();

  // Create chain: C >< B >< A where A is master
  CellRef cellA = arena.makeCell("MasterA");
  CellRef cellB = arena.makeCell();
  CellRef cellC = arena.makeCell();

  // Link B to A along d.clone: A pos to B, B neg to A
  EXPECT_TRUE(arena.link(cellA, coord.dimClone(), DimVector::POS, cellB));
  EXPECT_TRUE(arena.link(cellB, coord.dimClone(), DimVector::NEG, cellA));

  // Link C to B along d.clone: B pos to C, C neg to B
  EXPECT_TRUE(arena.link(cellB, coord.dimClone(), DimVector::POS, cellC));
  EXPECT_TRUE(arena.link(cellC, coord.dimClone(), DimVector::NEG, cellB));

  // Dereferencing any cell in the chain via '>' lands on Master A
  EXPECT_EQ(coord.derefCloneMaster(cellA), std::optional<CellRef>{cellA});
  EXPECT_EQ(coord.derefCloneMaster(cellB), std::optional<CellRef>{cellA});
  EXPECT_EQ(coord.derefCloneMaster(cellC), std::optional<CellRef>{cellA});
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

  const auto namedA = coord.resolveNamedStore("docA");
  const auto namedB = coord.resolveNamedStore("docB");
  ASSERT_TRUE(namedA.has_value());
  ASSERT_TRUE(namedB.has_value());
  const CellRef resolvedA = *namedA;
  const CellRef resolvedB = *namedB;
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

// A document branch forked from the root before the slice existed has the
// greatest name, so latest() names it; the store's home is on branch 0. Folded
// at latest(), `##` answered with a cell holding the store's label.
TEST(VQLMultiStoreTest, AGuessedVersionFoldsWhereTheHomeIs) {
  const auto dir = tempStoreDir("forked_document");
  UserPermascroll::Config config;
  config.storageDir = dir / "permascroll";
  auto scroll       = std::make_shared<UserPermascroll>(config);
  auto store        = std::make_shared<Store>(scroll);

  const auto typed  = store->insert(xanadu::MicroversionId{}, 0, "a document");
  const auto sliced = store->sliceGenesis(typed);
  const auto cell   = store->makeCell(sliced, "a cell");
  std::ignore = store->insert(xanadu::MicroversionId{}, 0, "another branch");
  ASSERT_FALSE(store->latest().isAncestorOf(cell)) << "latest is the fork";

  MultiStoreCoordinator coord;
  coord.addStore("forked", "primary", store);

  const auto folded = coord.findStore("forked");
  ASSERT_TRUE(folded.has_value());
  EXPECT_NE(folded->manifold->home(), noCell);
  EXPECT_EQ(coord.arena().textOf(coord.homeAnchor().value_or(noCell), *store),
            "home");
}

// A document's prose is no cell; find() answers a hit in it with the line,
// linked along d.source to where the line was found. Before, a store's own
// visible text was invisible to every query.
TEST(VQLMultiStoreTest, FindReadsDocumentProse) {
  const auto dir = tempStoreDir("find_prose");
  UserPermascroll::Config config;
  config.storageDir = dir / "permascroll";
  auto scroll       = std::make_shared<UserPermascroll>(config);
  auto store        = std::make_shared<Store>(scroll);
  std::ignore       = store->insert(xanadu::MicroversionId{}, 0,
                                    "first line\nthe needle line\nlast line");

  MultiStoreCoordinator coord;
  coord.addStore("prose", "primary", store);
  xanadu::vql::VQLEngine engine(coord);

  const auto hits = engine.execute(R"(find("needle"))");
  ASSERT_EQ(hits.size(), 1U);
  const auto &arena = coord.arena();
  EXPECT_EQ(arena.textOf(hits[0]), "the needle line");
  // The document's own bytes, not a copy of them.
  const auto quoted = arena.quotedContent(hits[0]);
  ASSERT_TRUE(quoted.has_value());
  EXPECT_EQ(quoted->store, store.get());
  EXPECT_EQ(quoted->spans,
            store->rebuild(store->primaryCurrentVersion()).spansFor(11, 15));
  const auto sourceDim = coord.core().findDimension("d.source");
  ASSERT_TRUE(sourceDim.has_value());
  const auto origin = arena.linked(hits[0], *sourceDim);
  ASSERT_NE(origin, noCell);
  EXPECT_THAT(arena.textOf(origin), ::testing::EndsWith("&at=15"));
}

TEST(VQLMultiStoreTest, ContainsAnswersAtTheTopLevel) {
  MultiStoreCoordinator coord;
  xanadu::vql::VQLEngine engine(coord);
  const auto yes = engine.execute(R"(contains("abc", "b"))");
  ASSERT_EQ(yes.size(), 1U);
  EXPECT_EQ(coord.core().render(yes[0]), zigzag::vortex::CellValue(true));
  const auto no = engine.execute(R"(contains("abc", "z"))");
  ASSERT_EQ(no.size(), 1U);
  EXPECT_EQ(coord.core().render(no[0]), zigzag::vortex::CellValue(false));
}

// Answering nothing for a function nobody defined reads as "no match".
TEST(VQLMultiStoreTest, AnUnknownFunctionIsRefusedByName) {
  MultiStoreCoordinator coord;
  xanadu::vql::VQLEngine engine(coord);
  EXPECT_THROW(std::ignore = engine.execute("frob(1)"), std::runtime_error);
}

TEST(VQLMultiStoreTest, StoreLoaderLoadStorePathways) {
  const auto dir = tempStoreDir("loader_pathway");
  auto scroll    = std::make_shared<UserPermascroll>();
  Store originalStore(scroll);
  const auto v1 =
      originalStore.insert(xanadu::MicroversionId{}, 0, "Test Store Content");
  originalStore.save(dir.string());

  // Pathway 1: loadStore(Store &store, path)
  Store destStore(scroll);
  xanadu::loadStore(destStore, dir);
  EXPECT_EQ(destStore.allVersions().size(), 1u);
  EXPECT_EQ(destStore.rebuild(v1).materialize(*scroll), "Test Store Content");

  // Pathway 2: loadStore(path, permascroll) -> std::unique_ptr<Store>
  auto loadedPtr = xanadu::loadStore(dir, scroll);
  ASSERT_NE(loadedPtr, nullptr);
  EXPECT_EQ(loadedPtr->allVersions().size(), 1u);
  EXPECT_EQ(loadedPtr->rebuild(v1).materialize(*scroll), "Test Store Content");
}

TEST(VQLMultiStoreTest, StoreLoaderImportFileStore) {
  const auto tempDir = tempStoreDir("import_file_test");
  const auto srcFile = tempDir / "sample_import.txt";
  const auto dstDir  = tempDir / "saved_store";

  {
    std::ofstream out(srcFile);
    out << "Singular Store Loader File Import Test Content\nLine 2";
  }

  auto scroll        = std::make_shared<UserPermascroll>();
  auto importedStore = xanadu::importFileStore(srcFile, scroll, dstDir);
  ASSERT_NE(importedStore, nullptr);
  EXPECT_TRUE(fs::exists(dstDir / "ops.nodes"));

  const auto versions = importedStore->allVersions();
  ASSERT_FALSE(versions.empty());
  EXPECT_EQ(importedStore->rebuild(versions.front()).materialize(*scroll),
            "Singular Store Loader File Import Test Content\nLine 2");
}

} // namespace

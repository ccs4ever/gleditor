/**
 * @file arena_promote_base_link_test.cpp
 * @brief promote() persists a link between an existing base cell and a cell it
 *        mints, whichever of the two is the root of the promotion.
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace {

using xanadu::MicroversionId;
using xanadu::Store;
using xanadu::UserPermascroll;
using zigzag::ArenaManifold;
using zigzag::DimVector;

struct BaseStore {
  std::shared_ptr<UserPermascroll> scroll{std::make_shared<UserPermascroll>()};
  Store store{scroll};
  MicroversionId head{store.sliceGenesis(MicroversionId{})};
  zigzag::DimRef dim{store.makeDimension(head, "d.c").dim};

  BaseStore() { head = store.latest(); }
  zigzag::CellRef home() const { return store.homeCell().value(); }
};

// The answer is the new cell; the base cell it hangs from is reached only
// through the new cell's back edge.
TEST(ArenaPromoteBaseLinkTest, ALinkFromAnExistingCellToTheRootIsPersisted) {
  BaseStore doc;
  const auto base = doc.store.rebuildManifold(doc.head);
  ArenaManifold arena{&base};

  const auto fresh = arena.makeCell("a");
  ASSERT_TRUE(arena.link(doc.home(), doc.dim, DimVector::POS, fresh));

  const auto promoted = zigzag::promote(doc.store, doc.head, arena, fresh);
  ASSERT_TRUE(promoted.has_value());
  const auto after = doc.store.rebuildManifold(promoted->version);
  ASSERT_EQ(promoted->cells.size(), 1U);
  EXPECT_EQ(after.linked(doc.home(), doc.dim, DimVector::POS),
            promoted->cells.front());
  EXPECT_EQ(after.linked(promoted->cells.front(), doc.dim, DimVector::NEG),
            doc.home());
  EXPECT_EQ(after.refusedOps(), 0U);
}

// The dimension is minted by the same evaluation, as `##/+d.c%"a"` does.
TEST(ArenaPromoteBaseLinkTest, ALinkOverADimensionTheQueryMintedIsPersisted) {
  BaseStore doc;
  const auto base = doc.store.rebuildManifold(doc.head);
  ArenaManifold arena{&base};

  const auto dim   = arena.ensureDimension("d.fresh");
  const auto fresh = arena.makeCell("a");
  ASSERT_TRUE(arena.link(doc.home(), dim, DimVector::POS, fresh));

  const auto promoted = zigzag::promote(doc.store, doc.head, arena, fresh);
  ASSERT_TRUE(promoted.has_value());
  const auto after = doc.store.rebuildManifold(promoted->version);
  // The minted dimension is a promoted cell like any other; the link from the
  // base home over it is what a second reader needs.
  zigzag::CellRef realDim{};
  for (const auto cell : promoted->cells) {
    if (after.textOf(cell, doc.store) == "d.fresh") realDim = cell;
  }
  const auto reached = after.linked(doc.home(), realDim, DimVector::POS);
  ASSERT_TRUE(reached.has_value());
  EXPECT_EQ(after.textOf(*reached, doc.store), "a");
  // A reader resolves a dimension by name, so it has to be on the d.dims rank.
  const auto named = after.dimensionNamed("d.fresh", doc.store);
  ASSERT_TRUE(named.has_value());
  EXPECT_EQ(*named, realDim);
  EXPECT_EQ(after.refusedOps(), 0U);
}

// What `vquery --in-place` does: the query reaches the store's home through
// the store's own space, so the end of the link that exists already is a proxy
// of a destination cell rather than the cell itself.
TEST(ArenaPromoteBaseLinkTest, ALinkFromTheStoresOwnProxyIsPersisted) {
  namespace fs       = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "arena_promote_proxy_link";
  fs::remove_all(dir);
  fs::create_directories(dir);
  fs::copy("tests/samples/xudu/core_hypertext/unified_store", dir / "store",
           fs::copy_options::recursive);
  fs::copy("tests/samples/xudu/permascroll", dir / "ps",
           fs::copy_options::recursive);
  UserPermascroll::Config config;
  config.storageDir = dir / "ps";
  const auto scroll = std::make_shared<UserPermascroll>(config);
  xanadu::MultiStoreCoordinator coordinator;
  coordinator.loadAndAddStore("store", "primary", (dir / "store").string(),
                              scroll);
  xanadu::vql::VQLEngine engine(coordinator);
  const auto results = engine.execute("##/+d.c%\"a\"");
  ASSERT_EQ(results.size(), 1U);

  auto &store       = *coordinator.primaryStore()->store;
  const auto home   = store.homeCell().value();
  const auto head   = store.structureHead().value_or(store.latest());
  const auto before = store.rebuildManifold(head).dimensions().size();
  const auto promoted =
      zigzag::promote(store, head, coordinator.arena(), results.front());
  ASSERT_TRUE(promoted.has_value());
  // The answer and the dimension it hangs on; the home was already there.
  EXPECT_EQ(promoted->cells.size(), 2U);

  auto after = store.rebuildManifold(promoted->version);
  EXPECT_EQ(after.refusedOps(), 0U);
  EXPECT_EQ(after.dimensions().size(), before + 1);
  const auto named = after.dimensionNamed("d.c", store);
  ASSERT_TRUE(named.has_value());
  const auto reached = after.linked(home, *named, DimVector::POS);
  ASSERT_TRUE(reached.has_value());
  EXPECT_EQ(after.textOf(*reached, store), "a");
  EXPECT_TRUE(after.verifyAgainstFullRebuild(store));
  fs::remove_all(dir);
}

} // namespace

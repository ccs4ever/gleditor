#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace {

struct ConvergenceStore {
  std::unique_ptr<xanadu::Store> store = std::make_unique<xanadu::Store>();
  xanadu::MicroversionId version;
  zigzag::CellRef first;
  zigzag::CellRef second;
  zigzag::DimRef dimension;
};

ConvergenceStore makeConvergenceStore() {
  ConvergenceStore result;
  auto &store          = *result.store;
  auto version         = store.insert({}, 0, "ordinary xanadoc text");
  version              = store.sliceGenesis(version);
  const auto dimension = store.makeDimension(version, "d.1");
  version              = dimension.version;
  version              = store.makeCell(version, "Document passage");
  result.first         = store.cellRefOf(version);
  version              = store.makeCell(version, "Linked passage");
  result.second        = store.cellRefOf(version);
  version              = store.setLink(version, result.first, dimension.dim,
                                       zigzag::DimVector::POS, result.second);
  result.version       = version;
  result.dimension     = dimension.dim;
  return result;
}

} // namespace

TEST(XuzzConvergenceSample, OneHeadReplaysAsDocumentAndManifold) {
  auto fixture = makeConvergenceStore();
  auto &store  = *fixture.store;

  ASSERT_EQ(store.currentVersions().size(), 1U);
  EXPECT_EQ(store.primaryCurrentVersion(), fixture.version);
  EXPECT_EQ(store.textOf(fixture.version), "ordinary xanadoc text");

  const auto manifold = store.rebuildManifold(fixture.version);
  EXPECT_EQ(manifold.refusedOps(), 0U);
  EXPECT_TRUE(manifold.verifyAgainstFullRebuild(store));
  EXPECT_EQ(manifold.textOf(fixture.first, store), "Document passage");
  EXPECT_EQ(
      manifold.linked(fixture.first, fixture.dimension, zigzag::DimVector::POS),
      fixture.second);
}

TEST(XuzzConvergenceSample, PlainDocumentDoesNotInventCells) {
  xanadu::Store store;
  const auto head = store.insert({}, 0, "ordinary xanadoc text");
  EXPECT_EQ(store.rebuildManifold(head).cellCount(), 0U);
}

TEST(XuzzConvergenceSample, SliceCanGainXanadocTextAfterGenesis) {
  xanadu::Store store;
  // History: genesis -> dimension -> cells -> Xanadoc text -> cell edit ->
  // text edit. The last two operations mutate the two projections
  // independently while sharing one linear microversion head.
  auto version         = store.sliceGenesis({});
  const auto dimension = store.makeDimension(version, "d.1");
  version              = dimension.version;
  version              = store.makeCell(version, "A slice cell");
  const auto cell      = store.cellRefOf(version);

  // Add the Xanadoc after the slice already has structure.
  version = store.insert(version, 0, "Xanadoc text added later");
  // Mutate the Zigzag cell, then mutate the Xanadoc independently.
  version = store.setCellText(version, cell, "A slice cell edited");
  version = store.insert(version, 24, " and edited");

  const auto manifold = store.rebuildManifold(version);

  EXPECT_EQ(store.textOf(version), "Xanadoc text added later and edited");
  EXPECT_EQ(manifold.textOf(cell, store), "A slice cell edited");
  EXPECT_EQ(manifold.cellCount(), 4U);
  EXPECT_EQ(manifold.refusedOps(), 0U);
  EXPECT_TRUE(manifold.verifyAgainstFullRebuild(store));
}

TEST(XuzzConvergenceSample, MaterializedStoreLoadsForInspection) {
  xanadu::UserPermascroll::Config config;
  config.storageDir =
      std::filesystem::path("tests/samples/xuzz/slice_then_xanadoc") /
      "permascroll";
  auto scroll = std::make_shared<xanadu::UserPermascroll>(std::move(config));
  xanadu::Store store(scroll);
  store.load("tests/samples/xuzz/slice_then_xanadoc");

  ASSERT_EQ(store.currentVersions().size(), 1U);
  const auto version = store.primaryCurrentVersion();
  EXPECT_EQ(version.str(), "9");
  const auto manifold = store.rebuildManifold(version);
  EXPECT_EQ(store.textOf(version), "Xanadoc text added later and edited");
  EXPECT_EQ(manifold.cellCount(), 4U);
  EXPECT_EQ(manifold.refusedOps(), 0U);
  EXPECT_TRUE(manifold.verifyAgainstFullRebuild(store));
}

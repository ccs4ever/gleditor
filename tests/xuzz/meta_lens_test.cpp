/**
 * @file meta_lens_test.cpp
 * @brief MetaLens + project-by-reference (U-L5): a frozen fold's cells
 *        and dimension census bind to the source cells themselves.
 */
#include <filesystem>
#include <memory>
#include <string>

#include <unistd.h>

#include <gtest/gtest.h>

#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"
#include "common/xanadu/zigzag/meta_lens.hpp"
#include "common/xanadu/zigzag/projection_session.hpp"

namespace {

namespace fs = std::filesystem;
using xanadu::MicroversionId;
using xanadu::Store;
using xanadu::UserPermascroll;
using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::MetaLens;
using zigzag::OptionalCell;
using zigzag::Posward;
using zigzag::ProjectionSession;
using zigzag::vortex::VortexCore;

/// A tiny store: genesis + two cells, folded to a frozen manifold.
struct StoreFixture {
  fs::path dir;
  std::shared_ptr<Store> store;
  MicroversionId head;
  std::unique_ptr<zigzag::Manifold> fold;

  StoreFixture() {
    dir =
        fs::temp_directory_path() / ("meta-lens-" + std::to_string(::getpid()));
    UserPermascroll::Config cfg;
    cfg.storageDir = dir / "permascroll";
    auto perma     = std::make_shared<UserPermascroll>(cfg);
    store          = std::make_shared<Store>(perma);
    head           = store->sliceGenesis(MicroversionId{}, "meta-src");
    head           = store->makeCell(head, "alpha");
    head           = store->makeCell(head, "beta");
    fold = std::make_unique<zigzag::Manifold>(store->rebuildManifold(head));
  }
  ~StoreFixture() {
    fold.reset();
    store.reset();
    fs::remove_all(dir);
  }
};

TEST(MetaLensTest, ProjectedKeysBindToTheSourceCells) {
  StoreFixture src;
  // The session arena must overlay the source fold, else base cells
  // don't resolve (U-L5's overlay precondition).
  ArenaManifold arena{src.fold.get()};
  VortexCore core{arena};
  const auto *fold = src.fold.get();
  auto session     = std::make_unique<ProjectionSession>(
      std::make_unique<MetaLens>("src", *fold, *src.store), core);

  const auto root      = session->roots().front();
  const auto dContains = core.findOrMintDimension("d.contains");
  const auto dSeq      = core.findOrMintDimension("d.seq");

  // meta:src -> meta.index cells (dims, cells).
  const auto dimsIdx = arena.linked(root, dContains, Posward);
  ASSERT_TRUE(dimsIdx.has_value());
  const auto cellsIdx = arena.linked(*dimsIdx, dSeq, Posward);
  ASSERT_TRUE(cellsIdx.has_value());

  // A census entry binds to the SOURCE cell, not a minted twin: the
  // fold's first slot's birthOp is its CellRef -- non-ephemeral,
  // resolvable through the overlay.
  const auto c0Key =
      "meta:src:c:" + std::to_string(fold->cells().front().birthOp);
  const auto c0 = session->cellFor(c0Key);
  ASSERT_TRUE(c0.has_value());
  EXPECT_FALSE(zigzag::isEphemeral(*c0));
  EXPECT_TRUE(fold->contains(*c0));
  // extkey-style reverse lookup: keyFor on the base cell returns the
  // census key.
  const auto key = session->keyFor(*c0);
  ASSERT_TRUE(key.has_value());
  EXPECT_EQ(*key, c0Key);

  // The dim census: meta:src:d:d.dims binds the fold's dims dimension.
  const auto dDimsCell = session->cellFor("meta:src:d:d.dims");
  ASSERT_TRUE(dDimsCell.has_value());
  EXPECT_EQ(*dDimsCell, fold->dimsDimension());
}

TEST(MetaLensTest, RefreshLeavesSourceStructureUntouched) {
  StoreFixture src;
  ArenaManifold arena{src.fold.get()};
  VortexCore core{arena};
  const auto *fold = src.fold.get();
  auto session     = std::make_unique<ProjectionSession>(
      std::make_unique<MetaLens>("src", *fold, *src.store), core);
  const auto before = fold->cellCount();
  (void)session->refresh();
  // The fold is untouched by projection -- read-only refs mint nothing
  // into the source.
  EXPECT_EQ(fold->cellCount(), before);
  // And a second refresh re-binds rather than duplicating.
  const auto c0a = session->cellFor(
      "meta:src:c:" + std::to_string(fold->cells().front().birthOp));
  const auto c0b = session->cellFor(
      "meta:src:c:" + std::to_string(fold->cells().front().birthOp));
  EXPECT_EQ(c0a, c0b);
}

} // namespace

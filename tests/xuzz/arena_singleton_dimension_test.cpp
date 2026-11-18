/**
 * @file arena_singleton_dimension_test.cpp
 * @brief A dimension name held by exactly one attached space keeps resolving
 *        to that space's dimension after another space is attached, and the
 *        undo of that attach leaves nothing behind.
 */
#include <gtest/gtest.h>

#include <memory>
#include <string_view>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace {

using xanadu::MicroversionId;
using xanadu::Store;
using xanadu::UserPermascroll;

struct NamedStore {
  std::shared_ptr<UserPermascroll> scroll{std::make_shared<UserPermascroll>()};
  Store store{scroll};
  MicroversionId head{store.sliceGenesis(MicroversionId{})};

  zigzag::DimRef addDimension(const std::string_view name) {
    const auto made = store.makeDimension(head, name);
    head            = made.version;
    return made.dim;
  }
  zigzag::Manifold fold() const { return store.rebuildManifold(head); }
};

TEST(ArenaSingletonDimensionTest, OneSpaceNameResolvesAfterASecondAttach) {
  NamedStore live;
  const auto sharedL  = live.addDimension("d.shared");
  const auto onlyLive = live.addDimension("d.only-live");
  NamedStore other;
  const auto sharedO   = other.addDimension("d.shared");
  const auto liveFold  = live.fold();
  const auto otherFold = other.fold();

  zigzag::ArenaManifold arena;
  const auto first  = arena.attach(zigzag::Space{
       .manifold = &liveFold, .store = &live.store, .label = "live"});
  const auto second = arena.attach(zigzag::Space{
      .manifold = &otherFold, .store = &other.store, .label = "other"});
  arena.bindDimensionsByNameMatch();

  const auto arenaOnly = arena.dimensionNamed("d.only-live", live.store);
  ASSERT_TRUE(arenaOnly.has_value());
  EXPECT_EQ(arena.dimIn(first, *arenaOnly), onlyLive);
  EXPECT_FALSE(arena.dimIn(second, *arenaOnly));

  // A name in two spaces still binds to each space's own dimension.
  const auto arenaShared = arena.dimensionNamed("d.shared", live.store);
  ASSERT_TRUE(arenaShared.has_value());
  EXPECT_EQ(arena.dimIn(first, *arenaShared), sharedL);
  EXPECT_EQ(arena.dimIn(second, *arenaShared), sharedO);
}

TEST(ArenaSingletonDimensionTest, ReleaseOfTheSecondAttachLeavesNoBinding) {
  NamedStore live;
  live.addDimension("d.shared");
  const auto onlyLive = live.addDimension("d.only-live");
  NamedStore other;
  other.addDimension("d.shared");
  other.addDimension("d.only-other");
  const auto liveFold  = live.fold();
  const auto otherFold = other.fold();

  zigzag::ArenaManifold arena;
  const auto first  = arena.attach(zigzag::Space{
       .manifold = &liveFold, .store = &live.store, .label = "live"});
  const auto mark   = arena.mark();
  const auto second = arena.attach(zigzag::Space{
      .manifold = &otherFold, .store = &other.store, .label = "other"});
  arena.bindDimensionsByNameMatch();
  const auto arenaOnly = arena.dimensionNamed("d.only-live", live.store);
  ASSERT_TRUE(arenaOnly.has_value());
  ASSERT_EQ(arena.dimIn(first, *arenaOnly), onlyLive);
  ASSERT_FALSE(arena.dimIn(second, *arenaOnly));

  ASSERT_TRUE(arena.release(mark));

  EXPECT_EQ(arena.spaceCount(), 1U);
  EXPECT_EQ(arena.dimIn(first, *arenaOnly), onlyLive);
  EXPECT_FALSE(arena.boundDimensionSet(*arenaOnly).has_value());
  EXPECT_FALSE(arena.dimensionNamed("d.only-other", other.store).has_value());
}

} // namespace

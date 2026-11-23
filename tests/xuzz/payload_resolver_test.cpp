#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <limits>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/payload_resolver.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {
using namespace xanadu::vortex;

TEST(PayloadResolverTest, SlicesBinaryBytesWithCheckedOffsets) {
  PayloadResolver resolver;
  const std::array bytes{std::byte{0x41}, std::byte{0x00}, std::byte{0xff},
                         std::byte{0x42}};
  const PayloadRef origin{1, 3, 9};
  ASSERT_TRUE(
      resolver.registerSource(origin, PayloadSourceKind::Durable, bytes));
  auto lease = resolver.retain({origin, 0, bytes.size()});
  ASSERT_TRUE(lease);
  auto part = resolver.slice(*lease, 1, 2);
  ASSERT_TRUE(part);
  EXPECT_EQ(part->range, (PayloadRange{origin, 1, 2}));
  auto read = resolver.read(*part, 0, 2);
  ASSERT_TRUE(read);
  EXPECT_EQ(*read, (std::vector<std::byte>{std::byte{0x00}, std::byte{0xff}}));
  EXPECT_EQ(resolver.slice(*lease, 1, std::numeric_limits<std::uint64_t>::max())
                .error(),
            PayloadError::InvalidRange);
  EXPECT_EQ(resolver.read(*lease, std::numeric_limits<std::uint64_t>::max(), 1)
                .error(),
            PayloadError::InvalidRange);
}

TEST(PayloadResolverTest, TransientPayloadIsRefusedByPersistenceBoundary) {
  PayloadResolver resolver;
  const std::array bytes{std::byte{0x00}};
  const PayloadRef origin{2, 1, 5};
  ASSERT_TRUE(
      resolver.registerSource(origin, PayloadSourceKind::Transient, bytes));
  auto lease = resolver.retain({origin, 0, 1});
  ASSERT_TRUE(lease);
  EXPECT_EQ(resolver.requirePersistable(*lease).error(),
            PayloadError::TransientNotPersistable);
  resolver.release(lease->lease);
  EXPECT_EQ(resolver.read(*lease, 0, 1).error(), PayloadError::StaleLease);
}

TEST(PayloadResolverTest, OwnerTokenRemainsReadableAfterCallerLeaseRelease) {
  PayloadResolver resolver;
  const std::array bytes{std::byte{0x7f}};
  const PayloadRef origin{4, 1, 2};
  ASSERT_TRUE(
      resolver.registerSource(origin, PayloadSourceKind::Durable, bytes));
  auto lease = resolver.retain({origin, 0, 1});
  ASSERT_TRUE(lease);
  auto owner = resolver.retainOwner(*lease);
  ASSERT_TRUE(owner);
  resolver.release(lease->lease);
  const auto read = resolver.read(**owner, 0, 1);
  ASSERT_TRUE(read);
  EXPECT_EQ(*read, (std::vector<std::byte>{std::byte{0x7f}}));
  owner->reset();
  EXPECT_EQ(resolver.read(*lease, 0, 1).error(), PayloadError::StaleLease);
}

TEST(PayloadResolverTest, ArenaOwnerRefusesPromotionBeforeStoreMutation) {
  PayloadResolver resolver;
  const std::array bytes{std::byte{0x00}, std::byte{0xff}};
  const PayloadRef origin{8, 2, 11};
  ASSERT_TRUE(
      resolver.registerSource(origin, PayloadSourceKind::Transient, bytes));
  auto lease = resolver.retain({origin, 0, bytes.size()});
  ASSERT_TRUE(lease);
  auto owner = resolver.retainOwner(*lease);
  ASSERT_TRUE(owner);

  xanadu::Store store;
  const auto parent = store.sliceGenesis(xanadu::MicroversionId{});
  zigzag::ArenaManifold arena;
  const auto cell        = arena.makeCell("transient payload");
  const auto incarnation = arena.incarnationOf(cell);
  ASSERT_TRUE(incarnation);
  ASSERT_EQ(arena.setOwnerLease(cell, *incarnation, *owner), &arena);
  const auto opCount = store.opCount();

  resolver.release(lease->lease);
  const auto pinnedRead = resolver.read(**owner, 0, bytes.size());
  ASSERT_TRUE(pinnedRead);
  EXPECT_EQ(*pinnedRead, (std::vector<std::byte>(bytes.begin(), bytes.end())));
  EXPECT_FALSE(zigzag::promote(store, parent, arena, cell).has_value());
  EXPECT_EQ(store.opCount(), opCount);
}
} // namespace

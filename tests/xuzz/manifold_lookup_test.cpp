#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory_resource>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/xanadu/osmic_walker.hpp"
#include "common/xanadu/zigzag/manifold_lookup.hpp"

namespace {

static_assert(
    !std::is_aggregate_v<zigzag::detail::ManifoldOpIndexMap::PreparedInsert>);
static_assert(!std::is_copy_constructible_v<
              zigzag::detail::ManifoldOpIndexMap::PreparedInsert>);
static_assert(std::is_move_constructible_v<
              zigzag::detail::ManifoldOpIndexMap::PreparedInsert>);

TEST(OsmicWalkerCountTest, EmptyPathReportsZeroBeforeReturning) {
  bool called       = false;
  const auto walked = xanadu::OsmicWalker::walkPathCustomWithCount(
      0, 0,
      [](std::uint32_t) -> const xanadu::CompactOpNode * {
        ADD_FAILURE() << "empty path must not read a node";
        return nullptr;
      },
      [](std::uint32_t, const xanadu::CompactOpNode &) {
        ADD_FAILURE() << "empty path must not visit a node";
      },
      [&called](const std::size_t count) {
        called = true;
        EXPECT_EQ(count, 0U);
      });
  EXPECT_TRUE(walked);
  EXPECT_TRUE(called);

  called = false;
  EXPECT_TRUE(xanadu::OsmicWalker::walkPathCustomWithCount(
      2, 2,
      [](std::uint32_t) -> const xanadu::CompactOpNode * {
        ADD_FAILURE() << "equal endpoints must not read a node";
        return nullptr;
      },
      [](std::uint32_t, const xanadu::CompactOpNode &) {
        ADD_FAILURE() << "equal endpoints must not visit a node";
      },
      [&called](const std::size_t count) {
        called = true;
        EXPECT_EQ(count, 0U);
      }));
  EXPECT_TRUE(called);
}

TEST(OsmicWalkerCountTest, ValidPathCountArrivesBeforeVisitor) {
  std::array<xanadu::CompactOpNode, 4> nodes{};
  nodes[1].parentIndex = 0;
  nodes[2].parentIndex = 1;
  nodes[3].parentIndex = 2;
  std::vector<std::uint32_t> visits;
  std::size_t callbackCount = 0;
  const auto walked         = xanadu::OsmicWalker::walkPathCustomWithCount(
      0, 3,
      [&nodes](const std::uint32_t index) -> const xanadu::CompactOpNode         *{
        return index < nodes.size() ? &nodes[index] : nullptr;
      },
      [&visits](const std::uint32_t index, const xanadu::CompactOpNode &) {
        visits.push_back(index);
      },
      [&callbackCount, &visits](const std::size_t count) {
        callbackCount = count;
        EXPECT_TRUE(visits.empty());
      });
  EXPECT_TRUE(walked);
  EXPECT_EQ(callbackCount, 3U);
  EXPECT_EQ(visits, (std::vector<std::uint32_t>{1, 2, 3}));
}

TEST(OsmicWalkerCountTest, InvalidAncestryDoesNotReportCountOrVisit) {
  std::array<xanadu::CompactOpNode, 4> nodes{};
  nodes[1].parentIndex = 0;
  nodes[2].parentIndex = 1;
  nodes[3].parentIndex = 2;
  bool callbackCalled  = false;
  std::vector<std::uint32_t> visits;
  const auto walked = xanadu::OsmicWalker::walkPathCustomWithCount(
      9, 3,
      [&nodes](const std::uint32_t index) -> const xanadu::CompactOpNode * {
        return index < nodes.size() ? &nodes[index] : nullptr;
      },
      [&visits](const std::uint32_t index, const xanadu::CompactOpNode &) {
        visits.push_back(index);
      },
      [&callbackCalled](std::size_t) { callbackCalled = true; });
  EXPECT_FALSE(walked);
  EXPECT_FALSE(callbackCalled);
  EXPECT_TRUE(visits.empty());
}

TEST(OsmicWalkerCountTest, NonzeroToZeroIsInvalidWithoutCallbacks) {
  bool countCalled = false;
  std::vector<std::uint32_t> visits;
  const auto walked = xanadu::OsmicWalker::walkPathCustomWithCount(
      2, 0,
      [](std::uint32_t) -> const xanadu::CompactOpNode * {
        ADD_FAILURE() << "invalid ancestry must not read a node";
        return nullptr;
      },
      [&visits](std::uint32_t index, const xanadu::CompactOpNode &) {
        visits.push_back(index);
      },
      [&countCalled](std::size_t) { countCalled = true; });
  EXPECT_FALSE(walked);
  EXPECT_FALSE(countCalled);
  EXPECT_TRUE(visits.empty());
}

TEST(OsmicWalkerCountTest, ThrowingCountCallbackPrecedesAnyVisitorEffect) {
  std::array<xanadu::CompactOpNode, 2> nodes{};
  nodes[1].parentIndex = 0;
  bool visited         = false;
  EXPECT_THROW(
      xanadu::OsmicWalker::walkPathCustomWithCount(
          0, 1,
          [&nodes](const std::uint32_t index) -> const xanadu::CompactOpNode * {
            return &nodes[index];
          },
          [&visited](std::uint32_t, const xanadu::CompactOpNode &) {
            visited = true;
          },
          [](std::size_t) { throw std::runtime_error("stop before visit"); }),
      std::runtime_error);
  EXPECT_FALSE(visited);
}

class FailingResource final : public std::pmr::memory_resource {
public:
  void failAfter(std::size_t successfulAllocations) noexcept {
    remaining_ = successfulAllocations;
    attempts_  = 0;
    failing_   = true;
  }
  void allow() noexcept { failing_ = false; }
  [[nodiscard]] std::size_t attempts() const noexcept { return attempts_; }

private:
  void *do_allocate(std::size_t bytes, std::size_t alignment) override {
    ++attempts_;
    if (failing_) {
      if (remaining_ == 0) throw std::bad_alloc{};
      --remaining_;
    }
    return std::pmr::new_delete_resource()->allocate(bytes, alignment);
  }
  void do_deallocate(void *pointer, std::size_t bytes,
                     std::size_t alignment) override {
    std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
  }
  bool
  do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
    return this == &other;
  }

  std::size_t remaining_{0};
  std::size_t attempts_{0};
  bool failing_{false};
};

using zigzag::detail::LookupPrepareFailure;
using zigzag::detail::ManifoldOpIndexMap;

TEST(ManifoldOpIndexMapTest, MapsZeroValuesAndArbitraryHighOperationKeys) {
  ManifoldOpIndexMap map;
  auto dense = map.prepareInsert(8191);
  ASSERT_TRUE(dense);
  ASSERT_TRUE(map.commitInsert(std::move(*dense), 8191, 0));
  auto high = map.prepareInsert(8192);
  ASSERT_TRUE(high);
  ASSERT_TRUE(map.commitInsert(std::move(*high), 8192, 0));
  auto next = map.prepareInsert(8193);
  ASSERT_TRUE(next);
  ASSERT_TRUE(map.commitInsert(std::move(*next), 8193, 17));

  EXPECT_EQ(map.find(8192), 0U);
  EXPECT_EQ(map.find(8193), 17U);
  EXPECT_EQ(map.find(0), std::nullopt);
  EXPECT_EQ(map.find(8191), 0U);
  EXPECT_EQ(map.find(8190), std::nullopt);
  const auto zero = map.prepareInsert(0);
  ASSERT_FALSE(zero);
  EXPECT_EQ(zero.error(), LookupPrepareFailure::InvalidOperationIndex);
  auto copy = map;
  EXPECT_TRUE(copy == map);
  ManifoldOpIndexMap moved(std::move(copy));
  EXPECT_TRUE(moved == map);
}

TEST(ManifoldOpIndexMapTest, HintChangesDoNotLoseExistingMappings) {
  ManifoldOpIndexMap map;
  map.setOpCountHint(1);
  auto first = map.prepareInsert(8192);
  ASSERT_TRUE(first);
  ASSERT_TRUE(map.commitInsert(std::move(*first), 8192, 12));

  map.setOpCountHint(5000);
  auto second = map.prepareInsert(8193);
  ASSERT_TRUE(second);
  ASSERT_TRUE(map.commitInsert(std::move(*second), 8193, 13));
  EXPECT_EQ(map.find(8192), 12U);
  EXPECT_EQ(map.find(8193), 13U);
}

TEST(ManifoldOpIndexMapTest, UnknownFallbackDiffersFromKnownShortPath) {
  ManifoldOpIndexMap unknown;
  EXPECT_EQ(unknown.opCountHint(), std::nullopt);
  auto unknownToken = unknown.prepareInsert(1);
  ASSERT_TRUE(unknownToken);
  ASSERT_TRUE(unknown.commitInsert(std::move(*unknownToken), 1, 11));
  EXPECT_GT(unknown.denseValueCapacity(), 0U);

  ManifoldOpIndexMap knownZero;
  knownZero.setOpCountHint(0U);
  EXPECT_EQ(knownZero.opCountHint(), std::optional<std::size_t>{0U});
  auto zeroToken = knownZero.prepareInsert(1);
  ASSERT_TRUE(zeroToken);
  ASSERT_TRUE(knownZero.commitInsert(std::move(*zeroToken), 1, 12));
  EXPECT_EQ(knownZero.denseValueCapacity(), 0U);
  EXPECT_EQ(knownZero.find(1), 12U);

  ManifoldOpIndexMap knownShort;
  knownShort.setOpCountHint(1U);
  auto shortToken = knownShort.prepareInsert(3);
  ASSERT_TRUE(shortToken);
  ASSERT_TRUE(knownShort.commitInsert(std::move(*shortToken), 3, 13));
  EXPECT_EQ(knownShort.denseValueCapacity(), 0U);
}

TEST(ManifoldOpIndexMapTest, PreparedTokensAreOpaqueAndOwnerBound) {
  FailingResource firstResource;
  FailingResource secondResource;
  ManifoldOpIndexMap first(&firstResource);
  ManifoldOpIndexMap second(&secondResource);
  auto token = first.prepareInsert(1);
  ASSERT_TRUE(token);
  EXPECT_FALSE(second.commitInsert(std::move(*token), 1, 99));
  EXPECT_TRUE(second.empty());
  EXPECT_FALSE(first.commitInsert(std::move(*token), 2, 99));
  ASSERT_TRUE(first.commitInsert(std::move(*token), 1, 17));
  EXPECT_EQ(first.find(1), 17U);
}

TEST(ManifoldOpIndexMapTest, MoveInvalidatesStaleAndAllowsFreshToken) {
  ManifoldOpIndexMap source;
  source.setOpCountHint(1);
  auto stale = source.prepareInsert(1);
  ASSERT_TRUE(stale);
  ManifoldOpIndexMap moved(std::move(source));
  auto fresh = source.prepareInsert(1);
  ASSERT_TRUE(fresh);
  EXPECT_FALSE(source.commitInsert(std::move(*stale), 1, 99));
  ASSERT_TRUE(source.commitInsert(std::move(*fresh), 1, 7));
  EXPECT_EQ(source.find(1), 7U);
  EXPECT_FALSE(moved.contains(1));
}

TEST(ManifoldOpIndexMapTest, MoveAssignmentInvalidatesBothOwnersTokens) {
  ManifoldOpIndexMap destination;
  destination.setOpCountHint(1);
  auto staleDestination = destination.prepareInsert(1);
  ASSERT_TRUE(staleDestination);
  ManifoldOpIndexMap source;
  source.setOpCountHint(1);
  auto staleSource = source.prepareInsert(2);
  ASSERT_TRUE(staleSource);
  destination = std::move(source);

  EXPECT_FALSE(destination.commitInsert(std::move(*staleDestination), 1, 99));
  auto freshDestination = destination.prepareInsert(1);
  ASSERT_TRUE(freshDestination);
  ASSERT_TRUE(destination.commitInsert(std::move(*freshDestination), 1, 8));
  auto freshSource = source.prepareInsert(2);
  ASSERT_TRUE(freshSource);
  EXPECT_FALSE(source.commitInsert(std::move(*staleSource), 2, 99));
  ASSERT_TRUE(source.commitInsert(std::move(*freshSource), 2, 9));
  EXPECT_EQ(destination.find(1), 8U);
  EXPECT_EQ(source.find(2), 9U);
}

TEST(ManifoldOpIndexMapTest,
     CopyAssignmentKeepsDestinationResourceAfterSourceDies) {
  FailingResource destinationResource;
  ManifoldOpIndexMap destination(&destinationResource);
  {
    FailingResource sourceResource;
    ManifoldOpIndexMap source(&sourceResource);
    source.setOpCountHint(1);
    auto dense = source.prepareInsert(1);
    ASSERT_TRUE(dense);
    ASSERT_TRUE(source.commitInsert(std::move(*dense), 1, 5));
    auto sparse = source.prepareInsert(8192);
    ASSERT_TRUE(sparse);
    ASSERT_TRUE(source.commitInsert(std::move(*sparse), 8192, 6));
    destination = source;
  }

  EXPECT_EQ(destination.find(1), 5U);
  EXPECT_EQ(destination.find(8192), 6U);
  for (std::uint32_t op = 8193; op < 8300; ++op) {
    auto prepared = destination.prepareInsert(op);
    ASSERT_TRUE(prepared);
    ASSERT_TRUE(destination.commitInsert(std::move(*prepared), op, op));
  }
}

std::size_t opIndexMapCopyAssignmentAllocationCount() {
  FailingResource destinationResource;
  FailingResource sourceResource;
  ManifoldOpIndexMap destination(&destinationResource);
  ManifoldOpIndexMap source(&sourceResource);
  destination.setOpCountHint(1);
  source.setOpCountHint(1);
  auto old = destination.prepareInsert(1);
  if (!old || !destination.commitInsert(std::move(*old), 1, 9)) return 0U;
  auto dense = source.prepareInsert(1);
  if (!dense || !source.commitInsert(std::move(*dense), 1, 17)) return 0U;
  auto sparse = source.prepareInsert(8192);
  if (!sparse || !source.commitInsert(std::move(*sparse), 8192, 18)) return 0U;
  destinationResource.failAfter(std::numeric_limits<std::size_t>::max());
  destination = source;
  return destinationResource.attempts();
}

TEST(ManifoldOpIndexMapTest, CopyAssignmentFailurePreservesDestinationEntries) {
  const auto allocationPoints = opIndexMapCopyAssignmentAllocationCount();
  ASSERT_GT(allocationPoints, 0U);
  for (std::size_t failAt = 0; failAt < allocationPoints; ++failAt) {
    FailingResource destinationResource;
    ManifoldOpIndexMap destination(&destinationResource);
    auto old = destination.prepareInsert(1);
    ASSERT_TRUE(old);
    ASSERT_TRUE(destination.commitInsert(std::move(*old), 1, 9));

    FailingResource sourceResource;
    ManifoldOpIndexMap source(&sourceResource);
    source.setOpCountHint(1);
    auto dense = source.prepareInsert(1);
    ASSERT_TRUE(dense);
    ASSERT_TRUE(source.commitInsert(std::move(*dense), 1, 17));
    auto sparse = source.prepareInsert(8192);
    ASSERT_TRUE(sparse);
    ASSERT_TRUE(source.commitInsert(std::move(*sparse), 8192, 18));

    destinationResource.failAfter(failAt);
    EXPECT_THROW(destination = source, std::bad_alloc);
    destinationResource.allow();
    EXPECT_EQ(destination.find(1), 9U);
    EXPECT_EQ(destination.find(8192), std::nullopt);
    EXPECT_EQ(source.find(8192), 18U);
  }
}

} // namespace

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

#include "common/xanadu/zigzag/prepared_u32_unordered_map.hpp"

namespace {

class FailingResource final : public std::pmr::memory_resource {
public:
  void failAfter(const std::size_t successful) noexcept {
    remaining_ = successful;
    attempts_  = 0;
    failing_   = true;
  }
  void allow() noexcept { failing_ = false; }
  [[nodiscard]] std::size_t attempts() const noexcept { return attempts_; }

private:
  void *do_allocate(const std::size_t bytes,
                    const std::size_t alignment) override {
    ++attempts_;
    if (failing_) {
      if (remaining_ == 0) throw std::bad_alloc{};
      --remaining_;
    }
    return std::pmr::new_delete_resource()->allocate(bytes, alignment);
  }
  void do_deallocate(void *ptr, const std::size_t bytes,
                     const std::size_t alignment) override {
    std::pmr::new_delete_resource()->deallocate(ptr, bytes, alignment);
  }
  [[nodiscard]] bool
  do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
    return this == &other;
  }
  std::size_t remaining_{0};
  std::size_t attempts_{0};
  bool failing_{false};
};

class OwnershipResource final : public std::pmr::memory_resource {
public:
  void failAllocations() noexcept { failAllocations_ = true; }
  [[nodiscard]] bool ownershipIntact() const noexcept {
    return ownershipIntact_;
  }

private:
  void *do_allocate(const std::size_t bytes,
                    const std::size_t alignment) override {
    if (failAllocations_) throw std::bad_alloc{};
    void *const allocation =
        std::pmr::new_delete_resource()->allocate(bytes, alignment);
    if (allocationCount_ == allocations_.size()) std::terminate();
    allocations_[allocationCount_++] = allocation;
    return allocation;
  }
  void do_deallocate(void *ptr, const std::size_t bytes,
                     const std::size_t alignment) override {
    bool found = false;
    for (std::size_t i = 0; i < allocationCount_; ++i) {
      if (allocations_[i] == ptr) {
        allocations_[i] = nullptr;
        found           = true;
        break;
      }
    }
    ownershipIntact_ &= found;
    std::pmr::new_delete_resource()->deallocate(ptr, bytes, alignment);
  }
  [[nodiscard]] bool
  do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
    return this == &other;
  }

  std::array<void *, 64> allocations_{};
  std::size_t allocationCount_{0};
  bool ownershipIntact_{true};
  bool failAllocations_{false};
};

using Map = zigzag::detail::PreparedU32UnorderedMap<std::uint32_t>;
static_assert(!std::is_aggregate_v<Map::PreparedInsert>);
static_assert(!std::is_copy_constructible_v<Map::PreparedInsert>);
static_assert(std::is_move_constructible_v<Map::PreparedInsert>);

bool insert(Map &map, const std::uint32_t key, const std::uint32_t value) {
  auto prepared = map.prepareInsert(key);
  return prepared && map.commitInsert(std::move(*prepared), value);
}

std::size_t prepareAllocationCount(const std::size_t existingEntries) {
  FailingResource resource;
  Map map(&resource);
  for (std::size_t i = 1; i <= existingEntries; ++i) {
    if (!insert(map, static_cast<std::uint32_t>(i),
                static_cast<std::uint32_t>(i * 10U)))
      return 0U;
  }
  const auto before = resource.attempts();
  const auto token =
      map.prepareInsert(static_cast<std::uint32_t>(existingEntries + 1U));
  return token ? resource.attempts() - before : 0U;
}

std::size_t entriesBeforeNextBucketGrowth() {
  Map map;
  std::size_t count = 0;
  while (static_cast<float>(map.size() + 1U) <=
         static_cast<float>(map.capacity()) * map.maxLoadFactor()) {
    ++count;
    if (!insert(map, static_cast<std::uint32_t>(count),
                static_cast<std::uint32_t>(count * 10U)))
      return 0U;
  }
  return count;
}

std::size_t copyAssignmentAllocationCount() {
  FailingResource destinationResource;
  FailingResource sourceResource;
  Map destination(&destinationResource);
  Map source(&sourceResource);
  if (!insert(destination, 1, 11) || !insert(source, 2, 22) ||
      !insert(source, 3, 33))
    return 0U;
  destinationResource.failAfter(std::numeric_limits<std::size_t>::max());
  destination = source;
  return destinationResource.attempts();
}

TEST(PreparedU32UnorderedMapTest, PreparedTokensAreOpaqueAndOwnerBound) {
  FailingResource firstResource;
  FailingResource secondResource;
  Map first(&firstResource);
  Map second(&secondResource);
  auto token = first.prepareInsert(7);
  ASSERT_TRUE(token);
  EXPECT_FALSE(second.commitInsert(std::move(*token), 70));
  EXPECT_TRUE(second.empty());
  ASSERT_TRUE(first.commitInsert(std::move(*token), 70));
  ASSERT_NE(first.find(7), first.end());
  EXPECT_EQ(first.find(7)->second, 70U);
}

TEST(PreparedU32UnorderedMapTest,
     MoveAssignmentReleasesNodeWithOriginalResource) {
  OwnershipResource firstResource;
  OwnershipResource secondResource;
  Map first(&firstResource);
  Map second(&secondResource);
  auto firstToken  = first.prepareInsert(1);
  auto secondToken = second.prepareInsert(2);
  ASSERT_TRUE(firstToken);
  ASSERT_TRUE(secondToken);

  firstResource.failAllocations();
  secondResource.failAllocations();
  *firstToken = std::move(*secondToken);

  EXPECT_FALSE(first.commitInsert(std::move(*firstToken), 10));
  ASSERT_TRUE(second.commitInsert(std::move(*firstToken), 20));
  EXPECT_FALSE(second.commitInsert(std::move(*secondToken), 30));
  EXPECT_EQ(second.find(2)->second, 20U);
  EXPECT_TRUE(firstResource.ownershipIntact());
  EXPECT_TRUE(secondResource.ownershipIntact());
}

TEST(PreparedU32UnorderedMapTest, PreparationPreservesColdReservation) {
  FailingResource resource;
  Map map(&resource);
  ASSERT_TRUE(map.reserveFor(128));
  const auto buckets = map.capacity();
  for (std::uint32_t key = 1; key <= 4; ++key) {
    auto prepared = map.prepareInsert(key);
    ASSERT_TRUE(prepared);
    EXPECT_EQ(map.capacity(), buckets);
    resource.failAfter(0);
    ASSERT_TRUE(map.commitInsert(std::move(*prepared), key * 10));
    EXPECT_EQ(resource.attempts(), 0U);
    resource.allow();
  }
  EXPECT_EQ(map.capacity(), buckets);
}

TEST(PreparedU32UnorderedMapTest, IncrementalGrowthPreservesEntries) {
  Map map;
  constexpr std::uint32_t entries = 512U;
  for (std::uint32_t key = 1; key <= entries; ++key)
    ASSERT_TRUE(insert(map, key, key * 10));

  EXPECT_EQ(map.size(), entries);
  for (std::uint32_t key = 1; key <= entries; ++key) {
    const auto found = map.find(key);
    ASSERT_NE(found, map.end());
    EXPECT_EQ(found->second, key * 10);
  }
}

TEST(PreparedU32UnorderedMapTest, PrepareExtractsAndCommitAllocatesNothing) {
  FailingResource resource;
  Map map(&resource);
  for (std::uint32_t key = 1; key <= 512U; ++key) {
    auto prepared = map.prepareInsert(key);
    ASSERT_TRUE(prepared);
    EXPECT_EQ(map.size(), static_cast<std::size_t>(key - 1U));
    EXPECT_FALSE(map.contains(key));
    resource.failAfter(0);
    ASSERT_TRUE(map.commitInsert(std::move(*prepared), key * 10U));
    EXPECT_EQ(resource.attempts(), 0U);
    resource.allow();
    EXPECT_EQ(map.size(), static_cast<std::size_t>(key));
    EXPECT_EQ(map.find(key)->second, key * 10U);
  }
}

TEST(PreparedU32UnorderedMapTest,
     CommitAfterRehashPreparationAllocatesNothing) {
  FailingResource resource;
  Map map(&resource);
  const auto existingEntries = entriesBeforeNextBucketGrowth();
  ASSERT_GT(existingEntries, 0U);
  for (std::size_t i = 1; i <= existingEntries; ++i)
    ASSERT_TRUE(insert(map, static_cast<std::uint32_t>(i),
                       static_cast<std::uint32_t>(i * 10U)));
  const auto oldBuckets = map.capacity();
  auto prepared =
      map.prepareInsert(static_cast<std::uint32_t>(existingEntries + 1U));
  ASSERT_TRUE(prepared);
  EXPECT_GT(map.capacity(), oldBuckets);
  EXPECT_EQ(map.size(), existingEntries);
  EXPECT_FALSE(map.contains(static_cast<std::uint32_t>(existingEntries + 1U)));
  resource.failAfter(0);
  ASSERT_TRUE(map.commitInsert(std::move(*prepared), 0x1234U));
  EXPECT_EQ(resource.attempts(), 0U);
  EXPECT_EQ(map.find(static_cast<std::uint32_t>(existingEntries + 1U))->second,
            0x1234U);
}

TEST(PreparedU32UnorderedMapTest,
     AllocationFailuresPreserveFirstAndRehashMaps) {
  const auto growthPoint = entriesBeforeNextBucketGrowth();
  ASSERT_GT(growthPoint, 0U);
  for (const auto existingEntries : {std::size_t{0}, growthPoint}) {
    const auto allocationPoints = prepareAllocationCount(existingEntries);
    ASSERT_GT(allocationPoints, 0U);
    for (std::size_t failAt = 0; failAt < allocationPoints; ++failAt) {
      FailingResource resource;
      Map map(&resource);
      for (std::size_t i = 1; i <= existingEntries; ++i)
        ASSERT_TRUE(insert(map, static_cast<std::uint32_t>(i),
                           static_cast<std::uint32_t>(i * 10U)));
      const auto oldBuckets = map.capacity();
      resource.failAfter(failAt);
      const auto result =
          map.prepareInsert(static_cast<std::uint32_t>(existingEntries + 1U));
      ASSERT_FALSE(result) << "existing=" << existingEntries
                           << " allocation=" << failAt;
      EXPECT_EQ(result.error(), Map::PrepareFailure::AllocationUnavailable);
      EXPECT_EQ(map.size(), existingEntries);
      EXPECT_GE(map.capacity(), oldBuckets);
      for (std::size_t i = 1; i <= existingEntries; ++i)
        EXPECT_EQ(map.find(static_cast<std::uint32_t>(i))->second, i * 10U);

      resource.allow();
      ASSERT_TRUE(insert(map, static_cast<std::uint32_t>(existingEntries + 1U),
                         0x1234U));
      EXPECT_EQ(
          map.find(static_cast<std::uint32_t>(existingEntries + 1U))->second,
          0x1234U);
    }
  }
}

TEST(PreparedU32UnorderedMapTest, DuplicatePrepareDoesNotReplaceOldValue) {
  Map map;
  ASSERT_TRUE(insert(map, 9, 5));
  auto duplicate = map.prepareInsert(9);
  ASSERT_FALSE(duplicate);
  EXPECT_EQ(duplicate.error(), Map::PrepareFailure::AlreadyPresent);
  EXPECT_EQ(map.find(9)->second, 5U);
}

TEST(PreparedU32UnorderedMapTest,
     MoveInvalidatesTokensAndMovedFromCanBeReused) {
  Map source;
  auto old = source.prepareInsert(1);
  ASSERT_TRUE(old);
  Map moved(std::move(source));
  EXPECT_FALSE(source.commitInsert(std::move(*old), 11));
  auto fresh = source.prepareInsert(1);
  ASSERT_TRUE(fresh);
  EXPECT_TRUE(source.commitInsert(std::move(*fresh), 12));
  EXPECT_EQ(source.find(1)->second, 12U);
  EXPECT_TRUE(moved.empty());
}

TEST(PreparedU32UnorderedMapTest, MoveAssignmentInvalidatesBothTokenOwners) {
  Map destination;
  auto staleDestination = destination.prepareInsert(3);
  ASSERT_TRUE(staleDestination);
  Map source;
  auto staleSource = source.prepareInsert(4);
  ASSERT_TRUE(staleSource);

  destination = std::move(source);
  EXPECT_FALSE(destination.commitInsert(std::move(*staleDestination), 33));
  EXPECT_FALSE(source.commitInsert(std::move(*staleSource), 44));
  auto fresh = source.prepareInsert(4);
  ASSERT_TRUE(fresh);
  EXPECT_TRUE(source.commitInsert(std::move(*fresh), 45));
  EXPECT_EQ(source.find(4)->second, 45U);
}

TEST(PreparedU32UnorderedMapTest, ZeroKeyIsRejectedBeforeMapMutation) {
  Map map;
  const auto result = map.prepareInsert(0);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), Map::PrepareFailure::InvalidOperationIndex);
  EXPECT_TRUE(map.empty());
}

TEST(PreparedU32UnorderedMapTest, CopyAssignmentUsesDestinationResource) {
  FailingResource destinationResource;
  Map destination(&destinationResource);
  {
    FailingResource sourceResource;
    Map source(&sourceResource);
    ASSERT_TRUE(insert(source, 2, 22));
    destination = source;
  }
  for (std::uint32_t key = 3; key < 30; ++key)
    ASSERT_TRUE(insert(destination, key, key * 10));
  EXPECT_EQ(destination.find(2)->second, 22U);
  EXPECT_EQ(destination.find(29)->second, 290U);
}

TEST(PreparedU32UnorderedMapTest, CopyAssignmentFailurePreservesDestination) {
  const auto allocationPoints = copyAssignmentAllocationCount();
  ASSERT_GT(allocationPoints, 0U);
  for (std::size_t failAt = 0; failAt < allocationPoints; ++failAt) {
    FailingResource destinationResource;
    Map destination(&destinationResource);
    ASSERT_TRUE(insert(destination, 1, 11));

    FailingResource sourceResource;
    Map source(&sourceResource);
    ASSERT_TRUE(insert(source, 2, 22));
    ASSERT_TRUE(insert(source, 3, 33));

    destinationResource.failAfter(failAt);
    EXPECT_THROW(destination = source, std::bad_alloc);
    destinationResource.allow();
    ASSERT_EQ(destination.size(), 1U);
    EXPECT_EQ(destination.find(1)->second, 11U);
    EXPECT_EQ(destination.find(2), destination.end());
    EXPECT_EQ(source.find(3)->second, 33U);
  }
}

} // namespace

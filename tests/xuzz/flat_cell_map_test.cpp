#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory_resource>
#include <new>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include "common/xanadu/zigzag/flat_u32_map.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace {

class FailOnceResource final : public std::pmr::memory_resource {
public:
  void failAfter(const std::size_t successfulAllocations) noexcept {
    remainingBeforeFailure_ = successfulAllocations;
  }

  void denyAllocations(const bool deny) noexcept { denyAllocations_ = deny; }
  [[nodiscard]] std::size_t allocationAttempts() const noexcept {
    return allocationAttempts_;
  }

private:
  void *do_allocate(const std::size_t bytes,
                    const std::size_t alignment) override {
    ++allocationAttempts_;
    if (denyAllocations_) {
      throw std::bad_alloc{};
    }
    if (remainingBeforeFailure_) {
      if (*remainingBeforeFailure_ == 0) {
        remainingBeforeFailure_.reset();
        throw std::bad_alloc{};
      }
      --*remainingBeforeFailure_;
    }
    return std::pmr::new_delete_resource()->allocate(bytes, alignment);
  }

  void do_deallocate(void *const pointer, const std::size_t bytes,
                     const std::size_t alignment) override {
    std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
  }

  [[nodiscard]] bool
  do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
    return this == &other;
  }

  std::optional<std::size_t> remainingBeforeFailure_;
  std::size_t allocationAttempts_{0};
  bool denyAllocations_{false};
};

using zigzag::StructureBirthInfo;
using zigzag::detail::FlatU32Map;
using zigzag::detail::FlatU32MapPrepareFailure;

static_assert(std::forward_iterator<FlatU32Map<std::uint32_t>::iterator>);
static_assert(std::forward_iterator<FlatU32Map<std::uint32_t>::const_iterator>);
static_assert(sizeof(FlatU32Map<StructureBirthInfo>::value_type) == 48);

StructureBirthInfo birth(const std::uint32_t op) {
  return StructureBirthInfo{.opIndex     = op,
                            .kind        = xanadu::StructureKind::Cell,
                            .containerOp = 0,
                            .nameSpan    = {}};
}

template <class Value, class Input>
bool insert(FlatU32Map<Value> &map, const std::uint32_t key, Input &&input) {
  const Value value = static_cast<Value>(std::forward<Input>(input));
  auto prepared     = map.prepareInsert(key);
  return prepared && map.commitInsert(std::move(*prepared), value);
}

TEST(FlatU32MapTest, StoresEveryUint32KeyWithExplicitOccupancy) {
  FlatU32Map<std::uint32_t> map;
  ASSERT_TRUE(insert(map, 0, 0));
  ASSERT_TRUE(insert(map, std::numeric_limits<std::uint32_t>::max(), 9));

  ASSERT_TRUE(map.contains(0));
  ASSERT_EQ(map.find(0)->second, 0U);
  ASSERT_EQ(map.find(std::numeric_limits<std::uint32_t>::max())->second, 9U);
  EXPECT_FALSE(map.contains(1));
  EXPECT_LE(map.size(),
            FlatU32Map<std::uint32_t>::maxLiveEntries(map.capacity()));
}

TEST(FlatU32MapTest, EraseReusesTombstonesAndInvalidatesPreparedTokens) {
  FlatU32Map<std::uint32_t> map;
  ASSERT_TRUE(insert(map, 11, 22));
  auto stale = map.prepareInsert(33);
  ASSERT_TRUE(stale);

  ASSERT_TRUE(map.erase(11));
  EXPECT_FALSE(map.erase(11));
  EXPECT_FALSE(map.commitInsert(std::move(*stale), 44));
  ASSERT_TRUE(insert(map, 33, 44));
  EXPECT_FALSE(map.contains(11));
  EXPECT_EQ(map.find(33)->second, 44U);

  map.clear();
  EXPECT_TRUE(map.empty());
  EXPECT_FALSE(map.contains(33));
  EXPECT_TRUE(insert(map, 0, 0));
}

TEST(FlatU32MapTest, MoveInvalidatesPreparedTokenBeforeMovedFromReuse) {
  FlatU32Map<std::uint32_t> source;
  auto stale = source.prepareInsert(42);
  ASSERT_TRUE(stale);
  FlatU32Map<std::uint32_t> moved(std::move(source));

  auto fresh = source.prepareInsert(42);
  ASSERT_TRUE(fresh);
  EXPECT_FALSE(source.commitInsert(std::move(*stale), 99));
  ASSERT_TRUE(source.commitInsert(std::move(*fresh), 77));
  EXPECT_TRUE(source.contains(42));
  EXPECT_EQ(source.find(42)->second, 77U);
  EXPECT_FALSE(moved.contains(42));
}

TEST(FlatU32MapTest, MoveAssignmentInvalidatesSourceTokensBeforeReuse) {
  FlatU32Map<std::uint32_t> source;
  auto stale = source.prepareInsert(21);
  ASSERT_TRUE(stale);
  FlatU32Map<std::uint32_t> target;
  target = std::move(source);

  auto fresh = source.prepareInsert(21);
  ASSERT_TRUE(fresh);
  EXPECT_FALSE(source.commitInsert(std::move(*stale), 99));
  ASSERT_TRUE(source.commitInsert(std::move(*fresh), 55));
  EXPECT_EQ(source.find(21)->second, 55U);
  EXPECT_FALSE(target.contains(21));
}

TEST(FlatU32MapTest, CopyMoveAndEqualityIgnoreBucketOrder) {
  FlatU32Map<StructureBirthInfo> left;
  FlatU32Map<StructureBirthInfo> right;
  ASSERT_TRUE(insert(left, 7, birth(7)));
  ASSERT_TRUE(insert(left, 9001, birth(9001)));
  ASSERT_TRUE(insert(right, 9001, birth(9001)));
  ASSERT_TRUE(insert(right, 7, birth(7)));
  EXPECT_TRUE(left == right);

  auto copied = left;
  EXPECT_TRUE(copied == left);
  FlatU32Map<StructureBirthInfo> assigned;
  assigned = right;
  EXPECT_TRUE(assigned == right);
  ASSERT_TRUE(copied.erase(7));
  EXPECT_FALSE(copied == left);
  ASSERT_TRUE(insert(copied, 7, birth(7)));
  EXPECT_TRUE(copied == left);

  FlatU32Map<StructureBirthInfo> moved;
  moved = std::move(copied);
  EXPECT_TRUE(moved == left);
  EXPECT_TRUE(copied.empty());
  EXPECT_TRUE(insert(copied, 0, birth(0)));
}

TEST(FlatU32MapTest, MutableAndConstIteratorsExposeRealPairReferences) {
  FlatU32Map<std::uint32_t> map;
  ASSERT_TRUE(insert(map, 4, 10));
  ASSERT_TRUE(insert(map, 8, 20));

  for (auto &[key, value] : map) {
    static_assert(std::is_const_v<std::remove_reference_t<decltype(key)>>);
    static_assert(!std::is_const_v<std::remove_reference_t<decltype(value)>>);
    value += key;
  }
  auto mutableIt = map.find(4);
  ASSERT_NE(mutableIt, map.end());
  mutableIt->second = 99;
  EXPECT_EQ(mutableIt->first, 4U);
  EXPECT_EQ(mutableIt->second, 99U);

  const auto &constMap = map;
  for (const auto &[key, value] : constMap) {
    static_assert(std::is_const_v<std::remove_reference_t<decltype(key)>>);
    static_assert(std::is_const_v<std::remove_reference_t<decltype(value)>>);
    EXPECT_NE(key, 0U);
    EXPECT_NE(value, 0U);
  }
  const auto constIt = constMap.find(8);
  ASSERT_NE(constIt, constMap.end());
  EXPECT_EQ(constIt->first, 8U);
  EXPECT_EQ(constIt->second, 28U);
  EXPECT_EQ(std::ranges::distance(map), 2);
}

TEST(FlatU32MapTest, PreparedCommitDoesNotAllocate) {
  FailOnceResource resource;
  FlatU32Map<std::uint32_t> map{&resource};
  auto prepared = map.prepareInsert(42);
  ASSERT_TRUE(prepared);
  const auto attemptsBeforeCommit = resource.allocationAttempts();

  resource.denyAllocations(true);
  ASSERT_TRUE(map.commitInsert(std::move(*prepared), 84));
  EXPECT_EQ(resource.allocationAttempts(), attemptsBeforeCommit);
  resource.denyAllocations(false);
  EXPECT_EQ(map.find(42)->second, 84U);
}

TEST(FlatU32MapTest, GrowthAllocationFailurePreservesLogicalContents) {
  for (std::size_t failAfter = 0; failAfter < 2; ++failAfter) {
    FailOnceResource resource;
    FlatU32Map<std::uint32_t> map{&resource};
    for (std::uint32_t key = 0; key < 5; ++key) {
      ASSERT_TRUE(insert(map, key, key * 3U));
    }
    ASSERT_EQ(map.size(), 5U);
    const auto oldCapacity = map.capacity();
    ASSERT_EQ(oldCapacity, 8U);

    resource.failAfter(failAfter);
    const auto failed = map.prepareInsert(77);
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error(), FlatU32MapPrepareFailure::AllocationUnavailable);
    EXPECT_EQ(map.size(), 5U);
    EXPECT_EQ(map.capacity(), oldCapacity);
    for (std::uint32_t key = 0; key < 5; ++key) {
      ASSERT_TRUE(map.contains(key));
      EXPECT_EQ(map.find(key)->second, key * 3U);
    }
    EXPECT_FALSE(map.contains(77));

    ASSERT_TRUE(insert(map, 77, 231));
    EXPECT_EQ(map.size(), 6U);
    EXPECT_TRUE(map.contains(77));
  }
}

TEST(FlatU32MapTest, TombstoneCleanupFailurePreservesLogicalContents) {
  for (std::size_t failAfter = 0; failAfter < 2; ++failAfter) {
    FailOnceResource resource;
    FlatU32Map<std::uint32_t> map{&resource};
    for (std::uint32_t key = 0; key < 5; ++key) {
      ASSERT_TRUE(insert(map, key, key + 10U));
    }
    ASSERT_TRUE(map.erase(0));
    ASSERT_EQ(map.size(), 4U);
    const auto oldCapacity = map.capacity();

    resource.failAfter(failAfter);
    const auto failed = map.prepareInsert(77);
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error(), FlatU32MapPrepareFailure::AllocationUnavailable);
    EXPECT_EQ(map.size(), 4U);
    EXPECT_EQ(map.capacity(), oldCapacity);
    EXPECT_FALSE(map.contains(0));
    for (std::uint32_t key = 1; key < 5; ++key) {
      ASSERT_TRUE(map.contains(key));
      EXPECT_EQ(map.find(key)->second, key + 10U);
    }
    ASSERT_TRUE(insert(map, 77, 87));
  }
}

TEST(FlatU32MapTest, CopyConstructionAllocationFailureKeepsSourceUsable) {
  for (std::size_t failAfter = 0; failAfter < 2; ++failAfter) {
    FailOnceResource resource;
    FlatU32Map<std::uint32_t> source{&resource};
    ASSERT_TRUE(insert(source, 5, 15));
    ASSERT_TRUE(insert(source, 6, 18));

    resource.failAfter(failAfter);
    bool allocationFailed = false;
    try {
      const FlatU32Map<std::uint32_t> copy{source};
      (void)copy;
    } catch (const std::bad_alloc &) {
      allocationFailed = true;
    }
    EXPECT_TRUE(allocationFailed);
    ASSERT_EQ(source.size(), 2U);
    EXPECT_EQ(source.find(5)->second, 15U);
    EXPECT_EQ(source.find(6)->second, 18U);
  }
}

TEST(FlatU32MapTest, CopyAssignmentFailureLeavesTargetUnchanged) {
  FlatU32Map<std::uint32_t> source;
  ASSERT_TRUE(insert(source, 100, 200));
  ASSERT_TRUE(insert(source, 101, 201));

  FailOnceResource resource;
  FlatU32Map<std::uint32_t> target{&resource};
  ASSERT_TRUE(insert(target, 1, 2));
  ASSERT_TRUE(insert(target, 3, 4));
  resource.failAfter(1); // entries allocation succeeds; state allocation fails

  EXPECT_THROW(target = source, std::bad_alloc);
  EXPECT_EQ(target.size(), 2U);
  ASSERT_TRUE(target.contains(1));
  EXPECT_EQ(target.find(1)->second, 2U);
  ASSERT_TRUE(target.contains(3));
  EXPECT_EQ(target.find(3)->second, 4U);
  EXPECT_FALSE(target.contains(100));
}

TEST(FlatU32MapTest, DuplicatePrepareIsDistinctFromCapacityFailure) {
  FlatU32Map<std::uint32_t> map;
  ASSERT_TRUE(insert(map, 3, 8));
  const auto duplicate = map.prepareInsert(3);
  ASSERT_FALSE(duplicate);
  EXPECT_EQ(duplicate.error(), FlatU32MapPrepareFailure::AlreadyPresent);
}

} // namespace

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <ranges>
#include <type_traits>
#include <vector>

#include "common/xanadu/vortex/operand_list.hpp"

namespace {

template <class T> struct FailingAllocator {
  using value_type                      = T;
  static inline bool failNextAllocation = false;

  FailingAllocator() noexcept = default;
  template <class U> FailingAllocator(const FailingAllocator<U> &) noexcept {}

  [[nodiscard]] T *allocate(const std::size_t count) {
    if (failNextAllocation) {
      failNextAllocation = false;
      throw std::bad_alloc{};
    }
    return std::allocator<T>{}.allocate(count);
  }

  void deallocate(T *memory, const std::size_t count) noexcept {
    std::allocator<T>{}.deallocate(memory, count);
  }

  template <class U>
  bool operator==(const FailingAllocator<U> &) const noexcept {
    return true;
  }
};

using TestOperandList =
    zigzag::vortex::BasicOperandList<FailingAllocator<zigzag::CellRef>>;
static_assert(std::ranges::contiguous_range<zigzag::vortex::OperandList>);
static_assert(std::ranges::sized_range<zigzag::vortex::OperandList>);

TEST(OperandListTest, SupportsInlineBoundarySpillAndLargeWings) {
  for (const std::size_t count : {0U, 8U, 9U, 1024U}) {
    zigzag::vortex::OperandList operands;
    for (std::size_t i = 0; i < count; ++i)
      operands.push_back(static_cast<zigzag::CellRef>(i + 10));

    ASSERT_EQ(operands.size(), count);
    EXPECT_EQ(operands.empty(), count == 0);
    for (std::size_t i = 0; i < count; ++i) EXPECT_EQ(operands[i], i + 10);
    if (count != 0) {
      EXPECT_EQ(operands.front(), 10U);
      EXPECT_EQ(operands.back(), count + 9U);
    }
    auto view = operands.span();
    EXPECT_EQ(view.size(), count);
    if (count > 1) {
      view[1] = 777;
      EXPECT_EQ(operands[1], 777U);
    }
  }
}

TEST(OperandListTest, CopyAndMovePreserveBothRepresentationsAndSourceValidity) {
  zigzag::vortex::OperandList inlineValues;
  for (zigzag::CellRef value = 0; value < 8; ++value)
    inlineValues.push_back(value);
  auto inlineCopy = inlineValues;
  EXPECT_EQ(inlineCopy, inlineValues);
  auto inlineMove = std::move(inlineCopy);
  EXPECT_EQ(inlineMove, inlineValues);
  EXPECT_TRUE(inlineCopy.empty());
  inlineCopy.push_back(81);
  EXPECT_EQ(inlineCopy.front(), 81U);

  zigzag::vortex::OperandList spilledValues;
  for (zigzag::CellRef value = 0; value < 64; ++value)
    spilledValues.push_back(value + 100);
  auto spilledCopy = spilledValues;
  EXPECT_EQ(spilledCopy, spilledValues);
  auto spilledMove = std::move(spilledCopy);
  EXPECT_EQ(spilledMove, spilledValues);
  EXPECT_TRUE(spilledCopy.empty());
  spilledCopy.push_back(999);
  EXPECT_EQ(spilledCopy.front(), 999U);

  inlineMove = spilledValues;
  EXPECT_EQ(inlineMove, spilledValues);
  inlineMove = std::move(inlineCopy);
  ASSERT_EQ(inlineMove.size(), 1U);
  EXPECT_EQ(inlineMove.front(), 81U);
  EXPECT_TRUE(inlineCopy.empty());
}

TEST(OperandListTest, SpilledCopyAssignmentFailurePreservesInlineDestination) {
  TestOperandList source;
  for (std::uint32_t value = 0; value < 12; ++value)
    source.push_back(value + 100);

  TestOperandList destination;
  destination.push_back(7);
  destination.push_back(8);
  destination.push_back(9);

  FailingAllocator<std::uint32_t>::failNextAllocation = true;
  EXPECT_THROW(destination = source, std::bad_alloc);
  ASSERT_EQ(destination.size(), 3U);
  EXPECT_EQ(destination[0], 7U);
  EXPECT_EQ(destination[1], 8U);
  EXPECT_EQ(destination[2], 9U);

  destination = source;
  EXPECT_EQ(destination.size(), source.size());
  for (std::size_t i = 0; i < source.size(); ++i)
    EXPECT_EQ(destination[i], source[i]);
}

TEST(OperandListTest, NinthElementAllocationFailureLeavesInlinePrefixIntact) {
  TestOperandList operands;
  for (std::uint32_t value = 0; value < 8; ++value)
    operands.push_back(value + 20);

  FailingAllocator<std::uint32_t>::failNextAllocation = true;
  EXPECT_THROW(operands.push_back(28), std::bad_alloc);
  ASSERT_EQ(operands.size(), 8U);
  for (std::size_t i = 0; i < 8; ++i) EXPECT_EQ(operands[i], i + 20);

  operands.push_back(28);
  ASSERT_EQ(operands.size(), 9U);
  EXPECT_EQ(operands.back(), 28U);
  operands.clear();
  EXPECT_TRUE(operands.empty());
  operands.push_back(91);
  ASSERT_EQ(operands.size(), 1U);
  EXPECT_EQ(operands.front(), 91U);
}

TEST(OperandListTest, HasNoImplicitVectorConversionAndMeetsTargetSize) {
  static_assert(!std::is_convertible_v<zigzag::vortex::OperandList,
                                       std::vector<zigzag::CellRef>>);
  EXPECT_EQ(sizeof(zigzag::vortex::OperandList), 64U);
}

} // namespace

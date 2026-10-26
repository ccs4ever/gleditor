/**
 * @file cpp26_compat_test.cpp
 * @brief Native and fallback C++26 facade compatibility checks.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <new>
#include <ranges>
#include <span>
#include <type_traits>
#include <vector>

#include <gleditor/cpp26.hpp>
#include <gleditor/cpp26_concat.hpp>
#include <gleditor/cpp26_inplace_vector.hpp>

namespace {

int twice(const int value) noexcept { return value * 2; }

int invoke(gleditor::cpp26::function_ref<int(int)> callback) {
  return callback(21);
}

TEST(Cpp26CompatibilityTest, FunctionRefBorrowsCallableForTheCall) {
  int base      = 20;
  auto callback = [&base](const int value) { return base + value; };
  EXPECT_EQ(invoke(callback), 41);
  EXPECT_EQ(invoke([](const int value) { return value + 1; }), 22);
  base = 1;
  EXPECT_EQ(invoke(callback), 22);
}

TEST(Cpp26CompatibilityTest, FunctionRefSupportsNoexceptFunctionPointer) {
  using Ref = gleditor::cpp26::function_ref<int(int) noexcept>;
  static_assert(std::is_trivially_copyable_v<Ref>);
  const Ref callback{twice};
  EXPECT_EQ(callback(21), 42);
}

TEST(Cpp26CompatibilityTest, OptionalReferenceBorrowsMutatesAndRebinds) {
  using Ref  = gleditor::cpp26::optional<int &>;
  int first  = 3;
  int second = 8;
  Ref borrowed{first};

  ASSERT_TRUE(borrowed.has_value());
  EXPECT_EQ(&*borrowed, &first);
  *borrowed = 5;
  EXPECT_EQ(first, 5);

  borrowed = second;
  EXPECT_EQ(&*borrowed, &second);
  EXPECT_EQ(first, 5);
  EXPECT_EQ(second, 8);

  Ref another{first};
  borrowed = another;
  EXPECT_EQ(&*borrowed, &first);
  borrowed = gleditor::cpp26::nullopt;
  EXPECT_FALSE(borrowed.has_value());
}

TEST(Cpp26CompatibilityTest, OptionalReferenceIsZeroOrOneElementRange) {
  int value = 13;
  gleditor::cpp26::optional<int &> present{value};
  std::size_t count = 0;
  for (int &element : present) {
    ++count;
    EXPECT_EQ(&element, &value);
    element = 21;
  }
  EXPECT_EQ(count, 1U);
  EXPECT_EQ(value, 21);

  gleditor::cpp26::optional<int &> empty;
  count = 0;
  for (int &element : empty) {
    (void)element;
    ++count;
  }
  EXPECT_EQ(count, 0U);
}

TEST(Cpp26CompatibilityTest, InplaceVectorProvidesBoundedContiguousStorage) {
  gleditor::cpp26::inplace_vector<int, 32> points;
  static_assert(std::ranges::contiguous_range<decltype(points)>);

  points.resize(32);
  const std::span<int> view{points.data(), points.size()};
  for (std::size_t i = 0; i < view.size(); ++i) {
    view[i] = static_cast<int>(i);
  }

  EXPECT_EQ(points.front(), 0);
  EXPECT_EQ(points.back(), 31);
  EXPECT_EQ(points.size(), points.capacity());
  EXPECT_THROW(points.resize(33), std::bad_alloc);
  EXPECT_EQ(points.size(), 32U);
}

TEST(Cpp26CompatibilityTest, ConcatPreservesBorrowedOrderAndDuplicates) {
  const std::vector<int> first{1, 2, 2};
  const std::vector<int> second{2, 3};
  auto view = gleditor::cpp26::views::concat(std::span<const int>{first},
                                             std::span<const int>{second});
  static_assert(std::ranges::input_range<decltype(view)>);
  static_assert(std::is_same_v<std::ranges::range_reference_t<decltype(view)>,
                               const int &>);

  EXPECT_EQ(&*view.begin(), first.data());
  std::vector<int> result;
  std::ranges::copy(view, std::back_inserter(result));
  EXPECT_EQ(result, (std::vector<int>{1, 2, 2, 2, 3}));

  const auto collect = [](std::span<const int> a, std::span<const int> b) {
    std::vector<int> values;
    for (int value : gleditor::cpp26::views::concat(a, b)) {
      values.push_back(value);
    }
    return values;
  };
  EXPECT_TRUE(collect({}, {}).empty());
  EXPECT_EQ(collect({}, second), second);
  EXPECT_EQ(collect(first, {}), first);
}

} // namespace

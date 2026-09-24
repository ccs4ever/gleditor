/**
 * @file cpp26_compat_test.cpp
 * @brief Native and fallback C++26 callable compatibility checks.
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

#include "common/cpp26.hpp"
#include "common/cpp26_concat.hpp"
#include "common/cpp26_inplace_vector.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace {

int twice(const int value) noexcept { return value * 2; }

int invoke(common::cpp26::function_ref<int(int)> callback) {
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
  using Ref = common::cpp26::function_ref<int(int) noexcept>;
  static_assert(std::is_trivially_copyable_v<Ref>);
  const Ref callback{twice};
  EXPECT_EQ(callback(21), 42);
}

TEST(Cpp26CompatibilityTest, InplaceVectorProvidesBoundedContiguousStorage) {
  common::cpp26::inplace_vector<int, 17> points;
  static_assert(std::ranges::contiguous_range<decltype(points)>);

  points.resize(17);
  const std::span<int> view{points.data(), points.size()};
  for (std::size_t i = 0; i < view.size(); ++i) {
    view[i] = static_cast<int>(i);
  }

  EXPECT_EQ(points.front(), 0);
  EXPECT_EQ(points.back(), 16);
  EXPECT_EQ(points.size(), points.capacity());
  EXPECT_THROW(points.resize(18), std::bad_alloc);
  EXPECT_EQ(points.size(), 17U);
}

TEST(Cpp26CompatibilityTest, ConcatPreservesBorrowedCellOrderAndDuplicates) {
  using zigzag::CellRef;
  const std::vector<CellRef> first{1, 2, 2};
  const std::vector<CellRef> second{2, 3};
  auto view = common::cpp26::views::concat(std::span<const CellRef>{first},
                                           std::span<const CellRef>{second});
  static_assert(std::ranges::input_range<decltype(view)>);
  static_assert(std::is_same_v<std::ranges::range_reference_t<decltype(view)>,
                               const CellRef &>);

  EXPECT_EQ(&*view.begin(), first.data());
  std::vector<CellRef> result;
  std::ranges::copy(view, std::back_inserter(result));
  EXPECT_EQ(result, (std::vector<CellRef>{1, 2, 2, 2, 3}));

  const auto collect = [](std::span<const CellRef> a,
                          std::span<const CellRef> b) {
    std::vector<CellRef> values;
    for (CellRef value : common::cpp26::views::concat(a, b)) {
      values.push_back(value);
    }
    return values;
  };
  EXPECT_TRUE(collect({}, {}).empty());
  EXPECT_EQ(collect({}, second), second);
  EXPECT_EQ(collect(first, {}), first);
}

} // namespace

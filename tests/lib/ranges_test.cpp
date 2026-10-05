#include <gleditor/ranges.hpp>
#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace gleditor {
namespace {

TEST(RangesTest, CollectsBorrowedValuesWithoutMovingFromLvalues) {
  std::array<std::string, 2> source{"first", "second"};
  const auto collected = toVector(std::span(source));
  static_assert(std::is_same_v<decltype(toVector(std::span(source))),
                               std::vector<std::string>>);
  EXPECT_EQ(collected, (std::vector<std::string>{"first", "second"}));
  EXPECT_EQ(source[0], "first");
  EXPECT_EQ(source[1], "second");
  EXPECT_GE(collected.capacity(), source.size());
}

TEST(RangesTest, CollectsSinglePassNonCommonInputRange) {
  std::istringstream input("2 3 5 7");
  auto values = std::ranges::istream_view<int>(input);
  static_assert(!std::ranges::common_range<decltype(values)>);
  static_assert(!std::ranges::forward_range<decltype(values)>);
  EXPECT_EQ(values | toVector(), (std::vector<int>{2, 3, 5, 7}));
  EXPECT_TRUE(input.eof());
}

TEST(RangesTest, MovesMoveOnlyPrvaluesAndKeepsOwningPipelinesValid) {
  const auto values = std::views::iota(1, 4) |
                      std::views::transform([](const int value) {
                        return std::make_unique<int>(value);
                      }) |
                      toVector;
  ASSERT_EQ(values.size(), 3);
  EXPECT_EQ(*values[0], 1);
  EXPECT_EQ(*values[2], 3);

  const auto filtered =
      std::views::iota(1, 5) | toVector() |
      std::views::filter([](const int value) { return value % 2 == 0; }) |
      toVector();
  EXPECT_EQ(filtered, (std::vector<int>{2, 4}));
}

TEST(RangesTest, MaterializesProxyReferencesAsTheirValueType) {
  std::vector<bool> flags{true, false, true};
  const auto collected = toVector(flags);
  static_assert(std::is_same_v<decltype(toVector(flags)), std::vector<bool>>);
  EXPECT_EQ(collected, flags);
  EXPECT_TRUE(toVector(std::views::empty<int>).empty());
}

} // namespace
} // namespace gleditor

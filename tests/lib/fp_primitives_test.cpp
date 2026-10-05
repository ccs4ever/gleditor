#include <gleditor/ranges.hpp>
#include <gleditor/sentinel.hpp>
#include <gleditor/stepped_view.hpp>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace gleditor {
namespace {

TEST(FpPrimitivesTest, FromAndToSentinelRoundTrip) {
  constexpr std::uint32_t kSentinel = 0xFFFF'FFFFU;
  static_assert(fromSentinel<kSentinel>(kSentinel) == std::nullopt);
  static_assert(fromSentinel<kSentinel>(42U) ==
                std::optional<std::uint32_t>{42U});
  static_assert(toSentinel<kSentinel>(std::optional<std::uint32_t>{42U}) ==
                42U);
  static_assert(toSentinel<kSentinel>(
                    std::optional<std::uint32_t>{std::nullopt}) == kSentinel);

  EXPECT_FALSE(fromSentinel<kSentinel>(kSentinel).has_value());
  EXPECT_EQ(fromSentinel<kSentinel>(100U), 100U);
  EXPECT_EQ(toSentinel<kSentinel>(std::optional<std::uint32_t>{100U}), 100U);
  EXPECT_EQ(toSentinel<kSentinel>(std::nullopt), kSentinel);
}

TEST(FpPrimitivesTest, RangesNthOfAndSingleOf) {
  const std::vector<int> numbers{10, 20, 30};
  EXPECT_EQ(nthOf(numbers, 0), 10);
  EXPECT_EQ(nthOf(numbers, 1), 20);
  EXPECT_EQ(nthOf(numbers, 2), 30);
  EXPECT_EQ(nthOf(numbers, 3), std::nullopt);

  const std::vector<int> emptyVec;
  EXPECT_EQ(nthOf(emptyVec, 0), std::nullopt);
  EXPECT_EQ(singleOf(emptyVec), std::nullopt);

  const std::vector<int> singleVec{99};
  EXPECT_EQ(singleOf(singleVec), 99);

  EXPECT_EQ(singleOf(numbers), std::nullopt);
}

TEST(FpPrimitivesTest, SteppedPathViewLinearAndCycleTermination) {
  // Linear sequence: 1 -> 2 -> 3 -> stop (nullopt)
  auto nextStep = [](int x) -> std::optional<int> {
    if (x < 3) {
      return x + 1;
    }
    return std::nullopt;
  };

  std::vector<int> collected;
  for (int val : stepped_path_view(nextStep, std::optional{1})) {
    collected.push_back(val);
  }
  EXPECT_EQ(collected, (std::vector<int>{1, 2, 3}));

  // Self loop: 5 -> 5 (stops on self-loop)
  auto selfLoopStep = [](int) -> std::optional<int> { return 5; };
  std::vector<int> loopRes;
  for (int val : stepped_path_view(selfLoopStep, std::optional{5})) {
    loopRes.push_back(val);
  }
  EXPECT_EQ(loopRes, (std::vector<int>{5}));

  // Ring: 1 -> 2 -> 1 (stops when returning to start)
  auto ringStep = [](int x) -> std::optional<int> {
    if (x == 1) return 2;
    return 1;
  };
  std::vector<int> ringRes;
  for (int val : stepped_path_view(ringStep, std::optional{1})) {
    ringRes.push_back(val);
  }
  EXPECT_EQ(ringRes, (std::vector<int>{1, 2}));
}

TEST(FpPrimitivesTest, FilterPresentRangeAdaptor) {
  const std::vector<std::optional<int>> options{1, std::nullopt, 3,
                                                std::nullopt, 5};
  std::vector<int> unwrapped;
  for (int val : options | filter_present) {
    unwrapped.push_back(val);
  }
  EXPECT_EQ(unwrapped, (std::vector<int>{1, 3, 5}));
}

} // namespace
} // namespace gleditor

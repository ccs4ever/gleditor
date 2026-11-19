/**
 * @file vlace_render_test.cpp
 * @brief Rendering long lists: a field that already holds a constructed term is
 * read, not forced, so a literal list costs no VM run per element.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "common/xanadu/vlace/driver.hpp"

namespace {
using namespace xanadu::vlace;

std::string intList(std::size_t count) {
  std::string src = "[";
  for (std::size_t i = 0; i < count; ++i) {
    if (i != 0) src += ", ";
    src += std::to_string(i);
  }
  return src + "]";
}

TEST(VlaceRenderTest, ShortListsRenderInFull) {
  Driver d;
  const auto r = d.run("[1, 2, 3]");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->value, "[1, 2, 3]");
}

// 100,000 elements, rendered to the default budget of 10,000 and then
// truncated with ", ...]". Each tail used to be forced by its own VM run, whose
// cost grew with every earlier run (3,000 elements took 2 s, 12,000 took 47 s).
TEST(VlaceRenderTest, ALongLiteralListRendersToTheBudgetInSeconds) {
  const auto start = std::chrono::steady_clock::now();
  Driver d;
  const auto r = d.run(intList(100'000));
  ASSERT_TRUE(r.has_value());
  const auto seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  EXPECT_EQ(r->value.substr(0, 14), "[0, 1, 2, 3, 4");
  EXPECT_EQ(r->value.substr(r->value.size() - 22), "9997, 9998, 9999, ...]");
  EXPECT_LT(seconds, 30.0);
}

// A lazily built list of 4,000 elements: each tail is its own VM run. Before
// the frame opcodes journalled their changes this took quadratic time, and past
// about 3,500 elements the VM's lifetime backing-cell count (1,048,576) refused
// the run.
TEST(VlaceRenderTest, ALazilyBuiltListPastTheOldBackingCellLimitRenders) {
  const auto start = std::chrono::steady_clock::now();
  Driver d;
  const auto r = d.run(
      "from n = n : from (n + 1) ; take k xs = if k == 0 then [] else case xs "
      "of [] => [] ; (h : t) => h : take (k - 1) t ; take 4000 (from 1)");
  ASSERT_TRUE(r.has_value()) << r.error().detail;
  EXPECT_EQ(r->value.substr(0, 14), "[1, 2, 3, 4, 5");
  const auto seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  EXPECT_LT(seconds, 60.0);
}

// A list that fits the budget has no truncation marker.
TEST(VlaceRenderTest, AListWithinTheBudgetIsNotTruncated) {
  Driver d;
  const auto r = d.run(intList(9'000));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->value.substr(r->value.size() - 11), "8998, 8999]");
  EXPECT_EQ(r->value.find("..."), std::string::npos);
}

} // namespace

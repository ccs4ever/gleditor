/**
 * @file vicon_driver_test.cpp
 * @brief Tests for the vicon driver and runtime execution.
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/xanadu/vicon/driver.hpp"
#include "common/xanadu/vicon/lexer.hpp"

namespace {

using namespace xanadu::vicon;

TEST(ViconDriverTest, BasicIntegerSequence) {
  Driver driver;
  auto res = driver.runAll("1 to 3");
  ASSERT_TRUE(res.has_value());
  const std::vector<std::string> want = {"1", "2", "3"};
  EXPECT_EQ(*res, want);
}

TEST(ViconDriverTest, RadixDigitsRejectOutOfBaseAndAcceptUpperBoundary) {
  auto valid = Lexer("36rZ").tokenize();
  ASSERT_EQ(valid.front().kind, TokenKind::IntegerLiteral);
  EXPECT_EQ(valid.front().intValue, 35);

  auto invalid = Lexer("2r2").tokenize();
  ASSERT_EQ(invalid.front().kind, TokenKind::Error);
  EXPECT_EQ(invalid.front().stringValue,
            "digit out of range for radix literal");
}

TEST(ViconDriverTest, Alternation) {
  Driver driver;
  auto res = driver.runAll("10 | 20 | 30");
  ASSERT_TRUE(res.has_value());
  const std::vector<std::string> want = {"10", "20", "30"};
  EXPECT_EQ(*res, want);
}

TEST(ViconDriverTest, Limitation) {
  Driver driver;
  auto res = driver.runAll("(1 to 10) \\ 3");
  ASSERT_TRUE(res.has_value());
  const std::vector<std::string> want = {"1", "2", "3"};
  EXPECT_EQ(*res, want);
}

TEST(ViconDriverTest, EveryLoop) {
  Driver driver;
  auto res = driver.runAll("every write(1 to 3)");
  ASSERT_TRUE(res.has_value());
  const std::vector<std::string> want = {"1", "2", "3", ""};
  EXPECT_EQ(*res, want);
}

TEST(ViconDriverTest, WhileLoop) {
  Driver driver;
  auto res = driver.runAll("i := 0; while i < 3 do { write(i); i := i + 1 }");
  ASSERT_TRUE(res.has_value());
  const std::vector<std::string> want = {"0", "1", "2", ""};
  EXPECT_EQ(*res, want);
}

TEST(ViconDriverTest, UntilLoop) {
  Driver driver;
  auto res = driver.runAll("i := 0; until i >= 3 do { write(i); i := i + 1 }");
  ASSERT_TRUE(res.has_value());
  const std::vector<std::string> want = {"0", "1", "2", ""};
  EXPECT_EQ(*res, want);
}

} // namespace

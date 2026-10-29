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

TEST(ViconDriverTest, LimitationRejectsNonpositiveCounts) {
  Driver driver;
  auto zero = driver.runAll("(1 to 4) \\ 0");
  ASSERT_TRUE(zero.has_value());
  EXPECT_TRUE(zero->empty());

  auto negative = driver.runAll("(1 to 4) \\ -2");
  ASSERT_TRUE(negative.has_value());
  EXPECT_TRUE(negative->empty());
}

TEST(ViconDriverTest, NestedAndDynamicLimitCountersSurviveResume) {
  Driver nested;
  auto nestedResults = nested.runAll("((1 to 4) \\ 2) \\ 1");
  ASSERT_TRUE(nestedResults.has_value());
  EXPECT_EQ(*nestedResults, (std::vector<std::string>{"1"}));

  Driver dynamic;
  auto dynamicResults = dynamic.runAll("n := 3; (1 to 10) \\ n");
  ASSERT_TRUE(dynamicResults.has_value());
  EXPECT_EQ(*dynamicResults, (std::vector<std::string>{"1", "2", "3"}));
}

TEST(ViconDriverTest, LimitTokenReinitializesOnEachLoopActivation) {
  Driver driver;
  auto res = driver.runAll("every (1 to 2) do write((1 to 10) \\ 2)");
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(*res, (std::vector<std::string>{"1", "2", "1", "2", ""}));
}

TEST(ViconDriverTest, IfResumeDoesNotReenterElseAfterThenProduced) {
  Driver driver;
  auto res = driver.runAll("if 1 to 2 then 7 else 9");
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(*res, (std::vector<std::string>{"7"}));

  Driver thenGenerator;
  auto thenResults = thenGenerator.runAll("if 1 then (7 | 8) else 9");
  ASSERT_TRUE(thenResults.has_value());
  EXPECT_EQ(*thenResults, (std::vector<std::string>{"7", "8"}));
}

TEST(ViconDriverTest, RecursiveProcedureCycleIsRefusedAtCompileTime) {
  Driver driver;
  auto selfRecursive = driver.start("procedure f() f() end; f()");
  ASSERT_FALSE(selfRecursive.has_value());
  EXPECT_EQ(selfRecursive.error().phase, "compile");
  EXPECT_NE(selfRecursive.error().detail.find("recursive procedure"),
            std::string::npos);

  auto mutuallyRecursive =
      driver.start("procedure f() g() end; procedure g() f() end; f()");
  ASSERT_FALSE(mutuallyRecursive.has_value());
  EXPECT_EQ(mutuallyRecursive.error().phase, "compile");
  EXPECT_NE(mutuallyRecursive.error().detail.find("recursive procedure"),
            std::string::npos);
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

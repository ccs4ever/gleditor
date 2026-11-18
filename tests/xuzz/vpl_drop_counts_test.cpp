/**
 * @file vpl_drop_counts_test.cpp
 * @brief Drop counts: a count that is not a whole number is refused by name
 * (at compile time for a literal, at run time for a computed one), and a
 * count beyond the rank's length drops everything in a handful of cycles.
 */
#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "common/xanadu/vpl/vpl_engine.hpp"

namespace xanadu::vpl {
namespace {

using Values = std::vector<std::int64_t>;

class VplDropCountsTest : public ::testing::Test {
protected:
  Values values(const std::string &source) {
    const auto view = engine.evaluate(source);
    Values out;
    for (const auto cell : view.collectCells(engine.arena())) {
      out.push_back(engine.arena().asInt64(cell).value_or(-1));
    }
    return out;
  }

  /// The message of the failure, or empty when the source was answered.
  std::string refusal(const std::string &source) {
    try {
      engine.evaluate(source);
    } catch (const VPLExecutionError &error) {
      return error.what();
    } catch (const std::exception &error) {
      return error.what();
    }
    return {};
  }

  void expectRefused(const std::string &source, const std::string &text,
                     ExecutionError::Stage stage) {
    try {
      engine.evaluate(source);
      FAIL() << source << " was answered";
    } catch (const VPLExecutionError &error) {
      EXPECT_EQ(error.stage(), stage) << source;
      EXPECT_NE(std::string{error.what()}.find(text), std::string::npos)
          << source << ": " << error.what();
    }
  }

  VPLEngine engine;
};

TEST_F(VplDropCountsTest, ATextLiteralCountIsRefusedAtCompileTime) {
  expectRefused("A ← 5⍳d.1; \"x\" ↓ A", "drop count must be a number",
                ExecutionError::Stage::Compile);
  expectRefused("A ← 5⍳d.1; \"2\" ↓ A", "drop count must be a number",
                ExecutionError::Stage::Compile);
}

TEST_F(VplDropCountsTest, ATextCountHeldInAVariableFailsAtRunTime) {
  expectRefused("A ← 5⍳d.1; n ← \"2\"; n ↓ A", "drop count is not a number",
                ExecutionError::Stage::Runtime);
}

TEST_F(VplDropCountsTest, ANonIntegerComputedCountFailsAtRunTime) {
  expectRefused("A ← 5⍳d.1; n ← 0.5+1; n ↓ A",
                "drop count is not a whole number", ExecutionError::Stage::Runtime);
  expectRefused("A ← 5⍳d.1; (0.5+1.25) ↓ A", "drop count is not a whole number",
                ExecutionError::Stage::Runtime);
}

TEST_F(VplDropCountsTest, ANonIntegerLiteralCountIsRefusedAtCompileTime) {
  expectRefused("A ← 5⍳d.1; 1.5 ↓ A", "DOMAIN ERROR",
                ExecutionError::Stage::Compile);
}

TEST_F(VplDropCountsTest, AWholeNumberInADoubleIsAccepted) {
  EXPECT_EQ(values("A ← 5⍳d.1; n ← 0.5+1.5; n ↓ A"), (Values{3, 4, 5}));
  EXPECT_EQ(values("A ← 5⍳d.1; n ← 1+1; n ↓ A"), (Values{3, 4, 5}));
  EXPECT_EQ(values("A ← 5⍳d.1; 0 ↓ A"), (Values{1, 2, 3, 4, 5}));
}

TEST_F(VplDropCountsTest, ACountBeyondTheLengthDropsEverythingCheaply) {
  EXPECT_EQ(values("A ← 5⍳d.1; ≢ 1000000000 ↓ A"), (Values{0}));
  EXPECT_EQ(values("A ← 5⍳d.1; +/ 1000000000 ↓ A"), (Values{0}));
  const auto before = engine.totalCycles();
  values("A ← 5⍳d.1; 1000000000 ↓ A");
  EXPECT_LT(engine.totalCycles() - before, 5000u);
}

TEST_F(VplDropCountsTest, ANegativeCountIsStillRefused) {
  EXPECT_FALSE(refusal("A ← 5⍳d.1; n ← 0-1; n ↓ A").empty());
}

} // namespace
} // namespace xanadu::vpl

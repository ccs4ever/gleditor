/**
 * @file vpl_catenate_ravel_test.cpp
 * @brief Catenate and ravel never link into an operand's cells: results are
 * fresh cells, operands are untouched, at source level through the real
 * compile-and-run path.
 */
#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "common/xanadu/vpl/vpl_engine.hpp"

namespace xanadu::vpl {
namespace {

using Values = std::vector<std::int64_t>;

class VplCatenateRavelTest : public ::testing::Test {
protected:
  Values values(const std::string &source) {
    const auto view = engine.evaluate(source);
    Values out;
    for (const auto cell : view.collectCells(engine.arena())) {
      const auto value = engine.arena().asInt64(cell);
      EXPECT_TRUE(value.has_value()) << source;
      out.push_back(value.value_or(-1));
    }
    return out;
  }

  std::vector<zigzag::CellRef> cells(const std::string &source) {
    return engine.evaluate(source).collectCells(engine.arena());
  }

  VPLEngine engine;
};

TEST_F(VplCatenateRavelTest, RepeatedPrependsToOneBoundOperandAreIndependent) {
  values("A ← 1 2 3");
  const auto before = cells("A");
  EXPECT_EQ(values("B ← 5 , A"), (Values{5, 1, 2, 3}));
  EXPECT_EQ(values("C ← 6 , A"), (Values{6, 1, 2, 3}));
  EXPECT_EQ(values("B"), (Values{5, 1, 2, 3}));
  EXPECT_EQ(values("A"), (Values{1, 2, 3}));
  EXPECT_EQ(cells("A"), before);
}

TEST_F(VplCatenateRavelTest, TwoPrependsOfTheSameScalarCatenateWhole) {
  values("A ← 1 2 3");
  values("B ← 0 , A");
  values("C ← 0 , A");
  EXPECT_EQ(values("B , C"), (Values{0, 1, 2, 3, 0, 1, 2, 3}));
  EXPECT_EQ(values("A"), (Values{1, 2, 3}));
}

TEST_F(VplCatenateRavelTest, PrependsInsideOneExpressionDoNotShareTheOperand) {
  values("A ← 1 2 3");
  EXPECT_EQ(values("(0 , A) , (0 , A)"), (Values{0, 1, 2, 3, 0, 1, 2, 3}));
  EXPECT_EQ(values("+/ (0 , A) , (9 , A)"), (Values{21}));
  EXPECT_EQ(values("(0 , A) + (9 , A)"), (Values{9, 2, 4, 6}));
  EXPECT_EQ(values("A"), (Values{1, 2, 3}));
}

TEST_F(VplCatenateRavelTest, PrependsToALiteralRankAreIndependent) {
  EXPECT_EQ(values("(0 , 1 2 3) , (0 , 1 2 3)"),
            (Values{0, 1, 2, 3, 0, 1, 2, 3}));
  EXPECT_EQ(values("+/ (0 , 1 2 3) , (9 , 1 2 3)"), (Values{21}));
}

TEST_F(VplCatenateRavelTest, PrependToAnIotaRankLeavesTheRankIntact) {
  values("A ← 3⍳d.1");
  const auto original = values("A");
  EXPECT_EQ(values("B ← 0 , A").size(), original.size() + 1);
  EXPECT_EQ(values("C ← 7 , A").front(), 7);
  EXPECT_EQ(values("B").front(), 0);
  EXPECT_EQ(values("B").size(), original.size() + 1);
  EXPECT_EQ(values("A"), original);
}

TEST_F(VplCatenateRavelTest, RankThenScalarAndRankThenRank) {
  values("A ← 1 2 3");
  EXPECT_EQ(values("A , 4"), (Values{1, 2, 3, 4}));
  EXPECT_EQ(values("A , 5"), (Values{1, 2, 3, 5}));
  EXPECT_EQ(values("A , A"), (Values{1, 2, 3, 1, 2, 3}));
  EXPECT_EQ(cells("A , A").size(), 6u);
  EXPECT_EQ(values("A"), (Values{1, 2, 3}));
}

TEST_F(VplCatenateRavelTest, SelfCatenationMintsFreshCells) {
  values("A ← 1 2 3");
  const auto original = cells("A");
  const auto joined   = cells("A , A");
  ASSERT_EQ(joined.size(), 6u);
  for (const auto cell : joined) {
    for (const auto source : original) EXPECT_NE(cell, source);
  }
  EXPECT_EQ(cells("A"), original);
}

TEST_F(VplCatenateRavelTest, ReversedOperandsReadWholeAndAreUntouched) {
  values("A ← 1 2 3");
  EXPECT_EQ(values("0 , ⌽A"), (Values{0, 3, 2, 1}));
  EXPECT_EQ(values("(⌽A) , 0"), (Values{3, 2, 1, 0}));
  EXPECT_EQ(values("(⌽A) , (⌽A)"), (Values{3, 2, 1, 3, 2, 1}));
  EXPECT_EQ(values("A , ⌽A"), (Values{1, 2, 3, 3, 2, 1}));
  EXPECT_EQ(values("(0 , ⌽A) , (9 , ⌽A)"), (Values{0, 3, 2, 1, 9, 3, 2, 1}));
  EXPECT_EQ(values("A"), (Values{1, 2, 3}));
  EXPECT_EQ(values("⌽A"), (Values{3, 2, 1}));
}

TEST_F(VplCatenateRavelTest, EmptyOperands) {
  // An empty rank cannot be carried between programs, so each case binds it
  // inside the program it is used in.
  values("A ← 1 2 3");
  EXPECT_EQ(values("E ← 0⍳d.1; E , A"), (Values{1, 2, 3}));
  EXPECT_EQ(values("E ← 0⍳d.1; A , E"), (Values{1, 2, 3}));
  EXPECT_EQ(values("E ← 0⍳d.1; E , E").size(), 0u);
  EXPECT_EQ(values("E ← 0⍳d.1; 7 , E"), (Values{7}));
  EXPECT_EQ(values("E ← 0⍳d.1; E , 7"), (Values{7}));
  EXPECT_EQ(values("E ← 0⍳d.1; (5 , E) , (6 , E)"), (Values{5, 6}));
  EXPECT_EQ(values("A"), (Values{1, 2, 3}));
}

TEST_F(VplCatenateRavelTest, RavelOfABoundRankIsAFreshCopy) {
  values("A ← 1 2 3");
  const auto original = cells("A");
  const auto raveled  = cells(", A");
  ASSERT_EQ(raveled.size(), 3u);
  for (std::size_t i = 0; i < raveled.size(); ++i) {
    EXPECT_NE(raveled[i], original[i]);
  }
  EXPECT_EQ(values(", ⌽A"), (Values{3, 2, 1}));
  EXPECT_EQ(cells("A"), original);
}

TEST_F(VplCatenateRavelTest, ScalarCatenationChainsStayIndependent) {
  EXPECT_EQ(values("1 , 2"), (Values{1, 2}));
  values("R ← 1 , 2 , 3");
  EXPECT_EQ(values("0 , R"), (Values{0, 1, 2, 3}));
  EXPECT_EQ(values("9 , R"), (Values{9, 1, 2, 3}));
  EXPECT_EQ(values("R"), (Values{1, 2, 3}));
}

} // namespace
} // namespace xanadu::vpl

/**
 * @file vpl_take_bound_test.cpp
 * @brief Card V6: take, negative drop and drop of a reversed rank as live
 * views with a far-end bound, and every operation that reads such a view.
 */
#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "common/xanadu/vpl/vpl_engine.hpp"

namespace xanadu::vpl {
namespace {

using Values = std::vector<std::int64_t>;

class VplTakeBoundTest : public ::testing::Test {
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

  std::string failure(const std::string &source) {
    try {
      engine.evaluate(source);
    } catch (const std::exception &error) {
      return error.what();
    }
    return {};
  }

  VPLEngine engine;
};

TEST_F(VplTakeBoundTest, TakeIsTheFirstCellsOfTheOperandsOwnRank) {
  const auto a = cells("A ← 5⍳d.1; A");
  ASSERT_EQ(a.size(), 5u);
  EXPECT_EQ(values("3 ↑ A"), (Values{1, 2, 3}));
  const auto taken = cells("3 ↑ A");
  ASSERT_EQ(taken.size(), 3u);
  for (std::size_t i = 0; i < taken.size(); ++i) EXPECT_EQ(taken[i], a[i]);
  EXPECT_EQ(values("≢ 3 ↑ A"), (Values{3}));
  EXPECT_EQ(values("5 ↑ A"), (Values{1, 2, 3, 4, 5}));
  EXPECT_EQ(values("1 ↑ A"), (Values{1}));
  EXPECT_EQ(values("A"), (Values{1, 2, 3, 4, 5}));
}

TEST_F(VplTakeBoundTest, TakeOfZeroIsEmptyAndKeepsItsAxis) {
  const auto view = engine.evaluate("A ← 5⍳d.1; 0 ↑ A");
  EXPECT_EQ(view.axes().size(), 1u);
  EXPECT_FALSE(view.origin().has_value());
  EXPECT_TRUE(view.collectCells(engine.arena()).empty());
  EXPECT_EQ(values("A ← 5⍳d.1; ≢ 0 ↑ A"), (Values{0}));
  EXPECT_EQ(values("A ← 5⍳d.1; +/ 0 ↑ A"), (Values{0}));
  EXPECT_EQ(values("A ← 5⍳d.1; +\\ 0 ↑ A").size(), 0u);
  EXPECT_EQ(values("A ← 5⍳d.1; ⌽ 0 ↑ A").size(), 0u);
}

TEST_F(VplTakeBoundTest, TakeBeyondTheLengthAndNegativeTakeAreRefusedByName) {
  values("A ← 5⍳d.1");
  EXPECT_NE(failure("6 ↑ A").find("would pad with fill"), std::string::npos);
  EXPECT_NE(failure("A ← 5⍳d.1; n ← 2+4; n ↑ A").find("would pad with fill"),
            std::string::npos);
  EXPECT_NE(failure("¯2 ↑ A").find("take of a negative count"),
            std::string::npos);
  EXPECT_NE(failure("A ← 5⍳d.1; n ← 0-2; n ↑ A").find("negative count"),
            std::string::npos);
  EXPECT_NE(failure("A ← 5⍳d.1; 1.5 ↑ A").find("DOMAIN ERROR"),
            std::string::npos);
  EXPECT_NE(failure("A ← 5⍳d.1; \"x\" ↑ A").find("count must be a number"),
            std::string::npos);
  EXPECT_NE(failure("A ← 5⍳d.1; n ← \"2\"; n ↑ A").find("not a number"),
            std::string::npos);
  EXPECT_NE(failure("3 ↑ 4").find("take of a scalar"), std::string::npos);
}

TEST_F(VplTakeBoundTest, NegativeDropRemovesTheLastCells) {
  values("A ← 5⍳d.1");
  EXPECT_EQ(values("¯2 ↓ A"), (Values{1, 2, 3}));
  EXPECT_EQ(values("¯4 ↓ A"), (Values{1}));
  EXPECT_EQ(values("¯5 ↓ A").size(), 0u);
  EXPECT_EQ(values("¯9 ↓ A").size(), 0u);
  EXPECT_EQ(values("¯0 ↓ A"), (Values{1, 2, 3, 4, 5}));
  EXPECT_EQ(values("n ← 0-2; n ↓ A"), (Values{1, 2, 3}));
  EXPECT_EQ(values("(0-1) ↓ A"), (Values{1, 2, 3, 4}));
  EXPECT_EQ(values("≢ ¯2 ↓ A"), (Values{3}));
}

TEST_F(VplTakeBoundTest, DropOfAReversedRankSkipsTheReversedFront) {
  values("A ← 5⍳d.1");
  EXPECT_EQ(values("2 ↓ ⌽A"), (Values{3, 2, 1}));
  EXPECT_EQ(values("0 ↓ ⌽A"), (Values{5, 4, 3, 2, 1}));
  EXPECT_EQ(values("5 ↓ ⌽A").size(), 0u);
  EXPECT_EQ(values("9 ↓ ⌽A").size(), 0u);
  EXPECT_EQ(values("¯2 ↓ ⌽A"), (Values{5, 4, 3}));
  EXPECT_EQ(values("n ← 2; n ↓ ⌽A"), (Values{3, 2, 1}));
  EXPECT_EQ(values("n ← 0-2; n ↓ ⌽A"), (Values{5, 4, 3}));
  EXPECT_EQ(values("⌽ 2 ↓ ⌽A"), (Values{1, 2, 3}));
  EXPECT_EQ(values("A"), (Values{1, 2, 3, 4, 5}));
}

TEST_F(VplTakeBoundTest, TakeOfAReversedRankIsTheReversedFront) {
  values("A ← 5⍳d.1");
  EXPECT_EQ(values("3 ↑ ⌽A"), (Values{5, 4, 3}));
  EXPECT_EQ(values("5 ↑ ⌽A"), (Values{5, 4, 3, 2, 1}));
  EXPECT_EQ(values("0 ↑ ⌽A").size(), 0u);
  EXPECT_EQ(values("+/ 3 ↑ ⌽A"), (Values{12}));
  EXPECT_EQ(values("+\\ 3 ↑ ⌽A"), (Values{5, 9, 12}));
}

TEST_F(VplTakeBoundTest, ReverseOfABoundedViewReadsTheBoundedPrefixReversed) {
  values("A ← 5⍳d.1");
  EXPECT_EQ(values("⌽ 3 ↑ A"), (Values{3, 2, 1}));
  EXPECT_EQ(values("⌽ ¯2 ↓ A"), (Values{3, 2, 1}));
  EXPECT_EQ(values("⌽⌽ 3 ↑ A"), (Values{1, 2, 3}));
  EXPECT_EQ(values("≢ ⌽ 3 ↑ A"), (Values{3}));
  EXPECT_EQ(values("⍉ 3 ↑ A"), (Values{1, 2, 3}));
  EXPECT_EQ(values("⌽ 2 ↓ 4 ↑ A"), (Values{4, 3}));
}

TEST_F(VplTakeBoundTest, DropAndTakeOfABoundedViewCompose) {
  values("A ← 5⍳d.1");
  EXPECT_EQ(values("2 ↓ 4 ↑ A"), (Values{3, 4}));
  EXPECT_EQ(values("2 ↑ 3 ↓ A"), (Values{4, 5}));
  EXPECT_EQ(values("2 ↑ 4 ↑ A"), (Values{1, 2}));
  EXPECT_EQ(values("¯1 ↓ 4 ↑ A"), (Values{1, 2, 3}));
  EXPECT_EQ(values("¯1 ↓ 2 ↓ A"), (Values{3, 4}));
  EXPECT_EQ(values("1 ↓ ¯1 ↓ A"), (Values{2, 3, 4}));
  EXPECT_EQ(values("4 ↓ 4 ↑ A").size(), 0u);
  EXPECT_EQ(values("9 ↓ 4 ↑ A").size(), 0u);
  EXPECT_NE(failure("5 ↑ 4 ↑ A").find("would pad with fill"),
            std::string::npos);
  EXPECT_EQ(values("1 ↑ ⌽ 3 ↑ A"), (Values{3}));
  EXPECT_EQ(values("2 ↓ ⌽ 3 ↑ A"), (Values{1}));
  EXPECT_EQ(values("¯1 ↓ ⌽ 3 ↑ A"), (Values{3, 2}));
}

TEST_F(VplTakeBoundTest, FoldsAndScansStopAtTheBound) {
  values("A ← 5⍳d.1");
  EXPECT_EQ(values("+/ 3 ↑ A"), (Values{6}));
  EXPECT_EQ(values("×/ 3 ↑ A"), (Values{6}));
  EXPECT_EQ(values("⌈/ 3 ↑ A"), (Values{3}));
  EXPECT_EQ(values("⌊/ 2 ↓ 4 ↑ A"), (Values{3}));
  EXPECT_EQ(values("+\\ 3 ↑ A"), (Values{1, 3, 6}));
  EXPECT_EQ(values("+/ ¯2 ↓ A"), (Values{6}));
  EXPECT_EQ(values("+\\ ¯2 ↓ ⌽A"), (Values{5, 9, 12}));
  EXPECT_EQ(values("⍴ 3 ↑ A"), (Values{3}));
  EXPECT_EQ(values("+/ ⌽ 3 ↑ A"), (Values{6}));
}

TEST_F(VplTakeBoundTest, ElementWiseReadsOnlyTheBoundedCells) {
  values("A ← 5⍳d.1");
  EXPECT_EQ(values("(3 ↑ A) + (3 ↑ A)"), (Values{2, 4, 6}));
  EXPECT_EQ(values("(3 ↑ A) + (2 ↓ A)"), (Values{4, 6, 8}));
  EXPECT_EQ(values("(3 ↑ A) × 10"), (Values{10, 20, 30}));
  EXPECT_EQ(values("10 - 3 ↑ A"), (Values{9, 8, 7}));
  EXPECT_EQ(values("- 3 ↑ A"), (Values{-1, -2, -3}));
  EXPECT_EQ(values("(3 ↑ A) + ⌽ 3 ↑ A"), (Values{4, 4, 4}));
  EXPECT_NE(failure("(3 ↑ A) + A").find("same length"), std::string::npos);
  EXPECT_NE(failure("(3 ↑ A) + (2 ↑ A)").find("same length"),
            std::string::npos);
}

TEST_F(VplTakeBoundTest, CatenateAndRavelReadOnlyTheBoundedCells) {
  values("A ← 5⍳d.1");
  EXPECT_EQ(values("(3 ↑ A) , (2 ↓ A)"), (Values{1, 2, 3, 3, 4, 5}));
  EXPECT_EQ(values("0 , 2 ↑ A"), (Values{0, 1, 2}));
  EXPECT_EQ(values("(2 ↑ A) , 9"), (Values{1, 2, 9}));
  EXPECT_EQ(values(", ⌽ 3 ↑ A"), (Values{3, 2, 1}));
  EXPECT_EQ(values("(2 ↑ A) , (2 ↑ A)"), (Values{1, 2, 1, 2}));
  EXPECT_EQ(values("(⌽ 2 ↑ A) , (¯3 ↓ A)"), (Values{2, 1, 1, 2}));
  EXPECT_EQ(values("A"), (Values{1, 2, 3, 4, 5}));
}

TEST_F(VplTakeBoundTest, IndexingStaysInsideTheBound) {
  values("A ← 5⍳d.1");
  EXPECT_EQ(values("(3 ↑ A)[0]"), (Values{1}));
  EXPECT_EQ(values("(3 ↑ A)[2]"), (Values{3}));
  EXPECT_EQ(values("(⌽ 3 ↑ A)[0]"), (Values{3}));
  EXPECT_FALSE(failure("(3 ↑ A)[3]").empty());
  EXPECT_FALSE(failure("(3 ↑ A)[4]").empty());
}

TEST_F(VplTakeBoundTest, GradeWhereAndOuterProductReadOnlyTheBoundedCells) {
  values("A ← 5⍳d.1");
  EXPECT_EQ(values("⍒ 3 ↑ A"), (Values{2, 1, 0}));
  EXPECT_EQ(values("⍋ 3 ↑ A"), (Values{0, 1, 2}));
  const auto matrix = engine.evaluate("(2 ↑ A) ∘.+ (3 ↑ A)");
  EXPECT_EQ(matrix.shape(engine.arena()), (std::vector<std::size_t>{2, 3}));
}

TEST_F(VplTakeBoundTest, EncloseOfABoundedViewIsRefusedByName) {
  values("A ← 5⍳d.1");
  EXPECT_NE(failure("⊂ 3 ↑ A").find("enclose of a bounded view"),
            std::string::npos);
  EXPECT_NE(failure("⊂ ¯1 ↓ A").find("enclose of a bounded view"),
            std::string::npos);
}

TEST_F(VplTakeBoundTest, ABoundedViewKeepsItsBoundWhenBoundToAName) {
  values("A ← 5⍳d.1");
  EXPECT_EQ(values("B ← 3 ↑ A; B"), (Values{1, 2, 3}));
  EXPECT_EQ(values("B"), (Values{1, 2, 3}));
  EXPECT_EQ(values("+/ B"), (Values{6}));
  EXPECT_EQ(values("⌽B"), (Values{3, 2, 1}));
  EXPECT_EQ(values("2 ↓ B"), (Values{3}));
  EXPECT_EQ(values("B , B"), (Values{1, 2, 3, 1, 2, 3}));
  EXPECT_EQ(values("C ← ⌽ 3 ↑ A; C"), (Values{3, 2, 1}));
  EXPECT_EQ(values("1 ↓ C"), (Values{2, 1}));
  EXPECT_EQ(values("D ← ¯2 ↓ ⌽A; D"), (Values{5, 4, 3}));
  EXPECT_EQ(values("≢ D"), (Values{3}));
  EXPECT_EQ(values("A"), (Values{1, 2, 3, 4, 5}));
}

TEST_F(VplTakeBoundTest, ADropGrowsWithTheRankAndATakeDoesNot) {
  values("A ← 5⍳d.1");
  values("D ← 2 ↓ A");
  values("T ← 3 ↑ A");
  values("E ← ¯1 ↓ A");
  // A later mint after A's tail lengthens the rank. A drop is a new origin
  // over the open rank and grows with it; a take or a negative drop ends at a
  // fixed cell and does not.
  values("3 ⍳ d.1");
  EXPECT_EQ(values("D"), (Values{3, 4, 5, 1, 2, 3}));
  EXPECT_EQ(values("T"), (Values{1, 2, 3}));
  EXPECT_EQ(values("E"), (Values{1, 2, 3, 4}));
}

TEST_F(VplTakeBoundTest, ABoundedViewThroughAnotherProgramIsNotWidened) {
  values("A ← 5⍳d.1");
  values("T ← 3 ↑ A");
  EXPECT_EQ(values("2 ↑ T"), (Values{1, 2}));
  EXPECT_EQ(values("T"), (Values{1, 2, 3}));
  EXPECT_EQ(values("+\\ T"), (Values{1, 3, 6}));
  EXPECT_NE(failure("4 ↑ T").find("would pad with fill"), std::string::npos);
}

} // namespace
} // namespace xanadu::vpl

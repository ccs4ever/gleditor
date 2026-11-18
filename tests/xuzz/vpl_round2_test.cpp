/**
 * @file vpl_round2_test.cpp
 * @brief Fixed far ends, named index failures, numeric equality, doubles for
 * division, no text in arithmetic, boolean folds, count clamping and the
 * negward-axis take, at source level through the compile-and-run path.
 */
#include <cstdint>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <vector>

#include "common/xanadu/vpl/execution.hpp"
#include "common/xanadu/vpl/vpl_engine.hpp"

namespace xanadu::vpl {
namespace {

using Numbers = std::vector<double>;

class VplRound2Test : public ::testing::Test {
protected:
  Numbers numbers(const std::string &source) {
    const auto view = engine.evaluate(source);
    Numbers out;
    for (const auto cell : view.collectCells(engine.arena())) {
      if (const auto i = engine.arena().asInt64(cell)) {
        out.push_back(static_cast<double>(*i));
      } else if (const auto d = engine.arena().asDouble(cell)) {
        out.push_back(*d);
      } else if (const auto b = engine.arena().asBool(cell)) {
        out.push_back(*b ? 1.0 : 0.0);
      } else {
        ADD_FAILURE() << source << ": a cell is not a number";
        out.push_back(-999.0);
      }
    }
    return out;
  }

  std::string failure(const std::string &source) {
    try {
      engine.evaluate(source);
    } catch (const std::exception &error) {
      return error.what();
    }
    return {};
  }

  void expectFails(const std::string &source, const std::string &text) {
    const auto message = failure(source);
    EXPECT_FALSE(message.empty()) << source << " was answered";
    EXPECT_NE(message.find(text), std::string::npos)
        << source << ": " << message;
  }

  VPLEngine engine;
};

// Item 1: a take of a reversed rank is fixed when evaluated.
TEST_F(VplRound2Test, ReversedTakeAndDropsFixTheirFarEnd) {
  numbers("A ← 5⍳d.1");
  numbers("Q ← 3 ↑ ⌽A");
  numbers("R ← 2 ↓ ⌽A");
  numbers("S ← ¯2 ↓ ⌽A");
  numbers("E ← ¯1 ↓ A");
  numbers("D ← 2 ↓ A");
  numbers("3 ⍳ d.1");
  EXPECT_EQ(numbers("Q"), (Numbers{5, 4, 3}));
  EXPECT_EQ(numbers("⍴Q"), (Numbers{3}));
  EXPECT_EQ(numbers("R"), (Numbers{3, 2, 1}));
  EXPECT_EQ(numbers("S"), (Numbers{5, 4, 3}));
  EXPECT_EQ(numbers("E"), (Numbers{1, 2, 3, 4}));
  // The one view that grows: a positive drop of an unreversed rank.
  EXPECT_EQ(numbers("D"), (Numbers{3, 4, 5, 1, 2, 3}));
  EXPECT_EQ(numbers("⌽Q"), (Numbers{3, 4, 5}));
  EXPECT_EQ(numbers("+/ Q"), (Numbers{12}));
}

// Item 2: an index the view does not hold fails by name.
TEST_F(VplRound2Test, OutOfRangeIndexFailsByName) {
  numbers("A ← 1 2 3 4 5");
  const std::string text = "VPL index out of range";
  expectFails("A[0 9]", text);
  expectFails("A[10]", text);
  expectFails("(3↑A)[0 1 2 9 3]", text);
  expectFails("(3↑A)[3]", text);
  expectFails("(0↑A)[0]", text);
  expectFails("(3↑A)[0 3]", text);
  EXPECT_EQ(numbers("A[0 4]"), (Numbers{1, 5}));
  EXPECT_EQ(numbers("(3↑A)[0 1 2]"), (Numbers{1, 2, 3}));
  EXPECT_EQ(numbers("A[2]"), (Numbers{3}));
}

// Item 3: numbers compare by value.
TEST_F(VplRound2Test, EqualityIsNumericAndTextIsNotZero) {
  EXPECT_EQ(numbers("2.0 = 2"), (Numbers{1}));
  EXPECT_EQ(numbers("2 = 2.0"), (Numbers{1}));
  EXPECT_EQ(numbers("2.0 ≠ 2"), (Numbers{0}));
  EXPECT_EQ(numbers("(0.5+1.5) = 2"), (Numbers{1}));
  EXPECT_EQ(numbers("A ← 1 2 3; A = 2.0"), (Numbers{0, 1, 0}));
  EXPECT_EQ(numbers("A ← 1 2 3; A ≠ 2.0"), (Numbers{1, 0, 1}));
  EXPECT_EQ(numbers("2.5 = 2"), (Numbers{0}));
  EXPECT_EQ(numbers("\"x\" = 0"), (Numbers{0}));
  EXPECT_EQ(numbers("0 = \"x\""), (Numbers{0}));
  EXPECT_EQ(numbers("\"x\" = \"x\""), (Numbers{1}));
  EXPECT_EQ(numbers("\"x\" ≠ 0"), (Numbers{1}));
  EXPECT_EQ(numbers("A ← 5⍳d.1; (3 ↑ A) = 2.0"), (Numbers{0, 1, 0}));
  EXPECT_EQ(numbers("A ← 5⍳d.1; (⌽ 3 ↑ A) = 2.0"), (Numbers{0, 1, 0}));
  EXPECT_EQ(numbers("(1 2 3 = 3) = 1"), (Numbers{0, 0, 1}));
}

TEST_F(VplRound2Test, GradeTiesAcrossIntAndDouble) {
  EXPECT_EQ(numbers("⍋ 2 2.0 1"), (Numbers{2, 0, 1}));
  EXPECT_EQ(numbers("⍒ 2 2.0 3"), (Numbers{2, 0, 1}));
  EXPECT_EQ(numbers("A ← 5⍳d.1; ⍋ 3 ↑ A"), (Numbers{0, 1, 2}));
  EXPECT_EQ(numbers("⍋ ⌽ 1 2.0 2"), (Numbers{2, 0, 1}));
  EXPECT_EQ(numbers("⍋ 3 ↑ 2 2.0 1 0"), (Numbers{2, 0, 1}));
}

// Item 4: division is on doubles.
TEST_F(VplRound2Test, DivisionIsOnDoublesAndZeroFailsByName) {
  EXPECT_EQ(numbers("5 ÷ 2"), (Numbers{2.5}));
  EXPECT_EQ(numbers("6 ÷ 2"), (Numbers{3.0}));
  EXPECT_NEAR(numbers("1 ÷ 3").front(), 1.0 / 3.0, 1e-12);
  EXPECT_EQ(numbers("1 2 3 ÷ 2"), (Numbers{0.5, 1.0, 1.5}));
  EXPECT_EQ(numbers("÷ 4"), (Numbers{0.25}));
  EXPECT_EQ(numbers("÷ 2 4"), (Numbers{0.5, 0.25}));
  EXPECT_EQ(numbers("10 20 ÷ 4 8"), (Numbers{2.5, 2.5}));
  expectFails("5 ÷ 0", "divide by zero");
  expectFails("÷ 0", "divide by zero");
  expectFails("1 2 ÷ 0 1", "divide by zero");
  expectFails("1 2 3 ÷ 0", "divide by zero");
  expectFails("A ← 5⍳d.1; n ← 5 ÷ 2; n ↓ A", "not a whole number");
  EXPECT_EQ(numbers("A ← 5⍳d.1; n ← 6 ÷ 2; n ↓ A"), (Numbers{4, 5}));
  EXPECT_EQ(numbers("A ← 5⍳d.1; n ← 6 ÷ 3; n ↑ A"), (Numbers{1, 2}));
}

// Item 5: text is not a number.
TEST_F(VplRound2Test, TextInArithmeticIsRefused) {
  expectFails("\"x\" + 1", "arithmetic on text is unsupported");
  expectFails("\"x\" × 2", "arithmetic on text is unsupported");
  expectFails("1 + \"5\"", "arithmetic on text is unsupported");
  expectFails("1 2 3 + \"a\"", "arithmetic on text is unsupported");
  expectFails("n ← \"5\"; n + 1", "arithmetic on text is unsupported");
  expectFails("n ← \"x\"; 2 × n", "arithmetic on text is unsupported");
  expectFails("n ← \"5\"; - n", "arithmetic on text is unsupported");
  expectFails("n ← \"5\"; ÷ n", "arithmetic on text is unsupported");
  expectFails("n ← \"5\"; 1 2 ⌈ n", "arithmetic on text is unsupported");
  EXPECT_EQ(numbers("2 + 3"), (Numbers{5}));
  EXPECT_EQ(numbers("2.5 + 3"), (Numbers{5.5}));
  EXPECT_EQ(numbers("(1 2 3 = 3) + 1"), (Numbers{1, 1, 2}));
}

// Item 6: boolean folds.
TEST_F(VplRound2Test, AndOrFoldsAndTheNonAssociativeFoldsRefused) {
  EXPECT_EQ(numbers("∧/ 1 0 1"), (Numbers{0}));
  EXPECT_EQ(numbers("∨/ 1 0 1"), (Numbers{1}));
  EXPECT_EQ(numbers("∧/ 1 1 1"), (Numbers{1}));
  EXPECT_EQ(numbers("∨/ 0 0 0"), (Numbers{0}));
  EXPECT_EQ(numbers("∧\\ 1 1 0 1"), (Numbers{1, 1, 0, 0}));
  EXPECT_EQ(numbers("∨\\ 0 0 1 0"), (Numbers{0, 0, 1, 1}));
  EXPECT_EQ(numbers("A ← 0⍳d.1; ∧/A"), (Numbers{1}));
  EXPECT_EQ(numbers("A ← 0⍳d.1; ∨/A"), (Numbers{0}));
  EXPECT_EQ(numbers("A ← 4⍳d.1; ∧/ A ≥ 1"), (Numbers{1}));
  expectFails("=/ 1 2 3", "supports only");
  expectFails("≠/ 1 2 3", "supports only");
  expectFails("A ← 5⍳d.1; =/ A", "supports only");
}

// Item 7: counts at the int64 boundary and boolean counts.
TEST_F(VplRound2Test, HugeAndBooleanCounts) {
  EXPECT_EQ(numbers("A ← 5⍳d.1; n ← 9223372036854775807; ≢ n ↓ A"),
            (Numbers{0}));
  EXPECT_EQ(numbers("A ← 5⍳d.1; ≢ 1000000000000 ↓ A"), (Numbers{0}));
  EXPECT_EQ(numbers("A ← 5⍳d.1; n ← 1000000000.0 × 1000000000.0 × "
                    "1000000000.0; ≢ n ↓ A"),
            (Numbers{0}));
  EXPECT_EQ(numbers("A ← 5⍳d.1; n ← 0 - 9223372036854775807; n ↓ A").size(),
            0u);
  EXPECT_EQ(numbers("A ← 5⍳d.1; (1=1) ↓ A"), (Numbers{2, 3, 4, 5}));
  EXPECT_EQ(numbers("A ← 5⍳d.1; n ← 1=1; n ↑ A"), (Numbers{1}));
  EXPECT_EQ(numbers("A ← 5⍳d.1; n ← 1=2; n ↑ A").size(), 0u);
  expectFails("A ← 5⍳d.1; n ← 9223372036854775807; n ↑ A", "would pad");
}

// Item 9: take and drops on a negward axis.
TEST_F(VplRound2Test, TakeAndDropsOnANegwardAxis) {
  const auto dimension = engine.resolveDimension("d.1");
  std::vector<zigzag::CellRef> cells;
  for (const std::int64_t value : {7, 8, 9}) {
    const auto cell = engine.arena().makeCell();
    engine.core().writeValue(cell, 0, std::nullopt, value);
    cells.push_back(cell);
  }
  for (std::size_t i = 0; i + 1 < cells.size(); ++i) {
    zigzag::expectWritten(engine.arena().link(
        cells[i], dimension, zigzag::DimVector::NEG, cells[i + 1]));
  }
  engine.setVariable(
      "N", VplView{cells.front(), {{dimension, zigzag::DimVector::NEG}}});
  EXPECT_EQ(numbers("2 ↑ N"), (Numbers{7, 8}));
  EXPECT_EQ(numbers("¯1 ↓ N"), (Numbers{7, 8}));
  EXPECT_EQ(numbers("1 ↓ N"), (Numbers{8, 9}));
  EXPECT_EQ(numbers("1 ↓ ⌽N"), (Numbers{8, 7}));
  EXPECT_EQ(numbers("2 ↑ ⌽N"), (Numbers{9, 8}));
  EXPECT_EQ(numbers("¯1 ↓ ⌽N"), (Numbers{9, 8}));
  EXPECT_EQ(numbers("+/ 2 ↑ N"), (Numbers{15}));
  EXPECT_EQ(numbers("⌽ 2 ↑ N"), (Numbers{8, 7}));
}

} // namespace
} // namespace xanadu::vpl

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>
#include <string_view>

#include "common/xanadu/vql/vql_engine.hpp"

namespace {

using xanadu::vql::VQLEngine;

// A plain text cell keeps its text origin at run time: two text operands
// compare as text, and a number is inferred from text only when the other side
// is a number. A scalar cell is already typed, so it stays numeric.
class VQLTextComparisonTest : public ::testing::Test {
protected:
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  VQLEngine engine{core};

  bool matches(std::string_view predicate) {
    return !engine
                .execute("let $root := ## where " + std::string(predicate) +
                         " return $root")
                .empty();
  }

  void text(const std::string &name, const std::string &value) {
    engine.setVariable(name, arena.makeCell(value));
  }
};

TEST_F(VQLTextComparisonTest, TextCellsThatLookLikeNanCompareAsText) {
  text("l", "nan");
  text("r", "nan");
  EXPECT_TRUE(matches("$l = $r"));
  EXPECT_FALSE(matches("$l != $r"));
  EXPECT_TRUE(matches(R"($l = "nan")"));
  EXPECT_FALSE(matches(R"($l != "nan")"));
  EXPECT_TRUE(matches(R"("nan" = $r)"));
}

TEST_F(VQLTextComparisonTest, TextCellsThatLookLikeInfCompareAsText) {
  text("l", "inf");
  text("r", "inf");
  EXPECT_TRUE(matches("$l = $r"));
  EXPECT_FALSE(matches("$l != $r"));
  EXPECT_TRUE(matches(R"($l = "inf")"));
  EXPECT_TRUE(matches("$l <= $r"));
  // Numerically inf equals infinity; as text the shorter spelling sorts first.
  EXPECT_TRUE(matches(R"($l < "infinity")"));
}

TEST_F(VQLTextComparisonTest, TextCellsDifferingOnlyInNumericSpellingDiffer) {
  text("l", "1");
  text("r", "1.0");
  EXPECT_FALSE(matches("$l = $r"));
  EXPECT_TRUE(matches("$l != $r"));
  EXPECT_FALSE(matches(R"($l = "1.0")"));
  EXPECT_TRUE(matches(R"($r = "1.0")"));
}

TEST_F(VQLTextComparisonTest, TextCellsOrderAsText) {
  text("l", "10");
  text("r", "9");
  EXPECT_TRUE(matches("$l < $r"));
  EXPECT_FALSE(matches("$l > $r"));
  EXPECT_TRUE(matches(R"($l < "9")"));
}

TEST_F(VQLTextComparisonTest, TextAgainstANumberStillComparesNumerically) {
  text("t", "1.0");
  EXPECT_TRUE(matches("$t = 1"));
  EXPECT_TRUE(matches("1 = $t"));
  text("big", "10");
  EXPECT_TRUE(matches("$big > 9"));
  text("nan", "nan");
  EXPECT_FALSE(matches("$nan = 1"));
  engine.setVariable("one", arena.makeScalarCell(std::int64_t{1}));
  EXPECT_TRUE(matches("$t = $one"));
  EXPECT_TRUE(matches("$one = $t"));
}

TEST_F(VQLTextComparisonTest, TypedScalarCellsStayNumeric) {
  engine.setVariable("l", arena.makeScalarCell(std::int64_t{10}));
  engine.setVariable("r", arena.makeScalarCell(std::int64_t{9}));
  EXPECT_TRUE(matches("$l > $r"));
  engine.setVariable("one", arena.makeScalarCell(std::int64_t{1}));
  engine.setVariable("onef", arena.makeScalarCell(1.0));
  EXPECT_TRUE(matches("$one = $onef"));
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  engine.setVariable("n1", arena.makeScalarCell(nan));
  engine.setVariable("n2", arena.makeScalarCell(nan));
  EXPECT_TRUE(matches("$n1 != $n2"));
  EXPECT_FALSE(matches("$n1 = $n2"));
}

} // namespace

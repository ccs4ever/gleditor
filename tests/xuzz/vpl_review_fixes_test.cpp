/**
 * @file vpl_review_fixes_test.cpp
 * @brief Regressions from the adversarial review of VPL cards V9 to V12, read
 * through the vpl runner so what is checked is what a user sees printed.
 */
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>

namespace {

std::string runVpl(const std::string &expression) {
  const auto root = std::filesystem::current_path().string();
  const std::string command =
      "SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 "
      "XDG_DATA_HOME=" +
      root + "/build/xdg/data XDG_CONFIG_HOME=" + root +
      "/build/xdg/config XDG_CACHE_HOME=" + root + "/build/xdg/cache " + root +
      "/build/vpl --headless -e '" + expression + "' 2>&1";
  FILE *pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) return {};
  std::string out;
  char buffer[256];
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) out += buffer;
  pclose(pipe);
  return out;
}

bool mentions(const std::string &output, const std::string &text) {
  return output.find(text) != std::string::npos;
}

// A raw cell handle printed as a value is the failure these tests exist for.
bool leaksCellHandle(const std::string &output) {
  return mentions(output, "c#");
}

TEST(VplReviewEncloseTest, EncloseKeepsAReversedOperandReversed) {
  EXPECT_EQ(runVpl("⌽1 2 3"), "3 2 1\n");
  EXPECT_EQ(runVpl("A←1 2 3\nB←⌽A\nC←⊂B\n⊃C"), "3 2 1\n");
  EXPECT_EQ(runVpl("⊃⊂⌽1 2 3"), "3 2 1\n");
  EXPECT_EQ(runVpl("⊂⌽1 2 3"), "< 3 2 1 >\n");
}

TEST(VplReviewEncloseTest, EnclosePlainValuesRoundTrip) {
  EXPECT_EQ(runVpl("⊃⊂1 2 3"), "1 2 3\n");
  EXPECT_EQ(runVpl("⊂1 2 3"), "< 1 2 3 >\n");
  EXPECT_EQ(runVpl("⊃ 1 2 3"), "1 2 3\n");
}

TEST(VplReviewEncloseTest, NestedEncloseRefusesByNameAndNeverLeaksACell) {
  for (const char *source : {"⊂⊂1 2", "⊃⊂⊂1 2 3", "⊃⊃⊂⊂1 2 3", "A←⊂1 2\n⊂A"}) {
    const auto out = runVpl(source);
    EXPECT_FALSE(leaksCellHandle(out)) << source << ": " << out;
    EXPECT_TRUE(mentions(out, "VPL nested enclose is unsupported by the VM"))
        << source << ": " << out;
  }
}

TEST(VplReviewEncloseTest, ShapeOfAnEnclosureIsRefusedByName) {
  const auto out = runVpl("⍴ ⊂1 2 3");
  EXPECT_TRUE(mentions(out, "monadic shape of an enclosed value")) << out;
}

TEST(VplReviewEncloseTest, DiscloseOfANonEnclosedValueIsIdentity) {
  EXPECT_EQ(runVpl("⊃ 4 5"), "4 5\n");
  EXPECT_EQ(runVpl("⊃ 7"), "7\n");
}

TEST(VplReviewIndexTest, MatrixIndexIsRefusedByName) {
  for (const char *source :
       {"(10 20 30)[(0 1)∘.+(0 1)]", "(10 20 30)[(1 2)∘.+(0 1)]"}) {
    const auto out = runVpl(source);
    EXPECT_TRUE(mentions(out, "VPL indexing with a rank of more than one axis"))
        << source << ": " << out;
  }
}

TEST(VplReviewIndexTest, WholeValuedDoublesIndexLikeIntegers) {
  EXPECT_EQ(runVpl("(10 20 30)[1.0]"), "20\n");
  EXPECT_EQ(runVpl("(10 20 30)[4÷2]"), "30\n");
  EXPECT_EQ(runVpl("(10 20 30)[0 2.0]"), "10 30\n");
  EXPECT_EQ(runVpl("(10 20 30)[(4÷2) 0]"), "30 10\n");
}

TEST(VplReviewIndexTest, ComputedAndLiteralFractionsAgree) {
  EXPECT_TRUE(mentions(runVpl("(10 20 30)[1.5]"), "DOMAIN ERROR"));
  EXPECT_TRUE(mentions(runVpl("(10 20 30)[3÷2]"), "DOMAIN ERROR"));
  EXPECT_TRUE(mentions(runVpl("(10 20 30)[(3÷2) 0]"), "DOMAIN ERROR"));
  EXPECT_FALSE(mentions(runVpl("(10 20 30)[3÷2]"), "out of range"));
}

TEST(VplReviewIndexTest, OutOfRangeAndNegativeStayNamed) {
  EXPECT_TRUE(mentions(runVpl("(10 20 30)[5]"), "VPL index out of range"));
  EXPECT_TRUE(mentions(runVpl("(10 20 30)[1 ¯1]"), "negative index"));
  EXPECT_TRUE(mentions(runVpl("(10 20 30)[2 5]"), "VPL index out of range"));
}

TEST(VplReviewWhereTest, BooleansAndZeroOneIntegersAreAccepted) {
  EXPECT_EQ(runVpl("⍸ 0 1 0 1 1"), "1 3 4\n");
  EXPECT_EQ(runVpl("⍸ 1 2 3 = 2"), "1\n");
  EXPECT_EQ(runVpl("⍸ 1 2 3 ≤ 2"), "0 1\n");
  EXPECT_EQ(runVpl("⍸ 1.0 0.0 1.0"), "0 2\n");
}

TEST(VplReviewWhereTest, NonBooleanInputIsRefusedByName) {
  for (const char *source : {"⍸ 2 0 3", "⍸ 0.5 0", "⍸ 0 ¯1 1", "⍸ 1 0 7"}) {
    const auto out = runVpl(source);
    EXPECT_TRUE(mentions(out, "VPL where requires booleans"))
        << source << ": " << out;
  }
}

TEST(VplReviewCompareTest, DyadicAngleBracketsAreComparisons) {
  EXPECT_EQ(runVpl("1 < 2"), "1\n");
  EXPECT_EQ(runVpl("2 < 1"), "0\n");
  EXPECT_EQ(runVpl("3 > 2"), "1\n");
  EXPECT_EQ(runVpl("1 2 < 2 3"), "1 1\n");
  EXPECT_EQ(runVpl("1 2 3 > 2"), "0 0 1\n");
  EXPECT_EQ(runVpl("1 2 ∘.< 2 1"), "1 0\n0 0\n");
  EXPECT_EQ(runVpl("1 2 ∘.> 2 1"), "0 0\n0 1\n");
}

TEST(VplReviewCompareTest, MonadicAngleBracketsStayEncloseAndDisclose) {
  EXPECT_EQ(runVpl("<1 2 3"), "< 1 2 3 >\n");
  EXPECT_EQ(runVpl(">1 2 3"), "1 2 3\n");
}

TEST(VplReviewCompareTest, RefusalsNameTheirVerb) {
  EXPECT_TRUE(mentions(runVpl("1 ⍟ 2"), "⍟"));
  EXPECT_TRUE(mentions(runVpl("1 2 ⍟ 3 4"), "⍟"));
  EXPECT_TRUE(mentions(runVpl("1 2 ∘., 3 4"), "outer product of Catenate"));
  EXPECT_TRUE(mentions(runVpl("1 2 ∘.⍳ 3"), "outer product of Iota"));
}

TEST(VplReviewOuterTest, EmptyOperandsRefuseByNameAndNeverLeakACell) {
  for (const char *source :
       {"1 2 ∘.× ⍳0", "(⍳0) ∘.× 1 2", "(⍳0) ∘.× ⍳0", "⍴ 1 2 ∘.× ⍳0",
        "⍴ (⍳0) ∘.× 1 2", "⍴ (⍳0) ∘.× ⍳0"}) {
    const auto out = runVpl(source);
    EXPECT_FALSE(leaksCellHandle(out)) << source << ": " << out;
    EXPECT_TRUE(mentions(out, "VPL outer product with an empty operand"))
        << source << ": " << out;
  }
}

TEST(VplReviewOuterTest, TransposeReadsEveryColumn) {
  EXPECT_EQ(runVpl("⍉ 1 2 3 ∘.× 4 5"), "4 8 12\n5 10 15\n");
  EXPECT_EQ(runVpl("⍴ ⍉ 1 2 3 ∘.× 4 5"), "2 3\n");
  EXPECT_EQ(runVpl("⍉ ⍉ 1 2 3 ∘.× 4 5"), "4 5\n8 10\n12 15\n");
  EXPECT_EQ(runVpl("⍉ 1 2 3 ∘.× 4"), "4 8 12\n");
  EXPECT_EQ(runVpl("⍉ 7 ∘.× 4 5 6"), "28\n35\n42\n");
}

TEST(VplReviewOuterTest, ReversalsWorkInBothOrientations) {
  EXPECT_EQ(runVpl("⌽ 1 2 3 ∘.× 4 5"), "12 15\n8 10\n4 5\n");
  EXPECT_EQ(runVpl("⊖ 1 2 3 ∘.× 4 5"), "5 4\n10 8\n15 12\n");
  EXPECT_EQ(runVpl("⌽ ⍉ 1 2 3 ∘.× 4 5"), "5 10 15\n4 8 12\n");
  EXPECT_EQ(runVpl("⊖ ⍉ 1 2 3 ∘.× 4 5"), "12 8 4\n15 10 5\n");
  EXPECT_EQ(runVpl("⍉ ⌽ 1 2 3 ∘.× 4 5"), "12 8 4\n15 10 5\n");
}

TEST(VplReviewOuterTest, ReduceAndScanOfAMatrixRefuseByNameOnBothAxes) {
  for (const char *source :
       {"+/ 1 2 3 ∘.× 4 5", "+⌿ 1 2 3 ∘.× 4 5", "+\\ 1 2 3 ∘.× 4 5",
        "+/ ⍉ 1 2 3 ∘.× 4 5", "+⌿ ⍉ 1 2 3 ∘.× 4 5"}) {
    const auto out = runVpl(source);
    EXPECT_TRUE(mentions(out, "more than one axis")) << source << ": " << out;
  }
}

TEST(VplReviewOuterTest, ResultDoesNotAliasAnOperandThatIsExtendedLater) {
  EXPECT_EQ(runVpl("A←1 2\nM←A∘.×3 4\nA←A,9\nM"), "3 4\n6 8\n");
  EXPECT_EQ(runVpl("A←1 2\nM←A∘.×3 4\nA←A,9\n⍴M"), "2 2\n");
  EXPECT_EQ(runVpl("A←1 2\nM←A∘.×3 4\nA←A,9\n⍉M"), "3 6\n4 8\n");
}

} // namespace

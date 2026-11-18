/**
 * @file vpl_fold_edges_test.cpp
 * @brief Fold edges: reduce and scan of empty ranks (including through a drop
 * that leaves nothing), the identities and refusals, and the same on the
 * literal-vector path, at source level through the compile-and-run path.
 */
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

#include "common/xanadu/vpl/ast.hpp"
#include "common/xanadu/vpl/vpl_engine.hpp"

namespace xanadu::vpl {
namespace {

using Values = std::vector<std::int64_t>;

class VplFoldEdgesTest : public ::testing::Test {
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

  std::string refusal(const std::string &source) {
    try {
      engine.evaluate(source);
    } catch (const std::exception &error) {
      return error.what();
    }
    return {};
  }

  VPLEngine engine;
};

TEST_F(VplFoldEdgesTest, ScanOfAnEmptyRankIsEmpty) {
  EXPECT_EQ(values("A ← 0⍳d.1; ≢ +\\A"), (Values{0}));
  EXPECT_EQ(values("A ← 0⍳d.1; +\\A").size(), 0u);
  EXPECT_EQ(values("A ← 0⍳d.1; ×\\A").size(), 0u);
  EXPECT_EQ(values("A ← 0⍳d.1; ⌈\\A").size(), 0u);
  EXPECT_EQ(values("A ← 0⍳d.1; ⌊\\A").size(), 0u);
}

TEST_F(VplFoldEdgesTest, ScanThroughADropThatLeavesNothingIsEmpty) {
  EXPECT_EQ(values("A ← 5⍳d.1; ≢ +\\ 5 ↓ A"), (Values{0}));
  EXPECT_EQ(values("A ← 5⍳d.1; ≢ +\\ 7 ↓ A"), (Values{0}));
  EXPECT_EQ(values("A ← 5⍳d.1; +\\ 5 ↓ A").size(), 0u);
}

TEST_F(VplFoldEdgesTest, ScanOfANonEmptyRankStillMintsEveryCell) {
  EXPECT_EQ(values("A ← 5⍳d.1; +\\ 3 ↓ A"), (Values{4, 9}));
  EXPECT_EQ(values("A ← 1 2 3; +\\A"), (Values{1, 3, 6}));
  EXPECT_EQ(values("A ← 1 2 3; ≢ +\\A"), (Values{3}));
}

TEST_F(VplFoldEdgesTest, ReduceOfAnEmptyRankIsTheIdentityForSumAndProduct) {
  // Owner 2026-11-18: the APL identities for the two verbs that have them.
  EXPECT_EQ(values("A ← 0⍳d.1; +/A"), (Values{0}));
  EXPECT_EQ(values("A ← 0⍳d.1; ×/A"), (Values{1}));
}

TEST_F(VplFoldEdgesTest, ReduceThroughAnEmptyDropIsTheIdentity) {
  EXPECT_EQ(values("A ← 5⍳d.1; +/ 5 ↓ A"), (Values{0}));
  EXPECT_EQ(values("A ← 5⍳d.1; ×/ 5 ↓ A"), (Values{1}));
  EXPECT_EQ(values("A ← 5⍳d.1; +/ 9 ↓ A"), (Values{0}));
}

TEST_F(VplFoldEdgesTest, ReduceOfAnEmptyRankHasNoMaximumOrMinimum) {
  EXPECT_NE(
      refusal("A ← 0⍳d.1; ⌈/A")
          .find("VPL reduce of an empty rank has no identity for maximum"),
      std::string::npos);
  EXPECT_NE(
      refusal("A ← 0⍳d.1; ⌊/A")
          .find("VPL reduce of an empty rank has no identity for minimum"),
      std::string::npos);
  EXPECT_NE(refusal("A ← 5⍳d.1; ⌈/ 5 ↓ A").find("has no identity for maximum"),
            std::string::npos);
  EXPECT_NE(refusal("A ← 5⍳d.1; ⌊/ 5 ↓ A").find("has no identity for minimum"),
            std::string::npos);
}

TEST_F(VplFoldEdgesTest, ReduceOfNonEmptyRanksIsUnchanged) {
  EXPECT_EQ(values("A ← 5⍳d.1; +/ 3 ↓ A"), (Values{9}));
  EXPECT_EQ(values("A ← 5⍳d.1; ×/ 3 ↓ A"), (Values{20}));
  EXPECT_EQ(values("A ← 5⍳d.1; ⌈/ 3 ↓ A"), (Values{5}));
  EXPECT_EQ(values("A ← 5⍳d.1; ⌊/ 3 ↓ A"), (Values{4}));
  EXPECT_EQ(values("+/ 4 8 2"), (Values{14}));
}

TEST_F(VplFoldEdgesTest, EmptyLiteralVectorAgreesWithTheRuntimePath) {
  const auto emptyVector = [] {
    return std::make_shared<VectorExpr>(
        SourceLocation{}, std::vector<std::shared_ptr<AstNode>>{});
  };
  const auto reduce = [&](const TokenKind verb) {
    auto adverb = std::make_shared<AdverbExpr>(
        SourceLocation{}, TokenKind::Reduce,
        std::make_shared<VerbExpr>(SourceLocation{}, verb));
    return MonadicExpr{SourceLocation{},
                       std::static_pointer_cast<AstNode>(adverb),
                       std::static_pointer_cast<AstNode>(emptyVector())};
  };
  const auto sum = [&](const MonadicExpr &expr) {
    const auto view  = engine.evaluate(expr);
    const auto cells = view.collectCells(engine.arena());
    EXPECT_EQ(cells.size(), 1u);
    return engine.arena().asInt64(cells.front()).value_or(-1);
  };
  EXPECT_EQ(sum(reduce(TokenKind::Plus)), 0);
  EXPECT_EQ(sum(reduce(TokenKind::Times)), 1);
  EXPECT_THROW(engine.evaluate(reduce(TokenKind::Ceiling)), std::exception);
  EXPECT_THROW(engine.evaluate(reduce(TokenKind::Floor)), std::exception);
}

} // namespace
} // namespace xanadu::vpl

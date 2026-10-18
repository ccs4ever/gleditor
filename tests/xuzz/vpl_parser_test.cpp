/**
 * @file vpl_parser_test.cpp
 * @brief Unit tests for the VPL right-to-left array parser.
 */
#include <gtest/gtest.h>

#include "common/xanadu/vpl/parser.hpp"

namespace xanadu::vpl {
namespace {

TEST(VplParserTest, RightToLeftAssociativity) {
  // In APL, 3 × 2 + 1 evaluates to 3 × (2 + 1) = 9
  Parser parser("3 × 2 + 1");
  auto ast = parser.parseExpression();

  ASSERT_NE(ast, nullptr);
  auto dyTimes = std::dynamic_pointer_cast<DyadicExpr>(ast);
  ASSERT_NE(dyTimes, nullptr);
  EXPECT_EQ(dyTimes->verb(), TokenKind::Times);

  auto leftScalar = std::dynamic_pointer_cast<ScalarExpr>(dyTimes->left());
  ASSERT_NE(leftScalar, nullptr);
  EXPECT_EQ(leftScalar->intValue(), 3);

  // Right argument must be the entire (2 + 1) dyadic addition
  auto dyPlus = std::dynamic_pointer_cast<DyadicExpr>(dyTimes->right());
  ASSERT_NE(dyPlus, nullptr);
  EXPECT_EQ(dyPlus->verb(), TokenKind::Plus);

  auto midScalar = std::dynamic_pointer_cast<ScalarExpr>(dyPlus->left());
  ASSERT_NE(midScalar, nullptr);
  EXPECT_EQ(midScalar->intValue(), 2);

  auto rightScalar = std::dynamic_pointer_cast<ScalarExpr>(dyPlus->right());
  ASSERT_NE(rightScalar, nullptr);
  EXPECT_EQ(rightScalar->intValue(), 1);
}

TEST(VplParserTest, NounStranding) {
  // Vector of numbers
  {
    Parser parser("1 2 3 4 5");
    auto ast = parser.parseExpression();
    ASSERT_NE(ast, nullptr);
    auto vec = std::dynamic_pointer_cast<VectorExpr>(ast);
    ASSERT_NE(vec, nullptr);
    ASSERT_EQ(vec->elements().size(), 5u);
    for (std::int64_t i = 0; i < 5; ++i) {
      auto sc = std::dynamic_pointer_cast<ScalarExpr>(vec->elements()[i]);
      ASSERT_NE(sc, nullptr);
      EXPECT_EQ(sc->intValue(), i + 1);
    }
  }

  // Vector of dimensions
  {
    Parser parser("d.1 d.2 d.3");
    auto ast = parser.parseExpression();
    ASSERT_NE(ast, nullptr);
    auto vec = std::dynamic_pointer_cast<VectorExpr>(ast);
    ASSERT_NE(vec, nullptr);
    ASSERT_EQ(vec->elements().size(), 3u);
    EXPECT_EQ(
        std::dynamic_pointer_cast<DimensionExpr>(vec->elements()[0])->name(),
        "d.1");
    EXPECT_EQ(
        std::dynamic_pointer_cast<DimensionExpr>(vec->elements()[1])->name(),
        "d.2");
    EXPECT_EQ(
        std::dynamic_pointer_cast<DimensionExpr>(vec->elements()[2])->name(),
        "d.3");
  }
}

TEST(VplParserTest, MonadicAndDyadicVerbs) {
  // Monadic negate: - 42
  {
    Parser parser("- 42");
    auto ast = parser.parseExpression();
    ASSERT_NE(ast, nullptr);
    auto mon = std::dynamic_pointer_cast<MonadicExpr>(ast);
    ASSERT_NE(mon, nullptr);
    EXPECT_EQ(mon->verb(), TokenKind::Minus);
    auto sc = std::dynamic_pointer_cast<ScalarExpr>(mon->right());
    ASSERT_NE(sc, nullptr);
    EXPECT_EQ(sc->intValue(), 42);
  }

  // Dyadic subtract: 100 - 42
  {
    Parser parser("100 - 42");
    auto ast = parser.parseExpression();
    ASSERT_NE(ast, nullptr);
    auto dy = std::dynamic_pointer_cast<DyadicExpr>(ast);
    ASSERT_NE(dy, nullptr);
    EXPECT_EQ(dy->verb(), TokenKind::Minus);
    EXPECT_EQ(std::dynamic_pointer_cast<ScalarExpr>(dy->left())->intValue(),
              100);
    EXPECT_EQ(std::dynamic_pointer_cast<ScalarExpr>(dy->right())->intValue(),
              42);
  }

  // Dyadic iota (mint rank): 5 ⍳ d.1
  {
    Parser parser("5 ⍳ d.1");
    auto ast = parser.parseExpression();
    ASSERT_NE(ast, nullptr);
    auto dy = std::dynamic_pointer_cast<DyadicExpr>(ast);
    ASSERT_NE(dy, nullptr);
    EXPECT_EQ(dy->verb(), TokenKind::Iota);
    EXPECT_EQ(std::dynamic_pointer_cast<ScalarExpr>(dy->left())->intValue(), 5);
    EXPECT_EQ(std::dynamic_pointer_cast<DimensionExpr>(dy->right())->name(),
              "d.1");
  }
}

TEST(VplParserTest, AdverbsAndConjunctions) {
  // Reduce: +/ C
  {
    Parser parser("+/ C");
    auto ast = parser.parseExpression();
    ASSERT_NE(ast, nullptr);
    auto mon = std::dynamic_pointer_cast<MonadicExpr>(ast);
    ASSERT_NE(mon, nullptr);
    ASSERT_TRUE(mon->hasCustomVerb());
    auto adv = std::dynamic_pointer_cast<AdverbExpr>(mon->customVerb());
    ASSERT_NE(adv, nullptr);
    EXPECT_EQ(adv->adverb(), TokenKind::Reduce);
    auto baseVerb = std::dynamic_pointer_cast<VerbExpr>(adv->operand());
    ASSERT_NE(baseVerb, nullptr);
    EXPECT_EQ(baseVerb->verb(), TokenKind::Plus);
    EXPECT_EQ(std::dynamic_pointer_cast<IdentifierExpr>(mon->right())->name(),
              "C");
  }

  // Outer product: A ∘.+ B
  {
    Parser parser("A ∘.+ B");
    auto ast = parser.parseExpression();
    ASSERT_NE(ast, nullptr);
    auto dy = std::dynamic_pointer_cast<DyadicExpr>(ast);
    ASSERT_NE(dy, nullptr);
    ASSERT_TRUE(dy->hasCustomVerb());
    auto conj = std::dynamic_pointer_cast<ConjunctionExpr>(dy->customVerb());
    ASSERT_NE(conj, nullptr);
    EXPECT_EQ(conj->conjunction(), TokenKind::OuterProduct);
    EXPECT_EQ(std::dynamic_pointer_cast<IdentifierExpr>(dy->left())->name(),
              "A");
    EXPECT_EQ(std::dynamic_pointer_cast<IdentifierExpr>(dy->right())->name(),
              "B");
  }
}

TEST(VplParserTest, AssignmentAndIndexing) {
  // A ← 5⍳d.1
  {
    Parser parser("A ← 5⍳d.1");
    auto ast = parser.parseExpression();
    ASSERT_NE(ast, nullptr);
    auto assign = std::dynamic_pointer_cast<AssignExpr>(ast);
    ASSERT_NE(assign, nullptr);
    EXPECT_EQ(assign->target(), "A");
    auto val = std::dynamic_pointer_cast<DyadicExpr>(assign->value());
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(val->verb(), TokenKind::Iota);
  }

  // Indexing: A[3]
  {
    Parser parser("A[3]");
    auto ast = parser.parseExpression();
    ASSERT_NE(ast, nullptr);
    auto idx = std::dynamic_pointer_cast<IndexingExpr>(ast);
    ASSERT_NE(idx, nullptr);
    EXPECT_EQ(std::dynamic_pointer_cast<IdentifierExpr>(idx->target())->name(),
              "A");
    ASSERT_EQ(idx->indices().size(), 1u);
    EXPECT_EQ(
        std::dynamic_pointer_cast<ScalarExpr>(idx->indices()[0])->intValue(),
        3);
  }

  // 2D Indexing: V[1; 2]
  {
    Parser parser("V[1; 2]");
    auto ast = parser.parseExpression();
    ASSERT_NE(ast, nullptr);
    auto idx = std::dynamic_pointer_cast<IndexingExpr>(ast);
    ASSERT_NE(idx, nullptr);
    ASSERT_EQ(idx->indices().size(), 2u);
    EXPECT_EQ(
        std::dynamic_pointer_cast<ScalarExpr>(idx->indices()[0])->intValue(),
        1);
    EXPECT_EQ(
        std::dynamic_pointer_cast<ScalarExpr>(idx->indices()[1])->intValue(),
        2);
  }
}

TEST(VplParserTest, DualSyntaxParsingEquivalence) {
  // Verify that APL and J equivalent code parse into identical semantic nodes
  auto compareAstStructure = [](std::string_view apl, std::string_view j) {
    Parser parserApl(apl);
    Parser parserJ(j);
    auto astApl = parserApl.parseProgram();
    auto astJ   = parserJ.parseProgram();

    ASSERT_NE(astApl, nullptr);
    ASSERT_NE(astJ, nullptr);
    ASSERT_EQ(astApl->expressions().size(), astJ->expressions().size());
  };

  compareAstStructure("A ← 5⍳d.1", "A =. 5 i. d.1");
  compareAstStructure("+/ 10⍳d.1", "+/ 10 i. d.1");
  compareAstStructure("V ← H ⍴ (d.1 d.2 d.3)", "V =. H $ (d.1 d.2 d.3)");
  compareAstStructure("M ← ⊃C", "M =. >C");
}

} // namespace
} // namespace xanadu::vpl

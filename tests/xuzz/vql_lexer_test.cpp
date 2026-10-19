/**
 * @file vql_lexer_test.cpp
 * @brief Unit tests for VQL Lexer: sigils, tokens, comments, placements,
 * yields.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "common/xanadu/vql/lexer.hpp"

namespace {

using namespace xanadu::vql;

TEST(VQLLexerTest, AnchorsAndSigils) {
  Lexer lexer("## ##math # ^ ^COMPILER $worker . / :: ! % >< := ? >");
  auto tokens = lexer.tokenizeAll();

  ASSERT_GE(tokens.size(), 14u);
  EXPECT_EQ(tokens[0].kind, TokenKind::Home);
  EXPECT_EQ(tokens[1].kind, TokenKind::NamedStore);
  EXPECT_EQ(tokens[1].stringValue, "math");
  EXPECT_EQ(tokens[2].kind, TokenKind::Root);
  EXPECT_EQ(tokens[3].kind, TokenKind::Cursor);
  EXPECT_EQ(tokens[4].kind, TokenKind::NamedCursor);
  EXPECT_EQ(tokens[4].stringValue, "COMPILER");
  EXPECT_EQ(tokens[5].kind, TokenKind::Variable);
  EXPECT_EQ(tokens[5].stringValue, "worker");
  EXPECT_EQ(tokens[6].kind, TokenKind::Dot);
  EXPECT_EQ(tokens[7].kind, TokenKind::Slash);
  EXPECT_EQ(tokens[8].kind, TokenKind::DoubleColon);
  EXPECT_EQ(tokens[9].kind, TokenKind::Bang);
  EXPECT_EQ(tokens[10].kind, TokenKind::Percent);
  EXPECT_EQ(tokens[11].kind, TokenKind::CloneJoin);
  EXPECT_EQ(tokens[12].kind, TokenKind::Assign);
  EXPECT_EQ(tokens[13].kind, TokenKind::Question);
  EXPECT_EQ(tokens[14].kind, TokenKind::GreaterThan); // or DerefMaster
}

TEST(VQLLexerTest, Keywords) {
  Lexer lexer("for in let where return weave if else and or not true false");
  auto tokens = lexer.tokenizeAll();

  EXPECT_EQ(tokens[0].kind, TokenKind::KwFor);
  EXPECT_EQ(tokens[1].kind, TokenKind::KwIn);
  EXPECT_EQ(tokens[2].kind, TokenKind::KwLet);
  EXPECT_EQ(tokens[3].kind, TokenKind::KwWhere);
  EXPECT_EQ(tokens[4].kind, TokenKind::KwReturn);
  EXPECT_EQ(tokens[5].kind, TokenKind::KwWeave);
  EXPECT_EQ(tokens[6].kind, TokenKind::KwIf);
  EXPECT_EQ(tokens[7].kind, TokenKind::KwElse);
  EXPECT_EQ(tokens[8].kind, TokenKind::KwAnd);
  EXPECT_EQ(tokens[9].kind, TokenKind::KwOr);
  EXPECT_EQ(tokens[10].kind, TokenKind::KwNot);
  EXPECT_EQ(tokens[11].kind, TokenKind::KwTrue);
  EXPECT_EQ(tokens[12].kind, TokenKind::KwFalse);
}

TEST(VQLLexerTest, LiteralsAndComments) {
  Lexer lexer(R"(
    // line comment
    /* block comment */
    (: XQuery comment :)
    "hello \"world\"\n" 123 45.67 d.pinning-cursors std:math
  )");
  auto tokens = lexer.tokenizeAll();

  ASSERT_GE(tokens.size(), 5u);
  EXPECT_EQ(tokens[0].kind, TokenKind::StringLiteral);
  EXPECT_EQ(tokens[0].stringValue, "hello \"world\"\n");

  EXPECT_EQ(tokens[1].kind, TokenKind::IntegerLiteral);
  EXPECT_EQ(tokens[1].intValue, 123);

  EXPECT_EQ(tokens[2].kind, TokenKind::FloatLiteral);
  EXPECT_DOUBLE_EQ(tokens[2].floatValue, 45.67);

  EXPECT_EQ(tokens[3].kind, TokenKind::Identifier);
  EXPECT_EQ(tokens[3].text, "d.pinning-cursors");

  EXPECT_EQ(tokens[4].kind, TokenKind::Identifier);
  EXPECT_EQ(tokens[4].text, "std:math");
}

TEST(VQLLexerTest, ComparisonsAndDelimiters) {
  Lexer lexer("= != < > <= >= ( ) [ ] { } ,");
  auto tokens = lexer.tokenizeAll();

  EXPECT_EQ(tokens[0].kind, TokenKind::Equal);
  EXPECT_EQ(tokens[1].kind, TokenKind::NotEqual);
  EXPECT_EQ(tokens[2].kind, TokenKind::LessThan);
  EXPECT_EQ(tokens[3].kind, TokenKind::GreaterThan);
  EXPECT_EQ(tokens[4].kind, TokenKind::LessEqual);
  EXPECT_EQ(tokens[5].kind, TokenKind::GreaterEqual);
  EXPECT_EQ(tokens[6].kind, TokenKind::OpenParen);
  EXPECT_EQ(tokens[7].kind, TokenKind::CloseParen);
  EXPECT_EQ(tokens[8].kind, TokenKind::OpenBracket);
  EXPECT_EQ(tokens[9].kind, TokenKind::CloseBracket);
  EXPECT_EQ(tokens[10].kind, TokenKind::OpenBrace);
  EXPECT_EQ(tokens[11].kind, TokenKind::CloseBrace);
  EXPECT_EQ(tokens[12].kind, TokenKind::Comma);
}

} // namespace

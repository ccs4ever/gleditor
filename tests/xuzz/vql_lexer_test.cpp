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

TEST(VQLLexerTest, ExtendedTokensAndSigils) {
  Lexer lexer(": => ~ @ .. <~ <=> \\");
  auto tokens = lexer.tokenizeAll();

  ASSERT_EQ(tokens.size(), 9u);
  EXPECT_EQ(tokens[0].kind, TokenKind::Colon);
  EXPECT_EQ(tokens[1].kind, TokenKind::Arrow);
  EXPECT_EQ(tokens[2].kind, TokenKind::Tilde);
  EXPECT_EQ(tokens[3].kind, TokenKind::At);
  EXPECT_EQ(tokens[4].kind, TokenKind::DotDot);
  EXPECT_EQ(tokens[5].kind, TokenKind::Transclude);
  EXPECT_EQ(tokens[6].kind, TokenKind::DimPivot);
  EXPECT_EQ(tokens[7].kind, TokenKind::Backslash);
  EXPECT_EQ(tokens[8].kind, TokenKind::EndOfFile);
}

TEST(VQLLexerTest, MicroversionLiterals) {
  Lexer lexer("@0 @1 @88 @2a9 @8c5 v\"2a9\" @(2a9)");
  auto tokens = lexer.tokenizeAll();

  ASSERT_GE(tokens.size(), 8u);
  EXPECT_EQ(tokens[0].kind, TokenKind::MicroversionLiteral);
  EXPECT_EQ(tokens[0].stringValue, "0");
  EXPECT_EQ(tokens[0].text, "@0");

  EXPECT_EQ(tokens[1].kind, TokenKind::MicroversionLiteral);
  EXPECT_EQ(tokens[1].stringValue, "1");

  EXPECT_EQ(tokens[2].kind, TokenKind::MicroversionLiteral);
  EXPECT_EQ(tokens[2].stringValue, "88");

  EXPECT_EQ(tokens[3].kind, TokenKind::MicroversionLiteral);
  EXPECT_EQ(tokens[3].stringValue, "2a9");

  EXPECT_EQ(tokens[4].kind, TokenKind::MicroversionLiteral);
  EXPECT_EQ(tokens[4].stringValue, "8c5");

  EXPECT_EQ(tokens[5].kind, TokenKind::TypedVersionLiteral);
  EXPECT_EQ(tokens[5].stringValue, "2a9");

  // @(2a9) should lex as At, OpenParen, MicroversionLiteral (or
  // Identifier/BareVersion), CloseParen
  EXPECT_EQ(tokens[6].kind, TokenKind::At);
  EXPECT_EQ(tokens[7].kind, TokenKind::OpenParen);
}

TEST(VQLLexerTest, ExtendedKeywords) {
  Lexer lexer("current local topic btpk slices docs citing backlinks find "
              "system btih ledger def mount as watch since lens list");
  auto tokens = lexer.tokenizeAll();

  ASSERT_EQ(tokens.size(), 20u);
  EXPECT_EQ(tokens[0].kind, TokenKind::KwCurrent);
  EXPECT_EQ(tokens[1].kind, TokenKind::KwLocal);
  EXPECT_EQ(tokens[2].kind, TokenKind::KwTopic);
  EXPECT_EQ(tokens[3].kind, TokenKind::KwBtpk);
  EXPECT_EQ(tokens[4].kind, TokenKind::KwSlices);
  EXPECT_EQ(tokens[5].kind, TokenKind::KwDocs);
  EXPECT_EQ(tokens[6].kind, TokenKind::KwCiting);
  EXPECT_EQ(tokens[7].kind, TokenKind::KwBacklinks);
  EXPECT_EQ(tokens[8].kind, TokenKind::KwFind);
  EXPECT_EQ(tokens[9].kind, TokenKind::KwSystem);
  EXPECT_EQ(tokens[10].kind, TokenKind::KwBtih);
  EXPECT_EQ(tokens[11].kind, TokenKind::KwLedger);
  EXPECT_EQ(tokens[12].kind, TokenKind::KwDef);
  EXPECT_EQ(tokens[13].kind, TokenKind::KwMount);
  EXPECT_EQ(tokens[14].kind, TokenKind::KwAs);
  EXPECT_EQ(tokens[15].kind, TokenKind::KwWatch);
  EXPECT_EQ(tokens[16].kind, TokenKind::KwSince);
  EXPECT_EQ(tokens[17].kind, TokenKind::KwLens);
  EXPECT_EQ(tokens[18].kind, TokenKind::KwList);
  EXPECT_EQ(tokens[19].kind, TokenKind::EndOfFile);
}

TEST(VQLLexerTest, VQL15_1TokensAndKeywords) {
  Lexer lexer("^^ ^^foo until with perspective break");
  auto tokens = lexer.tokenizeAll();

  ASSERT_EQ(tokens.size(), 7u);
  EXPECT_EQ(tokens[0].kind, TokenKind::AllCursors);
  EXPECT_EQ(tokens[0].text, "^^");

  EXPECT_EQ(tokens[1].kind, TokenKind::AllNamedCursors);
  EXPECT_EQ(tokens[1].text, "^^foo");
  EXPECT_EQ(tokens[1].stringValue, "foo");

  EXPECT_EQ(tokens[2].kind, TokenKind::KwUntil);
  EXPECT_EQ(tokens[3].kind, TokenKind::KwWith);
  EXPECT_EQ(tokens[4].kind, TokenKind::KwPerspective);
  EXPECT_EQ(tokens[5].kind, TokenKind::KwBreak);
  EXPECT_EQ(tokens[6].kind, TokenKind::EndOfFile);
}

TEST(VQLLexerTest, VQLIteration2Tokens) {
  Lexer lexer("pure ... $args...");
  auto tokens = lexer.tokenizeAll();

  ASSERT_EQ(tokens.size(), 5u);
  EXPECT_EQ(tokens[0].kind, TokenKind::KwPure);
  EXPECT_EQ(tokens[0].text, "pure");

  EXPECT_EQ(tokens[1].kind, TokenKind::DotDotDot);
  EXPECT_EQ(tokens[1].text, "...");

  EXPECT_EQ(tokens[2].kind, TokenKind::Variable);
  EXPECT_EQ(tokens[2].stringValue, "args");

  EXPECT_EQ(tokens[3].kind, TokenKind::DotDotDot);
  EXPECT_EQ(tokens[3].text, "...");

  EXPECT_EQ(tokens[4].kind, TokenKind::EndOfFile);
}

} // namespace

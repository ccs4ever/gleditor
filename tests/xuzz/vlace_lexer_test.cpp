/**
 * @file vlace_lexer_test.cpp
 * @brief Stage 1 unit tests for the vlace tokenizer.
 */
#include <gtest/gtest.h>

#include <vector>

#include "common/xanadu/vlace/lexer.hpp"

namespace {

using namespace xanadu::vlace;

std::vector<TokenKind> kinds(std::string_view src) {
  Lexer lex(src);
  std::vector<TokenKind> out;
  for (const auto &t : lex.tokenizeAll()) {
    out.push_back(t.kind);
  }
  return out;
}

TEST(VlaceLexerTest, IdentifiersConstructorsKeywords) {
  Lexer lex("foldr Cons let x");
  auto t0 = lex.nextToken();
  EXPECT_EQ(t0.kind, TokenKind::Identifier);
  EXPECT_EQ(t0.text, "foldr");
  auto t1 = lex.nextToken();
  EXPECT_EQ(t1.kind, TokenKind::Constructor);
  EXPECT_EQ(t1.text, "Cons");
  auto t2 = lex.nextToken();
  EXPECT_EQ(t2.kind, TokenKind::KwLet);
  auto t3 = lex.nextToken();
  EXPECT_EQ(t3.kind, TokenKind::Identifier);
  EXPECT_EQ(t3.text, "x");
}

TEST(VlaceLexerTest, AllKeywords) {
  auto k = kinds("let letrec in if then else case of fn data type import as "
                 "infixl infixr infix");
  const std::vector<TokenKind> want = {
      TokenKind::KwLet,   TokenKind::KwLetrec, TokenKind::KwIn,
      TokenKind::KwIf,    TokenKind::KwThen,   TokenKind::KwElse,
      TokenKind::KwCase,  TokenKind::KwOf,     TokenKind::KwFn,
      TokenKind::KwData,  TokenKind::KwType,   TokenKind::KwImport,
      TokenKind::KwAs,    TokenKind::KwInfixl, TokenKind::KwInfixr,
      TokenKind::KwInfix, TokenKind::EndOfFile};
  EXPECT_EQ(k, want);
}

TEST(VlaceLexerTest, ReservedSymbolsVsOperators) {
  // Reserved symbols get their own kinds; other operator runs are Operator.
  auto k                            = kinds("= => | :: -> . + ++ >>= < <= &&");
  const std::vector<TokenKind> want = {
      TokenKind::Equals,      TokenKind::FatArrow,  TokenKind::Bar,
      TokenKind::DoubleColon, TokenKind::ThinArrow, TokenKind::Dot,
      TokenKind::Operator, // +
      TokenKind::Operator, // ++
      TokenKind::Operator, // >>=
      TokenKind::Operator, // <
      TokenKind::Operator, // <=
      TokenKind::Operator, // &&
      TokenKind::EndOfFile};
  EXPECT_EQ(k, want);
}

TEST(VlaceLexerTest, StructuralPunctuation) {
  auto k                            = kinds("( ) [ ] { } , ; _ @");
  const std::vector<TokenKind> want = {
      TokenKind::OpenParen,    TokenKind::CloseParen, TokenKind::OpenBracket,
      TokenKind::CloseBracket, TokenKind::OpenBrace,  TokenKind::CloseBrace,
      TokenKind::Comma,        TokenKind::Semicolon,  TokenKind::Underscore,
      TokenKind::At,           TokenKind::EndOfFile};
  EXPECT_EQ(k, want);
}

TEST(VlaceLexerTest, IntegerLiterals) {
  Lexer lex("42 0xFF 0o17 0b1010");
  auto a = lex.nextToken();
  EXPECT_EQ(a.kind, TokenKind::IntegerLiteral);
  EXPECT_EQ(a.intValue, 42);
  auto b = lex.nextToken();
  EXPECT_EQ(b.kind, TokenKind::IntegerLiteral);
  EXPECT_EQ(b.intValue, 255);
  auto c = lex.nextToken();
  EXPECT_EQ(c.kind, TokenKind::IntegerLiteral);
  EXPECT_EQ(c.intValue, 15);
  auto d = lex.nextToken();
  EXPECT_EQ(d.kind, TokenKind::IntegerLiteral);
  EXPECT_EQ(d.intValue, 10);
}

TEST(VlaceLexerTest, FloatLiteral) {
  Lexer lex("3.14 1.0e-9");
  auto a = lex.nextToken();
  EXPECT_EQ(a.kind, TokenKind::FloatLiteral);
  EXPECT_DOUBLE_EQ(a.floatValue, 3.14);
  auto b = lex.nextToken();
  EXPECT_EQ(b.kind, TokenKind::FloatLiteral);
  EXPECT_DOUBLE_EQ(b.floatValue, 1.0e-9);
}

TEST(VlaceLexerTest, StringAndCharLiterals) {
  Lexer lex("\"hello\" 'a' '\\n'");
  auto s = lex.nextToken();
  EXPECT_EQ(s.kind, TokenKind::StringLiteral);
  EXPECT_EQ(s.stringValue, "hello");
  auto c = lex.nextToken();
  EXPECT_EQ(c.kind, TokenKind::CharLiteral);
  EXPECT_EQ(c.stringValue, "a");
  auto nl = lex.nextToken();
  EXPECT_EQ(nl.kind, TokenKind::CharLiteral);
  EXPECT_EQ(nl.stringValue, "\n");
}

TEST(VlaceLexerTest, LineCommentsAreSkipped) {
  auto k                            = kinds("x -- this is ignored\ny");
  const std::vector<TokenKind> want = {
      TokenKind::Identifier, TokenKind::Identifier, TokenKind::EndOfFile};
  EXPECT_EQ(k, want);
}

TEST(VlaceLexerTest, PeekDoesNotAdvance) {
  Lexer lex("let x");
  auto p = lex.peekToken();
  EXPECT_EQ(p.kind, TokenKind::KwLet);
  auto n = lex.nextToken();
  EXPECT_EQ(n.kind, TokenKind::KwLet);
  auto n2 = lex.nextToken();
  EXPECT_EQ(n2.kind, TokenKind::Identifier);
}

TEST(VlaceLexerTest, PrimedIdentifier) {
  Lexer lex("x' acc1");
  auto a = lex.nextToken();
  EXPECT_EQ(a.kind, TokenKind::Identifier);
  EXPECT_EQ(a.text, "x'");
  auto b = lex.nextToken();
  EXPECT_EQ(b.kind, TokenKind::Identifier);
  EXPECT_EQ(b.text, "acc1");
}

} // namespace

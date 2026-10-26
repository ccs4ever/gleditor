#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vagent/lexer.hpp"

namespace {

using namespace xanadu::vagent;

std::vector<TokenKind> kinds(const std::vector<Token> &tokens) {
  std::vector<TokenKind> result;
  for (const auto &token : tokens) result.push_back(token.kind);
  return result;
}

TEST(VagentLexerTest, ReservedAndContextualWordsStayDistinct) {
  auto result = Lexer("agent emit by within true false none as holds module "
                      "parent outer occurrence preempts requires priority")
                    .tokenize();
  ASSERT_TRUE(result) << result.error().detail;
  ASSERT_EQ(result->size(), 17U);
  for (std::size_t i = 0; i < 7; ++i) {
    EXPECT_EQ((*result)[i].kind, TokenKind::Keyword) << (*result)[i].text;
  }
  for (std::size_t i = 7; i + 1 < result->size(); ++i) {
    EXPECT_EQ((*result)[i].kind, TokenKind::Identifier) << (*result)[i].text;
  }
  EXPECT_EQ(result->back().kind, TokenKind::EndOfFile);
}

TEST(VagentLexerTest, EveryReservedWordInTheSpecificationIsClassified) {
  constexpr std::string_view words[] = {
      "achieve",   "adopt",   "agent",      "answer",    "ask",      "await",
      "behavior",  "believe", "believes",   "by",        "capacity", "consume",
      "def",       "do",      "drop",       "effect",    "else",     "emit",
      "event",     "extends", "fail",       "findall",   "for",      "forget",
      "goal",      "if",      "implements", "import",    "in",       "install",
      "key",       "let",     "mount",      "native",    "none",     "not",
      "on",        "plan",    "predicate",  "question",  "record",   "return",
      "revise",    "self",    "skill",      "spawn",     "stop",     "tell",
      "terminate", "then",    "to",         "uninstall", "unmount",  "untell",
      "uses",      "var",     "where",      "while",     "within",   "yield"};
  for (const auto word : words) EXPECT_TRUE(isReservedWord(word)) << word;
  EXPECT_FALSE(isReservedWord("maintain"));
  EXPECT_FALSE(isReservedWord("ensures"));
}

TEST(VagentLexerTest, OperatorsUseLongestMatch) {
  auto result = Lexer("a<-1 := ++ + - * / % == != ! < <= > >= & | ? : . , ; "
                      "( ) [ ] { }")
                    .tokenize();
  ASSERT_TRUE(result) << result.error().detail;
  EXPECT_EQ(kinds(*result),
            (std::vector<TokenKind>{
                TokenKind::Identifier,     TokenKind::RevAssign,
                TokenKind::IntegerLiteral, TokenKind::Assign,
                TokenKind::Concat,         TokenKind::Plus,
                TokenKind::Minus,          TokenKind::Star,
                TokenKind::Slash,          TokenKind::Percent,
                TokenKind::Equal,          TokenKind::NotEqual,
                TokenKind::Bang,           TokenKind::Less,
                TokenKind::LessEqual,      TokenKind::Greater,
                TokenKind::GreaterEqual,   TokenKind::Ampersand,
                TokenKind::Alternation,    TokenKind::Scan,
                TokenKind::Colon,          TokenKind::Dot,
                TokenKind::Comma,          TokenKind::Semicolon,
                TokenKind::OpenParen,      TokenKind::CloseParen,
                TokenKind::OpenBracket,    TokenKind::CloseBracket,
                TokenKind::OpenBrace,      TokenKind::CloseBrace,
                TokenKind::EndOfFile}));
}

TEST(VagentLexerTest, CommentsPreservePhysicalNewlinesAndLocations) {
  const std::string source = "a\r\n/*x\n y*/ b // trailing\n";
  auto result              = Lexer(source).tokenize();
  ASSERT_TRUE(result) << result.error().detail;
  ASSERT_EQ(result->size(), 6U);
  EXPECT_EQ((*result)[1].kind, TokenKind::Newline);
  EXPECT_EQ((*result)[1].text, "\r\n");
  EXPECT_EQ((*result)[1].loc.line, 1U);
  EXPECT_EQ((*result)[2].kind, TokenKind::Newline);
  EXPECT_EQ((*result)[2].loc.line, 2U);
  EXPECT_EQ((*result)[3].text, "b");
  EXPECT_EQ((*result)[3].loc.line, 3U);
  EXPECT_EQ((*result)[3].loc.column, 6U);
  EXPECT_EQ((*result)[3].loc.offset, source.find('b'));
  EXPECT_EQ(result->back().loc.line, 4U);
}

TEST(VagentLexerTest, BlockCommentsDoNotNest) {
  auto result = Lexer("/* outer /* inner */ visible").tokenize();
  ASSERT_TRUE(result) << result.error().detail;
  ASSERT_EQ(result->size(), 2U);
  EXPECT_EQ(result->front().text, "visible");
}

TEST(VagentLexerTest, NewlinesRemainInsideGroupingForParserToInterpret) {
  auto result = Lexer("(1\n +2)\n{a\nb}").tokenize();
  ASSERT_TRUE(result) << result.error().detail;
  EXPECT_EQ(std::ranges::count(kinds(*result), TokenKind::Newline), 3);
}

TEST(VagentLexerTest, SourceAndTokensOwnTheirSpellings) {
  std::string source = "agent Example { var name: Str := \"old\" }";
  Lexer lexer(source);
  source.assign(source.size(), 'x');
  auto result = lexer.tokenize();
  ASSERT_TRUE(result) << result.error().detail;
  EXPECT_EQ(result->front().text, "agent");
  EXPECT_EQ((*result)[1].text, "Example");
  const auto literal = std::ranges::find_if(*result, [](const Token &token) {
    return token.kind == TokenKind::StringLiteral;
  });
  ASSERT_NE(literal, result->end());
  EXPECT_EQ(literal->text, "\"old\"");
  EXPECT_EQ(literal->stringValue, "old");
}

TEST(VagentLexerTest, UnicodeEscapesRejectNonHexDigitsAndAcceptScalarBounds) {
  const auto invalid = Lexer(R"("\u{G}")").tokenize();
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().detail, "invalid Unicode escape");

  const auto valid = Lexer(R"("\u{10FFFF}")").tokenize();
  ASSERT_TRUE(valid) << valid.error().detail;
  ASSERT_EQ(valid->front().kind, TokenKind::StringLiteral);
  EXPECT_EQ(valid->front().stringValue, "\xF4\x8F\xBF\xBF");
}

TEST(VagentLexerTest, NumericKindsKeepUncoercedSpellings) {
  auto result = Lexer("1_000 0x2A 3.5 1e-3 250ms 2s 5min 1.5s").tokenize();
  ASSERT_TRUE(result) << result.error().detail;
  ASSERT_EQ(result->size(), 9U);
  EXPECT_EQ((*result)[0].text, "1_000");
  EXPECT_EQ((*result)[1].kind, TokenKind::IntegerLiteral);
  EXPECT_EQ((*result)[1].text, "0x2A");
  EXPECT_EQ((*result)[2].kind, TokenKind::FloatLiteral);
  EXPECT_EQ((*result)[3].kind, TokenKind::FloatLiteral);
  for (std::size_t i = 4; i < 8; ++i) {
    EXPECT_EQ((*result)[i].kind, TokenKind::DurationLiteral);
  }
}

TEST(VagentLexerTest, SignedMinimumMagnitudeIsAvailableToUnaryMinus) {
  auto result = Lexer("-9223372036854775808 0x8000000000000000").tokenize();
  ASSERT_TRUE(result) << result.error().detail;
  EXPECT_EQ((*result)[0].kind, TokenKind::Minus);
  EXPECT_EQ((*result)[1].text, "9223372036854775808");
  EXPECT_EQ((*result)[2].text, "0x8000000000000000");
}

TEST(VagentLexerTest, StringsDecodeDocumentedEscapesAndUnicodeScalars) {
  auto result = Lexer(R"("line\n\t\"\\\u{0}\u{00e9}\u{1f680}")").tokenize();
  ASSERT_TRUE(result) << result.error().detail;
  std::string expected = "line\n\t\"\\";
  expected += '\0';
  expected += "\xc3\xa9\xf0\x9f\x9a\x80";
  EXPECT_EQ(result->front().stringValue, expected);
  EXPECT_EQ(result->front().loc.column, 1U);
}

TEST(VagentLexerTest, MalformedNumbersAreTypedLexicalFailures) {
  constexpr std::string_view invalid[] = {"0x",
                                          "0xG",
                                          "1_",
                                          "1__2",
                                          "0x_2",
                                          "1e",
                                          "1e+",
                                          "1e_2",
                                          "1e2_",
                                          "1wat",
                                          "2hours",
                                          "0x1s",
                                          "9223372036854775809",
                                          "18446744073709551616",
                                          "1e9999",
                                          "1e-9999"};
  for (const auto source : invalid) {
    SCOPED_TRACE(source);
    auto result = Lexer(source).tokenize();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, DiagnosticCode::Lexical);
    EXPECT_FALSE(result.error().detail.empty());
  }
}

TEST(VagentLexerTest, MalformedStringsAreRefused) {
  constexpr std::string_view invalid[] = {
      "\"unfinished",    "\"raw\nnewline\"", R"("\q")",       R"("\u1234")",
      R"("\u{}")",       R"("\u{g}")",       R"("\u{d800}")", R"("\u{dfff}")",
      R"("\u{110000}")", R"("\u{0000000}")", R"("\u{123")"};
  for (const auto source : invalid) {
    SCOPED_TRACE(source);
    auto result = Lexer(source).tokenize();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, DiagnosticCode::Lexical);
  }
}

TEST(VagentLexerTest, UnsupportedCharactersAndOperatorsAreRefused) {
  constexpr std::string_view invalid[] = {
      "$name", "@", "'single quotes'", "=", "&&", "||", "~="};
  for (const auto source : invalid) {
    SCOPED_TRACE(source);
    auto result = Lexer(source).tokenize();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, DiagnosticCode::Lexical);
  }
}

TEST(VagentLexerTest, UnterminatedCommentNamesItsOpeningLocation) {
  auto result = Lexer("\n  /* never closes").tokenize();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().loc.line, 2U);
  EXPECT_EQ(result.error().loc.column, 3U);
  EXPECT_EQ(result.error().loc.offset, 3U);
  EXPECT_NE(formatDiagnostic(result.error()).find("2:3: Lexical:"),
            std::string::npos);
}

TEST(VagentLexerTest, EmptyInputStillHasLocatedEndToken) {
  auto result = Lexer("").tokenize();
  ASSERT_TRUE(result);
  ASSERT_EQ(result->size(), 1U);
  EXPECT_EQ(result->front().kind, TokenKind::EndOfFile);
  EXPECT_EQ(result->front().loc.line, 1U);
  EXPECT_EQ(result->front().loc.column, 1U);
  EXPECT_EQ(result->front().loc.offset, 0U);
}

} // namespace

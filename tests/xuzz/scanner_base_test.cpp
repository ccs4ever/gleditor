#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "common/xanadu/scanner_base.hpp"
#include "common/xanadu/vagent/lexer.hpp"
#include "common/xanadu/vicon/lexer.hpp"
#include "common/xanadu/vlace/lexer.hpp"
#include "common/xanadu/vpl/lexer.hpp"
#include "common/xanadu/vql/lexer.hpp"

namespace {

TEST(ScannerBaseTest, endOfInputDoesNotMatchANulCharacter) {
  xanadu::ScannerBase scanner("");
  EXPECT_FALSE(scanner.peekChar());
  EXPECT_FALSE(scanner.advanceChar());
  EXPECT_FALSE(scanner.match('\0'));
  EXPECT_EQ(scanner.position(), 0U);
  EXPECT_FALSE(scanner.peekIsDigit());
}

TEST(ScannerBaseTest, anActualNulIsPresentAndAdvancesExactlyOnce) {
  const std::string source("\0x", 2);
  xanadu::ScannerBase scanner(source);
  ASSERT_TRUE(scanner.peekChar());
  EXPECT_EQ(scanner.peekChar(), '\0');
  EXPECT_TRUE(scanner.match('\0'));
  EXPECT_EQ(scanner.position(), 1U);
  EXPECT_FALSE(scanner.peekChar(std::numeric_limits<std::size_t>::max()));
  EXPECT_EQ(scanner.advanceChar(), 'x');
  EXPECT_FALSE(scanner.match('\0'));
  EXPECT_EQ(scanner.position(), 2U);
}

TEST(ScannerBaseTest, quotedNulIsDataRatherThanEndOfInput) {
  const std::string source("\"a\0b\"", 5);
  xanadu::ScannerBase scanner(source);
  ASSERT_TRUE(scanner.match('"'));
  const auto parsed = scanner.scanQuotedString();
  ASSERT_TRUE(parsed.success) << parsed.errorMessage;
  EXPECT_EQ(parsed.value, std::string("a\0b", 3));
  EXPECT_TRUE(scanner.isAtEnd());
}

TEST(ScannerBaseTest, numericZeroIsPresentAndInvalidNumbersAreErrors) {
  xanadu::ScannerBase zero("0");
  const auto integer = zero.scanNumberLiteral();
  ASSERT_TRUE(integer);
  EXPECT_FALSE(integer->isFloat);
  EXPECT_EQ(integer->intValue, 0);
  xanadu::ScannerBase floating("0.0");
  const auto real = floating.scanNumberLiteral();
  ASSERT_TRUE(real);
  EXPECT_TRUE(real->isFloat);
  EXPECT_DOUBLE_EQ(real->floatValue, 0.0);
  for (const std::string_view source :
       {"9223372036854775808", "1e9999", "1e+", ""}) {
    xanadu::ScannerBase invalid(source);
    const auto result = invalid.scanNumberLiteral();
    EXPECT_FALSE(result) << source;
  }
}

TEST(ScannerBaseTest, consumedSignsRetainTheirNumericMeaning) {
  xanadu::ScannerBase positive("+42");
  ASSERT_TRUE(positive.match('+'));
  const auto plus = positive.scanNumberLiteral(false, true);
  ASSERT_TRUE(plus);
  EXPECT_EQ(plus->intValue, 42);
  xanadu::ScannerBase negative("-9223372036854775808");
  ASSERT_TRUE(negative.match('-'));
  const auto minimum = negative.scanNumberLiteral(true);
  ASSERT_TRUE(minimum);
  EXPECT_EQ(minimum->intValue, std::numeric_limits<std::int64_t>::min());
}

TEST(ScannerBaseTest, lexerErrorsDoNotBecomeNumericZero) {
  EXPECT_EQ(xanadu::vql::Lexer("1e9999").nextToken().kind,
            xanadu::vql::TokenKind::Error);
  EXPECT_EQ(xanadu::vlace::Lexer("0x").nextToken().kind,
            xanadu::vlace::TokenKind::Error);
  EXPECT_EQ(xanadu::vicon::Lexer("4294967298r11").next().kind,
            xanadu::vicon::TokenKind::Error);
  EXPECT_EQ(xanadu::vpl::Lexer("1e9999").nextToken().kind,
            xanadu::vpl::TokenKind::Error);
}

TEST(ScannerBaseTest, lexerEndChecksAndUnterminatedEscapesAreExplicit) {
  EXPECT_EQ(xanadu::vicon::Lexer("").next().kind,
            xanadu::vicon::TokenKind::EndOfFile);
  EXPECT_EQ(xanadu::vlace::Lexer("--").nextToken().kind,
            xanadu::vlace::TokenKind::EndOfFile);
  EXPECT_EQ(xanadu::vlace::Lexer("'").nextToken().kind,
            xanadu::vlace::TokenKind::Error);
  EXPECT_EQ(xanadu::vlace::Lexer("'\\").nextToken().kind,
            xanadu::vlace::TokenKind::Error);
}

TEST(ScannerBaseTest, sourceNulIsNotSilentlyConsumedAsEof) {
  const std::string source("\0agent", 6);
  const auto result = xanadu::vagent::Lexer(source).tokenize();
  EXPECT_FALSE(result);
  const std::string quoted("\"a\0b\"", 5);
  const auto token = xanadu::vql::Lexer(quoted).nextToken();
  EXPECT_EQ(token.kind, xanadu::vql::TokenKind::StringLiteral);
  EXPECT_EQ(token.stringValue, std::string("a\0b", 3));
}

} // namespace

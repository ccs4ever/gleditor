/**
 * @file vpl_lexer_test.cpp
 * @brief Unit tests for the VPL dual-syntax tokenizer (APL and J-style ASCII).
 */
#include <gtest/gtest.h>

#include "common/xanadu/vpl/lexer.hpp"

namespace xanadu::vpl {
namespace {

TEST(VplLexerTest, APLGlyphsTokenization) {
  std::string source = "A ← 5⍳d.1 +/ ⍴ ⍉ ⌽ ⊖ ↑ ↓ ⊂ ⊃ ∊ ⍸ ≡ ≢ ⍋ ⍒ ⍟ ⍫ ⌺ ⌸ ⍤";
  Lexer lexer(source);
  auto tokens = lexer.tokenizeAll();

  ASSERT_GE(tokens.size(), 20u);
  EXPECT_EQ(tokens[0].kind, TokenKind::Identifier);
  EXPECT_EQ(tokens[0].text, "A");
  EXPECT_EQ(tokens[1].kind, TokenKind::Assign);
  EXPECT_EQ(tokens[2].kind, TokenKind::Number);
  EXPECT_EQ(tokens[2].intValue, 5);
  EXPECT_EQ(tokens[3].kind, TokenKind::Iota);
  EXPECT_EQ(tokens[4].kind, TokenKind::Dimension);
  EXPECT_EQ(tokens[4].text, "d.1");
  EXPECT_EQ(tokens[5].kind, TokenKind::Plus);
  EXPECT_EQ(tokens[6].kind, TokenKind::Reduce);
  EXPECT_EQ(tokens[7].kind, TokenKind::Rho);
  EXPECT_EQ(tokens[8].kind, TokenKind::Transpose);
  EXPECT_EQ(tokens[9].kind, TokenKind::ReverseFirst);
  EXPECT_EQ(tokens[10].kind, TokenKind::ReverseLast);
  EXPECT_EQ(tokens[11].kind, TokenKind::Take);
  EXPECT_EQ(tokens[12].kind, TokenKind::Drop);
  EXPECT_EQ(tokens[13].kind, TokenKind::Enclose);
  EXPECT_EQ(tokens[14].kind, TokenKind::Disclose);
  EXPECT_EQ(tokens[15].kind, TokenKind::Member);
  EXPECT_EQ(tokens[16].kind, TokenKind::Where);
  EXPECT_EQ(tokens[17].kind, TokenKind::Match);
  EXPECT_EQ(tokens[18].kind, TokenKind::Tally);
  EXPECT_EQ(tokens[19].kind, TokenKind::GradeUp);
  EXPECT_EQ(tokens[20].kind, TokenKind::GradeDown);
  EXPECT_EQ(tokens[21].kind, TokenKind::Hypertime);
  EXPECT_EQ(tokens[22].kind, TokenKind::Scrub);
  EXPECT_EQ(tokens[23].kind, TokenKind::Transclude);
  EXPECT_EQ(tokens[24].kind, TokenKind::Key);
  EXPECT_EQ(tokens[25].kind, TokenKind::RankOp);
}

TEST(VplLexerTest, JStyleASCIITokenization) {
  std::string source = "A =. 5 i. d.1 +/ $ |: |. |.. {. }. < > e. I. -: # /: "
                       "\\: time. scrub. trans. key. \"";
  Lexer lexer(source);
  auto tokens = lexer.tokenizeAll();

  ASSERT_GE(tokens.size(), 20u);
  EXPECT_EQ(tokens[0].kind, TokenKind::Identifier);
  EXPECT_EQ(tokens[0].text, "A");
  EXPECT_EQ(tokens[1].kind, TokenKind::Assign);
  EXPECT_EQ(tokens[2].kind, TokenKind::Number);
  EXPECT_EQ(tokens[2].intValue, 5);
  EXPECT_EQ(tokens[3].kind, TokenKind::Iota);
  EXPECT_EQ(tokens[4].kind, TokenKind::Dimension);
  EXPECT_EQ(tokens[4].text, "d.1");
  EXPECT_EQ(tokens[5].kind, TokenKind::Plus);
  EXPECT_EQ(tokens[6].kind, TokenKind::Reduce);
  EXPECT_EQ(tokens[7].kind, TokenKind::Rho);
  EXPECT_EQ(tokens[8].kind, TokenKind::Transpose);
  EXPECT_EQ(tokens[9].kind, TokenKind::ReverseFirst);
  EXPECT_EQ(tokens[10].kind, TokenKind::ReverseLast);
  EXPECT_EQ(tokens[11].kind, TokenKind::Take);
  EXPECT_EQ(tokens[12].kind, TokenKind::Drop);
  EXPECT_EQ(tokens[13].kind, TokenKind::Enclose);
  EXPECT_EQ(tokens[14].kind, TokenKind::Disclose);
  EXPECT_EQ(tokens[15].kind, TokenKind::Member);
  EXPECT_EQ(tokens[16].kind, TokenKind::Where);
  EXPECT_EQ(tokens[17].kind, TokenKind::Match);
  EXPECT_EQ(tokens[18].kind, TokenKind::Tally);
  EXPECT_EQ(tokens[19].kind, TokenKind::GradeUp);
  EXPECT_EQ(tokens[20].kind, TokenKind::GradeDown);
  EXPECT_EQ(tokens[21].kind, TokenKind::Hypertime);
  EXPECT_EQ(tokens[22].kind, TokenKind::Scrub);
  EXPECT_EQ(tokens[23].kind, TokenKind::Transclude);
  EXPECT_EQ(tokens[24].kind, TokenKind::Key);
  EXPECT_EQ(tokens[25].kind, TokenKind::RankOp);
}

TEST(VplLexerTest, DualSyntaxEquivalence) {
  auto compareTokenKinds = [](std::string_view apl, std::string_view j) {
    Lexer lexerApl(apl);
    Lexer lexerJ(j);
    auto toksApl = lexerApl.tokenizeAll();
    auto toksJ   = lexerJ.tokenizeAll();

    ASSERT_EQ(toksApl.size(), toksJ.size())
        << "Mismatch in token counts for:\n  APL: " << apl << "\n  J:   " << j;

    for (std::size_t i = 0; i < toksApl.size(); ++i) {
      EXPECT_EQ(toksApl[i].kind, toksJ[i].kind)
          << "Token kind mismatch at index " << i << ":\n  APL ("
          << toksApl[i].text << ") vs J (" << toksJ[i].text << ")";
    }
  };

  // 1. Iota & assignment
  compareTokenKinds("A ← 5⍳d.1", "A =. 5 i. d.1");

  // 2. Reshape and valence
  compareTokenKinds("V ← H ⍴ (d.1 d.2 d.3)", "V =. H $ (d.1 d.2 d.3)");
  compareTokenKinds("⍴⍴V", "$$V");

  // 3. Transpose
  compareTokenKinds("⍴⍉V", "$|:V");

  // 4. Reduction & Scan
  compareTokenKinds("+/ 10⍳d.1", "+/ 10 i. d.1");
  compareTokenKinds("+\\C", "+\\C");

  // 5. Compress & Each
  compareTokenKinds("(5 < ≢¨ W) ⌿ W", "(5 < # each. W) copy. W");

  // 6. Outer product
  compareTokenKinds("A ∘.+ B", "A o.+ B");

  // 7. Clone master & Disclose
  compareTokenKinds("M ← ⊃C", "M =. >C");

  // 8. Key operator
  compareTokenKinds("W ⌸ ≢¨ W", "W key. # each. W");

  // 9. Hypertime & Scrub
  compareTokenKinds("T ← ⍟A", "T =. time. A");
  compareTokenKinds("A ⍫ T", "A scrub. T");

  // 10. Transclusion discovery
  compareTokenKinds("⌺ ⎕READ 'chapter.xanadoc'",
                    "trans. read. 'chapter.xanadoc'");
}

TEST(VplLexerTest, NumbersAndNegatives) {
  // APL high minus ¯ and J-style _
  Lexer lexer("42 ¯17 _17 3.14 ¯0.5 _0.5 1e3");
  auto tokens = lexer.tokenizeAll();

  ASSERT_EQ(tokens.size(), 8u); // 7 numbers + EOF
  EXPECT_EQ(tokens[0].kind, TokenKind::Number);
  EXPECT_EQ(tokens[0].intValue, 42);

  EXPECT_EQ(tokens[1].kind, TokenKind::Number);
  EXPECT_EQ(tokens[1].intValue, -17);

  EXPECT_EQ(tokens[2].kind, TokenKind::Number);
  EXPECT_EQ(tokens[2].intValue, -17);

  EXPECT_EQ(tokens[3].kind, TokenKind::Number);
  EXPECT_DOUBLE_EQ(tokens[3].floatValue, 3.14);

  EXPECT_EQ(tokens[4].kind, TokenKind::Number);
  EXPECT_DOUBLE_EQ(tokens[4].floatValue, -0.5);

  EXPECT_EQ(tokens[5].kind, TokenKind::Number);
  EXPECT_DOUBLE_EQ(tokens[5].floatValue, -0.5);

  EXPECT_EQ(tokens[6].kind, TokenKind::Number);
  EXPECT_DOUBLE_EQ(tokens[6].floatValue, 1000.0);
}

TEST(VplLexerTest, CommentsAndStrings) {
  std::string source = "A ← 'hello world' ⍝ this is an APL comment\n"
                       "B =. 'can''t stop' NB. this is a J comment\n"
                       "C := \"string\" // C++ comment\n";
  Lexer lexer(source);
  auto tokens = lexer.tokenizeAll();

  std::vector<TokenKind> expected = {
      TokenKind::Identifier, TokenKind::Assign,     TokenKind::String,
      TokenKind::Newline,    TokenKind::Identifier, TokenKind::Assign,
      TokenKind::String,     TokenKind::Newline,    TokenKind::Identifier,
      TokenKind::Assign,     TokenKind::String,     TokenKind::Newline,
      TokenKind::EndOfFile};

  ASSERT_EQ(tokens.size(), expected.size());
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    EXPECT_EQ(tokens[i].kind, expected[i]);
  }
  EXPECT_EQ(tokens[2].stringValue, "hello world");
  EXPECT_EQ(tokens[6].stringValue, "can't stop");
  EXPECT_EQ(tokens[10].stringValue, "string");
}

} // namespace
} // namespace xanadu::vpl

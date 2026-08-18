/**
 * @file windows_quoting.cpp
 * @brief Whether an argument survives being joined into a Windows command
 *        line and read back apart from its neighbours.
 *
 * Run on every platform this project tests on, none of which is Windows: the
 * algorithm is pure string handling with no Windows API in it, which is the
 * whole reason it was split out of provenance.cpp's run(). What is checked
 * here is what CommandLineToArgvW documents as its own parsing rules, in
 * reverse -- gpg is never run against these strings, because nothing here
 * can run gpg.exe.
 */
#include <gtest/gtest.h>

#include <xudu/core/windows_quoting.hpp>

namespace {
using xudu::quoteWindowsArgument;
}

TEST(WindowsQuotingTest, aPlainWordNeedsNoQuoting) {
  // Nothing in it a command line splits on, so nothing to protect it from.
  EXPECT_EQ(quoteWindowsArgument(L"gpg"), L"gpg");
}

TEST(WindowsQuotingTest, aSpaceMeansTheWholeThingIsQuoted) {
  // Unquoted, this would be two arguments rather than one name.
  EXPECT_EQ(quoteWindowsArgument(L"Ada Lovelace"), L"\"Ada Lovelace\"");
}

TEST(WindowsQuotingTest, anEmptyArgumentIsQuotedRatherThanDropped) {
  // An empty token is not the same as no token at all -- "gpg --output """
  // and "gpg --output" ask for different things.
  EXPECT_EQ(quoteWindowsArgument(L""), L"\"\"");
}

TEST(WindowsQuotingTest, aLiteralQuoteIsEscaped) {
  // Passed through unescaped, this quote would close the argument early and
  // let whatever follows it be read as something else.
  EXPECT_EQ(quoteWindowsArgument(L"5'11\" tall"), L"\"5'11\\\" tall\"");
}

TEST(WindowsQuotingTest, backslashesBeforeAQuoteAreDoubled) {
  // The one rule that is easy to get backwards: a backslash is ordinary
  // except immediately before a quote, where each one has to become two so
  // that it still means one literal backslash once the following quote is
  // unescaped rather than being read as escaping it.
  EXPECT_EQ(quoteWindowsArgument(LR"(C:\path\"quoted\")"),
            LR"("C:\path\\\"quoted\\\"")");
}

TEST(WindowsQuotingTest, backslashesNotBeforeAQuoteAreOrdinary) {
  // No quote follows these, so nothing here needs doubling -- only the
  // characters that make quoting necessary at all (the space) are special.
  EXPECT_EQ(quoteWindowsArgument(LR"(C:\Program Files\gpg)"),
            LR"("C:\Program Files\gpg")");
}

TEST(WindowsQuotingTest, aTrailingBackslashWithNothingElseSpecialIsNotQuoted) {
  // No space, no quote: nothing in this argument would otherwise split it, so
  // it is returned bare. A trailing backslash only needs doubling when it is
  // about to meet a closing quote, and unquoted there is none.
  EXPECT_EQ(quoteWindowsArgument(LR"(C:\gnupg\)"), LR"(C:\gnupg\)");
}

TEST(WindowsQuotingTest,
     trailingBackslashesAreDoubledBeforeASyntheticClosingQuote) {
  // The space forces quoting, and now there is a closing quote for a trailing
  // backslash to run into -- the case the whole doubling rule exists for.
  // Naive quoting would let the backslash escape the closing quote and merge
  // this argument into the one after it.
  EXPECT_EQ(quoteWindowsArgument(LR"(C:\gnupg dir\)"), LR"("C:\gnupg dir\\")");
}

TEST(WindowsQuotingTest, aPassphraseWithBothIsQuotedCorrectly) {
  // What this exists for: a passphrase is somebody else's text, typed by
  // them and not chosen to be convenient here.
  EXPECT_EQ(quoteWindowsArgument(LR"(back\slash and "quote")"),
            LR"("back\slash and \"quote\"")");
}

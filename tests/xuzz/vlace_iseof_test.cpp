/**
 * @file vlace_iseof_test.cpp
 * @brief `isEOF :: IO Bool` (spec 6.5): a peek at the console input that
 * consumes nothing, so a following getLine still returns the next line and
 * the named end-of-input refusal of getLine stays the only way a read fails.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <sstream>
#include <string>

#include "common/xanadu/vlace/driver.hpp"

namespace {

using xanadu::vlace::Driver;

struct Run {
  std::string out;
  std::string value;
  std::optional<xanadu::vlace::DriverError> error;
};

Run perform(const std::string &src, const std::string &stdinText = "") {
  Driver d;
  std::istringstream in(stdinText);
  std::ostringstream out;
  auto r = d.runIO(src, in, out);
  Run run;
  run.out = out.str();
  if (r)
    run.value = r->value;
  else
    run.error = r.error();
  return run;
}

std::string typeOf(const std::string &src) {
  Driver d;
  auto r = d.run(src);
  return r ? r->type : std::string("ERR:") + Driver::formatError(r.error());
}

// Prints the flag, then the next line when there is one.
constexpr const char *peekThenRead =
    "isEOF >>= (fn e => if e then putStrLn \"eof\" else "
    "getLine >>= (fn l => putStrLn l))";

TEST(VlaceIsEofTest, HasTypeIOBool) { EXPECT_EQ(typeOf("isEOF"), "IO Bool"); }

TEST(VlaceIsEofTest, MisuseAsABoolIsATypeErrorNamingIsEOF) {
  const std::string misuse = "if isEOF then 1 else 2";
  const auto t             = typeOf(misuse);
  EXPECT_EQ(t.rfind("ERR:", 0), 0u) << t;
  EXPECT_NE(t.find("isEOF"), std::string::npos) << t;
  const auto r = perform(misuse);
  ASSERT_TRUE(r.error.has_value());
  EXPECT_NE(Driver::formatError(*r.error).find("isEOF"), std::string::npos)
      << Driver::formatError(*r.error);
}

TEST(VlaceIsEofTest, EmptyInputIsAtEnd) {
  const auto r = perform("isEOF", "");
  ASSERT_FALSE(r.error.has_value()) << r.error->detail;
  EXPECT_EQ(r.value, "True");
}

TEST(VlaceIsEofTest, InputWithALineIsNotAtEnd) {
  const auto r = perform("isEOF", "a\n");
  ASSERT_FALSE(r.error.has_value()) << r.error->detail;
  EXPECT_EQ(r.value, "False");
}

TEST(VlaceIsEofTest, InputOfOnlyANewlineHoldsOneEmptyLine) {
  const auto r = perform(
      "isEOF >>= (fn a => getLine >>= (fn l => isEOF >>= (fn b => "
      "putStrLn ((if a then \"T\" else \"F\") ++ \"|\" ++ l ++ \"|\" ++ "
      "(if b then \"T\" else \"F\")))))",
      "\n");
  ASSERT_FALSE(r.error.has_value()) << r.error->detail;
  EXPECT_EQ(r.out, "F||T\n");
}

TEST(VlaceIsEofTest, PeekingDoesNotConsumeALine) {
  const auto r = perform("isEOF >> getLine", "first\nsecond\n");
  ASSERT_FALSE(r.error.has_value()) << r.error->detail;
  EXPECT_EQ(r.value, "first");
}

TEST(VlaceIsEofTest, RepeatedPeeksAreIdempotent) {
  const auto r = perform("isEOF >> isEOF >> isEOF >> getLine", "x\n");
  ASSERT_FALSE(r.error.has_value()) << r.error->detail;
  EXPECT_EQ(r.value, "x");
  const auto end = perform("isEOF >> isEOF >> isEOF", "");
  ASSERT_FALSE(end.error.has_value()) << end.error->detail;
  EXPECT_EQ(end.value, "True");
}

TEST(VlaceIsEofTest, InterleavedPeeksAndReadsLoseNoLine) {
  const std::string prog =
      std::string(peekThenRead) + " >> " + peekThenRead + " >> " + peekThenRead;
  const auto r = perform(prog, "one\ntwo");
  ASSERT_FALSE(r.error.has_value()) << r.error->detail;
  EXPECT_EQ(r.out, "one\ntwo\neof\n");
}

TEST(VlaceIsEofTest, FinalLineWithoutNewlineIsStillALine) {
  const auto r = perform("getLine >>= (fn l => isEOF >>= (fn e => "
                         "putStrLn (l ++ (if e then \"!\" else \"?\"))))",
                         "last");
  ASSERT_FALSE(r.error.has_value()) << r.error->detail;
  EXPECT_EQ(r.out, "last!\n");
  const auto before = perform("isEOF", "last");
  ASSERT_FALSE(before.error.has_value());
  EXPECT_EQ(before.value, "False");
}

TEST(VlaceIsEofTest, GetLineAtEndIsStillTheNamedError) {
  const auto r = perform("isEOF >> getLine", "");
  ASSERT_TRUE(r.error.has_value());
  EXPECT_NE(r.error->detail.find("getLine: end of input"), std::string::npos)
      << r.error->detail;
  const auto after = perform("getLine >> isEOF >> getLine", "only\n");
  ASSERT_TRUE(after.error.has_value());
  EXPECT_NE(after.error->detail.find("getLine: end of input"),
            std::string::npos)
      << after.error->detail;
}

// A built description peeks nothing; the peek happens when it is performed,
// after the earlier effect and before the later one.
TEST(VlaceIsEofTest, ABuiltPeekRunsNothingUntilPerformed) {
  const auto r = perform("let p = isEOF in pure 1", "");
  ASSERT_FALSE(r.error.has_value()) << r.error->detail;
  EXPECT_EQ(r.value, "1");
  const auto ordered = perform("getLine >> isEOF", "a\n");
  ASSERT_FALSE(ordered.error.has_value()) << ordered.error->detail;
  EXPECT_EQ(ordered.value, "True");
}

TEST(VlaceIsEofTest, ALoopReadsUntilEndOfInput) {
  const auto r = perform("loop = isEOF >>= (fn e => if e then pure 0 else "
                         "getLine >>= (fn l => putStrLn l >> loop)) ; loop",
                         "a\nb\nc\n");
  ASSERT_FALSE(r.error.has_value()) << r.error->detail;
  EXPECT_EQ(r.out, "a\nb\nc\n");
  EXPECT_EQ(r.value, "0");
}

// VM-FRAME makes each step cost what the one before did, so a long input is
// linear. The counter is compared each step so it is forced then; a counter
// only used at the end would be a 10,000-deep thunk chain, which is the
// program's laziness and not the input loop's.
TEST(VlaceIsEofTest, TenThousandLinesRunInLinearTime) {
  std::string input;
  constexpr int lines = 10000;
  for (int i = 0; i < lines; ++i) input += "x\n";
  const auto start = std::chrono::steady_clock::now();
  const auto r     = perform("count n = isEOF >>= (fn e => if e then pure n "
                                 "else if n < 0 then pure n else getLine >> "
                                 "count (n + 1)) ; count 0",
                             input);
  ASSERT_FALSE(r.error.has_value()) << r.error->detail;
  EXPECT_EQ(r.value, "10000");
  const auto seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  EXPECT_LT(seconds, 120.0);
}

} // namespace

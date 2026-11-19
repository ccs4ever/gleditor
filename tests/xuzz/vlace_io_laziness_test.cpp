/**
 * @file vlace_io_laziness_test.cpp
 * @brief An IO value is a description (spec 6.5): building it runs nothing, so
 * the operands of >>, >>=, pure and putStr are forced when the action is
 * PERFORMED, in order, not when it is built. Also getLine at end of input and
 * the vlace binary's handling of an IO program's final value.
 */
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>

#include "common/xanadu/vlace/driver.hpp"

namespace {

using xanadu::vlace::CompileError;
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

// The effect "a" happens, then the fault surfaces when `pure (1/0)` is run.
// Forcing the right operand while the action was built raised ArithFault
// before anything was printed.
TEST(VlaceIoLazinessTest, EarlierEffectsHappenBeforeALaterOperandFaults) {
  const auto r = perform("putStrLn \"a\" >> pure (1 / 0)");
  EXPECT_EQ(r.out, "a\n");
  ASSERT_TRUE(r.error.has_value());
  EXPECT_EQ(r.error->code, CompileError::ArithFault);
}

TEST(VlaceIoLazinessTest, PutStrArgumentIsForcedWhenPerformed) {
  const auto r =
      perform("putStrLn \"a\" >> putStr (if 1 / 0 == 0 then \"x\" else \"y\")");
  EXPECT_EQ(r.out, "a\n");
  ASSERT_TRUE(r.error.has_value());
  EXPECT_EQ(r.error->code, CompileError::ArithFault);
}

TEST(VlaceIoLazinessTest, ABoundValueThatIsNeverUsedIsNeverForced) {
  const auto r = perform("pure (1 / 0) >>= (fn x => putStrLn \"b\")");
  EXPECT_FALSE(r.error.has_value());
  EXPECT_EQ(r.out, "b\n");
}

TEST(VlaceIoLazinessTest, ABuiltDescriptionRunsNothing) {
  // The left operand of >> is only described; the pair is never performed.
  Driver d;
  const auto r = d.run("let a = putStrLn \"a\" >> pure (1 / 0) in 5");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->value, "5");
}

// A recursive loop is trampolined by the runner, one step at a time, so its
// depth does not grow with the iteration count. The activation-depth budget is
// 512 frames and the strict build of the action hit it between 500 and 1,000
// iterations. Each step costs about what the one before did (the frame opcodes
// journal what they change) and RunLimits raises the VM's lifetime backing-cell
// ceiling, so 5,000 iterations, which the default 1,048,576 refused, run in
// constant depth and a few seconds.
TEST(VlaceIoLazinessTest, ALongRecursiveLoopRunsInConstantFrameDepth) {
  const auto start = std::chrono::steady_clock::now();
  const auto r     = perform("loop n = if n == 0 then pure 0 else putStr \"\" "
                                 ">> loop (n - 1) ; loop 5000");
  EXPECT_FALSE(r.error.has_value())
      << (r.error ? Driver::formatError(*r.error) : "");
  EXPECT_EQ(r.value, "0");
  const auto seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  EXPECT_LT(seconds, 60.0);
}

// getLine :: IO String has no value to give at end of input, and the spec has
// no end-of-input form, so it is an error that names the cause rather than a
// silently empty line (which is also what an empty line reads as).
TEST(VlaceIoLazinessTest, GetLineAtEndOfInputIsANamedError) {
  const auto r = perform("getLine");
  ASSERT_TRUE(r.error.has_value());
  EXPECT_NE(r.error->detail.find("getLine: end of input"), std::string::npos)
      << r.error->detail;
}

TEST(VlaceIoLazinessTest, GetLineReadsLinesThenReportsTheEnd) {
  const auto ok = perform("getLine >>= (fn a => getLine >>= (fn b => "
                          "putStrLn (a ++ b)))",
                          "one\ntwo");
  ASSERT_FALSE(ok.error.has_value()) << ok.error->detail;
  EXPECT_EQ(ok.out, "onetwo\n");
  const auto empty = perform("getLine", "\n");
  ASSERT_FALSE(empty.error.has_value());
  EXPECT_EQ(empty.value, "");
  const auto twice = perform("getLine >> getLine", "only\n");
  ASSERT_TRUE(twice.error.has_value());
  EXPECT_NE(twice.error->detail.find("end of input"), std::string::npos);
}

// The program's type is known without evaluating it, so the runner can perform
// an IO program instead of rendering its description first.
TEST(VlaceIoLazinessTest, TypeOfDoesNotEvaluate) {
  Driver d;
  const auto t = d.typeOf("putStrLn \"a\" >> pure (1 / 0)");
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(*t, "IO Int");
  EXPECT_FALSE(d.typeOf("1 +").has_value());
  d.withTypeChecking(false);
  const auto off = d.typeOf("1 + 2");
  ASSERT_TRUE(off.has_value());
  EXPECT_EQ(*off, "");
}

std::string runBinary(const std::string &args, int &exitCode) {
  const std::string bin = std::filesystem::exists("build/vlace")
                              ? "build/vlace --headless "
                              : "vlace --headless ";
  FILE *pipe            = popen((bin + args + " 2>&1 </dev/null").c_str(), "r");
  if (pipe == nullptr) return "popen failed";
  std::string output;
  char buffer[256];
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) output += buffer;
  const int status = pclose(pipe);
  exitCode         = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return output;
}

// The runner performs an IO program and prints the action's final value, as its
// comment promises, except Unit, which carries nothing.
TEST(VlaceIoLazinessTest, TheBinaryPrintsAnIoProgramsFinalValue) {
  int code = 0;
  EXPECT_EQ(runBinary("-e 'pure 5'", code), "5\n");
  EXPECT_EQ(code, 0);
  EXPECT_EQ(runBinary("-e 'putStrLn \"a\" >> pure 7'", code), "a\n7\n");
  EXPECT_EQ(runBinary("-e 'putStrLn \"x\"'", code), "x\n");
  // The effect happens before the later operand's fault is reported.
  const auto faulted = runBinary("-e 'putStrLn \"a\" >> pure (1 / 0)'", code);
  EXPECT_EQ(faulted.substr(0, 2), "a\n");
  EXPECT_NE(faulted.find("ArithFault"), std::string::npos) << faulted;
  EXPECT_EQ(code, 1);
  EXPECT_EQ(runBinary("-e 'getLine'", code).find("end of input") !=
                std::string::npos,
            true);
  EXPECT_EQ(code, 1);
  EXPECT_EQ(runBinary("-e '1 + 2'", code), "3\n");
}

} // namespace

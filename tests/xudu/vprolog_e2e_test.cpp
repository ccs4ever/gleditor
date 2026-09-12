/**
 * @file vprolog_e2e_test.cpp
 * @brief End-to-end tests for the vprolog CLI binary runner and REPL.
 */
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;

struct ProcessResult {
  int exitCode{0};
  std::string output;
};

ProcessResult runProcess(const std::string &cmd) {
  std::string fullCmd = cmd + " 2>&1";
  FILE *pipe          = popen(fullCmd.c_str(), "r");
  if (!pipe) {
    return {-1, "Failed to popen: " + cmd};
  }
  char buffer[512];
  std::string output;
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    output += buffer;
  }
  int status = pclose(pipe);
  int code   = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return {code, output};
}

std::string vprologBin() {
  if (fs::exists("build/vprolog")) {
    return "build/vprolog";
  }
  if (fs::exists("./build/vprolog")) {
    return "./build/vprolog";
  }
  return "vprolog";
}

TEST(VPrologE2ETest, HelpFlag) {
  std::string bin = vprologBin();
  auto res        = runProcess(bin + " --help");
  EXPECT_EQ(res.exitCode, 0);
  EXPECT_NE(res.output.find("Usage: vprolog"), std::string::npos);
  EXPECT_NE(res.output.find("Hyperstructural Logic Engine"), std::string::npos);
}

TEST(VPrologE2ETest, ArithmeticEvaluation) {
  std::string bin = vprologBin();
  auto res        = runProcess(bin + " --headless -e 'X is 10 + 32.'");
  EXPECT_EQ(res.exitCode, 0);
  EXPECT_NE(res.output.find("X = 42."), std::string::npos);

  auto resMul = runProcess(bin + " --headless -e 'X is 7 * 6.'");
  EXPECT_EQ(resMul.exitCode, 0);
  EXPECT_NE(resMul.output.find("X = 42."), std::string::npos);
}

TEST(VPrologE2ETest, AppendConcatenationAndSplitting) {
  std::string bin = vprologBin();

  // Concatenation
  auto resConcat =
      runProcess(bin + " --headless -e 'append([a, b], [c], Out).'");
  EXPECT_EQ(resConcat.exitCode, 0);
  EXPECT_NE(resConcat.output.find("Out = [a, b, c]."), std::string::npos);

  // Reversible splitting
  auto resSplit = runProcess(bin + " --headless -e 'append(X, Y, [1, 2]).'");
  EXPECT_EQ(resSplit.exitCode, 0);
  EXPECT_NE(resSplit.output.find("X = [], Y = [1, 2]"), std::string::npos);
  EXPECT_NE(resSplit.output.find("X = [1], Y = [2]"), std::string::npos);
  EXPECT_NE(resSplit.output.find("X = [1, 2], Y = []"), std::string::npos);
}

TEST(VPrologE2ETest, ConsultSourceFileAndRecursion) {
  std::string bin = vprologBin();

  fs::path tempPl = fs::temp_directory_path() / "vprolog_family_test.pl";
  {
    std::ofstream ofs(tempPl);
    ofs << "parent(pam, bob).\n"
        << "parent(bob, ann).\n"
        << "parent(bob, pat).\n"
        << "ancestor(X, Y) :- parent(X, Y).\n"
        << "ancestor(X, Y) :- parent(X, Z), ancestor(Z, Y).\n";
  }

  auto res = runProcess(bin + " --headless " + tempPl.string() +
                        " -e 'ancestor(pam, X).'");
  fs::remove(tempPl);

  EXPECT_EQ(res.exitCode, 0);
  EXPECT_NE(res.output.find("X = bob"), std::string::npos);
  EXPECT_NE(res.output.find("X = ann"), std::string::npos);
  EXPECT_NE(res.output.find("X = pat"), std::string::npos);
}

TEST(VPrologE2ETest, QueryFailureExitsFalse) {
  std::string bin = vprologBin();
  auto res = runProcess(bin + " --headless -e 'append([1], [2], [99]).'");
  EXPECT_NE(res.exitCode, 0);
  EXPECT_NE(res.output.find("false."), std::string::npos);
}

TEST(VPrologE2ETest, GridVisualizer) {
  std::string bin = vprologBin();

  fs::path tempPl = fs::temp_directory_path() / "vprolog_grid_test.pl";
  {
    std::ofstream ofs(tempPl);
    ofs << "edge(a, b).\n"
        << "edge(b, c).\n";
  }

  auto res = runProcess(bin + " --headless " + tempPl.string() +
                        " -e 'edge(a, X).' --grid");
  fs::remove(tempPl);

  EXPECT_EQ(res.exitCode, 0);
  EXPECT_NE(res.output.find("X = b."), std::string::npos);
  // Grid should include cell connection topology
  EXPECT_NE(res.output.find("Cell Connection View"), std::string::npos);
  EXPECT_NE(res.output.find("Cell Link Topology Roster"), std::string::npos);
}

TEST(VPrologE2ETest, ListingFlag) {
  std::string bin = vprologBin();

  fs::path tempPl = fs::temp_directory_path() / "vprolog_listing_test.pl";
  {
    std::ofstream ofs(tempPl);
    ofs << "likes(john, pizza).\n"
        << "likes(mary, sushi).\n";
  }

  auto res = runProcess(bin + " --headless " + tempPl.string() +
                        " --listing -e 'true.'");
  fs::remove(tempPl);

  EXPECT_EQ(res.exitCode, 0);
  EXPECT_NE(res.output.find("likes(john, pizza)."), std::string::npos);
  EXPECT_NE(res.output.find("likes(mary, sushi)."), std::string::npos);
}

TEST(VPrologE2ETest, IntrospectVortexStdLibE2E) {
  std::string bin = vprologBin();
  auto res        = runProcess(bin + " --headless -e 'vortex_function(M, F).'");
  EXPECT_EQ(res.exitCode, 0);
  EXPECT_NE(res.output.find("M = std:math, F = abs"), std::string::npos);
  EXPECT_NE(res.output.find("M = std:string, F = trim"), std::string::npos);
  EXPECT_NE(res.output.find("M = std:logic, F = unify"), std::string::npos);
}

} // namespace

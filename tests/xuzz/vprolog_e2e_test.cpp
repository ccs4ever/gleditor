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

TEST(VPrologE2ETest, IntrospectVortexHyperstructureE2E) {
  std::string bin = vprologBin();

  // Instructions along +d.spin
  auto resInst = runProcess(
      bin + " --headless -e \"vortex_instruction('std:math/clamp', S, L).\"");
  EXPECT_EQ(resInst.exitCode, 0);
  EXPECT_NE(resInst.output.find("S = 0, L = #CLAMP_MAX"), std::string::npos);
  EXPECT_NE(resInst.output.find("S = 1, L = #CLAMP_MIN"), std::string::npos);

  // Contracts along +/-d.contract
  auto resCont = runProcess(
      bin + " --headless -e \"vortex_contract('std:math/div', T, L).\"");
  EXPECT_EQ(resCont.exitCode, 0);
  EXPECT_NE(resCont.output.find("T = precondition, L = #REQUIRE_NON_ZERO"),
            std::string::npos);

  // Parameter wings
  auto resParam = runProcess(
      bin + " --headless -e \"vortex_param('std:math/max', W, S, T).\"");
  EXPECT_EQ(resParam.exitCode, 0);
  EXPECT_NE(resParam.output.find("W = input, S = 0"), std::string::npos);
  EXPECT_NE(resParam.output.find("W = input, S = 1"), std::string::npos);
  EXPECT_NE(resParam.output.find("W = output, S = 0"), std::string::npos);
}

TEST(VPrologE2ETest, CutPruningAlternatives) {
  std::string bin = vprologBin();

  fs::path tempPl = fs::temp_directory_path() / "vprolog_cut_test.pl";
  {
    std::ofstream ofs(tempPl);
    ofs << "max(X, Y, X) :- X >= Y, !.\n"
        << "max(X, Y, Y).\n"
        << "classify(X, negative) :- X < 0, !.\n"
        << "classify(X, zero) :- X =:= 0, !.\n"
        << "classify(X, positive).\n"
        << "different(X, X) :- !, fail.\n"
        << "different(X, Y).\n"
        << "p(X, Y) :- q(X), !, r(Y).\n"
        << "q(1).\n"
        << "q(2).\n"
        << "r(a).\n"
        << "r(b).\n";
  }

  // 1. max/3 - green cut
  auto resMax1 = runProcess(bin + " --headless " + tempPl.string() +
                            " -m 10 -e 'max(10, 5, M).'");
  EXPECT_EQ(resMax1.exitCode, 0);
  EXPECT_NE(resMax1.output.find("M = 10."), std::string::npos);
  EXPECT_EQ(resMax1.output.find("M = 5"), std::string::npos);

  auto resMax2 = runProcess(bin + " --headless " + tempPl.string() +
                            " -m 10 -e 'max(3, 5, M).'");
  EXPECT_EQ(resMax2.exitCode, 0);
  EXPECT_NE(resMax2.output.find("M = 5."), std::string::npos);

  // 2. classify/2 - red cut
  auto resNeg = runProcess(bin + " --headless " + tempPl.string() +
                           " -e 'classify(-42, C).'");
  EXPECT_EQ(resNeg.exitCode, 0);
  EXPECT_NE(resNeg.output.find("C = negative."), std::string::npos);

  auto resZero = runProcess(bin + " --headless " + tempPl.string() +
                            " -e 'classify(0, C).'");
  EXPECT_EQ(resZero.exitCode, 0);
  EXPECT_NE(resZero.output.find("C = zero."), std::string::npos);

  auto resPos = runProcess(bin + " --headless " + tempPl.string() +
                           " -e 'classify(100, C).'");
  EXPECT_EQ(resPos.exitCode, 0);
  EXPECT_NE(resPos.output.find("C = positive."), std::string::npos);

  // 3. different/2 - cut-fail
  auto resDiff1 = runProcess(bin + " --headless " + tempPl.string() +
                             " -e 'different(apple, orange).'");
  EXPECT_EQ(resDiff1.exitCode, 0);
  EXPECT_NE(resDiff1.output.find("true."), std::string::npos);

  auto resDiff2 = runProcess(bin + " --headless " + tempPl.string() +
                             " -e 'different(apple, apple).'");
  EXPECT_NE(resDiff2.exitCode, 0);
  EXPECT_NE(resDiff2.output.find("false."), std::string::npos);

  // 4. Backtracking choices after cut
  auto resChoices = runProcess(bin + " --headless " + tempPl.string() +
                               " -m 10 -e 'p(X, Y).'");
  EXPECT_EQ(resChoices.exitCode, 0);
  EXPECT_NE(resChoices.output.find("X = 1, Y = a"), std::string::npos);
  EXPECT_NE(resChoices.output.find("X = 1, Y = b"), std::string::npos);
  EXPECT_EQ(resChoices.output.find("X = 2"), std::string::npos);

  fs::remove(tempPl);
}

} // namespace

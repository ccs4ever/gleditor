/**
 * @file vpl_e2e_test.cpp
 * @brief End-to-end CLI integration test suite for VPL compiler (vplc) and
 * runner (vpl).
 */
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace {

namespace fs = std::filesystem;

struct ProcessResult {
  int exitCode{0};
  std::string output;
};

ProcessResult runCommand(const std::string &cmdLine) {
  std::string fullCmd =
      "SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy "
      "LIBGL_ALWAYS_SOFTWARE=1 XDG_DATA_HOME=" +
      fs::current_path().string() +
      "/build/xdg/data XDG_CONFIG_HOME=" + fs::current_path().string() +
      "/build/xdg/config XDG_CACHE_HOME=" + fs::current_path().string() +
      "/build/xdg/cache " + cmdLine + " 2>&1";

  FILE *pipe = popen(fullCmd.c_str(), "r");
  if (!pipe) {
    return {-1, "Failed to popen: " + fullCmd};
  }

  char buffer[512];
  std::string out;
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    out += buffer;
  }
  int status = pclose(pipe);
  int code   = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return {code, out};
}

class VPLE2ETest : public ::testing::Test {
protected:
  void SetUp() override {
    testDir =
        fs::temp_directory_path() / ("vpl_e2e_" + std::to_string(std::rand()));
    fs::create_directories(testDir);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(testDir, ec);
  }

  fs::path testDir;
};

// -- vplc CLI Tests -----------------------------------------------------------

TEST_F(VPLE2ETest, VplcHelpFlag) {
  auto res = runCommand("./build/vplc --help");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("Usage: vplc"), std::string::npos);
  EXPECT_NE(res.output.find("--dump-ast"), std::string::npos);
  EXPECT_NE(res.output.find("--dump-asm"), std::string::npos);
  EXPECT_NE(res.output.find("--library"), std::string::npos);
}

TEST_F(VPLE2ETest, VplcDumpsAstAndCompilesIotaToAGraph) {
  auto res = runCommand("./build/vplc --dump-ast -e \"A ← 5⍳d.1\"");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("Program"), std::string::npos);
  EXPECT_NE(res.output.find("AssignExpr: A"), std::string::npos);
  EXPECT_NE(res.output.find("DyadicExpr: ⍳"), std::string::npos);
  EXPECT_NE(res.output.find("ScalarExpr: 5"), std::string::npos);
  EXPECT_NE(res.output.find("DimensionExpr: d.1"), std::string::npos);
}

TEST_F(VPLE2ETest, VplcCompilesIotaInBothSyntaxesToALoopWhoseSizeIgnoresN) {
  const auto dump = [&](const std::string &expression) {
    return runCommand("./build/vplc --dump-asm -e \"" + expression + "\"");
  };
  const auto lines = [](const std::string &text) {
    return std::ranges::count(text, '\n');
  };
  const auto small = dump("A ← 5⍳d.1");
  EXPECT_EQ(small.exitCode, 0) << small.output;
  EXPECT_NE(small.output.find("#MINT_CELL"), std::string::npos);
  const auto large = dump("A ← 500⍳d.1");
  EXPECT_EQ(large.exitCode, 0) << large.output;
  // Unrolled work would grow with N; a loop does not.
  EXPECT_EQ(lines(small.output), lines(large.output));
  const auto jSyntax = dump("A =. 5 i. d.1");
  EXPECT_EQ(jSyntax.exitCode, 0) << jSyntax.output;
  EXPECT_NE(jSyntax.output.find("#MINT_CELL"), std::string::npos);
  EXPECT_EQ(lines(jSyntax.output), lines(small.output));
}

TEST_F(VPLE2ETest, VplcConstantFoldingOptimization) {
  auto resUnopt = runCommand("./build/vplc --dump-asm -e \"3 + 4\"");
  EXPECT_EQ(resUnopt.exitCode, 0) << resUnopt.output;
  EXPECT_NE(resUnopt.output.find("#ADD"), std::string::npos);

  auto resOpt = runCommand("./build/vplc --dump-asm -O -e \"3 + 4\"");
  EXPECT_EQ(resOpt.exitCode, 0) << resOpt.output;
  EXPECT_EQ(resOpt.output.find("#ADD"), std::string::npos);
  EXPECT_NE(resOpt.output.find("(7)"), std::string::npos);
}

TEST_F(VPLE2ETest, VplcStoreExportAndReload) {
  fs::path outStore = testDir / "vplc_store";
  auto res          = runCommand("./build/vplc -e \"10 + 20\" -o \"" +
                                 outStore.string() + "\"");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_TRUE(fs::exists(outStore / "ops.nodes"));
  EXPECT_TRUE(fs::exists(outStore / "store.tables"));

  auto permascroll = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Store store(permascroll);
  EXPECT_NO_THROW(store.load(outStore.string()));
  EXPECT_TRUE(store.homeCell().has_value());
}

// -- vpl CLI Tests ------------------------------------------------------------

TEST_F(VPLE2ETest, VplHelpFlag) {
  auto res = runCommand("./build/vpl --help");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("Usage: vpl"), std::string::npos);
  EXPECT_NE(res.output.find("--engine"), std::string::npos);
  EXPECT_NE(res.output.find("--grid"), std::string::npos);
}

TEST_F(VPLE2ETest, VplDefaultVmReduction) {
  auto res = runCommand("./build/vpl --headless -e \"+/ 1 2 3 4 5\"");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("15"), std::string::npos);
}

TEST_F(VPLE2ETest, VplEvaluatesIotaInBothSyntaxesOnTheVm) {
  for (const auto &expression : {"5⍳d.1", "5 i. d.1"}) {
    const auto result = runCommand("./build/vpl --headless -e \"" +
                                   std::string(expression) + "\"");
    EXPECT_EQ(result.exitCode, 0) << result.output;
    EXPECT_NE(result.output.find("1 2 3 4 5"), std::string::npos)
        << result.output;
  }
}

TEST_F(VPLE2ETest, VplVortexEngineEvaluation) {
  auto res = runCommand("./build/vpl --headless --engine vortex -e \"3 + 4\"");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("7"), std::string::npos);
}

TEST_F(VPLE2ETest, VplGridVisualization) {
  auto res = runCommand("./build/vpl --headless --grid -e \"1 2 3 4 5\"");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("Xanadu Zigzag Cell Connection View"),
            std::string::npos);
  EXPECT_NE(res.output.find("2D Spatial Lattice Projection"),
            std::string::npos);
  EXPECT_NE(res.output.find("Viewing Dimensions: [X] d.step"),
            std::string::npos);
  EXPECT_NE(res.output.find("Result Set: 5 cells"), std::string::npos);
}

TEST_F(VPLE2ETest, VplScriptFileExecution) {
  fs::path scriptPath = testDir / "test.vpl";
  {
    std::ofstream ofs(scriptPath);
    ofs << "A ← 3\n";
    ofs << "A + 12\n";
  }

  auto res =
      runCommand("./build/vpl --headless \"" + scriptPath.string() + "\"");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("15"), std::string::npos);
}

TEST_F(VPLE2ETest, VplPipedReplInput) {
  std::string cmd = "printf '3 + 4\\n:quit\\n' | ./build/vpl --headless";
  auto res        = runCommand(cmd);
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("7"), std::string::npos);
}

} // namespace

TEST_F(VPLE2ETest, VmDefaultPreservesCompleteVector) {
  const auto result = runCommand("./build/vpl -e '1 2 3'");
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_EQ(result.output, "1 2 3\n");
}

TEST_F(VPLE2ETest, VmProgramReturnsFinalExpression) {
  const auto source = testDir / "last.vpl";
  {
    std::ofstream out(source);
    out << "1 + 2\n4 + 5\n";
  }
  const auto result = runCommand("./build/vpl -f '" + source.string() + "'");
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_EQ(result.output, "9\n");
}

TEST_F(VPLE2ETest, VmReplPreservesBindingsAndRankShape) {
  const auto source = testDir / "session.txt";
  {
    std::ofstream out(source);
    out << "A ← 1 2 3\nA\nB ← 5\nB + 2\n";
  }
  const auto result = runCommand("./build/vpl < '" + source.string() + "'");
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_EQ(result.output, "1 2 3\n1 2 3\n5\n7\n");
}

TEST_F(VPLE2ETest, DirectFlagRefusesBeforeExecution) {
  const auto result = runCommand("./build/vpl --engine direct -e '1 + 2'");
  EXPECT_EQ(result.exitCode, 2) << result.output;
  EXPECT_NE(result.output.find("Unsupported VPL execution engine"),
            std::string::npos);
}

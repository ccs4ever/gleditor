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
      "/build/xdg/config " + cmdLine + " 2>&1";

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

TEST_F(VPLE2ETest, VplcDumpAst) {
  auto res = runCommand("./build/vplc --dump-ast -e \"A ← 5⍳d.1\"");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("Program"), std::string::npos);
  EXPECT_NE(res.output.find("AssignExpr: A"), std::string::npos);
  EXPECT_NE(res.output.find("DyadicExpr: ⍳"), std::string::npos);
  EXPECT_NE(res.output.find("ScalarExpr: 5"), std::string::npos);
  EXPECT_NE(res.output.find("DimensionExpr: d.1"), std::string::npos);
}

TEST_F(VPLE2ETest, VplcDumpAsmDualSyntax) {
  auto resApl = runCommand("./build/vplc --dump-asm -e \"A ← 5⍳d.1\"");
  EXPECT_EQ(resApl.exitCode, 0) << resApl.output;
  EXPECT_NE(resApl.output.find("#NEW"), std::string::npos);
  EXPECT_NE(resApl.output.find("#VALUE"), std::string::npos);
  EXPECT_NE(resApl.output.find("#BIND A"), std::string::npos);
  EXPECT_NE(resApl.output.find("#HALT"), std::string::npos);

  auto resJ = runCommand("./build/vplc --dump-asm -e \"A =. 5 i. d.1\"");
  EXPECT_EQ(resJ.exitCode, 0) << resJ.output;
  EXPECT_NE(resJ.output.find("#NEW"), std::string::npos);
  EXPECT_NE(resJ.output.find("#VALUE"), std::string::npos);
  EXPECT_NE(resJ.output.find("#BIND A"), std::string::npos);
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
  EXPECT_NE(store.homeCell(), zigzag::noCell);
}

// -- vpl CLI Tests ------------------------------------------------------------

TEST_F(VPLE2ETest, VplHelpFlag) {
  auto res = runCommand("./build/vpl --help");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("Usage: vpl"), std::string::npos);
  EXPECT_NE(res.output.find("--engine"), std::string::npos);
  EXPECT_NE(res.output.find("--grid"), std::string::npos);
}

TEST_F(VPLE2ETest, VplDirectEvaluation) {
  auto res = runCommand("./build/vpl --headless -e \"+/ 1 2 3 4 5\"");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("15"), std::string::npos);
}

TEST_F(VPLE2ETest, VplDualSyntaxEvaluation) {
  auto resApl = runCommand("./build/vpl --headless -e \"5⍳d.1\"");
  EXPECT_EQ(resApl.exitCode, 0) << resApl.output;
  EXPECT_NE(resApl.output.find("1 2 3 4 5"), std::string::npos);

  auto resJ = runCommand("./build/vpl --headless -e \"5 i. d.1\"");
  EXPECT_EQ(resJ.exitCode, 0) << resJ.output;
  EXPECT_NE(resJ.output.find("1 2 3 4 5"), std::string::npos);
}

TEST_F(VPLE2ETest, VplVortexEngineEvaluation) {
  auto res = runCommand("./build/vpl --headless --engine vortex -e \"3 + 4\"");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("7"), std::string::npos);
}

TEST_F(VPLE2ETest, VplGridVisualization) {
  auto res = runCommand("./build/vpl --headless --grid -e \"5 ⍳ d.1\"");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("Xanadu Zigzag Cell Connection View"),
            std::string::npos);
  EXPECT_NE(res.output.find("2D Spatial Lattice Projection"),
            std::string::npos);
  EXPECT_NE(res.output.find("+d.1--->"), std::string::npos);
}

TEST_F(VPLE2ETest, VplScriptFileExecution) {
  fs::path scriptPath = testDir / "test.vpl";
  {
    std::ofstream ofs(scriptPath);
    ofs << "A ← 5⍳d.1\n";
    ofs << "+/ A\n";
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

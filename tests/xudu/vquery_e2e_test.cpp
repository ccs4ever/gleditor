/**
 * @file vquery_e2e_test.cpp
 * @brief End-to-end CLI integration test suite for the VQL query runner
 * (vquery).
 *
 * Tests CLI argument handling, multi-store loading, ##NAME resolution,
 * on-the-fly Vortex compilation, in-place store edits, and output store
 * creation.
 */
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace {

namespace fs = std::filesystem;

struct ProcessResult {
  int exitCode{0};
  std::string output;
};

ProcessResult runVQuery(const std::string &args) {
  std::string binary = "./build/vquery";
  if (!fs::exists(binary)) {
    return {-1, "vquery binary not found at " + binary};
  }

  std::string cmd =
      "SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy "
      "LIBGL_ALWAYS_SOFTWARE=1 XDG_DATA_HOME=" +
      fs::current_path().string() +
      "/build/xdg/data XDG_CONFIG_HOME=" + fs::current_path().string() +
      "/build/xdg/config " + binary + " " + args + " 2>&1";

  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    return {-1, "Failed to popen: " + cmd};
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

class VQueryE2ETest : public ::testing::Test {
protected:
  void SetUp() override {
    testDir = fs::temp_directory_path() /
              ("vquery_test_" + std::to_string(std::rand()));
    fs::create_directories(testDir);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(testDir, ec);
  }

  fs::path testDir;
};

TEST_F(VQueryE2ETest, HelpFlag) {
  auto res = runVQuery("--help");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("Usage: vquery"), std::string::npos);
  EXPECT_NE(res.output.find("--engine"), std::string::npos);
  EXPECT_NE(res.output.find("--output-store"), std::string::npos);
}

TEST_F(VQueryE2ETest, DirectEvaluationBasic) {
  auto res = runVQuery("-e \"##\" --format cells");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("[#"), std::string::npos);
}

TEST_F(VQueryE2ETest, OnTheFlyVortexCompilation) {
  auto res =
      runVQuery("-e \"##/d.1\" --engine vortex --dump-asm --format cells");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("#TRAVERSE"), std::string::npos);
  EXPECT_NE(res.output.find("#HALT"), std::string::npos);
}

TEST_F(VQueryE2ETest, AsciiVisualizationMode) {
  auto res = runVQuery("-e \"##\" --ascii");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("=== VQL Query Results"), std::string::npos);
}

TEST_F(VQueryE2ETest, MultiStoreNamedResolution) {
  fs::path storeA = "tests/samples/xudu/core_hypertext/xanadoc_a";
  fs::path storeB = "tests/samples/xudu/core_hypertext/xanadoc_b";

  if (!fs::exists(storeA) || !fs::exists(storeB)) {
    GTEST_SKIP() << "Sample fixtures not present";
  }

  // 1. Query name of store B
  std::string argsName =
      storeA.string() + " " + storeB.string() + " -e \"##xanadoc_b/d.name\"";
  auto resName = runVQuery(argsName);
  EXPECT_EQ(resName.exitCode, 0) << resName.output;
  EXPECT_NE(resName.output.find("xanadoc_b"), std::string::npos);

  // 2. Query role of store B (should be library)
  std::string argsRoleB =
      storeA.string() + " " + storeB.string() + " -e \"##xanadoc_b/d.role\"";
  auto resRoleB = runVQuery(argsRoleB);
  EXPECT_EQ(resRoleB.exitCode, 0) << resRoleB.output;
  EXPECT_NE(resRoleB.output.find("library"), std::string::npos);

  // 3. Query role of store A (should be primary)
  std::string argsRoleA =
      storeA.string() + " " + storeB.string() + " -e \"##xanadoc_a/d.role\"";
  auto resRoleA = runVQuery(argsRoleA);
  EXPECT_EQ(resRoleA.exitCode, 0) << resRoleA.output;
  EXPECT_NE(resRoleA.output.find("primary"), std::string::npos);
}

TEST_F(VQueryE2ETest, OutputStoreCreation) {
  fs::path outStorePath = testDir / "result_store";

  auto res =
      runVQuery("-e \"##/d.step%'CreatedCell'\" -o " + outStorePath.string());
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("Result store written to:"), std::string::npos);

  // Verify store directory was created and contains valid ops.nodes
  EXPECT_TRUE(fs::exists(outStorePath / "ops.nodes"));

  // Verify store can be loaded by xanadu::Store
  auto perma = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Store store(perma);
  EXPECT_NO_THROW(store.load(outStorePath.string()));
  EXPECT_FALSE(store.primaryCurrentVersion().isZero());
}

TEST_F(VQueryE2ETest, InPlaceStoreMutation) {
  fs::path sampleSrc = "tests/samples/xudu/core_hypertext/xanadoc_a";
  if (!fs::exists(sampleSrc)) {
    GTEST_SKIP() << "Sample fixture xanadoc_a not present";
  }

  fs::path copyStore = testDir / "inplace_store";
  fs::copy(sampleSrc, copyStore, fs::copy_options::recursive);

  auto res = runVQuery(copyStore.string() +
                       " -e \"##/d.mutated%'CellData'\" --in-place");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("Saved in-place changes to primary store:"),
            std::string::npos);

  // Load and verify store is valid
  auto perma = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Store store(perma);
  EXPECT_NO_THROW(store.load(copyStore.string()));
}

TEST_F(VQueryE2ETest, QueryFileExecution) {
  fs::path qFile = testDir / "query.vql";
  {
    std::ofstream ofs(qFile);
    ofs << "##/d.test%'FromVQLFile'\n";
  }

  auto res = runVQuery("-f " + qFile.string());
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("FromVQLFile"), std::string::npos);
}

TEST_F(VQueryE2ETest, CellConnectionViewMode) {
  auto res = runVQuery("-e \"##/d.1%hello%world/d.2%bar!both\" --view");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("=== Xanadu Zigzag Cell Connection View ==="),
            std::string::npos);
  EXPECT_NE(res.output.find("Viewing Dimensions: [X] d.1  [Y] d.2  [Z] d.3"),
            std::string::npos);
  EXPECT_NE(res.output.find("Result Set: 4 cells"), std::string::npos);
  EXPECT_NE(res.output.find("[2D Spatial Lattice Projection]"),
            std::string::npos);
  EXPECT_NE(res.output.find("+d.1"), std::string::npos);
  EXPECT_NE(res.output.find("+d.2"), std::string::npos);
  EXPECT_NE(res.output.find("--- Cell Link Topology Roster ---"),
            std::string::npos);
  EXPECT_NE(res.output.find("hello"), std::string::npos);
  EXPECT_NE(res.output.find("world"), std::string::npos);
  EXPECT_NE(res.output.find("bar"), std::string::npos);
}

TEST_F(VQueryE2ETest, CellConnectionCustomDimensions) {
  auto res =
      runVQuery("-e \"##/d.1%hello%world/d.2%bar!both\" --grid --dims d.2,d.1");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("=== Xanadu Zigzag Cell Connection View ==="),
            std::string::npos);
  EXPECT_NE(res.output.find("Viewing Dimensions: [X] d.2  [Y] d.1"),
            std::string::npos);
  EXPECT_NE(res.output.find("Result Set: 4 cells"), std::string::npos);
  EXPECT_NE(res.output.find("+d.2"), std::string::npos);
  EXPECT_NE(res.output.find("+d.1"), std::string::npos);
}

} // namespace

#include <gtest/gtest.h>

#include <gleditor/paths.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

/**
 * @brief Where the program looks for its shaders and its icon.
 *
 * The search matters to packaging more than to anything else: every distro
 * package installs the data files somewhere the working directory has no
 * relation to, so a mistake here is a package that builds, installs, and then
 * fails to start. SDL is not initialised in these tests, so the two
 * executable-relative candidates are skipped and what is exercised is the
 * environment override and the source-tree fallback.
 */
class AssetPathTest : public testing::Test {
protected:
  void SetUp() override { clear(); }
  void TearDown() override { clear(); }

  static void clear() {
    unsetenv("GLEDITOR_ASSET_DIR");
    gleditor::resetAssetDirForTesting();
  }

  static void request(const std::string &dir) {
    setenv("GLEDITOR_ASSET_DIR", dir.c_str(), 1);
    gleditor::resetAssetDirForTesting();
  }
};

TEST_F(AssetPathTest, fallsBackToTheSourceTree) {
  EXPECT_EQ(gleditor::assetDir(), "assets");
}

TEST_F(AssetPathTest, theEnvironmentOverridesEverything) {
  request("/opt/gleditor/share");
  EXPECT_EQ(gleditor::assetDir(), "/opt/gleditor/share");
}

// Reported as given rather than checked for existence: a wrong value should
// surface as "no shader at the path you named", not as a silent fall-through
// to a directory the caller did not ask for.
TEST_F(AssetPathTest, aRequestedDirectoryIsNotSecondGuessed) {
  request("/nowhere/at/all");
  EXPECT_EQ(gleditor::assetDir(), "/nowhere/at/all");
}

// An empty variable is not a request. Leaving it set to nothing is how a shell
// script accidentally passes "unset", and honouring it would send the program
// looking in the filesystem root.
TEST_F(AssetPathTest, anEmptyRequestIsNoRequest) {
  setenv("GLEDITOR_ASSET_DIR", "", 1);
  gleditor::resetAssetDirForTesting();
  EXPECT_EQ(gleditor::assetDir(), "assets");
}

TEST_F(AssetPathTest, joinsWithThePlatformSeparator) {
  request("/usr/share/gleditor");
  const std::filesystem::path expected =
      std::filesystem::path("/usr/share/gleditor") / "shaders";
  EXPECT_EQ(gleditor::assetPath("shaders"), expected.string());
}

// The answer is cached, so that a program which has already loaded shaders
// from one directory cannot be sent to another midway through.
TEST_F(AssetPathTest, theAnswerDoesNotChangeUnderTheProgram) {
  request("/first");
  ASSERT_EQ(gleditor::assetDir(), "/first");
  setenv("GLEDITOR_ASSET_DIR", "/second", 1);
  EXPECT_EQ(gleditor::assetDir(), "/first");
}

// The shaders really are where the fallback says they are, which is what keeps
// `make run` and the comparison scripts working from the source tree.
TEST_F(AssetPathTest, theSourceTreeFallbackNamesRealShaders) {
  EXPECT_TRUE(std::filesystem::exists(gleditor::assetPath("shaders") +
                                      "/glyph.vert.glsl"))
      << "run the tests from the repository root";
}

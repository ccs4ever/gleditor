#include <gtest/gtest.h>

#include <gleditor/paths.hpp>

#include <cstdlib>
#include <filesystem>
#include <optional>
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

TEST(XdgPathsTest, respectsEnvironmentAndFallbacks) {
  // Every variable the assertions below depend on, not just the one the first
  // half sets. This used to save and clear `XDG_CONFIG_HOME` alone while
  // asserting on cacheDir() and dataDir() as well, so the fallback half only
  // passed on a machine where `XDG_DATA_HOME` and `XDG_CACHE_HOME` happened to
  // be unset -- which stopped being true the moment `make` started exporting
  // `XDG_DATA_HOME` to keep test runs out of the developer's real permascroll.
  // A test that reads the environment has to own all of it.
  struct SavedVar {
    const char *name;
    std::optional<std::string> value;

    explicit SavedVar(const char *const varName) : name(varName) {
      if (const auto *const held = std::getenv(varName); nullptr != held) {
        value = held;
      }
      unsetenv(varName);
    }
    ~SavedVar() {
      if (value.has_value()) {
        setenv(name, value->c_str(), 1);
      } else {
        unsetenv(name);
      }
    }
    SavedVar(const SavedVar &)            = delete;
    SavedVar &operator=(const SavedVar &) = delete;
    SavedVar(SavedVar &&)                 = delete;
    SavedVar &operator=(SavedVar &&)      = delete;
  };

  const SavedVar config("XDG_CONFIG_HOME");
  const SavedVar data("XDG_DATA_HOME");
  const SavedVar cache("XDG_CACHE_HOME");
  const SavedVar home("HOME");

  setenv("XDG_CONFIG_HOME", "/custom/config", 1);
  EXPECT_EQ(gleditor::paths::configDir("xudu"), "/custom/config/xudu");
  EXPECT_EQ(gleditor::paths::configPath("xudu", "config.yaml"),
            "/custom/config/xudu/config.yaml");

  unsetenv("XDG_CONFIG_HOME");
  setenv("HOME", "/home/testuser", 1);
  EXPECT_EQ(gleditor::paths::configDir("xudu"), "/home/testuser/.config/xudu");
  EXPECT_EQ(gleditor::paths::cacheDir("zigzag/slices"),
            "/home/testuser/.cache/zigzag/slices");
  EXPECT_EQ(gleditor::paths::dataDir("gleditor"),
            "/home/testuser/.local/share/gleditor");
}

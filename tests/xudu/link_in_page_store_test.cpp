/**
 * @file link_in_page_store_test.cpp
 * @brief A link forged while a Ctrl+N page is active is filed in that page's
 *        own store; it must still be drawn, stepped to and read in the link
 *        panel, as one forged on the default document is.
 *
 * Found by J17 (design/audit-2026-10-03-ux-j17.md): the beams read links from
 * store 0 alone, so such a link was saved but nothing could reach it. Driven
 * through the real binary with the chords a person would press, because the
 * defect lived in how the program gathers what it shows.
 */
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <sys/wait.h>

namespace {

namespace fs = std::filesystem;

struct ExecutionResult {
  int exitCode{-1};
  std::string output;
};

ExecutionResult executeProcess(const std::string &cmd) {
  const std::string fullCmd = cmd + " 2>&1";
  FILE *pipe                = popen(fullCmd.c_str(), "r");
  if (nullptr == pipe) {
    return {-1, "Failed to popen: " + cmd};
  }
  char buffer[512];
  std::string output;
  while (nullptr != fgets(buffer, sizeof(buffer), pipe)) {
    output += buffer;
  }
  const int status = pclose(pipe);
  const int code   = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return {code, output};
}

fs::path findXuduBinary() {
  const std::vector<fs::path> candidates = {
      fs::current_path() / "build" / "xudu",
      fs::current_path() / "xudu",
      fs::current_path() / ".." / "build" / "xudu",
  };
  for (const auto &cand : candidates) {
    if (fs::exists(cand) && (fs::status(cand).permissions() &
                             fs::perms::owner_exec) != fs::perms::none) {
      return cand;
    }
  }
  return fs::current_path() / "build" / "xudu";
}

class LinkInPageStoreTest : public testing::Test {
protected:
  fs::path testRoot;

  void SetUp() override {
    testRoot = fs::current_path() / "build" / "test_link_in_page_store";
    fs::remove_all(testRoot);
    fs::create_directories(testRoot);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(testRoot, ec);
  }
};

TEST_F(LinkInPageStoreTest, ALinkForgedOnANewPageIsSteppedToAndRead) {
  const auto xuduBin = findXuduBinary();
  ASSERT_TRUE(fs::exists(xuduBin)) << "xudu binary not found at " << xuduBin;

  // Its own homes: a launch with no store opens the default xanadoc there.
  const std::string cmd = "XDG_DATA_HOME=" + (testRoot / "data").string() +
                          " XDG_CONFIG_HOME=" + (testRoot / "config").string() +
                          " " + xuduBin.string() +
                          " --headless"
                          " --chord Ctrl+N --type 'Homestead page passage here'"
                          " --chord Ctrl+N --type 'Toward page passage here'"
                          " --select 0,11 --chord 'Ctrl+Alt+]'"
                          " --chord Ctrl+2 --select 0,14 --chord 'Ctrl+Alt+['"
                          " --chord Ctrl+Alt+L --chord Alt+Shift+N --dump-a11y";
  const auto res = executeProcess(cmd);

  ASSERT_EQ(res.exitCode, 0) << res.output;
  ASSERT_NE(res.output.find("forged clasp link on active document 1"),
            std::string::npos)
      << res.output;
  EXPECT_NE(res.output.find("links between the open documents"),
            std::string::npos)
      << "the link was never drawn: " << res.output;
  EXPECT_NE(res.output.find("group \"Selected link\""), std::string::npos)
      << "Alt+Shift+N found nothing to step to: " << res.output;
  EXPECT_NE(res.output.find("document 1, bytes 0 to 14"), std::string::npos)
      << res.output;
  EXPECT_NE(res.output.find("document 2, bytes 0 to 11"), std::string::npos)
      << res.output;
}

} // namespace

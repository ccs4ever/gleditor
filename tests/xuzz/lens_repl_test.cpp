/**
 * @file lens_repl_test.cpp
 * @brief LensRepl: the ":lens" surface every Vortex frontend shares --
 *        open/mount/unmount/next/commit/close over openLensUri, with
 *        the meta: fold a frontend binds itself.
 */
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <unistd.h>

#include <gtest/gtest.h>

#include "common/xanadu/zigzag/lens_repl.hpp"

namespace {

namespace fs = std::filesystem;
using zigzag::LensRepl;

struct LensReplFixture {
  fs::path dir;
  LensReplFixture() {
    dir =
        fs::temp_directory_path() / ("lens-repl-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir / "repo");
    run("git init -q -b main " + (dir / "repo").string());
    run("git -C " + (dir / "repo").string() +
        " config user.email t@t && "
        "git -C " +
        (dir / "repo").string() + " config user.name t");
    run("git -C " + (dir / "repo").string() + " commit -q --allow-empty -m c1");
    {
      std::ofstream f(dir / "cfg.json");
      f << "{\"a\":1}\n";
    }
  }
  ~LensReplFixture() { fs::remove_all(dir); }
  static void run(const std::string &cmd) {
    EXPECT_EQ(::system(cmd.c_str()), 0) << cmd;
  }
};

TEST(LensReplTest, HandlesOnlyItsOwnPrefix) {
  EXPECT_TRUE(LensRepl::handles(":lens"));
  EXPECT_TRUE(LensRepl::handles(":lens /tmp"));
  EXPECT_FALSE(LensRepl::handles(":lenses"));
  EXPECT_FALSE(LensRepl::handles("lens"));
  EXPECT_FALSE(LensRepl::handles(":quit"));
}

TEST(LensReplTest, OpenPrintsTheTreeWithKeysAndCloseClears) {
  LensReplFixture fix;
  LensRepl repl;
  std::ostringstream out;
  EXPECT_TRUE(repl.run(":lens " + fix.dir.string(), out));
  EXPECT_NE(out.str().find("cells,"), std::string::npos);
  EXPECT_NE(out.str().find("repo  [dir"), std::string::npos);
  EXPECT_NE(out.str().find("fs:"), std::string::npos) << "keys shown";

  out.str("");
  EXPECT_TRUE(repl.run(":lens close", out));
  out.str("");
  repl.run(":lens", out);
  EXPECT_NE(out.str().find("nothing open"), std::string::npos);
}

TEST(LensReplTest, MountAndUnmountRoundTripThroughAFileBrowser) {
  LensReplFixture fix;
  LensRepl repl;
  std::ostringstream out;
  repl.run(":lens " + fix.dir.string(), out);
  // The repo row prints its basename and its fs: key on the same line.
  const auto repoAt = out.str().find("repo  [dir");
  ASSERT_NE(repoAt, std::string::npos);
  const auto fsKey = out.str().find("fs:", repoAt);
  ASSERT_NE(fsKey, std::string::npos);
  const auto key = out.str().substr(fsKey, out.str().find('\n', fsKey) - fsKey);
  const auto end = key.find_last_not_of(" ");
  const auto trimmed = key.substr(0, end + 1);

  out.str("");
  repl.run(":lens mount " + trimmed, out);
  EXPECT_NE(out.str().find("git.repo"), std::string::npos) << out.str();

  out.str("");
  repl.run(":lens unmount " + trimmed, out);
  EXPECT_EQ(out.str().find("git.repo"), std::string::npos);
  EXPECT_EQ(out.str().find("unmount refused"), std::string::npos);
}

TEST(LensReplTest, BadUriRefusesWithoutLosingTheSession) {
  LensReplFixture fix;
  LensRepl repl;
  std::ostringstream out;
  repl.run(":lens " + fix.dir.string(), out);
  out.str("");
  repl.run(":lens sql://nonsense::", out);
  // A refused open reports; the earlier session is untouched.
  out.str("");
  repl.run(":lens", out);
  EXPECT_NE(out.str().find("cells,"), std::string::npos);
}

} // namespace

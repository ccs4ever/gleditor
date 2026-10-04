/**
 * @file keyboard_slice_test.cpp
 * @brief Journey J2 of design/ux_workflow_real_work.md, by keyboard alone:
 *        start a slice, add cells, name them and link them, in the built
 *        xuzz binary.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <sys/wait.h>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct Result {
  int exitCode{-1};
  std::string output;
};

Result run(const std::string &command) {
  Result result;
  FILE *const pipe = popen((command + " 2>&1").c_str(), "r");
  if (nullptr == pipe) {
    return result;
  }
  std::array<char, 512> buffer{};
  while (nullptr != fgets(buffer.data(), buffer.size(), pipe)) {
    result.output += buffer.data();
  }
  const int status = pclose(pipe);
  result.exitCode  = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return result;
}

fs::path built(const std::string &name) {
  return fs::current_path() / "build" / name;
}

// Every letter here is a ZigZag command while ZigZag has the keyboard, and
// would be text in the document if the keyboard were not routed by pane:
// the audit found no way to reach cells from xuzz at all.
TEST(KeyboardSliceTest, aSliceIsBuiltNamedAndLinkedFromTheKeyboard) {
  ASSERT_TRUE(fs::exists(built("xuzz"))) << "xuzz did not link";
  ASSERT_TRUE(fs::exists(built("xudu-dump"))) << "xudu-dump did not link";

  const auto root = fs::current_path() / "build" / "xuzz_keyboard_slice";
  fs::remove_all(root);
  fs::create_directories(root);
  const auto permascroll = root / "permascroll";

  const auto session =
      run("XDG_CONFIG_HOME=" + (root / "config").string() +
          " XDG_DATA_HOME=" + (root / "data").string() + " timeout 120 " +
          built("xuzz").string() + " --permascroll " + permascroll.string() +
          " --profile --chord Ctrl+Alt+N --chord N --chord E --type alpha"
          " --chord Return --chord M --chord Home --chord D --chord E"
          " --type beta --chord Return --chord L");
  ASSERT_EQ(session.exitCode, 0) << session.output;
  EXPECT_THAT(session.output, ::testing::HasSubstr("started a new slice"));

  std::vector<fs::path> untitled;
  for (const auto &entry :
       fs::directory_iterator(root / "data" / "xudu" / "xanadocs")) {
    if (entry.path().filename().string().starts_with("untitled-")) {
      untitled.push_back(entry.path());
    }
  }
  ASSERT_EQ(untitled.size(), 1U) << session.output;

  const auto dump = run(built("xudu-dump").string() +
                        " --section=ops --permascroll=" + permascroll.string() +
                        " " + untitled.front().string());
  ASSERT_EQ(dump.exitCode, 0) << dump.output;
  EXPECT_THAT(dump.output,
              ::testing::ContainsRegex("make cell.*text=\"home\""));
  EXPECT_THAT(dump.output,
              ::testing::ContainsRegex("setValue.*text=\"alpha\""));
  EXPECT_THAT(dump.output, ::testing::ContainsRegex("setValue.*text=\"beta\""));
  // Two insertions and the marked link: at least three links made by keys.
  std::size_t links = 0;
  for (std::size_t at = 0;
       (at = dump.output.find("[setLink", at)) != std::string::npos; ++at) {
    ++links;
  }
  EXPECT_GE(links, 3U) << dump.output;
  // None of it was typed into the slice's document.
  EXPECT_THAT(dump.output, ::testing::Not(::testing::HasSubstr("kind=insert")))
      << dump.output;
}

// J5: quit and relaunch with nothing named, and carry on. The selection made
// before quitting is what typing replaces afterwards, and the slice comes
// back focused where it was with the keyboard still in it, so E renames that
// cell and the edit continues its line of history rather than forking it.
TEST(KeyboardSliceTest, aRelaunchedSessionCarriesOnWhereItStopped) {
  ASSERT_TRUE(fs::exists(built("xuzz"))) << "xuzz did not link";
  const auto root = fs::current_path() / "build" / "xuzz_resume";
  fs::remove_all(root);
  fs::create_directories(root);
  const auto xuzz = [&](const std::string &script) {
    return run("XDG_CONFIG_HOME=" + (root / "config").string() +
               " XDG_DATA_HOME=" + (root / "data").string() + " timeout 120 " +
               built("xuzz").string() + " --profile " + script);
  };
  const auto untitledStores = [&] {
    std::vector<fs::path> found;
    for (const auto &entry :
         fs::directory_iterator(root / "data" / "xudu" / "xanadocs")) {
      if (entry.path().filename().string().starts_with("untitled-")) {
        found.push_back(entry.path());
      }
    }
    return found;
  };
  const auto dumpOf = [&](const fs::path &store) {
    fs::path permascroll;
    for (const auto &entry :
         fs::directory_iterator(root / "data" / "xudu" / "permascroll")) {
      permascroll = entry.path();
    }
    return run(built("xudu-dump").string() + " --section=ops --permascroll=" +
               permascroll.string() + " " + store.string())
        .output;
  };

  const auto typed = xuzz("--chord Ctrl+N --type 'hello world' --chord Left"
                          " --chord Left --chord Left --chord Shift+Left"
                          " --chord Shift+Left");
  ASSERT_EQ(typed.exitCode, 0) << typed.output;
  const auto resumed = xuzz("--type Z");
  ASSERT_EQ(resumed.exitCode, 0) << resumed.output;
  ASSERT_EQ(untitledStores().size(), 1U);
  const auto text = dumpOf(untitledStores().front());
  EXPECT_THAT(text, ::testing::ContainsRegex("kind=delete [^\n]* at=6 len=2"))
      << text;
  EXPECT_THAT(text, ::testing::ContainsRegex(
                        "kind=insert [^\n]* at=6 [^\n]*text=\"Z\""))
      << text;

  fs::remove_all(root / "data" / "xudu" / "xanadocs");
  const auto sliced = xuzz("--chord Ctrl+Alt+N --chord N");
  ASSERT_EQ(sliced.exitCode, 0) << sliced.output;
  const auto renamed = xuzz("--chord E --type renamed --chord Return");
  ASSERT_EQ(renamed.exitCode, 0) << renamed.output;
  ASSERT_EQ(untitledStores().size(), 1U);
  const auto slice = dumpOf(untitledStores().front());
  // On the store's own line: "produces=N" with no branch letter in N.
  EXPECT_THAT(slice, ::testing::ContainsRegex(
                         "produces=[0-9]+ .*setValue.*text=\"renamed\""))
      << slice;
}

} // namespace

TEST(PublicationUiTest, aSliceCanBeAddedToTheCurrentTextStoreAndReopened) {
  ASSERT_TRUE(fs::exists(built("xuzz")));
  const auto root = fs::current_path() / "build" / "publication_same_store";
  fs::remove_all(root);
  fs::create_directories(root);
  const auto store       = root / "story";
  const auto permascroll = root / "permascroll";
  const auto launch      = [&](const std::string &script) {
    return run("XDG_CONFIG_HOME=" + (root / "config").string() +
               " XDG_DATA_HOME=" + (root / "data").string() + " timeout 120 " +
               built("xuzz").string() + " " + store.string() +
               " --permascroll " + permascroll.string() + " --profile " +
               script);
  };
  const auto first = launch("--type 'Story Ideas' --chord Ctrl+Alt+Shift+N"
                            " --chord N --chord E --type alpha --chord Return"
                            " --dump-a11y --capture " +
                            (root / "slice.ppm").string());
  ASSERT_EQ(first.exitCode, 0) << first.output;
  EXPECT_THAT(first.output,
              ::testing::HasSubstr("started a new slice (store 0)"));
  EXPECT_THAT(first.output, ::testing::Not(::testing::HasSubstr(
                                "created new sovereign document")));
  const auto dump = [&] {
    return run(built("xudu-dump").string() + " --section=ops --permascroll=" +
               permascroll.string() + " " + store.string());
  };
  const auto before = dump();
  ASSERT_EQ(before.exitCode, 0) << before.output;
  EXPECT_THAT(before.output, ::testing::HasSubstr("Story Ideas"));
  EXPECT_THAT(before.output,
              ::testing::ContainsRegex("setValue.*text=\"alpha\""));
  const auto reopened =
      launch("--dump-a11y --capture " + (root / "reopened.ppm").string());
  ASSERT_EQ(reopened.exitCode, 0) << reopened.output;
  const auto after = dump();
  ASSERT_EQ(after.exitCode, 0) << after.output;
  EXPECT_THAT(after.output, ::testing::HasSubstr("Story Ideas"));
  EXPECT_THAT(after.output,
              ::testing::ContainsRegex("setValue.*text=\"alpha\""));
}

TEST(PublicationUiTest, telescopeSearchDoesNotWriteIntoTheDocument) {
  ASSERT_TRUE(fs::exists(built("xuzz")));
  const auto root = fs::current_path() / "build" / "publication_search";
  fs::remove_all(root);
  fs::create_directories(root);
  const auto store       = root / "notes";
  const auto permascroll = root / "permascroll";
  const auto session =
      run("XDG_CONFIG_HOME=" + (root / "config").string() + " XDG_DATA_HOME=" +
          (root / "data").string() + " timeout 120 " + built("xuzz").string() +
          " " + store.string() + " --permascroll " + permascroll.string() +
          " --profile --type 'Private notes.' --chord F3"
          " --type 'Ideas café' --chord Backspace --dump-a11y --capture " +
          (root / "search.ppm").string() +
          " --chord Escape --dump-a11y --capture " +
          (root / "document.ppm").string());
  ASSERT_EQ(session.exitCode, 0) << session.output;
  EXPECT_THAT(session.output,
              ::testing::HasSubstr("Search publications\" = \"Ideas caf\""));
  EXPECT_THAT(session.output,
              ::testing::HasSubstr("focus: Search publications"));
  const auto dump = run(built("xudu-dump").string() +
                        " --section=ops --permascroll=" + permascroll.string() +
                        " " + store.string());
  ASSERT_EQ(dump.exitCode, 0) << dump.output;
  EXPECT_THAT(dump.output, ::testing::HasSubstr("Private notes."));
  EXPECT_THAT(dump.output, ::testing::Not(::testing::HasSubstr("Ideas")));
  EXPECT_THAT(dump.output, ::testing::Not(::testing::HasSubstr("caf")));
}

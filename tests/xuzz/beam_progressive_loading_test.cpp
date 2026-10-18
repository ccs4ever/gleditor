/**
 * @file beam_progressive_loading_test.cpp
 * @brief A beam between two small, fast-loading documents no longer waits on
 * an unrelated large document that is still building its pages -- see
 * design/priority-page-building.md's Stage 0.
 *
 * Before that change, LinkBeams::drawFrame() returned early -- and never
 * called resolveAnchors() or drew anything -- unless *every* open document
 * reported isFullyLoaded(), regardless of whether a given beam's own two
 * ends were ready. This is the regression test for the scene that change
 * targets: it does not (and, without new render-thread instrumentation
 * beyond what --profile already reports, cannot) assert that the beam
 * appears *before* settling -- that was confirmed by hand for the change
 * itself, and design/priority-page-building.md's Stage 4 is where recording
 * it permanently is planned -- but it does assert the whole scene, camera
 * alignment included, still settles correctly and promptly rather than
 * hanging or mis-timing out, which is exactly the risk a per-strand
 * settling condition (replacing a per-document one) introduces if it ever
 * gets a strand's resolvability wrong.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace {

namespace fs = std::filesystem;
using xanadu::Link;
using xanadu::LinkType;
using xanadu::MicroversionId;
using xanadu::Store;

std::shared_ptr<xanadu::UserPermascroll> permascrollAt(const fs::path &dir) {
  xanadu::UserPermascroll::Config config;
  config.storageDir = dir;
  return std::make_shared<xanadu::UserPermascroll>(std::move(config));
}

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

class BeamProgressiveLoadingTest : public testing::Test {
protected:
  fs::path testRoot;

  void SetUp() override {
    testRoot = fs::current_path() / "build" / "test_beam_progressive_loading";
    fs::remove_all(testRoot);
    fs::create_directories(testRoot);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(testRoot, ec);
  }
};

TEST_F(BeamProgressiveLoadingTest,
       ABeamBetweenFastDocumentsStillSettlesAlongsideASlowUnrelatedOne) {
  const auto xuduBin = findXuduBinary();
  ASSERT_TRUE(fs::exists(xuduBin)) << "xudu binary not found at " << xuduBin;

  const auto permascroll = testRoot / "permascroll";
  Store store(permascrollAt(permascroll));

  // Two small documents with a link between them: "alpha" in the first
  // links to "gamma", quoted whole into a second, tiny document.
  const auto typed =
      store.insert(MicroversionId{}, 0, "alpha beta gamma delta");
  const auto text = store.rebuild(typed);
  Link link;
  link.type         = LinkType::Comment;
  link.owner        = "someone";
  link.left         = text.spansFor(0, 5);  // "alpha"
  link.right        = text.spansFor(11, 5); // "gamma"
  const auto linked = store.addLink(typed, link);
  const auto quoted = store.transclude(MicroversionId{}, 0, linked, 11, 5);

  // A large, unrelated third document -- slow to paginate, so it is still
  // building its pages well after linked/quoted have finished.
  std::string big;
  const std::string para =
      "The quick brown fox jumps over the lazy dog. Pack my box with five "
      "dozen liquor jugs. How vexingly quick daft zebras jump!\n\n";
  while (big.size() < 3U * 1024U * 1024U) {
    big += para;
  }
  const auto slow = store.insert(MicroversionId{}, 0, big);

  const auto storePath = testRoot / "store";
  store.save(storePath.string());

  const std::string cmd = xuduBin.string() + " --permascroll " +
                          permascroll.string() +
                          " --backend opengl --version-id " + linked.str() +
                          " --alongside " + quoted.str() + " --background " +
                          slow.str() + " --profile " + storePath.string();

  const auto start = std::chrono::steady_clock::now();
  const auto res   = executeProcess(cmd);
  const auto elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();

  EXPECT_EQ(res.exitCode, 0) << "run failed: " << res.output;
  EXPECT_NE(res.output.find("Complete render settled"), std::string::npos)
      << "run never settled: " << res.output;
  EXPECT_NE(res.output.find("docs: 3"), std::string::npos)
      << "not all three documents were open at settle time: " << res.output;
  // Generous: the slow document alone takes a couple of seconds to build on
  // this scale of text. The property worth guarding is that nothing hangs
  // waiting on a strand that will never resolve, not a tight timing bound.
  EXPECT_LT(elapsed, 60.0)
      << "run took far longer than building three documents should, "
         "possibly hung waiting on a strand that never resolved";
}

} // namespace

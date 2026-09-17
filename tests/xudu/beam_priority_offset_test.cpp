/**
 * @file beam_priority_offset_test.cpp
 * @brief A beam whose far end lands deep inside a large document no longer
 * has to wait for that document to build every page in front of it at the
 * ordinary rate -- see design/priority-page-building.md's Stage 1, which
 * wires LinkBeams::updatePriorityOffsets() to push the far end's byte offset
 * through Doc::setPriorityOffsets() once the ribbon between the two
 * documents' anchors could plausibly cross the viewport.
 *
 * This does not assert a specific speedup: wall-clock savings depend on
 * machine speed, and Stage 1 deliberately does not reorder page building (see
 * the design doc's own "without reordering anything" subtitle), so the
 * benefit is a wider per-call time budget rather than the target page
 * appearing out of turn. What is worth guarding permanently is that wiring
 * the priority channel through a real beam -- rather than through
 * Doc::setPriorityOffsets() directly, as tests/lib/doc_page_budget_test.cpp
 * already does -- settles correctly and without hanging, the same property
 * beam_progressive_loading_test.cpp guards for Stage 0.
 *
 * --no-sworph is deliberate and load-bearing here, not incidental: sworphing
 * would bring the large document alongside the moment its far end resolves,
 * pulling it into camera auto-framing that (independently of this feature)
 * does not scale to a document with well over a thousand pages. That is an
 * existing, orthogonal cost this test has no business exercising -- see the
 * commit introducing this file for how it was found and ruled out as
 * unrelated to Stage 1's own wiring.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <xudu/core/microversion.hpp>
#include <xudu/core/ops.hpp>
#include <xudu/core/store.hpp>
#include <xudu/core/user_permascroll.hpp>

namespace {

namespace fs = std::filesystem;
using xudu::Link;
using xudu::LinkType;
using xudu::MicroversionId;
using xudu::Store;

std::shared_ptr<xudu::UserPermascroll> permascrollAt(const fs::path &dir) {
  xudu::UserPermascroll::Config config;
  config.storageDir = dir;
  return std::make_shared<xudu::UserPermascroll>(std::move(config));
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

class BeamPriorityOffsetTest : public testing::Test {
protected:
  fs::path testRoot;

  void SetUp() override {
    testRoot = fs::current_path() / "build" / "test_beam_priority_offset";
    fs::remove_all(testRoot);
    fs::create_directories(testRoot);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(testRoot, ec);
  }
};

TEST_F(BeamPriorityOffsetTest,
       ABeamIntoALateOffsetOfALargeDocumentStillSettlesPromptly) {
  const auto xuduBin = findXuduBinary();
  ASSERT_TRUE(fs::exists(xuduBin)) << "xudu binary not found at " << xuduBin;

  const auto permascroll = testRoot / "permascroll";
  Store store(permascrollAt(permascroll));

  const auto small = store.insert(MicroversionId{}, 0, "alpha beta gamma");

  // Large enough to need well over a thousand pages -- small enough that the
  // whole run, prioritised or not, finishes in seconds rather than minutes.
  std::string big;
  const std::string para =
      "The quick brown fox jumps over the lazy dog. Pack my box with five "
      "dozen liquor jugs. How vexingly quick daft zebras jump!\n\n";
  while (big.size() < 3U * 1024U * 1024U) {
    big += para;
  }
  const auto bigVer = store.insert(MicroversionId{}, 0, big);

  // The link's far end lands five bytes from the very end of the large
  // document -- as far as an offset can be from where plain in-order building
  // starts, which is exactly the case a priority push exists to help.
  Link link;
  link.type  = LinkType::Comment;
  link.owner = "someone";
  link.left  = store.rebuild(small).spansFor(0, 5); // "alpha"
  const auto targetOffset = static_cast<std::uint32_t>(big.size() - 6);
  link.right              = store.rebuild(bigVer).spansFor(targetOffset, 5);
  const auto linked       = store.addLink(small, link);

  const auto storePath = testRoot / "store";
  store.save(storePath.string());

  const std::string cmd = xuduBin.string() + " --permascroll " +
                          permascroll.string() +
                          " --backend opengl --version-id " + linked.str() +
                          " --background " + bigVer.str() +
                          " --no-sworph --profile " + storePath.string();

  const auto start = std::chrono::steady_clock::now();
  const auto res   = executeProcess(cmd);
  const auto elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();

  EXPECT_EQ(res.exitCode, 0) << "run failed: " << res.output;
  EXPECT_NE(res.output.find("Complete render settled"), std::string::npos)
      << "run never settled: " << res.output;
  EXPECT_NE(res.output.find("docs: 2"), std::string::npos)
      << "not both documents were open at settle time: " << res.output;
  // Generous: building well over a thousand pages of software-rendered text
  // takes real time on its own. The property worth guarding is that a beam
  // reaching deep into a large document does not hang or regress into the
  // tens-of-seconds territory design/kjv-load-blocking-regression.md
  // describes, not a tight timing bound.
  EXPECT_LT(elapsed, 60.0)
      << "run took far longer than building one large document should, "
         "possibly hung waiting on the prioritised strand";
}

} // namespace

/**
 * @file animation_transclusion_test.cpp
 * @brief Integration tests verifying animated GIF and animated SVG
 *        transclusion and sub-range playback within XanaDocs.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <xudu/core/microversion.hpp>
#include <xudu/core/store.hpp>

namespace {

namespace fs = std::filesystem;

using xudu::MicroversionId;
using xudu::Store;

struct ExecutionResult {
  int exitCode{-1};
  std::string output;
};

ExecutionResult executeProcess(const std::string &cmd) {
  std::string fullCmd = cmd + " 2>&1";
  FILE *pipe          = popen(fullCmd.c_str(), "r");
  if (!pipe) {
    return {-1, "Failed to popen: " + cmd};
  }
  char buffer[512];
  std::string output;
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    output += buffer;
  }
  const int status = pclose(pipe);
  const int code   = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return {code, output};
}

struct PpmImageInfo {
  bool valid{false};
  int width{0};
  int height{0};
  std::size_t distinctColors{0};
  std::string errorMessage;
};

PpmImageInfo inspectPpm(const fs::path &path) {
  PpmImageInfo info;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    info.errorMessage = "Failed to open PPM: " + path.string();
    return info;
  }
  std::string magic;
  in >> magic;
  if (magic != "P6") {
    info.errorMessage = "Not P6 format: " + magic;
    return info;
  }
  in >> info.width >> info.height;
  int maxVal = 0;
  in >> maxVal;
  char ws = 0;
  in.read(&ws, 1);

  std::vector<char> rawPixels(
      static_cast<std::size_t>(info.width * info.height * 3));
  in.read(rawPixels.data(), rawPixels.size());
  if (in.gcount() != static_cast<std::streamsize>(rawPixels.size())) {
    info.errorMessage = "Truncated pixel stream in PPM";
    return info;
  }

  std::set<std::tuple<uint8_t, uint8_t, uint8_t>> unique;
  for (std::size_t i = 0; i + 2 < rawPixels.size(); i += 3) {
    unique.emplace(static_cast<uint8_t>(rawPixels[i]),
                   static_cast<uint8_t>(rawPixels[i + 1]),
                   static_cast<uint8_t>(rawPixels[i + 2]));
  }
  info.distinctColors = unique.size();
  info.valid          = true;
  return info;
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

std::string activeBackend() {
  const char *env = std::getenv("XUDU_BACKEND");
  return (env && *env) ? std::string(env) : "gl";
}

std::string readWhole(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

class AnimationTransclusionTest : public testing::Test {
protected:
  fs::path testRoot;
  fs::path screenshotDir;

  void SetUp() override {
    testRoot      = fs::current_path() / "build" / "test_anim_transclusion";
    screenshotDir = testRoot / "screenshots";
    fs::remove_all(testRoot);
    fs::create_directories(screenshotDir);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(testRoot, ec);
  }
};

TEST_F(AnimationTransclusionTest, TranscludeAnimatedGifRendersAsMediaCard) {
  const auto xuduBin = findXuduBinary();
  ASSERT_TRUE(fs::exists(xuduBin)) << "xudu binary not found at " << xuduBin;

  const auto gifBytes = readWhole("tests/samples/sample_animated.gif");
  ASSERT_GT(gifBytes.size(), 50U) << "missing animated GIF sample";

  Store store;
  const auto animVersion =
      store.insertMedia(MicroversionId{}, 0, gifBytes, "image/gif").version;

  const std::string before = "Header text introducing animated GIF:\n\n";
  const std::string after  = "\n\nTrailing paragraph below the animation.\n";

  auto textVer     = store.insert(MicroversionId{}, 0, before);
  std::uint32_t at = static_cast<std::uint32_t>(before.size());
  textVer = store.transclude(textVer, at, animVersion, 0,
                             static_cast<std::uint32_t>(gifBytes.size()));
  at += static_cast<std::uint32_t>(gifBytes.size());
  textVer = store.insert(textVer, at, after);

  const auto storePath = testRoot / "store_gif";
  store.save(storePath.string());

  const auto ppmPath    = screenshotDir / "animated_gif.ppm";
  const std::string cmd = xuduBin.string() + " --backend " + activeBackend() +
                          " --profile --strict-diagnostics --version-id " +
                          textVer.str() + " --screenshot " + ppmPath.string() +
                          " " + storePath.string();
  const auto res        = executeProcess(cmd);
  EXPECT_EQ(res.exitCode, 0)
      << "rendering transcluded animated GIF failed: " << res.output;
  EXPECT_TRUE(fs::exists(ppmPath))
      << "no screenshot produced for animated GIF document";

  const auto info = inspectPpm(ppmPath);
  EXPECT_TRUE(info.valid) << "PPM screenshot invalid: " << info.errorMessage;
  EXPECT_GE(info.distinctColors, 4U);
}

TEST_F(AnimationTransclusionTest, TranscludeAnimatedSvgRendersAsMediaCard) {
  const auto xuduBin = findXuduBinary();
  ASSERT_TRUE(fs::exists(xuduBin)) << "xudu binary not found at " << xuduBin;

  const auto svgBytes = readWhole("tests/samples/sample_animated.svg");
  ASSERT_GT(svgBytes.size(), 20U) << "missing animated SVG sample";

  Store store;
  const auto animVersion =
      store.insertMedia(MicroversionId{}, 0, svgBytes, "image/svg+xml").version;

  const std::string before = "Heading for animated SVG transclusion:\n\n";
  const std::string after  = "\n\nConclusion text after SVG animation.\n";

  auto textVer     = store.insert(MicroversionId{}, 0, before);
  std::uint32_t at = static_cast<std::uint32_t>(before.size());
  textVer = store.transclude(textVer, at, animVersion, 0,
                             static_cast<std::uint32_t>(svgBytes.size()));
  at += static_cast<std::uint32_t>(svgBytes.size());
  textVer = store.insert(textVer, at, after);

  const auto storePath = testRoot / "store_svg";
  store.save(storePath.string());

  const auto ppmPath    = screenshotDir / "animated_svg.ppm";
  const std::string cmd = xuduBin.string() + " --backend " + activeBackend() +
                          " --profile --strict-diagnostics --version-id " +
                          textVer.str() + " --screenshot " + ppmPath.string() +
                          " " + storePath.string();
  const auto res        = executeProcess(cmd);
  EXPECT_EQ(res.exitCode, 0)
      << "rendering transcluded animated SVG failed: " << res.output;
  EXPECT_TRUE(fs::exists(ppmPath))
      << "no screenshot produced for animated SVG document";

  const auto info = inspectPpm(ppmPath);
  EXPECT_TRUE(info.valid) << "PPM screenshot invalid: " << info.errorMessage;
  EXPECT_GE(info.distinctColors, 4U);
}

TEST_F(AnimationTransclusionTest, TranscludePartialFragmentOfAnimation) {
  const auto xuduBin = findXuduBinary();
  ASSERT_TRUE(fs::exists(xuduBin)) << "xudu binary not found at " << xuduBin;

  const auto gifBytes = readWhole("tests/samples/sample_animated.gif");
  ASSERT_GT(gifBytes.size(), 50U);

  Store store;
  const auto animVersion =
      store.insertMedia(MicroversionId{}, 0, gifBytes, "image/gif").version;

  const std::string before = "Quoting only a portion of the animation:\n\n";
  const std::string after  = "\n\nEnd of fragment document.\n";

  auto textVer     = store.insert(MicroversionId{}, 0, before);
  std::uint32_t at = static_cast<std::uint32_t>(before.size());
  // Transclude half the bytes (temporal sub-range fragment)
  const auto halfLen = static_cast<std::uint32_t>(gifBytes.size() / 2);
  textVer            = store.transclude(textVer, at, animVersion, 10, halfLen);
  at += halfLen;
  textVer = store.insert(textVer, at, after);

  const auto storePath = testRoot / "store_fragment";
  store.save(storePath.string());

  const auto ppmPath    = screenshotDir / "animated_fragment.ppm";
  const std::string cmd = xuduBin.string() + " --backend " + activeBackend() +
                          " --profile --strict-diagnostics --version-id " +
                          textVer.str() + " --screenshot " + ppmPath.string() +
                          " " + storePath.string();
  const auto res        = executeProcess(cmd);
  EXPECT_EQ(res.exitCode, 0)
      << "rendering partial fragment of animation failed: " << res.output;
  EXPECT_TRUE(fs::exists(ppmPath));

  const auto info = inspectPpm(ppmPath);
  EXPECT_TRUE(info.valid);
}

} // namespace

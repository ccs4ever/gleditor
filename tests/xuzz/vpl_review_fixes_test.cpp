/**
 * @file vpl_review_fixes_test.cpp
 * @brief Regressions from the adversarial review of VPL cards V9 to V12, read
 * through the vpl runner so what is checked is what a user sees printed.
 */
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>

namespace {

std::string runVpl(const std::string &expression) {
  const auto root = std::filesystem::current_path().string();
  const std::string command =
      "SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 "
      "XDG_DATA_HOME=" +
      root + "/build/xdg/data XDG_CONFIG_HOME=" + root +
      "/build/xdg/config XDG_CACHE_HOME=" + root + "/build/xdg/cache " + root +
      "/build/vpl --headless -e '" + expression + "' 2>&1";
  FILE *pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) return {};
  std::string out;
  char buffer[256];
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) out += buffer;
  pclose(pipe);
  return out;
}

bool mentions(const std::string &output, const std::string &text) {
  return output.find(text) != std::string::npos;
}

// A raw cell handle printed as a value is the failure these tests exist for.
bool leaksCellHandle(const std::string &output) {
  return mentions(output, "c#");
}

TEST(VplReviewEncloseTest, EncloseKeepsAReversedOperandReversed) {
  EXPECT_EQ(runVpl("⌽1 2 3"), "3 2 1\n");
  EXPECT_EQ(runVpl("A←1 2 3\nB←⌽A\nC←⊂B\n⊃C"), "3 2 1\n");
  EXPECT_EQ(runVpl("⊃⊂⌽1 2 3"), "3 2 1\n");
  EXPECT_EQ(runVpl("⊂⌽1 2 3"), "< 3 2 1 >\n");
}

TEST(VplReviewEncloseTest, EnclosePlainValuesRoundTrip) {
  EXPECT_EQ(runVpl("⊃⊂1 2 3"), "1 2 3\n");
  EXPECT_EQ(runVpl("⊂1 2 3"), "< 1 2 3 >\n");
  EXPECT_EQ(runVpl("⊃ 1 2 3"), "1 2 3\n");
}

TEST(VplReviewEncloseTest, NestedEncloseRefusesByNameAndNeverLeaksACell) {
  for (const char *source : {"⊂⊂1 2", "⊃⊂⊂1 2 3", "⊃⊃⊂⊂1 2 3", "A←⊂1 2\n⊂A"}) {
    const auto out = runVpl(source);
    EXPECT_FALSE(leaksCellHandle(out)) << source << ": " << out;
    EXPECT_TRUE(mentions(out, "VPL nested enclose is unsupported by the VM"))
        << source << ": " << out;
  }
}

TEST(VplReviewEncloseTest, ShapeOfAnEnclosureIsRefusedByName) {
  const auto out = runVpl("⍴ ⊂1 2 3");
  EXPECT_TRUE(mentions(out, "monadic shape of an enclosed value")) << out;
}

TEST(VplReviewEncloseTest, DiscloseOfANonEnclosedValueIsIdentity) {
  EXPECT_EQ(runVpl("⊃ 4 5"), "4 5\n");
  EXPECT_EQ(runVpl("⊃ 7"), "7\n");
}

} // namespace

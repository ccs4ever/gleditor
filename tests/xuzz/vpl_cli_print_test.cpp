/**
 * @file vpl_cli_print_test.cpp
 * @brief The vpl runner prints boolean cells as values, not as cell handles.
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

TEST(VplCliPrintTest, ComparisonsPrintAsOneAndZero) {
  EXPECT_EQ(runVpl("1 2 3 = 3"), "0 0 1\n");
  EXPECT_EQ(runVpl("1 2 3 ≥ 2"), "0 1 1\n");
}

} // namespace

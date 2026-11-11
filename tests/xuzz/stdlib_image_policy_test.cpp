#include "common/xanadu/vortex/stdlib_image.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <gleditor/paths.hpp>

namespace {

using namespace zigzag::vortex;

void assignEnv(const char *name, const std::optional<std::string> &value) {
#ifdef _WIN32
  _putenv_s(name, value ? value->c_str() : "");
#else
  if (value)
    setenv(name, value->c_str(), 1);
  else
    unsetenv(name);
#endif
}

class PolicyEnvironment {
public:
  PolicyEnvironment()
      : names_{"GLEDITOR_STDLIB_SOURCE", "GLEDITOR_STDLIB_DIR",
               "GLEDITOR_ASSET_DIR", "GLEDITOR_STDLIB_LOCAL",
               "GLEDITOR_STDLIB_LOCAL_DIR"} {
    for (const auto *name : names_) {
      const char *value = std::getenv(name);
      saved_.push_back(value ? std::optional<std::string>{value}
                             : std::nullopt);
      assignEnv(name, std::nullopt);
    }
    resetStdlibPolicyForTesting();
  }

  ~PolicyEnvironment() {
    resetStdlibPolicyForTesting();
    for (std::size_t i = 0; i < names_.size(); ++i)
      assignEnv(names_[i], saved_[i]);
  }

  void set(const char *name, std::optional<std::string> value) {
    assignEnv(name, value);
  }

private:
  std::array<const char *, 5> names_;
  std::vector<std::optional<std::string>> saved_;
};

TEST(StdlibImagePolicyTest, ModeSpellingsAreExact) {
  EXPECT_EQ(stdlibModeName(StdlibMode::Image), "image");
  EXPECT_EQ(stdlibModeName(StdlibMode::Cpp), "cpp");
  EXPECT_EQ(stdlibModeName(StdlibMode::ImageThenCpp), "imagethencpp");
  EXPECT_EQ(parseStdlibMode("image"), StdlibMode::Image);
  EXPECT_EQ(parseStdlibMode("cpp"), StdlibMode::Cpp);
  EXPECT_EQ(parseStdlibMode("imagethencpp"), StdlibMode::ImageThenCpp);
  EXPECT_EQ(parseStdlibMode("Image"), std::nullopt);
  EXPECT_EQ(parseStdlibMode("image-then-cpp"), std::nullopt);
}

TEST(StdlibImagePolicyTest, ExplicitEnvironmentAndSettingsPrecedence) {
  PolicyEnvironment env;
  env.set("GLEDITOR_STDLIB_SOURCE", "image");
  const auto beforeSettings = stdlibPolicySnapshot();
  EXPECT_EQ(applyStdlibSettingsSource("cpp"), beforeSettings);
  EXPECT_EQ(resolveStdlibPolicy().mode, StdlibMode::Image);

  setStdlibSource(StdlibMode::Cpp);
  EXPECT_EQ(resolveStdlibPolicy().mode, StdlibMode::Cpp);
  env.set("GLEDITOR_STDLIB_SOURCE", "bad-value");
  resetStdlibPolicyForTesting();
  ASSERT_TRUE(applyStdlibSettingsSource("image"));
  EXPECT_EQ(resolveStdlibPolicy().mode, StdlibMode::Image);
}

TEST(StdlibImagePolicyTest, InvalidEnvironmentFallsThroughToDefault) {
  PolicyEnvironment env;
  env.set("GLEDITOR_STDLIB_SOURCE", "IMAGE");
  EXPECT_EQ(resolveStdlibPolicy().mode, StdlibMode::ImageThenCpp);
  env.set("GLEDITOR_STDLIB_LOCAL", "yes");
  const auto policy = resolveStdlibPolicy();
  EXPECT_TRUE(policy.localImage);
  EXPECT_FALSE(policy.localExplicit);
}

TEST(StdlibImagePolicyTest, PolicyFieldWritersDoNotClobberEachOther) {
  PolicyEnvironment env;
  const std::filesystem::path expectedDir{"/tmp/stdlib-image-policy"};
  std::thread modeWriter([] {
    for (int i = 0; i < 100; ++i) setStdlibSource(StdlibMode::Image);
  });
  std::thread dirWriter([&] {
    for (int i = 0; i < 100; ++i) setStdlibImageDir(expectedDir);
  });
  std::thread localWriter([] {
    for (int i = 0; i < 100; ++i) setStdlibLocalImage(false);
  });
  modeWriter.join();
  dirWriter.join();
  localWriter.join();

  const auto snapshot = stdlibPolicySnapshot();
  ASSERT_TRUE(snapshot->mode);
  ASSERT_TRUE(snapshot->imageDir);
  ASSERT_TRUE(snapshot->localImage);
  EXPECT_EQ(*snapshot->mode, StdlibMode::Image);
  EXPECT_EQ(*snapshot->imageDir, expectedDir);
  EXPECT_FALSE(*snapshot->localImage);
  EXPECT_EQ(setStdlibLocalImage(false), snapshot);
}

TEST(StdlibImagePolicyTest, DirectoryAndLocalChainsAreHeadless) {
  PolicyEnvironment env;
  const auto defaults = resolveStdlibPolicy();
  EXPECT_EQ(defaults.imageDir.filename(), "vortex-stdlib");
  ASSERT_TRUE(defaults.localDir);
  EXPECT_EQ(*defaults.localDir,
            gleditor::paths::dataPath("xuzz", "vortex-stdlib-local/"));

  env.set("GLEDITOR_STDLIB_DIR", "/taken/as/leaf");
  env.set("GLEDITOR_ASSET_DIR", "/ignored/assets");
  EXPECT_EQ(resolveStdlibPolicy().imageDir,
            std::filesystem::path{"/taken/as/leaf"});
  env.set("GLEDITOR_STDLIB_DIR", std::nullopt);
  EXPECT_EQ(resolveStdlibPolicy().imageDir,
            std::filesystem::path{"/ignored/assets"} / "vortex-stdlib");

  setStdlibImageDir("/explicit/leaf");
  env.set("GLEDITOR_STDLIB_DIR", "/environment/leaf");
  EXPECT_EQ(resolveStdlibPolicy().imageDir,
            std::filesystem::path{"/explicit/leaf"});

  env.set("GLEDITOR_STDLIB_LOCAL", "0");
  EXPECT_FALSE(resolveStdlibPolicy().localDir);
  env.set("GLEDITOR_STDLIB_LOCAL", "1");
  env.set("GLEDITOR_STDLIB_LOCAL_DIR", "/local/leaf");
  const auto local = resolveStdlibPolicy();
  EXPECT_TRUE(local.localExplicit);
  EXPECT_EQ(local.localDir, std::optional<std::filesystem::path>{
                                std::filesystem::path{"/local/leaf"}});
}

} // namespace

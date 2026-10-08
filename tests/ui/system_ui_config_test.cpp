#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "common/ui/xanadoc/session.hpp"
#include "common/xanadu/system_docs.hpp"

namespace {
class TemporaryConfig {
public:
  std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("xuzz_ui_upgrade_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  TemporaryConfig() {
    if (const auto *value = std::getenv("XDG_CONFIG_HOME")) previous = value;
    std::filesystem::create_directories(root);
    setenv("XDG_CONFIG_HOME", root.c_str(), 1);
  }
  ~TemporaryConfig() {
    if (previous)
      setenv("XDG_CONFIG_HOME", previous->c_str(), 1);
    else
      unsetenv("XDG_CONFIG_HOME");
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

private:
  std::optional<std::string> previous;
};

TEST(SystemUiConfigTest,
     ExistingNativeStoreAddsTypographyWithoutChangingNotesOrValues) {
  TemporaryConfig config;
  // System xanadocs are written against the user's own permascroll, whatever
  // permascroll the session's documents use.
  const auto scroll = xanadu::PermascrollRegistry::instance().defaultUser();
  xanadu::Store existing(scroll);
  existing.setSystem(true);
  xanadu::initializeSystemStoreGenesis(existing, xanadu::SystemDocKind::UI);
  const std::string notes = "My custom UI notes must survive an upgrade.\n";
  existing.repointCurrentVersion(
      existing.insert(existing.primaryCurrentVersion(), 0, notes));
  const auto specs = xanadu::defaultSettingSpecs(xanadu::SystemDocKind::UI);
  for (const auto &spec : specs) {
    if (spec.name == xanadu::settings::kLinkPanelFont) {
      existing.repointCurrentVersion(xanadu::ensureSetting(
          existing, existing.primaryCurrentVersion(), spec));
      break;
    }
  }
  existing.repointCurrentVersion(xanadu::setSetting(
      existing, existing.primaryCurrentVersion(),
      xanadu::settings::kLinkPanelFont, std::string{"Serif 17"}));
  const auto directory = xanadu::systemDocDirectory(xanadu::SystemDocKind::UI);
  std::filesystem::create_directories(directory);
  existing.save(directory.string());
  const auto originalCount = existing.opCount();
  std::size_t upgradedCount{};
  {
    xanadu::Session session{"", scroll};
    auto &store = session.systemStore(xanadu::SystemDocKind::UI);
    EXPECT_EQ(store.textOf(store.primaryCurrentVersion()), notes);
    EXPECT_EQ(xanadu::UIConfig::fromStore(store).linkPanel.font, "Serif 17");
    const auto model = xanadu::SystemStoreModel::fromStore(store);
    EXPECT_TRUE(model.find(xanadu::settings::kUiScale).has_value());
    EXPECT_TRUE(model
                    .find(xanadu::settings::uiFontPointsKey(
                        gleditor::ui::FontRole::Caption))
                    .has_value());
    upgradedCount = store.opCount();
    EXPECT_GT(upgradedCount, originalCount);
    EXPECT_EQ(session.systemStore(xanadu::SystemDocKind::UI).opCount(),
              upgradedCount);
  }
  {
    xanadu::Session reopened{"", scroll};
    auto &store = reopened.systemStore(xanadu::SystemDocKind::UI);
    EXPECT_EQ(store.opCount(), upgradedCount);
    EXPECT_EQ(store.textOf(store.primaryCurrentVersion()), notes);
    EXPECT_EQ(xanadu::UIConfig::fromStore(store).linkPanel.font, "Serif 17");
  }
}
// A store holds no text: a system xanadoc written against another
// permascroll loads, but its names resolve to nothing. It is kept, moved aside
// whole, and a default takes its place; nothing is minted into either
// permascroll on its behalf.
TEST(SystemUiConfigTest, AnUnresolvableSystemStoreIsMovedAsideWhole) {
  TemporaryConfig config;
  const auto original = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Store existing(original);
  xanadu::initializeSystemStore(existing, xanadu::SystemDocKind::UI);
  const auto directory = xanadu::systemDocDirectory(xanadu::SystemDocKind::UI);
  std::filesystem::create_directories(directory);
  existing.save(directory.string());
  const auto count         = existing.opCount();
  const auto head          = existing.primaryCurrentVersion();
  const auto other         = std::make_shared<xanadu::UserPermascroll>();
  const auto originalBytes = original->bytes().size();
  {
    xanadu::Session session{"", other};
    const auto model = xanadu::SystemStoreModel::fromStore(
        session.systemStore(xanadu::SystemDocKind::UI));
    EXPECT_TRUE(model.isValid()) << model.validationError();
    EXPECT_TRUE(model.find(xanadu::settings::kUiScale).has_value());
    EXPECT_EQ(other->bytes().size(), 0U);
    EXPECT_EQ(original->bytes().size(), originalBytes);
  }
  auto aside = directory;
  aside += ".unreadable";
  ASSERT_TRUE(std::filesystem::exists(aside));
  xanadu::Store preserved(original);
  preserved.load(aside.string());
  EXPECT_EQ(preserved.opCount(), count);
  EXPECT_EQ(preserved.textOf(head), existing.textOf(head));
}

// The keymap lives in the user's config directory, so it must read the same
// on every launch, whatever permascroll the documents of that launch use. It
// was written with the first session's permascroll, and a later launch with
// another one found a keymap with no bindings in it.
TEST(SystemUiConfigTest, TheKeymapReadsTheSameWhicheverPermascrollIsInUse) {
  TemporaryConfig config;
  std::size_t first{};
  {
    xanadu::Session session{"", std::make_shared<xanadu::UserPermascroll>()};
    first = xanadu::KeymapConfig::fromStore(
                session.systemStore(xanadu::SystemDocKind::Keymap))
                .bindings.size();
  }
  ASSERT_GT(first, 0U);
  xanadu::Session later{"", std::make_shared<xanadu::UserPermascroll>()};
  EXPECT_EQ(xanadu::KeymapConfig::fromStore(
                later.systemStore(xanadu::SystemDocKind::Keymap))
                .bindings.size(),
            first);
}
} // namespace

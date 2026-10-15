#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "common/ui/slice/unified_transclusion_engine.hpp"
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
     NativeSliceCommitPublishesSettingsWithoutMovingHistory) {
  TemporaryConfig config;
  const auto scroll = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Session session{"", scroll};
  const auto kind  = xanadu::SystemDocKind::Layout;
  const auto index = session.systemStoreIndex(kind);
  auto &store      = session.store(index);
  const auto base  = store.primaryCurrentVersion();
  session.viewOpened(base, index);
  session.viewOpened(base, index);
  const auto model   = xanadu::SystemStoreModel::fromStore(store);
  const auto setting = model.find(xanadu::settings::kReadableTextPx);
  ASSERT_TRUE(setting);
  ASSERT_EQ(setting->value.valueCells.size(), 1U);
  const auto cell = setting->value.valueCells.front();
  zigzag::UnifiedTransclusionEngine slice(store, base);
  std::size_t notifications{};
  session.setSystemDocChangedCallback(
      [&](const xanadu::SystemDocKind changed, const xanadu::Store &current) {
        ++notifications;
        EXPECT_EQ(changed, kind);
        EXPECT_EQ(current.primaryCurrentVersion(), slice.head());
        EXPECT_EQ(xanadu::LayoutConfig::fromStore(current).readableTextPx, 24);
      });
  slice.setAuthoredCommitHandler([&](const xanadu::MicroversionId &parent,
                                     const xanadu::MicroversionId &head) {
    EXPECT_TRUE(session.advanceViewAfterCommit(0, parent, head));
  });

  slice.updateCellText(cell, "24.0");
  const auto head = slice.head();
  EXPECT_EQ(notifications, 1U);
  EXPECT_EQ(session.views()[0].version, head);
  EXPECT_EQ(session.views()[1].version, base);
  EXPECT_EQ(store.primaryCurrentVersion(), head);
  EXPECT_EQ(xanadu::LayoutConfig::fromStore(store).readableTextPx, 24);
  xanadu::Store reopened(scroll);
  reopened.load(xanadu::systemDocDirectory(kind).string());
  EXPECT_EQ(reopened.primaryCurrentVersion(), head);
  EXPECT_EQ(xanadu::LayoutConfig::fromStore(reopened).readableTextPx, 24);

  auto operations = store.opCount();
  EXPECT_TRUE(session.advanceViewAfterCommit(0, head, head));
  EXPECT_EQ(store.opCount(), operations);
  EXPECT_EQ(notifications, 1U);
  const auto sibling = store.setCellText(base, cell, "30.0");
  operations         = store.opCount();
  EXPECT_FALSE(session.advanceViewAfterCommit(0, base, sibling));
  EXPECT_EQ(store.opCount(), operations);
  EXPECT_EQ(store.primaryCurrentVersion(), head);
  EXPECT_EQ(session.views()[1].version, base);
  EXPECT_EQ(notifications, 1U);
}

TEST(SystemUiConfigTest,
     ExistingNativeStoreAddsTypographyWithoutChangingNotesOrValues) {
  TemporaryConfig config;
  const auto scroll = std::make_shared<xanadu::UserPermascroll>();
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
TEST(SystemUiConfigTest, UnresolvedPermascrollLeavesExistingStoreUntouched) {
  TemporaryConfig config;
  const auto original = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Store existing(original);
  xanadu::initializeSystemStore(existing, xanadu::SystemDocKind::UI);
  const auto directory = xanadu::systemDocDirectory(xanadu::SystemDocKind::UI);
  std::filesystem::create_directories(directory);
  existing.save(directory.string());
  const auto count = existing.opCount();
  const auto head  = existing.primaryCurrentVersion();
  const auto other = std::make_shared<xanadu::UserPermascroll>();
  {
    xanadu::Session session{"", other};
    auto &loaded = session.systemStore(xanadu::SystemDocKind::UI);
    EXPECT_EQ(loaded.opCount(), count);
    EXPECT_EQ(other->bytes().size(), 0U);
    EXPECT_EQ(xanadu::UIConfig::fromStore(loaded).uiTheme,
              xanadu::UIConfig{}.uiTheme);
  }
  xanadu::Store preserved(original);
  preserved.load(directory.string());
  EXPECT_EQ(preserved.opCount(), count);
  EXPECT_EQ(preserved.textOf(head), existing.textOf(head));
}
} // namespace

/**
 * @file test_editor_config.cpp
 * @brief Unit tests for TSV configuration parsing in apps/gleditor.
 */
#include <gtest/gtest.h>

#include "gleditor/editor_config.hpp"

namespace {

TEST(EditorConfigTest, ParseDefaultConfig) {
  const std::string tsv = gleditor::defaultEditorConfigTsv();
  const auto cfg        = gleditor::parseEditorConfig(tsv);

  EXPECT_FLOAT_EQ(cfg.settings.fontSize, 16.0F);
  EXPECT_EQ(cfg.settings.fontFamily, "Monospace");
  EXPECT_FLOAT_EQ(cfg.settings.lineHeight, 1.4F);
  EXPECT_EQ(cfg.settings.autoSaveSeconds, 5U);
  EXPECT_EQ(cfg.settings.theme, "system");

  EXPECT_FLOAT_EQ(cfg.spatial.documentSpacingX, 70.0F);
  EXPECT_FLOAT_EQ(cfg.spatial.depthZ, -45.0F);
  EXPECT_FLOAT_EQ(cfg.spatial.docArrivalSeconds, 0.22F);
  EXPECT_FLOAT_EQ(cfg.spatial.backgroundOpacity, 0.35F);

  EXPECT_FALSE(cfg.keymap.empty());
  bool foundNew = false;
  for (const auto &[name, chord] : cfg.keymap) {
    if (name == "new" && chord == "Ctrl+N") {
      foundNew = true;
    }
  }
  EXPECT_TRUE(foundNew);
  EXPECT_FALSE(cfg.userNotes.empty());
}

TEST(EditorConfigTest, ParseCustomConfigOverrides) {
  const std::string customTsv =
      "settings.fontSize\t20\n"
      "settings.fontFamily\tFira Code\n"
      "settings.lineHeight\t1.6\n"
      "settings.autoSaveSeconds\t15\n"
      "settings.theme\tdark\n"
      "spatial.documentSpacingX\t85.0\n"
      "spatial.depthZ\t-60.0\n"
      "spatial.docArrivalSeconds\t0.15\n"
      "spatial.backgroundOpacity\t0.50\n"
      "keymap.new\tCtrl+T\n"
      "keymap.save\tCtrl+S\n"
      "user_notes.notes\tMy personal editor configuration\n";

  const auto cfg = gleditor::parseEditorConfig(customTsv);

  EXPECT_FLOAT_EQ(cfg.settings.fontSize, 20.0F);
  EXPECT_EQ(cfg.settings.fontFamily, "Fira Code");
  EXPECT_FLOAT_EQ(cfg.settings.lineHeight, 1.6F);
  EXPECT_EQ(cfg.settings.autoSaveSeconds, 15U);
  EXPECT_EQ(cfg.settings.theme, "dark");

  EXPECT_FLOAT_EQ(cfg.spatial.documentSpacingX, 85.0F);
  EXPECT_FLOAT_EQ(cfg.spatial.depthZ, -60.0F);
  EXPECT_FLOAT_EQ(cfg.spatial.docArrivalSeconds, 0.15F);
  EXPECT_FLOAT_EQ(cfg.spatial.backgroundOpacity, 0.50F);

  EXPECT_EQ(cfg.keymap.size(), 2U);
  EXPECT_EQ(cfg.keymap[0].first, "new");
  EXPECT_EQ(cfg.keymap[0].second, "Ctrl+T");
  EXPECT_EQ(cfg.keymap[1].first, "save");
  EXPECT_EQ(cfg.keymap[1].second, "Ctrl+S");

  EXPECT_EQ(cfg.userNotes, "My personal editor configuration");
}

TEST(EditorConfigTest, InvalidTsvFallsBackGracefully) {
  const std::string invalidTsv = "this has no tab";
  const auto cfg               = gleditor::parseEditorConfig(invalidTsv);

  // Should retain default struct values
  EXPECT_FLOAT_EQ(cfg.settings.fontSize, 16.0F);
  EXPECT_EQ(cfg.settings.fontFamily, "Monospace");
  EXPECT_FLOAT_EQ(cfg.spatial.documentSpacingX, 70.0F);
  EXPECT_TRUE(cfg.keymap.empty());
}

TEST(EditorConfigTest, LoadAssetsConfig) {
  const auto cfg = gleditor::loadEditorConfig("assets/gleditor/config.tsv");

  EXPECT_FLOAT_EQ(cfg.settings.fontSize, 16.0F);
  EXPECT_EQ(cfg.settings.fontFamily, "Monospace");
  EXPECT_FLOAT_EQ(cfg.spatial.documentSpacingX, 70.0F);
  EXPECT_FALSE(cfg.keymap.empty());
  EXPECT_FALSE(cfg.userNotes.empty());
}

} // namespace

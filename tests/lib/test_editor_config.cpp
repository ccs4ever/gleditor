/**
 * @file test_editor_config.cpp
 * @brief Unit tests for YAML configuration parsing in apps/gleditor.
 */
#include <gtest/gtest.h>

#include "gleditor/editor_config.hpp"

namespace {

TEST(EditorConfigTest, ParseDefaultConfig) {
  const std::string yaml = gleditor::defaultEditorConfigYaml();
  const auto cfg         = gleditor::parseEditorConfig(yaml);

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
  const std::string customYaml = R"(
settings:
  fontSize: 20
  fontFamily: "Fira Code"
  lineHeight: 1.6
  autoSaveSeconds: 15
  theme: "dark"

spatial:
  documentSpacingX: 85.0
  depthZ: -60.0
  docArrivalSeconds: 0.15
  backgroundOpacity: 0.50

keymap:
  new: "Ctrl+T"
  save: "Ctrl+S"

user_notes:
  notes: "My personal editor configuration"
)";

  const auto cfg = gleditor::parseEditorConfig(customYaml);

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

TEST(EditorConfigTest, InvalidYamlFallsBackGracefully) {
  const std::string invalidYaml = ": this is not valid yaml :::";
  const auto cfg                = gleditor::parseEditorConfig(invalidYaml);

  // Should retain default struct values
  EXPECT_FLOAT_EQ(cfg.settings.fontSize, 16.0F);
  EXPECT_EQ(cfg.settings.fontFamily, "Monospace");
  EXPECT_FLOAT_EQ(cfg.spatial.documentSpacingX, 70.0F);
  EXPECT_TRUE(cfg.keymap.empty());
}

TEST(EditorConfigTest, LoadAssetsConfig) {
  const auto cfg = gleditor::loadEditorConfig("assets/gleditor/config.yaml");

  EXPECT_FLOAT_EQ(cfg.settings.fontSize, 16.0F);
  EXPECT_EQ(cfg.settings.fontFamily, "Monospace");
  EXPECT_FLOAT_EQ(cfg.spatial.documentSpacingX, 70.0F);
  EXPECT_FALSE(cfg.keymap.empty());
  EXPECT_FALSE(cfg.userNotes.empty());
}

} // namespace

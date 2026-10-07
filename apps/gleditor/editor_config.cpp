/**
 * @file editor_config.cpp
 * @brief Plain TSV configuration reader and schema for apps/gleditor.
 */
#include "editor_config.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "common/tsv.hpp"

namespace gleditor {

std::string defaultEditorConfigTsv() {
  return "settings.fontSize\t16\n"
         "settings.fontFamily\tMonospace\n"
         "settings.lineHeight\t1.4\n"
         "settings.autoSaveSeconds\t5\n"
         "settings.theme\tsystem\n"
         "ui.scale\t1\n"
         "ui.fontScale\t1\n"
         "ui.safeMarginShare\t0.05\n"
         "ui.minTouchPx\t44\n"
         "ui.minFontPx\t9\n"
         "spatial.documentSpacingX\t70.0\n"
         "spatial.depthZ\t-45.0\n"
         "spatial.docArrivalSeconds\t0.22\n"
         "spatial.backgroundOpacity\t0.35\n"
         "input.modalGlobalCommands\tquit\n"
         "keymap.new\tCtrl+N\n"
         "keymap.close\tCtrl+W\n"
         "keymap.save\tCtrl+S\n"
         "keymap.bold\tCtrl+B\n"
         "keymap.italic\tCtrl+I\n"
         "keymap.underline\tCtrl+U\n"
         "keymap.3d-overview\tF10\n"
         "keymap.next-doc\tCtrl+Tab\n"
         "keymap.prev-doc\tCtrl+Shift+Tab\n"
         "user_notes.notes\tUser notes and customization preferences for "
         "gleditor.\n";
}

EditorConfig parseEditorConfig(const std::string_view tsv) {
  EditorConfig config;
  const auto entries = common::tsv::read(tsv);
  if (!entries) {
    return config;
  }
  constexpr std::string_view keymapPrefix = "keymap.";
  const auto positive = [](const std::string &value, float fallback) {
    const auto parsed = common::tsv::parseFloat(value, fallback);
    return std::isfinite(parsed) && parsed > 0 ? parsed : fallback;
  };
  constexpr std::array<std::string_view, ui::kFontRoleCount> roles{
      "caption", "label", "body", "title", "mono"};
  for (const auto &[key, value] : *entries) {
    bool fontSetting = false;
    for (std::size_t i = 0; i < roles.size(); ++i) {
      const auto prefix = "ui.font." + std::string(roles[i]);
      if (key == prefix + ".family" && !value.empty()) {
        config.uiTheme.fonts[i].family = value;
        fontSetting                    = true;
      } else if (key == prefix + ".points") {
        config.uiTheme.fonts[i].points =
            positive(value, config.uiTheme.fonts[i].points);
        fontSetting = true;
      }
    }
    if (fontSetting) continue;
    if ("settings.fontSize" == key) {
      config.settings.fontSize =
          common::tsv::parseFloat(value, config.settings.fontSize);
    } else if ("settings.fontFamily" == key) {
      config.settings.fontFamily = value;
    } else if ("settings.lineHeight" == key) {
      config.settings.lineHeight =
          common::tsv::parseFloat(value, config.settings.lineHeight);
    } else if ("settings.autoSaveSeconds" == key) {
      config.settings.autoSaveSeconds =
          common::tsv::parseUint(value, config.settings.autoSaveSeconds);
    } else if ("settings.theme" == key) {
      config.settings.theme = value;
    } else if ("ui.scale" == key) {
      config.uiScale = positive(value, config.uiScale);
    } else if ("ui.fontScale" == key) {
      config.uiFontScale = positive(value, config.uiFontScale);
    } else if ("ui.safeMarginShare" == key) {
      const auto parsed =
          common::tsv::parseFloat(value, config.uiSafeMarginShare);
      if (std::isfinite(parsed) && parsed >= 0 && parsed <= .5F)
        config.uiSafeMarginShare = parsed;
    } else if ("ui.minTouchPx" == key) {
      config.uiTheme.type.minTouchPx =
          positive(value, config.uiTheme.type.minTouchPx);
    } else if ("ui.minFontPx" == key) {
      config.uiTheme.type.minFontPx =
          positive(value, config.uiTheme.type.minFontPx);
    } else if ("spatial.documentSpacingX" == key) {
      config.spatial.documentSpacingX =
          common::tsv::parseFloat(value, config.spatial.documentSpacingX);
    } else if ("spatial.depthZ" == key) {
      config.spatial.depthZ =
          common::tsv::parseFloat(value, config.spatial.depthZ);
    } else if ("spatial.docArrivalSeconds" == key) {
      config.spatial.docArrivalSeconds =
          common::tsv::parseFloat(value, config.spatial.docArrivalSeconds);
    } else if ("spatial.backgroundOpacity" == key) {
      config.spatial.backgroundOpacity =
          common::tsv::parseFloat(value, config.spatial.backgroundOpacity);
    } else if ("input.modalGlobalCommands" == key) {
      config.modalGlobalCommands.clear();
      std::istringstream commands(value);
      for (std::string command; commands >> command;) {
        config.modalGlobalCommands.push_back(std::move(command));
      }
    } else if (key.starts_with(keymapPrefix)) {
      config.keymap.emplace_back(key.substr(keymapPrefix.size()), value);
    } else if ("user_notes.notes" == key) {
      config.userNotes = value;
    }
  }
  return config;
}

EditorConfig loadEditorConfig(const std::string &path) {
  std::vector<std::filesystem::path> candidates;
  if (!path.empty()) {
    candidates.emplace_back(path);
  }
  if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
    candidates.emplace_back(std::filesystem::path(xdg) / "gleditor" /
                            "config.tsv");
  } else if (const char *home = std::getenv("HOME"); home && *home) {
    candidates.emplace_back(std::filesystem::path(home) / ".config" /
                            "gleditor" / "config.tsv");
  }
  candidates.emplace_back("assets/gleditor/config.tsv");
  candidates.emplace_back("assets/config.tsv");

  for (const auto &candidate : candidates) {
    if (std::filesystem::exists(candidate)) {
      std::ifstream in(candidate, std::ios::binary);
      if (in.is_open()) {
        std::stringstream text;
        text << in.rdbuf();
        return parseEditorConfig(text.str());
      }
    }
  }
  return parseEditorConfig(defaultEditorConfigTsv());
}

} // namespace gleditor

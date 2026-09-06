/**
 * @file editor_config.cpp
 * @brief Plain YAML configuration reader and schema for apps/gleditor.
 */
#include "editor_config.hpp"

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <ryml.hpp>
#include <ryml_std.hpp>

namespace gleditor {

std::string defaultEditorConfigYaml() {
  return "# Gleditor Application Configuration\n"
         "settings:\n"
         "  fontSize: 16\n"
         "  fontFamily: \"Monospace\"\n"
         "  lineHeight: 1.4\n"
         "  autoSaveSeconds: 5\n"
         "  theme: \"system\"\n\n"
         "spatial:\n"
         "  documentSpacingX: 70.0\n"
         "  depthZ: -45.0\n"
         "  docArrivalSeconds: 0.22\n"
         "  backgroundOpacity: 0.35\n\n"
         "keymap:\n"
         "  new: \"Ctrl+N\"\n"
         "  close: \"Ctrl+W\"\n"
         "  save: \"Ctrl+S\"\n"
         "  bold: \"Ctrl+B\"\n"
         "  italic: \"Ctrl+I\"\n"
         "  underline: \"Ctrl+U\"\n"
         "  3d-overview: \"F10\"\n"
         "  next-doc: \"Ctrl+Tab\"\n"
         "  prev-doc: \"Ctrl+Shift+Tab\"\n\n"
         "schema:\n"
         "  purpose: \"Configuration file for gleditor plain GPU text editor.\"\n"
         "  fields:\n"
         "    fontSize: \"Base font size in points.\"\n"
         "    fontFamily: \"Font family name resolved via fontconfig.\"\n"
         "    lineHeight: \"Line height multiplier.\"\n"
         "    autoSaveSeconds: \"Auto-save interval in seconds.\"\n"
         "    documentSpacingX: \"Horizontal spacing between document columns in 3D space.\"\n"
         "    depthZ: \"Depth offset in Z units per background document layer.\"\n"
         "    docArrivalSeconds: \"Arrival animation duration in seconds.\"\n"
         "    backgroundOpacity: \"Resting opacity for inactive background documents.\"\n\n"
         "user_notes:\n"
         "  notes: \"User notes and customization preferences for gleditor.\"\n";
}

namespace {

void rymlErrorHandler(const c4::csubstr msg,
                      const c4::yml::ErrorDataBasic &, void *) {
  throw std::runtime_error(std::string{msg.str, msg.len});
}

struct ScopedCallbacks {
  c4::yml::Callbacks prev;
  ScopedCallbacks() {
    prev = c4::yml::get_callbacks();
    c4::yml::Callbacks cb;
    cb.m_error_basic = rymlErrorHandler;
    c4::yml::set_callbacks(cb);
  }
  ~ScopedCallbacks() { c4::yml::set_callbacks(prev); }
};

std::string_view trimStr(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                        s.front() == '\r' || s.front() == '\n')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                        s.back() == '\r' || s.back() == '\n')) {
    s.remove_suffix(1);
  }
  return s;
}

std::string stripQuotes(std::string_view s) {
  s = trimStr(s);
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\''))) {
    s = s.substr(1, s.size() - 2);
  }
  return std::string{s};
}

float parseFloat(std::string_view s, const float fallback) {
  s = trimStr(s);
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\''))) {
    s = s.substr(1, s.size() - 2);
  }
  float val = fallback;
  const auto res = std::from_chars(s.data(), s.data() + s.size(), val);
  if (res.ec == std::errc{}) {
    return val;
  }
  return fallback;
}

std::uint32_t parseUint(std::string_view s, const std::uint32_t fallback) {
  s = trimStr(s);
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\''))) {
    s = s.substr(1, s.size() - 2);
  }
  std::uint32_t val = fallback;
  const auto res = std::from_chars(s.data(), s.data() + s.size(), val);
  if (res.ec == std::errc{}) {
    return val;
  }
  return fallback;
}

} // namespace

EditorConfig parseEditorConfig(const std::string_view yamlText) {
  EditorConfig cfg;
  if (yamlText.empty()) {
    return cfg;
  }

  const ScopedCallbacks scoped;
  try {
    const c4::yml::Tree tree = c4::yml::parse_in_arena(
        c4::csubstr{yamlText.data(), yamlText.size()});
    if (tree.empty()) {
      return cfg;
    }
    const auto root = tree.rootref();
    if (!root.is_map()) {
      return cfg;
    }

    if (root.has_child("settings") && root["settings"].is_map()) {
      const auto s = root["settings"];
      if (s.has_child("fontSize") && s["fontSize"].has_val()) {
        const auto v = s["fontSize"].val();
        cfg.settings.fontSize = parseFloat(std::string_view{v.data(), v.size()}, cfg.settings.fontSize);
      }
      if (s.has_child("fontFamily") && s["fontFamily"].has_val()) {
        const auto v = s["fontFamily"].val();
        cfg.settings.fontFamily = stripQuotes(std::string_view{v.data(), v.size()});
      }
      if (s.has_child("lineHeight") && s["lineHeight"].has_val()) {
        const auto v = s["lineHeight"].val();
        cfg.settings.lineHeight = parseFloat(std::string_view{v.data(), v.size()}, cfg.settings.lineHeight);
      }
      if (s.has_child("theme") && s["theme"].has_val()) {
        const auto v = s["theme"].val();
        cfg.settings.theme = stripQuotes(std::string_view{v.data(), v.size()});
      }
      if (s.has_child("autoSaveSeconds") && s["autoSaveSeconds"].has_val()) {
        const auto v = s["autoSaveSeconds"].val();
        cfg.settings.autoSaveSeconds = parseUint(std::string_view{v.data(), v.size()}, cfg.settings.autoSaveSeconds);
      }
    }

    if (root.has_child("spatial") && root["spatial"].is_map()) {
      const auto sp = root["spatial"];
      if (sp.has_child("documentSpacingX") && sp["documentSpacingX"].has_val()) {
        const auto v = sp["documentSpacingX"].val();
        cfg.spatial.documentSpacingX = parseFloat(std::string_view{v.data(), v.size()}, cfg.spatial.documentSpacingX);
      }
      if (sp.has_child("depthZ") && sp["depthZ"].has_val()) {
        const auto v = sp["depthZ"].val();
        cfg.spatial.depthZ = parseFloat(std::string_view{v.data(), v.size()}, cfg.spatial.depthZ);
      }
      if (sp.has_child("docArrivalSeconds") && sp["docArrivalSeconds"].has_val()) {
        const auto v = sp["docArrivalSeconds"].val();
        cfg.spatial.docArrivalSeconds = parseFloat(std::string_view{v.data(), v.size()}, cfg.spatial.docArrivalSeconds);
      }
      if (sp.has_child("backgroundOpacity") && sp["backgroundOpacity"].has_val()) {
        const auto v = sp["backgroundOpacity"].val();
        cfg.spatial.backgroundOpacity = parseFloat(std::string_view{v.data(), v.size()}, cfg.spatial.backgroundOpacity);
      }
    }

    if (root.has_child("keymap") && root["keymap"].is_map()) {
      for (const auto child : root["keymap"].children()) {
        if (child.has_key() && child.has_val()) {
          std::string k{child.key().data(), child.key().size()};
          std::string v = stripQuotes(std::string_view{child.val().data(), child.val().size()});
          cfg.keymap.emplace_back(std::move(k), std::move(v));
        }
      }
    }

    if (root.has_child("user_notes") && root["user_notes"].is_map()) {
      const auto un = root["user_notes"];
      if (un.has_child("notes") && un["notes"].has_val()) {
        const auto v = un["notes"].val();
        cfg.userNotes = stripQuotes(std::string_view{v.data(), v.size()});
      }
    }
  } catch (const std::exception &) {
    // Return fallback cfg on error
  }

  return cfg;
}

EditorConfig loadEditorConfig(const std::string &path) {
  std::vector<std::filesystem::path> candidates;
  if (!path.empty()) {
    candidates.emplace_back(path);
  }

  if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
    candidates.emplace_back(std::filesystem::path(xdg) / "gleditor" / "config.yaml");
  } else if (const char *home = std::getenv("HOME"); home && *home) {
    candidates.emplace_back(std::filesystem::path(home) / ".config" / "gleditor" / "config.yaml");
  }

  candidates.emplace_back("assets/gleditor/config.yaml");
  candidates.emplace_back("assets/config.yaml");

  for (const auto &candidate : candidates) {
    if (std::filesystem::exists(candidate)) {
      std::ifstream in(candidate, std::ios::binary);
      if (in.is_open()) {
        std::stringstream ss;
        ss << in.rdbuf();
        return parseEditorConfig(ss.str());
      }
    }
  }

  return parseEditorConfig(defaultEditorConfigYaml());
}

} // namespace gleditor

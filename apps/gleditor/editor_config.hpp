/**
 * @file editor_config.hpp
 * @brief Plain YAML configuration reader and schema for apps/gleditor.
 */
#ifndef GLEDITOR_EDITOR_CONFIG_HPP
#define GLEDITOR_EDITOR_CONFIG_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gleditor {

struct SpatialConfig {
  float documentSpacingX{70.0F};
  float depthZ{-45.0F};
  float docArrivalSeconds{0.22F};
  float backgroundOpacity{0.35F};
};

struct EditorSettings {
  float fontSize{16.0F};
  std::string fontFamily{"Monospace"};
  float lineHeight{1.4F};
  std::string theme{"system"};
  std::uint32_t autoSaveSeconds{5};
};

struct EditorConfig {
  EditorSettings settings;
  SpatialConfig spatial;
  std::vector<std::pair<std::string, std::string>> keymap;
  std::string userNotes;
};

[[nodiscard]] EditorConfig parseEditorConfig(std::string_view yamlText);
[[nodiscard]] EditorConfig loadEditorConfig(const std::string &path = "");
[[nodiscard]] std::string defaultEditorConfigYaml();

} // namespace gleditor

#endif // GLEDITOR_EDITOR_CONFIG_HPP

/**
 * @file system_docs.hpp
 * @brief Sovereign system xanadocs managing runtime parameters.
 */
#ifndef XUDU_SYSTEM_DOCS_H
#define XUDU_SYSTEM_DOCS_H

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <gleditor/radial_menu.hpp>

namespace xudu {

/**
 * @brief The kinds of sovereign system xanadocs managing runtime parameters.
 */
enum class SystemDocKind : std::uint8_t {
  Keymap,
  Settings,
  Layout,
  UI,
  Pouches,
  Count
};

[[nodiscard]] constexpr std::string_view
systemDocName(const SystemDocKind kind) noexcept {
  switch (kind) {
  case SystemDocKind::Keymap:
    return "keymap";
  case SystemDocKind::Settings:
    return "settings";
  case SystemDocKind::Layout:
    return "layout";
  case SystemDocKind::UI:
    return "ui";
  case SystemDocKind::Pouches:
    return "pouches";
  case SystemDocKind::Count:
    return "unknown";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view
systemDocUri(const SystemDocKind kind) noexcept {
  switch (kind) {
  case SystemDocKind::Keymap:
    return "system://keymap";
  case SystemDocKind::Settings:
    return "system://settings";
  case SystemDocKind::Layout:
    return "system://layout";
  case SystemDocKind::UI:
    return "system://ui";
  case SystemDocKind::Pouches:
    return "system://pouches";
  case SystemDocKind::Count:
    return "system://unknown";
  }
  return "system://unknown";
}

[[nodiscard]] inline std::optional<SystemDocKind>
systemDocKindFromUri(const std::string_view uri) noexcept {
  if (uri == "system://keymap") {
    return SystemDocKind::Keymap;
  }
  if (uri == "system://settings") {
    return SystemDocKind::Settings;
  }
  if (uri == "system://layout") {
    return SystemDocKind::Layout;
  }
  if (uri == "system://ui") {
    return SystemDocKind::UI;
  }
  if (uri == "system://pouches") {
    return SystemDocKind::Pouches;
  }
  return std::nullopt;
}

[[nodiscard]] std::string defaultSystemDocContent(SystemDocKind kind);
[[nodiscard]] std::filesystem::path systemDocDirectory(SystemDocKind kind);

[[nodiscard]] gleditor::RadialConfig
parseRadialConfig(std::string_view yamlText);

} // namespace xudu

#endif // XUDU_SYSTEM_DOCS_H

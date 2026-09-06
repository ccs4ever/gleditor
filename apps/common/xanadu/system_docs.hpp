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

namespace zigzag {
struct ZzStructureDocument;
} // namespace zigzag

namespace xanadu {

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
[[nodiscard]] std::string defaultSystemDocSchema(SystemDocKind kind);
[[nodiscard]] std::string defaultSystemDocNotes(SystemDocKind kind);
[[nodiscard]] std::filesystem::path systemDocDirectory(SystemDocKind kind);

[[nodiscard]] inline std::string_view
extractConfigSection(const std::string_view docText) noexcept {
  const auto pos = docText.find("Schema and Purpose");
  if (pos != std::string_view::npos) {
    return docText.substr(0, pos);
  }
  return docText;
}

class Store;
void initializeSystemStore(Store &store, SystemDocKind kind);
void initializeSystemStoreFromSlice(Store &store, SystemDocKind kind,
                                    const zigzag::ZzStructureDocument &slice);

struct KeymapConfig {
  std::vector<std::pair<std::string, std::string>> bindings;
  [[nodiscard]] std::optional<std::string>
  bindingFor(const std::string_view action) const {
    for (const auto &[act, key] : bindings) {
      if (act == action) {
        return key;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] static KeymapConfig fromYaml(std::string_view yamlText);
  [[nodiscard]] static KeymapConfig
  fromSlice(const zigzag::ZzStructureDocument &slice);
};

struct SettingsConfig {
  float fontSize{16.0F};
  std::string fontFamily{"Monospace"};
  float lineHeight{1.4F};
  std::string theme{"system"};
  std::uint32_t autoSaveSeconds{5};

  [[nodiscard]] static SettingsConfig fromYaml(std::string_view yamlText);
  [[nodiscard]] static SettingsConfig
  fromSlice(const zigzag::ZzStructureDocument &slice);
};

enum class ToastAnchor : std::uint8_t {
  TopRight,
  TopLeft,
  BottomRight,
  BottomLeft,
  TopCenter
};

enum class PouchDock : std::uint8_t { Left, Right };

struct LayoutConfig {
  std::uint32_t columns{2};
  float pageWidthPx{800.0F};
  float pageHeightPx{1000.0F};
  ToastAnchor toastAnchor{ToastAnchor::TopRight};
  float toastOffsetX{24.0F};
  float toastOffsetY{48.0F};
  PouchDock pouchDock{PouchDock::Right};
  float documentSpacingX{70.0F};
  bool transclusionPrisms{true};
  bool xanalinkRibbons{true};

  [[nodiscard]] static LayoutConfig fromYaml(std::string_view yamlText);
  [[nodiscard]] static LayoutConfig
  fromSlice(const zigzag::ZzStructureDocument &slice);
};

struct UIConfig {
  bool tabBarVisible{true};
  bool statusBarVisible{true};
  bool hypertimeMapVisible{false};
  gleditor::RadialConfig radialMenu;

  [[nodiscard]] static UIConfig fromYaml(std::string_view yamlText);
  [[nodiscard]] static UIConfig
  fromSlice(const zigzag::ZzStructureDocument &slice);
};

[[nodiscard]] KeymapConfig parseKeymapConfig(std::string_view yamlText);
[[nodiscard]] SettingsConfig parseSettingsConfig(std::string_view yamlText);
[[nodiscard]] LayoutConfig parseLayoutConfig(std::string_view yamlText);
[[nodiscard]] UIConfig parseUIConfig(std::string_view yamlText);

[[nodiscard]] gleditor::RadialConfig
parseRadialConfig(std::string_view yamlText);

} // namespace xanadu

#endif // XUDU_SYSTEM_DOCS_H

/**
 * @file system_docs.cpp
 * @brief Sovereign system xanadocs managing runtime parameters.
 */
#include "xudu/core/system_docs.hpp"

#include <filesystem>
#include <string>

#include "xudu/core/config.hpp"

namespace xudu {

std::string defaultSystemDocContent(const SystemDocKind kind) {
  switch (kind) {
  case SystemDocKind::Keymap:
    return "# Xudu System Keymap\n"
           "# Key bindings and hotkeys\n"
           "new-doc: \"Ctrl+N\"\n"
           "open-doc: \"Ctrl+O\"\n"
           "close-doc: \"Ctrl+W\"\n"
           "forward: \"Ctrl+Shift+N\"\n"
           "scrub-forward: \"Ctrl+]\"\n"
           "scrub-backward: \"Ctrl+[\"\n"
           "hypertime-map: \"Ctrl+H\"\n";
  case SystemDocKind::Settings:
    return "# Xudu System Settings\n"
           "fontSize: \"16\"\n"
           "fontFamily: \"Monospace\"\n"
           "lineHeight: \"1.4\"\n"
           "autoSaveSeconds: \"5\"\n"
           "theme: \"system\"\n";
  case SystemDocKind::Layout:
    return "# Xudu System Layout\n"
           "columns: \"2\"\n"
           "pageWidthPx: \"800\"\n"
           "pageHeightPx: \"1000\"\n"
           "transclusionPrisms: \"true\"\n"
           "xanalinkRibbons: \"true\"\n";
  case SystemDocKind::UI:
    return "# Xudu System UI Configuration\n"
           "notificationPosition: \"top-right\"\n"
           "notificationDurationMs: \"3000\"\n"
           "tabBarVisible: \"true\"\n"
           "statusBarVisible: \"true\"\n"
           "hypertimeMapVisible: \"false\"\n";
  case SystemDocKind::Pouches:
    return "# Xudu System Pouches\n"
           "# Drop zones for ghost spanables\n"
           "zone:\n"
           "  - \"To Link\"\n"
           "  - \"Notes for Later\"\n"
           "  - \"Scratch\"\n";
  case SystemDocKind::Count:
    return "";
  }
  return "";
}

std::filesystem::path systemDocDirectory(const SystemDocKind kind) {
  return std::filesystem::path(configPath()).parent_path() / "system" /
         systemDocName(kind);
}

} // namespace xudu

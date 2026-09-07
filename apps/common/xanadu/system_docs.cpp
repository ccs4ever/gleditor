/**
 * @file system_docs.cpp
 * @brief Sovereign system xanadocs managing runtime parameters.
 */
#include "common/xanadu/system_docs.hpp"

#include <charconv>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <ryml.hpp>
#include <ryml_std.hpp>

#include "common/xanadu/config.hpp"
#include "common/xanadu/format.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/version.hpp"
#include "common/xanadu/zigzag/zz_system_projector.hpp"
#include "common/xanadu/zigzag/zzstructure.hpp"
#include "common/yaml_helpers.hpp"

namespace xanadu {

std::string defaultSystemDocContent(const SystemDocKind kind) {
  switch (kind) {
  case SystemDocKind::Keymap:
    return "new-doc: \"Ctrl+N\"\n"
           "open-doc: \"Ctrl+O\"\n"
           "close-doc: \"Ctrl+W\"\n"
           "forward: \"Ctrl+Shift+N\"\n"
           "scrub-forward: \"Ctrl+]\"\n"
           "scrub-backward: \"Ctrl+[\"\n"
           "hypertime-map: \"Ctrl+H\"\n"
           "radial-menu: \"Ctrl+M\"\n";
  case SystemDocKind::Settings:
    return "fontSize: \"16\"\n"
           "fontFamily: \"Monospace\"\n"
           "lineHeight: \"1.4\"\n"
           "autoSaveSeconds: \"5\"\n"
           "theme: \"system\"\n";
  case SystemDocKind::Layout:
    return "columns: \"2\"\n"
           "pageWidthPx: \"800\"\n"
           "pageHeightPx: \"1000\"\n"
           "transclusionPrisms: \"true\"\n"
           "xanalinkRibbons: \"true\"\n"
           "physics:\n"
           "  kRepel: \"4500.0\"\n"
           "  kPlane: \"14.0\"\n"
           "  kAlign: \"28.0\"\n"
           "  kTier: \"12.0\"\n"
           "  kDamping: \"7.5\"\n"
           "  backgroundDepthZ: \"-40.0\"\n"
           "  defaultGap: \"8.0\"\n"
           "  maxForce: \"10000.0\"\n"
           "  maxVelocity: \"1000.0\"\n"
           "  timeStep: \"0.016\"\n"
           "beams:\n"
           "  bandStrandLimit: \"7\"\n"
           "  bandStrandPitch: \"2.2\"\n"
           "  bandFillAlpha: \"0.85\"\n"
           "  stubWidthOfBeam: \"1.35\"\n"
           "  stubMinOfLine: \"0.9\"\n"
           "  marginKerf: \"0.04\"\n"
           "  bypassDepthPerDoc: \"-20.0\"\n"
           "  bypassDepthLimit: \"-120.0\"\n"
           "  bypassSegments: \"9\"\n";
  case SystemDocKind::UI:
    return "notificationPosition: \"top-right\"\n"
           "notificationDurationMs: \"3000\"\n"
           "tabBarVisible: \"true\"\n"
           "statusBarVisible: \"true\"\n"
           "hypertimeMapVisible: \"false\"\n"
           "radialMenu:\n"
           "  radius: 130.0\n"
           "  innerRadius: 42.0\n"
           "  actions:\n"
           "    - id: \"format:bold\"\n"
           "      label: \"Bold\"\n"
           "      icon: \"B\"\n"
           "    - id: \"format:italic\"\n"
           "      label: \"Italic\"\n"
           "      icon: \"I\"\n"
           "    - id: \"format:underline\"\n"
           "      label: \"Underline\"\n"
           "      icon: \"U\"\n"
           "    - id: \"format:superscript\"\n"
           "      label: \"Superscript\"\n"
           "      icon: \"X²\"\n"
           "    - id: \"format:subscript\"\n"
           "      label: \"Subscript\"\n"
           "      icon: \"X₂\"\n"
           "    - id: \"group:align\"\n"
           "      label: \"Align\"\n"
           "      icon: \"=\"\n"
           "      subActions:\n"
           "        - id: \"align:left\"\n"
           "          label: \"Left\"\n"
           "          icon: \"|<\"\n"
           "        - id: \"align:centre\"\n"
           "          label: \"Centre\"\n"
           "          icon: \"><\"\n"
           "        - id: \"align:right\"\n"
           "          label: \"Right\"\n"
           "          icon: \">|\"\n"
           "        - id: \"align:justify\"\n"
           "          label: \"Justify\"\n"
           "          icon: \"|=\"\n"
           "    - id: \"op:pagebreak\"\n"
           "      label: \"Page Break\"\n"
           "      icon: \"--\"\n"
           "    - id: \"op:transclude\"\n"
           "      label: \"Transclude\"\n"
           "      icon: \"[]\"\n"
           "    - id: \"info:author\"\n"
           "      label: \"Author\"\n"
           "      icon: \"@\"\n";
  case SystemDocKind::Pouches:
    return "zone:\n"
           "  - \"To Link\"\n"
           "  - \"Notes for Later\"\n"
           "  - \"Scratch\"\n";
  case SystemDocKind::Count:
    return "";
  }
  return "";
}

std::string defaultSystemDocSchema(const SystemDocKind kind) {
  switch (kind) {
  case SystemDocKind::Keymap:
    return "Schema and Purpose\n\n"
           "Purpose:\n"
           "The sovereign Keymap system xanadoc manages interactive keyboard "
           "shortcuts and hotkey bindings across Xudu sessions. Each entry "
           "maps an application action identifier to a key combination "
           "string.\n\n"
           "Schema:\n"
           "new-doc: Action shortcut to create a new sovereign document. "
           "Default is Ctrl+N.\n"
           "open-doc: Action shortcut to open an existing xanadoc from "
           "storage. Default is Ctrl+O.\n"
           "close-doc: Action shortcut to close the active document. Default "
           "is Ctrl+W.\n"
           "forward: Action shortcut to advance document head. Default is "
           "Ctrl+Shift+N.\n"
           "scrub-forward: Action shortcut to scrub forward along hypertime "
           "branch. Default is Ctrl+].\n"
           "scrub-backward: Action shortcut to scrub backward along hypertime "
           "branch. Default is Ctrl+[.\n"
           "hypertime-map: Action shortcut to toggle visual hypertime tree "
           "display. Default is Ctrl+H.\n"
           "radial-menu: Action shortcut to open context-sensitive radial "
           "menu. Default is Ctrl+M.\n";
  case SystemDocKind::Settings:
    return "Schema and Purpose\n\n"
           "Purpose:\n"
           "The sovereign Settings system xanadoc controls global text layout "
           "metrics, typography parameters, visual themes, and automatic "
           "persistence intervals.\n\n"
           "Schema:\n"
           "fontSize: Base font size in device-independent points (pt). "
           "Default is 16.\n"
           "fontFamily: Font family name resolved via Fontconfig. Default is "
           "Monospace.\n"
           "lineHeight: Proportional line spacing multiplier relative to font "
           "height. Default is 1.4.\n"
           "autoSaveSeconds: Inactivity interval in seconds before changes are "
           "committed. Default is 5.\n"
           "theme: Color scheme palette identifier (system, light, dark). "
           "Default is system.\n";
  case SystemDocKind::Layout:
    return "Schema and Purpose\n\n"
           "Purpose:\n"
           "The sovereign Layout system xanadoc defines multi-column page "
           "dimensions, notification toast anchors, pouch dock orientation, "
           "and optic ribbon rendering flags.\n\n"
           "Schema:\n"
           "columns: Number of parallel document columns displayed "
           "simultaneously. Default is 2.\n"
           "pageWidthPx: Logical page rendering width in pixels. Default is "
           "800.\n"
           "pageHeightPx: Logical page rendering height in pixels. Default is "
           "1000.\n"
           "toastAnchor: Notification anchor position (top-right, top-left, "
           "bottom-right, bottom-left, top-center). Default is top-right.\n"
           "toastOffsetX: Horizontal offset in pixels for toast notification "
           "window. Default is 24.\n"
           "toastOffsetY: Vertical offset in pixels for toast notification "
           "window. Default is 48.\n"
           "pouchDock: Edge alignment for span drop pouches (left or right). "
           "Default is right.\n"
           "documentSpacingX: Horizontal gap between parallel document columns "
           "in pixels. Default is 70.\n"
           "transclusionPrisms: Enable Identity Gold volumetric prisms for "
           "transcluded spans. Default is true.\n"
           "xanalinkRibbons: Enable cyan and magenta 3D optical link ribbons. "
           "Default is true.\n"
           "physics:\n"
           "  kRepel: Soft-body Coulomb repulsion constant between documents. "
           "Default is 4500.0.\n"
           "  kPlane: Spring constant pulling active documents to Z = 0 "
           "reading plane. Default is 14.0.\n"
           "  kAlign: Collinear link alignment spring constant. Default is "
           "28.0.\n"
           "  kTier: Depth holding spring constant for background corpora. "
           "Default is 12.0.\n"
           "  kDamping: Linear velocity damping coefficient. Default is 7.5.\n"
           "  backgroundDepthZ: Resting background depth coordinate. Default "
           "is -40.0.\n"
           "  defaultGap: Minimum comfortable horizontal reading gap. Default "
           "is 8.0.\n"
           "  maxForce: Maximum instantaneous physical force magnitude. "
           "Default is 10000.0.\n"
           "  maxVelocity: Maximum linear velocity magnitude for physical "
           "stability. Default is 1000.0.\n"
           "  timeStep: Default simulation time step in seconds. Default is "
           "0.016.\n"
           "beams:\n"
           "  bandStrandLimit: Maximum number of ribbon strands per link band. "
           "Default is 7.\n"
           "  bandStrandPitch: Spacing between strands in beam widths. Default "
           "is 2.2.\n"
           "  bandFillAlpha: Alpha multiplier for inner band fill strands. "
           "Default is 0.85.\n"
           "  stubWidthOfBeam: Width multiplier for margin anchor spine "
           "relative to beam width. Default is 1.35.\n"
           "  stubMinOfLine: Shortest margin anchor bracket as fraction of "
           "line height. Default is 0.9.\n"
           "  marginKerf: Gap between margin anchors sharing a lane. Default "
           "is 0.04.\n"
           "  bypassDepthPerDoc: Z offset per document passed when routing "
           "behind. Default is -20.0.\n"
           "  bypassDepthLimit: Deepest Z offset for bypass routing. Default "
           "is -120.0.\n"
           "  bypassSegments: Curve subdivision segment count for bypass "
           "routing. Default is 9.\n";
  case SystemDocKind::UI:
    return "Schema and Purpose\n\n"
           "Purpose:\n"
           "The sovereign UI system xanadoc configures visibility of chrome "
           "bars, status indicators, hypertime navigation overlays, and "
           "interactive radial action wheels.\n\n"
           "Schema:\n"
           "notificationPosition: Screen location for system notifications. "
           "Default is top-right.\n"
           "notificationDurationMs: Duration in milliseconds before "
           "notifications dismiss. Default is 3000.\n"
           "tabBarVisible: Flag indicating whether top document tab bar is "
           "visible. Default is true.\n"
           "statusBarVisible: Flag indicating whether bottom status bar is "
           "visible. Default is true.\n"
           "hypertimeMapVisible: Flag indicating whether hypertime graph "
           "overlay is open. Default is false.\n"
           "radialMenu: Nested configuration dictionary defining action items, "
           "icons, and radial radius.\n";
  case SystemDocKind::Pouches:
    return "Schema and Purpose\n\n"
           "Purpose:\n"
           "The sovereign Pouches system xanadoc specifies persistent staging "
           "zones for ghost spanables, link targets, and scratchpad passages "
           "awaiting hyperlinking.\n\n"
           "Schema:\n"
           "zone: List of named staging pouches available in the UI for "
           "dragging and collecting spans. Default zones are To Link, Notes "
           "for Later, and Scratch.\n";
  case SystemDocKind::Count:
    return "";
  }
  return "";
}

std::string defaultSystemDocNotes(const SystemDocKind kind) {
  switch (kind) {
  case SystemDocKind::Keymap:
    return "Notes\n\n"
           "User Annotations and Customization Record:\n"
           "This page is reserved for author notes, keybinding rationale, and "
           "custom shortcut modifications.\n";
  case SystemDocKind::Settings:
    return "Notes\n\n"
           "User Annotations and Customization Record:\n"
           "This page is reserved for author notes, typographic preferences, "
           "and display calibration details.\n";
  case SystemDocKind::Layout:
    return "Notes\n\n"
           "User Annotations and Customization Record:\n"
           "This page is reserved for author notes, screen dimension notes, "
           "and optical rendering customizations.\n";
  case SystemDocKind::UI:
    return "Notes\n\n"
           "User Annotations and Customization Record:\n"
           "This page is reserved for author notes, workflow preferences, and "
           "custom radial action mappings.\n";
  case SystemDocKind::Pouches:
    return "Notes\n\n"
           "User Annotations and Customization Record:\n"
           "This page is reserved for author notes, research taxonomy, and "
           "span drop zone organization notes.\n";
  case SystemDocKind::Count:
    return "";
  }
  return "";
}

void initializeSystemStore(Store &store, const SystemDocKind kind) {
  const std::string p1 = defaultSystemDocContent(kind);
  const std::string p2 = defaultSystemDocSchema(kind);
  const std::string p3 = defaultSystemDocNotes(kind);

  MicroversionId cur{};
  cur = store.insert(cur, 0, p1);

  const auto p1Size = static_cast<std::uint32_t>(p1.size());
  cur               = store.insertBreak(cur, p1Size);
  cur               = store.insert(cur, p1Size, p2);

  const auto p12Size = static_cast<std::uint32_t>(p1Size + p2.size());
  cur                = store.insertBreak(cur, p12Size);
  cur                = store.insert(cur, p12Size, p3);

  const Version doc = store.rebuild(cur);

  // Format links for Page 2 header: "Schema and Purpose" (centered and bold)
  constexpr std::string_view schemaHeader = "Schema and Purpose";
  const auto schemaHeaderSpans =
      doc.spansFor(p1Size, static_cast<std::uint32_t>(schemaHeader.size()));
  if (!schemaHeaderSpans.empty()) {
    Link boldLink;
    boldLink.type  = LinkType::Format;
    boldLink.tier  = ProminenceTier::Author;
    boldLink.owner = "system";
    boldLink.left  = schemaHeaderSpans;
    boldLink.right = {vocabularySpanFor(FormatAttribute::Bold)};
    cur            = store.addLink(cur, std::move(boldLink));

    Link centreLink;
    centreLink.type  = LinkType::Format;
    centreLink.tier  = ProminenceTier::Author;
    centreLink.owner = "system";
    centreLink.left  = schemaHeaderSpans;
    centreLink.right = {vocabularySpanFor(FormatAttribute::AlignCentre)};
    cur              = store.addLink(cur, std::move(centreLink));
  }

  // Format links for Page 3 header: "Notes" (centered and bold)
  constexpr std::string_view notesHeader = "Notes";
  const auto notesHeaderSpans =
      doc.spansFor(p12Size, static_cast<std::uint32_t>(notesHeader.size()));
  if (!notesHeaderSpans.empty()) {
    Link boldLink;
    boldLink.type  = LinkType::Format;
    boldLink.tier  = ProminenceTier::Author;
    boldLink.owner = "system";
    boldLink.left  = notesHeaderSpans;
    boldLink.right = {vocabularySpanFor(FormatAttribute::Bold)};
    cur            = store.addLink(cur, std::move(boldLink));

    Link centreLink;
    centreLink.type  = LinkType::Format;
    centreLink.tier  = ProminenceTier::Author;
    centreLink.owner = "system";
    centreLink.left  = notesHeaderSpans;
    centreLink.right = {vocabularySpanFor(FormatAttribute::AlignCentre)};
    cur              = store.addLink(cur, std::move(centreLink));
  }

  // Butterfly links connecting config (Page 1) to Schema (Page 2) and Notes
  // (Page 3)
  const auto p1Spans = doc.spansFor(0, p1Size);
  const auto p2Spans =
      doc.spansFor(p1Size, static_cast<std::uint32_t>(p2.size()));
  const auto p3Spans =
      doc.spansFor(p12Size, static_cast<std::uint32_t>(p3.size()));

  if (!p1Spans.empty() && !p2Spans.empty()) {
    Link schemaLink;
    schemaLink.type  = LinkType::Comment;
    schemaLink.tier  = ProminenceTier::Author;
    schemaLink.owner = "system";
    schemaLink.left  = p1Spans;
    schemaLink.right = p2Spans;
    cur              = store.addLink(cur, std::move(schemaLink));
  }

  if (!p1Spans.empty() && !p3Spans.empty()) {
    Link notesLink;
    notesLink.type  = LinkType::Comment;
    notesLink.tier  = ProminenceTier::Author;
    notesLink.owner = "system";
    notesLink.left  = p1Spans;
    notesLink.right = p3Spans;
    cur             = store.addLink(cur, std::move(notesLink));
  }

  store.repointCurrentVersion(cur);
  store.setVersionAnnotation(
      cur, {.alias       = "default",
            .description = "System default " + std::string(systemDocName(kind)),
            .tag         = "system",
            .timestamp   = ""});
}

std::filesystem::path systemDocDirectory(const SystemDocKind kind) {
  return std::filesystem::path(configPath()).parent_path() / "system" /
         systemDocName(kind);
}

namespace {

using common::yaml::parseBool;
using common::yaml::parseFloat;
using common::yaml::parseUint;
using common::yaml::ScopedCallbacks;
using common::yaml::stripQuotes;
using common::yaml::trimStr;

using ScopedRadialCallbacks = common::yaml::ScopedCallbacks;

std::vector<std::pair<std::string, std::string>>
parseKeyValueLines(const std::string_view text) {
  std::vector<std::pair<std::string, std::string>> pairs;
  std::size_t start = 0;
  while (start < text.size()) {
    auto end = text.find('\n', start);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    auto line = text.substr(start, end - start);
    start     = end + 1;
    if (const auto hash = line.find('#'); hash != std::string_view::npos) {
      line = line.substr(0, hash);
    }
    const auto colon = line.find(':');
    if (colon != std::string_view::npos) {
      auto k = trimStr(line.substr(0, colon));
      auto v = stripQuotes(line.substr(colon + 1));
      if (!k.empty() && !v.empty()) {
        pairs.emplace_back(std::string{k}, std::move(v));
      }
    }
  }
  return pairs;
}

} // namespace

gleditor::RadialConfig parseRadialConfig(const std::string_view yamlText) {
  auto makeAction = [](std::string id, std::string label, std::string icon,
                       std::string action) {
    gleditor::RadialAction a;
    a.id     = std::move(id);
    a.label  = std::move(label);
    a.desc   = a.label;
    a.icon   = std::move(icon);
    a.action = std::move(action);
    return a;
  };

  gleditor::RadialConfig cfg;
  cfg.radius      = 130.0F;
  cfg.innerRadius = 42.0F;

  auto alignAction =
      makeAction("group:align", "Align", "=", "subwheel:alignment");
  alignAction.desc       = "Alignment Sub-Menu";
  alignAction.subActions = {
      makeAction("align:left", "Left", "|<", "align:left"),
      makeAction("align:centre", "Centre", "><", "align:centre"),
      makeAction("align:right", "Right", ">|", "align:right"),
      makeAction("align:justify", "Justify", "|=", "align:justify"),
  };

  cfg.actions = {
      makeAction("format:bold", "Bold", "B", "format:bold"),
      makeAction("format:italic", "Italic", "I", "format:italic"),
      makeAction("format:underline", "Underline", "U", "format:underline"),
      makeAction("format:superscript", "Superscript", "X²",
                 "format:superscript"),
      makeAction("format:subscript", "Subscript", "X₂", "format:subscript"),
      std::move(alignAction),
      makeAction("op:pagebreak", "Page Break", "--", "op:pagebreak"),
      makeAction("op:transclude", "Transclude", "[]", "op:transclude"),
      makeAction("info:author", "Author", "@", "info:author"),
  };

  const auto effectiveYaml = extractConfigSection(yamlText);
  if (effectiveYaml.empty()) {
    return cfg;
  }

  const ScopedRadialCallbacks scoped;
  try {
    const c4::yml::Tree tree = c4::yml::parse_in_arena(
        c4::csubstr{effectiveYaml.data(), effectiveYaml.size()});
    if (tree.empty()) {
      return cfg;
    }
    const auto root = tree.rootref();
    if (!root.is_map() || !root.has_child("radialMenu")) {
      return cfg;
    }
    const auto rmenu = root["radialMenu"];
    if (!rmenu.is_map()) {
      return cfg;
    }

    if (rmenu.has_child("radius") && rmenu["radius"].has_val()) {
      float r      = cfg.radius;
      const auto v = rmenu["radius"].val();
      std::from_chars(v.data(), v.data() + v.size(), r);
      if (r > 10.0F) {
        cfg.radius = r;
      }
    }
    if (rmenu.has_child("innerRadius") && rmenu["innerRadius"].has_val()) {
      float ir     = cfg.innerRadius;
      const auto v = rmenu["innerRadius"].val();
      std::from_chars(v.data(), v.data() + v.size(), ir);
      if (ir >= 0.0F) {
        cfg.innerRadius = ir;
      }
    }

    if (rmenu.has_child("actions") && rmenu["actions"].is_seq()) {
      std::vector<gleditor::RadialAction> parsedActions;
      auto parseActionNode = [](const auto &node) -> gleditor::RadialAction {
        gleditor::RadialAction action;
        if (node.has_child("id") && node["id"].has_val()) {
          action.id =
              std::string{node["id"].val().data(), node["id"].val().size()};
        }
        if (node.has_child("label") && node["label"].has_val()) {
          action.label = std::string{node["label"].val().data(),
                                     node["label"].val().size()};
        }
        if (node.has_child("desc") && node["desc"].has_val()) {
          action.desc =
              std::string{node["desc"].val().data(), node["desc"].val().size()};
        } else {
          action.desc = action.label;
        }
        if (node.has_child("action") && node["action"].has_val()) {
          action.action = std::string{node["action"].val().data(),
                                      node["action"].val().size()};
        } else {
          action.action = action.id;
        }
        if (node.has_child("icon") && node["icon"].has_val()) {
          action.icon =
              std::string{node["icon"].val().data(), node["icon"].val().size()};
        }
        if (node.has_child("subActions") && node["subActions"].is_seq()) {
          for (const auto subNode : node["subActions"].children()) {
            gleditor::RadialAction sub;
            if (subNode.has_child("id") && subNode["id"].has_val()) {
              sub.id = std::string{subNode["id"].val().data(),
                                   subNode["id"].val().size()};
            }
            if (subNode.has_child("label") && subNode["label"].has_val()) {
              sub.label = std::string{subNode["label"].val().data(),
                                      subNode["label"].val().size()};
            }
            if (subNode.has_child("desc") && subNode["desc"].has_val()) {
              sub.desc = std::string{subNode["desc"].val().data(),
                                     subNode["desc"].val().size()};
            } else {
              sub.desc = sub.label;
            }
            if (subNode.has_child("action") && subNode["action"].has_val()) {
              sub.action = std::string{subNode["action"].val().data(),
                                       subNode["action"].val().size()};
            } else {
              sub.action = sub.id;
            }
            if (subNode.has_child("icon") && subNode["icon"].has_val()) {
              sub.icon = std::string{subNode["icon"].val().data(),
                                     subNode["icon"].val().size()};
            }
            if (!sub.id.empty() || !sub.label.empty()) {
              action.subActions.push_back(std::move(sub));
            }
          }
        }
        return action;
      };

      for (const auto actNode : rmenu["actions"].children()) {
        auto act = parseActionNode(actNode);
        if (!act.id.empty() || !act.label.empty()) {
          parsedActions.push_back(std::move(act));
        }
      }
      if (!parsedActions.empty()) {
        cfg.actions = std::move(parsedActions);
      }
    }
  } catch (const std::exception &) {
    // Return fallback cfg on parse error
  }

  return cfg;
}

KeymapConfig parseKeymapConfig(const std::string_view yamlText) {
  const auto effectiveYaml = extractConfigSection(yamlText);
  KeymapConfig cfg;
  if (effectiveYaml.empty()) {
    return cfg;
  }

  const ScopedRadialCallbacks scoped;
  try {
    const c4::yml::Tree tree = c4::yml::parse_in_arena(
        c4::csubstr{effectiveYaml.data(), effectiveYaml.size()});
    if (!tree.empty()) {
      const auto root = tree.rootref();
      if (root.is_map()) {
        for (const auto child : root.children()) {
          if (child.has_key() && child.has_val()) {
            std::string k{child.key().data(), child.key().size()};
            std::string v = stripQuotes(
                std::string_view{child.val().data(), child.val().size()});
            cfg.bindings.emplace_back(std::move(k), std::move(v));
          }
        }
        if (!cfg.bindings.empty()) {
          return cfg;
        }
      }
    }
  } catch (const std::exception &) {
    // Fall back to line-based parsing
  }

  cfg.bindings = parseKeyValueLines(effectiveYaml);
  return cfg;
}

SettingsConfig parseSettingsConfig(const std::string_view yamlText) {
  const auto effectiveYaml = extractConfigSection(yamlText);
  SettingsConfig cfg;
  if (effectiveYaml.empty()) {
    return cfg;
  }

  const auto applyKv = [&](const std::string_view k, const std::string_view v) {
    if (k == "fontSize") {
      cfg.fontSize = parseFloat(v, cfg.fontSize);
    } else if (k == "fontFamily") {
      cfg.fontFamily = stripQuotes(v);
    } else if (k == "lineHeight") {
      cfg.lineHeight = parseFloat(v, cfg.lineHeight);
    } else if (k == "theme") {
      cfg.theme = stripQuotes(v);
    } else if (k == "autoSaveSeconds") {
      cfg.autoSaveSeconds = parseUint(v, cfg.autoSaveSeconds);
    }
  };

  const ScopedRadialCallbacks scoped;
  try {
    const c4::yml::Tree tree = c4::yml::parse_in_arena(
        c4::csubstr{effectiveYaml.data(), effectiveYaml.size()});
    if (!tree.empty()) {
      const auto root = tree.rootref();
      if (root.is_map()) {
        for (const auto child : root.children()) {
          if (child.has_key() && child.has_val()) {
            const std::string_view k{child.key().data(), child.key().size()};
            const std::string_view v{child.val().data(), child.val().size()};
            applyKv(k, v);
          }
        }
        return cfg;
      }
    }
  } catch (const std::exception &) {
    // Fall back to line-based parsing
  }

  for (const auto &[k, v] : parseKeyValueLines(effectiveYaml)) {
    applyKv(k, v);
  }
  return cfg;
}

LayoutConfig parseLayoutConfig(const std::string_view yamlText) {
  const auto effectiveYaml = extractConfigSection(yamlText);
  LayoutConfig cfg;
  if (effectiveYaml.empty()) {
    return cfg;
  }

  const auto applyKv = [&](const std::string_view k, const std::string_view v) {
    if (k == "columns") {
      cfg.columns = parseUint(v, cfg.columns);
    } else if (k == "pageWidthPx") {
      cfg.pageWidthPx = parseFloat(v, cfg.pageWidthPx);
    } else if (k == "pageHeightPx") {
      cfg.pageHeightPx = parseFloat(v, cfg.pageHeightPx);
    } else if (k == "toastAnchor" || k == "notificationPosition") {
      const auto val = stripQuotes(v);
      if (val == "top-left" || val == "TopLeft") {
        cfg.toastAnchor = ToastAnchor::TopLeft;
      } else if (val == "bottom-right" || val == "BottomRight") {
        cfg.toastAnchor = ToastAnchor::BottomRight;
      } else if (val == "bottom-left" || val == "BottomLeft") {
        cfg.toastAnchor = ToastAnchor::BottomLeft;
      } else if (val == "top-center" || val == "TopCenter") {
        cfg.toastAnchor = ToastAnchor::TopCenter;
      } else {
        cfg.toastAnchor = ToastAnchor::TopRight;
      }
    } else if (k == "toastOffsetX") {
      cfg.toastOffsetX = parseFloat(v, cfg.toastOffsetX);
    } else if (k == "toastOffsetY") {
      cfg.toastOffsetY = parseFloat(v, cfg.toastOffsetY);
    } else if (k == "pouchDock") {
      const auto val = stripQuotes(v);
      if (val == "left" || val == "Left") {
        cfg.pouchDock = PouchDock::Left;
      } else {
        cfg.pouchDock = PouchDock::Right;
      }
    } else if (k == "documentSpacingX") {
      cfg.documentSpacingX = parseFloat(v, cfg.documentSpacingX);
    } else if (k == "transclusionPrisms") {
      cfg.transclusionPrisms = parseBool(v, cfg.transclusionPrisms);
    } else if (k == "xanalinkRibbons") {
      cfg.xanalinkRibbons = parseBool(v, cfg.xanalinkRibbons);
    } else if (k == "kRepel" || k == "physics.kRepel") {
      cfg.physics.kRepel = parseFloat(v, cfg.physics.kRepel);
    } else if (k == "kPlane" || k == "physics.kPlane") {
      cfg.physics.kPlane = parseFloat(v, cfg.physics.kPlane);
    } else if (k == "kAlign" || k == "physics.kAlign") {
      cfg.physics.kAlign = parseFloat(v, cfg.physics.kAlign);
    } else if (k == "kTier" || k == "physics.kTier") {
      cfg.physics.kTier = parseFloat(v, cfg.physics.kTier);
    } else if (k == "kDamping" || k == "physics.kDamping") {
      cfg.physics.kDamping = parseFloat(v, cfg.physics.kDamping);
    } else if (k == "backgroundDepthZ" || k == "physics.backgroundDepthZ") {
      cfg.physics.backgroundDepthZ =
          parseFloat(v, cfg.physics.backgroundDepthZ);
    } else if (k == "defaultGap" || k == "physics.defaultGap") {
      cfg.physics.defaultGap = parseFloat(v, cfg.physics.defaultGap);
    } else if (k == "settleVelocityThreshold" ||
               k == "physics.settleVelocityThreshold") {
      cfg.physics.settleVelocityThreshold =
          parseFloat(v, cfg.physics.settleVelocityThreshold);
    } else if (k == "maxForce" || k == "physics.maxForce") {
      cfg.physics.maxForce = parseFloat(v, cfg.physics.maxForce);
    } else if (k == "maxVelocity" || k == "physics.maxVelocity") {
      cfg.physics.maxVelocity = parseFloat(v, cfg.physics.maxVelocity);
    } else if (k == "timeStep" || k == "physics.timeStep") {
      cfg.physics.timeStep = parseFloat(v, cfg.physics.timeStep);
    } else if (k == "bandStrandLimit" || k == "beams.bandStrandLimit") {
      cfg.beams.bandStrandLimit =
          parseUint(v, static_cast<std::uint32_t>(cfg.beams.bandStrandLimit));
    } else if (k == "bandStrandPitch" || k == "beams.bandStrandPitch") {
      cfg.beams.bandStrandPitch = parseFloat(v, cfg.beams.bandStrandPitch);
    } else if (k == "bandFillAlpha" || k == "beams.bandFillAlpha") {
      cfg.beams.bandFillAlpha = parseFloat(v, cfg.beams.bandFillAlpha);
    } else if (k == "stubWidthOfBeam" || k == "beams.stubWidthOfBeam") {
      cfg.beams.stubWidthOfBeam = parseFloat(v, cfg.beams.stubWidthOfBeam);
    } else if (k == "stubMinOfLine" || k == "beams.stubMinOfLine") {
      cfg.beams.stubMinOfLine = parseFloat(v, cfg.beams.stubMinOfLine);
    } else if (k == "marginKerf" || k == "beams.marginKerf") {
      cfg.beams.marginKerf = parseFloat(v, cfg.beams.marginKerf);
    } else if (k == "bypassDepthPerDoc" || k == "beams.bypassDepthPerDoc") {
      cfg.beams.bypassDepthPerDoc = parseFloat(v, cfg.beams.bypassDepthPerDoc);
    } else if (k == "bypassDepthLimit" || k == "beams.bypassDepthLimit") {
      cfg.beams.bypassDepthLimit = parseFloat(v, cfg.beams.bypassDepthLimit);
    } else if (k == "bypassSegments" || k == "beams.bypassSegments") {
      cfg.beams.bypassSegments =
          parseUint(v, static_cast<std::uint32_t>(cfg.beams.bypassSegments));
    }
  };

  const ScopedRadialCallbacks scoped;
  try {
    const c4::yml::Tree tree = c4::yml::parse_in_arena(
        c4::csubstr{effectiveYaml.data(), effectiveYaml.size()});
    if (!tree.empty()) {
      const auto root = tree.rootref();
      if (root.is_map()) {
        for (const auto child : root.children()) {
          if (child.has_key() && child.has_val()) {
            const std::string_view k{child.key().data(), child.key().size()};
            const std::string_view v{child.val().data(), child.val().size()};
            applyKv(k, v);
          } else if (child.has_key() && child.is_map()) {
            const std::string_view sectionKey{child.key().data(),
                                              child.key().size()};
            for (const auto grandChild : child.children()) {
              if (grandChild.has_key() && grandChild.has_val()) {
                const std::string_view gk{grandChild.key().data(),
                                          grandChild.key().size()};
                const std::string_view gv{grandChild.val().data(),
                                          grandChild.val().size()};
                applyKv(gk, gv);
                const std::string compositeKey =
                    std::string(sectionKey) + "." + std::string(gk);
                applyKv(compositeKey, gv);
              }
            }
          }
        }
        return cfg;
      }
    }
  } catch (const std::exception &) {
    // Fall back to line-based parsing
  }

  for (const auto &[k, v] : parseKeyValueLines(effectiveYaml)) {
    applyKv(k, v);
  }
  return cfg;
}

UIConfig parseUIConfig(const std::string_view yamlText) {
  const auto effectiveYaml = extractConfigSection(yamlText);
  UIConfig cfg;
  cfg.radialMenu = parseRadialConfig(effectiveYaml);
  if (effectiveYaml.empty()) {
    return cfg;
  }

  const auto applyKv = [&](const std::string_view k, const std::string_view v) {
    if (k == "tabBarVisible") {
      cfg.tabBarVisible = parseBool(v, cfg.tabBarVisible);
    } else if (k == "statusBarVisible") {
      cfg.statusBarVisible = parseBool(v, cfg.statusBarVisible);
    } else if (k == "hypertimeMapVisible") {
      cfg.hypertimeMapVisible = parseBool(v, cfg.hypertimeMapVisible);
    }
  };

  const ScopedRadialCallbacks scoped;
  try {
    const c4::yml::Tree tree = c4::yml::parse_in_arena(
        c4::csubstr{effectiveYaml.data(), effectiveYaml.size()});
    if (!tree.empty()) {
      const auto root = tree.rootref();
      if (root.is_map()) {
        for (const auto child : root.children()) {
          if (child.has_key() && child.has_val()) {
            const std::string_view k{child.key().data(), child.key().size()};
            const std::string_view v{child.val().data(), child.val().size()};
            applyKv(k, v);
          }
        }
        return cfg;
      }
    }
  } catch (const std::exception &) {
    // Fall back to line-based parsing
  }

  for (const auto &[k, v] : parseKeyValueLines(effectiveYaml)) {
    applyKv(k, v);
  }
  return cfg;
}

void initializeSystemStoreFromSlice(Store &store, const SystemDocKind kind,
                                    const zigzag::ZzStructureDocument &slice) {
  zigzag::projectSystemSliceToStore(slice, store, kind);
}

KeymapConfig KeymapConfig::fromYaml(const std::string_view yamlText) {
  return parseKeymapConfig(yamlText);
}

KeymapConfig KeymapConfig::fromSlice(const zigzag::ZzStructureDocument &slice) {
  const std::string configText = zigzag::extractSliceConfigText(slice);
  return parseKeymapConfig(configText);
}

SettingsConfig SettingsConfig::fromYaml(const std::string_view yamlText) {
  return parseSettingsConfig(yamlText);
}

SettingsConfig
SettingsConfig::fromSlice(const zigzag::ZzStructureDocument &slice) {
  const std::string configText = zigzag::extractSliceConfigText(slice);
  return parseSettingsConfig(configText);
}

LayoutConfig LayoutConfig::fromYaml(const std::string_view yamlText) {
  return parseLayoutConfig(yamlText);
}

LayoutConfig LayoutConfig::fromSlice(const zigzag::ZzStructureDocument &slice) {
  const std::string configText = zigzag::extractSliceConfigText(slice);
  return parseLayoutConfig(configText);
}

UIConfig UIConfig::fromYaml(const std::string_view yamlText) {
  return parseUIConfig(yamlText);
}

UIConfig UIConfig::fromSlice(const zigzag::ZzStructureDocument &slice) {
  const std::string configText = zigzag::extractSliceConfigText(slice);
  return parseUIConfig(configText);
}

} // namespace xanadu

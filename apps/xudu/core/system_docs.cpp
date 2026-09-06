/**
 * @file system_docs.cpp
 * @brief Sovereign system xanadocs managing runtime parameters.
 */
#include "xudu/core/system_docs.hpp"

#include <charconv>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <ryml.hpp>
#include <ryml_std.hpp>

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
           "hypertime-map: \"Ctrl+H\"\n"
           "radial-menu: \"Ctrl+M\"\n";
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

namespace {

void rymlRadialErrorHandler(const c4::csubstr msg,
                            const c4::yml::ErrorDataBasic &, void *) {
  throw std::runtime_error(std::string{msg.str, msg.len});
}

struct ScopedRadialCallbacks {
  c4::yml::Callbacks prev;
  ScopedRadialCallbacks() {
    prev = c4::yml::get_callbacks();
    c4::yml::Callbacks cb;
    cb.m_error_basic = rymlRadialErrorHandler;
    c4::yml::set_callbacks(cb);
  }
  ~ScopedRadialCallbacks() { c4::yml::set_callbacks(prev); }
};

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

  if (yamlText.empty()) {
    return cfg;
  }

  const ScopedRadialCallbacks scoped;
  try {
    const c4::yml::Tree tree =
        c4::yml::parse_in_arena(c4::csubstr{yamlText.data(), yamlText.size()});
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

} // namespace xudu

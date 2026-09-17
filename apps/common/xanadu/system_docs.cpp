/**
 * @file system_docs.cpp
 * @brief Sovereign system xanadocs managing runtime parameters.
 */
#include "common/xanadu/system_docs.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/config.hpp"
#include "common/xanadu/format.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/version.hpp"
#include "common/xanadu/zigzag/dimension_registry.hpp"
#include "common/xanadu/zigzag/manifold.hpp"
#include "common/xanadu/zigzag/zz_system_projector.hpp"
#include "common/xanadu/zigzag/zzstructure.hpp"
#include <gleditor/color.hpp>

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
           "transclusionLoom: \"true\"\n"
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
           "  bypassSegments: \"9\"\n"
           "  loomBundlingEnabled: \"true\"\n"
           "  loomAlpha: \"0.35\"\n"
           "  loomHoverAlpha: \"1.0\"\n";
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
           "  - id: \"to_link_left\"\n"
           "    label: \"To Link (Left)\"\n"
           "    aura: \"#06B6D4\"\n"
           "    weight: 1.0\n"
           "  - id: \"to_link_right\"\n"
           "    label: \"To Link (Right)\"\n"
           "    aura: \"#EC4899\"\n"
           "    weight: 1.0\n"
           "  - id: \"notes\"\n"
           "    label: \"Notes\"\n"
           "    aura: \"#EAB308\"\n"
           "    weight: 1.0\n"
           "  - id: \"scratch\"\n"
           "    label: \"Scratch\"\n"
           "    aura: \"#10B981\"\n"
           "    weight: 1.0\n";
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
           "transclusionLoom: Bundle adjacent rank transclusions into "
           "continuous "
           "golden looms. Default is true.\n"
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
           "routing. Default is 9.\n"
           "  loomBundlingEnabled: Group contiguous rank transclusion strands "
           "into unified laminar looms. Default is true.\n"
           "  loomAlpha: Semi-transparent resting alpha for golden "
           "transclusion "
           "looms. Default is 0.35.\n"
           "  loomHoverAlpha: Active or hovered alpha for brightened "
           "transclusion "
           "strands. Default is 1.0.\n"
           "zigzag: System-slice presentation policy for cell card padding, "
           "content width limits, rank clearance, HUD spacing, and "
           "connection beam width. All lengths are logical pixels.\n";
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

std::filesystem::path systemDocDirectory(const SystemDocKind kind) {
  return std::filesystem::path(configPath()).parent_path() / "system" /
         systemDocName(kind);
}

namespace {

std::vector<std::string> splitTokens(const std::string_view str,
                                     const char delim) {
  std::vector<std::string> tokens;
  std::size_t start = 0;
  while (start < str.size()) {
    auto end = str.find(delim, start);
    if (end == std::string_view::npos) {
      end = str.size();
    }
    tokens.emplace_back(str.substr(start, end - start));
    start = end + 1;
  }
  return tokens;
}

zigzag::DimRef getOrMakeDim(Store &store, MicroversionId &cur,
                            zigzag::Manifold &manifold,
                            const std::string_view name) {
  return zigzag::DimensionRegistry::instance().getOrCreate(store, cur, manifold,
                                                           name);
}

zigzag::CellRef findPrototypeCell(const zigzag::Manifold &manifold,
                                  const zigzag::DimRef schemasDim,
                                  const std::string_view typeName,
                                  const SpanReader &reader) {
  zigzag::CellRef found = zigzag::noCell;
  const auto norm       = [](const std::string_view t) -> std::string_view {
    if (t == "double") return "float";
    if (t == "int" || t == "int64") return "integer";
    if (t == "boolean") return "bool";
    return t;
  };
  const auto targetNorm = norm(typeName);
  manifold.walkRank(schemasDim, schemasDim, zigzag::DimVector::POS,
                    [&](const zigzag::CellRef cell) {
                      if (cell != schemasDim) {
                        const auto cellType = manifold.textOf(cell, reader);
                        if (cellType == typeName ||
                            norm(cellType) == targetNorm) {
                          found = cell;
                          return false;
                        }
                      }
                      return true;
                    });
  return found;
}

MicroversionId makeValueCell(Store &store, MicroversionId cur,
                             const CellValue &val, zigzag::CellRef &outRef) {
  if (std::holds_alternative<double>(val)) {
    cur = store.makeScalarCell(cur, std::get<double>(val));
  } else if (std::holds_alternative<std::int64_t>(val)) {
    cur = store.makeScalarCell(cur, std::get<std::int64_t>(val));
  } else if (std::holds_alternative<bool>(val)) {
    cur = store.makeScalarCell(cur, std::get<bool>(val));
  } else {
    cur = store.makeCell(cur, std::get<std::string>(val));
  }
  outRef = store.cellRefOf(cur);
  return cur;
}

bool matchShape(const SettingSchemaShape &shape,
                const std::span<const CellValue> values,
                std::string *const errOut) {
  if (values.size() != shape.expectedTypes.size()) {
    if (errOut != nullptr) {
      *errOut = "Element count mismatch: expected " +
                std::to_string(shape.expectedTypes.size()) + ", got " +
                std::to_string(values.size());
    }
    return false;
  }
  for (std::size_t i = 0; i < values.size(); ++i) {
    const auto &expected = shape.expectedTypes[i];
    const auto &val      = values[i];
    if (expected == "float" || expected == "double") {
      if (!std::holds_alternative<double>(val) &&
          !std::holds_alternative<std::int64_t>(val)) {
        if (std::holds_alternative<std::string>(val)) {
          try {
            std::stod(std::get<std::string>(val));
          } catch (...) {
            if (errOut != nullptr) {
              *errOut = "Element " + std::to_string(i) + " is not a float";
            }
            return false;
          }
        } else {
          if (errOut != nullptr) {
            *errOut = "Element " + std::to_string(i) + " is not a float";
          }
          return false;
        }
      }
    } else if (expected == "integer" || expected == "int" ||
               expected == "int64") {
      if (!std::holds_alternative<std::int64_t>(val)) {
        if (std::holds_alternative<double>(val)) {
          const double d = std::get<double>(val);
          if (std::floor(d) != d) {
            if (errOut != nullptr) {
              *errOut = "Element " + std::to_string(i) + " is not an integer";
            }
            return false;
          }
        } else if (std::holds_alternative<std::string>(val)) {
          try {
            std::stoll(std::get<std::string>(val));
          } catch (...) {
            if (errOut != nullptr) {
              *errOut = "Element " + std::to_string(i) + " is not an integer";
            }
            return false;
          }
        } else {
          if (errOut != nullptr) {
            *errOut = "Element " + std::to_string(i) + " is not an integer";
          }
          return false;
        }
      }
    } else if (expected == "bool" || expected == "boolean") {
      if (!std::holds_alternative<bool>(val)) {
        if (std::holds_alternative<std::string>(val)) {
          const auto &s = std::get<std::string>(val);
          if (s != "true" && s != "false" && s != "1" && s != "0") {
            if (errOut != nullptr) {
              *errOut = "Element " + std::to_string(i) + " is not a bool";
            }
            return false;
          }
        } else if (std::holds_alternative<std::int64_t>(val)) {
          const auto n = std::get<std::int64_t>(val);
          if (n != 0 && n != 1) {
            if (errOut != nullptr) {
              *errOut = "Element " + std::to_string(i) + " is not a bool";
            }
            return false;
          }
        } else {
          if (errOut != nullptr) {
            *errOut = "Element " + std::to_string(i) + " is not a bool";
          }
          return false;
        }
      }
    }
  }
  return true;
}

} // namespace

std::vector<SettingSpec> defaultSettingSpecs(const SystemDocKind kind) {
  std::vector<SettingSpec> specs;
  switch (kind) {
  case SystemDocKind::Layout:
    specs = {
        {std::string(settings::kColumns),
         "Number of document columns in layout",
         {{{"integer"}, {std::int64_t{2}}}}},
        {std::string(settings::kPageWidthPx),
         "Width of each virtual page in pixels",
         {{{"float"}, {800.0}}}},
        {std::string(settings::kPageHeightPx),
         "Height of each virtual page in pixels",
         {{{"float"}, {1000.0}}}},
        {std::string(settings::kTransclusionPrisms),
         "Render 3D prisms for transclusion bridges",
         {{{"bool"}, {true}}}},
        {std::string(settings::kTransclusionLoom),
         "Enable loom view bundling for transclusions",
         {{{"bool"}, {true}}}},
        {std::string(settings::kXanalinkRibbons),
         "Render curved ribbon links between xanadocs",
         {{{"bool"}, {true}}}},
        {std::string(settings::kPouchDock),
         "Docking edge for pouch drawer ('left' or 'right')",
         {{{"string"}, {std::string{"right"}}}}},
        {std::string(settings::kPouchWidthPx),
         "Width of pouch drawer in pixels",
         {{{"float"}, {360.0}}}},
        {std::string(settings::kPhysicsKRepel),
         "Repulsion spring stiffness",
         {{{"float"}, {4500.0}}}},
        {std::string(settings::kPhysicsKPlane),
         "In-plane alignment spring stiffness",
         {{{"float"}, {14.0}}}},
        {std::string(settings::kPhysicsKAlign),
         "Inter-page angle alignment stiffness",
         {{{"float"}, {28.0}}}},
        {std::string(settings::kPhysicsKTier),
         "Prominence tier spring stiffness",
         {{{"float"}, {12.0}}}},
        {std::string(settings::kPhysicsKDamping),
         "Velocity damping coefficient",
         {{{"float"}, {7.5}}}},
        {std::string(settings::kPhysicsBackgroundDepthZ),
         "Z-depth offset for background pages",
         {{{"float"}, {-40.0}}}},
        {std::string(settings::kPhysicsDefaultGap),
         "Default gap between pages in pixels",
         {{{"float"}, {8.0}}}},
        {std::string(settings::kPhysicsSettleVelocityThreshold),
         "Velocity cutoff for physics settle",
         {{{"float"}, {0.02}}}},
        {std::string(settings::kPhysicsMaxForce),
         "Maximum instantaneous physical force clamp",
         {{{"float"}, {10000.0}}}},
        {std::string(settings::kPhysicsMaxVelocity),
         "Maximum linear velocity clamp",
         {{{"float"}, {1000.0}}}},
        {std::string(settings::kPhysicsTimeStep),
         "Physics simulation time delta in seconds",
         {{{"float"}, {0.016}}}},
        {std::string(settings::kBeamsBandStrandLimit),
         "Max strands in a ribbon band",
         {{{"integer"}, {std::int64_t{7}}}}},
        {std::string(settings::kBeamsBandStrandPitch),
         "Spacing between strands in beam widths",
         {{{"float"}, {2.2}}}},
        {std::string(settings::kBeamsBandFillAlpha),
         "Opacity for band fill ribbons",
         {{{"float"}, {0.85}}}},
        {std::string(settings::kBeamsStubWidthOfBeam),
         "Stub width factor relative to beam",
         {{{"float"}, {1.35}}}},
        {std::string(settings::kBeamsStubMinOfLine),
         "Minimum stub length in line heights",
         {{{"float"}, {0.9}}}},
        {std::string(settings::kBeamsMarginKerf),
         "Kerf margin fraction",
         {{{"float"}, {0.04}}}},
        {std::string(settings::kBeamsBypassDepthPerDoc),
         "Z-depth step for bypass routing",
         {{{"float"}, {-20.0}}}},
        {std::string(settings::kBeamsBypassDepthLimit),
         "Max Z-depth limit for bypasses",
         {{{"float"}, {-120.0}}}},
        {std::string(settings::kBeamsBypassSegments),
         "Spline subdivisions for bypass curves",
         {{{"integer"}, {std::int64_t{9}}}}},
        {std::string(settings::kBeamsLoomBundlingEnabled),
         "Whether loom cables bundle together",
         {{{"bool"}, {true}}}},
        {std::string(settings::kBeamsLoomAlpha),
         "Base opacity for loom cables",
         {{{"float"}, {0.35}}}},
        {std::string(settings::kBeamsLoomHoverAlpha),
         "Hovered opacity for loom cables",
         {{{"float"}, {1.0}}}},
        {std::string(settings::kZigzagCellHorizontalPaddingPx),
         "Horizontal cell padding in px",
         {{{"float"}, {8.0}}}},
        {std::string(settings::kZigzagCellVerticalPaddingPx),
         "Vertical cell padding in px",
         {{{"float"}, {6.0}}}},
        {std::string(settings::kZigzagCellBandGapPx),
         "Gap between cell bands in pixels",
         {{{"float"}, {4.0}}}},
        {std::string(settings::kZigzagContentMaxWidthPx),
         "Max width in content projection",
         {{{"float"}, {260.0}}}},
        {std::string(settings::kZigzagTopologyMaxWidthPx),
         "Max width in topology projection",
         {{{"float"}, {140.0}}}},
        {std::string(settings::kZigzagRankClearancePx),
         "Clearance between perpendicular ranks",
         {{{"float"}, {24.0}}}},
        {std::string(settings::kZigzagHudHorizontalPaddingPx),
         "HUD horizontal padding",
         {{{"float"}, {16.0}}}},
        {std::string(settings::kZigzagHudVerticalPaddingPx),
         "HUD vertical padding",
         {{{"float"}, {8.0}}}},
        {std::string(settings::kZigzagHudColumnGapPx),
         "HUD gap between columns",
         {{{"float"}, {8.0}}}},
        {std::string(settings::kZigzagConnectionBeamWidthPx),
         "Connection beam line width",
         {{{"float"}, {4.0}}}},
    };
    break;

  case SystemDocKind::Settings:
    specs = {
        {std::string(settings::kFontSize),
         "Base text point size",
         {{{"float"}, {16.0}}}},
        {std::string(settings::kFontFamily),
         "Default typeface family name",
         {{{"string"}, {std::string{"Monospace"}}}}},
        {std::string(settings::kLineHeight),
         "Line spacing multiplier",
         {{{"float"}, {1.4}}}},
        {std::string(settings::kAutoSaveSeconds),
         "Interval in seconds between auto-saves",
         {{{"integer"}, {std::int64_t{5}}}}},
        {std::string(settings::kTheme),
         "Active UI theme name",
         {{{"string"}, {std::string{"system"}}}}},
        {std::string(settings::kThemeBackground),
         "Theme background color (RGB as 3 floats 0..1 or 3 integers 0..255)",
         {{{"float", "float", "float"}, {0.1, 0.1, 0.1}},
          {{"integer", "integer", "integer"},
           {std::int64_t{26}, std::int64_t{26}, std::int64_t{26}}}}},
    };
    break;

  case SystemDocKind::UI:
    specs = {
        {std::string(settings::kTabBarVisible),
         "Visibility of the document tab switcher bar",
         {{{"bool"}, {true}}}},
        {std::string(settings::kStatusBarVisible),
         "Visibility of the status bar",
         {{{"bool"}, {true}}}},
        {std::string(settings::kHypertimeMapVisible),
         "Visibility of the hypertime map",
         {{{"bool"}, {false}}}},
        {std::string(settings::kNotificationPosition),
         "Screen position for toast notifications",
         {{{"string"}, {std::string{"top-right"}}}}},
        {std::string(settings::kNotificationDurationMs),
         "Toast notification duration in milliseconds",
         {{{"integer"}, {std::int64_t{3000}}}}},
        {std::string(settings::kRadialMenuRadius),
         "Outer radius of radial context menu in pixels",
         {{{"float"}, {130.0}}}},
        {std::string(settings::kRadialMenuInnerRadius),
         "Inner deadzone radius of radial menu in pixels",
         {{{"float"}, {42.0}}}},
    };
    break;

  case SystemDocKind::Keymap:
    specs = {
        {std::string(settings::kKeymapNewDoc),
         "Shortcut to create a new document",
         {{{"string"}, {std::string{"Ctrl+N"}}}}},
        {std::string(settings::kKeymapOpenDoc),
         "Shortcut to open a document",
         {{{"string"}, {std::string{"Ctrl+O"}}}}},
        {std::string(settings::kKeymapCloseDoc),
         "Shortcut to close active document",
         {{{"string"}, {std::string{"Ctrl+W"}}}}},
        {std::string(settings::kKeymapForward),
         "Shortcut to branch/step forward in hypertime",
         {{{"string"}, {std::string{"Ctrl+Shift+N"}}}}},
        {std::string(settings::kKeymapScrubForward),
         "Shortcut to scrub hypertime forward",
         {{{"string"}, {std::string{"Ctrl+]"}}}}},
        {std::string(settings::kKeymapScrubBackward),
         "Shortcut to scrub hypertime backward",
         {{{"string"}, {std::string{"Ctrl+["}}}}},
        {std::string(settings::kKeymapHypertimeMap),
         "Shortcut to toggle hypertime map",
         {{{"string"}, {std::string{"Ctrl+H"}}}}},
        {std::string(settings::kKeymapRadialMenu),
         "Shortcut to invoke radial menu",
         {{{"string"}, {std::string{"Ctrl+M"}}}}},
    };
    break;

  case SystemDocKind::Pouches:
    specs = {
        {std::string(settings::kPouchZoneToLinkLeft),
         "Left transclusion staging drop zone: [label, auraColor, "
         "heightWeight]",
         {{{"string", "integer", "float"},
           {std::string{"To Link (Left)"},
            static_cast<std::int64_t>(0x06B6D4FFU), 1.0}}}},
        {std::string(settings::kPouchZoneToLinkRight),
         "Right transclusion staging drop zone: [label, auraColor, "
         "heightWeight]",
         {{{"string", "integer", "float"},
           {std::string{"To Link (Right)"},
            static_cast<std::int64_t>(0xEC4899FFU), 1.0}}}},
        {std::string(settings::kPouchZoneNotes),
         "Notes drop zone: [label, auraColor, heightWeight]",
         {{{"string", "integer", "float"},
           {std::string{"Notes"}, static_cast<std::int64_t>(0xEAB308FFU),
            1.0}}}},
        {std::string(settings::kPouchZoneScratch),
         "Scratch drop zone: [label, auraColor, heightWeight]",
         {{{"string", "integer", "float"},
           {std::string{"Scratch"}, static_cast<std::int64_t>(0x10B981FFU),
            1.0}}}},
    };
    break;

  case SystemDocKind::Count:
    break;
  }
  return specs;
}

MicroversionId initializeSystemStoreGenesis(Store &store,
                                            const SystemDocKind kind,
                                            const MicroversionId &parent) {
  auto cur = parent.isZero() ? (store.currentVersions().empty()
                                    ? MicroversionId{}
                                    : store.currentVersions().front())
                             : parent;
  if (store.homeCell() == zigzag::noCell) {
    cur = store.sliceGenesis(cur);
  }
  auto manifold = store.rebuildManifold(cur);

  // Mint all 9 dimensions (d.dims was minted by sliceGenesis)
  getOrMakeDim(store, cur, manifold, kDimVars);
  getOrMakeDim(store, cur, manifold, kDimValues);
  const auto groupsDim = getOrMakeDim(store, cur, manifold, kDimGroups);
  getOrMakeDim(store, cur, manifold, kDimSubgroups);
  getOrMakeDim(store, cur, manifold, kDimClone);
  const auto notesDim   = getOrMakeDim(store, cur, manifold, kDimNotes);
  const auto schemasDim = getOrMakeDim(store, cur, manifold, kDimSchemas);
  getOrMakeDim(store, cur, manifold, kDimAlternates);
  getOrMakeDim(store, cur, manifold, kDimDefault);

  // Set home cell description of dimensions in use
  constexpr std::string_view kHomeDesc =
      "Dimensions in use:\n"
      "- d.dims: dimension registry (posward from home)\n"
      "- d.vars: master setting names (posward from home) and group member "
      "clones\n"
      "- d.values: active setting values (posward from setting name)\n"
      "- d.groups: top-level group rank (posward from home; 1st cell is blank "
      "empty group) and sibling groups\n"
      "- d.subgroups: hierarchical subgroup rank (posward from parent group)\n"
      "- d.clone: master setting to group clone, and prototype type cell to "
      "schema type clone\n"
      "- d.notes: store description (posward from home) and setting "
      "description (posward from setting)\n"
      "- d.schemas: schema shape blank cell (posward from setting), and "
      "expected type clones\n"
      "- d.alternates: alternative schema shape choices (posward between blank "
      "cells)\n"
      "- d.default: default value cell (posward from each schema type clone)";
  cur      = store.setCellText(cur, store.homeCell(), kHomeDesc, &manifold);
  manifold = store.rebuildManifold(cur);

  // Mint store-level notes cell off home along +d.notes
  const auto storeNoteCell =
      manifold.linked(store.homeCell(), notesDim, zigzag::DimVector::POS);
  if (storeNoteCell == zigzag::noCell) {
    const std::string storeNoteText = "Sovereign system store managing " +
                                      std::string(systemDocName(kind)) +
                                      " configuration.";
    cur                             = store.makeCell(cur, storeNoteText);
    const auto noteRef              = store.cellRefOf(cur);
    manifold                        = store.rebuildManifold(cur);
    cur = store.setLink(cur, store.homeCell(), notesDim, zigzag::DimVector::POS,
                        noteRef, &manifold);
    manifold = store.rebuildManifold(cur);
  }

  // Mint empty group (blank cell) off home along +d.groups if not present
  const auto firstGroupCell =
      manifold.linked(store.homeCell(), groupsDim, zigzag::DimVector::POS);
  if (firstGroupCell == zigzag::noCell) {
    cur                      = store.makeCell(cur, "");
    const auto emptyGroupRef = store.cellRefOf(cur);
    manifold                 = store.rebuildManifold(cur);
    cur      = store.setLink(cur, store.homeCell(), groupsDim,
                             zigzag::DimVector::POS, emptyGroupRef, &manifold);
    manifold = store.rebuildManifold(cur);
  }

  // Mint prototype type cells along d.schemas off d.schemas dimension cell
  const auto protoHead =
      manifold.linked(schemasDim, schemasDim, zigzag::DimVector::POS);
  if (protoHead == zigzag::noCell) {
    const std::string_view protoTypes[] = {"float", "integer", "bool",
                                           "string"};
    auto prev                           = schemasDim;
    for (const auto &typeStr : protoTypes) {
      cur             = store.makeCell(cur, typeStr);
      const auto cell = store.cellRefOf(cur);
      manifold        = store.rebuildManifold(cur);
      cur = store.setLink(cur, prev, schemasDim, zigzag::DimVector::POS, cell,
                          &manifold);
      manifold = store.rebuildManifold(cur);
      prev     = cell;
    }
  }

  store.repointCurrentVersion(cur);
  return cur;
}

MicroversionId ensureSetting(Store &store, const MicroversionId &parent,
                             const SettingSpec &spec,
                             const zigzag::Manifold *const known) {
  auto cur = parent.isZero() ? (store.currentVersions().empty()
                                    ? MicroversionId{}
                                    : store.currentVersions().front())
                             : parent;
  if (store.homeCell() == zigzag::noCell) {
    cur = initializeSystemStoreGenesis(store, SystemDocKind::Layout, cur);
  }

  std::optional<zigzag::Manifold> folded;
  if (known == nullptr) {
    folded = store.rebuildManifold(cur);
  }
  zigzag::Manifold localM   = (known != nullptr) ? *known : folded.value();
  const auto varsDim        = getOrMakeDim(store, cur, localM, kDimVars);
  const auto valuesDim      = getOrMakeDim(store, cur, localM, kDimValues);
  const auto groupsDim      = getOrMakeDim(store, cur, localM, kDimGroups);
  const auto subgroupsDim   = getOrMakeDim(store, cur, localM, kDimSubgroups);
  const auto cloneDim       = getOrMakeDim(store, cur, localM, kDimClone);
  const auto notesDim       = getOrMakeDim(store, cur, localM, kDimNotes);
  const auto schemasDim     = getOrMakeDim(store, cur, localM, kDimSchemas);
  const auto altsDim        = getOrMakeDim(store, cur, localM, kDimAlternates);
  const auto defaultDim     = getOrMakeDim(store, cur, localM, kDimDefault);
  folded                    = localM;
  const zigzag::Manifold *m = &folded.value();
  const auto &reader        = static_cast<const SpanReader &>(store);

  // Check if master setting cell already exists along d.vars
  zigzag::CellRef masterCell    = zigzag::noCell;
  zigzag::CellRef lastMasterVar = store.homeCell();
  m->walkRank(store.homeCell(), varsDim, zigzag::DimVector::POS,
              [&](const zigzag::CellRef c) {
                if (c != store.homeCell()) {
                  if (m->textOf(c, reader) == spec.name) {
                    masterCell = c;
                    return false;
                  }
                  lastMasterVar = c;
                }
                return true;
              });

  if (masterCell != zigzag::noCell) {
    return cur;
  }

  // Parse group hierarchy tokens
  const auto tokens = splitTokens(spec.name, '.');
  const auto emptyGroupCell =
      m->linked(store.homeCell(), groupsDim, zigzag::DimVector::POS);

  zigzag::CellRef targetGroupCell = emptyGroupCell;
  if (tokens.size() > 1) {
    // Navigate or mint top group
    const auto &topGroupName     = tokens[0];
    zigzag::CellRef topGroupCell = zigzag::noCell;
    zigzag::CellRef lastTopGroup = emptyGroupCell;

    m->walkRank(emptyGroupCell, groupsDim, zigzag::DimVector::POS,
                [&](const zigzag::CellRef g) {
                  if (g != emptyGroupCell) {
                    if (m->textOf(g, reader) == topGroupName) {
                      topGroupCell = g;
                      return false;
                    }
                    lastTopGroup = g;
                  }
                  return true;
                });

    if (topGroupCell == zigzag::noCell) {
      cur          = store.makeCell(cur, topGroupName);
      topGroupCell = store.cellRefOf(cur);
      folded       = store.rebuildManifold(cur);
      m            = &folded.value();
      cur = store.setLink(cur, lastTopGroup, groupsDim, zigzag::DimVector::POS,
                          topGroupCell, m);
      folded = store.rebuildManifold(cur);
      m      = &folded.value();
    }

    // Subgroups
    zigzag::CellRef currentGroup = topGroupCell;
    for (std::size_t k = 1; k < tokens.size() - 1; ++k) {
      const auto &subName = tokens[k];
      const auto firstChild =
          m->linked(currentGroup, subgroupsDim, zigzag::DimVector::POS);

      zigzag::CellRef matchedSub = zigzag::noCell;
      zigzag::CellRef lastSub    = firstChild;

      if (firstChild != zigzag::noCell) {
        m->walkRank(firstChild, groupsDim, zigzag::DimVector::POS,
                    [&](const zigzag::CellRef s) {
                      if (m->textOf(s, reader) == subName) {
                        matchedSub = s;
                        return false;
                      }
                      lastSub = s;
                      return true;
                    });
      }

      if (matchedSub != zigzag::noCell) {
        currentGroup = matchedSub;
      } else {
        cur        = store.makeCell(cur, subName);
        matchedSub = store.cellRefOf(cur);
        folded     = store.rebuildManifold(cur);
        m          = &folded.value();
        if (firstChild == zigzag::noCell) {
          cur = store.setLink(cur, currentGroup, subgroupsDim,
                              zigzag::DimVector::POS, matchedSub, m);
        } else {
          cur = store.setLink(cur, lastSub, groupsDim, zigzag::DimVector::POS,
                              matchedSub, m);
        }
        folded       = store.rebuildManifold(cur);
        m            = &folded.value();
        currentGroup = matchedSub;
      }
    }
    targetGroupCell = currentGroup;
  }

  // Mint master setting cell on d.vars from home
  cur        = store.makeCell(cur, spec.name);
  masterCell = store.cellRefOf(cur);
  folded     = store.rebuildManifold(cur);
  m          = &folded.value();
  cur    = store.setLink(cur, lastMasterVar, varsDim, zigzag::DimVector::POS,
                         masterCell, m);
  folded = store.rebuildManifold(cur);
  m      = &folded.value();

  // Mint setting clone on d.vars from targetGroupCell
  zigzag::CellRef lastGroupVar = targetGroupCell;
  m->walkRank(targetGroupCell, varsDim, zigzag::DimVector::POS,
              [&](const zigzag::CellRef v) {
                lastGroupVar = v;
                return true;
              });

  const auto leafName = tokens.back();
  cur                 = store.makeCell(cur, leafName);
  const auto cloneRef = store.cellRefOf(cur);
  folded              = store.rebuildManifold(cur);
  m                   = &folded.value();
  cur    = store.setLink(cur, lastGroupVar, varsDim, zigzag::DimVector::POS,
                         cloneRef, m);
  folded = store.rebuildManifold(cur);
  m      = &folded.value();
  cur    = store.setLink(cur, masterCell, cloneDim, zigzag::DimVector::POS,
                         cloneRef, m);
  folded = store.rebuildManifold(cur);
  m      = &folded.value();

  // Notes
  if (!spec.notes.empty()) {
    cur                = store.makeCell(cur, spec.notes);
    const auto noteRef = store.cellRefOf(cur);
    folded             = store.rebuildManifold(cur);
    m                  = &folded.value();
    cur    = store.setLink(cur, masterCell, notesDim, zigzag::DimVector::POS,
                           noteRef, m);
    folded = store.rebuildManifold(cur);
    m      = &folded.value();
  }

  // Schemas and defaults
  const auto schemasToBuild =
      spec.schemas.empty()
          ? std::vector<SettingSchemaShape>{{{"string"}, {std::string{""}}}}
          : spec.schemas;

  zigzag::CellRef prevBlank = zigzag::noCell;
  for (std::size_t s = 0; s < schemasToBuild.size(); ++s) {
    const auto &shape   = schemasToBuild[s];
    cur                 = store.makeCell(cur, "");
    const auto blankRef = store.cellRefOf(cur);
    folded              = store.rebuildManifold(cur);
    m                   = &folded.value();

    if (s == 0) {
      cur = store.setLink(cur, masterCell, schemasDim, zigzag::DimVector::POS,
                          blankRef, m);
    } else {
      cur = store.setLink(cur, prevBlank, altsDim, zigzag::DimVector::POS,
                          blankRef, m);
    }
    folded    = store.rebuildManifold(cur);
    m         = &folded.value();
    prevBlank = blankRef;

    auto prevTypeClone = blankRef;
    for (std::size_t i = 0; i < shape.expectedTypes.size(); ++i) {
      const auto &typeName = shape.expectedTypes[i];
      cur                  = store.makeCell(cur, typeName);
      const auto typeRef   = store.cellRefOf(cur);
      folded               = store.rebuildManifold(cur);
      m                    = &folded.value();

      cur           = store.setLink(cur, prevTypeClone, schemasDim,
                                    zigzag::DimVector::POS, typeRef, m);
      folded        = store.rebuildManifold(cur);
      m             = &folded.value();
      prevTypeClone = typeRef;

      // Link to prototype type cell on d.clone
      const auto protoCell =
          findPrototypeCell(*m, schemasDim, typeName, reader);
      if (protoCell != zigzag::noCell) {
        cur    = store.setLink(cur, protoCell, cloneDim, zigzag::DimVector::POS,
                               typeRef, m);
        folded = store.rebuildManifold(cur);
        m      = &folded.value();
      }

      // Default value
      if (i < shape.defaultValues.size()) {
        zigzag::CellRef defValRef{zigzag::noCell};
        cur    = makeValueCell(store, cur, shape.defaultValues[i], defValRef);
        folded = store.rebuildManifold(cur);
        m      = &folded.value();
        cur    = store.setLink(cur, typeRef, defaultDim, zigzag::DimVector::POS,
                               defValRef, m);
        folded = store.rebuildManifold(cur);
        m      = &folded.value();
      }
    }
  }

  // Active values along d.values from master
  const auto &initialDefaults = schemasToBuild[0].defaultValues;
  auto prevVal                = masterCell;
  for (const auto &val : initialDefaults) {
    zigzag::CellRef valRef{zigzag::noCell};
    cur    = makeValueCell(store, cur, val, valRef);
    folded = store.rebuildManifold(cur);
    m      = &folded.value();
    cur = store.setLink(cur, prevVal, valuesDim, zigzag::DimVector::POS, valRef,
                        m);
    folded  = store.rebuildManifold(cur);
    m       = &folded.value();
    prevVal = valRef;
  }

  return cur;
}

MicroversionId ensureAllSettings(Store &store, const MicroversionId &parent,
                                 const SystemDocKind kind) {
  auto cur = parent.isZero() ? (store.currentVersions().empty()
                                    ? MicroversionId{}
                                    : store.currentVersions().front())
                             : parent;
  if (store.homeCell() == zigzag::noCell) {
    cur = initializeSystemStoreGenesis(store, kind, cur);
  }
  for (const auto &spec : defaultSettingSpecs(kind)) {
    cur = ensureSetting(store, cur, spec);
  }
  store.repointCurrentVersion(cur);
  return cur;
}

MicroversionId ensureAllSettings(Store &store, const SystemDocKind kind) {
  return ensureAllSettings(store, {}, kind);
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

  // Format links for Page 2 header
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

  // Format links for Page 3 header
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

  // Butterfly links
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

  // Slice genesis and populate standardized settings
  cur = initializeSystemStoreGenesis(store, kind, cur);
  cur = ensureAllSettings(store, cur, kind);

  store.repointCurrentVersion(cur);
  store.setVersionAnnotation(
      cur, {.alias       = "default",
            .description = "System default " + std::string(systemDocName(kind)),
            .tag         = "system",
            .timestamp   = ""});
}

void initializeSystemStoreFromSlice(Store &store, const SystemDocKind kind,
                                    const zigzag::ZzStructureDocument &slice) {
  zigzag::projectSystemSliceToStore(slice, store, kind);
}

// -----------------------------------------------------------------------------
// SystemStoreModel Implementation
// -----------------------------------------------------------------------------

SystemStoreModel SystemStoreModel::fromStore(const Store &store,
                                             const MicroversionId &version) {
  SystemStoreModel model;
  if (store.opCount() == 0 || store.homeCell() == zigzag::noCell) {
    model.isValid_ = false;
    model.error_   = "Store has no operations or home cell";
    return model;
  }
  const auto ver = version.isZero() ? store.primaryCurrentVersion() : version;
  const auto manifold = store.rebuildManifold(ver);
  const auto &reader  = static_cast<const SpanReader &>(store);

  model.storeDesc_ = manifold.textOf(store.homeCell(), reader);

  const auto varsDim      = manifold.dimensionNamed(kDimVars, reader);
  const auto valuesDim    = manifold.dimensionNamed(kDimValues, reader);
  const auto groupsDim    = manifold.dimensionNamed(kDimGroups, reader);
  const auto subgroupsDim = manifold.dimensionNamed(kDimSubgroups, reader);
  const auto notesDim     = manifold.dimensionNamed(kDimNotes, reader);
  const auto schemasDim   = manifold.dimensionNamed(kDimSchemas, reader);
  const auto altsDim      = manifold.dimensionNamed(kDimAlternates, reader);
  const auto defaultDim   = manifold.dimensionNamed(kDimDefault, reader);

  if (notesDim != zigzag::noCell) {
    const auto noteCell =
        manifold.linked(store.homeCell(), notesDim, zigzag::DimVector::POS);
    if (noteCell != zigzag::noCell) {
      model.storeDesc_ = manifold.textOf(noteCell, reader);
    }
  }

  // Read group hierarchy recursively
  std::function<SettingGroup(zigzag::CellRef)> readGroupNode =
      [&](const zigzag::CellRef gCell) -> SettingGroup {
    SettingGroup grp;
    grp.groupCell = gCell;
    grp.name      = manifold.textOf(gCell, reader);

    if (varsDim != zigzag::noCell) {
      manifold.walkRank(gCell, varsDim, zigzag::DimVector::POS,
                        [&](const zigzag::CellRef cloneCell) {
                          if (cloneCell != gCell) {
                            grp.memberSettingNames.push_back(
                                manifold.textOf(cloneCell, reader));
                          }
                        });
    }

    if (subgroupsDim != zigzag::noCell) {
      const auto firstChild =
          manifold.linked(gCell, subgroupsDim, zigzag::DimVector::POS);
      if (firstChild != zigzag::noCell) {
        manifold.walkRank(firstChild, groupsDim, zigzag::DimVector::POS,
                          [&](const zigzag::CellRef subCell) {
                            grp.childSubgroups.push_back(
                                readGroupNode(subCell));
                          });
      }
    }
    return grp;
  };

  if (groupsDim != zigzag::noCell) {
    manifold.walkRank(store.homeCell(), groupsDim, zigzag::DimVector::POS,
                      [&](const zigzag::CellRef gCell) {
                        if (gCell != store.homeCell()) {
                          model.groups_.push_back(readGroupNode(gCell));
                        }
                      });
  }

  // Read master settings along d.vars
  if (varsDim != zigzag::noCell) {
    manifold.walkRank(
        store.homeCell(), varsDim, zigzag::DimVector::POS,
        [&](const zigzag::CellRef setCell) {
          if (setCell == store.homeCell()) {
            return;
          }
          SettingEntry entry;
          entry.nameCell  = setCell;
          entry.name      = manifold.textOf(setCell, reader);
          entry.groupPath = splitTokens(entry.name, '.');
          if (!entry.groupPath.empty()) {
            entry.groupPath.pop_back();
          }

          // Notes
          if (notesDim != zigzag::noCell) {
            const auto noteCell =
                manifold.linked(setCell, notesDim, zigzag::DimVector::POS);
            if (noteCell != zigzag::noCell) {
              entry.notes = manifold.textOf(noteCell, reader);
            }
          }

          // Schemas & defaults
          if (schemasDim != zigzag::noCell) {
            const auto firstBlank =
                manifold.linked(setCell, schemasDim, zigzag::DimVector::POS);
            auto curBlank = firstBlank;
            while (curBlank != zigzag::noCell) {
              SettingSchemaShape shape;
              manifold.walkRank(
                  curBlank, schemasDim, zigzag::DimVector::POS,
                  [&](const zigzag::CellRef typeClone) {
                    if (typeClone != curBlank) {
                      shape.expectedTypes.push_back(
                          manifold.textOf(typeClone, reader));
                      if (defaultDim != zigzag::noCell) {
                        const auto defCell = manifold.linked(
                            typeClone, defaultDim, zigzag::DimVector::POS);
                        if (defCell != zigzag::noCell) {
                          if (manifold.valueKindOf(defCell) ==
                                  ValueKind::Double &&
                              manifold.asDouble(defCell)) {
                            shape.defaultValues.emplace_back(
                                *manifold.asDouble(defCell));
                          } else if (manifold.valueKindOf(defCell) ==
                                         ValueKind::Int64 &&
                                     manifold.asInt64(defCell)) {
                            shape.defaultValues.emplace_back(
                                *manifold.asInt64(defCell));
                          } else if (manifold.valueKindOf(defCell) ==
                                         ValueKind::Bool &&
                                     manifold.asBool(defCell)) {
                            shape.defaultValues.emplace_back(
                                *manifold.asBool(defCell));
                          } else {
                            shape.defaultValues.emplace_back(
                                manifold.textOf(defCell, reader));
                          }
                        }
                      }
                    }
                  });
              entry.schema.alternatives.push_back(std::move(shape));
              if (altsDim != zigzag::noCell) {
                curBlank =
                    manifold.linked(curBlank, altsDim, zigzag::DimVector::POS);
              } else {
                break;
              }
            }
          }

          // Active values along d.values
          if (valuesDim != zigzag::noCell) {
            manifold.walkRank(setCell, valuesDim, zigzag::DimVector::POS,
                              [&](const zigzag::CellRef valCell) {
                                if (valCell != setCell) {
                                  entry.value.valueCells.push_back(valCell);
                                  if (manifold.valueKindOf(valCell) ==
                                          ValueKind::Double &&
                                      manifold.asDouble(valCell)) {
                                    entry.value.elements.emplace_back(
                                        *manifold.asDouble(valCell));
                                  } else if (manifold.valueKindOf(valCell) ==
                                                 ValueKind::Int64 &&
                                             manifold.asInt64(valCell)) {
                                    entry.value.elements.emplace_back(
                                        *manifold.asInt64(valCell));
                                  } else if (manifold.valueKindOf(valCell) ==
                                                 ValueKind::Bool &&
                                             manifold.asBool(valCell)) {
                                    entry.value.elements.emplace_back(
                                        *manifold.asBool(valCell));
                                  } else {
                                    entry.value.elements.emplace_back(
                                        manifold.textOf(valCell, reader));
                                  }
                                }
                              });
          }

          // Validate
          std::string err;
          if (!validate(entry.schema, entry.value, &err)) {
            entry.isValid         = false;
            entry.validationError = err;
            model.isValid_        = false;
            if (model.error_.empty()) {
              model.error_ = "Setting '" + entry.name + "': " + err;
            }
          }

          model.settings_.push_back(std::move(entry));
        });
  }

  return model;
}

const SettingEntry *
SystemStoreModel::find(const std::string_view name) const noexcept {
  for (const auto &s : settings_) {
    if (s.name == name) {
      return &s;
    }
  }
  return nullptr;
}

double SystemStoreModel::getDouble(const std::string_view name,
                                   const double fallback) const {
  const auto *const entry = find(name);
  return entry != nullptr ? entry->value.asDouble(0, fallback) : fallback;
}

std::int64_t SystemStoreModel::getInt64(const std::string_view name,
                                        const std::int64_t fallback) const {
  const auto *const entry = find(name);
  return entry != nullptr ? entry->value.asInt64(0, fallback) : fallback;
}

bool SystemStoreModel::getBool(const std::string_view name,
                               const bool fallback) const {
  const auto *const entry = find(name);
  return entry != nullptr ? entry->value.asBool(0, fallback) : fallback;
}

std::string SystemStoreModel::getString(const std::string_view name,
                                        const std::string_view fallback) const {
  const auto *const entry = find(name);
  return entry != nullptr ? entry->value.asString(0, fallback)
                          : std::string{fallback};
}

std::vector<CellValue>
SystemStoreModel::getValues(const std::string_view name) const {
  const auto *const entry = find(name);
  return entry != nullptr ? entry->value.elements : std::vector<CellValue>{};
}

std::vector<double>
SystemStoreModel::getDoubleList(const std::string_view name) const {
  std::vector<double> result;
  const auto *const entry = find(name);
  if (entry != nullptr) {
    for (std::size_t i = 0; i < entry->value.elements.size(); ++i) {
      result.push_back(entry->value.asDouble(i));
    }
  }
  return result;
}

std::vector<std::int64_t>
SystemStoreModel::getInt64List(const std::string_view name) const {
  std::vector<std::int64_t> result;
  const auto *const entry = find(name);
  if (entry != nullptr) {
    for (std::size_t i = 0; i < entry->value.elements.size(); ++i) {
      result.push_back(entry->value.asInt64(i));
    }
  }
  return result;
}

std::vector<std::string>
SystemStoreModel::getStringList(const std::string_view name) const {
  std::vector<std::string> result;
  const auto *const entry = find(name);
  if (entry != nullptr) {
    for (std::size_t i = 0; i < entry->value.elements.size(); ++i) {
      result.push_back(entry->value.asString(i));
    }
  }
  return result;
}

bool SystemStoreModel::validate(const SettingSchema &schema,
                                const std::span<const CellValue> values,
                                std::string *const errOut) {
  if (schema.alternatives.empty()) {
    return true;
  }
  std::string lastErr;
  for (const auto &alt : schema.alternatives) {
    if (matchShape(alt, values, &lastErr)) {
      return true;
    }
  }
  if (errOut != nullptr) {
    *errOut = "No schema alternative matched (" + lastErr + ")";
  }
  return false;
}

bool SystemStoreModel::validate(const SettingSchema &schema,
                                const SettingValue &val,
                                std::string *const errOut) {
  return validate(schema, std::span<const CellValue>{val.elements}, errOut);
}

MicroversionId
SystemStoreModel::updateSetting(Store &store, const MicroversionId &parent,
                                const std::string_view name,
                                const std::span<const CellValue> values,
                                const zigzag::Manifold *const known) {
  const auto curVer = parent.isZero() ? store.primaryCurrentVersion() : parent;
  const auto model  = fromStore(store, curVer);
  const auto *const entry = model.find(name);
  if (entry == nullptr) {
    throw std::invalid_argument("Setting '" + std::string(name) +
                                "' not found in store");
  }

  std::string err;
  if (!validate(entry->schema, values, &err)) {
    throw std::invalid_argument("Schema violation for setting '" +
                                std::string(name) + "': " + err);
  }

  std::optional<zigzag::Manifold> folded;
  if (known == nullptr) {
    folded = store.rebuildManifold(curVer);
  }
  const zigzag::Manifold *m = (known != nullptr) ? known : &folded.value();
  const auto &reader        = static_cast<const SpanReader &>(store);
  const auto valuesDim      = m->dimensionNamed(kDimValues, reader);

  auto cur = curVer;
  if (entry->value.valueCells.size() == values.size()) {
    for (std::size_t i = 0; i < values.size(); ++i) {
      const auto c  = entry->value.valueCells[i];
      const auto &v = values[i];
      if (std::holds_alternative<double>(v)) {
        cur = store.setScalar(cur, c, std::get<double>(v), m);
      } else if (std::holds_alternative<std::int64_t>(v)) {
        cur = store.setScalar(cur, c, std::get<std::int64_t>(v), m);
      } else if (std::holds_alternative<bool>(v)) {
        cur = store.setScalar(cur, c, std::get<bool>(v), m);
      } else {
        cur = store.setCellText(cur, c, std::get<std::string>(v), m);
      }
      folded = store.rebuildManifold(cur);
      m      = &folded.value();
    }
  } else {
    // Relink rank of values
    auto prev = entry->nameCell;
    for (const auto &v : values) {
      zigzag::CellRef valRef{zigzag::noCell};
      cur    = makeValueCell(store, cur, v, valRef);
      folded = store.rebuildManifold(cur);
      m      = &folded.value();
      cur = store.setLink(cur, prev, valuesDim, zigzag::DimVector::POS, valRef,
                          m);
      folded = store.rebuildManifold(cur);
      m      = &folded.value();
      prev   = valRef;
    }
  }

  return cur;
}

MicroversionId
SystemStoreModel::resetToDefault(Store &store, const MicroversionId &parent,
                                 const std::string_view name,
                                 const zigzag::Manifold *const known) {
  const auto curVer = parent.isZero() ? store.primaryCurrentVersion() : parent;
  const auto model  = fromStore(store, curVer);
  const auto *const entry = model.find(name);
  if (entry == nullptr) {
    throw std::invalid_argument("Setting '" + std::string(name) +
                                "' not found in store");
  }
  if (entry->schema.alternatives.empty() ||
      entry->schema.alternatives[0].defaultValues.empty()) {
    return curVer;
  }
  return updateSetting(store, curVer, name,
                       entry->schema.alternatives[0].defaultValues, known);
}

std::vector<CellValue> getSetting(const Store &store,
                                  const std::string_view name) {
  const auto model = SystemStoreModel::fromStore(store);
  return model.getValues(name);
}

MicroversionId setSetting(Store &store, const MicroversionId &parent,
                          const std::string_view name,
                          const std::span<const CellValue> values,
                          const zigzag::Manifold *const known) {
  return SystemStoreModel::updateSetting(store, parent, name, values, known);
}

MicroversionId resetSettingToDefault(Store &store, const MicroversionId &parent,
                                     const std::string_view name,
                                     const zigzag::Manifold *const known) {
  return SystemStoreModel::resetToDefault(store, parent, name, known);
}

// -----------------------------------------------------------------------------
// Config Struct fromStore Loaders
// -----------------------------------------------------------------------------

KeymapConfig KeymapConfig::fromStore(const Store &store) {
  KeymapConfig cfg;
  if (store.opCount() == 0 || store.homeCell() == zigzag::noCell) {
    return cfg;
  }
  const auto model = SystemStoreModel::fromStore(store);
  for (const auto &s : model.settings()) {
    cfg.bindings.emplace_back(s.name, s.value.asString(0));
  }
  return cfg;
}

SettingsConfig SettingsConfig::fromStore(const Store &store) {
  SettingsConfig cfg;
  if (store.opCount() == 0 || store.homeCell() == zigzag::noCell) {
    return cfg;
  }
  const auto model = SystemStoreModel::fromStore(store);
  cfg.fontSize     = static_cast<float>(
      model.getDouble(settings::kFontSize, static_cast<double>(cfg.fontSize)));
  cfg.fontFamily      = model.getString(settings::kFontFamily, cfg.fontFamily);
  cfg.lineHeight      = static_cast<float>(model.getDouble(
      settings::kLineHeight, static_cast<double>(cfg.lineHeight)));
  cfg.autoSaveSeconds = static_cast<std::uint32_t>(
      model.getInt64(settings::kAutoSaveSeconds,
                     static_cast<std::int64_t>(cfg.autoSaveSeconds)));
  cfg.theme        = model.getString(settings::kTheme, cfg.theme);
  const auto bgRgb = model.getDoubleList(settings::kThemeBackground);
  if (bgRgb.size() == 3) {
    cfg.themeBackgroundRgb = bgRgb;
  }
  return cfg;
}

LayoutConfig LayoutConfig::fromStore(const Store &store) {
  LayoutConfig cfg;
  if (store.opCount() == 0 || store.homeCell() == zigzag::noCell) {
    return cfg;
  }
  const auto model = SystemStoreModel::fromStore(store);

  cfg.columns      = static_cast<std::uint32_t>(model.getInt64(
      settings::kColumns, static_cast<std::int64_t>(cfg.columns)));
  cfg.pageWidthPx  = static_cast<float>(model.getDouble(
      settings::kPageWidthPx, static_cast<double>(cfg.pageWidthPx)));
  cfg.pageHeightPx = static_cast<float>(model.getDouble(
      settings::kPageHeightPx, static_cast<double>(cfg.pageHeightPx)));
  cfg.transclusionPrisms =
      model.getBool(settings::kTransclusionPrisms, cfg.transclusionPrisms);
  cfg.transclusionLoom =
      model.getBool(settings::kTransclusionLoom, cfg.transclusionLoom);
  cfg.xanalinkRibbons =
      model.getBool(settings::kXanalinkRibbons, cfg.xanalinkRibbons);

  const auto dockStr = model.getString(settings::kPouchDock, "right");
  cfg.pouchDock      = (dockStr == "left") ? PouchDock::Left : PouchDock::Right;

  cfg.physics.kRepel           = static_cast<float>(model.getDouble(
      settings::kPhysicsKRepel, static_cast<double>(cfg.physics.kRepel)));
  cfg.physics.kPlane           = static_cast<float>(model.getDouble(
      settings::kPhysicsKPlane, static_cast<double>(cfg.physics.kPlane)));
  cfg.physics.kAlign           = static_cast<float>(model.getDouble(
      settings::kPhysicsKAlign, static_cast<double>(cfg.physics.kAlign)));
  cfg.physics.kTier            = static_cast<float>(model.getDouble(
      settings::kPhysicsKTier, static_cast<double>(cfg.physics.kTier)));
  cfg.physics.kDamping         = static_cast<float>(model.getDouble(
      settings::kPhysicsKDamping, static_cast<double>(cfg.physics.kDamping)));
  cfg.physics.backgroundDepthZ = static_cast<float>(
      model.getDouble(settings::kPhysicsBackgroundDepthZ,
                      static_cast<double>(cfg.physics.backgroundDepthZ)));
  cfg.physics.defaultGap = static_cast<float>(
      model.getDouble(settings::kPhysicsDefaultGap,
                      static_cast<double>(cfg.physics.defaultGap)));
  cfg.physics.settleVelocityThreshold = static_cast<float>(model.getDouble(
      settings::kPhysicsSettleVelocityThreshold,
      static_cast<double>(cfg.physics.settleVelocityThreshold)));
  cfg.physics.maxForce                = static_cast<float>(model.getDouble(
      settings::kPhysicsMaxForce, static_cast<double>(cfg.physics.maxForce)));
  cfg.physics.maxVelocity             = static_cast<float>(
      model.getDouble(settings::kPhysicsMaxVelocity,
                      static_cast<double>(cfg.physics.maxVelocity)));
  cfg.physics.timeStep = static_cast<float>(model.getDouble(
      settings::kPhysicsTimeStep, static_cast<double>(cfg.physics.timeStep)));

  cfg.beams.bandStrandLimit = static_cast<std::uint32_t>(
      model.getInt64(settings::kBeamsBandStrandLimit,
                     static_cast<std::int64_t>(cfg.beams.bandStrandLimit)));
  cfg.beams.bandStrandPitch = static_cast<float>(
      model.getDouble(settings::kBeamsBandStrandPitch,
                      static_cast<double>(cfg.beams.bandStrandPitch)));
  cfg.beams.bandFillAlpha = static_cast<float>(
      model.getDouble(settings::kBeamsBandFillAlpha,
                      static_cast<double>(cfg.beams.bandFillAlpha)));
  cfg.beams.stubWidthOfBeam = static_cast<float>(
      model.getDouble(settings::kBeamsStubWidthOfBeam,
                      static_cast<double>(cfg.beams.stubWidthOfBeam)));
  cfg.beams.stubMinOfLine = static_cast<float>(
      model.getDouble(settings::kBeamsStubMinOfLine,
                      static_cast<double>(cfg.beams.stubMinOfLine)));
  cfg.beams.marginKerf        = static_cast<float>(model.getDouble(
      settings::kBeamsMarginKerf, static_cast<double>(cfg.beams.marginKerf)));
  cfg.beams.bypassDepthPerDoc = static_cast<float>(
      model.getDouble(settings::kBeamsBypassDepthPerDoc,
                      static_cast<double>(cfg.beams.bypassDepthPerDoc)));
  cfg.beams.bypassDepthLimit = static_cast<float>(
      model.getDouble(settings::kBeamsBypassDepthLimit,
                      static_cast<double>(cfg.beams.bypassDepthLimit)));
  cfg.beams.bypassSegments = static_cast<std::uint32_t>(
      model.getInt64(settings::kBeamsBypassSegments,
                     static_cast<std::int64_t>(cfg.beams.bypassSegments)));
  cfg.beams.loomBundlingEnabled = model.getBool(
      settings::kBeamsLoomBundlingEnabled, cfg.beams.loomBundlingEnabled);
  cfg.beams.loomAlpha      = static_cast<float>(model.getDouble(
      settings::kBeamsLoomAlpha, static_cast<double>(cfg.beams.loomAlpha)));
  cfg.beams.loomHoverAlpha = static_cast<float>(
      model.getDouble(settings::kBeamsLoomHoverAlpha,
                      static_cast<double>(cfg.beams.loomHoverAlpha)));

  cfg.zigzag.cellHorizontalPaddingPx = static_cast<float>(
      model.getDouble(settings::kZigzagCellHorizontalPaddingPx,
                      static_cast<double>(cfg.zigzag.cellHorizontalPaddingPx)));
  cfg.zigzag.cellVerticalPaddingPx = static_cast<float>(
      model.getDouble(settings::kZigzagCellVerticalPaddingPx,
                      static_cast<double>(cfg.zigzag.cellVerticalPaddingPx)));
  cfg.zigzag.cellBandGapPx = static_cast<float>(
      model.getDouble(settings::kZigzagCellBandGapPx,
                      static_cast<double>(cfg.zigzag.cellBandGapPx)));
  cfg.zigzag.contentMaxWidthPx = static_cast<float>(
      model.getDouble(settings::kZigzagContentMaxWidthPx,
                      static_cast<double>(cfg.zigzag.contentMaxWidthPx)));
  cfg.zigzag.topologyMaxWidthPx = static_cast<float>(
      model.getDouble(settings::kZigzagTopologyMaxWidthPx,
                      static_cast<double>(cfg.zigzag.topologyMaxWidthPx)));
  cfg.zigzag.rankClearancePx = static_cast<float>(
      model.getDouble(settings::kZigzagRankClearancePx,
                      static_cast<double>(cfg.zigzag.rankClearancePx)));
  cfg.zigzag.hudHorizontalPaddingPx = static_cast<float>(
      model.getDouble(settings::kZigzagHudHorizontalPaddingPx,
                      static_cast<double>(cfg.zigzag.hudHorizontalPaddingPx)));
  cfg.zigzag.hudVerticalPaddingPx = static_cast<float>(
      model.getDouble(settings::kZigzagHudVerticalPaddingPx,
                      static_cast<double>(cfg.zigzag.hudVerticalPaddingPx)));
  cfg.zigzag.hudColumnGapPx = static_cast<float>(
      model.getDouble(settings::kZigzagHudColumnGapPx,
                      static_cast<double>(cfg.zigzag.hudColumnGapPx)));
  cfg.zigzag.connectionBeamWidthPx = static_cast<float>(
      model.getDouble(settings::kZigzagConnectionBeamWidthPx,
                      static_cast<double>(cfg.zigzag.connectionBeamWidthPx)));

  return cfg;
}

namespace {
gleditor::RadialConfig createDefaultRadialConfig() {
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
  return cfg;
}
} // namespace

UIConfig::UIConfig() : radialMenu(createDefaultRadialConfig()) {}

UIConfig UIConfig::fromStore(const Store &store) {
  UIConfig cfg;
  if (store.opCount() == 0 || store.homeCell() == zigzag::noCell) {
    return cfg;
  }
  const auto model = SystemStoreModel::fromStore(store);
  cfg.tabBarVisible =
      model.getBool(settings::kTabBarVisible, cfg.tabBarVisible);
  cfg.statusBarVisible =
      model.getBool(settings::kStatusBarVisible, cfg.statusBarVisible);
  cfg.hypertimeMapVisible =
      model.getBool(settings::kHypertimeMapVisible, cfg.hypertimeMapVisible);
  cfg.notificationPosition   = model.getString(settings::kNotificationPosition,
                                               cfg.notificationPosition);
  cfg.notificationDurationMs = static_cast<std::uint32_t>(
      model.getInt64(settings::kNotificationDurationMs,
                     static_cast<std::int64_t>(cfg.notificationDurationMs)));
  cfg.radialMenu.radius      = static_cast<float>(model.getDouble(
      settings::kRadialMenuRadius, static_cast<double>(cfg.radialMenu.radius)));
  cfg.radialMenu.innerRadius = static_cast<float>(
      model.getDouble(settings::kRadialMenuInnerRadius,
                      static_cast<double>(cfg.radialMenu.innerRadius)));
  return cfg;
}

PouchConfig PouchConfig::fromStore(const Store &store) {
  PouchConfig cfg;
  if (store.opCount() == 0 || store.homeCell() == zigzag::noCell) {
    return cfg;
  }
  const auto model = SystemStoreModel::fromStore(store);
  for (const auto &s : model.settings()) {
    if (s.name.starts_with("zone.")) {
      DropZoneSpec spec;
      spec.id    = s.name.substr(5);
      spec.label = s.value.asString(0, spec.id);
      if (s.value.elements.size() > 1) {
        spec.auraColor = static_cast<std::uint32_t>(s.value.asInt64(1));
      }
      if (s.value.elements.size() > 2) {
        spec.heightWeight = static_cast<float>(s.value.asDouble(2, 1.0));
      }
      cfg.zones.push_back(std::move(spec));
    }
  }
  if (cfg.zones.empty()) {
    cfg.zones = {
        DropZoneSpec{.id           = "to_link_left",
                     .label        = "To Link (Left)",
                     .auraColor    = 0x06B6D4FFU,
                     .heightWeight = 1.0F},
        DropZoneSpec{.id           = "to_link_right",
                     .label        = "To Link (Right)",
                     .auraColor    = 0xEC4899FFU,
                     .heightWeight = 1.0F},
        DropZoneSpec{.id           = "notes",
                     .label        = "Notes",
                     .auraColor    = 0xEAB308FFU,
                     .heightWeight = 1.0F},
        DropZoneSpec{.id           = "scratch",
                     .label        = "Scratch",
                     .auraColor    = 0x10B981FFU,
                     .heightWeight = 1.0F},
    };
  }
  return cfg;
}

} // namespace xanadu

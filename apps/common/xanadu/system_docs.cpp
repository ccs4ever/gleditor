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
#include <gleditor/color.hpp>

namespace xanadu {

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
           "  zFightJitterAmplitude: Deterministic small Z nudge (+/-), "
           "derived from each beam's own link or tag id, so beams that "
           "cross in screen space rarely land at exactly the same depth. "
           "Default is 0.6.\n"
           "  activeZBoost: Z boost added to the active or selected link's "
           "beam, kept well clear of zFightJitterAmplitude so the selected "
           "beam always renders in front of a crossing one. Default is "
           "4.0.\n"
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

// A zero parent means "start from whatever the store already points at, or
// genesis if it has no current version yet" -- the shared fallback every
// ensure*() entry point needs before it can decide whether genesis has to
// run first.
MicroversionId resolveStartingVersion(const Store &store,
                                      const MicroversionId &parent) {
  if (!parent.isZero()) {
    return parent;
  }
  if (store.currentVersions().empty()) {
    return MicroversionId{};
  }
  return store.currentVersions().front();
}

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
        {.name    = std::string(settings::kColumns),
         .notes   = "Number of document columns in layout",
         .schemas = {{.expectedTypes = {"integer"},
                      .defaultValues = {std::int64_t{2}}}}},
        {.name    = std::string(settings::kPageWidthPx),
         .notes   = "Width of each virtual page in pixels",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {800.0}}}},
        {.name    = std::string(settings::kPageHeightPx),
         .notes   = "Height of each virtual page in pixels",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {1000.0}}}},
        {.name    = std::string(settings::kTransclusionPrisms),
         .notes   = "Render 3D prisms for transclusion bridges",
         .schemas = {{.expectedTypes = {"bool"}, .defaultValues = {true}}}},
        {.name    = std::string(settings::kTransclusionLoom),
         .notes   = "Enable loom view bundling for transclusions",
         .schemas = {{.expectedTypes = {"bool"}, .defaultValues = {true}}}},
        {.name    = std::string(settings::kXanalinkRibbons),
         .notes   = "Render curved ribbon links between xanadocs",
         .schemas = {{.expectedTypes = {"bool"}, .defaultValues = {true}}}},
        {.name    = std::string(settings::kPouchDock),
         .notes   = "Docking edge for pouch drawer ('left' or 'right')",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"right"}}}}},
        {.name    = std::string(settings::kPouchWidthPx),
         .notes   = "Width of pouch drawer in pixels",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {360.0}}}},
        {.name    = std::string(settings::kPhysicsKRepel),
         .notes   = "Repulsion spring stiffness",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {4500.0}}}},
        {.name    = std::string(settings::kPhysicsKPlane),
         .notes   = "In-plane alignment spring stiffness",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {14.0}}}},
        {.name    = std::string(settings::kPhysicsKAlign),
         .notes   = "Inter-page angle alignment stiffness",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {28.0}}}},
        {.name    = std::string(settings::kPhysicsKTier),
         .notes   = "Prominence tier spring stiffness",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {12.0}}}},
        {.name    = std::string(settings::kPhysicsKDamping),
         .notes   = "Velocity damping coefficient",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {7.5}}}},
        {.name    = std::string(settings::kPhysicsBackgroundDepthZ),
         .notes   = "Z-depth offset for background pages",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {-40.0}}}},
        {.name    = std::string(settings::kPhysicsDefaultGap),
         .notes   = "Default gap between pages in pixels",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {8.0}}}},
        {.name    = std::string(settings::kPhysicsSettleVelocityThreshold),
         .notes   = "Velocity cutoff for physics settle",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {0.02}}}},
        {.name    = std::string(settings::kPhysicsMaxForce),
         .notes   = "Maximum instantaneous physical force clamp",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {10000.0}}}},
        {.name    = std::string(settings::kPhysicsMaxVelocity),
         .notes   = "Maximum linear velocity clamp",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {1000.0}}}},
        {.name    = std::string(settings::kPhysicsTimeStep),
         .notes   = "Physics simulation time delta in seconds",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {0.016}}}},
        {.name    = std::string(settings::kBeamsBandStrandLimit),
         .notes   = "Max strands in a ribbon band",
         .schemas = {{.expectedTypes = {"integer"},
                      .defaultValues = {std::int64_t{7}}}}},
        {.name    = std::string(settings::kBeamsBandStrandPitch),
         .notes   = "Spacing between strands in beam widths",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {2.2}}}},
        {.name    = std::string(settings::kBeamsBandFillAlpha),
         .notes   = "Opacity for band fill ribbons",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {0.85}}}},
        {.name    = std::string(settings::kBeamsStubWidthOfBeam),
         .notes   = "Stub width factor relative to beam",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {1.35}}}},
        {.name    = std::string(settings::kBeamsStubMinOfLine),
         .notes   = "Minimum stub length in line heights",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {0.9}}}},
        {.name    = std::string(settings::kBeamsMarginKerf),
         .notes   = "Kerf margin fraction",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {0.04}}}},
        {.name    = std::string(settings::kBeamsBypassDepthPerDoc),
         .notes   = "Z-depth step for bypass routing",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {-20.0}}}},
        {.name    = std::string(settings::kBeamsBypassDepthLimit),
         .notes   = "Max Z-depth limit for bypasses",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {-120.0}}}},
        {.name    = std::string(settings::kBeamsBypassSegments),
         .notes   = "Spline subdivisions for bypass curves",
         .schemas = {{.expectedTypes = {"integer"},
                      .defaultValues = {std::int64_t{9}}}}},
        {.name    = std::string(settings::kBeamsLoomBundlingEnabled),
         .notes   = "Whether loom cables bundle together",
         .schemas = {{.expectedTypes = {"bool"}, .defaultValues = {true}}}},
        {.name    = std::string(settings::kBeamsLoomAlpha),
         .notes   = "Base opacity for loom cables",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {0.35}}}},
        {.name    = std::string(settings::kBeamsLoomHoverAlpha),
         .notes   = "Hovered opacity for loom cables",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {1.0}}}},
        {.name = std::string(settings::kBeamsZFightJitterAmplitude),
         .notes =
             "Deterministic per-beam Z nudge amplitude to reduce z-fighting "
             "between crossing beams",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {0.6}}}},
        {.name    = std::string(settings::kBeamsActiveZBoost),
         .notes   = "Z boost for the active/selected link's beam",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {4.0}}}},
        {.name    = std::string(settings::kZigzagCellHorizontalPaddingPx),
         .notes   = "Horizontal cell padding in px",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {8.0}}}},
        {.name    = std::string(settings::kZigzagCellVerticalPaddingPx),
         .notes   = "Vertical cell padding in px",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {6.0}}}},
        {.name    = std::string(settings::kZigzagCellBandGapPx),
         .notes   = "Gap between cell bands in pixels",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {4.0}}}},
        {.name    = std::string(settings::kZigzagContentMaxWidthPx),
         .notes   = "Max width in content projection",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {260.0}}}},
        {.name    = std::string(settings::kZigzagTopologyMaxWidthPx),
         .notes   = "Max width in topology projection",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {140.0}}}},
        {.name    = std::string(settings::kZigzagRankClearancePx),
         .notes   = "Clearance between perpendicular ranks",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {24.0}}}},
        {.name    = std::string(settings::kZigzagHudHorizontalPaddingPx),
         .notes   = "HUD horizontal padding",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {16.0}}}},
        {.name    = std::string(settings::kZigzagHudVerticalPaddingPx),
         .notes   = "HUD vertical padding",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {8.0}}}},
        {.name    = std::string(settings::kZigzagHudColumnGapPx),
         .notes   = "HUD gap between columns",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {8.0}}}},
        {.name    = std::string(settings::kZigzagConnectionBeamWidthPx),
         .notes   = "Connection beam line width",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {4.0}}}},
        {.name    = std::string(settings::kBridgeCellRadius),
         .notes   = "Discovery and visual neighborhood cell radius",
         .schemas = {{.expectedTypes = {"integer"},
                      .defaultValues = {std::int64_t{3}}}}},
        {.name    = std::string(settings::kBridgeBackgroundDepthZ),
         .notes   = "Background continuum depth Z",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {-40.0}}}},
        {.name    = std::string(settings::kBridgeBackgroundOpacity),
         .notes   = "Background continuum opacity",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {0.85}}}},
        {.name    = std::string(settings::kBridgeSatelloidAlignment),
         .notes   = "Collinear satelloid alignment enabled",
         .schemas = {{.expectedTypes = {"bool"}, .defaultValues = {true}}}},
        {.name    = std::string(settings::kBridgeSatelloidTether),
         .notes   = "Tenuous parent tether enabled",
         .schemas = {{.expectedTypes = {"bool"}, .defaultValues = {true}}}},
        {.name    = std::string(settings::kBridgeSatelloidMass),
         .notes   = "Satelloid physical mass",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {1.0}}}},
        {.name    = std::string(settings::kBridgeSatelloidGap),
         .notes   = "Satelloid clearance gap in px",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {12.0}}}},
        {.name    = std::string(settings::kBridgeSatelloidWidth),
         .notes   = "Satelloid default cell width in px",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {24.0}}}},
        {.name    = std::string(settings::kBridgeSatelloidHeight),
         .notes   = "Satelloid default cell height in px",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {14.0}}}},
        {.name    = std::string(settings::kBridgeTetherControlDepth),
         .notes   = "Tether curve control depth Z",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {-20.0}}}},
        {.name    = std::string(settings::kBridgeTetherSegments),
         .notes   = "Tether curve tessellation segments",
         .schemas = {{.expectedTypes = {"integer"},
                      .defaultValues = {std::int64_t{16}}}}},
        {.name    = std::string(settings::kBridgeTetherDepthThreshold),
         .notes   = "Tether depth activation threshold",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {-5.0}}}},
        {.name    = std::string(settings::kBridgeTetherColour),
         .notes   = "Tether RGBA hexadecimal colour",
         .schemas = {{.expectedTypes = {"integer"},
                      .defaultValues = {static_cast<std::int64_t>(
                          0x38BDF844U)}}}},
        {.name    = std::string(settings::kBridgeLoomBundling),
         .notes   = "Transclusion loom volumetric ribbon bundling enabled",
         .schemas = {{.expectedTypes = {"bool"}, .defaultValues = {true}}}},
        {.name    = std::string(settings::kBridgeLoomAlpha),
         .notes   = "Transclusion loom ribbon base alpha",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {0.35}}}},
        {.name    = std::string(settings::kBridgeLoomHoverAlpha),
         .notes   = "Transclusion loom ribbon hover alpha",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {1.0}}}},
    };
    break;

  case SystemDocKind::Settings:
    specs = {
        {.name    = std::string(settings::kFontSize),
         .notes   = "Base text point size",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {16.0}}}},
        {.name    = std::string(settings::kFontFamily),
         .notes   = "Default typeface family name",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Monospace"}}}}},
        {.name    = std::string(settings::kLineHeight),
         .notes   = "Line spacing multiplier",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {1.4}}}},
        {.name    = std::string(settings::kAutoSaveSeconds),
         .notes   = "Interval in seconds between auto-saves",
         .schemas = {{.expectedTypes = {"integer"},
                      .defaultValues = {std::int64_t{5}}}}},
        {.name    = std::string(settings::kTheme),
         .notes   = "Active UI theme name",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"system"}}}}},
        {.name  = std::string(settings::kThemeBackground),
         .notes = "Theme background color (RGB as 3 floats 0..1 or 3 integers "
                  "0..255)",
         .schemas = {{.expectedTypes = {"float", "float", "float"},
                      .defaultValues = {0.1, 0.1, 0.1}},
                     {.expectedTypes = {"integer", "integer", "integer"},
                      .defaultValues = {std::int64_t{26}, std::int64_t{26},
                                        std::int64_t{26}}}}},
    };
    break;

  case SystemDocKind::UI:
    specs = {
        {.name    = std::string(settings::kTabBarVisible),
         .notes   = "Visibility of the document tab switcher bar",
         .schemas = {{.expectedTypes = {"bool"}, .defaultValues = {true}}}},
        {.name    = std::string(settings::kStatusBarVisible),
         .notes   = "Visibility of the status bar",
         .schemas = {{.expectedTypes = {"bool"}, .defaultValues = {true}}}},
        {.name    = std::string(settings::kHypertimeMapVisible),
         .notes   = "Visibility of the hypertime map",
         .schemas = {{.expectedTypes = {"bool"}, .defaultValues = {false}}}},
        {.name    = std::string(settings::kNotificationPosition),
         .notes   = "Screen position for toast notifications",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"top-right"}}}}},
        {.name    = std::string(settings::kNotificationDurationMs),
         .notes   = "Toast notification duration in milliseconds",
         .schemas = {{.expectedTypes = {"integer"},
                      .defaultValues = {std::int64_t{3000}}}}},
        {.name    = std::string(settings::kRadialMenuRadius),
         .notes   = "Outer radius of radial context menu in pixels",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {130.0}}}},
        {.name    = std::string(settings::kRadialMenuInnerRadius),
         .notes   = "Inner deadzone radius of radial menu in pixels",
         .schemas = {{.expectedTypes = {"float"}, .defaultValues = {42.0}}}},
    };
    break;

  case SystemDocKind::Keymap:
    specs = {
        // Xudu Core Actions
        {.name    = std::string(settings::kKeymapQuit),
         .notes   = "Shortcut to save and close the application",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+Q"}}}}},
        {.name    = std::string(settings::kKeymapSave),
         .notes   = "Shortcut to save or preserve active document",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+S"}}}}},
        {.name    = std::string(settings::kKeymapClose),
         .notes   = "Shortcut to close the active document",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+W"}}}}},
        {.name    = std::string(settings::kKeymapNextDoc),
         .notes   = "Shortcut to switch to next document",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+Tab"}}}}},
        {.name    = std::string(settings::kKeymapPrevDoc),
         .notes   = "Shortcut to switch to previous document",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+Shift+Tab"}}}}},
        {.name    = std::string(settings::kKeymapDoc1),
         .notes   = "Shortcut to switch to document 1",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+1"}}}}},
        {.name    = std::string(settings::kKeymapDoc2),
         .notes   = "Shortcut to switch to document 2",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+2"}}}}},
        {.name    = std::string(settings::kKeymapDoc3),
         .notes   = "Shortcut to switch to document 3",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+3"}}}}},
        {.name    = std::string(settings::kKeymapDoc4),
         .notes   = "Shortcut to switch to document 4",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+4"}}}}},
        {.name    = std::string(settings::kKeymapDoc5),
         .notes   = "Shortcut to switch to document 5",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+5"}}}}},
        {.name    = std::string(settings::kKeymapDoc6),
         .notes   = "Shortcut to switch to document 6",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+6"}}}}},
        {.name    = std::string(settings::kKeymapDoc7),
         .notes   = "Shortcut to switch to document 7",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+7"}}}}},
        {.name    = std::string(settings::kKeymapDoc8),
         .notes   = "Shortcut to switch to document 8",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+8"}}}}},
        {.name    = std::string(settings::kKeymapDoc9),
         .notes   = "Shortcut to switch to document 9",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+9"}}}}},
        {.name    = std::string(settings::kKeymapBack),
         .notes   = "Shortcut to navigate to previous hypertime state",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+B"}}}}},
        {.name    = std::string(settings::kKeymapNewDoc),
         .notes   = "Shortcut to create a new document",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+N"}}}}},
        {.name    = std::string(settings::kKeymapForward),
         .notes   = "Shortcut to branch/step forward in hypertime",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+Shift+N"}}}}},
        {.name    = std::string(settings::kKeymapOpenDoc),
         .notes   = "Shortcut to open a document",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+O"}}}}},
        {.name    = std::string(settings::kKeymapCloseDoc),
         .notes   = "Shortcut to close active document",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+W"}}}}},
        {.name    = std::string(settings::kKeymapOnionSkin),
         .notes   = "Shortcut to toggle 3D onion skinning",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+Shift+O"}}}}},
        {.name    = std::string(settings::kKeymapPouchToggle),
         .notes   = "Shortcut to toggle screen-edge pouch drawer",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+\\"}}}}},
        {.name    = std::string(settings::kKeymapPouchToggleF2),
         .notes   = "Function key to toggle screen-edge pouch drawer",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"F2"}}}}},
        {.name    = std::string(settings::kKeymapTelescopeToggle),
         .notes   = "Shortcut to toggle swarm telescope overlay",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+Shift+T"}}}}},
        {.name    = std::string(settings::kKeymapTelescopeToggleF3),
         .notes   = "Function key to toggle swarm telescope overlay",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"F3"}}}}},
        {.name    = std::string(settings::kKeymapTensionPhysicsToggle),
         .notes   = "Function key to toggle 3-way tension spring layout",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"F4"}}}}},
        {.name    = std::string(settings::kKeymapUnlockTranscopyright),
         .notes   = "Shortcut to unlock transcopyright span",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+U"}}}}},
        {.name    = std::string(settings::kKeymapUnlockTranscopyrightF5),
         .notes   = "Function key to unlock transcopyright span",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"F5"}}}}},
        {.name    = std::string(settings::kKeymapUnlockTranscopyrightCtrlU),
         .notes   = "Shortcut to unlock transcopyright span (alternate)",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+U"}}}}},
        {.name    = std::string(settings::kKeymapScrubForward),
         .notes   = "Shortcut to scrub hypertime forward",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+]"}}}}},
        {.name    = std::string(settings::kKeymapScrubBackward),
         .notes   = "Shortcut to scrub hypertime backward",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+["}}}}},
        {.name    = std::string(settings::kKeymapTransclude),
         .notes   = "Shortcut to transclude selection into document",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+T"}}}}},
        {.name    = std::string(settings::kKeymapXanalink),
         .notes   = "Shortcut to forge a xanalink",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+L"}}}}},
        {.name    = std::string(settings::kKeymapCancelLink),
         .notes   = "Shortcut to cancel pending xanalink mark",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+Shift+L"}}}}},
        {.name    = std::string(settings::kKeymapBeams),
         .notes   = "Shortcut to toggle links and transclusions beams",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+K"}}}}},
        {.name    = std::string(settings::kKeymapSworph),
         .notes   = "Shortcut to sworph between documents",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+Shift+K"}}}}},
        {.name    = std::string(settings::kKeymapPublish),
         .notes   = "Shortcut to publish sovereign document",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+Shift+S"}}}}},
        {.name    = std::string(settings::kKeymapHistory),
         .notes   = "Shortcut to open history view",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+P"}}}}},
        {.name    = std::string(settings::kKeymapDelete),
         .notes   = "Shortcut to delete selected text or item",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Backspace"}}}}},
        {.name    = std::string(settings::kKeymapPageBreak),
         .notes   = "Shortcut to insert manual page break",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+Return"}}}}},
        {.name    = std::string(settings::kKeymapHypertimeMap),
         .notes   = "Shortcut to toggle hypertime map",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+H"}}}}},
        {.name    = std::string(settings::kKeymapMap),
         .notes   = "Shortcut to toggle hypertime map",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+H"}}}}},
        {.name    = std::string(settings::kKeymapScrubBack),
         .notes   = "Shortcut to scrub hypertime backward",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+["}}}}},
        {.name    = std::string(settings::kKeymapRadialMenu),
         .notes   = "Shortcut to invoke radial menu",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+M"}}}}},

        // Zigzag Visualizer & Pure Vortex Actions
        {.name    = std::string(settings::kKeymapViewModeContent1),
         .notes   = "Shortcut to switch to Cell Content View",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"1"}}}}},
        {.name    = std::string(settings::kKeymapViewModeContentV),
         .notes   = "Shortcut to switch to Cell Content View (v)",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"V"}}}}},
        {.name    = std::string(settings::kKeymapViewModeTopology),
         .notes   = "Shortcut to switch to Topology View",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"2"}}}}},
        {.name    = std::string(settings::kKeymapViewModeTopologyT),
         .notes   = "Shortcut to switch to Topology View (t)",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"T"}}}}},
        {.name    = std::string(settings::kKeymapBundleExecution),
         .notes   = "Shortcut to switch to Execution dimension bundle",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+1"}}}}},
        {.name    = std::string(settings::kKeymapBundleScope),
         .notes   = "Shortcut to switch to Scope dimension bundle",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+2"}}}}},
        {.name    = std::string(settings::kKeymapBundleContract),
         .notes   = "Shortcut to switch to Contract dimension bundle",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+3"}}}}},
        {.name    = std::string(settings::kKeymapBundleLogic),
         .notes   = "Shortcut to switch to Logic dimension bundle",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+4"}}}}},
        {.name    = std::string(settings::kKeymapBundleStdlib),
         .notes   = "Shortcut to switch to Stdlib dimension bundle",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+5"}}}}},
        {.name    = std::string(settings::kKeymapBundleCycle),
         .notes   = "Shortcut to cycle active dimension bundle forward",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+B"}}}}},
        {.name    = std::string(settings::kKeymapTogglePalette),
         .notes   = "Shortcut to toggle Vortex opcode and library palette HUD",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"F4"}}}}},
        {.name = std::string(settings::kKeymapVqlTranslateAttach),
         .notes =
             "Shortcut to translate VQL filter text and attach to active chain",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"F5"}}}}},
        {.name    = std::string(settings::kKeymapToggleCommandBar),
         .notes   = "Shortcut to toggle interactive VQL Command Omnibar",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"F2"}}}}},
        {.name    = std::string(settings::kKeymapOpenCommandBarSlash),
         .notes   = "Shortcut to open Command Omnibar with / prefix",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"/"}}}}},
        {.name    = std::string(settings::kKeymapOpenCommandBarColon),
         .notes   = "Shortcut to open Command Omnibar with : prefix",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{":"}}}}},
        {.name    = std::string(settings::kKeymapConfirmAction),
         .notes   = "Shortcut to execute Command Omnibar or clone selected "
                    "palette symbol",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Return"}}}}},
        {.name    = std::string(settings::kKeymapDismissOverlay),
         .notes   = "Shortcut to dismiss Command Omnibar or palette HUD",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Escape"}}}}},
        {.name    = std::string(settings::kKeymapStepXPos),
         .notes   = "Shortcut to step focus positive along X dimension",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Right"}}}}},
        {.name    = std::string(settings::kKeymapStepXNeg),
         .notes   = "Shortcut to step focus negative along X dimension",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Left"}}}}},
        {.name    = std::string(settings::kKeymapStepYPos),
         .notes   = "Shortcut to step focus positive along Y dimension",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Up"}}}}},
        {.name    = std::string(settings::kKeymapStepYNeg),
         .notes   = "Shortcut to step focus negative along Y dimension",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Down"}}}}},
        {.name    = std::string(settings::kKeymapStepZPos),
         .notes   = "Shortcut to step focus positive along Z dimension",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"PageUp"}}}}},
        {.name    = std::string(settings::kKeymapStepZNeg),
         .notes   = "Shortcut to step focus negative along Z dimension",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"PageDown"}}}}},
        {.name    = std::string(settings::kKeymapSwapXY),
         .notes   = "Shortcut to swap X and Y dimension bindings",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Space"}}}}},
        {.name    = std::string(settings::kKeymapCycleDimsForward),
         .notes   = "Shortcut to cycle active dimension bindings forward",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Tab"}}}}},
        {.name    = std::string(settings::kKeymapCycleDimsBackward),
         .notes   = "Shortcut to cycle active dimension bindings backward",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Shift+Tab"}}}}},
        {.name    = std::string(settings::kKeymapJumpHome),
         .notes   = "Shortcut to jump focus to home cell",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Home"}}}}},
        {.name    = std::string(settings::kKeymapHopHead),
         .notes   = "Shortcut to hop to head of current rank along X dimension",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+Home"}}}}},
        {.name    = std::string(settings::kKeymapHopTail),
         .notes   = "Shortcut to hop to tail of current rank along X dimension",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+End"}}}}},
        {.name    = std::string(settings::kKeymapDuplicateFocusCell),
         .notes   = "Shortcut to duplicate focused cell along d.clone",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+Shift+C"}}}}},
        {.name    = std::string(settings::kKeymapRasterizePrint),
         .notes   = "Shortcut to print 2D raster reading text to stdout",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"R"}}}}},
        {.name    = std::string(settings::kKeymapExportLinkPackage),
         .notes   = "Shortcut to export current slice as Xudu LinkPackage",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+S"}}}}},
        {.name = std::string(settings::kKeymapInsertCellXPos),
         .notes =
             "Shortcut to insert connected cell positive along X dimension",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"N"}}}}},
        {.name = std::string(settings::kKeymapInsertCellXNeg),
         .notes =
             "Shortcut to insert connected cell negative along X dimension",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Shift+N"}}}}},
        {.name = std::string(settings::kKeymapInsertCellYPos),
         .notes =
             "Shortcut to insert connected cell positive along Y dimension",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"D"}}}}},
        {.name = std::string(settings::kKeymapInsertCellYNeg),
         .notes =
             "Shortcut to insert connected cell negative along Y dimension",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Shift+D"}}}}},
        {.name  = std::string(settings::kKeymapUnlinkXPos),
         .notes = "Shortcut to unlink focused cell along positive X dimension",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"U"}}}}},
        {.name  = std::string(settings::kKeymapUnlinkXNeg),
         .notes = "Shortcut to unlink focused cell along negative X dimension",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Shift+U"}}}}},
        {.name    = std::string(settings::kKeymapDeleteFocusCell),
         .notes   = "Shortcut to delete currently focused cell",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Delete"}}}}},
        {.name    = std::string(settings::kKeymapDeleteFocusCellBksp),
         .notes   = "Shortcut to delete currently focused cell (backspace)",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Backspace"}}}}},
        {.name    = std::string(settings::kKeymapSaveStore),
         .notes   = "Shortcut to save current slice to sovereign store",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+Shift+S"}}}}},

        // Xuzz Zigzag Presentation Actions
        {.name    = std::string(settings::kKeymapZigzagTogglePalette),
         .notes   = "Shortcut to toggle presentation palette HUD in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"F4"}}}}},
        {.name    = std::string(settings::kKeymapZigzagVqlTranslateAttach),
         .notes   = "Shortcut to translate and attach VQL in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"F5"}}}}},
        {.name    = std::string(settings::kKeymapZigzagToggleCommandBar),
         .notes   = "Shortcut to toggle Command Omnibar in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"F2"}}}}},
        {.name    = std::string(settings::kKeymapZigzagOpenCommandBarSlash),
         .notes   = "Shortcut to open Omnibar with / prefix in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Alt+/"}}}}},
        {.name    = std::string(settings::kKeymapZigzagOpenCommandBarColon),
         .notes   = "Shortcut to open Omnibar with : prefix in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Alt+:"}}}}},
        {.name    = std::string(settings::kKeymapZigzagViewModeContent),
         .notes   = "Shortcut to switch to Cell Content View in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Alt+V"}}}}},
        {.name    = std::string(settings::kKeymapZigzagViewModeTopology),
         .notes   = "Shortcut to switch to Topology View in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Alt+T"}}}}},
        {.name    = std::string(settings::kKeymapZigzagBundleExecution),
         .notes   = "Shortcut to switch to Execution bundle in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Alt+1"}}}}},
        {.name    = std::string(settings::kKeymapZigzagBundleScope),
         .notes   = "Shortcut to switch to Scope bundle in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Alt+2"}}}}},
        {.name    = std::string(settings::kKeymapZigzagBundleContract),
         .notes   = "Shortcut to switch to Contract bundle in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Alt+3"}}}}},
        {.name    = std::string(settings::kKeymapZigzagBundleLogic),
         .notes   = "Shortcut to switch to Logic bundle in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Alt+4"}}}}},
        {.name    = std::string(settings::kKeymapZigzagBundleStdlib),
         .notes   = "Shortcut to switch to Stdlib bundle in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Alt+5"}}}}},
        {.name    = std::string(settings::kKeymapZigzagBundleCycle),
         .notes   = "Shortcut to cycle dimension bundles in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Alt+B"}}}}},
        {.name    = std::string(settings::kKeymapZigzagSwapXY),
         .notes   = "Shortcut to swap X and Y axes in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Alt+Space"}}}}},
        {.name    = std::string(settings::kKeymapZigzagCycleDimsForward),
         .notes   = "Shortcut to cycle active dimensions forward in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Alt+Tab"}}}}},
        {.name    = std::string(settings::kKeymapZigzagCycleDimsBackward),
         .notes   = "Shortcut to cycle active dimensions backward in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Alt+Shift+Tab"}}}}},
        {.name    = std::string(settings::kKeymapZigzagJumpHome),
         .notes   = "Shortcut to jump focus to home cell in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Alt+Home"}}}}},
        {.name    = std::string(settings::kKeymapZigzagHopHead),
         .notes   = "Shortcut to hop to rank head in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+Alt+Home"}}}}},
        {.name    = std::string(settings::kKeymapZigzagHopTail),
         .notes   = "Shortcut to hop to rank tail in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+Alt+End"}}}}},
        {.name    = std::string(settings::kKeymapZigzagDuplicateCell),
         .notes   = "Shortcut to duplicate focused cell in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Alt+Shift+C"}}}}},
        {.name    = std::string(settings::kKeymapZigzagSaveStore),
         .notes   = "Shortcut to save presentation store in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Ctrl+Alt+S"}}}}},
        {.name    = std::string(settings::kKeymapZigzagStepXPos),
         .notes   = "Shortcut to step positive along X dimension in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Alt+Right"}}}}},
        {.name    = std::string(settings::kKeymapZigzagStepXNeg),
         .notes   = "Shortcut to step negative along X dimension in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Alt+Left"}}}}},
        {.name    = std::string(settings::kKeymapZigzagStepYPos),
         .notes   = "Shortcut to step positive along Y dimension in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Alt+Up"}}}}},
        {.name    = std::string(settings::kKeymapZigzagStepYNeg),
         .notes   = "Shortcut to step negative along Y dimension in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Alt+Down"}}}}},
        {.name    = std::string(settings::kKeymapZigzagStepZPos),
         .notes   = "Shortcut to step positive along Z dimension in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Alt+PageUp"}}}}},
        {.name    = std::string(settings::kKeymapZigzagStepZNeg),
         .notes   = "Shortcut to step negative along Z dimension in Xuzz",
         .schemas = {{.expectedTypes = {"string"},
                      .defaultValues = {std::string{"Alt+PageDown"}}}}},
    };
    break;

  case SystemDocKind::Pouches:
    specs = {
        {.name    = std::string(settings::kPouchZoneToLinkLeft),
         .notes   = "Left transclusion staging drop zone: [label, auraColor, "
                    "heightWeight]",
         .schemas = {{.expectedTypes = {"string", "integer", "float"},
                      .defaultValues = {std::string{"To Link (Left)"},
                                        static_cast<std::int64_t>(0x06B6D4FFU),
                                        1.0}}}},
        {.name    = std::string(settings::kPouchZoneToLinkRight),
         .notes   = "Right transclusion staging drop zone: [label, auraColor, "
                    "heightWeight]",
         .schemas = {{.expectedTypes = {"string", "integer", "float"},
                      .defaultValues = {std::string{"To Link (Right)"},
                                        static_cast<std::int64_t>(0xEC4899FFU),
                                        1.0}}}},
        {.name    = std::string(settings::kPouchZoneNotes),
         .notes   = "Notes drop zone: [label, auraColor, heightWeight]",
         .schemas = {{.expectedTypes = {"string", "integer", "float"},
                      .defaultValues = {std::string{"Notes"},
                                        static_cast<std::int64_t>(0xEAB308FFU),
                                        1.0}}}},
        {.name    = std::string(settings::kPouchZoneScratch),
         .notes   = "Scratch drop zone: [label, auraColor, heightWeight]",
         .schemas = {{.expectedTypes = {"string", "integer", "float"},
                      .defaultValues = {std::string{"Scratch"},
                                        static_cast<std::int64_t>(0x10B981FFU),
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
  auto cur = resolveStartingVersion(store, parent);
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
  auto cur = resolveStartingVersion(store, parent);
  if (store.homeCell() == zigzag::noCell) {
    cur = initializeSystemStoreGenesis(store, SystemDocKind::Layout, cur);
  }

  std::optional<zigzag::Manifold> folded;
  if (known == nullptr) {
    folded = store.rebuildManifold(cur);
  }
  // folded is unconditionally assigned just above whenever known == nullptr
  // (the only time this branch is taken), and rebuildManifold() never
  // returns an empty optional.
  zigzag::Manifold localM =
      (known != nullptr)
          ? *known
          : folded.value(); // NOLINT(bugprone-unchecked-optional-access)
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

  const auto &leafName = tokens.back();
  cur                  = store.makeCell(cur, leafName);
  const auto cloneRef  = store.cellRefOf(cur);
  folded               = store.rebuildManifold(cur);
  m                    = &folded.value();
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
          ? std::vector<SettingSchemaShape>{{.expectedTypes = {"string"},
                                             .defaultValues = {std::string{
                                                 ""}}}}
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
  auto cur = resolveStartingVersion(store, parent);
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
  const std::string p1 = defaultSystemDocSchema(kind);
  const std::string p2 = defaultSystemDocNotes(kind);

  MicroversionId cur{};
  cur = store.insert(cur, 0, p1);

  const auto p1Size = static_cast<std::uint32_t>(p1.size());
  cur               = store.insertBreak(cur, p1Size);
  cur               = store.insert(cur, p1Size, p2);

  const Version doc = store.rebuild(cur);

  // Format links for Page 1 header
  constexpr std::string_view schemaHeader = "Schema and Purpose";
  const auto schemaHeaderSpans =
      doc.spansFor(0, static_cast<std::uint32_t>(schemaHeader.size()));
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

  // Format links for Page 2 header
  constexpr std::string_view notesHeader = "Notes";
  const auto notesHeaderSpans =
      doc.spansFor(p1Size, static_cast<std::uint32_t>(notesHeader.size()));
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

  // Bidirectional xanalink between the schema and notes pages, per this
  // repo's system-xanadoc governance: every system xanadoc is xanalinked to
  // both its schema/purpose page and its notes page.
  const auto p1Spans = doc.spansFor(0, p1Size);
  const auto p2Spans =
      doc.spansFor(p1Size, static_cast<std::uint32_t>(p2.size()));

  if (!p1Spans.empty() && !p2Spans.empty()) {
    Link schemaNotesLink;
    schemaNotesLink.type  = LinkType::Comment;
    schemaNotesLink.tier  = ProminenceTier::Author;
    schemaNotesLink.owner = "system";
    schemaNotesLink.left  = p1Spans;
    schemaNotesLink.right = p2Spans;
    cur                   = store.addLink(cur, std::move(schemaNotesLink));
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

// -----------------------------------------------------------------------------
// SystemStoreModel Implementation
// -----------------------------------------------------------------------------

SystemStoreModel SystemStoreModel::fromStore(const Store &store,
                                             const MicroversionId &version) {
  if (store.opCount() == 0 || store.homeCell() == zigzag::noCell) {
    SystemStoreModel model;
    model.isValid_ = false;
    model.error_   = "Store has no operations or home cell";
    return model;
  }
  const auto ver = version.isZero() ? store.primaryCurrentVersion() : version;
  const auto manifold = store.rebuildManifold(ver);
  const auto &reader  = static_cast<const SpanReader &>(store);
  return fromManifold(manifold, store.homeCell(), &reader);
}

namespace {
struct NullSpanReader final : public SpanReader {
  [[nodiscard]] std::string read(const PrimediaSpan & /*span*/) const override {
    return {};
  }
};
} // namespace

SystemStoreModel
SystemStoreModel::fromManifold(const zigzag::Manifold &manifold,
                               zigzag::CellRef homeCell,
                               const SpanReader *reader) {
  SystemStoreModel model;
  if (reader == nullptr && manifold.store() != nullptr) {
    reader = manifold.store();
  }
  static const NullSpanReader kNullReader;
  if (reader == nullptr) {
    reader = &kNullReader;
  }

  if (homeCell == zigzag::noCell) {
    if (manifold.contains(1)) {
      homeCell = 1;
    } else if (!manifold.cells().empty()) {
      homeCell = manifold.cells().front().birthOp;
    } else {
      model.isValid_ = false;
      model.error_   = "Manifold has no cells";
      return model;
    }
  } else if (!manifold.contains(homeCell)) {
    model.isValid_ = false;
    model.error_   = "Manifold does not contain specified home cell";
    return model;
  }

  const auto varsDim      = manifold.dimensionNamed(kDimVars, *reader);
  const auto valuesDim    = manifold.dimensionNamed(kDimValues, *reader);
  const auto groupsDim    = manifold.dimensionNamed(kDimGroups, *reader);
  const auto subgroupsDim = manifold.dimensionNamed(kDimSubgroups, *reader);
  const auto notesDim     = manifold.dimensionNamed(kDimNotes, *reader);
  const auto schemasDim   = manifold.dimensionNamed(kDimSchemas, *reader);
  const auto altsDim      = manifold.dimensionNamed(kDimAlternates, *reader);
  const auto defaultDim   = manifold.dimensionNamed(kDimDefault, *reader);

  if (varsDim != zigzag::noCell) {
    while (true) {
      const auto prev =
          manifold.linked(homeCell, varsDim, zigzag::DimVector::NEG);
      if (prev == zigzag::noCell || prev == homeCell) {
        break;
      }
      homeCell = prev;
    }
  }

  model.storeDesc_ = manifold.textOf(homeCell, *reader);

  if (notesDim != zigzag::noCell) {
    const auto noteCell =
        manifold.linked(homeCell, notesDim, zigzag::DimVector::POS);
    if (noteCell != zigzag::noCell) {
      model.storeDesc_ = manifold.textOf(noteCell, *reader);
    }
  }

  // Read group hierarchy recursively
  std::function<SettingGroup(zigzag::CellRef)> readGroupNode =
      [&](const zigzag::CellRef gCell) -> SettingGroup {
    SettingGroup grp;
    grp.groupCell = gCell;
    grp.name      = manifold.textOf(gCell, *reader);

    if (varsDim != zigzag::noCell) {
      manifold.walkRank(gCell, varsDim, zigzag::DimVector::POS,
                        [&](const zigzag::CellRef cloneCell) {
                          if (cloneCell != gCell) {
                            grp.memberSettingNames.push_back(
                                manifold.textOf(cloneCell, *reader));
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
    manifold.walkRank(homeCell, groupsDim, zigzag::DimVector::POS,
                      [&](const zigzag::CellRef gCell) {
                        if (gCell != homeCell) {
                          model.groups_.push_back(readGroupNode(gCell));
                        }
                      });
  }

  // Read master settings along d.vars
  if (varsDim != zigzag::noCell) {
    manifold.walkRank(
        homeCell, varsDim, zigzag::DimVector::POS,
        [&](const zigzag::CellRef setCell) {
          if (setCell == homeCell) {
            return;
          }
          SettingEntry entry;
          entry.nameCell  = setCell;
          entry.name      = manifold.textOf(setCell, *reader);
          entry.groupPath = splitTokens(entry.name, '.');
          if (!entry.groupPath.empty()) {
            entry.groupPath.pop_back();
          }

          // Notes
          if (notesDim != zigzag::noCell) {
            const auto noteCell =
                manifold.linked(setCell, notesDim, zigzag::DimVector::POS);
            if (noteCell != zigzag::noCell) {
              entry.notes = manifold.textOf(noteCell, *reader);
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
                          manifold.textOf(typeClone, *reader));
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
                                manifold.textOf(defCell, *reader));
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
                                        manifold.textOf(valCell, *reader));
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

namespace {
const CellValue *findDefaultSettingValue(const std::string_view name) {
  static const auto defaultsMap = []() {
    std::unordered_map<std::string, CellValue> m;
    for (int k = 0; k < static_cast<int>(SystemDocKind::Count); ++k) {
      for (const auto &spec :
           defaultSettingSpecs(static_cast<SystemDocKind>(k))) {
        if (!spec.schemas.empty() &&
            !spec.schemas.front().defaultValues.empty()) {
          m.emplace(spec.name, spec.schemas.front().defaultValues.front());
        }
      }
    }
    return m;
  }();
  const auto it = defaultsMap.find(std::string(name));
  return it != defaultsMap.end() ? &it->second : nullptr;
}
} // namespace

double SystemStoreModel::getDouble(const std::string_view name,
                                   const double fallback) const {
  const auto *const entry = find(name);
  if (entry != nullptr && !entry->value.elements.empty()) {
    return entry->value.asDouble(0, fallback);
  }
  if (entry != nullptr && !entry->schema.alternatives.empty() &&
      !entry->schema.alternatives.front().defaultValues.empty()) {
    const auto &el = entry->schema.alternatives.front().defaultValues.front();
    if (std::holds_alternative<double>(el)) {
      return std::get<double>(el);
    }
    if (std::holds_alternative<std::int64_t>(el)) {
      return static_cast<double>(std::get<std::int64_t>(el));
    }
    if (std::holds_alternative<bool>(el)) {
      return std::get<bool>(el) ? 1.0 : 0.0;
    }
  }
  if (const auto *def = findDefaultSettingValue(name)) {
    if (std::holds_alternative<double>(*def)) {
      return std::get<double>(*def);
    }
    if (std::holds_alternative<std::int64_t>(*def)) {
      return static_cast<double>(std::get<std::int64_t>(*def));
    }
    if (std::holds_alternative<bool>(*def)) {
      return std::get<bool>(*def) ? 1.0 : 0.0;
    }
  }
  return fallback;
}

std::int64_t SystemStoreModel::getInt64(const std::string_view name,
                                        const std::int64_t fallback) const {
  const auto *const entry = find(name);
  if (entry != nullptr && !entry->value.elements.empty()) {
    return entry->value.asInt64(0, fallback);
  }
  if (entry != nullptr && !entry->schema.alternatives.empty() &&
      !entry->schema.alternatives.front().defaultValues.empty()) {
    const auto &el = entry->schema.alternatives.front().defaultValues.front();
    if (std::holds_alternative<std::int64_t>(el)) {
      return std::get<std::int64_t>(el);
    }
    if (std::holds_alternative<double>(el)) {
      return static_cast<std::int64_t>(std::get<double>(el));
    }
    if (std::holds_alternative<bool>(el)) {
      return std::get<bool>(el) ? 1 : 0;
    }
  }
  if (const auto *def = findDefaultSettingValue(name)) {
    if (std::holds_alternative<std::int64_t>(*def)) {
      return std::get<std::int64_t>(*def);
    }
    if (std::holds_alternative<double>(*def)) {
      return static_cast<std::int64_t>(std::get<double>(*def));
    }
    if (std::holds_alternative<bool>(*def)) {
      return std::get<bool>(*def) ? 1 : 0;
    }
  }
  return fallback;
}

bool SystemStoreModel::getBool(const std::string_view name,
                               const bool fallback) const {
  const auto *const entry = find(name);
  if (entry != nullptr && !entry->value.elements.empty()) {
    return entry->value.asBool(0, fallback);
  }
  if (entry != nullptr && !entry->schema.alternatives.empty() &&
      !entry->schema.alternatives.front().defaultValues.empty()) {
    const auto &el = entry->schema.alternatives.front().defaultValues.front();
    if (std::holds_alternative<bool>(el)) {
      return std::get<bool>(el);
    }
    if (std::holds_alternative<std::int64_t>(el)) {
      return std::get<std::int64_t>(el) != 0;
    }
    if (std::holds_alternative<double>(el)) {
      return std::get<double>(el) != 0.0;
    }
  }
  if (const auto *def = findDefaultSettingValue(name)) {
    if (std::holds_alternative<bool>(*def)) {
      return std::get<bool>(*def);
    }
    if (std::holds_alternative<std::int64_t>(*def)) {
      return std::get<std::int64_t>(*def) != 0;
    }
    if (std::holds_alternative<double>(*def)) {
      return std::get<double>(*def) != 0.0;
    }
  }
  return fallback;
}

std::string SystemStoreModel::getString(const std::string_view name,
                                        const std::string_view fallback) const {
  const auto *const entry = find(name);
  if (entry != nullptr && !entry->value.elements.empty()) {
    return entry->value.asString(0, fallback);
  }
  if (entry != nullptr && !entry->schema.alternatives.empty() &&
      !entry->schema.alternatives.front().defaultValues.empty()) {
    const auto &el = entry->schema.alternatives.front().defaultValues.front();
    if (std::holds_alternative<std::string>(el)) {
      return std::get<std::string>(el);
    }
  }
  if (const auto *def = findDefaultSettingValue(name)) {
    if (std::holds_alternative<std::string>(*def)) {
      return std::get<std::string>(*def);
    }
  }
  return std::string{fallback};
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
  // folded is populated in exactly the branch where the ternary below reads
  // it (known == nullptr), so .value() can't actually throw here -- the
  // analyzer just can't correlate the two conditions.
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
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
      cur  = store.setLink(cur, prev, valuesDim, zigzag::DimVector::POS, valRef,
                           m);
      prev = valRef;
    }
  }

  return cur;
}

MicroversionId
SystemStoreModel::resetToDefault(Store &store, const MicroversionId &parent,
                                 const std::string_view name,
                                 const zigzag::Manifold *const known) {
  auto curVer      = parent.isZero() ? store.primaryCurrentVersion() : parent;
  const auto model = fromStore(store, curVer);
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
// Config Struct fromStore Loaders & Keymap Resolution
// -----------------------------------------------------------------------------

namespace {
struct ActionAlias {
  std::string_view legacy;
  std::string_view canonical;
};

constexpr ActionAlias kActionAliases[] = {
    // Xudu Core Actions
    {.legacy = "quit", .canonical = "std:xudu/quit"},
    {.legacy = "save", .canonical = "std:xudu/save"},
    {.legacy = "close", .canonical = "std:xudu/close"},
    {.legacy = "next-doc", .canonical = "std:xudu/next_doc"},
    {.legacy = "prev-doc", .canonical = "std:xudu/prev_doc"},
    {.legacy = "doc-1", .canonical = "std:xudu/doc_1"},
    {.legacy = "doc-2", .canonical = "std:xudu/doc_2"},
    {.legacy = "doc-3", .canonical = "std:xudu/doc_3"},
    {.legacy = "doc-4", .canonical = "std:xudu/doc_4"},
    {.legacy = "doc-5", .canonical = "std:xudu/doc_5"},
    {.legacy = "doc-6", .canonical = "std:xudu/doc_6"},
    {.legacy = "doc-7", .canonical = "std:xudu/doc_7"},
    {.legacy = "doc-8", .canonical = "std:xudu/doc_8"},
    {.legacy = "doc-9", .canonical = "std:xudu/doc_9"},
    {.legacy = "back", .canonical = "std:xudu/back"},
    {.legacy = "new-doc", .canonical = "std:xudu/new_doc"},
    {.legacy = "forward", .canonical = "std:xudu/forward"},
    {.legacy = "open-doc", .canonical = "std:xudu/open_doc"},
    {.legacy = "close-doc", .canonical = "std:xudu/close_doc"},
    {.legacy = "onion-skin", .canonical = "std:xudu/onion_skin"},
    {.legacy = "pouch-toggle", .canonical = "std:xudu/pouch_toggle"},
    {.legacy = "pouch-toggle-f2", .canonical = "std:xudu/pouch_toggle_f2"},
    {.legacy = "telescope-toggle", .canonical = "std:xudu/telescope_toggle"},
    {.legacy    = "telescope-toggle-f3",
     .canonical = "std:xudu/telescope_toggle_f3"},
    {.legacy    = "tension-physics-toggle",
     .canonical = "std:xudu/tension_physics_toggle"},
    {.legacy    = "unlock-transcopyright",
     .canonical = "std:xudu/unlock_transcopyright"},
    {.legacy    = "unlock-transcopyright-f5",
     .canonical = "std:xudu/unlock_transcopyright_f5"},
    {.legacy    = "unlock-transcopyright-ctrl-u",
     .canonical = "std:xudu/unlock_transcopyright_ctrl_u"},
    {.legacy = "scrub-forward", .canonical = "std:xudu/scrub_forward"},
    {.legacy = "scrub-backward", .canonical = "std:xudu/scrub_backward"},
    {.legacy = "transclude", .canonical = "std:xudu/transclude"},
    {.legacy = "xanalink", .canonical = "std:xudu/xanalink"},
    {.legacy = "cancel-link", .canonical = "std:xudu/cancel_link"},
    {.legacy = "beams", .canonical = "std:xudu/beams"},
    {.legacy = "sworph", .canonical = "std:xudu/sworph"},
    {.legacy = "publish", .canonical = "std:xudu/publish"},
    {.legacy = "history", .canonical = "std:xudu/history"},
    {.legacy = "delete", .canonical = "std:xudu/delete"},
    {.legacy = "page-break", .canonical = "std:xudu/page_break"},
    {.legacy = "hypertime-map", .canonical = "std:xudu/hypertime_map"},
    {.legacy = "map", .canonical = "std:xudu/map"},
    {.legacy = "scrub-back", .canonical = "std:xudu/scrub_back"},
    {.legacy = "radial-menu", .canonical = "std:xudu/radial_menu"},

    // Zigzag Visualizer & Pure Vortex Actions
    {.legacy    = "view-mode-content-1",
     .canonical = "std:ui/view_mode_content_1"},
    {.legacy    = "view-mode-content-v",
     .canonical = "std:ui/view_mode_content_v"},
    {.legacy = "view-mode-topology", .canonical = "std:ui/view_mode_topology"},
    {.legacy    = "view-mode-topology-t",
     .canonical = "std:ui/view_mode_topology_t"},
    {.legacy = "bundle-execution", .canonical = "std:ui/bundle_execution"},
    {.legacy = "bundle-scope", .canonical = "std:ui/bundle_scope"},
    {.legacy = "bundle-contract", .canonical = "std:ui/bundle_contract"},
    {.legacy = "bundle-logic", .canonical = "std:ui/bundle_logic"},
    {.legacy = "bundle-stdlib", .canonical = "std:ui/bundle_stdlib"},
    {.legacy = "bundle-cycle", .canonical = "std:ui/bundle_cycle"},
    {.legacy = "toggle-palette", .canonical = "std:ui/toggle_palette"},
    {.legacy    = "vql-translate-attach",
     .canonical = "std:ui/vql_translate_attach"},
    {.legacy = "toggle-command-bar", .canonical = "std:ui/toggle_command_bar"},
    {.legacy    = "open-command-bar-slash",
     .canonical = "std:ui/open_command_bar_slash"},
    {.legacy    = "open-command-bar-colon",
     .canonical = "std:ui/open_command_bar_colon"},
    {.legacy = "confirm-action", .canonical = "std:ui/confirm_action"},
    {.legacy = "dismiss-overlay", .canonical = "std:ui/dismiss_overlay"},
    {.legacy = "step-x-pos", .canonical = "std:nav/step_x_pos"},
    {.legacy = "step-x-neg", .canonical = "std:nav/step_x_neg"},
    {.legacy = "step-y-pos", .canonical = "std:nav/step_y_pos"},
    {.legacy = "step-y-neg", .canonical = "std:nav/step_y_neg"},
    {.legacy = "step-z-pos", .canonical = "std:nav/step_z_pos"},
    {.legacy = "step-z-neg", .canonical = "std:nav/step_z_neg"},
    {.legacy = "swap-xy", .canonical = "std:ui/swap_axes"},
    {.legacy = "cycle-dims-forward", .canonical = "std:ui/cycle_dims_forward"},
    {.legacy    = "cycle-dims-backward",
     .canonical = "std:ui/cycle_dims_backward"},
    {.legacy = "jump-home", .canonical = "std:nav/jump_home"},
    {.legacy = "hop-head", .canonical = "std:nav/hop_head"},
    {.legacy = "hop-tail", .canonical = "std:nav/hop_tail"},
    {.legacy = "duplicate-focus-cell", .canonical = "std:zigzag/duplicate"},
    {.legacy = "rasterize-print", .canonical = "std:zigzag/rasterize_print"},
    {.legacy    = "export-link-package",
     .canonical = "std:zigzag/export_link_package"},
    {.legacy    = "insert-cell-x-pos",
     .canonical = "std:zigzag/insert_cell_x_pos"},
    {.legacy    = "insert-cell-x-neg",
     .canonical = "std:zigzag/insert_cell_x_neg"},
    {.legacy    = "insert-cell-y-pos",
     .canonical = "std:zigzag/insert_cell_y_pos"},
    {.legacy    = "insert-cell-y-neg",
     .canonical = "std:zigzag/insert_cell_y_neg"},
    {.legacy = "unlink-x-pos", .canonical = "std:zigzag/unlink_x_pos"},
    {.legacy = "unlink-x-neg", .canonical = "std:zigzag/unlink_x_neg"},
    {.legacy    = "delete-focus-cell",
     .canonical = "std:zigzag/delete_focus_cell"},
    {.legacy    = "delete-focus-cell-bksp",
     .canonical = "std:zigzag/delete_focus_cell_bksp"},
    {.legacy = "save-store", .canonical = "std:zigzag/save_store"},

    // Xuzz Zigzag Presentation Actions
    {.legacy    = "zigzag-toggle-palette",
     .canonical = "std:ui/zigzag_toggle_palette"},
    {.legacy    = "zigzag-vql-translate-attach",
     .canonical = "std:ui/zigzag_vql_translate_attach"},
    {.legacy    = "zigzag-toggle-command-bar",
     .canonical = "std:ui/zigzag_toggle_command_bar"},
    {.legacy    = "zigzag-open-command-bar-slash",
     .canonical = "std:ui/zigzag_open_command_bar_slash"},
    {.legacy    = "zigzag-open-command-bar-colon",
     .canonical = "std:ui/zigzag_open_command_bar_colon"},
    {.legacy    = "zigzag-view-mode-content",
     .canonical = "std:ui/zigzag_view_mode_content"},
    {.legacy    = "zigzag-view-mode-topology",
     .canonical = "std:ui/zigzag_view_mode_topology"},
    {.legacy    = "zigzag-bundle-execution",
     .canonical = "std:ui/zigzag_bundle_execution"},
    {.legacy    = "zigzag-bundle-scope",
     .canonical = "std:ui/zigzag_bundle_scope"},
    {.legacy    = "zigzag-bundle-contract",
     .canonical = "std:ui/zigzag_bundle_contract"},
    {.legacy    = "zigzag-bundle-logic",
     .canonical = "std:ui/zigzag_bundle_logic"},
    {.legacy    = "zigzag-bundle-stdlib",
     .canonical = "std:ui/zigzag_bundle_stdlib"},
    {.legacy    = "zigzag-bundle-cycle",
     .canonical = "std:ui/zigzag_bundle_cycle"},
    {.legacy = "zigzag-swap-xy", .canonical = "std:ui/zigzag_swap_xy"},
    {.legacy    = "zigzag-cycle-dims-forward",
     .canonical = "std:ui/zigzag_cycle_dims_forward"},
    {.legacy    = "zigzag-cycle-dims-backward",
     .canonical = "std:ui/zigzag_cycle_dims_backward"},
    {.legacy = "zigzag-jump-home", .canonical = "std:nav/zigzag_jump_home"},
    {.legacy = "zigzag-hop-head", .canonical = "std:nav/zigzag_hop_head"},
    {.legacy = "zigzag-hop-tail", .canonical = "std:nav/zigzag_hop_tail"},
    {.legacy    = "zigzag-duplicate-cell",
     .canonical = "std:zigzag/zigzag_duplicate_cell"},
    {.legacy    = "zigzag-save-store",
     .canonical = "std:zigzag/zigzag_save_store"},
    {.legacy = "zigzag-step-x-pos", .canonical = "std:nav/zigzag_step_x_pos"},
    {.legacy = "zigzag-step-x-neg", .canonical = "std:nav/zigzag_step_x_neg"},
    {.legacy = "zigzag-step-y-pos", .canonical = "std:nav/zigzag_step_y_pos"},
    {.legacy = "zigzag-step-y-neg", .canonical = "std:nav/zigzag_step_y_neg"},
    {.legacy = "zigzag-step-z-pos", .canonical = "std:nav/zigzag_step_z_pos"},
    {.legacy = "zigzag-step-z-neg", .canonical = "std:nav/zigzag_step_z_neg"},
};
} // namespace

std::string_view canonicalKeymapAction(const std::string_view action) {
  for (const auto &alias : kActionAliases) {
    if (alias.legacy == action) {
      return alias.canonical;
    }
  }
  return action;
}

std::string_view legacyKeymapAction(const std::string_view action) {
  for (const auto &alias : kActionAliases) {
    if (alias.canonical == action) {
      return alias.legacy;
    }
  }
  return action;
}

std::optional<std::string>
KeymapConfig::bindingFor(const std::string_view action) const {
  const auto canon  = canonicalKeymapAction(action);
  const auto legacy = legacyKeymapAction(action);
  for (const auto &[act, key] : bindings) {
    if (act == action || act == canon || act == legacy) {
      return key;
    }
  }
  return std::nullopt;
}

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
  cfg.beams.zFightJitterAmplitude = static_cast<float>(
      model.getDouble(settings::kBeamsZFightJitterAmplitude,
                      static_cast<double>(cfg.beams.zFightJitterAmplitude)));
  cfg.beams.activeZBoost = static_cast<float>(
      model.getDouble(settings::kBeamsActiveZBoost,
                      static_cast<double>(cfg.beams.activeZBoost)));

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

  cfg.bridge = BridgeRuntimeConfig::fromSystemDocs(store);

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

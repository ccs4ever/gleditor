/**
 * @file system_docs.hpp
 * @brief Sovereign system xanadocs managing runtime parameters.
 */
#ifndef XUDU_SYSTEM_DOCS_H
#define XUDU_SYSTEM_DOCS_H

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/xanadu/bridge_config.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/tension_layout.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"
#include <gleditor/radial_menu.hpp>

#include <gleditor/cpp26.hpp>

namespace zigzag {
class Manifold;
} // namespace zigzag

namespace xanadu {

class SpanReader;

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

[[nodiscard]] std::string defaultSystemDocSchema(SystemDocKind kind);
[[nodiscard]] std::string defaultSystemDocNotes(SystemDocKind kind);
[[nodiscard]] std::filesystem::path systemDocDirectory(SystemDocKind kind);

class Store;

// Standardized dimensional constants for system store cell geometry:
inline constexpr std::string_view kDimDims       = "d.dims";
inline constexpr std::string_view kDimVars       = "d.vars";
inline constexpr std::string_view kDimValues     = "d.values";
inline constexpr std::string_view kDimGroups     = "d.groups";
inline constexpr std::string_view kDimSubgroups  = "d.subgroups";
inline constexpr std::string_view kDimClone      = "d.clone";
inline constexpr std::string_view kDimNotes      = "d.notes";
inline constexpr std::string_view kDimSchemas    = "d.schemas";
inline constexpr std::string_view kDimAlternates = "d.alternates";
inline constexpr std::string_view kDimDefault    = "d.default";

namespace settings {
// Layout
inline constexpr std::string_view kColumns      = "columns";
inline constexpr std::string_view kPageWidthPx  = "pageWidthPx";
inline constexpr std::string_view kPageHeightPx = "pageHeightPx";
/// On-screen height, in screen pixels, of a line of document text at the
/// camera's default zoom and wherever it frames a passage for reading.
inline constexpr std::string_view kReadableTextPx     = "readableTextPx";
inline constexpr std::string_view kTransclusionPrisms = "transclusionPrisms";
inline constexpr std::string_view kTransclusionLoom   = "transclusionLoom";
inline constexpr std::string_view kXanalinkRibbons    = "xanalinkRibbons";
inline constexpr std::string_view kPouchDock          = "pouchDock";
inline constexpr std::string_view kPouchWidthPx       = "pouchWidthPx";
inline constexpr std::string_view kPhysicsKRepel      = "physics.kRepel";
inline constexpr std::string_view kPhysicsKPlane      = "physics.kPlane";
inline constexpr std::string_view kPhysicsKAlign      = "physics.kAlign";
inline constexpr std::string_view kPhysicsKTier       = "physics.kTier";
inline constexpr std::string_view kPhysicsKDamping    = "physics.kDamping";
inline constexpr std::string_view kPhysicsBackgroundDepthZ =
    "physics.backgroundDepthZ";
inline constexpr std::string_view kPhysicsDefaultGap = "physics.defaultGap";
inline constexpr std::string_view kPhysicsSettleVelocityThreshold =
    "physics.settleVelocityThreshold";
inline constexpr std::string_view kPhysicsMaxForce    = "physics.maxForce";
inline constexpr std::string_view kPhysicsMaxVelocity = "physics.maxVelocity";
inline constexpr std::string_view kPhysicsTimeStep    = "physics.timeStep";
inline constexpr std::string_view kBeamsBandStrandLimit =
    "beams.bandStrandLimit";
inline constexpr std::string_view kBeamsBandStrandPitch =
    "beams.bandStrandPitch";
inline constexpr std::string_view kBeamsBandFillAlpha = "beams.bandFillAlpha";
inline constexpr std::string_view kBeamsStubWidthOfBeam =
    "beams.stubWidthOfBeam";
inline constexpr std::string_view kBeamsStubMinOfLine = "beams.stubMinOfLine";
inline constexpr std::string_view kBeamsMarginKerf    = "beams.marginKerf";
inline constexpr std::string_view kBeamsBypassDepthPerDoc =
    "beams.bypassDepthPerDoc";
inline constexpr std::string_view kBeamsBypassDepthLimit =
    "beams.bypassDepthLimit";
inline constexpr std::string_view kBeamsBypassSegments = "beams.bypassSegments";
inline constexpr std::string_view kBeamsLoomBundlingEnabled =
    "beams.loomBundlingEnabled";
inline constexpr std::string_view kBeamsLoomAlpha      = "beams.loomAlpha";
inline constexpr std::string_view kBeamsLoomHoverAlpha = "beams.loomHoverAlpha";
inline constexpr std::string_view kBeamsZFightJitterAmplitude =
    "beams.zFightJitterAmplitude";
inline constexpr std::string_view kBeamsActiveZBoost = "beams.activeZBoost";
inline constexpr std::string_view kZigzagCellHorizontalPaddingPx =
    "zigzag.cellHorizontalPaddingPx";
inline constexpr std::string_view kZigzagCellVerticalPaddingPx =
    "zigzag.cellVerticalPaddingPx";
inline constexpr std::string_view kZigzagCellBandGapPx = "zigzag.cellBandGapPx";
inline constexpr std::string_view kZigzagContentMaxWidthPx =
    "zigzag.contentMaxWidthPx";
inline constexpr std::string_view kZigzagTopologyMaxWidthPx =
    "zigzag.topologyMaxWidthPx";
inline constexpr std::string_view kZigzagRankClearancePx =
    "zigzag.rankClearancePx";
inline constexpr std::string_view kZigzagHudHorizontalPaddingPx =
    "zigzag.hudHorizontalPaddingPx";
inline constexpr std::string_view kZigzagHudVerticalPaddingPx =
    "zigzag.hudVerticalPaddingPx";
inline constexpr std::string_view kZigzagHudColumnGapPx =
    "zigzag.hudColumnGapPx";
inline constexpr std::string_view kZigzagMinReadableTextPx =
    "zigzag.minReadableTextPx";
inline constexpr std::string_view kZigzagConnectionBeamWidthPx =
    "zigzag.connectionBeamWidthPx";

// Bridge Runtime Configuration
inline constexpr std::string_view kBridgeCellRadius = "bridge.cellRadius";
inline constexpr std::string_view kBridgeBackgroundDepthZ =
    "bridge.backgroundDepthZ";
inline constexpr std::string_view kBridgeBackgroundOpacity =
    "bridge.backgroundOpacity";
inline constexpr std::string_view kBridgeSatelloidAlignment =
    "bridge.satelloid.alignment";
inline constexpr std::string_view kBridgeSatelloidTether =
    "bridge.satelloid.tether";
inline constexpr std::string_view kBridgeSatelloidMass =
    "bridge.satelloid.mass";
inline constexpr std::string_view kBridgeSatelloidGap = "bridge.satelloid.gap";
inline constexpr std::string_view kBridgeSatelloidWidth =
    "bridge.satelloid.width";
inline constexpr std::string_view kBridgeSatelloidHeight =
    "bridge.satelloid.height";
inline constexpr std::string_view kBridgeTetherControlDepth =
    "bridge.tether.controlDepth";
inline constexpr std::string_view kBridgeTetherSegments =
    "bridge.tether.segments";
inline constexpr std::string_view kBridgeTetherDepthThreshold =
    "bridge.tether.depthThreshold";
inline constexpr std::string_view kBridgeTetherColour = "bridge.tether.colour";
inline constexpr std::string_view kBridgeLoomBundling = "bridge.loom.bundling";
inline constexpr std::string_view kBridgeLoomAlpha    = "bridge.loom.alpha";
inline constexpr std::string_view kBridgeLoomHoverAlpha =
    "bridge.loom.hoverAlpha";

// Settings
inline constexpr std::string_view kFontSize        = "fontSize";
inline constexpr std::string_view kFontFamily      = "fontFamily";
inline constexpr std::string_view kLineHeight      = "lineHeight";
inline constexpr std::string_view kTheme           = "theme";
inline constexpr std::string_view kAutoSaveSeconds = "autoSaveSeconds";
inline constexpr std::string_view kThemeBackground = "theme.background";

// UI
inline constexpr std::string_view kTabBarVisible       = "tabBarVisible";
inline constexpr std::string_view kStatusBarVisible    = "statusBarVisible";
inline constexpr std::string_view kHypertimeMapVisible = "hypertimeMapVisible";
inline constexpr std::string_view kNotificationPosition =
    "notificationPosition";
inline constexpr std::string_view kNotificationDurationMs =
    "notificationDurationMs";
inline constexpr std::string_view kRadialMenuRadius = "radialMenu.radius";
inline constexpr std::string_view kRadialMenuInnerRadius =
    "radialMenu.innerRadius";
// The overview panel. Colours are RGBA8, most significant byte red.
inline constexpr std::string_view kOverviewVisible  = "overview.visible";
inline constexpr std::string_view kOverviewWidthPx  = "overview.widthPx";
inline constexpr std::string_view kOverviewHeightPx = "overview.heightPx";
inline constexpr std::string_view kOverviewLeftPx   = "overview.leftPx";
inline constexpr std::string_view kOverviewBottomPx = "overview.bottomPx";
inline constexpr std::string_view kOverviewBackgroundColour =
    "overview.backgroundColour";
inline constexpr std::string_view kOverviewPageColour = "overview.pageColour";
inline constexpr std::string_view kOverviewViewportColour =
    "overview.viewportColour";
inline constexpr std::string_view kOverviewMarkColour = "overview.markColour";
// The selected-link panel. Colours are RGBA8, most significant byte red.
inline constexpr std::string_view kLinkPanelFont      = "linkPanel.font";
inline constexpr std::string_view kLinkPanelMarginPx  = "linkPanel.marginPx";
inline constexpr std::string_view kLinkPanelTopPx     = "linkPanel.topPx";
inline constexpr std::string_view kLinkPanelPaddingPx = "linkPanel.paddingPx";
inline constexpr std::string_view kLinkPanelLineGapPx = "linkPanel.lineGapPx";
inline constexpr std::string_view kLinkPanelBackgroundColour =
    "linkPanel.backgroundColour";
inline constexpr std::string_view kLinkPanelTextColour = "linkPanel.textColour";
inline constexpr std::string_view kLinkPanelMutedColour =
    "linkPanel.mutedColour";
inline constexpr std::string_view kLinkPanelButtonColour =
    "linkPanel.buttonColour";
inline constexpr std::string_view kLinkPanelChosenHighlightColour =
    "linkPanel.chosenHighlightColour";
inline constexpr std::string_view kLinkPanelMemberHighlightColour =
    "linkPanel.memberHighlightColour";

// Keymap - Xudu Core Actions (Sovereign Vortex Function Calls)
inline constexpr std::string_view kKeymapQuit        = "std:xudu/quit";
inline constexpr std::string_view kKeymapSave        = "std:xudu/save";
inline constexpr std::string_view kKeymapClose       = "std:xudu/close";
inline constexpr std::string_view kKeymapNextDoc     = "std:xudu/next_doc";
inline constexpr std::string_view kKeymapPrevDoc     = "std:xudu/prev_doc";
inline constexpr std::string_view kKeymapDoc1        = "std:xudu/doc_1";
inline constexpr std::string_view kKeymapDoc2        = "std:xudu/doc_2";
inline constexpr std::string_view kKeymapDoc3        = "std:xudu/doc_3";
inline constexpr std::string_view kKeymapDoc4        = "std:xudu/doc_4";
inline constexpr std::string_view kKeymapDoc5        = "std:xudu/doc_5";
inline constexpr std::string_view kKeymapDoc6        = "std:xudu/doc_6";
inline constexpr std::string_view kKeymapDoc7        = "std:xudu/doc_7";
inline constexpr std::string_view kKeymapDoc8        = "std:xudu/doc_8";
inline constexpr std::string_view kKeymapDoc9        = "std:xudu/doc_9";
inline constexpr std::string_view kKeymapBack        = "std:xudu/back";
inline constexpr std::string_view kKeymapNewDoc      = "std:xudu/new_doc";
inline constexpr std::string_view kKeymapForward     = "std:xudu/forward";
inline constexpr std::string_view kKeymapOpenDoc     = "std:xudu/open_doc";
inline constexpr std::string_view kKeymapOnionSkin   = "std:xudu/onion_skin";
inline constexpr std::string_view kKeymapPouchToggle = "std:xudu/pouch_toggle";
inline constexpr std::string_view kKeymapPouchToggleF2 =
    "std:xudu/pouch_toggle_f2";
inline constexpr std::string_view kKeymapTelescopeToggle =
    "std:xudu/telescope_toggle";
inline constexpr std::string_view kKeymapTelescopeToggleF3 =
    "std:xudu/telescope_toggle_f3";
inline constexpr std::string_view kKeymapTensionPhysicsToggle =
    "std:xudu/tension_physics_toggle";
inline constexpr std::string_view kKeymapUnlockTranscopyrightF5 =
    "std:xudu/unlock_transcopyright_f5";
inline constexpr std::string_view kKeymapUnlockTranscopyrightCtrlU =
    "std:xudu/unlock_transcopyright_ctrl_u";
inline constexpr std::string_view kKeymapScrubForward =
    "std:xudu/scrub_forward";
inline constexpr std::string_view kKeymapScrubBackward =
    "std:xudu/scrub_backward";
inline constexpr std::string_view kKeymapTransclude = "std:xudu/transclude";
inline constexpr std::string_view kKeymapXanalink   = "std:xudu/xanalink";
inline constexpr std::string_view kKeymapCancelLink = "std:xudu/cancel_link";
inline constexpr std::string_view kKeymapBeams      = "std:xudu/beams";
inline constexpr std::string_view kKeymapSworph     = "std:xudu/sworph";
inline constexpr std::string_view kKeymapPublish    = "std:xudu/publish";
inline constexpr std::string_view kKeymapHistory    = "std:xudu/history";
inline constexpr std::string_view kKeymapDelete     = "std:xudu/delete";
inline constexpr std::string_view kKeymapPageBreak  = "std:xudu/page_break";
inline constexpr std::string_view kKeymapHypertimeMap =
    "std:xudu/hypertime_map";
inline constexpr std::string_view kKeymapRadialMenu = "std:xudu/radial_menu";

// Keymap - Zigzag Visualizer & Pure Vortex Actions
inline constexpr std::string_view kKeymapViewModeContent1 =
    "std:ui/view_mode_content_1";
inline constexpr std::string_view kKeymapViewModeContentV =
    "std:ui/view_mode_content_v";
inline constexpr std::string_view kKeymapViewModeTopology =
    "std:ui/view_mode_topology";
inline constexpr std::string_view kKeymapViewModeTopologyT =
    "std:ui/view_mode_topology_t";
inline constexpr std::string_view kKeymapBundleExecution =
    "std:ui/bundle_execution";
inline constexpr std::string_view kKeymapBundleScope = "std:ui/bundle_scope";
inline constexpr std::string_view kKeymapBundleContract =
    "std:ui/bundle_contract";
inline constexpr std::string_view kKeymapBundleLogic  = "std:ui/bundle_logic";
inline constexpr std::string_view kKeymapBundleStdlib = "std:ui/bundle_stdlib";
inline constexpr std::string_view kKeymapBundleCycle  = "std:ui/bundle_cycle";
inline constexpr std::string_view kKeymapTogglePalette =
    "std:ui/toggle_palette";
inline constexpr std::string_view kKeymapVqlTranslateAttach =
    "std:ui/vql_translate_attach";
inline constexpr std::string_view kKeymapToggleCommandBar =
    "std:ui/toggle_command_bar";
inline constexpr std::string_view kKeymapOpenCommandBarSlash =
    "std:ui/open_command_bar_slash";
inline constexpr std::string_view kKeymapOpenCommandBarColon =
    "std:ui/open_command_bar_colon";
inline constexpr std::string_view kKeymapConfirmAction =
    "std:ui/confirm_action";
inline constexpr std::string_view kKeymapDismissOverlay =
    "std:ui/dismiss_overlay";
inline constexpr std::string_view kKeymapStepXPos = "std:nav/step_x_pos";
inline constexpr std::string_view kKeymapStepXNeg = "std:nav/step_x_neg";
inline constexpr std::string_view kKeymapStepYPos = "std:nav/step_y_pos";
inline constexpr std::string_view kKeymapStepYNeg = "std:nav/step_y_neg";
inline constexpr std::string_view kKeymapStepZPos = "std:nav/step_z_pos";
inline constexpr std::string_view kKeymapStepZNeg = "std:nav/step_z_neg";
inline constexpr std::string_view kKeymapSwapXY   = "std:ui/swap_axes";
inline constexpr std::string_view kKeymapCycleDimsForward =
    "std:ui/cycle_dims_forward";
inline constexpr std::string_view kKeymapCycleDimsBackward =
    "std:ui/cycle_dims_backward";
inline constexpr std::string_view kKeymapJumpHome = "std:nav/jump_home";
inline constexpr std::string_view kKeymapHopHead  = "std:nav/hop_head";
inline constexpr std::string_view kKeymapHopTail  = "std:nav/hop_tail";
inline constexpr std::string_view kKeymapDuplicateFocusCell =
    "std:zigzag/duplicate";
inline constexpr std::string_view kKeymapRasterizePrint =
    "std:zigzag/rasterize_print";
inline constexpr std::string_view kKeymapExportLinkPackage =
    "std:zigzag/export_link_package";
inline constexpr std::string_view kKeymapInsertCellXPos =
    "std:zigzag/insert_cell_x_pos";
inline constexpr std::string_view kKeymapInsertCellXNeg =
    "std:zigzag/insert_cell_x_neg";
inline constexpr std::string_view kKeymapInsertCellYPos =
    "std:zigzag/insert_cell_y_pos";
inline constexpr std::string_view kKeymapInsertCellYNeg =
    "std:zigzag/insert_cell_y_neg";
inline constexpr std::string_view kKeymapUnlinkXPos = "std:zigzag/unlink_x_pos";
inline constexpr std::string_view kKeymapUnlinkXNeg = "std:zigzag/unlink_x_neg";
inline constexpr std::string_view kKeymapDeleteFocusCell =
    "std:zigzag/delete_focus_cell";
inline constexpr std::string_view kKeymapDeleteFocusCellBksp =
    "std:zigzag/delete_focus_cell_bksp";
inline constexpr std::string_view kKeymapSaveStore = "std:zigzag/save_store";

// Keymap - Xuzz Zigzag Presentation Actions
inline constexpr std::string_view kKeymapZigzagTogglePalette =
    "std:ui/zigzag_toggle_palette";
inline constexpr std::string_view kKeymapZigzagVqlTranslateAttach =
    "std:ui/zigzag_vql_translate_attach";
inline constexpr std::string_view kKeymapZigzagToggleCommandBar =
    "std:ui/zigzag_toggle_command_bar";
inline constexpr std::string_view kKeymapZigzagOpenCommandBarSlash =
    "std:ui/zigzag_open_command_bar_slash";
inline constexpr std::string_view kKeymapZigzagOpenCommandBarColon =
    "std:ui/zigzag_open_command_bar_colon";
inline constexpr std::string_view kKeymapZigzagViewModeContent =
    "std:ui/zigzag_view_mode_content";
inline constexpr std::string_view kKeymapZigzagViewModeTopology =
    "std:ui/zigzag_view_mode_topology";
inline constexpr std::string_view kKeymapZigzagBundleExecution =
    "std:ui/zigzag_bundle_execution";
inline constexpr std::string_view kKeymapZigzagBundleScope =
    "std:ui/zigzag_bundle_scope";
inline constexpr std::string_view kKeymapZigzagBundleContract =
    "std:ui/zigzag_bundle_contract";
inline constexpr std::string_view kKeymapZigzagBundleLogic =
    "std:ui/zigzag_bundle_logic";
inline constexpr std::string_view kKeymapZigzagBundleStdlib =
    "std:ui/zigzag_bundle_stdlib";
inline constexpr std::string_view kKeymapZigzagBundleCycle =
    "std:ui/zigzag_bundle_cycle";
inline constexpr std::string_view kKeymapZigzagSwapXY = "std:ui/zigzag_swap_xy";
inline constexpr std::string_view kKeymapZigzagCycleDimsForward =
    "std:ui/zigzag_cycle_dims_forward";
inline constexpr std::string_view kKeymapZigzagCycleDimsBackward =
    "std:ui/zigzag_cycle_dims_backward";
inline constexpr std::string_view kKeymapZigzagJumpHome =
    "std:nav/zigzag_jump_home";
inline constexpr std::string_view kKeymapZigzagHopHead =
    "std:nav/zigzag_hop_head";
inline constexpr std::string_view kKeymapZigzagHopTail =
    "std:nav/zigzag_hop_tail";
inline constexpr std::string_view kKeymapZigzagDuplicateCell =
    "std:zigzag/zigzag_duplicate_cell";
inline constexpr std::string_view kKeymapZigzagSaveStore =
    "std:zigzag/zigzag_save_store";
inline constexpr std::string_view kKeymapZigzagStepXPos =
    "std:nav/zigzag_step_x_pos";
inline constexpr std::string_view kKeymapZigzagStepXNeg =
    "std:nav/zigzag_step_x_neg";
inline constexpr std::string_view kKeymapZigzagStepYPos =
    "std:nav/zigzag_step_y_pos";
inline constexpr std::string_view kKeymapZigzagStepYNeg =
    "std:nav/zigzag_step_y_neg";
inline constexpr std::string_view kKeymapZigzagStepZPos =
    "std:nav/zigzag_step_z_pos";
inline constexpr std::string_view kKeymapZigzagStepZNeg =
    "std:nav/zigzag_step_z_neg";

// Keymap - Xuzz selected-link navigation. Activity Back is its own name,
// apart from kKeymapBack/kKeymapForward, which walk document microversions.
inline constexpr std::string_view kKeymapLinkNext = "std:xuzz/link_next";
inline constexpr std::string_view kKeymapLinkPrevious =
    "std:xuzz/link_previous";
inline constexpr std::string_view kKeymapLinkMemberNext =
    "std:xuzz/link_member_next";
inline constexpr std::string_view kKeymapLinkMemberPrevious =
    "std:xuzz/link_member_previous";
inline constexpr std::string_view kKeymapLinkOccurrenceNext =
    "std:xuzz/link_occurrence_next";
inline constexpr std::string_view kKeymapLinkOccurrencePrevious =
    "std:xuzz/link_occurrence_previous";
inline constexpr std::string_view kKeymapLinkCross   = "std:xuzz/link_cross";
inline constexpr std::string_view kKeymapLinkEnter   = "std:xuzz/link_enter";
inline constexpr std::string_view kKeymapLinkOrigin  = "std:xuzz/link_origin";
inline constexpr std::string_view kKeymapLinkDismiss = "std:xuzz/link_dismiss";
inline constexpr std::string_view kKeymapOverviewToggle =
    "std:xudu/overview_toggle";
inline constexpr std::string_view kKeymapActivityBack =
    "std:xuzz/activity_back";
inline constexpr std::string_view kKeymapFocusToggle = "std:xuzz/focus_toggle";

// Pouches
inline constexpr std::string_view kPouchZoneToLinkLeft  = "zone.to_link_left";
inline constexpr std::string_view kPouchZoneToLinkRight = "zone.to_link_right";
inline constexpr std::string_view kPouchZoneNotes       = "zone.notes";
inline constexpr std::string_view kPouchZoneScratch     = "zone.scratch";
} // namespace settings

using CellValue = std::variant<double, std::int64_t, bool, std::string>;

struct SettingSchemaShape {
  std::vector<std::string> expectedTypes;
  std::vector<CellValue> defaultValues;
};

struct SettingSchema {
  std::vector<SettingSchemaShape> alternatives;
};

struct SettingValue {
  std::vector<CellValue> elements;
  std::vector<zigzag::CellRef> valueCells;

  // Every std::get below is guarded by the holds_alternative check
  // immediately preceding it, so bad_variant_access can never actually
  // fire; only std::stod's already-caught throw could otherwise escape.
  // NOLINTNEXTLINE(bugprone-exception-escape)
  [[nodiscard]] double asDouble(const std::size_t idx = 0,
                                const double fallback = 0.0) const noexcept {
    if (idx >= elements.size()) {
      return fallback;
    }
    const auto &el = elements[idx];
    if (std::holds_alternative<double>(el)) {
      return std::get<double>(el);
    }
    if (std::holds_alternative<std::int64_t>(el)) {
      return static_cast<double>(std::get<std::int64_t>(el));
    }
    if (std::holds_alternative<bool>(el)) {
      return std::get<bool>(el) ? 1.0 : 0.0;
    }
    if (std::holds_alternative<std::string>(el)) {
      try {
        return std::stod(std::get<std::string>(el));
      } catch (...) {
        return fallback;
      }
    }
    return fallback;
  }

  // Every std::get below is guarded by the holds_alternative check
  // immediately preceding it, so bad_variant_access can never actually
  // fire; only std::stoll's already-caught throw could otherwise escape.
  [[nodiscard]] std::int64_t
  asInt64(const std::size_t idx       = 0, // NOLINT(bugprone-exception-escape)
          const std::int64_t fallback = 0) const noexcept {
    if (idx >= elements.size()) {
      return fallback;
    }
    const auto &el = elements[idx];
    if (std::holds_alternative<std::int64_t>(el)) {
      return std::get<std::int64_t>(el);
    }
    if (std::holds_alternative<double>(el)) {
      return static_cast<std::int64_t>(std::get<double>(el));
    }
    if (std::holds_alternative<bool>(el)) {
      return std::get<bool>(el) ? 1 : 0;
    }
    if (std::holds_alternative<std::string>(el)) {
      try {
        return std::stoll(std::get<std::string>(el));
      } catch (...) {
        return fallback;
      }
    }
    return fallback;
  }

  // Every std::get below is guarded by the holds_alternative check
  // immediately preceding it, so bad_variant_access can never actually fire.
  // NOLINTNEXTLINE(bugprone-exception-escape)
  [[nodiscard]] bool asBool(const std::size_t idx = 0,
                            const bool fallback   = false) const noexcept {
    if (idx >= elements.size()) {
      return fallback;
    }
    const auto &el = elements[idx];
    if (std::holds_alternative<bool>(el)) {
      return std::get<bool>(el);
    }
    if (std::holds_alternative<std::int64_t>(el)) {
      return std::get<std::int64_t>(el) != 0;
    }
    if (std::holds_alternative<double>(el)) {
      return std::get<double>(el) != 0.0;
    }
    if (std::holds_alternative<std::string>(el)) {
      return std::get<std::string>(el) == "true" ||
             std::get<std::string>(el) == "1";
    }
    return fallback;
  }

  [[nodiscard]] std::string
  asString(const std::size_t idx           = 0,
           const std::string_view fallback = "") const {
    if (idx >= elements.size()) {
      return std::string{fallback};
    }
    const auto &el = elements[idx];
    if (std::holds_alternative<std::string>(el)) {
      return std::get<std::string>(el);
    }
    if (std::holds_alternative<bool>(el)) {
      return std::get<bool>(el) ? "true" : "false";
    }
    if (std::holds_alternative<std::int64_t>(el)) {
      return std::to_string(std::get<std::int64_t>(el));
    }
    if (std::holds_alternative<double>(el)) {
      const double d = std::get<double>(el);
      if (d == static_cast<double>(static_cast<std::int64_t>(d))) {
        return std::to_string(static_cast<std::int64_t>(d));
      }
      return std::to_string(d);
    }
    return std::string{fallback};
  }
};

struct SettingEntry {
  std::string name;
  std::vector<std::string> groupPath;
  std::string notes;
  SettingSchema schema;
  SettingValue value;
  zigzag::CellRef nameCell{zigzag::noCell};
  zigzag::CellRef cloneCell{zigzag::noCell};
  bool isValid{true};
  std::string validationError;
};

struct SettingGroup {
  std::string name;
  zigzag::CellRef groupCell{zigzag::noCell};
  std::vector<std::string> memberSettingNames;
  std::vector<SettingGroup> childSubgroups;
};

struct SettingSpec {
  std::string name;
  std::string notes;
  std::vector<SettingSchemaShape> schemas;
};

class SystemStoreModel {
public:
  [[nodiscard]] static SystemStoreModel
  fromStore(const Store &store, const MicroversionId &version = {});

  [[nodiscard]] static SystemStoreModel
  fromManifold(const zigzag::Manifold &manifold,
               zigzag::CellRef homeCell = zigzag::noCell,
               const SpanReader *reader = nullptr);

  [[nodiscard]] bool isValid() const noexcept { return isValid_; }
  [[nodiscard]] const std::string &validationError() const noexcept {
    return error_;
  }
  [[nodiscard]] const std::string &storeDescription() const noexcept {
    return storeDesc_;
  }
  [[nodiscard]] const std::vector<SettingGroup> &groups() const noexcept {
    return groups_;
  }
  [[nodiscard]] const std::vector<SettingEntry> &settings() const noexcept {
    return settings_;
  }
  [[nodiscard]] gleditor::cpp26::optional<const SettingEntry &>
  find(std::string_view name) const noexcept;

  // Generic value queries
  [[nodiscard]] double getDouble(std::string_view name,
                                 double fallback = 0.0) const;
  [[nodiscard]] std::int64_t getInt64(std::string_view name,
                                      std::int64_t fallback = 0) const;
  [[nodiscard]] bool getBool(std::string_view name,
                             bool fallback = false) const;
  [[nodiscard]] std::string getString(std::string_view name,
                                      std::string_view fallback = "") const;
  [[nodiscard]] std::vector<CellValue> getValues(std::string_view name) const;
  [[nodiscard]] std::vector<double> getDoubleList(std::string_view name) const;
  [[nodiscard]] std::vector<std::int64_t>
  getInt64List(std::string_view name) const;
  [[nodiscard]] std::vector<std::string>
  getStringList(std::string_view name) const;

  // Validation
  [[nodiscard]] static bool validate(const SettingSchema &schema,
                                     std::span<const CellValue> values,
                                     std::string *errOut = nullptr);
  [[nodiscard]] static bool validate(const SettingSchema &schema,
                                     const SettingValue &val,
                                     std::string *errOut = nullptr);

  // Updates & Reset
  static MicroversionId updateSetting(Store &store,
                                      const MicroversionId &parent,
                                      std::string_view name,
                                      std::span<const CellValue> values,
                                      const zigzag::Manifold *known = nullptr);

  static MicroversionId resetToDefault(Store &store,
                                       const MicroversionId &parent,
                                       std::string_view name,
                                       const zigzag::Manifold *known = nullptr);

private:
  bool isValid_{true};
  std::string error_;
  std::string storeDesc_;
  std::vector<SettingGroup> groups_;
  std::vector<SettingEntry> settings_;
};

[[nodiscard]] std::vector<CellValue> getSetting(const Store &store,
                                                std::string_view name);

MicroversionId setSetting(Store &store, const MicroversionId &parent,
                          std::string_view name,
                          std::span<const CellValue> values,
                          const zigzag::Manifold *known = nullptr);

template <typename... Args>
MicroversionId setSetting(Store &store, const MicroversionId &parent,
                          std::string_view name, Args &&...args) {
  std::vector<CellValue> vals;
  vals.reserve(sizeof...(args));
  (vals.emplace_back(std::forward<Args>(args)), ...);
  return setSetting(store, parent, name, std::span<const CellValue>{vals});
}

MicroversionId resetSettingToDefault(Store &store, const MicroversionId &parent,
                                     std::string_view name,
                                     const zigzag::Manifold *known = nullptr);

MicroversionId initializeSystemStoreGenesis(Store &store, SystemDocKind kind,
                                            const MicroversionId &parent = {});

MicroversionId ensureSetting(Store &store, const MicroversionId &parent,
                             const SettingSpec &spec,
                             const zigzag::Manifold *known = nullptr);

MicroversionId ensureAllSettings(Store &store, const MicroversionId &parent,
                                 SystemDocKind kind);
MicroversionId ensureAllSettings(Store &store, SystemDocKind kind);

void initializeSystemStore(Store &store, SystemDocKind kind);

[[nodiscard]] std::vector<SettingSpec> defaultSettingSpecs(SystemDocKind kind);

/// Keyboard scopes a key binding can be confined to (gleditor::Command::scope):
/// the pane that has the keyboard.
inline constexpr std::string_view kKeyScopeDocument = "document";
inline constexpr std::string_view kKeyScopeZigzag   = "zigzag";

/**
 * @brief The scope @p action's binding is live in; empty for anywhere.
 *
 * ZigZag's bare-key set (std:nav/*, std:ui/*, std:zigzag/*) steps, edits and
 * cycles cells with the arrows, letters and Space a document needs for text,
 * so in xuzz it only reaches the keyboard while ZigZag has it. Its
 * Alt-prefixed zigzag_* twins are how a reader in a document reaches ZigZag,
 * and are live anywhere. std:edit/* is caret movement, a document's.
 */
[[nodiscard]] std::string_view keymapScope(std::string_view action);

[[nodiscard]] std::string_view canonicalKeymapAction(std::string_view action);
[[nodiscard]] std::string_view legacyKeymapAction(std::string_view action);

struct KeymapConfig {
  std::vector<std::pair<std::string, std::string>> bindings;
  [[nodiscard]] std::optional<std::string>
  bindingFor(std::string_view action) const;

  [[nodiscard]] static KeymapConfig fromStore(const Store &store);
};

struct SettingsConfig {
  float fontSize{16.0F};
  std::string fontFamily{"Monospace"};
  float lineHeight{1.4F};
  std::string theme{"system"};
  std::uint32_t autoSaveSeconds{5};
  std::vector<double> themeBackgroundRgb{0.1, 0.1, 0.1};

  [[nodiscard]] static SettingsConfig fromStore(const Store &store);
};

enum class ToastAnchor : std::uint8_t {
  TopRight,
  TopLeft,
  BottomRight,
  BottomLeft,
  TopCenter
};

enum class PouchDock : std::uint8_t { Left, Right };

struct PhysicsConfig {
  float kRepel{4500.0F};
  float kPlane{14.0F};
  float kAlign{28.0F};
  float kTier{12.0F};
  float kDamping{7.5F};
  float backgroundDepthZ{-40.0F};
  float defaultGap{8.0F};
  float settleVelocityThreshold{0.02F};
  float maxForce{10000.0F};
  float maxVelocity{1000.0F};
  float timeStep{0.016F};

  [[nodiscard]] TensionParams toTensionParams() const noexcept {
    return TensionParams{
        .kRepel                  = kRepel,
        .kPlane                  = kPlane,
        .kAlign                  = kAlign,
        .kTier                   = kTier,
        .kDamping                = kDamping,
        .backgroundDepthZ        = backgroundDepthZ,
        .defaultGap              = defaultGap,
        .settleVelocityThreshold = settleVelocityThreshold,
        .maxForce                = maxForce,
        .maxVelocity             = maxVelocity,
        .timeStep                = timeStep,
    };
  }

  [[nodiscard]] static PhysicsConfig
  fromTensionParams(const TensionParams &tp) noexcept {
    return PhysicsConfig{
        .kRepel                  = tp.kRepel,
        .kPlane                  = tp.kPlane,
        .kAlign                  = tp.kAlign,
        .kTier                   = tp.kTier,
        .kDamping                = tp.kDamping,
        .backgroundDepthZ        = tp.backgroundDepthZ,
        .defaultGap              = tp.defaultGap,
        .settleVelocityThreshold = tp.settleVelocityThreshold,
        .maxForce                = tp.maxForce,
        .maxVelocity             = tp.maxVelocity,
        .timeStep                = tp.timeStep,
    };
  }
};

struct BeamConfig {
  std::size_t bandStrandLimit{7};
  float bandStrandPitch{2.2F};
  float bandFillAlpha{0.85F};
  float stubWidthOfBeam{1.35F};
  float stubMinOfLine{0.9F};
  float marginKerf{0.04F};
  float bypassDepthPerDoc{-20.0F};
  float bypassDepthLimit{-120.0F};
  std::size_t bypassSegments{9};
  bool loomBundlingEnabled{true};
  float loomAlpha{0.35F};
  float loomHoverAlpha{1.0F};
  /// Deterministic per-beam Z nudge amplitude, so beams that cross in screen
  /// space rarely land at exactly the same depth. See xanadu::linkZJitter().
  float zFightJitterAmplitude{0.6F};
  /// Z boost for the active/selected link's beam, kept well clear of
  /// zFightJitterAmplitude so the selected beam always renders in front.
  float activeZBoost{4.0F};
};

/// Presentation policy for Zigzag's content and topology projections.
struct ZigzagPresentationConfig {
  float cellHorizontalPaddingPx{8.0F};
  float cellVerticalPaddingPx{6.0F};
  float cellBandGapPx{4.0F};
  float contentMaxWidthPx{260.0F};
  float topologyMaxWidthPx{140.0F};
  float rankClearancePx{24.0F};
  float hudHorizontalPaddingPx{16.0F};
  float hudVerticalPaddingPx{8.0F};
  float hudColumnGapPx{8.0F};
  float connectionBeamWidthPx{4.0F};
  /// Smallest on-screen height, in screen pixels, of a line of card text in
  /// a presentation embedded beside a page. Below it the presentation is
  /// scaled up; zero leaves it at the page's own scale.
  float minReadableTextPx{14.0F};

  // A byte-wise memcmp is unsafe here: +0.0F and -0.0F compare equal but
  // have different bit patterns, so field-wise == is the correct notion of
  // equality for a struct of floats.
  bool operator==(const ZigzagPresentationConfig &other) const noexcept {
    return cellHorizontalPaddingPx == other.cellHorizontalPaddingPx &&
           cellVerticalPaddingPx == other.cellVerticalPaddingPx &&
           cellBandGapPx == other.cellBandGapPx &&
           contentMaxWidthPx == other.contentMaxWidthPx &&
           topologyMaxWidthPx == other.topologyMaxWidthPx &&
           rankClearancePx == other.rankClearancePx &&
           hudHorizontalPaddingPx == other.hudHorizontalPaddingPx &&
           hudVerticalPaddingPx == other.hudVerticalPaddingPx &&
           hudColumnGapPx == other.hudColumnGapPx &&
           connectionBeamWidthPx == other.connectionBeamWidthPx &&
           minReadableTextPx == other.minReadableTextPx;
  }
};

struct LayoutConfig {
  std::uint32_t columns{2};
  float pageWidthPx{800.0F};
  float pageHeightPx{1000.0F};
  ToastAnchor toastAnchor{ToastAnchor::TopRight};
  float toastOffsetX{24.0F};
  float toastOffsetY{48.0F};
  PouchDock pouchDock{PouchDock::Right};
  float documentSpacingX{70.0F};
  /// See settings::kReadableTextPx. Zero keeps the old whole-page framing.
  float readableTextPx{16.0F};
  bool transclusionPrisms{true};
  bool transclusionLoom{true};
  bool xanalinkRibbons{true};
  PhysicsConfig physics{};
  BeamConfig beams{};
  ZigzagPresentationConfig zigzag{};
  BridgeRuntimeConfig bridge{};

  [[nodiscard]] static LayoutConfig fromStore(const Store &store);
};

/**
 * @brief The overview panel: the whole scene condensed into a corner, with
 *        the camera's view outlined on it.
 *
 * The companion of reading-first framing (LayoutConfig::readableTextPx): the
 * camera stays where text can be read, and this shows what lies around it.
 * Defaults here are the ones defaultSettingSpecs() seeds system://ui with.
 */
struct OverviewConfig {
  bool visible{true};
  float widthPx{220.0F};
  float heightPx{160.0F};
  /// Gap from the window's left edge.
  float leftPx{16.0F};
  /// Gap from the window's bottom edge, clear of a status line.
  float bottomPx{56.0F};
  std::uint32_t backgroundColour{0x0F172AE0U};
  std::uint32_t pageColour{0xCBD5E1FFU};
  /// The outline of what the camera shows.
  std::uint32_t viewportColour{0xFACC15FFU};
  /// The selected link's chosen places and the focused ZigZag card.
  std::uint32_t markColour{0xF472B6FFU};

  bool operator==(const OverviewConfig &) const = default;
};

/**
 * @brief How the selected-link panel looks
 *        (design/ui/prototypes/link-context.md).
 *
 * The defaults here are the ones defaultSettingSpecs() seeds system://ui
 * with, so the two cannot drift.
 */
struct LinkPanelConfig {
  std::string font{"Sans 10"};
  /// Gap between the panel and the window's right edge.
  float marginPx{16.0F};
  /// Gap between the panel and the window's top edge: clear of the document
  /// tab bar, which the panel must not cover.
  float topPx{44.0F};
  float paddingPx{10.0F};
  float lineGapPx{4.0F};
  std::uint32_t backgroundColour{0x1E293BE6U};
  std::uint32_t textColour{0xE2E8F0FFU};
  /// Unset cursors, members not in view, and the origin line.
  std::uint32_t mutedColour{0x94A3B8FFU};
  /// Behind the panel's buttons.
  std::uint32_t buttonColour{0x334155FFU};
  /// Behind the chosen occurrence in the document text.
  std::uint32_t chosenHighlightColour{0xFACC1570U};
  /// Behind the chosen member's other occurrences.
  std::uint32_t memberHighlightColour{0xFACC1530U};

  bool operator==(const LinkPanelConfig &) const = default;
};

struct UIConfig {
  bool tabBarVisible{true};
  bool statusBarVisible{true};
  bool hypertimeMapVisible{false};
  std::string notificationPosition{"top-right"};
  std::uint32_t notificationDurationMs{3000};
  gleditor::RadialConfig radialMenu;
  LinkPanelConfig linkPanel;
  OverviewConfig overview;

  UIConfig();
  [[nodiscard]] static UIConfig fromStore(const Store &store);
};

struct DropZoneSpec {
  std::string id;
  std::string label{"Notes"};
  std::uint32_t auraColor{0x06B6D4FFU};
  float heightWeight{1.0F};
};

struct PouchConfig {
  std::vector<DropZoneSpec> zones;

  [[nodiscard]] static PouchConfig fromStore(const Store &store);
};

} // namespace xanadu

#endif // XUDU_SYSTEM_DOCS_H

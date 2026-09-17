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

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/tension_layout.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"
#include <gleditor/radial_menu.hpp>

namespace zigzag {
class Manifold;
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
  const auto ffPos     = docText.find('\f');
  const auto schemaPos = docText.find("Schema and Purpose");
  auto end             = ffPos;
  if (end == std::string_view::npos ||
      (schemaPos != std::string_view::npos && schemaPos < end)) {
    end = schemaPos;
  }
  if (end != std::string_view::npos) {
    auto res = docText.substr(0, end);
    if (!res.empty() && res.back() == '\f') {
      res.remove_suffix(1);
    }
    return res;
  }
  return docText;
}

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
inline constexpr std::string_view kColumns            = "columns";
inline constexpr std::string_view kPageWidthPx        = "pageWidthPx";
inline constexpr std::string_view kPageHeightPx       = "pageHeightPx";
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
inline constexpr std::string_view kZigzagConnectionBeamWidthPx =
    "zigzag.connectionBeamWidthPx";

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

// Keymap
inline constexpr std::string_view kKeymapNewDoc        = "new-doc";
inline constexpr std::string_view kKeymapOpenDoc       = "open-doc";
inline constexpr std::string_view kKeymapCloseDoc      = "close-doc";
inline constexpr std::string_view kKeymapForward       = "forward";
inline constexpr std::string_view kKeymapScrubForward  = "scrub-forward";
inline constexpr std::string_view kKeymapScrubBackward = "scrub-backward";
inline constexpr std::string_view kKeymapHypertimeMap  = "hypertime-map";
inline constexpr std::string_view kKeymapRadialMenu    = "radial-menu";

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

  [[nodiscard]] std::int64_t
  asInt64(const std::size_t idx       = 0,
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
  [[nodiscard]] const SettingEntry *find(std::string_view name) const noexcept;

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
  bool transclusionPrisms{true};
  bool transclusionLoom{true};
  bool xanalinkRibbons{true};
  PhysicsConfig physics{};
  BeamConfig beams{};
  ZigzagPresentationConfig zigzag{};

  [[nodiscard]] static LayoutConfig fromStore(const Store &store);
};

struct UIConfig {
  bool tabBarVisible{true};
  bool statusBarVisible{true};
  bool hypertimeMapVisible{false};
  std::string notificationPosition{"top-right"};
  std::uint32_t notificationDurationMs{3000};
  gleditor::RadialConfig radialMenu;

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

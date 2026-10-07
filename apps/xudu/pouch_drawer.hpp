/**
 * @file pouch_drawer.hpp
 * @brief Screen-edge Pouch Drawer overlay with partitioned drop zones and clasp
 * bench.
 */
#ifndef XUDU_POUCH_DRAWER_HPP
#define XUDU_POUCH_DRAWER_HPP

#include "common/xanadu/system_docs.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <gleditor/ui/overlay.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/canvas.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/modal_input.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/renderer.hpp>

#include "clasp_link_forge.hpp"
#include "common/xanadu/pouch_zone.hpp"
#include "session.hpp"

namespace xudu {
using namespace ::xanadu;

/**
 * @class PouchDrawer
 * @brief High-performance overlay drawer containing partitioned drop zones and
 * clasp assembly bench.
 */
class PouchDrawer : public gleditor::FrameContributor,
                    public gleditor::PickObserver,
                    public gleditor::ModalInput,
                    public gleditor::a11y::Source {
public:
  [[nodiscard]] bool grabbing() const override { return isOpen_.load(); }
  bool keyPressed(gleditor::Key key, gleditor::KeyMods) override;
  void textTyped(const std::string &) override {}
  bool pointerPick(const render::PickingResult &pick,
                   RenderState &state) override {
    return picked(pick, state);
  }

  enum class DockSide : std::uint8_t { Left, Right };

  static constexpr std::uint32_t kTagDrawerClose     = 7001U;
  static constexpr std::uint32_t kTagDrawerAddZone   = 7002U;
  static constexpr std::uint32_t kTagDrawerFlipDock  = 7003U;
  static constexpr std::uint32_t kTagDrawerNextZones = 7004U;
  static constexpr std::uint32_t kTagZoneClearBase   = 7100U;
  static constexpr std::uint32_t kTagItemBase        = 8000U;
  static constexpr std::uint32_t kTagItemDismissBase = 12000U;
  /// A card's insert button: the item into the document at the caret.
  static constexpr std::uint32_t kTagItemUseBase = 20000U;

  using SwingBackHandler = std::function<void(const PouchItem &)>;

  PouchDrawer(Session &session, RendererRef renderer, std::string fontName = {},
              DockSide side = DockSide::Left);
  ~PouchDrawer() override;

  // FrameContributor
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &pipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool busy() const override;

  // PickObserver
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;

  // Accessibility
  void describe(gleditor::a11y::Builder &into) override;
  [[nodiscard]] std::uint64_t accessibilityRevision() const override;
  bool performAction(std::uint64_t nodeId, gleditor::a11y::Action action,
                     std::string_view value) override;

  void setOpen(bool open, bool animated = true) noexcept;
  [[nodiscard]] bool isOpen() const noexcept { return isOpen_.load(); }
  void toggle() noexcept { setOpen(!isOpen_); }

  void setDockSide(DockSide side) noexcept;
  void setConfig(PouchPanelConfig config);
  [[nodiscard]] std::shared_ptr<const gleditor::ui::LayoutResult>
  focusLayout() const override;
  void focusedNodeChanged(std::uint32_t) override;
  bool activateNode(std::uint32_t) override;
  bool pointerEvent(const gleditor::ui::PointerEvent &) override;
  void focusChanged(bool) override;
  [[nodiscard]] std::optional<gleditor::InputArea> pointerArea() const override;
  [[nodiscard]] DockSide dockSide() const noexcept { return side_; }

  /// What a card's insert button does with its item: xudu transcludes it
  /// at the caret, which is how a pouch item is used later.
  void setUseHandler(SwingBackHandler handler) {
    useHandler_ = std::move(handler);
  }
  void setSwingBackHandler(SwingBackHandler handler) {
    swingBackHandler_ = std::move(handler);
  }

  // Zone Partition Management
  DropZone &addZone(DropZoneConfig config) {
    return pouchManager_.addZone(std::move(config));
  }
  bool removeZone(std::string_view id) { return pouchManager_.removeZone(id); }
  [[nodiscard]] gleditor::cpp26::optional<DropZone &>
  zoneById(std::string_view id) noexcept {
    return pouchManager_.zoneById(id);
  }
  [[nodiscard]] gleditor::cpp26::optional<const DropZone &>
  zoneById(std::string_view id) const noexcept {
    return pouchManager_.zoneById(id);
  }
  [[nodiscard]] gleditor::cpp26::optional<DropZone &>
  zoneAt(float screenX, float screenY) noexcept {
    return pouchManager_.zoneAt(screenX, screenY);
  }
  [[nodiscard]] const std::vector<std::unique_ptr<DropZone>> &
  zones() const noexcept {
    return pouchManager_.zones();
  }

  PouchManager &manager() noexcept { return pouchManager_; }
  [[nodiscard]] const PouchManager &manager() const noexcept {
    return pouchManager_;
  }

  LinkForgeWidget &forge() noexcept { return forgeWidget_; }
  [[nodiscard]] const LinkForgeWidget &forge() const noexcept {
    return forgeWidget_;
  }

  // Drag Interaction Hook
  bool handleGhostDrop(const PrimediaSpan &span, const std::string &preview,
                       const MicroversionId &sourceVer, float screenX,
                       float screenY, std::uint32_t docIndex = 0,
                       std::uint32_t charStart = 0, std::uint32_t charEnd = 0);

  bool
  handleCellDrop(const PrimediaSpan &span, const std::string &preview,
                 std::uint32_t cellRef, std::string_view rankCoord,
                 float screenX, float screenY, std::uint32_t sliceIndex = 0,
                 const std::optional<GlobalOpRef> &originOpRef = std::nullopt);

  [[nodiscard]] float currentWidth() const noexcept {
    return currentSlideWidth_;
  }

private:
  void layout(const gleditor::ui::UiMetrics &, const gleditor::ui::Theme &);
  void rebuildModels(const gleditor::ui::UiMetrics &,
                     const gleditor::ui::Theme &);
  void drainActions();
  float clampedWidth(float logical) const;
  void cancelResize();
  void requestWidth(float logical);
  struct ResizeGesture {
    std::uint32_t pointer{};
    float startX{}, originalWidth{}, startActualWidth{};
    DockSide side{};
  };
  struct WidthCommit {
    float logical{};
  };
  std::optional<ResizeGesture> resize_;
  std::optional<WidthCommit> widthCommit_;
  std::unique_ptr<gleditor::ui::ScreenOverlay> resizeHandle_;
  std::optional<gleditor::ui::Rect> resizeBounds_;
  gleditor::ui::WidgetId resizeId_{0x0F000000}, focusedNode_{};
  float handleWidthPx_{};
  float adaptiveWidthPx_{}, minimumWidthPx_{};
  std::array<float, 4> controlWidths_{};
  std::size_t headerRows_{2};
  bool forgeCompact_{};
  gleditor::text::ShapingCache measurement_;
  struct Action {
    std::uint32_t tag{};
    std::uint64_t item{};
    std::string zone;
    std::uint64_t epoch{}, forgeRevision{}, sessionGeneration{};
  };
  void enqueue(Action);
  gleditor::ui::WidgetId actionId(std::string key, Action);
  std::uint64_t contentStamp() const;

  Session &session_;
  RendererRef renderer_;
  std::string fontName_;
  DockSide side_{DockSide::Left};
  std::atomic<bool> isOpen_{false};
  mutable std::recursive_mutex guard_;
  PouchPanelConfig config_;
  std::uint64_t openEpoch_{1}, modelStamp_{};
  bool snapNextLayout_{};
  gleditor::ui::Theme legacyTheme_;
  std::optional<gleditor::ui::Theme> legacyBase_;
  std::optional<gleditor::ui::UiMetrics> preparedMetrics_;
  gleditor::ui::Theme preparedTheme_;
  std::unique_ptr<gleditor::ui::ScreenOverlay> header_;
  struct ZonePresentation {
    std::string zone;
    std::unique_ptr<gleditor::ui::ScreenOverlay> overlay;
    std::optional<gleditor::ui::Rect> bounds;
    gleditor::ui::Theme theme;
  };
  std::vector<ZonePresentation> presentations_;
  std::unordered_map<std::string, gleditor::ui::WidgetId> ids_;
  std::unordered_map<gleditor::ui::WidgetId, Action> actions_;
  std::unordered_map<gleditor::ui::WidgetId, std::vector<Action>> rowActions_;
  gleditor::ui::WidgetId nextId_{100};
  std::vector<Action> pending_;
  std::shared_ptr<const gleditor::ui::LayoutResult> focus_;
  float touchPx_{}, gapPx_{}, headerPx_{}, captionLinePx_{}, capTouchPx_{},
      zoneTouchPx_{};
  gleditor::ui::Theme compactTheme_;
  gleditor::ui::Theme handleTheme_;
  std::optional<gleditor::ui::Theme> compactSource_;
  std::optional<gleditor::ui::UiMetrics> compactMetrics_;
  std::uint64_t focusStamp_{};
  std::size_t zoneStart_{}, visibleZones_{1};
  std::unordered_map<std::string, std::size_t> zonePage_;
  std::optional<gleditor::ui::Rect> headerBounds_;
  render::RenderDevice *device_{};
  std::optional<render::PipelineDesc> pipeline_;
  float currentSlideWidth_{0.0F};
  float targetSlideWidth_{0.0F};

  PouchManager pouchManager_;
  LinkForgeWidget forgeWidget_;

  SwingBackHandler swingBackHandler_;
  SwingBackHandler useHandler_;
  std::uint64_t a11yRevision_{1};

  float drawerX_{0.0F};
  float drawerY_{0.0F};
  float drawerW_{0.0F};
  float drawerH_{0.0F};
};

} // namespace xudu

#endif // XUDU_POUCH_DRAWER_HPP

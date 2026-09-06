/**
 * @file pouch_drawer.hpp
 * @brief Screen-edge Pouch Drawer overlay with partitioned drop zones and clasp
 * bench.
 */
#ifndef XUDU_POUCH_DRAWER_HPP
#define XUDU_POUCH_DRAWER_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/canvas.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/renderer.hpp>

#include "clasp_link_forge.hpp"
#include "core/pouch_zone.hpp"
#include "session.hpp"

namespace xudu {

/**
 * @class PouchDrawer
 * @brief High-performance overlay drawer containing partitioned drop zones and
 * clasp assembly bench.
 */
class PouchDrawer : public gleditor::FrameContributor,
                    public gleditor::PickObserver,
                    public gleditor::a11y::Source {
public:
  enum class DockSide : std::uint8_t { Left, Right };

  static constexpr std::uint32_t kTagDrawerClose     = 7001U;
  static constexpr std::uint32_t kTagDrawerAddZone   = 7002U;
  static constexpr std::uint32_t kTagDrawerFlipDock  = 7003U;
  static constexpr std::uint32_t kTagZoneClearBase   = 7100U;
  static constexpr std::uint32_t kTagItemBase        = 8000U;
  static constexpr std::uint32_t kTagItemDismissBase = 12000U;

  using SwingBackHandler = std::function<void(const PouchItem &)>;

  PouchDrawer(Session &session, RendererRef renderer,
              std::string fontName = "Sans 10", DockSide side = DockSide::Left);
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

  void setOpen(bool open, bool animated = true) noexcept;
  [[nodiscard]] bool isOpen() const noexcept { return isOpen_; }
  void toggle() noexcept { setOpen(!isOpen_); }

  void setDockSide(DockSide side) noexcept { side_ = side; }
  [[nodiscard]] DockSide dockSide() const noexcept { return side_; }

  void setSwingBackHandler(SwingBackHandler handler) {
    swingBackHandler_ = std::move(handler);
  }

  // Zone Partition Management
  DropZone &addZone(DropZoneConfig config) {
    return pouchManager_.addZone(std::move(config));
  }
  bool removeZone(std::string_view id) { return pouchManager_.removeZone(id); }
  [[nodiscard]] DropZone *zoneById(std::string_view id) noexcept {
    return pouchManager_.zoneById(id);
  }
  [[nodiscard]] const DropZone *zoneById(std::string_view id) const noexcept {
    return pouchManager_.zoneById(id);
  }
  [[nodiscard]] DropZone *zoneAt(float screenX, float screenY) noexcept {
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

  [[nodiscard]] float currentWidth() const noexcept {
    return currentSlideWidth_;
  }

private:
  void layout(float screenWidth, float screenHeight);

  Session &session_;
  RendererRef renderer_;
  std::string fontName_;
  DockSide side_{DockSide::Left};
  bool isOpen_{false};
  float currentSlideWidth_{0.0F};
  float targetSlideWidth_{0.0F};
  static constexpr float kDrawerWidth = 320.0F;

  PouchManager pouchManager_;
  LinkForgeWidget forgeWidget_;
  std::unique_ptr<gleditor::Canvas> canvas_;

  SwingBackHandler swingBackHandler_;
  std::uint64_t a11yRevision_{1};

  float drawerX_{0.0F};
  float drawerY_{0.0F};
  float drawerW_{0.0F};
  float drawerH_{0.0F};
};

} // namespace xudu

#endif // XUDU_POUCH_DRAWER_HPP

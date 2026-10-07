/**
 * @file radial_menu.hpp
 * @brief 3D Radial Marking Menu for gesture-driven formatting and operations.
 */
#ifndef GLEDITOR_RADIAL_MENU_HPP
#define GLEDITOR_RADIAL_MENU_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/canvas.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/text/shaping_cache.hpp>
#include <gleditor/ui/focus_manager.hpp>
#include <gleditor/ui/layout.hpp>

struct RenderState;

namespace render {
class RenderDevice;
}

namespace gleditor {

/**
 * @struct RadialAction
 * @brief Descriptor for a single action in the radial marking menu.
 */
struct RadialAction {
  std::string id;
  std::string label;
  std::string desc;
  std::string icon;
  std::string action; // e.g. "format:bold", "align:centre", "op:pagebreak"
  std::vector<RadialAction> subActions;
  std::uint32_t color{0xF1F5F9FFU};
  std::uint32_t accentColor{0x38BDF8FFU};
  bool enabled{true};
  bool active{false};
  std::function<void()> onSelect;
};

/**
 * @struct RadialConfig
 * @brief User-configurable radial geometry and action list.
 */
struct RadialConfig {
  float radius{84.0F};
  float innerRadius{26.0F};
  std::vector<RadialAction> actions;

  [[nodiscard]] static RadialConfig createDefault();
};

/**
 * @class RadialMenu
 * @brief Overlay radial marking menu positioned at cursor or selection.
 */
class RadialMenu : public FrameContributor,
                   public PickObserver,
                   public ui::FocusScope,
                   public a11y::Source {
public:
  void cancel() override {
    keyPressed(gleditor::Key::Escape, gleditor::KeyMods::None);
  }

  bool keyPressed(const gleditor::ui::KeyEvent &event) override {
    return keyPressed(event.key, event.mods);
  }

  [[nodiscard]] bool active() const override { return isOpen(); }
  bool keyPressed(gleditor::Key key, gleditor::KeyMods);
  [[nodiscard]] std::shared_ptr<const ui::LayoutResult>
  focusLayout() const override;
  void focusedNodeChanged(std::uint32_t) override;
  bool activateNode(std::uint32_t) override;
  void textTyped(std::string_view) override {}
  [[nodiscard]] bool usesGpuPointerPicking() const override { return true; }
  bool pointerPick(const render::PickingResult &pick,
                   RenderState &state) override {
    return picked(pick, state);
  }

  static constexpr std::uint32_t kRadialTagBase = 0x8000U;
  static constexpr std::uint32_t kRadialTagHub  = 0x8050U;
  static constexpr std::uint32_t kRadialTagBack = 0x8051U;

  explicit RadialMenu(std::string aFontName = {});
  ~RadialMenu() override;

  RadialMenu(const RadialMenu &)            = delete;
  RadialMenu &operator=(const RadialMenu &) = delete;
  RadialMenu(RadialMenu &&)                 = delete;
  RadialMenu &operator=(RadialMenu &&)      = delete;

  // -- Configuration & Navigation ---------------------------------------------
  void setConfig(RadialConfig aConfig);
  [[nodiscard]] RadialConfig config() const {
    const std::scoped_lock lock(guard_);
    return config_;
  }

  void setRadius(float outer, float inner);
  [[nodiscard]] float radius() const noexcept {
    const std::scoped_lock lock(guard_);
    return config_.radius;
  }
  [[nodiscard]] float innerRadius() const noexcept {
    const std::scoped_lock lock(guard_);
    return config_.innerRadius;
  }

  void open(float screenX, float screenY, std::uint32_t aDocIndex = 0,
            std::uint32_t aCharOffset = 0, std::uint32_t aCharLength = 0);
  void openAtWindowCoords(float windowX, float windowY,
                          std::uint32_t aDocIndex   = 0,
                          std::uint32_t aCharOffset = 0,
                          std::uint32_t aCharLength = 0);
  void close();
  void toggle(float screenX, float screenY, std::uint32_t aDocIndex = 0,
              std::uint32_t aCharOffset = 0, std::uint32_t aCharLength = 0);
  [[nodiscard]] bool isOpen() const noexcept {
    const std::scoped_lock lock(guard_);
    return open_;
  }
  [[nodiscard]] float lastScreenWidth() const noexcept {
    const std::scoped_lock lock(guard_);
    return lastScreenWidth_;
  }
  [[nodiscard]] float lastScreenHeight() const noexcept {
    const std::scoped_lock lock(guard_);
    return lastScreenHeight_;
  }

  [[nodiscard]] float centerX() const noexcept {
    const std::scoped_lock lock(guard_);
    return centerX_;
  }
  [[nodiscard]] float centerY() const noexcept {
    const std::scoped_lock lock(guard_);
    return centerY_;
  }
  [[nodiscard]] std::uint32_t targetDocIndex() const noexcept {
    const std::scoped_lock lock(guard_);
    return targetDocIndex_;
  }
  [[nodiscard]] std::uint32_t targetCharOffset() const noexcept {
    const std::scoped_lock lock(guard_);
    return targetCharOffset_;
  }
  [[nodiscard]] std::uint32_t targetCharLength() const noexcept {
    const std::scoped_lock lock(guard_);
    return targetCharLength_;
  }

  void enterSubRadial(std::size_t actionIndex);
  void exitSubRadial();
  [[nodiscard]] bool inSubRadial() const noexcept {
    const std::scoped_lock lock(guard_);
    return inSubRadialLocked();
  }

  void setActionHandler(
      std::function<void(const std::string &id, const std::string &action,
                         std::uint32_t docIndex, std::uint32_t charOffset,
                         std::uint32_t charLength)>
          handler) {
    const std::scoped_lock lock(guard_);
    actionHandler_ = std::move(handler);
  }

  void setOpenOnRightClick(bool enable) noexcept {
    const std::scoped_lock lock(guard_);
    openOnRightClick_ = enable;
  }
  [[nodiscard]] bool openOnRightClick() const noexcept {
    const std::scoped_lock lock(guard_);
    return openOnRightClick_;
  }

  // -- FrameContributor -------------------------------------------------------
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &documentPipeline) override;
  void drawFrame(FrameContext &ctx) override;
  [[nodiscard]] bool busy() const override;

  // -- PickObserver -----------------------------------------------------------
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;

  // -- a11y::Source -----------------------------------------------------------
  void describe(a11y::Builder &into) override;
  bool performAction(std::uint64_t, a11y::Action, std::string_view) override;
  [[nodiscard]] std::uint64_t accessibilityRevision() const override {
    const std::scoped_lock lock(guard_);
    return revision_;
  }

  // -- Geometry Helper --------------------------------------------------------
  /// The sector of @p count, clockwise from North, that the direction
  /// (@p dx, @p dy) points into; nullopt for a menu with no sectors.
  [[nodiscard]] static std::optional<std::size_t>
  resolveSector(float dx, float dy, std::size_t count) noexcept;

  /// Retained pixel geometry, also inspectable without a graphics device.
  [[nodiscard]] std::shared_ptr<const ui::LayoutResult>
  prepareLayout(const ui::UiMetrics &, const ui::Theme &);
  [[nodiscard]] text::ShapingCache::Stats shapingStats() const {
    const std::scoped_lock lock(guard_);
    return shaping_.stats();
  }

private:
  struct ActionDispatch {
    bool handled{};
    std::function<void()> callback;
    ActionDispatch(bool handled = false, std::function<void()> callback = {})
        : handled(handled), callback(std::move(callback)) {}
  };
  static bool finishDispatch(ActionDispatch);
  [[nodiscard]] bool inSubRadialLocked() const noexcept {
    return inSubWheel_ && activeParentAction_ < config_.actions.size();
  }
  void openLocked(float, float, std::uint32_t, std::uint32_t, std::uint32_t);
  void closeLocked();
  void enterSubRadialLocked(std::size_t);
  void exitSubRadialLocked();
  std::shared_ptr<const ui::LayoutResult>
  prepareLayoutLocked(const ui::UiMetrics &, const ui::Theme &);
  ActionDispatch activateNodeLocked(std::uint32_t);
  ActionDispatch pickedLocked(const render::PickingResult &, RenderState &);
  struct PodLayout {
    std::size_t actionIndex{0};
    float x{0.0F};
    float y{0.0F};
    float width{0.0F};
    float height{0.0F};
    float angle{0.0F};
    float startAngle{0.0F};
    float endAngle{0.0F};
    float innerRadius{0.0F};
    float outerRadius{0.0F};
    std::uint32_t tag{0};
    std::string label;
    std::string desc;
    text::FittedText fitted;
  };

  void rebuildLayout(float screenW, float screenH);
  ActionDispatch selectActionLocked(std::size_t);
  static void drawDisc(Canvas &canvas, float cX, float cY, float radius,
                       std::uint32_t fillCol, std::uint32_t borderCol = 0,
                       float borderWidth = 0.0F, std::size_t slices = 24);
  static void drawWedge(Canvas &canvas, float cX, float cY, float rIn,
                        float rOut, float aStart, float aEnd,
                        std::uint32_t fillCol, std::uint32_t borderCol = 0,
                        float borderWidth = 0.0F);

  mutable std::mutex guard_;
  std::string fontName_;
  std::unique_ptr<Canvas> canvas_;
  render::RenderDevice *device_{};
  std::optional<render::PipelineDesc> pipeline_;
  ui::UiMetrics metrics_;
  ui::Theme theme_;
  std::shared_ptr<const ui::LayoutResult> layout_;
  text::ShapingCache shaping_;
  text::FontFacePtr font_;
  std::string resolvedFont_;
  std::string canvasFont_;
  text::FittedText hubFitted_;
  float outerRadiusPx_{84.0F}, innerRadiusPx_{26.0F};
  std::uint64_t preparedRevision_{}, drawnRevision_{};
  std::uint32_t focusedNode_{};
  std::uint32_t pickScope_{};
  std::shared_ptr<const std::vector<std::uint32_t>> pickingTargets_;
  std::uint64_t pickingRevision_{1}, builtPickingRevision_{};
  std::uint32_t nextPickingTarget_{};
  RadialConfig config_;

  bool open_{false};
  bool inSubWheel_{false};
  std::size_t activeParentAction_{0};

  float centerX_{0.0F};
  float centerY_{0.0F};
  std::uint32_t targetDocIndex_{0};
  std::uint32_t targetCharOffset_{0};
  std::uint32_t targetCharLength_{0};

  std::vector<PodLayout> currentPods_;
  float hubX_{0.0F};
  float hubY_{0.0F};
  float hubSize_{48.0F};
  float hubRadius_{24.0F};
  float lastScreenWidth_{0.0F};
  float lastScreenHeight_{0.0F};

  std::uint64_t revision_{1};
  bool openOnRightClick_{true};
  std::function<void(const std::string &, const std::string &, std::uint32_t,
                     std::uint32_t, std::uint32_t)>
      actionHandler_;
};

} // namespace gleditor

#endif // GLEDITOR_RADIAL_MENU_HPP

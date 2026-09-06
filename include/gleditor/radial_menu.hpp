/**
 * @file radial_menu.hpp
 * @brief 3D Radial Marking Menu for gesture-driven formatting and operations.
 */
#ifndef GLEDITOR_RADIAL_MENU_HPP
#define GLEDITOR_RADIAL_MENU_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/canvas.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/render/types.hpp>

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
                   public a11y::Source {
public:
  static constexpr std::uint32_t kRadialTagBase = 0x8000U;
  static constexpr std::uint32_t kRadialTagHub  = 0x8050U;
  static constexpr std::uint32_t kRadialTagBack = 0x8051U;

  explicit RadialMenu(std::string aFontName = "Sans 10");
  ~RadialMenu() override;

  RadialMenu(const RadialMenu &)            = delete;
  RadialMenu &operator=(const RadialMenu &) = delete;
  RadialMenu(RadialMenu &&)                 = delete;
  RadialMenu &operator=(RadialMenu &&)      = delete;

  // -- Configuration & Navigation ---------------------------------------------
  void setConfig(RadialConfig aConfig);
  [[nodiscard]] const RadialConfig &config() const noexcept { return config_; }

  void setRadius(float outer, float inner);
  [[nodiscard]] float radius() const noexcept { return config_.radius; }
  [[nodiscard]] float innerRadius() const noexcept {
    return config_.innerRadius;
  }

  void open(float screenX, float screenY, std::uint32_t aDocIndex = 0,
            std::uint32_t aCharOffset = 0, std::uint32_t aCharLength = 0);
  void openAtWindowCoords(float windowX, float windowY,
                          std::uint32_t aDocIndex = 0,
                          std::uint32_t aCharOffset = 0,
                          std::uint32_t aCharLength = 0);
  void close();
  void toggle(float screenX, float screenY, std::uint32_t aDocIndex = 0,
              std::uint32_t aCharOffset = 0, std::uint32_t aCharLength = 0);
  [[nodiscard]] bool isOpen() const noexcept { return open_; }
  [[nodiscard]] float lastScreenWidth() const noexcept {
    return lastScreenWidth_;
  }
  [[nodiscard]] float lastScreenHeight() const noexcept {
    return lastScreenHeight_;
  }

  [[nodiscard]] float centerX() const noexcept { return centerX_; }
  [[nodiscard]] float centerY() const noexcept { return centerY_; }
  [[nodiscard]] std::uint32_t targetDocIndex() const noexcept {
    return targetDocIndex_;
  }
  [[nodiscard]] std::uint32_t targetCharOffset() const noexcept {
    return targetCharOffset_;
  }
  [[nodiscard]] std::uint32_t targetCharLength() const noexcept {
    return targetCharLength_;
  }

  void enterSubRadial(std::size_t actionIndex);
  void exitSubRadial();
  [[nodiscard]] bool inSubRadial() const noexcept {
    return inSubWheel_ && activeParentAction_ < config_.actions.size();
  }

  void setActionHandler(
      std::function<void(const std::string &id, const std::string &action,
                         std::uint32_t docIndex, std::uint32_t charOffset,
                         std::uint32_t charLength)>
          handler) {
    actionHandler_ = std::move(handler);
  }

  void setOpenOnRightClick(bool enable) noexcept { openOnRightClick_ = enable; }
  [[nodiscard]] bool openOnRightClick() const noexcept {
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
  [[nodiscard]] std::uint64_t accessibilityRevision() const override {
    return revision_;
  }

  // -- Geometry Helper --------------------------------------------------------
  [[nodiscard]] static int resolveSector(float dx, float dy,
                                         std::size_t count) noexcept;

private:
  struct PodLayout {
    std::size_t actionIndex{0};
    float x{0.0F};
    float y{0.0F};
    float width{0.0F};
    float height{0.0F};
    float angle{0.0F};
    std::uint32_t tag{0};
    std::string label;
    std::string desc;
  };

  void rebuildLayout(float screenW, float screenH);

  std::string fontName_;
  std::unique_ptr<Canvas> canvas_;
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

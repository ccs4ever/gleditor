/**
 * @file clasp_link_forge.hpp
 * @brief Tripartite Clasp Assembly Bench widget for N x M asymmetric
 * hyperlinking.
 */
#ifndef XUDU_CLASP_LINK_FORGE_HPP
#define XUDU_CLASP_LINK_FORGE_HPP

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <gleditor/canvas.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/ui/overlay.hpp>

#include "common/xanadu/ops.hpp"
#include "common/xanadu/pouch_zone.hpp"
#include "session.hpp"

struct RenderState;

namespace xudu {
using namespace ::xanadu;

/**
 * @class LinkForgeWidget
 * @brief Tripartite link creation control: Homestead (Left), Relation Nexus,
 * Toward (Right).
 */
class LinkForgeWidget {
public:
  static constexpr std::uint32_t kTagClaspLeftDrop     = 7010U;
  static constexpr std::uint32_t kTagClaspRightDrop    = 7011U;
  static constexpr std::uint32_t kTagClaspTypeSelector = 7012U;
  static constexpr std::uint32_t kTagClaspTierSelector = 7013U;
  static constexpr std::uint32_t kTagClaspForgeButton  = 7014U;
  static constexpr std::uint32_t kTagClaspClearLeft    = 7015U;
  static constexpr std::uint32_t kTagClaspClearRight   = 7016U;

  LinkForgeWidget();

  void setGeometry(float x, float y, float width, float height) noexcept;
  [[nodiscard]] std::shared_ptr<const gleditor::ui::WidgetScene>
  prepareBench(const gleditor::ui::UiMetrics &, const gleditor::ui::Theme &,
               gleditor::ui::Rect bounds, bool compact = false);
  void deviceReady(render::RenderDevice &, const render::PipelineDesc &);
  void drawPrepared(gleditor::FrameContext &);
  void setActionHandler(std::function<void(std::uint32_t)>);
  [[nodiscard]] auto snapshot() const { return presentation_.snapshot(); }
  [[nodiscard]] auto focusLayout() const { return presentation_.focusLayout(); }
  [[nodiscard]] auto &presentation() noexcept { return presentation_; }
  void describe(gleditor::a11y::Builder &builder) {
    presentation_.describe(builder);
  }
  bool performAction(std::uint64_t id, gleditor::a11y::Action action,
                     std::string_view value) {
    if (modelDirty_) return false;
    return presentation_.performAction(id, action, value);
  }
  bool picked(const render::PickingResult &pick, RenderState &state) {
    if ((modelDirty_ || !presentation_.visible()) &&
        pick.tag.kind == render::tagKindOverlay && pick.overlayWidgetId) {
      const auto scene = snapshot();
      if (scene && scene->find(*pick.overlayWidgetId)) return true;
    }
    return presentation_.picked(pick, state);
  }
  [[nodiscard]] std::uint64_t accessibilityRevision() const {
    return presentation_.accessibilityRevision();
  }
  void setVisible(bool visible) {
    if (presentation_.visible() != visible) {
      changed();
      freshSelectors_ = true;
    }
    presentation_.setVisible(visible);
  }
  [[nodiscard]] std::uint64_t semanticRevision() const noexcept {
    return semanticRevision_.load();
  }
  [[nodiscard]] float preferredHeight(const gleditor::ui::UiMetrics &,
                                      const gleditor::ui::Theme &,
                                      bool compact = false) const;
  bool picked(std::uint32_t tag, Session &session,
              std::uint32_t activeDocIndex);

  void dropLeft(PouchItem item);
  void dropRight(PouchItem item);
  void clearLeft() noexcept;
  void clearRight() noexcept;

  void cycleType() noexcept;
  void cycleTier() noexcept;

  void setLinkType(const LinkType type) noexcept {
    if (selectedType_ != type) {
      selectedType_ = type;
      changed();
    }
  }
  [[nodiscard]] LinkType linkType() const noexcept { return selectedType_; }

  void setProminenceTier(const ProminenceTier tier) noexcept {
    if (selectedTier_ != tier) {
      selectedTier_ = tier;
      changed();
    }
  }
  [[nodiscard]] ProminenceTier prominenceTier() const noexcept {
    return selectedTier_;
  }

  [[nodiscard]] bool canForge() const noexcept {
    return !leftSpans_.empty() && !rightSpans_.empty();
  }
  bool forge(Session &session, std::uint32_t activeDocIndex);

  [[nodiscard]] bool containsLeft(float screenX, float screenY) const noexcept;
  [[nodiscard]] bool containsRight(float screenX, float screenY) const noexcept;

  [[nodiscard]] const std::vector<PouchItem> &leftSpans() const noexcept {
    return leftSpans_;
  }
  [[nodiscard]] const std::vector<PouchItem> &rightSpans() const noexcept {
    return rightSpans_;
  }

  [[nodiscard]] float x() const noexcept { return x_; }
  [[nodiscard]] float y() const noexcept { return y_; }
  [[nodiscard]] float width() const noexcept { return width_; }
  [[nodiscard]] float height() const noexcept { return height_; }

  void setDragGuide(float originX, float originY, float targetX, float targetY,
                    bool active) noexcept {
    guideActive_  = active;
    guideOriginX_ = originX;
    guideOriginY_ = originY;
    guideTargetX_ = targetX;
    guideTargetY_ = targetY;
  }
  [[nodiscard]] bool isDragGuideActive() const noexcept { return guideActive_; }

  void triggerBurst() noexcept { burstTimer_ = 0.6F; }
  void update(float deltaTime) noexcept {
    if (burstTimer_ > 0.0F) {
      burstTimer_ = std::max(0.0F, burstTimer_ - deltaTime);
    }
  }
  [[nodiscard]] float burstProgress() const noexcept {
    return burstTimer_ > 0.0F ? (1.0F - burstTimer_ / 0.6F) : 0.0F;
  }

private:
  void changed() noexcept {
    modelDirty_ = true;
    ++semanticRevision_;
  }
  void rebuildModel(const gleditor::ui::UiMetrics &,
                    const gleditor::ui::Theme &, gleditor::ui::Rect);
  void drawAnimation(gleditor::Canvas &);
  gleditor::ui::ScreenOverlay presentation_;
  bool modelDirty_{true};
  std::atomic<std::uint64_t> semanticRevision_{1};
  gleditor::ui::WidgetId nextIdentity_{0x10000000U};
  gleditor::ui::WidgetId leftIdentity_{}, rightIdentity_{};
  gleditor::ui::WidgetId typeIdentity_{}, tierIdentity_{};
  bool freshSelectors_{true};
  bool compactMode_{};
  std::optional<bool> preparedCompact_;
  std::optional<gleditor::ui::UiMetrics> preparedMetrics_;
  std::optional<gleditor::ui::Theme> preparedTheme_;
  gleditor::ui::Theme effectiveTheme_;
  std::optional<gleditor::ui::Rect> preparedBounds_;
  mutable std::optional<gleditor::ui::UiMetrics> heightMetrics_;
  mutable std::optional<gleditor::ui::Theme> heightTheme_;
  mutable float preferredHeight_{};
  mutable bool heightCompact_{};
  std::unique_ptr<gleditor::Canvas> animation_;
  std::function<void(std::uint32_t)> actionHandler_;
  float x_{0.0F};
  float y_{0.0F};
  float width_{0.0F};
  float height_{0.0F};

  float leftX_{0.0F};
  float leftY_{0.0F};
  float leftW_{0.0F};
  float leftH_{0.0F};

  float rightX_{0.0F};
  float rightY_{0.0F};
  float rightW_{0.0F};
  float rightH_{0.0F};

  LinkType selectedType_{LinkType::Comment};
  ProminenceTier selectedTier_{ProminenceTier::Author};

  std::vector<PouchItem> leftSpans_;
  std::vector<PouchItem> rightSpans_;

  bool guideActive_{false};
  float guideOriginX_{0.0F};
  float guideOriginY_{0.0F};
  float guideTargetX_{0.0F};
  float guideTargetY_{0.0F};
  float burstTimer_{0.0F};
};

} // namespace xudu

#endif // XUDU_CLASP_LINK_FORGE_HPP

#pragma once

#include <array>
#include <gleditor/ui/overlay.hpp>
#include <limits>
#include <stdexcept>

namespace xudu {
/// Presentation only: a new target gets a new identity, so delayed picks cannot
/// split the paragraph currently under the pointer instead of the clicked one.
class PageBreakPresentation : public gleditor::ui::ScreenOverlay {
public:
  PageBreakPresentation()
      : ScreenOverlay({.id    = 1,
                       .model = gleditor::ui::Button{
                           "+ Split to New Page (Ctrl+Ret)", "split"}}) {
    setVisible(false);
  }

  void select(std::optional<std::uint64_t> target) {
    if (target == target_) return;
    target_ = target;
    prepared_.reset();
    setVisible(target.has_value());
    if (!target) return;
    if (id_ == std::numeric_limits<gleditor::ui::WidgetId>::max())
      throw std::overflow_error("page-break widget identities exhausted");
    setModel({.id    = ++id_,
              .model = gleditor::ui::Button{"+ Split to New Page (Ctrl+Ret)",
                                            "split"}});
  }

  [[nodiscard]] std::shared_ptr<const gleditor::ui::WidgetScene>
  prepareGap(gleditor::ui::Rect gap, const gleditor::ui::UiMetrics &metrics,
             const gleditor::ui::Theme &theme,
             std::string_view legacyFont = {}) {
    using namespace gleditor::ui;
    const std::array<float, 4> gapStamp{gap.left, gap.bottom, gap.width,
                                        gap.height};
    if (prepared_ && prepared_->metrics == metrics &&
        prepared_->theme == theme && prepared_->legacyFont == legacyFont &&
        prepared_->gap == gapStamp)
      return snapshot();
    const auto effective = withFontOverride(theme, FontRole::Label, legacyFont);
    // Intrinsic measurement is cached; only placement changes as the page
    // moves.
    const auto font = gleditor::text::FontManager::instance().getFont(
        metrics.fontDescription(FontRole::Label, effective));
    const auto &label =
        measurement_.fitted("+ Split to New Page (Ctrl+Ret)", font, {});
    const float pad   = font->metrics().lineHeight * effective.paddingEm;
    const float touch = std::max(effective.type.minTouchPx,
                                 metrics.px(effective.type.minTouchPx));
    const float width = std::ceil(std::max(touch, label.widthPx + 2 * pad)) + 2;
    const float height =
        std::ceil(std::max(touch, font->metrics().lineHeight + 2 * pad)) + 2;
    const auto bounds =
        clampToSafeArea({gap.left + (gap.width - width) * 0.5F,
                         gap.bottom - height * 0.5F, width, height},
                        metrics.pixelSafeArea());
    if (!bounds_ || bounds_->left != bounds.left ||
        bounds_->bottom != bounds.bottom || bounds_->width != bounds.width ||
        bounds_->height != bounds.height) {
      setBounds(bounds);
      bounds_ = bounds;
    }
    effective_       = effective;
    const auto scene = prepare(metrics, effective);
    prepared_        = Stamp{metrics, theme, std::string(legacyFont), gapStamp};
    return scene;
  }

  void drawPrepared(gleditor::FrameContext &ctx) {
    auto metrics         = ctx.metrics;
    metrics.screenWidth  = ctx.screenWidth;
    metrics.screenHeight = ctx.screenHeight;
    metrics.chrome       = ctx.chrome;
    gleditor::FrameContext prepared{.state          = ctx.state,
                                    .viewProjection = ctx.viewProjection,
                                    .screenWidth    = ctx.screenWidth,
                                    .screenHeight   = ctx.screenHeight,
                                    .timeline       = ctx.timeline,
                                    .chrome         = ctx.chrome,
                                    .settledChrome  = ctx.settledChrome,
                                    .metrics        = metrics,
                                    .theme          = effective_};
    drawFrame(prepared);
  }

private:
  struct Stamp {
    gleditor::ui::UiMetrics metrics;
    gleditor::ui::Theme theme;
    std::string legacyFont;
    std::array<float, 4> gap;
  };
  std::optional<Stamp> prepared_;
  std::optional<std::uint64_t> target_;
  gleditor::ui::WidgetId id_{1};
  gleditor::text::ShapingCache measurement_;
  gleditor::ui::Theme effective_;
  std::optional<gleditor::ui::Rect> bounds_;
};
} // namespace xudu

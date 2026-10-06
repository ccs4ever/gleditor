#ifndef GLEDITOR_UI_OVERLAY_HPP
#define GLEDITOR_UI_OVERLAY_HPP

#include <array>
#include <gleditor/canvas.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/ui/widgets.hpp>
#include <mutex>

namespace gleditor::ui {
/// A retained screen-space overlay. Models are values; application callbacks
/// receive action IDs without introducing dependencies on application types.
class ScreenOverlay : public FrameContributor,
                      public PickObserver,
                      public a11y::Source {
public:
  explicit ScreenOverlay(Widget);
  ~ScreenOverlay() override;
  void setModel(Widget);
  void setVisible(bool);
  [[nodiscard]] bool visible() const;
  /// Nullopt uses the metrics' safe area. Bounds are Canvas pixels.
  void setBounds(std::optional<Rect>);
  void setActionHandler(std::function<void(const WidgetAction &)>);
  void invalidate();
  /// Also usable without a render device for geometry/accessibility inspection.
  [[nodiscard]] std::shared_ptr<const WidgetScene> prepare(const UiMetrics &,
                                                           const Theme &);
  [[nodiscard]] std::shared_ptr<const WidgetScene> snapshot() const;
  [[nodiscard]] std::uint64_t layoutRevision() const;
  [[nodiscard]] text::ShapingCache::Stats shapingStats() const;
  bool activate(WidgetId, std::optional<double> fraction = std::nullopt);
  bool typeInto(WidgetId, std::string_view);
  bool keyInto(WidgetId, Key, KeyMods = KeyMods::None);
  bool scrollList(WidgetId, float deltaPx);
  void deviceReady(render::RenderDevice &,
                   const render::PipelineDesc &) override;
  void drawFrame(FrameContext &) override;
  [[nodiscard]] bool picked(const render::PickingResult &,
                            RenderState &) override;
  void describe(a11y::Builder &) override;
  [[nodiscard]] std::uint64_t accessibilityRevision() const override;
  bool performAction(std::uint64_t, a11y::Action, std::string_view) override;

private:
  void changed();
  void rebuildCanvas(RenderState &, const WidgetScene &);
  mutable std::mutex guard_;
  Widget model_;
  bool visible_{true}, dirty_{true};
  std::optional<Rect> bounds_;
  UiMetrics metrics_;
  Theme theme_;
  std::shared_ptr<const WidgetScene> scene_;
  text::ShapingCache shaping_;
  std::uint64_t revision_{1}, layoutRevision_{}, drawnRevision_{};
  std::uint32_t identity_{};
  std::function<void(const WidgetAction &)> actionHandler_;
  render::RenderDevice *device_{};
  std::optional<render::PipelineDesc> pipeline_;
  std::unique_ptr<Canvas> background_;
  std::array<std::unique_ptr<Canvas>, kFontRoleCount> textCanvases_;
};
} // namespace gleditor::ui
#endif

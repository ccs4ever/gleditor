#ifndef GLEDITOR_UI_WORLD_PANEL_HPP
#define GLEDITOR_UI_WORLD_PANEL_HPP

#include <array>
#include <gleditor/canvas.hpp>
#include <gleditor/ui/widgets.hpp>
#include <glm/ext/matrix_float4x4.hpp>
#include <memory>
#include <optional>

namespace gleditor::ui {
/// Canvas-local rectangle projected into bottom-up viewport pixels. The screen
/// box is clipped to the viewport; scales describe the uncut plane footprint.
struct ProjectedPlane {
  Rect bounds;
  float widthPx{}, heightPx{}, minPixelsPerUnit{};
};
/// Rejects nonfinite, degenerate, fully off-screen and near-plane-crossing
/// planes. Such planes must never produce a fictitious screen anchor.
[[nodiscard]] std::optional<ProjectedPlane>
projectPlane(Rect local, const glm::mat4 &canvasToClip, Size viewport);
struct LabelLodPolicy {
  float minWidthPx{};
  float minLineHeightPx{};
  bool operator==(const LabelLodPolicy &) const = default;
};
[[nodiscard]] bool labelLOD(const ProjectedPlane &, float localLineHeight,
                            const LabelLodPolicy &);

/// Retained local widget geometry drawn through the caller's world or screen
/// matrix. One fixed Canvas pool holds the entire panel; labels retain their
/// full source names and local boxes even when projected text is illegible.
/// This painter deliberately does not own application picking semantics.
class WorldPanel {
public:
  explicit WorldPanel(Widget model = {});
  ~WorldPanel();
  WorldPanel(const WorldPanel &)            = delete;
  WorldPanel &operator=(const WorldPanel &) = delete;
  void setModel(Widget);
  void setBounds(Rect);
  void setLabelLod(LabelLodPolicy);
  [[nodiscard]] std::shared_ptr<const WidgetScene> prepare(const UiMetrics &,
                                                           const Theme &);
  [[nodiscard]] std::shared_ptr<const WidgetScene> snapshot() const;
  [[nodiscard]] text::ShapingCache::Stats shapingStats() const;
  void deviceReady(render::RenderDevice &, const render::PipelineDesc &,
                   bool depthTest = true);
  /// Every quad preserves tag exactly, including backgrounds below label LOD.
  /// viewport is in physical pixels. opacity is applied by the draw uniform.
  void draw(RenderState &, const glm::mat4 &canvasToClip, Size viewport,
            render::PickingTag tag = {}, float opacity = 1);
  [[nodiscard]] std::optional<ProjectedPlane>
  projected(WidgetId, const glm::mat4 &canvasToClip, Size viewport) const;
  /// Optional metadata description of the SAME projected visual boxes. Full
  /// names survive label LOD. Application activation stays with its owner.
  void describe(a11y::Builder &, const glm::mat4 &canvasToClip,
                Size viewport) const;

private:
  Widget model_;
  Rect bounds_;
  UiMetrics metrics_;
  Theme theme_;
  LabelLodPolicy lod_{0, TypeScale{}.minFontPx};
  bool dirty_{true}, drawingDirty_{true};
  std::shared_ptr<const WidgetScene> scene_;
  text::ShapingCache shaping_;
  render::RenderDevice *device_{};
  std::optional<render::PipelineDesc> pipeline_;
  std::unique_ptr<Canvas> background_;
  std::array<std::unique_ptr<Canvas>, kFontRoleCount> text_;
  std::vector<bool> drawnLabels_;
  std::optional<render::PickingTag> tag_;
  bool depthTest_{true};
  FontRole primaryRole_{FontRole::Body};
  void rebuild(RenderState &, render::PickingTag);
};
} // namespace gleditor::ui
#endif

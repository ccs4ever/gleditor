#include <gleditor/ui/world_panel.hpp>

#include <algorithm>
#include <cmath>
#include <gleditor/render_state.hpp>
#include <glm/ext/vector_float4.hpp>
#include <limits>
#include <stdexcept>

namespace gleditor::ui {
namespace {
bool finite(Rect rect) {
  return std::isfinite(rect.left) && std::isfinite(rect.bottom) &&
         std::isfinite(rect.width) && std::isfinite(rect.height);
}
float minimumScale(glm::vec2 x, glm::vec2 y) {
  const auto a            = glm::dot(x, x);
  const auto b            = glm::dot(x, y);
  const auto d            = glm::dot(y, y);
  const auto discriminant = std::hypot(a - d, 2 * b);
  return std::sqrt(std::max(0.F, (a + d - discriminant) * .5F));
}
bool sameTag(const render::PickingTag &a, const render::PickingTag &b) {
  return a.kind == b.kind && a.docIndex == b.docIndex &&
         a.pageIndex == b.pageIndex && a.clusterIndex == b.clusterIndex;
}
void tagCanvas(Canvas &canvas, const render::PickingTag &tag) {
  canvas.setIdentity(tag.docIndex, tag.pageIndex);
  canvas.setTag(tag.kind, tag.clusterIndex);
}
void translate(Rect &rect, glm::vec2 offset) {
  rect.left += offset.x;
  rect.bottom += offset.y;
}
} // namespace
std::optional<ProjectedPlane>
projectPlane(Rect local, const glm::mat4 &canvasToClip, Size viewport) {
  if (!finite(local) || local.width <= 0 || local.height <= 0 ||
      !std::isfinite(viewport.width) || !std::isfinite(viewport.height) ||
      viewport.width <= 0 || viewport.height <= 0)
    return std::nullopt;
  for (int column = 0; column < 4; ++column)
    for (int row = 0; row < 4; ++row)
      if (!std::isfinite(canvasToClip[column][row])) return std::nullopt;
  const std::array<glm::vec2, 4> corners{
      {{local.left, local.bottom},
       {local.left + local.width, local.bottom},
       {local.left + local.width, local.bottom + local.height},
       {local.left, local.bottom + local.height}}};
  std::array<glm::vec2, 4> screen{};
  float left = std::numeric_limits<float>::infinity(), bottom = left;
  float right = -left, top = -left;
  for (std::size_t i = 0; i < corners.size(); ++i) {
    const auto clip = canvasToClip * glm::vec4(corners[i], 0.F, 1.F);
    if (!std::isfinite(clip.x) || !std::isfinite(clip.y) ||
        !std::isfinite(clip.z) || !std::isfinite(clip.w) || clip.w <= 0 ||
        clip.z < -clip.w || clip.z > clip.w)
      return std::nullopt;
    screen[i] = {(clip.x / clip.w * .5F + .5F) * viewport.width,
                 (clip.y / clip.w * .5F + .5F) * viewport.height};
    if (!std::isfinite(screen[i].x) || !std::isfinite(screen[i].y))
      return std::nullopt;
    left   = std::min(left, screen[i].x);
    right  = std::max(right, screen[i].x);
    bottom = std::min(bottom, screen[i].y);
    top    = std::max(top, screen[i].y);
  }
  const auto width   = std::min(glm::length(screen[1] - screen[0]),
                                glm::length(screen[2] - screen[3]));
  const auto height  = std::min(glm::length(screen[3] - screen[0]),
                                glm::length(screen[2] - screen[1]));
  const auto bottomX = (screen[1] - screen[0]) / local.width;
  const auto topX    = (screen[2] - screen[3]) / local.width;
  const auto leftY   = (screen[3] - screen[0]) / local.height;
  const auto rightY  = (screen[2] - screen[1]) / local.height;
  const auto scale =
      std::min({minimumScale(bottomX, leftY), minimumScale(bottomX, rightY),
                minimumScale(topX, rightY), minimumScale(topX, leftY)});
  const auto clippedLeft   = std::max(0.F, left),
             clippedBottom = std::max(0.F, bottom);
  const auto clippedRight  = std::min(viewport.width, right),
             clippedTop    = std::min(viewport.height, top);
  if (clippedRight <= clippedLeft || clippedTop <= clippedBottom ||
      !std::isfinite(scale) || scale <= 0 || width <= 0 || height <= 0)
    return std::nullopt;
  return ProjectedPlane{{clippedLeft, clippedBottom, clippedRight - clippedLeft,
                         clippedTop - clippedBottom},
                        width,
                        height,
                        scale};
}
bool labelLOD(const ProjectedPlane &plane, float localLineHeight,
              const LabelLodPolicy &policy) {
  return finite(plane.bounds) && std::isfinite(plane.widthPx) &&
         std::isfinite(plane.minPixelsPerUnit) &&
         std::isfinite(localLineHeight) && std::isfinite(policy.minWidthPx) &&
         std::isfinite(policy.minLineHeightPx) && localLineHeight > 0 &&
         plane.minPixelsPerUnit > 0 && policy.minWidthPx >= 0 &&
         policy.minLineHeightPx >= 0 &&
         std::min(plane.widthPx, plane.bounds.width) >= policy.minWidthPx &&
         localLineHeight * plane.minPixelsPerUnit >= policy.minLineHeightPx;
}
WorldPanel::WorldPanel(Widget model) : model_(std::move(model)) {}
WorldPanel::~WorldPanel() = default;
void WorldPanel::setModel(Widget model) {
  model_ = std::move(model);
  dirty_ = true;
}
void WorldPanel::setBounds(Rect bounds) {
  if (!finite(bounds) || bounds.width < 0 || bounds.height < 0)
    throw std::invalid_argument("Invalid world panel bounds");
  if (bounds_.left != bounds.left || bounds_.bottom != bounds.bottom ||
      bounds_.width != bounds.width || bounds_.height != bounds.height) {
    bounds_ = bounds;
    dirty_  = true;
  }
}
void WorldPanel::setLabelLod(LabelLodPolicy policy) {
  if (!std::isfinite(policy.minWidthPx) ||
      !std::isfinite(policy.minLineHeightPx) || policy.minWidthPx < 0 ||
      policy.minLineHeightPx < 0)
    throw std::invalid_argument("Invalid world label LOD");
  lod_ = policy;
}
std::shared_ptr<const WidgetScene> WorldPanel::prepare(const UiMetrics &metrics,
                                                       const Theme &theme) {
  if (dirty_ || !scene_ || metrics_ != metrics || theme_ != theme) {
    auto localMetrics         = metrics;
    localMetrics.chrome       = {};
    localMetrics.marginShare  = 0;
    localMetrics.screenWidth  = static_cast<int>(std::ceil(bounds_.width));
    localMetrics.screenHeight = static_cast<int>(std::ceil(bounds_.height));
    auto scene = layoutWidgets(model_, {0, 0, bounds_.width, bounds_.height},
                               localMetrics, theme, shaping_);
    const glm::vec2 offset{bounds_.left, bounds_.bottom};
    translate(scene.layout.bounds, offset);
    for (auto &box : scene.layout.boxes) {
      translate(box.rect, offset);
      translate(box.contentRect, offset);
    }
    scene_ = std::make_shared<const WidgetScene>(std::move(scene));
    drawnLabels_.assign(scene_->visuals.size(), false);
    metrics_      = metrics;
    theme_        = theme;
    dirty_        = false;
    drawingDirty_ = true;
  }
  return scene_;
}
std::shared_ptr<const WidgetScene> WorldPanel::snapshot() const {
  return scene_;
}
text::ShapingCache::Stats WorldPanel::shapingStats() const {
  return shaping_.stats();
}
void WorldPanel::deviceReady(render::RenderDevice &device,
                             const render::PipelineDesc &pipeline,
                             bool depthTest) {
  device_    = &device;
  pipeline_  = pipeline;
  depthTest_ = depthTest;
  background_.reset();
  for (auto &canvas : text_) canvas.reset();
  drawingDirty_ = true;
}
void WorldPanel::rebuild(RenderState &state, render::PickingTag tag) {
  primaryRole_ = FontRole::Body;
  for (const auto &visual : scene_->visuals)
    if (!visual.text.empty()) {
      primaryRole_ = visual.fontRole;
      break;
    }
  const auto primaryDescription =
      metrics_.fontDescription(primaryRole_, theme_);
  if (!background_) {
    background_ = std::make_unique<Canvas>(device_, primaryDescription);
    background_->createPipeline(*pipeline_, depthTest_);
  }
  background_->setFontDescription(primaryDescription);
  background_->clear();
  tagCanvas(*background_, tag);
  for (auto &canvas : text_)
    if (canvas) {
      canvas->clear();
      tagCanvas(*canvas, tag);
    }
  const auto surface = rgba(theme_.colours.surface);
  for (std::size_t i = 0; i < scene_->visuals.size(); ++i) {
    const auto &visual = scene_->visuals[i];
    const auto *box    = scene_->layout.find(visual.id);
    if (!box) continue;
    const auto fill =
        rgba(visual.selected ? theme_.colours.accent
             : visual.accessibilityRole == a11y::Role::Button
                 ? theme_.colours.buttonSurface.value_or(theme_.colours.surface)
                 : theme_.colours.surface);
    if (visual.background) {
      const auto &r = box->rect;
      background_->pushClip(r);
      background_->addRect(r.left, r.bottom, r.width, r.height, fill);
      const auto border             = rgba(theme_.colours.border);
      constexpr float borderWidthPx = 1.F;
      background_->addLine(r.left, r.bottom, r.left + r.width, r.bottom,
                           borderWidthPx, border);
      background_->addLine(r.left + r.width, r.bottom, r.left + r.width,
                           r.bottom + r.height, borderWidthPx, border);
      background_->addLine(r.left + r.width, r.bottom + r.height, r.left,
                           r.bottom + r.height, borderWidthPx, border);
      background_->addLine(r.left, r.bottom + r.height, r.left, r.bottom,
                           borderWidthPx, border);
      background_->popClip();
    }
    if (!drawnLabels_[i]) continue;
    auto *canvas = background_.get();
    if (visual.fontRole != primaryRole_) {
      auto &roleCanvas = text_[static_cast<std::size_t>(visual.fontRole)];
      if (!roleCanvas) {
        roleCanvas = std::make_unique<Canvas>(device_, visual.fontDescription);
        roleCanvas->createPipeline(*pipeline_, depthTest_);
        tagCanvas(*roleCanvas, tag);
      }
      roleCanvas->setFontDescription(visual.fontDescription);
      canvas = roleCanvas.get();
    }
    const auto colour =
        rgba(!box->enabled                 ? theme_.colours.disabled
             : visual.tone == Tone::Muted  ? theme_.colours.muted
             : visual.tone == Tone::Accent ? theme_.colours.accent
                                           : theme_.colours.text);
    canvas->pushClip(box->contentRect);
    auto textBox = box->contentRect;
    if (visual.textInput) {
      textBox.left -= visual.textOffsetPx;
      textBox.width =
          std::max(textBox.width + visual.textOffsetPx, visual.fitted.widthPx);
    }
    canvas->addText(state, textBox, visual.fitted, colour,
                    visual.selected ? fill : surface);
    canvas->popClip();
  }
  state.glyphCache.flush();
  background_->commit();
  for (auto &canvas : text_)
    if (canvas) canvas->commit();
  drawingDirty_ = false;
  tag_          = tag;
}
void WorldPanel::draw(RenderState &state, const glm::mat4 &canvasToClip,
                      Size viewport, render::PickingTag tag, float opacity) {
  if (!scene_ || !device_ || !pipeline_) return;
  if (!std::isfinite(opacity))
    throw std::invalid_argument("Invalid world panel opacity");
  for (std::size_t i = 0; i < scene_->visuals.size(); ++i) {
    const auto &visual = scene_->visuals[i];
    const auto *box    = scene_->layout.find(visual.id);
    const auto plane =
        box ? projectPlane(box->contentRect, canvasToClip, viewport)
            : std::nullopt;
    const bool visible =
        plane && visual.font && !visual.text.empty() &&
        !visual.fitted.shaping.glyphs.empty() &&
        labelLOD(*plane, visual.font->metrics().lineHeight, lod_);
    if (visible != drawnLabels_[i]) {
      drawnLabels_[i] = visible;
      drawingDirty_   = true;
    }
  }
  if (drawingDirty_ || !tag_ || !sameTag(*tag_, tag)) rebuild(state, tag);
  background_->draw(state, canvasToClip, opacity);
  for (auto &canvas : text_)
    if (canvas) canvas->draw(state, canvasToClip, opacity);
}
std::optional<ProjectedPlane>
WorldPanel::projected(WidgetId id, const glm::mat4 &canvasToClip,
                      Size viewport) const {
  const auto *box = scene_ ? scene_->layout.find(id) : nullptr;
  return box ? projectPlane(box->rect, canvasToClip, viewport) : std::nullopt;
}
void WorldPanel::describe(a11y::Builder &into, const glm::mat4 &canvasToClip,
                          Size viewport) const {
  if (!scene_) return;
  for (const auto &visual : scene_->visuals) {
    const auto plane = projected(visual.id, canvasToClip, viewport);
    if (!plane) continue;
    auto &node         = into.add(visual.id, visual.accessibilityRole);
    node.label         = visual.accessibleLabel;
    node.value         = visual.value;
    const auto &bounds = plane->bounds;
    node.bounds =
        a11y::Rect{bounds.left, viewport.height - bounds.bottom - bounds.height,
                   bounds.left + bounds.width, viewport.height - bounds.bottom};
    for (const auto &child : scene_->layout.boxes)
      if (child.parentId == visual.id &&
          projected(child.id, canvasToClip, viewport))
        node.children.push_back(into.id(child.id));
    const auto *box = scene_->layout.find(visual.id);
    if (box && box->parentId == 0) into.contribute(into.id(visual.id));
  }
}
} // namespace gleditor::ui

#include <gleditor/ui/overlay.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <gleditor/color.hpp>
#include <gleditor/render_state.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <stdexcept>
#include <utility>

namespace gleditor::ui {
namespace {
std::atomic<std::uint32_t> identities{1};
std::uint32_t rgba(const glm::vec4 &colour) {
  return color::packRgba(colour.r, colour.g, colour.b, colour.a);
}
Widget *findWidget(Widget &widget, WidgetId id) {
  if (widget.id == id) return &widget;
  for (auto &child : widget.children)
    if (auto *found = findWidget(child, id)) return found;
  return nullptr;
}
} // namespace
ScreenOverlay::ScreenOverlay(Widget model)
    : model_(std::move(model)), identity_(identities++) {
  constexpr auto maximumIdentity = (1U << render::tagPageBits) - 1U;
  if (identity_ > maximumIdentity)
    throw std::length_error("Screen overlay picking identities exhausted");
}
ScreenOverlay::~ScreenOverlay() = default;
void ScreenOverlay::changed() {
  dirty_ = true;
  ++revision_;
}
void ScreenOverlay::setModel(Widget model) {
  const std::scoped_lock lock(guard_);
  model_ = std::move(model);
  changed();
}
void ScreenOverlay::setVisible(bool visible) {
  const std::scoped_lock lock(guard_);
  if (visible_ != visible) {
    visible_ = visible;
    changed();
  }
}
bool ScreenOverlay::visible() const {
  const std::scoped_lock lock(guard_);
  return visible_;
}
void ScreenOverlay::setBounds(std::optional<Rect> bounds) {
  const std::scoped_lock lock(guard_);
  bounds_ = bounds;
  changed();
}
void ScreenOverlay::setActionHandler(
    std::function<void(const WidgetAction &)> handler) {
  const std::scoped_lock lock(guard_);
  actionHandler_ = std::move(handler);
}
void ScreenOverlay::invalidate() {
  const std::scoped_lock lock(guard_);
  changed();
}
std::shared_ptr<const WidgetScene>
ScreenOverlay::prepare(const UiMetrics &metrics, const Theme &theme) {
  const std::scoped_lock lock(guard_);
  if (!scene_ || dirty_ || metrics_ != metrics || theme_ != theme) {
    auto next = std::make_shared<WidgetScene>(
        layoutWidgets(model_, bounds_.value_or(metrics.pixelSafeArea()),
                      metrics, theme, shaping_));
    scene_   = std::move(next);
    metrics_ = metrics;
    theme_   = theme;
    dirty_   = false;
    ++layoutRevision_;
    ++revision_;
  }
  return scene_;
}
std::shared_ptr<const WidgetScene> ScreenOverlay::snapshot() const {
  const std::scoped_lock lock(guard_);
  return scene_;
}
std::uint64_t ScreenOverlay::layoutRevision() const {
  const std::scoped_lock lock(guard_);
  return layoutRevision_;
}
text::ShapingCache::Stats ScreenOverlay::shapingStats() const {
  const std::scoped_lock lock(guard_);
  return shaping_.stats();
}
bool ScreenOverlay::activate(WidgetId id, std::optional<double> fraction) {
  std::function<void(const WidgetAction &)> callback;
  WidgetAction action;
  {
    const std::scoped_lock lock(guard_);
    if (!visible_ || !scene_) return false;
    const auto *visual = scene_->find(id);
    const auto *box    = scene_->layout.find(id);
    if (!visual || !box || !box->enabled || !visual->interactive) return false;
    auto *owner = findWidget(model_, visual->ownerId);
    if (!owner) return false;
    action = {owner->id, visual->action, visual->value, visual->itemIndex};
    if (auto *step = std::get_if<Stepper>(&owner->model)) {
      if (step->change(visual->stepDirection)) changed();
      action.value = std::to_string(step->value);
    } else if (auto *tabs = std::get_if<Tabs>(&owner->model)) {
      if (tabs->selected != visual->itemIndex) {
        tabs->selected = visual->itemIndex;
        changed();
      }
    } else if (auto *scrubber = std::get_if<Scrubber>(&owner->model)) {
      if (!fraction) return false;
      if (scrubber->setFraction(*fraction)) changed();
      action.value = std::to_string(scrubber->value);
    }
    callback = actionHandler_;
  }
  if (callback) callback(action);
  return true;
}
bool ScreenOverlay::typeInto(WidgetId id, std::string_view text) {
  std::function<void(const WidgetAction &)> callback;
  WidgetAction action;
  {
    const std::scoped_lock lock(guard_);
    auto *widget = findWidget(model_, id);
    auto *input  = widget ? std::get_if<TextField>(&widget->model) : nullptr;
    if (!visible_ || !input) return false;
    input->insert(text);
    changed();
    action   = {id, input->action, input->value, 0};
    callback = actionHandler_;
  }
  if (callback) callback(action);
  return true;
}
bool ScreenOverlay::keyInto(WidgetId id, Key key, KeyMods mods) {
  std::function<void(const WidgetAction &)> callback;
  WidgetAction action;
  {
    const std::scoped_lock lock(guard_);
    auto *widget = findWidget(model_, id);
    auto *input  = widget ? std::get_if<TextField>(&widget->model) : nullptr;
    if (!visible_ || !input || !input->key(key, mods)) return false;
    changed();
    action   = {id, input->action, input->value, 0};
    callback = actionHandler_;
  }
  if (callback) callback(action);
  return true;
}
bool ScreenOverlay::scrollList(WidgetId id, float delta) {
  if (!std::isfinite(delta))
    throw std::invalid_argument("List scroll must be finite");
  const std::scoped_lock lock(guard_);
  auto *widget = findWidget(model_, id);
  auto *list   = widget ? std::get_if<List>(&widget->model) : nullptr;
  if (!visible_ || !list) return false;
  list->scrollPx = std::max(0.0F, list->scrollPx + delta);
  changed();
  return true;
}
void ScreenOverlay::deviceReady(render::RenderDevice &device,
                                const render::PipelineDesc &pipeline) {
  const std::scoped_lock lock(guard_);
  device_        = &device;
  pipeline_      = pipeline;
  drawnRevision_ = 0;
  background_.reset();
  for (auto &canvas : textCanvases_) canvas.reset();
}
void ScreenOverlay::rebuildCanvas(RenderState &state,
                                  const WidgetScene &scene) {
  if (!device_ || !pipeline_) return;
  if (!background_) {
    background_ = std::make_unique<Canvas>(
        device_, metrics_.fontDescription(FontRole::Body, theme_));
    background_->createPipeline(*pipeline_);
  }
  background_->clear();
  background_->setIdentity(0, identity_);
  // Canvases retain geometry, but a changed font description needs a new font
  // owner.
  for (auto &canvas : textCanvases_) canvas.reset();
  const auto surface = rgba(theme_.colours.surface);
  for (const auto &visual : scene.visuals) {
    const auto *box = scene.layout.find(visual.id);
    if (!box) continue;
    if (visual.background) {
      background_->setTag(render::tagKindOverlay, visual.id);
      const auto fill = rgba(visual.selected ? theme_.colours.accent
                                             : theme_.colours.surface);
      background_->addRect(box->rect.left, box->rect.bottom, box->rect.width,
                           box->rect.height, fill);
    }
    if (visual.scrubber) {
      background_->setTag(render::tagKindOverlay, visual.id);
      const auto thickness = metrics_.px(2);
      background_->addRect(box->contentRect.left, box->contentRect.bottom,
                           box->contentRect.width *
                               static_cast<float>(visual.fraction),
                           thickness, rgba(theme_.colours.accent));
    }
    if (visual.text.empty() || box->contentRect.width <= 0 ||
        box->contentRect.height <= 0)
      continue;
    auto &canvas = textCanvases_[static_cast<std::size_t>(visual.fontRole)];
    if (!canvas) {
      canvas = std::make_unique<Canvas>(device_, visual.fontDescription);
      canvas->createPipeline(*pipeline_);
      canvas->setIdentity(0, identity_);
    }
    canvas->setTag(render::tagKindOverlay, visual.id);
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
                    visual.selected ? rgba(theme_.colours.accent) : surface);
    canvas->popClip();
  }
  state.glyphCache.flush();
  background_->commit();
  for (auto &canvas : textCanvases_)
    if (canvas) canvas->commit();
}
void ScreenOverlay::drawFrame(FrameContext &context) {
  if (!visible()) return;
  auto metrics         = context.metrics;
  metrics.screenWidth  = context.screenWidth;
  metrics.screenHeight = context.screenHeight;
  static_cast<void>(prepare(metrics, context.theme));
  const std::scoped_lock lock(guard_);
  if (!device_ || !visible_) return;
  if (drawnRevision_ != layoutRevision_) {
    rebuildCanvas(context.state, *scene_);
    drawnRevision_ = layoutRevision_;
  }
  const auto projection =
      glm::ortho(0.0F, static_cast<float>(context.screenWidth), 0.0F,
                 static_cast<float>(context.screenHeight));
  if (background_) background_->draw(context.state, projection);
  for (const auto &canvas : textCanvases_)
    if (canvas) canvas->draw(context.state, projection);
}
bool ScreenOverlay::picked(const render::PickingResult &pick, RenderState &) {
  if (pick.tag.kind != render::tagKindOverlay ||
      pick.tag.pageIndex != identity_ || pick.tag.docIndex != 0)
    return false;
  const auto scene = snapshot();
  if (!scene || !visible()) return false;
  const auto *box = scene->layout.find(pick.tag.clusterIndex);
  if (!box) return false;
  const auto fraction =
      box->contentRect.width > 0
          ? (static_cast<double>(pick.x) - box->contentRect.left) /
                box->contentRect.width
          : 0;
  static_cast<void>(activate(pick.tag.clusterIndex, fraction));
  return true;
}
void ScreenOverlay::describe(a11y::Builder &builder) {
  const std::scoped_lock lock(guard_);
  if (!visible_ || !scene_) return;
  for (const auto &visual : scene_->visuals) {
    const auto *box = scene_->layout.find(visual.id);
    if (!box) continue;
    auto &node      = builder.add(visual.id, visual.accessibilityRole);
    node.label      = visual.accessibleLabel;
    node.value      = visual.value;
    const auto rect = box->rect;
    node.bounds     = a11y::Rect{
        rect.left, metrics_.screenHeight - (rect.bottom + rect.height),
        rect.left + rect.width, metrics_.screenHeight - rect.bottom};
    node.focusable = visual.interactive && box->enabled;
    if (node.focusable) node.actions = a11y::bit(a11y::Action::Click);
    if (visual.textInput || visual.scrubber)
      node.actions |= a11y::bit(a11y::Action::SetValue);
    if (visual.fitted.truncated) node.description = visual.text;
    for (const auto &child : scene_->layout.boxes)
      if (child.parentId == visual.id)
        node.children.push_back(builder.id(child.id));
    if (box->parentId == 0) builder.contribute(builder.id(visual.id));
  }
}
std::uint64_t ScreenOverlay::accessibilityRevision() const {
  const std::scoped_lock lock(guard_);
  return revision_;
}
bool ScreenOverlay::performAction(std::uint64_t id, a11y::Action action,
                                  std::string_view value) {
  const auto local = a11y::Ids::localOf(id);
  if (local > std::numeric_limits<WidgetId>::max()) return false;
  const auto widgetId = static_cast<WidgetId>(local);
  if (action == a11y::Action::Click) return activate(widgetId);
  if (action != a11y::Action::SetValue) return false;
  std::function<void(const WidgetAction &)> callback;
  WidgetAction notification;
  {
    const std::scoped_lock lock(guard_);
    auto *widget = findWidget(model_, widgetId);
    if (!visible_ || !widget) return false;
    if (auto *input = std::get_if<TextField>(&widget->model)) {
      TextField next = *input;
      next.value.clear();
      next.caret = 0;
      next.insert(value);
      *input = std::move(next);
      changed();
      notification = {widgetId, input->action, input->value, 0};
    } else if (auto *scrubber = std::get_if<Scrubber>(&widget->model)) {
      double number{};
      const auto parsed =
          std::from_chars(value.data(), value.data() + value.size(), number);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != value.data() + value.size() || !std::isfinite(number))
        return false;
      auto candidate  = *scrubber;
      candidate.value = number;
      if (scrubber->setFraction(candidate.fraction())) changed();
      notification = {widgetId, scrubber->action,
                      std::to_string(scrubber->value), 0};
    } else
      return false;
    callback = actionHandler_;
  }
  if (callback) callback(notification);
  return true;
}
} // namespace gleditor::ui

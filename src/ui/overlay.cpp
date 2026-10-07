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
  FocusScope::activate();
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
    if (visible)
      FocusScope::activate();
    else {
      deactivate();
      pressedNode_ = 0;
    }
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
    if (!visual || !box || !box->enabled || !visual->interactive ||
        box->rect.width <= 0 || box->rect.height <= 0)
      return false;
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
    action   = {id, input->action, input->value, 0, input->caret};
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
    action   = {id, input->action, input->value, 0, input->caret};
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
std::shared_ptr<const LayoutResult> ScreenOverlay::focusLayout() const {
  const std::scoped_lock lock(guard_);
  if (!visible_ || !scene_) return {};
  return {scene_, &scene_->layout};
}
void ScreenOverlay::focusedNodeChanged(std::uint32_t node) {
  const std::scoped_lock lock(guard_);
  if (focusedNode_ != node) {
    focusedNode_ = node;
    ++focusGeometryRevision_;
    ++revision_;
  }
}
void ScreenOverlay::focusChanged(bool focused) {
  const std::scoped_lock lock(guard_);
  if (scopeFocused_ != focused) {
    scopeFocused_ = focused;
    if (!focused) pressedNode_ = 0;
    ++focusGeometryRevision_;
    ++revision_;
  }
}
bool ScreenOverlay::activateNode(std::uint32_t node) {
  std::optional<double> fraction;
  {
    const std::scoped_lock lock(guard_);
    if (!visible_ || !scene_) return false;
    const auto *visual = scene_->find(node);
    if (visual && visual->scrubber) fraction = visual->fraction;
  }
  return activate(node, fraction);
}
bool ScreenOverlay::keyPressed(const KeyEvent &event) {
  WidgetId node, owner;
  bool input = false, numeric = false;
  double fraction = 0, step = 0;
  {
    const std::scoped_lock lock(guard_);
    if (!visible_ || !scene_ || !scopeFocused_) return false;
    const auto *visual = scene_->find(focusedNode_);
    if (!visual) return false;
    node    = visual->id;
    owner   = visual->ownerId;
    input   = visual->textInput;
    numeric = visual->scrubber;
    if (numeric) {
      const auto *widget = findWidget(model_, owner);
      const auto *scrubber =
          widget ? std::get_if<Scrubber>(&widget->model) : nullptr;
      if (!scrubber) return false;
      fraction = scrubber->fraction();
      step     = scrubber->keyboardStep;
      if (!std::isfinite(step) || step <= 0)
        throw std::invalid_argument(
            "Scrubber keyboard step must be positive and finite");
    }
  }
  if (input) return keyInto(owner, event.key, event.mods);
  if (numeric) {
    switch (event.key) {
    case Key::Left:
    case Key::Down:
      return activate(node, fraction - step);
    case Key::Right:
    case Key::Up:
      return activate(node, fraction + step);
    case Key::Home:
      return activate(node, 0);
    case Key::End:
      return activate(node, 1);
    default:
      break;
    }
  }
  return event.key == Key::Space && activateNode(node);
}
void ScreenOverlay::textTyped(std::string_view text) {
  WidgetId target = 0;
  {
    const std::scoped_lock lock(guard_);
    if (!visible_ || !scene_ || !scopeFocused_) return;
    const auto *visual = scene_->find(focusedNode_);
    if (visual && visual->textInput) target = visual->ownerId;
  }
  if (target) static_cast<void>(typeInto(target, text));
}
std::optional<InputArea> ScreenOverlay::textArea() const {
  const std::scoped_lock lock(guard_);
  if (!visible_ || !scene_ || !scopeFocused_) return std::nullopt;
  const auto *visual = scene_->find(focusedNode_);
  const auto *box    = scene_->layout.find(focusedNode_);
  return visual && box && visual->textInput && box->enabled
             ? std::optional<InputArea>{toInputArea(box->contentRect,
                                                    metrics_.screenHeight)}
             : std::nullopt;
}
std::optional<InputArea> ScreenOverlay::pointerArea() const {
  const std::scoped_lock lock(guard_);
  if (!visible_ || !scene_ || scene_->layout.boxes.empty()) return std::nullopt;
  return toInputArea(scene_->layout.boxes.front().rect, metrics_.screenHeight);
}
void ScreenOverlay::cancel() { setVisible(false); }
bool ScreenOverlay::acceptsCommand(std::string_view command) const {
  return command == "quit" || command == "std:xudu/quit";
}
bool ScreenOverlay::pointerEvent(const PointerEvent &event) {
  WidgetId activateId = 0, scrollId = 0;
  double fraction = 0;
  float scroll    = 0;
  bool handled    = false;
  {
    const std::scoped_lock lock(guard_);
    if (!visible_ || !scene_) return false;
    if (event.phase == PointerPhase::Cancel) {
      pressedNode_ = 0;
      return true;
    }
    const auto *hit = scene_->layout.hitTest(
        event.x, static_cast<float>(metrics_.screenHeight) - event.y);
    const auto *visual = hit ? scene_->find(hit->id) : nullptr;
    if (event.phase == PointerPhase::Wheel) {
      if (visual) {
        const auto *owner = findWidget(model_, visual->ownerId);
        const auto *list  = owner ? std::get_if<List>(&owner->model) : nullptr;
        if (list) {
          scrollId = owner->id;
          const auto pitch =
              list->rowHeightPx > 0
                  ? list->rowHeightPx
                  : std::max(theme_.type.minTouchPx,
                             metrics_.px(theme_.type.minTouchPx));
          scroll = -event.deltaY * pitch;
        }
      }
    } else if (event.phase == PointerPhase::Press && event.button == 1) {
      handled = hit != nullptr;
      if (hit && visual && hit->focusable && hit->enabled) {
        requestFocus(hit->id);
        pressedNode_    = hit->id;
        pressedPointer_ = event.pointerId;
        if (visual->scrubber) activateId = hit->id;
      }
    } else if (pressedNode_ && pressedPointer_ == event.pointerId) {
      const auto *pressed = scene_->find(pressedNode_);
      if (pressed && pressed->scrubber &&
          (event.phase == PointerPhase::Move ||
           event.phase == PointerPhase::Release)) {
        activateId = pressedNode_;
      } else if (event.phase == PointerPhase::Release && hit &&
                 hit->id == pressedNode_) {
        activateId = pressedNode_;
      }
      handled = true;
      if (event.phase == PointerPhase::Release) pressedNode_ = 0;
    }
    if (activateId) {
      const auto *box = scene_->layout.find(activateId);
      fraction        = box && box->contentRect.width > 0
                            ? (static_cast<double>(event.x) - box->contentRect.left) /
                           box->contentRect.width
                            : 0;
    }
  }
  if (scrollId) return scrollList(scrollId, scroll);
  if (activateId) return activate(activateId, fraction);
  return handled;
}
void ScreenOverlay::deviceReady(render::RenderDevice &device,
                                const render::PipelineDesc &pipeline) {
  const std::scoped_lock lock(guard_);
  device_        = &device;
  pipeline_      = pipeline;
  drawnRevision_ = 0;
  background_.reset();
  focusCanvas_.reset();
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
    const auto itemSurface = visual.accessibilityRole == a11y::Role::Button
                                 ? rgba(theme_.colours.buttonSurface.value_or(
                                       theme_.colours.surface))
                                 : surface;
    if (visual.background) {
      background_->setTag(render::tagKindOverlay, visual.pickingId);
      const auto fill = rgba(
          visual.selected ? theme_.colours.accent
          : visual.accessibilityRole == a11y::Role::Button
              ? theme_.colours.buttonSurface.value_or(theme_.colours.surface)
              : theme_.colours.surface);
      background_->addRect(box->rect.left, box->rect.bottom, box->rect.width,
                           box->rect.height, fill);
    }
    if (visual.scrubber) {
      background_->setTag(render::tagKindOverlay, visual.pickingId);
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
    canvas->setTag(render::tagKindOverlay, visual.pickingId);
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
                    visual.selected ? rgba(theme_.colours.accent)
                                    : itemSurface);
    canvas->popClip();
  }
  rebuildFocusCanvas(scene);
  state.glyphCache.flush();
  background_->commit();
  for (auto &canvas : textCanvases_)
    if (canvas) canvas->commit();
}
void ScreenOverlay::rebuildFocusCanvas(const WidgetScene &scene) {
  if (!device_ || !pipeline_) return;
  if (!focusCanvas_) {
    focusCanvas_ = std::make_unique<Canvas>(
        device_, metrics_.fontDescription(FontRole::Body, theme_));
    focusCanvas_->createPipeline(*pipeline_);
  }
  focusCanvas_->clear();
  focusCanvas_->setIdentity(0, identity_);
  if (scopeFocused_) {
    const auto *visual = scene.find(focusedNode_);
    if (const auto *box = scene.layout.find(focusedNode_);
        box && visual && box->focusable && box->enabled &&
        std::ranges::find(scene.layout.focusOrder, focusedNode_) !=
            scene.layout.focusOrder.end()) {
      const auto stroke = std::min(
          {std::max(1.0F, metrics_.px(1)), box->rect.width, box->rect.height});
      focusCanvas_->setTag(render::tagKindOverlay, visual->pickingId);
      focusCanvas_->pushClip(box->rect);
      const auto colour = rgba(theme_.colours.accent);
      const auto &rect  = box->rect;
      focusCanvas_->addRect(rect.left, rect.bottom, rect.width, stroke, colour);
      focusCanvas_->addRect(rect.left, rect.bottom + rect.height - stroke,
                            rect.width, stroke, colour);
      focusCanvas_->addRect(rect.left, rect.bottom, stroke, rect.height,
                            colour);
      focusCanvas_->addRect(rect.left + rect.width - stroke, rect.bottom,
                            stroke, rect.height, colour);
      if (visual && visual->textInput && visual->caretOffsetPx) {
        const auto &content    = box->contentRect;
        const auto caretStroke = std::min(stroke, content.width);
        const auto x =
            std::clamp(content.left + *visual->caretOffsetPx, content.left,
                       content.left + content.width - caretStroke);
        focusCanvas_->addRect(x, content.bottom, caretStroke, content.height,
                              colour);
      }
      focusCanvas_->popClip();
    }
  }
  focusCanvas_->commit();
}
void ScreenOverlay::drawFrame(FrameContext &context) {
  if (!visible()) return;
  auto metrics         = context.metrics;
  metrics.screenWidth  = context.screenWidth;
  metrics.screenHeight = context.screenHeight;
  static_cast<void>(prepare(metrics, context.theme));
  const std::scoped_lock lock(guard_);
  if (!device_ || !visible_) return;
  context.state.bindOverlayWidgets(
      render::packTagIdentity(render::tagKindOverlay, 0, identity_),
      scene_->pickingTargets);
  if (drawnRevision_ != layoutRevision_) {
    rebuildCanvas(context.state, *scene_);
    drawnRevision_      = layoutRevision_;
    drawnFocusRevision_ = focusGeometryRevision_;
  } else if (drawnFocusRevision_ != focusGeometryRevision_) {
    rebuildFocusCanvas(*scene_);
    drawnFocusRevision_ = focusGeometryRevision_;
  }
  const auto projection =
      glm::ortho(0.0F, static_cast<float>(context.screenWidth), 0.0F,
                 static_cast<float>(context.screenHeight));
  if (background_) background_->draw(context.state, projection);
  for (const auto &canvas : textCanvases_)
    if (canvas) canvas->draw(context.state, projection);
  if (focusCanvas_) focusCanvas_->draw(context.state, projection);
}
bool ScreenOverlay::picked(const render::PickingResult &pick, RenderState &) {
  if (pick.tag.kind != render::tagKindOverlay ||
      pick.tag.pageIndex != identity_ || pick.tag.docIndex != 0)
    return false;
  if (pick.requestId != 0 && !pick.overlayWidgetId) return true;
  const auto scene = snapshot();
  if (!scene || !visible()) return false;
  const auto id = pick.overlayWidgetId
                      ? pick.overlayWidgetId
                      : scene->resolvePickingId(pick.tag.clusterIndex);
  if (!id) return false;
  const auto *box = scene->layout.find(*id);
  if (!box) return pick.requestId != 0;
  const auto fraction =
      box->contentRect.width > 0
          ? (static_cast<double>(pick.x) - box->contentRect.left) /
                box->contentRect.width
          : 0;
  if (box->focusable && box->enabled) requestFocus(box->id);
  static_cast<void>(activate(*id, fraction));
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
    node.focusable = visual.interactive && box->enabled &&
                     box->rect.width > 0 && box->rect.height > 0;
    if (node.focusable)
      node.actions =
          a11y::bit(a11y::Action::Click) | a11y::bit(a11y::Action::Focus);
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
  if (action == a11y::Action::Focus) {
    const std::scoped_lock lock(guard_);
    const auto *box =
        visible_ && scene_ ? scene_->layout.find(widgetId) : nullptr;
    if (!box || !box->enabled || !box->focusable ||
        std::ranges::find(scene_->layout.focusOrder, widgetId) ==
            scene_->layout.focusOrder.end())
      return false;
    requestFocus(widgetId);
    return true;
  }
  if (action == a11y::Action::Click) {
    requestFocus(widgetId);
    return activateNode(widgetId);
  }
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
      notification = {widgetId, input->action, input->value, 0, input->caret};
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

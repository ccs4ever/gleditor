/**
 * @file store_object_manager.cpp
 * @brief Implementation of StoreObjectManager drawer.
 */
#include "common/ui/store_object_manager.hpp"

#include <algorithm>
#include <cmath>
#include <gleditor/text/font.hpp>
#include <limits>
#include <stdexcept>
#include <utility>

#include <gleditor/render/device.hpp>
#include <gleditor/render_state.hpp>

namespace xanadu {
namespace ui = gleditor::ui;

StoreObjectManager::StoreObjectManager(Store &store, std::string fontName,
                                       ToggleCallback onToggle,
                                       CreateCallback onCreate,
                                       CloseCallback onClose,
                                       IsOpenPredicate isOpen)
    : store_(store), fontName_(std::move(fontName)),
      overlay_({.id = 1, .model = gleditor::ui::Dock{}}),
      onToggle_(std::move(onToggle)), onCreate_(std::move(onCreate)),
      onClose_(std::move(onClose)), isOpen_(std::move(isOpen)) {
  overlay_.setVisible(false);
  overlay_.setActionHandler([this](const auto &action) { queue(action); });
  refresh();
}

StoreObjectManager::~StoreObjectManager() { releaseFocus(); }

void StoreObjectManager::deviceReady(
    render::RenderDevice &device,
    const render::PipelineDesc &documentPipeline) {
  overlay_.deviceReady(device, documentPipeline);
}

StoreObjectManager *StoreObjectManager::setVisible(const bool visible) {
  const std::scoped_lock lock(guard_);
  if (visible_.load() != visible) {
    visible_ = visible;
    ++generation_;
    if (visible) {
      if (nextId_ > std::numeric_limits<ui::WidgetId>::max() - 3)
        throw std::length_error("Store drawer action identities exhausted");
      closeId_ = nextId_++;
      sliceId_ = nextId_++;
      docId_   = nextId_++;
      identities_.clear();
    }
    pending_.clear();
    overlay_.setVisible(visible);
    dirty_ = true;
    if (visible_)
      activate();
    else
      deactivate();
    if (visible_) {
      refresh();
    }
    changed();
  }
  return this;
}

StoreObjectManager *StoreObjectManager::toggle() {
  setVisible(!visible_);
  return this;
}

StoreObjectManager *StoreObjectManager::refresh() {
  const std::scoped_lock lock(guard_);
  observedOps_ = store_.opCount();
  items_.clear();
  const auto births = store_.discoverStructureBirths(store_.latest());
  for (const auto &b : births) {
    bool open = false;
    if (isOpen_) {
      open = isOpen_(b.opIndex);
    }
    items_.push_back(ObjectItem{
        .birthOp = b.opIndex,
        .kind    = b.kind,
        .name    = b.name,
        .isOpen  = open,
        .yTop    = 0.0F,
        .yBottom = 0.0F,
    });
  }
  changed();
  return this;
}

StoreObjectManager *StoreObjectManager::createSlice() {
  const std::scoped_lock lock(guard_);
  if (onCreate_) {
    onCreate_(StructureKind::Slice);
  } else {
    const auto parent = store_.latest();
    const auto name   = "Slice " + std::to_string(store_.opCount() + 1);
    if (store_.homeCell() == zigzag::noCell) {
      std::ignore = store_.sliceGenesis(parent, name);
    } else {
      std::ignore = store_.makeSlice(parent, name);
    }
  }
  refresh();
  if (!items_.empty() && onToggle_) {
    const auto &last = items_.back();
    if (last.kind == StructureKind::Slice && !last.isOpen) {
      onToggle_(last.birthOp, last.kind, true);
    }
  }
  return this;
}

StoreObjectManager *StoreObjectManager::createXanadoc() {
  const std::scoped_lock lock(guard_);
  if (onCreate_) {
    onCreate_(StructureKind::Xanadoc);
  } else {
    const auto parent = store_.latest();
    const auto name   = "Document " + std::to_string(store_.opCount() + 1);
    std::ignore       = store_.makeXanadoc(parent, name);
  }
  refresh();
  if (!items_.empty() && onToggle_) {
    const auto &last = items_.back();
    if (last.kind == StructureKind::Xanadoc && !last.isOpen) {
      onToggle_(last.birthOp, last.kind, true);
    }
  }
  return this;
}

StoreObjectManager *StoreObjectManager::toggleItem(const std::size_t index) {
  const std::scoped_lock lock(guard_);
  if (index >= items_.size()) {
    return this;
  }
  auto &item          = items_[index];
  const bool newState = !item.isOpen;
  item.isOpen         = newState;
  if (onToggle_) {
    onToggle_(item.birthOp, item.kind, newState);
  }
  changed();
  return this;
}

StoreObjectManager *StoreObjectManager::closeItem(const std::size_t index) {
  const std::scoped_lock lock(guard_);
  if (index >= items_.size()) {
    return this;
  }
  auto &item = items_[index];
  if (item.isOpen) {
    item.isOpen = false;
    if (onClose_) {
      onClose_(item.birthOp);
    } else if (onToggle_) {
      onToggle_(item.birthOp, item.kind, false);
    }
    changed();
  }
  return this;
}

void StoreObjectManager::changed() {
  dirty_ = true;
  ++a11yRevision_;
}
StoreObjectManager *
StoreObjectManager::setConfig(const StorePanelConfig &config) {
  const std::scoped_lock lock(guard_);
  if (config_ != config) {
    config_ = config;
    changed();
  }
  return this;
}

std::shared_ptr<const ui::WidgetScene>
StoreObjectManager::prepare(const ui::UiMetrics &metrics,
                            const ui::Theme &sourceTheme) {
  const std::scoped_lock lock(guard_);
  if (!visible_) return {};
  if (!dirty_ && metrics_ == metrics && sourceTheme_ == sourceTheme)
    return overlay_.snapshot();
  sourceTheme_ = sourceTheme;
  auto theme =
      ui::withFontOverride(sourceTheme, ui::FontRole::Label, fontName_);
  const auto font = gleditor::text::FontManager::instance().getFont(
      metrics.fontDescription(ui::FontRole::Label, theme));
  const auto line = std::ceil(font->metrics().lineHeight) + 2;
  // Compact nested containers preserve readable rows at the largest font scale.
  theme.paddingEm  = std::min(theme.paddingEm, .25F);
  theme.gapEm      = std::min(theme.gapEm, .25F);
  metrics_         = metrics;
  theme_           = theme;
  const auto safe  = metrics.pixelSafeArea();
  const auto share = [](float value, float fallback) {
    return std::isfinite(value) ? std::clamp(value, .1F, 1.0F) : fallback;
  };
  const auto requested = std::isfinite(config_.widthPx)
                             ? std::max(0.0F, metrics.px(config_.widthPx))
                             : metrics.px(StorePanelConfig{}.widthPx);
  const auto padding   = font->metrics().lineHeight * theme.paddingEm;
  const auto actionWidth =
      std::max(measurements_.fitted("New Slice", font, {}).widthPx,
               measurements_.fitted("New Xanadoc", font, {}).widthPx) +
      padding * 2 + 2;
  const auto closeWidth =
      measurements_.fitted("Close Drawer", font, {}).widthPx + padding * 2 + 2;
  const auto headerWidth =
      measurements_.fitted("Store Object Manager", font, {}).widthPx +
      closeWidth + padding * 4;
  const auto width = std::floor(std::min(
      std::max({requested, actionWidth * 2 + padding * 4, headerWidth}),
      safe.width * share(config_.maxWidthShare, .9F)));
  const auto height =
      std::floor(safe.height * share(config_.maxHeightShare, 1));
  const auto pad      = font->metrics().lineHeight * theme.paddingEm;
  const auto gap      = font->metrics().lineHeight * theme.gapEm;
  const auto interior = std::max(0.0F, width - pad * 2);
  const auto buttonHeight =
      std::ceil(std::max(metrics.px(theme.type.minTouchPx), line + pad * 2));
  const auto flowHeight = buttonHeight + pad * 2;
  const auto columnWidth =
      std::floor(std::max(0.0F, (interior - pad * 2 - gap) * .5F));
  ui::Widget model{
      .id        = 1,
      .model     = ui::Dock{"", ui::DockSide::Right},
      .preferred = {metrics.logical(width), metrics.logical(height)}};
  ui::Widget header{.id        = 2,
                    .model     = ui::ButtonFlow{},
                    .preferred = {0, metrics.logical(flowHeight)}};
  header.children = {
      {.id        = 3,
       .model     = ui::Label{"Store Object Manager"},
       .preferred = {metrics.logical(
                         std::max(0.0F, interior - pad * 2 - gap - closeWidth)),
                     metrics.logical(buttonHeight)}},
      {.id        = closeId_,
       .model     = ui::Button{"Close Drawer", "close-drawer"},
       .preferred = {metrics.logical(closeWidth),
                     metrics.logical(buttonHeight)}}};
  model.children.push_back(std::move(header));
  ui::Widget creates{.id        = 5,
                     .model     = ui::ButtonFlow{},
                     .preferred = {0, metrics.logical(flowHeight)}};
  creates.children = {{.id        = sliceId_,
                       .model     = ui::Button{"New Slice", "create-slice"},
                       .preferred = {metrics.logical(columnWidth),
                                     metrics.logical(buttonHeight)}},
                      {.id        = docId_,
                       .model     = ui::Button{"New Xanadoc", "create-xanadoc"},
                       .preferred = {metrics.logical(columnWidth),
                                     metrics.logical(buttonHeight)}}};
  model.children.push_back(std::move(creates));
  actions_.clear();
  toggleActions_.clear();
  closeActions_.clear();
  actions_.emplace(closeId_,
                   Action{ActionKind::CloseDrawer, 0, false, generation_});
  actions_.emplace(sliceId_,
                   Action{ActionKind::CreateSlice, 0, false, generation_});
  actions_.emplace(docId_,
                   Action{ActionKind::CreateXanadoc, 0, false, generation_});
  const auto remaining =
      std::max(0.0F, height - pad * 2 - flowHeight * 2 - gap * 2);
  rowHeight_ =
      std::ceil(std::max(metrics.px(theme.type.minTouchPx), line + pad));
  viewportHeight_ = std::max(0.0F, remaining - pad * 4);
  scrollPx_ =
      std::clamp(scrollPx_, 0.0F,
                 std::max(0.0F, static_cast<float>(items_.size()) * rowHeight_ -
                                    viewportHeight_));
  if (items_.empty()) {
    model.children.push_back(
        {.id    = 8,
         .model = ui::Label{"No documents or slices yet. Create a Xanadoc or "
                            "Slice to initialize this store.",
                            ui::TextPurpose::Description},
         .preferred = {0, metrics.logical(remaining)},
         .maxLines  = 5});
  } else {
    ui::List toggles{
        .scrollPx = scrollPx_, .rowHeightPx = rowHeight_, .overscan = 0};
    ui::List closes{
        .scrollPx = scrollPx_, .rowHeightPx = rowHeight_, .overscan = 0};
    for (const auto &item : items_) {
      auto &identity = identities_[item.birthOp];
      if (!identity.toggle || identity.open != item.isOpen) {
        if (nextId_ > std::numeric_limits<ui::WidgetId>::max() - 2)
          throw std::length_error("Store drawer action identities exhausted");
        identity = {nextId_++, nextId_++, item.isOpen};
      }
      const auto type =
          item.kind == StructureKind::Slice ? "[SLICE] " : "[DOC] ";
      toggles.rows.push_back(
          {identity.toggle,
           std::string(item.isOpen ? "Open " : "Closed ") + type + item.name,
           "toggle"});
      closes.rows.push_back(
          {identity.close, "Close " + item.name, "close", item.isOpen});
      actions_.emplace(identity.toggle, Action{ActionKind::Toggle, item.birthOp,
                                               item.isOpen, generation_});
      actions_.emplace(identity.close, Action{ActionKind::Close, item.birthOp,
                                              item.isOpen, generation_});
      toggleActions_.push_back(actions_.at(identity.toggle));
      closeActions_.push_back(actions_.at(identity.close));
    }
    const auto columns = std::max(0.0F, interior - pad * 2 - gap);
    ui::Widget lists{.id        = 8,
                     .model     = ui::ButtonFlow{},
                     .preferred = {0, metrics.logical(remaining)}};
    lists.children = {
        {.id        = 9,
         .model     = std::move(toggles),
         .preferred = {metrics.logical(std::floor(columns * .7F)),
                       metrics.logical(std::max(0.0F, remaining - pad * 2))}},
        {.id        = 10,
         .model     = std::move(closes),
         .preferred = {metrics.logical(std::floor(columns * .3F)),
                       metrics.logical(std::max(0.0F, remaining - pad * 2))}}};
    model.children.push_back(std::move(lists));
  }
  overlay_.setModel(std::move(model));
  overlay_.setBounds(ui::clampToSafeArea(
      metrics.rounded({safe.left + safe.width - width,
                       safe.bottom + safe.height - height, width, height}),
      safe));
  auto result = overlay_.prepare(metrics, theme);
  for (auto &item : items_) {
    const auto identity = identities_.find(item.birthOp);
    const auto *box     = identity == identities_.end()
                              ? nullptr
                              : result->layout.find(identity->second.toggle);
    item.yBottom        = box ? box->rect.bottom : 0;
    item.yTop           = box ? box->rect.bottom + box->rect.height : 0;
  }
  dirty_ = false;
  return result;
}

void StoreObjectManager::queue(const ui::WidgetAction &action) {
  const std::scoped_lock lock(guard_);
  if (!visible_) return;
  // Activation entry points retain the host lock until this callback resolves
  // the list index, so a new scene cannot replace its birth snapshot midway.
  if (action.id == 9 || action.id == 10) {
    const auto &rows = action.id == 9 ? toggleActions_ : closeActions_;
    if (action.itemIndex < rows.size())
      pending_.push_back(rows[action.itemIndex]);
    return;
  }
  const auto found = actions_.find(action.id);
  if (found != actions_.end()) pending_.push_back(found->second);
}
void StoreObjectManager::drain() {
  const std::scoped_lock lock(guard_);
  auto pending = std::move(pending_);
  pending_.clear();
  for (const auto &action : pending) {
    if (!visible_ || action.generation != generation_) continue;
    switch (action.kind) {
    case ActionKind::CloseDrawer:
      setVisible(false);
      break;
    case ActionKind::CreateSlice:
      createSlice();
      break;
    case ActionKind::CreateXanadoc:
      createXanadoc();
      break;
    case ActionKind::Toggle:
    case ActionKind::Close: {
      const auto found =
          std::ranges::find(items_, action.birth, &ObjectItem::birthOp);
      if (found == items_.end() || found->isOpen != action.open) break;
      const auto index = static_cast<std::size_t>(found - items_.begin());
      if (action.kind == ActionKind::Toggle)
        toggleItem(index);
      else
        closeItem(index);
      break;
    }
    }
  }
}
bool StoreObjectManager::busy() const {
  const std::scoped_lock lock(guard_);
  return !pending_.empty();
}
void StoreObjectManager::drawFrame(gleditor::FrameContext &ctx) {
  const std::scoped_lock lock(guard_);
  if (visible_ && observedOps_ != store_.opCount()) refresh();
  drain();
  if (!visible_) return;
  auto metrics         = ctx.metrics;
  metrics.screenWidth  = ctx.screenWidth;
  metrics.screenHeight = ctx.screenHeight;
  metrics.chrome       = ctx.chrome;
  std::ignore          = prepare(metrics, ctx.theme);
  // The retained model was prepared with the legacy override, if supplied.
  gleditor::FrameContext draw{
      ctx.state,    ctx.viewProjection, ctx.screenWidth,   ctx.screenHeight,
      ctx.timeline, ctx.chrome,         ctx.settledChrome, metrics,
      theme_};
  overlay_.drawFrame(draw);
}
bool StoreObjectManager::picked(const render::PickingResult &pick,
                                RenderState &state) {
  const std::scoped_lock lock(guard_);
  if (!visible_) return false;
  if (pick.requestId || pick.tag.docIndex || pick.tag.pageIndex)
    return overlay_.picked(pick, state);
  // Compatibility for synthetic domain tests; rendered picks use the retained
  // scope.
  if (pick.tag.kind != render::tagKindOverlay) return false;
  const auto tag = pick.tag.clusterIndex;
  if (tag == kTagCloseDrawer) {
    setVisible(false);
    return true;
  }
  if (tag == kTagNewSlice) {
    createSlice();
    return true;
  }
  if (tag == kTagNewXanadoc) {
    createXanadoc();
    return true;
  }
  if (tag >= kTagItemCheckboxBase && tag < kTagItemCloseBase) {
    toggleItem(tag - kTagItemCheckboxBase);
    return true;
  }
  if (tag >= kTagItemCloseBase && tag < kTagItemCloseBase + 1000) {
    closeItem(tag - kTagItemCloseBase);
    return true;
  }
  return false;
}
void StoreObjectManager::describe(gleditor::a11y::Builder &builder) {
  overlay_.describe(builder);
}
bool StoreObjectManager::performAction(std::uint64_t id,
                                       gleditor::a11y::Action action,
                                       std::string_view value) {
  const std::scoped_lock lock(guard_);
  return visible_ && overlay_.performAction(id, action, value);
}
std::shared_ptr<const ui::LayoutResult>
StoreObjectManager::focusLayout() const {
  return overlay_.focusLayout();
}
void StoreObjectManager::focusedNodeChanged(std::uint32_t id) {
  overlay_.focusedNodeChanged(id);
}
bool StoreObjectManager::activateNode(std::uint32_t id) {
  const std::scoped_lock lock(guard_);
  return visible_ && overlay_.activateNode(id);
}
void StoreObjectManager::focusChanged(bool focused) {
  overlay_.focusChanged(focused);
}
std::optional<gleditor::InputArea> StoreObjectManager::pointerArea() const {
  return overlay_.pointerArea();
}
void StoreObjectManager::scroll(float delta) {
  const std::scoped_lock lock(guard_);
  scrollPx_ =
      std::clamp(scrollPx_ + delta, 0.0F,
                 std::max(0.0F, static_cast<float>(items_.size()) * rowHeight_ -
                                    viewportHeight_));
  changed();
}
bool StoreObjectManager::pointerEvent(const ui::PointerEvent &event) {
  const std::scoped_lock lock(guard_);
  if (!visible_) return false;
  if (event.phase == ui::PointerPhase::Wheel) {
    const auto area = pointerArea();
    if (!area || event.x < area->x || event.x >= area->x + area->width ||
        event.y < area->y || event.y >= area->y + area->height)
      return false;
    scroll(-event.deltaY * rowHeight_);
    return true;
  }
  return overlay_.pointerEvent(event);
}
bool StoreObjectManager::keyPressed(gleditor::Key key, gleditor::KeyMods mods) {
  const std::scoped_lock lock(guard_);
  if (!visible_) return false;
  if (key == gleditor::Key::Escape) {
    setVisible(false);
    return true;
  }
  if (key == gleditor::Key::PageDown || key == gleditor::Key::PageUp) {
    scroll((key == gleditor::Key::PageDown ? 1 : -1) * viewportHeight_);
    return true;
  }
  return overlay_.keyPressed({key, mods});
}
} // namespace xanadu

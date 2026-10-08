/**
 * @file pouch_drawer.cpp
 * @brief Screen-edge Pouch Drawer overlay with partitioned drop zones and clasp
 * bench.
 */
#include "pouch_drawer.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <string>

#include <glm/ext/matrix_clip_space.hpp>

#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>

#include "common/xanadu/system_docs.hpp"

namespace xanadu {

PouchDrawer::PouchDrawer(Session &session, RendererRef renderer,
                         std::string fontName, const DockSide side)
    : session_(session), renderer_(std::move(renderer)),
      fontName_(std::move(fontName)), side_(side),
      pouchManager_(session.systemStore(SystemDocKind::Pouches)) {
  pouchManager_.loadManifest();
  forgeWidget_.setActionHandler(
      [this](std::uint32_t tag) { enqueue({.tag = tag}); });
}

PouchDrawer::~PouchDrawer() = default;

void PouchDrawer::deviceReady(render::RenderDevice &device,
                              const render::PipelineDesc &pipeline) {
  const std::scoped_lock lock(guard_);
  device_   = &device;
  pipeline_ = pipeline;
  forgeWidget_.deviceReady(device, pipeline);
  if (header_) header_->deviceReady(device, pipeline);
  if (resizeHandle_) resizeHandle_->deviceReady(device, pipeline);
  for (auto &entry : presentations_)
    entry.overlay->deviceReady(device, pipeline);
}

void PouchDrawer::setConfig(PouchPanelConfig config) {
  const std::scoped_lock lock(guard_);
  const PouchPanelConfig defaults;
  if (!std::isfinite(config.widthPx) || config.widthPx <= 0)
    config.widthPx = defaults.widthPx;
  if (!std::isfinite(config.maxWidthShare))
    config.maxWidthShare = defaults.maxWidthShare;
  if (!std::isfinite(config.maxHeightShare))
    config.maxHeightShare = defaults.maxHeightShare;
  config.maxWidthShare  = std::clamp(config.maxWidthShare, .1F, 1.0F);
  config.maxHeightShare = std::clamp(config.maxHeightShare, .1F, 1.0F);
  config_               = config;
  compactMetrics_.reset();
  preparedMetrics_.reset();
}

void PouchDrawer::setDockSide(DockSide side) noexcept {
  const std::scoped_lock lock(guard_);
  if (side_ != side) {
    side_ = side;
    preparedMetrics_.reset();
    ++a11yRevision_;
  }
}

void PouchDrawer::setOpen(const bool open, const bool animated) noexcept {
  const std::scoped_lock lock(guard_);
  if (open != isOpen_) {
    cancelResize();
    ++resizeId_;
    resizeHandle_.reset();
    resizeBounds_.reset();
    forgeWidget_.setVisible(false);
    ++openEpoch_;
    ++a11yRevision_;
    preparedMetrics_.reset();
    pending_.clear();
    ids_.clear();
  }
  if (open && !isOpen_) activate();
  if (!open) deactivate();
  isOpen_           = open;
  targetSlideWidth_ = open ? config_.widthPx : 0.0F;
  if (!animated) {
    currentSlideWidth_ = targetSlideWidth_;
    snapNextLayout_    = true;
  }
  if (!open) focus_.reset();
  forgeWidget_.setVisible(open);
}

bool PouchDrawer::busy() const {
  const std::scoped_lock lock(guard_);
  return !pending_.empty() || widthCommit_.has_value() ||
         std::abs(currentSlideWidth_ - targetSlideWidth_) > 0.5F ||
         (currentSlideWidth_ >= 1.0F && forgeWidget_.burstProgress() > 0.0F &&
          forgeWidget_.burstProgress() < 1.0F);
}

void PouchDrawer::layout(const gleditor::ui::UiMetrics &metrics,
                         const gleditor::ui::Theme &theme) {
  using namespace gleditor::ui;
  const auto safe = metrics.pixelSafeArea();
  targetSlideWidth_ =
      isOpen_ ? std::min(std::max({metrics.px(config_.widthPx), minimumWidthPx_,
                                   adaptiveWidthPx_}),
                         safe.width * config_.maxWidthShare)
              : 0;
  if (snapNextLayout_) {
    currentSlideWidth_ = targetSlideWidth_;
    snapNextLayout_    = false;
  }
  if (std::abs(currentSlideWidth_ - targetSlideWidth_) > .5F)
    currentSlideWidth_ += (targetSlideWidth_ - currentSlideWidth_) * .28F;
  else
    currentSlideWidth_ = targetSlideWidth_;
  drawerW_ = std::min(currentSlideWidth_, safe.width);
  drawerH_ = safe.height * config_.maxHeightShare;
  drawerX_ =
      side_ == DockSide::Left ? safe.left : safe.left + safe.width - drawerW_;
  drawerY_ = safe.bottom;
  if (!preparedMetrics_ || *preparedMetrics_ != metrics ||
      preparedTheme_ != theme) {
    const auto font = gleditor::text::FontManager::instance().getFont(
        metrics.fontDescription(FontRole::Label, theme));
    const auto caption = gleditor::text::FontManager::instance().getFont(
        metrics.fontDescription(FontRole::Caption, theme));
    const float line = font->metrics().lineHeight;
    const float cap  = caption->metrics().lineHeight;
    touchPx_         = std::ceil(std::max(metrics.px(theme.type.minTouchPx),
                                          line * (1 + 2 * theme.paddingEm))) +
               2;
    zoneTouchPx_ =
        std::ceil(std::max(metrics.px(theme.type.minTouchPx), line)) + 2;
    captionLinePx_ = std::ceil(cap) + 2;
    capTouchPx_    = std::ceil(std::max(metrics.px(theme.type.minTouchPx),
                                        cap * (1 + 2 * theme.paddingEm))) +
                  2;
    gapPx_    = line * theme.gapEm;
    headerPx_ = captionLinePx_ + static_cast<float>(headerRows_) * capTouchPx_ +
                4 * line * theme.paddingEm + 3 * gapPx_;
  }
  handleWidthPx_ = std::min(drawerW_, metrics.px(theme.type.minTouchPx));
  const float contentX =
      drawerX_ + (side_ == DockSide::Right ? handleWidthPx_ : 0);
  const float contentW     = std::max(0.0F, drawerW_ - handleWidthPx_);
  const float gap          = gapPx_;
  const float headerHeight = std::min(drawerH_, headerPx_);
  const float benchHeight =
      std::min(std::max(0.0F, drawerH_ - headerHeight),
               forgeWidget_.preferredHeight(metrics, theme, forgeCompact_));
  forgeWidget_.setGeometry(contentX, drawerY_, contentW, benchHeight);
  const float available = std::max(0.0F, drawerH_ - headerHeight - benchHeight);
  const auto &zones     = pouchManager_.zones();
  if (zones.empty()) return;
  zoneStart_    = std::min(zoneStart_, zones.size() - 1);
  visibleZones_ = std::max<std::size_t>(
      1, static_cast<std::size_t>(available /
                                  std::max(1.0F, zoneTouchPx_ * 2 + gap + 4)));
  visibleZones_ = std::min(visibleZones_, zones.size() - zoneStart_);
  float weight  = 0;
  while (true) {
    weight = 0;
    for (std::size_t i = zoneStart_; i < zoneStart_ + visibleZones_; ++i)
      weight += std::max(0.0F, zones[i]->heightWeight());
    bool fits = true;
    for (std::size_t i = zoneStart_; i < zoneStart_ + visibleZones_; ++i) {
      const float share =
          weight > 0 ? std::max(0.0F, zones[i]->heightWeight()) / weight
                     : 1.0F / static_cast<float>(visibleZones_);
      if (available * share < 2 * zoneTouchPx_ + gapPx_ + 4) fits = false;
    }
    if (fits || visibleZones_ == 1) break;
    --visibleZones_;
  }
  float top = drawerY_ + drawerH_ - headerHeight;
  for (std::size_t i = 0; i < zones.size(); ++i) {
    if (i < zoneStart_ || i >= zoneStart_ + visibleZones_) {
      zones[i]->setRect(0, 0, 0, 0);
      continue;
    }
    const float share  = weight > 0
                             ? std::max(0.0F, zones[i]->heightWeight()) / weight
                             : 1.0F / static_cast<float>(visibleZones_);
    const float height = available * share;
    top -= height;
    const auto rect = metrics.rounded({contentX, top, contentW, height});
    zones[i]->setRect(rect.left, rect.bottom, rect.width, rect.height);
  }
}

std::uint64_t PouchDrawer::contentStamp() const {
  std::uint64_t stamp = pouchManager_.store().opCount();
  const auto mix      = [&](std::uint64_t value) {
    stamp ^= value + 0x9e3779b97f4a7c15ULL + (stamp << 6U) + (stamp >> 2U);
  };
  for (const auto &zone : pouchManager_.zones()) {
    mix(std::hash<std::string>{}(zone->id()));
    mix(std::hash<std::string>{}(zone->label()));
    mix(std::hash<float>{}(zone->heightWeight()));
    mix(zone->auraColor());
    mix(zone->isHovered());
    for (int i = 0; i < 4; ++i)
      mix(std::hash<float>{}(zone->backgroundColor()[i]));
    for (const auto &item : zone->items()) {
      mix(item.itemId);
      mix(std::hash<std::string>{}(item.previewText));
    }
  }
  for (const auto &[id, page] : zonePage_) {
    mix(std::hash<std::string>{}(id));
    mix(page);
  }
  mix(zoneStart_);
  mix(visibleZones_);
  return stamp;
}

void PouchDrawer::enqueue(Action action) {
  const std::scoped_lock lock(guard_);
  action.epoch             = openEpoch_;
  action.forgeRevision     = forgeWidget_.semanticRevision();
  action.sessionGeneration = session_.generation();
  pending_.push_back(std::move(action));
}

gleditor::ui::WidgetId PouchDrawer::actionId(std::string key, Action action) {
  auto [it, fresh] = ids_.try_emplace(std::move(key), nextId_);
  if (fresh) ++nextId_;
  action.epoch         = openEpoch_;
  actions_[it->second] = std::move(action);
  return it->second;
}

void PouchDrawer::rebuildModels(const gleditor::ui::UiMetrics &metrics,
                                const gleditor::ui::Theme &theme) {
  using namespace gleditor::ui;
  const auto stamp = contentStamp();
  if (header_ && stamp == modelStamp_ && preparedMetrics_ &&
      *preparedMetrics_ == metrics && preparedTheme_ == theme)
    return;
  const auto callback = [this](const WidgetAction &action) {
    const std::scoped_lock lock(guard_);
    if (const auto list = rowActions_.find(action.id);
        list != rowActions_.end()) {
      if (action.itemIndex < list->second.size())
        enqueue(list->second[action.itemIndex]);
      return;
    }
    if (const auto it = actions_.find(action.id); it != actions_.end())
      enqueue(it->second);
  };
  rowActions_.clear();
  Widget header{.id = 1, .model = Panel{}};
  header.children.push_back({.id        = 2,
                             .model     = Label{"Pouch drawer"},
                             .preferred = {0, metrics.logical(captionLinePx_)},
                             .fontRole  = FontRole::Caption});
  Widget controls{.id = 3, .model = ButtonFlow{}};
  for (const auto &[tag, label] :
       std::array<std::pair<std::uint32_t, const char *>, 4>{
           {{kTagDrawerClose, "Close"},
            {kTagDrawerAddZone, "+ Zone"},
            {kTagDrawerFlipDock, "Dock"},
            {kTagDrawerNextZones, "Zones"}}})
    controls.children.push_back({.id        = actionId(label, {.tag = tag}),
                                 .model     = Button{label, label},
                                 .preferred = {0, metrics.logical(capTouchPx_)},
                                 .fontRole  = FontRole::Caption});
  header.children.push_back(std::move(controls));
  if (!header_) {
    header_ = std::make_unique<ScreenOverlay>(std::move(header));
    if (device_) header_->deviceReady(*device_, *pipeline_);
  } else
    header_->setModel(std::move(header));
  header_->setActionHandler(callback);
  std::vector<ZonePresentation> next;
  const auto font = gleditor::text::FontManager::instance().getFont(
      metrics.fontDescription(FontRole::Label, theme));
  const float line = font->metrics().lineHeight;
  for (const auto &zone : pouchManager_.zones()) {
    if (zone->height() <= 0) continue;
    Widget model{.id = actionId("zone:" + zone->id(), {}), .model = Panel{}};
    model.children.push_back(
        {.id        = actionId("label:" + zone->id(),
                               {.tag = 7005U, .zone = zone->id()}),
         .model     = Button{zone->label() + " ›", "more"},
         .preferred = {0, metrics.logical(zoneTouchPx_)}});
    List list;
    list.rowHeightPx = zoneTouchPx_;
    if (const auto it = zonePage_.find(zone->id()); it != zonePage_.end())
      list.scrollPx = static_cast<float>(it->second) * zoneTouchPx_;
    for (const auto &item : zone->items()) {
      const auto key = std::to_string(item.itemId);
      list.rows.push_back({.id   = actionId("item:" + key, {.tag  = kTagItemBase,
                                                            .item = item.itemId,
                                                            .zone = zone->id()}),
                           .text = item.previewText,
                           .action = "origin"});
      list.rows.push_back({.id = actionId("use:" + key, {.tag = kTagItemUseBase,
                                                         .item = item.itemId,
                                                         .zone = zone->id()}),
                           .text   = "Insert: " + item.previewText,
                           .action = "use"});
      list.rows.push_back(
          {.id     = actionId("remove:" + key, {.tag  = kTagItemDismissBase,
                                                .item = item.itemId,
                                                .zone = zone->id()}),
           .text   = "Remove: " + item.previewText,
           .action = "remove"});
    }
    list.rows.push_back(
        {.id     = actionId("clear:" + zone->id(),
                            {.tag = kTagZoneClearBase, .zone = zone->id()}),
         .text   = "Clear zone",
         .action = "clear"});
    const auto listId = actionId("list:" + zone->id(), {});
    auto &rows        = rowActions_[listId];
    rows.reserve(list.rows.size());
    for (const auto &row : list.rows) rows.push_back(actions_.at(row.id));
    const float viewport =
        std::max(0.0F, zone->height() - zoneTouchPx_ - line * theme.gapEm - 2);
    const float fullRows = std::floor((viewport + .001F) / zoneTouchPx_);
    model.children.push_back(
        {.id        = listId,
         .model     = std::move(list),
         .preferred = {
             0, metrics.logical(std::max(1.0F, fullRows * zoneTouchPx_))}});
    auto old =
        std::ranges::find(presentations_, zone->id(), &ZonePresentation::zone);
    std::unique_ptr<ScreenOverlay> overlay;
    if (old != presentations_.end()) {
      overlay = std::move(old->overlay);
      overlay->setModel(std::move(model));
    } else {
      overlay = std::make_unique<ScreenOverlay>(std::move(model));
      if (device_) overlay->deviceReady(*device_, *pipeline_);
    }
    overlay->setActionHandler(callback);
    auto zoneTheme            = theme;
    zoneTheme.paddingEm       = 0;
    zoneTheme.colours.surface = zone->backgroundColor();
    const auto aura           = gleditor::color::unpackRgba(zone->auraColor());
    zoneTheme.colours.accent  = {aura.r, aura.g, aura.b, aura.a};
    next.push_back(
        {zone->id(), std::move(overlay), std::nullopt, std::move(zoneTheme)});
  }
  presentations_   = std::move(next);
  modelStamp_      = stamp;
  preparedMetrics_ = metrics;
  preparedTheme_   = theme;
  ++a11yRevision_;
}

bool PouchDrawer::handleGhostDrop(const PrimediaSpan &span,
                                  const std::string &preview,
                                  const MicroversionId &sourceVer,
                                  const float screenX, const float screenY,
                                  const std::uint32_t docIndex,
                                  const std::uint32_t charStart,
                                  const std::uint32_t charEnd) {
  const std::scoped_lock lock(guard_);
  if (!isOpen_ || docIndex >= session_.views().size()) return false;
  const auto carried =
      carrySpan(session_.store(session_.storeIndexOf(docIndex)),
                pouchManager_.store(), span);
  if (!carried) return false;
  PouchItem item{
      .itemId          = 0,
      .span            = *carried,
      .previewText     = preview,
      .originVersion   = sourceVer,
      .originDocIndex  = docIndex,
      .originCharStart = charStart,
      .originCharEnd   = charEnd,
      .timestampUtc    = 0,
  };

  // 1. Check Clasp Forge Homestead (Left) Slot
  if (forgeWidget_.containsLeft(screenX, screenY)) {
    forgeWidget_.dropLeft(std::move(item));
    return true;
  }

  // 2. Check Clasp Forge Toward (Right) Slot
  if (forgeWidget_.containsRight(screenX, screenY)) {
    forgeWidget_.dropRight(std::move(item));
    return true;
  }

  // 3. Check Partitioned Drop Zones
  if (const auto zone = zoneAt(screenX, screenY);
      zone && zone->width() > 0 && zone->height() > 0) {
    pouchManager_.dropSpan(zone->id(), *carried, preview, sourceVer, docIndex,
                           charStart, charEnd);
    return true;
  }

  return false;
}

bool PouchDrawer::handleCellDrop(
    const PrimediaSpan &span, const std::string &preview,
    const std::uint32_t cellRef, const std::string_view rankCoord,
    const float screenX, const float screenY, const std::uint32_t sliceIndex,
    const std::optional<GlobalOpRef> &originOpRef) {
  const std::scoped_lock lock(guard_);
  if (!isOpen_ || sliceIndex >= session_.storeCount()) return false;
  const auto carried =
      carrySpan(session_.store(sliceIndex), pouchManager_.store(), span);
  if (!carried) return false;
  PouchItem item{
      .itemId           = 0,
      .span             = *carried,
      .previewText      = preview,
      .originVersion    = MicroversionId{},
      .originDocIndex   = 0,
      .originCharStart  = 0,
      .originCharEnd    = static_cast<std::uint32_t>(span.length),
      .timestampUtc     = 0,
      .originKind       = PouchOriginKind::ZigzagCell,
      .originCell       = cellRef,
      .originSliceIndex = sliceIndex,
      .originRankCoord  = std::string(rankCoord),
      .originOpRef      = originOpRef,
  };

  // 1. Check Clasp Forge Homestead (Left) Slot
  if (forgeWidget_.containsLeft(screenX, screenY)) {
    forgeWidget_.dropLeft(std::move(item));
    return true;
  }

  // 2. Check Clasp Forge Toward (Right) Slot
  if (forgeWidget_.containsRight(screenX, screenY)) {
    forgeWidget_.dropRight(std::move(item));
    return true;
  }

  // 3. Check Partitioned Drop Zones
  if (const auto zone = zoneAt(screenX, screenY);
      zone && zone->width() > 0 && zone->height() > 0) {
    pouchManager_.dropCell(zone->id(), *carried, preview, cellRef, rankCoord,
                           sliceIndex, originOpRef);
    return true;
  }

  return false;
}

void PouchDrawer::drainActions(RenderState &state) {
  if (widthCommit_) {
    const auto commit = *widthCommit_;
    widthCommit_.reset();
    auto &store = session_.systemStore(SystemDocKind::UI);
    auto parent = store.primaryCurrentVersion();
    for (const auto &spec : defaultSettingSpecs(SystemDocKind::UI)) {
      if (spec.name == settings::kPouchPanelWidthPx) {
        parent = ensureSetting(store, parent, spec);
        break;
      }
    }
    const auto head = setSetting(store, parent, settings::kPouchPanelWidthPx,
                                 double{commit.logical});
    session_.repointSystemDoc(SystemDocKind::UI, head);
  }
  auto actions = std::move(pending_);
  pending_.clear();
  for (const auto &action : actions) {
    if (!isOpen_ || action.epoch != openEpoch_) continue;
    if (action.tag == kTagDrawerClose) {
      setOpen(false);
      continue;
    }
    if (action.tag == kTagDrawerFlipDock) {
      setDockSide(side_ == DockSide::Left ? DockSide::Right : DockSide::Left);
      continue;
    }
    if (action.tag == kTagDrawerNextZones) {
      zoneStart_ = (zoneStart_ + visibleZones_) %
                   std::max<std::size_t>(1, pouchManager_.zones().size());
      preparedMetrics_.reset();
      continue;
    }
    if (action.tag == kTagDrawerAddZone) {
      const auto count = pouchManager_.zones().size() + 1;
      pouchManager_.addZone({.id    = "custom_" + std::to_string(count),
                             .label = "Zone " + std::to_string(count)});
      continue;
    }
    if (action.tag >= LinkForgeWidget::kTagClaspLeftDrop &&
        action.tag <= LinkForgeWidget::kTagClaspClearRight) {
      if (action.forgeRevision != forgeWidget_.semanticRevision() ||
          action.sessionGeneration != session_.generation())
        continue;
      const auto destination = state.caret && state.caret->active()
                                   ? std::optional{state.caret->documentIndex()}
                                   : std::nullopt;
      forgeWidget_.picked(action.tag, session_, destination,
                          pouchManager_.store());
      continue;
    }
    const auto zone = pouchManager_.zoneById(action.zone);
    if (!zone) continue;
    if (action.tag == 7005U) {
      zonePage_[action.zone] =
          (zonePage_[action.zone] + 1) % (zone->items().size() * 3 + 1);
      preparedMetrics_.reset();
      continue;
    }
    if (action.tag == kTagZoneClearBase) {
      std::vector<std::uint64_t> ids;
      for (const auto &item : zone->items()) ids.push_back(item.itemId);
      for (auto id : ids) pouchManager_.dismissItem(id);
      continue;
    }
    const auto item =
        std::ranges::find(zone->items(), action.item, &PouchItem::itemId);
    if (item == zone->items().end()) continue;
    if (action.tag == kTagItemDismissBase) {
      pouchManager_.dismissItem(action.item);
      continue;
    }
    const auto &handler =
        action.tag == kTagItemUseBase ? useHandler_ : swingBackHandler_;
    if (handler) handler(*item);
  }
}

void PouchDrawer::drawFrame(gleditor::FrameContext &ctx) {
  const std::scoped_lock lock(guard_);
  drainActions(ctx.state);
  auto metrics         = ctx.metrics;
  metrics.screenWidth  = ctx.screenWidth;
  metrics.screenHeight = ctx.screenHeight;
  metrics.chrome       = ctx.chrome;
  if (!fontName_.empty() && (!legacyBase_ || *legacyBase_ != ctx.theme)) {
    legacyBase_  = ctx.theme;
    legacyTheme_ = gleditor::ui::withFontOverride(
        ctx.theme, gleditor::ui::FontRole::Label, fontName_);
  }
  const auto &sourceTheme = fontName_.empty() ? ctx.theme : legacyTheme_;
  if (!compactMetrics_ || *compactMetrics_ != metrics || !compactSource_ ||
      *compactSource_ != sourceTheme) {
    const auto font = gleditor::text::FontManager::instance().getFont(
        metrics.fontDescription(gleditor::ui::FontRole::Label, sourceTheme));
    const float line   = font->metrics().lineHeight;
    const auto caption = gleditor::text::FontManager::instance().getFont(
        metrics.fontDescription(gleditor::ui::FontRole::Caption, sourceTheme));
    const float cap = caption->metrics().lineHeight;
    const float zoneHeight =
        2 * (std::ceil(
                 std::max(metrics.px(sourceTheme.type.minTouchPx), line)) +
             2) +
        4;
    const float budget =
        metrics.pixelSafeArea().height * config_.maxHeightShare;
    std::size_t controlIndex = 0;
    for (const auto *label : {"Close", "+ Zone", "Dock", "Zones"})
      controlWidths_[controlIndex++] =
          std::ceil(measurement_.fitted(label, caption, {}).widthPx) + 2;
    forgeCompact_       = false;
    headerRows_         = 2;
    adaptiveWidthPx_    = 0;
    const auto required = [&](const gleditor::ui::Theme &candidate) {
      const float button =
          std::ceil(std::max(metrics.px(candidate.type.minTouchPx),
                             cap * (1 + 2 * candidate.paddingEm))) +
          2;
      return std::ceil(cap) + 2 + static_cast<float>(headerRows_) * button +
             4 * line * candidate.paddingEm + 3 * line * candidate.gapEm +
             forgeWidget_.preferredHeight(metrics, candidate, forgeCompact_) +
             zoneHeight + line * candidate.gapEm;
    };
    auto minimum      = sourceTheme;
    minimum.paddingEm = 0;
    minimum.gapEm     = 0;
    if (required(minimum) > budget) {
      headerRows_ = 1;
      if (required(minimum) > budget) forgeCompact_ = true;
      float width = metrics.px(sourceTheme.type.minTouchPx);
      for (const auto *label : {"Close", "+ Zone", "Dock", "Zones"}) {
        const auto &text = measurement_.fitted(label, caption, {});
        width += std::ceil(std::max(metrics.px(sourceTheme.type.minTouchPx),
                                    text.widthPx)) +
                 2;
      }
      adaptiveWidthPx_ = std::min(width, metrics.pixelSafeArea().width *
                                             config_.maxWidthShare);
    }
    const float maximumWidth =
        metrics.pixelSafeArea().width * config_.maxWidthShare;
    const auto requiredWidth = [&](const gleditor::ui::Theme &candidate) {
      const float padding = 2 * cap * candidate.paddingEm;
      const float pairs   = std::max(controlWidths_[0] + controlWidths_[1],
                                     controlWidths_[2] + controlWidths_[3]);
      return pairs + 2 * padding + line * candidate.gapEm +
             4 * line * candidate.paddingEm +
             metrics.px(candidate.type.minTouchPx) + 2;
    };
    auto compact = sourceTheme;
    float low = 0, high = 1;
    for (int step = 0; headerRows_ == 2 && step < 16; ++step) {
      const float share = (low + high) * .5F;
      compact.paddingEm = sourceTheme.paddingEm * share;
      compact.gapEm     = sourceTheme.gapEm * share;
      if (required(compact) <= budget && requiredWidth(compact) <= maximumWidth)
        low = share;
      else
        high = share;
    }
    compactTheme_ = sourceTheme;
    compactTheme_.paddingEm *= low;
    compactTheme_.gapEm *= low;
    handleTheme_           = compactTheme_;
    handleTheme_.paddingEm = 0;
    handleTheme_.gapEm     = 0;
    const float captionPad = 2 * cap * compactTheme_.paddingEm;
    const float pairs      = std::max(controlWidths_[0] + controlWidths_[1],
                                      controlWidths_[2] + controlWidths_[3]) +
                        2 * captionPad + line * compactTheme_.gapEm;
    minimumWidthPx_ =
        std::max(3 * (std::ceil(std::max(
                          metrics.px(sourceTheme.type.minTouchPx), line)) +
                      2) +
                     metrics.px(sourceTheme.type.minTouchPx),
                 pairs + 4 * line * compactTheme_.paddingEm +
                     metrics.px(sourceTheme.type.minTouchPx) + 2);
    compactSource_  = sourceTheme;
    compactMetrics_ = metrics;
  }
  const auto &theme = compactTheme_;
  layout(metrics, theme);
  if (drawerW_ < 1 || drawerH_ <= 0) {
    focus_.reset();
    return;
  }
  rebuildModels(metrics, theme);
  float headerBottom = forgeWidget_.y() + forgeWidget_.height();
  for (const auto &zone : pouchManager_.zones())
    headerBottom = std::max(headerBottom, zone->y() + zone->height());
  header_->setVisible(true);
  const gleditor::ui::Rect hb{forgeWidget_.x(), headerBottom,
                              forgeWidget_.width(),
                              drawerY_ + drawerH_ - headerBottom};
  const auto equal = [](const auto &a, const auto &b) {
    return a.left == b.left && a.bottom == b.bottom && a.width == b.width &&
           a.height == b.height;
  };
  if (!headerBounds_ || !equal(*headerBounds_, hb)) {
    header_->setBounds(hb);
    headerBounds_ = hb;
  }
  std::uint64_t stamp = 0;
  const auto mix      = [&](std::uint64_t value) {
    stamp ^= value + 0x9e3779b97f4a7c15ULL + (stamp << 6U) + (stamp >> 2U);
  };
  const auto draw = [&](gleditor::ui::ScreenOverlay &overlay,
                        const gleditor::ui::Theme &effective) {
    const auto scene = overlay.prepare(metrics, effective);
    if (scene) mix(overlay.layoutRevision());
    gleditor::FrameContext prepared{.state          = ctx.state,
                                    .viewProjection = ctx.viewProjection,
                                    .screenWidth    = ctx.screenWidth,
                                    .screenHeight   = ctx.screenHeight,
                                    .timeline       = ctx.timeline,
                                    .chrome         = ctx.chrome,
                                    .settledChrome  = ctx.settledChrome,
                                    .metrics        = metrics,
                                    .theme          = effective};
    if (device_) overlay.drawFrame(prepared);
  };
  if (!resizeHandle_) {
    resizeHandle_ = std::make_unique<gleditor::ui::ScreenOverlay>(
        gleditor::ui::Widget{.id       = resizeId_,
                             .model    = gleditor::ui::Button{"↔", "resize"},
                             .fontRole = gleditor::ui::FontRole::Caption});
    resizeHandle_->setActionHandler([this](const auto &) {
      requestWidth(preparedMetrics_ ? preparedMetrics_->logical(
                                          currentSlideWidth_ + handleWidthPx_)
                                    : config_.widthPx);
    });
    if (device_) resizeHandle_->deviceReady(*device_, *pipeline_);
  }
  const gleditor::ui::Rect handle{
      side_ == DockSide::Left ? drawerX_ + drawerW_ - handleWidthPx_ : drawerX_,
      drawerY_, handleWidthPx_, drawerH_};
  if (!resizeBounds_ || !equal(*resizeBounds_, handle)) {
    resizeHandle_->setModel({.id        = resizeId_,
                             .model     = gleditor::ui::Button{"↔", "resize"},
                             .preferred = {metrics.logical(handle.width),
                                           metrics.logical(handle.height)},
                             .fontRole  = gleditor::ui::FontRole::Caption});
    resizeHandle_->setBounds(handle);
    resizeBounds_ = handle;
  }
  draw(*resizeHandle_, handleTheme_);
  draw(*header_, theme);
  for (auto &entry : presentations_) {
    const auto zone = pouchManager_.zoneById(entry.zone);
    if (!zone) continue;
    entry.overlay->setVisible(true);
    const gleditor::ui::Rect bounds{zone->x(), zone->y(), zone->width(),
                                    zone->height()};
    if (!entry.bounds || !equal(*entry.bounds, bounds)) {
      entry.overlay->setBounds(bounds);
      entry.bounds = bounds;
    }
    draw(*entry.overlay, entry.theme);
  }
  forgeWidget_.setVisible(true);
  const auto forgeScene =
      forgeWidget_.prepareBench(metrics, theme,
                                {forgeWidget_.x(), forgeWidget_.y(),
                                 forgeWidget_.width(), forgeWidget_.height()},
                                forgeCompact_);
  if (forgeScene) mix(forgeWidget_.presentation().layoutRevision());
  forgeWidget_.update(.016F);
  if (device_) {
    gleditor::FrameContext prepared{.state          = ctx.state,
                                    .viewProjection = ctx.viewProjection,
                                    .screenWidth    = ctx.screenWidth,
                                    .screenHeight   = ctx.screenHeight,
                                    .timeline       = ctx.timeline,
                                    .chrome         = ctx.chrome,
                                    .settledChrome  = ctx.settledChrome,
                                    .metrics        = metrics,
                                    .theme          = theme};
    forgeWidget_.drawPrepared(prepared);
  }
  if (isOpen_ && (!focus_ || stamp != focusStamp_)) {
    auto combined = std::make_shared<gleditor::ui::LayoutResult>();
    if (const auto scene = resizeHandle_->snapshot())
      combined->append(scene->layout);
    if (const auto scene = header_->snapshot()) combined->append(scene->layout);
    for (const auto &entry : presentations_)
      if (const auto scene = entry.overlay->snapshot())
        combined->append(scene->layout);
    if (forgeScene) combined->append(forgeScene->layout);
    focus_      = std::move(combined);
    focusStamp_ = stamp;
    ++a11yRevision_;
  }
}

bool PouchDrawer::picked(const render::PickingResult &pick,
                         RenderState &state) {
  const std::scoped_lock lock(guard_);
  if (!isOpen_) return false;
  if (pick.overlayWidgetId && focus_ && focus_->find(*pick.overlayWidgetId))
    requestFocus(*pick.overlayWidgetId);
  if (resizeHandle_ && resizeHandle_->picked(pick, state)) return true;
  if (header_ && header_->picked(pick, state)) return true;
  for (auto &entry : presentations_)
    if (entry.overlay->picked(pick, state)) return true;
  return forgeWidget_.picked(pick, state);
}

void PouchDrawer::describe(gleditor::a11y::Builder &into) {
  const std::scoped_lock lock(guard_);
  if (!isOpen_) return;
  if (resizeBounds_ && preparedMetrics_) {
    auto &node = into.add(resizeId_, gleditor::a11y::Role::Button);
    node.label = "Resize pouch drawer";
    node.value = std::to_string(preparedMetrics_->logical(currentSlideWidth_));
    node.description = "Drag horizontally, or use Left and Right arrow keys. "
                       "Activate to widen.";
    node.focusable   = true;
    node.actions     = gleditor::a11y::bit(gleditor::a11y::Action::Click) |
                   gleditor::a11y::bit(gleditor::a11y::Action::SetValue);
    const auto scene = resizeHandle_->snapshot();
    const auto *box  = scene ? scene->layout.find(resizeId_) : nullptr;
    const auto &b    = box ? box->rect : *resizeBounds_;
    const auto h     = preparedMetrics_->screenHeight;
    node.bounds      = gleditor::a11y::Rect{b.left, h - b.bottom - b.height,
                                       b.left + b.width, h - b.bottom};
    into.contribute(into.id(resizeId_));
  }
  if (header_) header_->describe(into);
  for (auto &entry : presentations_) entry.overlay->describe(into);
  forgeWidget_.describe(into);
}
std::uint64_t PouchDrawer::accessibilityRevision() const {
  const std::scoped_lock lock(guard_);
  std::uint64_t revision = a11yRevision_ + pouchManager_.store().opCount() +
                           forgeWidget_.accessibilityRevision();
  if (header_) revision += header_->accessibilityRevision();
  if (resizeHandle_) revision += resizeHandle_->accessibilityRevision();
  for (const auto &entry : presentations_)
    revision += entry.overlay->accessibilityRevision();
  return revision;
}
bool PouchDrawer::performAction(std::uint64_t id, gleditor::a11y::Action action,
                                std::string_view value) {
  const std::scoped_lock lock(guard_);
  if (!isOpen_) return false;
  const auto local =
      static_cast<std::uint32_t>(gleditor::a11y::Ids::localOf(id));
  if (focus_ && focus_->find(local)) requestFocus(local);
  if (action == gleditor::a11y::Action::Focus && focus_ && focus_->find(local))
    return true;
  if (local == resizeId_ && preparedMetrics_) {
    if (action == gleditor::a11y::Action::Click) {
      requestWidth(
          preparedMetrics_->logical(currentSlideWidth_ + handleWidthPx_));
      return true;
    }
    if (action == gleditor::a11y::Action::SetValue) {
      float width{};
      const auto result =
          std::from_chars(value.data(), value.data() + value.size(), width);
      if (result.ec != std::errc{} ||
          result.ptr != value.data() + value.size() || !std::isfinite(width))
        return false;
      requestWidth(width);
      return true;
    }
    return false;
  }
  if (header_ && header_->performAction(id, action, value)) return true;
  for (auto &entry : presentations_)
    if (entry.overlay->performAction(id, action, value)) return true;
  return forgeWidget_.performAction(id, action, value);
}
std::shared_ptr<const gleditor::ui::LayoutResult>
PouchDrawer::focusLayout() const {
  const std::scoped_lock lock(guard_);
  return focus_;
}
void PouchDrawer::focusedNodeChanged(std::uint32_t id) {
  const std::scoped_lock lock(guard_);
  focusedNode_ = id;
  if (resizeHandle_) resizeHandle_->focusedNodeChanged(id);
  if (header_) header_->focusedNodeChanged(id);
  for (auto &entry : presentations_) entry.overlay->focusedNodeChanged(id);
  forgeWidget_.presentation().focusedNodeChanged(id);
}
bool PouchDrawer::activateNode(std::uint32_t id) {
  const std::scoped_lock lock(guard_);
  if (!isOpen_) return false;
  if (resizeHandle_ && resizeHandle_->activateNode(id)) return true;
  if (header_ && header_->activateNode(id)) return true;
  for (auto &entry : presentations_)
    if (entry.overlay->activateNode(id)) return true;
  return forgeWidget_.presentation().activateNode(id);
}
bool PouchDrawer::pointerEvent(const gleditor::ui::PointerEvent &event) {
  const std::scoped_lock lock(guard_);
  if (!isOpen_) return false;
  using gleditor::ui::PointerPhase;
  if (resize_) {
    if (event.pointerId != resize_->pointer) return false;
    if (event.phase == PointerPhase::Cancel) {
      cancelResize();
      return true;
    }
    if (event.phase == PointerPhase::Move ||
        event.phase == PointerPhase::Release) {
      const float delta = event.x - resize_->startX;
      config_.widthPx =
          delta == 0
              ? resize_->originalWidth
              : clampedWidth(
                    resize_->startActualWidth +
                    preparedMetrics_->logical(
                        resize_->side == DockSide::Left ? delta : -delta));
      snapNextLayout_ = true;
      if (event.phase == PointerPhase::Release) {
        if (config_.widthPx != resize_->originalWidth)
          widthCommit_ = WidthCommit{config_.widthPx};
        resize_.reset();
      }
      return true;
    }
  }
  if (event.phase == PointerPhase::Press && event.button == 1 &&
      resizeBounds_ && preparedMetrics_ && event.x >= resizeBounds_->left &&
      event.x <= resizeBounds_->left + resizeBounds_->width &&
      preparedMetrics_->screenHeight - event.y >= resizeBounds_->bottom &&
      preparedMetrics_->screenHeight - event.y <=
          resizeBounds_->bottom + resizeBounds_->height) {
    resize_ =
        ResizeGesture{event.pointerId, event.x, config_.widthPx,
                      preparedMetrics_->logical(currentSlideWidth_), side_};
    requestFocus(resizeId_);
    return true;
  }
  if (event.phase == PointerPhase::Press && focus_ && preparedMetrics_) {
    if (const auto box =
            focus_->hitTest(event.x, preparedMetrics_->screenHeight - event.y);
        box && box->focusable && box->enabled)
      requestFocus(box->id);
  }
  if (header_ && header_->pointerEvent(event)) return true;
  for (auto &entry : presentations_)
    if (entry.overlay->pointerEvent(event)) return true;
  return forgeWidget_.presentation().pointerEvent(event);
}
void PouchDrawer::focusChanged(bool focused) {
  const std::scoped_lock lock(guard_);
  if (!focused) cancelResize();
  if (resizeHandle_) resizeHandle_->focusChanged(focused);
  if (header_) header_->focusChanged(focused);
  for (auto &entry : presentations_) entry.overlay->focusChanged(focused);
  forgeWidget_.presentation().focusChanged(focused);
}
std::optional<gleditor::InputArea> PouchDrawer::pointerArea() const {
  const std::scoped_lock lock(guard_);
  if (!isOpen_ || !preparedMetrics_) return std::nullopt;
  return gleditor::ui::toInputArea({drawerX_, drawerY_, drawerW_, drawerH_},
                                   preparedMetrics_->screenHeight);
}
float PouchDrawer::clampedWidth(float logical) const {
  if (!preparedMetrics_) return std::max(1.0F, logical);
  const auto &m = *preparedMetrics_;
  return std::clamp(
      logical,
      m.logical(std::min(m.pixelSafeArea().width * config_.maxWidthShare,
                         std::max({minimumWidthPx_, adaptiveWidthPx_,
                                   m.px(preparedTheme_.type.minTouchPx) * 4}))),
      m.logical(m.pixelSafeArea().width * config_.maxWidthShare));
}
void PouchDrawer::cancelResize() {
  if (!resize_) return;
  config_.widthPx = resize_->originalWidth;
  resize_.reset();
  snapNextLayout_ = true;
}
void PouchDrawer::requestWidth(float logical) {
  const std::scoped_lock lock(guard_);
  if (!isOpen_ || !preparedMetrics_) return;
  const float width = clampedWidth(logical);
  if (width == config_.widthPx) return;
  config_.widthPx = width;
  widthCommit_    = WidthCommit{width};
  snapNextLayout_ = true;
}
bool PouchDrawer::keyPressed(gleditor::Key key, gleditor::KeyMods mods) {
  const std::scoped_lock lock(guard_);
  if (!isOpen_) return false;
  if (key == gleditor::Key::Escape) {
    setOpen(false);
    return true;
  }
  if (focusedNode_ == resizeId_ &&
      (key == gleditor::Key::Left || key == gleditor::Key::Right) &&
      preparedMetrics_) {
    requestWidth(preparedMetrics_->logical(currentSlideWidth_) +
                 preparedMetrics_->logical(handleWidthPx_) *
                     (key == gleditor::Key::Right ? 1 : -1));
    return true;
  }
  const gleditor::ui::KeyEvent event{key, mods, std::nullopt};
  if (resizeHandle_ && resizeHandle_->keyPressed(event)) return true;
  if (header_ && header_->keyPressed(event)) return true;
  for (auto &entry : presentations_)
    if (entry.overlay->keyPressed(event)) return true;
  return forgeWidget_.presentation().keyPressed(event);
}
} // namespace xanadu

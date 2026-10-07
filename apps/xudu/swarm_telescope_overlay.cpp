/**
 * @file swarm_telescope_overlay.cpp
 * @brief Retained discovery controls over the asynchronous swarm catalog.
 */
#include "swarm_telescope_overlay.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

#include <gleditor/render_state.hpp>
#include <gleditor/text/font.hpp>

namespace xudu {
namespace ui = gleditor::ui;
namespace {
std::string formatBytes(const std::uint64_t bytes) {
  constexpr auto kibibyte = 1024ULL;
  if (bytes < kibibyte) return std::to_string(bytes) + " B";
  std::ostringstream output;
  output << std::fixed << std::setprecision(1);
  if (bytes < kibibyte * kibibyte)
    output << static_cast<double>(bytes) / kibibyte << " KiB";
  else
    output << static_cast<double>(bytes) / (kibibyte * kibibyte) << " MiB";
  return output.str();
}
constexpr ui::WidgetId channelsList = 10, resultsList = 11, inspectorList = 12;
} // namespace

SwarmTelescopeOverlay::SwarmTelescopeOverlay(SwarmCatalog &catalog,
                                             RendererRef renderer,
                                             std::string fontName)
    : catalog_(catalog), renderer_(std::move(renderer)),
      fontName_(std::move(fontName)),
      overlay_({.id = 1, .model = ui::Modal{}}) {
  overlay_.setVisible(false);
  overlay_.setActionHandler([this](const auto &action) { queue(action); });
  refreshSearch();
}
SwarmTelescopeOverlay::~SwarmTelescopeOverlay() { releaseFocus(); }
void SwarmTelescopeOverlay::deviceReady(render::RenderDevice &device,
                                        const render::PipelineDesc &pipeline) {
  overlay_.deviceReady(device, pipeline);
}
void SwarmTelescopeOverlay::changed() {
  dirty_ = true;
  ++revision_;
}
void SwarmTelescopeOverlay::setConfig(const ModalPresentationConfig &config) {
  const std::scoped_lock lock(guard_);
  if (config_ != config) {
    config_ = config;
    changed();
  }
}
bool SwarmTelescopeOverlay::busy() const {
  const std::scoped_lock lock(guard_);
  return !pending_.empty();
}
void SwarmTelescopeOverlay::setVisible(const bool visible) {
  const std::scoped_lock lock(guard_);
  if (visible_ == visible) return;
  visible_   = visible;
  searchId_  = 0;
  focusedId_ = 0;
  rowIdentities_.clear();
  ++generation_;
  pending_.clear();
  overlay_.setVisible(visible);
  changed();
  if (visible) {
    page_ = 1;
    activate();
    refreshSearch();
  } else {
    deactivate();
  }
}
void SwarmTelescopeOverlay::toggle() {
  const std::scoped_lock lock(guard_);
  setVisible(!visible_);
}
bool SwarmTelescopeOverlay::isVisible() const noexcept {
  const std::scoped_lock lock(guard_);
  return visible_;
}
bool SwarmTelescopeOverlay::grabbing() const { return isVisible(); }
void SwarmTelescopeOverlay::setSearchQuery(const std::string_view query) {
  const std::scoped_lock lock(guard_);
  searchQuery_ = query;
  searchCaret_ = searchQuery_.size();
  refreshSearch();
}
std::string SwarmTelescopeOverlay::searchQuery() const {
  const std::scoped_lock lock(guard_);
  return searchQuery_;
}
void SwarmTelescopeOverlay::selectCategory(const CatalogCategory category) {
  const std::scoped_lock lock(guard_);
  activeCategory_ = category;
  scrollPx_       = {};
  refreshSearch();
}
void SwarmTelescopeOverlay::selectItem(const std::size_t index) {
  const std::scoped_lock lock(guard_);
  if (index >= currentResults_.size()) return;
  selectedResultIndex_ = index;
  summonId_            = 0;
  scrollPx_[2]         = 0;
  ++generation_;
  changed();
}
void SwarmTelescopeOverlay::refreshSearch() {
  const std::scoped_lock lock(guard_);
  const auto selectedHash =
      selectedResultIndex_ < currentResults_.size()
          ? currentResults_[selectedResultIndex_].entry.infoHash
          : std::string{};
  summonId_       = 0;
  currentResults_ = catalog_.search(searchQuery_, activeCategory_);
  const auto selected =
      std::ranges::find(currentResults_, selectedHash,
                        [](const auto &r) { return r.entry.infoHash; });
  selectedResultIndex_ =
      selected == currentResults_.end()
          ? 0
          : static_cast<std::size_t>(selected - currentResults_.begin());
  channels_.clear();
  if (activeCategory_ == CatalogCategory::TopicSwarms) {
    for (const auto &[topic, count] : catalog_.index().topTopics()) {
      static_cast<void>(count);
      channels_.push_back("#" + topic);
    }
  } else if (activeCategory_ == CatalogCategory::FollowedAuthors) {
    for (const auto &author : catalog_.followedAuthors())
      channels_.push_back(author.name);
  } else {
    for (const auto &result : currentResults_)
      channels_.push_back(result.entry.title);
  }
  scrollPx_ = {};
  ++generation_;
  changed();
}

std::shared_ptr<const ui::WidgetScene>
SwarmTelescopeOverlay::prepare(const ui::UiMetrics &metrics,
                               const ui::Theme &sourceTheme) {
  const std::scoped_lock lock(guard_);
  if (!visible_) return {};
  if (!dirty_ && metrics_ == metrics && sourceTheme_ == sourceTheme)
    return overlay_.snapshot();
  metrics_     = metrics;
  sourceTheme_ = sourceTheme;
  theme_ = ui::withFontOverride(sourceTheme, ui::FontRole::Label, fontName_);
  // Nested controls need their line budget at large font scales on small
  // displays.
  theme_.paddingEm = std::min(theme_.paddingEm, .125F);
  theme_.gapEm     = std::min(theme_.gapEm, .125F);
  const auto font  = gleditor::text::FontManager::instance().getFont(
      metrics.fontDescription(ui::FontRole::Label, theme_));
  const auto caption = gleditor::text::FontManager::instance().getFont(
      metrics.fontDescription(ui::FontRole::Caption, theme_));
  const auto safe  = metrics.pixelSafeArea();
  const auto share = [](float value) {
    return std::isfinite(value) ? std::clamp(value, .1F, 1.F) : .95F;
  };
  const auto extent = [&](float requested, float fallback, float available) {
    return std::min(available,
                    metrics.px(std::isfinite(requested) && requested > 0
                                   ? requested
                                   : fallback));
  };
  const auto width =
      extent(config_.widthPx, 860, safe.width * share(config_.maxWidthShare));
  const auto height = extent(config_.heightPx, 560,
                             safe.height * share(config_.maxHeightShare));
  const auto bounds = ui::clampToSafeArea(
      metrics.rounded({safe.left + (safe.width - width) / 2,
                       safe.bottom + (safe.height - height) / 2, width,
                       height}),
      safe);
  const auto touch         = metrics.px(theme_.type.minTouchPx);
  const auto controlHeight = [&]() {
    return std::ceil(std::max(touch, caption->metrics().lineHeight *
                                         (1 + 2 * theme_.paddingEm))) +
           2;
  };
  // Tabs pad their group and each button, so both levels need a line budget.
  const auto tabsHeight = [&]() {
    return std::ceil(std::max(touch, caption->metrics().lineHeight *
                                         (1 + 4 * theme_.paddingEm))) +
           4;
  };
  const auto minimumHeight = [&]() {
    return 3 * controlHeight() + 2 * tabsHeight() +
           2 * caption->metrics().lineHeight * theme_.paddingEm +
           2 * font->metrics().lineHeight * theme_.paddingEm +
           5 * font->metrics().lineHeight * theme_.gapEm +
           font->metrics().lineHeight * (1 + 3 * theme_.paddingEm) + 8;
  };
  if (minimumHeight() > bounds.height) {
    theme_.paddingEm = 0;
    theme_.gapEm     = 0;
  }
  const auto padding    = font->metrics().lineHeight * theme_.paddingEm;
  const auto gap        = font->metrics().lineHeight * theme_.gapEm;
  const auto control    = controlHeight();
  const auto tabControl = tabsHeight();
  rowHeight_            = std::ceil(std::max(touch, font->metrics().lineHeight *
                                                        (1 + theme_.paddingEm))) +
               2;
  listRowHeight_.fill(rowHeight_);
  listRowHeight_[2] =
      std::ceil(font->metrics().lineHeight * 3 * (1 + theme_.type.lineGapEm) +
                padding) +
      2;
  const auto id = [&]() {
    if (nextId_ == std::numeric_limits<ui::WidgetId>::max())
      throw std::length_error("Telescope action identities exhausted");
    return nextId_++;
  };
  const auto rowId = [&](std::string key) {
    const auto found = rowIdentities_.find(key);
    if (found != rowIdentities_.end()) return found->second;
    const auto identity = id();
    rowIdentities_.emplace(std::move(key), identity);
    return identity;
  };
  if (!searchId_) {
    searchId_   = id();
    closeId_    = id();
    refreshId_  = id();
    categoryId_ = id();
    pageId_     = id();
    for (auto &tab : categoryTabIds_) tab = id();
    for (auto &tab : pageTabIds_) tab = id();
  }
  if (!summonId_) summonId_ = id();
  const auto logical = [&](float pixels) {
    return metrics.logical(std::max(0.F, pixels));
  };
  const auto button = [&](ui::WidgetId identity, std::string label,
                          std::string action, float w = 0.F) {
    return ui::Widget{.id    = identity,
                      .model = ui::Button{std::move(label), std::move(action)},
                      .preferred = {logical(w), logical(control)},
                      .fontRole  = ui::FontRole::Caption};
  };
  const auto innerWidth = std::max(0.F, bounds.width - padding * 2);
  const auto closeWidth = std::max(
      metrics.px(theme_.type.minTouchPx),
      measurements_.fitted("Close", caption, {}).widthPx + padding * 2 + 2);
  const auto refreshWidth = std::max(
      metrics.px(theme_.type.minTouchPx),
      measurements_.fitted("Refresh", caption, {}).widthPx + padding * 2 + 2);
  const auto headerPadding = caption->metrics().lineHeight * theme_.paddingEm;
  ui::Widget header{
      .id        = 2,
      .model     = ui::ButtonFlow{},
      .children  = {{.id        = 3,
                     .model     = ui::Label{"Swarm Telescope"},
                     .preferred = {logical(innerWidth - closeWidth -
                                           refreshWidth - 2 * gap -
                                           2 * headerPadding - 2),
                                   logical(control)},
                     .fontRole  = ui::FontRole::Caption},
                    button(refreshId_, "Refresh", "refresh", refreshWidth),
                    button(closeId_, "Close", "close", closeWidth)},
      .preferred = {0, logical(control + 2 * headerPadding)},
      .fontRole  = ui::FontRole::Caption};
  ui::Widget model{
      .id = 1, .model = ui::Modal{}, .children = {std::move(header)}};
  model.children.push_back(
      {.id        = searchId_,
       .model     = ui::TextField{searchQuery_, "Search publications", "search",
                              searchCaret_},
       .preferred = {0, logical(control)},
       .fontRole  = ui::FontRole::Caption});
  ui::Tabs categories{{{categoryTabIds_[0], "Topics", "category"},
                       {categoryTabIds_[1], "Authors", "category"},
                       {categoryTabIds_[2], "Recent local", "category"}},
                      activeCategory_ == CatalogCategory::TopicSwarms ? 0U
                      : activeCategory_ == CatalogCategory::FollowedAuthors
                          ? 1U
                          : 2U};
  model.children.push_back({.id        = categoryId_,
                            .model     = std::move(categories),
                            .preferred = {0, logical(tabControl)},
                            .fontRole  = ui::FontRole::Caption});
  model.children.push_back(
      {.id        = pageId_,
       .model     = ui::Tabs{{{pageTabIds_[0], "Channels", "page"},
                              {pageTabIds_[1], "Publications", "page"},
                              {pageTabIds_[2], "Inspector", "page"}},
                         page_},
       .preferred = {0, logical(tabControl)},
       .fontRole  = ui::FontRole::Caption});
  const auto viewport =
      std::max(0.F, bounds.height - 2 * padding -
                        (3 * control + 2 * tabControl + 2 * headerPadding) -
                        5 * gap - 2);
  viewportHeight_.fill(viewport - 2 * padding);
  std::array<ui::List, 3> lists;
  for (std::size_t i = 0; i < channels_.size(); ++i)
    lists[0].rows.push_back(
        {rowId("channel:" +
               std::to_string(static_cast<unsigned>(activeCategory_)) + ":" +
               channels_[i]),
         channels_[i], "channel"});
  for (const auto &result : currentResults_) {
    std::string label = result.entry.title + " — " + result.entry.authorName +
                        " | " + std::to_string(result.seederCount) + " seeders";
    if (result.isVerified) label += " | Merkle verified";
    lists[1].rows.push_back({rowId("publication:" + result.entry.infoHash),
                             std::move(label), "select"});
  }
  if (selectedResultIndex_ < currentResults_.size()) {
    const auto &result = currentResults_[selectedResultIndex_];
    const auto &entry  = result.entry;
    std::vector<std::string> details{
        entry.title,
        "Author: " + entry.authorName,
        "Key: " + entry.authorFingerprint,
        "Size: " + formatBytes(entry.totalBytes) + " | " +
            std::to_string(entry.microversions) + " microversions",
        "Health: " + std::to_string(result.seederCount) + " seeders | " +
            std::to_string(result.peerCount) + " peers",
        "Abstract: " + entry.abstractText,
        "URI: " + entry.bep46Uri,
        result.isVerified ? "Merkle verified"
                          : "Merkle verification unavailable"};
    for (const auto &topic : entry.topics) details.push_back("Topic: " + topic);
    std::size_t detailIndex = 0;
    for (auto &label : details) {
      const auto purpose =
          label.starts_with("Key:") || label.starts_with("URI:")
              ? ui::TextPurpose::Identifier
              : ui::TextPurpose::Description;
      lists[2].rows.push_back(
          {rowId("inspector:" + entry.infoHash + ":" +
                 std::to_string(detailIndex++) + ":" + label),
           std::move(label), "inspect", true, purpose});
    }
  }
  if (lists[0].rows.empty())
    lists[0].rows.push_back(
        {rowId("empty-channels"), "No channels in this category", "inspect"});
  if (lists[1].rows.empty())
    lists[1].rows.push_back(
        {rowId("empty-publications"), "No matching publications", "inspect"});
  if (lists[2].rows.empty())
    lists[2].rows.push_back({rowId("empty-inspector"),
                             "Select a publication to inspect", "inspect"});
  const auto wide = innerWidth >= font->metrics().lineHeight * 36;
  if (wide) {
    ui::Widget columns{.id        = 4,
                       .model     = ui::ButtonFlow{},
                       .preferred = {0, logical(viewport)}};
    const auto columnWidth =
        std::floor((innerWidth - 2 * padding - 2 * gap - 2) / 3);
    for (std::size_t i = 0; i < lists.size(); ++i) {
      lists[i].scrollPx    = scrollPx_[i];
      lists[i].rowHeightPx = listRowHeight_[i];
      columns.children.push_back(
          {.id        = channelsList + static_cast<ui::WidgetId>(i),
           .model     = std::move(lists[i]),
           .preferred = {logical(columnWidth),
                         logical(viewport - 2 * padding - 2)}});
    }
    model.children.push_back(std::move(columns));
  } else {
    lists[page_].scrollPx    = scrollPx_[page_];
    lists[page_].rowHeightPx = listRowHeight_[page_];
    model.children.push_back(
        {.id        = channelsList + static_cast<ui::WidgetId>(page_),
         .model     = std::move(lists[page_]),
         .preferred = {0, logical(viewport)}});
  }
  auto summon = button(summonId_, "Open selected publication", "summon");
  std::get<ui::Button>(summon.model).enabled =
      selectedResultIndex_ < currentResults_.size();
  model.children.push_back(std::move(summon));
  overlay_.setModel(std::move(model));
  overlay_.setBounds(bounds);
  auto scene = overlay_.prepare(metrics, theme_);
  dirty_     = false;
  if (!focusedId_) {
    focusedId_ = searchId_;
    requestFocus(searchId_);
    overlay_.focusedNodeChanged(searchId_);
  }
  return scene;
}

void SwarmTelescopeOverlay::queue(const ui::WidgetAction &action) {
  const std::scoped_lock lock(guard_);
  if (!visible_) return;
  if (action.action == "search") {
    // Editing the shared field must keep accepting composed text until redraw.
    searchCaret_ = action.caret.value_or(action.value.size());
    if (searchQuery_ != action.value) {
      searchQuery_ = action.value;
      refreshSearch();
    } else {
      changed();
    }
    return;
  }
  if (dirty_) return;
  Pending pending{
      action.action, action.value, action.itemIndex, generation_, {}};
  if (action.action == "select" && action.itemIndex < currentResults_.size())
    pending.entry = currentResults_[action.itemIndex].entry;
  if (action.action == "summon" &&
      selectedResultIndex_ < currentResults_.size())
    pending.entry = currentResults_[selectedResultIndex_].entry;
  pending_.push_back(std::move(pending));
}
void SwarmTelescopeOverlay::drain() {
  auto pending = std::move(pending_);
  pending_.clear();
  for (const auto &action : pending) {
    if (!visible_ || action.generation != generation_) continue;
    if (action.action == "close")
      setVisible(false);
    else if (action.action == "refresh")
      refreshSearch();
    else if (action.action == "category") {
      selectCategory(action.item == 0   ? CatalogCategory::TopicSwarms
                     : action.item == 1 ? CatalogCategory::FollowedAuthors
                                        : CatalogCategory::RecentLocal);
    } else if (action.action == "page") {
      page_ = std::min<std::size_t>(2, action.item);
      changed();
    } else if (action.action == "channel" && action.item < channels_.size()) {
      // Channels select real metadata through XQL, never a fabricated local
      // row.
      if (activeCategory_ == CatalogCategory::TopicSwarms)
        setSearchQuery(channels_[action.item]);
      else if (activeCategory_ == CatalogCategory::FollowedAuthors)
        setSearchQuery("author:\"" + channels_[action.item] + "\"");
      else
        selectItem(action.item);
      page_ = 1;
      changed();
    } else if (action.action == "select" && action.entry) {
      const auto found =
          std::ranges::find(currentResults_, action.entry->infoHash,
                            [](const auto &r) { return r.entry.infoHash; });
      if (found != currentResults_.end())
        selectItem(static_cast<std::size_t>(found - currentResults_.begin()));
    } else if (action.action == "summon" && action.entry && onSummon_) {
      onSummon_(*action.entry);
      setVisible(false);
    }
  }
}
void SwarmTelescopeOverlay::drawFrame(gleditor::FrameContext &ctx) {
  const std::scoped_lock lock(guard_);
  drain();
  if (!visible_) return;
  auto metrics         = ctx.metrics;
  metrics.screenWidth  = ctx.screenWidth;
  metrics.screenHeight = ctx.screenHeight;
  metrics.chrome       = ctx.chrome;
  std::ignore          = prepare(metrics, ctx.theme);
  gleditor::FrameContext draw{
      ctx.state,    ctx.viewProjection, ctx.screenWidth,   ctx.screenHeight,
      ctx.timeline, ctx.chrome,         ctx.settledChrome, metrics,
      theme_};
  overlay_.drawFrame(draw);
}
bool SwarmTelescopeOverlay::picked(const render::PickingResult &pick,
                                   RenderState &state) {
  const std::scoped_lock lock(guard_);
  if (!visible_ || dirty_) return false;
  const auto handled = overlay_.picked(pick, state);
  if (handled && pick.overlayWidgetId) requestFocus(*pick.overlayWidgetId);
  return handled;
}
void SwarmTelescopeOverlay::setOnSummon(SummonHandler handler) {
  const std::scoped_lock lock(guard_);
  onSummon_ = std::move(handler);
}
void SwarmTelescopeOverlay::setSampleForceVisible(const bool force) {
  const std::scoped_lock lock(guard_);
  sampleForceVisible_ = force;
  if (force) setVisible(true);
}
bool SwarmTelescopeOverlay::keyPressed(const gleditor::Key key,
                                       const gleditor::KeyMods mods) {
  const std::scoped_lock lock(guard_);
  if (!visible_) return false;
  if (key == gleditor::Key::Escape) {
    setVisible(false);
    return true;
  }
  if (key == gleditor::Key::PageDown || key == gleditor::Key::PageUp) {
    scroll((key == gleditor::Key::PageDown ? 1 : -1) * viewportHeight_[page_],
           channelsList + static_cast<ui::WidgetId>(page_));
    return true;
  }
  if (key == gleditor::Key::Return && focusedId_ == searchId_) {
    refreshSearch();
    page_ = 1;
    changed();
    return true;
  }
  if (key == gleditor::Key::Return && !dirty_) {
    const auto scene   = overlay_.snapshot();
    const auto *visual = scene ? scene->find(focusedId_) : nullptr;
    if (visual && visual->ownerId == resultsList &&
        visual->action == "select" &&
        visual->itemIndex < currentResults_.size()) {
      pending_.push_back({"summon",
                          {},
                          visual->itemIndex,
                          generation_,
                          currentResults_[visual->itemIndex].entry});
      return true;
    }
  }
  if (overlay_.keyPressed({key, mods})) return true;
  if (key == gleditor::Key::Return && focusedId_)
    return activateNode(focusedId_);
  return false;
}
void SwarmTelescopeOverlay::textTyped(const std::string &utf8) {
  const std::scoped_lock lock(guard_);
  if (visible_) overlay_.textTyped(utf8);
}
std::optional<gleditor::InputArea> SwarmTelescopeOverlay::textArea() const {
  return overlay_.textArea();
}
std::uint64_t SwarmTelescopeOverlay::accessibilityRevision() const {
  const std::scoped_lock lock(guard_);
  return revision_ + overlay_.accessibilityRevision();
}
void SwarmTelescopeOverlay::describe(gleditor::a11y::Builder &into) {
  const std::scoped_lock lock(guard_);
  const auto scene = overlay_.snapshot();
  if (!visible_ || !scene) return;
  for (const auto &visual : scene->visuals) {
    const auto *box = scene->layout.find(visual.id);
    if (!box) continue;
    auto &node =
        into.add(visual.id, visual.id == 1 ? gleditor::a11y::Role::Dialog
                                           : visual.accessibilityRole);
    node.label = visual.id == 1 ? "Swarm Telescope" : visual.accessibleLabel;
    node.value = visual.value;
    const auto rect = box->rect;
    node.bounds     = gleditor::a11y::Rect{
        rect.left, metrics_.screenHeight - rect.bottom - rect.height,
        rect.left + rect.width, metrics_.screenHeight - rect.bottom};
    node.focusable =
        visual.interactive && box->enabled && rect.width > 0 && rect.height > 0;
    if (node.focusable)
      node.actions = gleditor::a11y::bit(gleditor::a11y::Action::Click) |
                     gleditor::a11y::bit(gleditor::a11y::Action::Focus);
    if (visual.textInput)
      node.actions |= gleditor::a11y::bit(gleditor::a11y::Action::SetValue);
    if (visual.fitted.truncated) node.description = visual.text;
    for (const auto &child : scene->layout.boxes)
      if (child.parentId == visual.id)
        node.children.push_back(into.id(child.id));
    if (box->parentId == 0) into.contribute(into.id(visual.id));
  }
}
bool SwarmTelescopeOverlay::performAction(std::uint64_t id,
                                          gleditor::a11y::Action action,
                                          std::string_view value) {
  const std::scoped_lock lock(guard_);
  if (!visible_ || dirty_) return false;
  const auto handled = overlay_.performAction(id, action, value);
  if (handled)
    requestFocus(static_cast<ui::WidgetId>(gleditor::a11y::Ids::localOf(id)));
  return handled;
}
std::shared_ptr<const ui::LayoutResult>
SwarmTelescopeOverlay::focusLayout() const {
  return overlay_.focusLayout();
}
void SwarmTelescopeOverlay::focusedNodeChanged(std::uint32_t id) {
  const std::scoped_lock lock(guard_);
  focusedId_ = id;
  overlay_.focusedNodeChanged(id);
}
bool SwarmTelescopeOverlay::activateNode(std::uint32_t id) {
  const std::scoped_lock lock(guard_);
  if (!visible_ || dirty_ || id == searchId_) return false;
  const auto scene   = overlay_.snapshot();
  const auto *visual = scene ? scene->find(id) : nullptr;
  if (visual && visual->ownerId == resultsList && visual->action == "select" &&
      visual->itemIndex < currentResults_.size()) {
    pending_.push_back({"summon",
                        {},
                        visual->itemIndex,
                        generation_,
                        currentResults_[visual->itemIndex].entry});
    return true;
  }
  return overlay_.activateNode(id);
}
void SwarmTelescopeOverlay::focusChanged(bool focused) {
  overlay_.focusChanged(focused);
}
std::optional<gleditor::InputArea> SwarmTelescopeOverlay::pointerArea() const {
  return overlay_.pointerArea();
}
void SwarmTelescopeOverlay::scroll(float delta, std::uint32_t owner) {
  const auto index = static_cast<std::size_t>(owner - channelsList);
  if (index >= scrollPx_.size()) return;
  std::size_t count = index == 0   ? channels_.size()
                      : index == 1 ? currentResults_.size()
                                   : 8;
  if (index == 2 && selectedResultIndex_ < currentResults_.size())
    count += currentResults_[selectedResultIndex_].entry.topics.size();
  scrollPx_[index] = std::clamp(
      scrollPx_[index] + delta, 0.F,
      std::max(0.F, static_cast<float>(count) * listRowHeight_[index] -
                        viewportHeight_[index]));
  changed();
}
bool SwarmTelescopeOverlay::pointerEvent(const ui::PointerEvent &event) {
  const std::scoped_lock lock(guard_);
  if (!visible_) return false;
  if (event.phase == ui::PointerPhase::Wheel) {
    const auto scene = overlay_.snapshot();
    if (!scene) return false;
    const auto *box = scene->layout.hitTest(
        event.x, static_cast<float>(metrics_.screenHeight) - event.y);
    if (!box) return false;
    const auto *visual = scene->find(box->id);
    const auto owner   = visual ? visual->ownerId : box->id;
    if (owner >= channelsList && owner <= inspectorList) {
      scroll(-event.deltaY * listRowHeight_[owner - channelsList], owner);
      return true;
    }
  }
  if (!dirty_ && event.phase == ui::PointerPhase::Press) {
    const auto scene = overlay_.snapshot();
    const auto *box =
        scene
            ? scene->layout.hitTest(
                  event.x, static_cast<float>(metrics_.screenHeight) - event.y)
            : nullptr;
    if (box && box->focusable && box->enabled) requestFocus(box->id);
  }
  return !dirty_ && overlay_.pointerEvent(event);
}
} // namespace xudu

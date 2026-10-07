/**
 * @file doc_switcher.cpp
 * @brief Retained document tabs sharing text fitting, metrics and layout.
 */
#include <gleditor/doc_switcher.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <utility>

#include <gleditor/doc.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/text/fit.hpp>
#include <gleditor/text/font.hpp>
#include <glm/ext/matrix_clip_space.hpp>

namespace gleditor {
namespace {
constexpr std::uint32_t barId     = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t tabsId    = barId - 1;
constexpr std::uint32_t actionsId = barId - 2;

std::string formatDocTitle(const std::string &rawName, std::size_t index) {
  if (rawName.empty() || rawName == "0")
    return "Doc " + std::to_string(index + 1);
  if (rawName.starts_with("system://")) return "⚙ " + rawName;
  return std::filesystem::path(rawName).filename().string();
}
ui::Rect textBox(ui::Rect parent, float padding, float lineHeight) {
  const float inset  = std::min(std::max(0.0F, padding), parent.width * 0.5F);
  const float height = std::min(lineHeight, parent.height);
  return {parent.left + inset, parent.bottom + (parent.height - height) * 0.5F,
          std::max(0.0F, parent.width - 2 * inset), height};
}
a11y::Rect accessibleBounds(ui::Rect box, int screenHeight) {
  return {box.left, static_cast<double>(screenHeight) - box.bottom - box.height,
          box.left + box.width, static_cast<double>(screenHeight) - box.bottom};
}
} // namespace

DocumentSwitcher::DocumentSwitcher(std::string aFontName)
    : fontName(std::move(aFontName)) {}
DocumentSwitcher::~DocumentSwitcher() = default;

void DocumentSwitcher::deviceReady(
    render::RenderDevice &nextDevice,
    const render::PipelineDesc &documentPipeline) {
  const std::scoped_lock lock(guard);
  device   = &nextDevice;
  pipeline = documentPipeline;
  canvas.reset();
  builtRevision = 0;
}

std::uint32_t
DocumentSwitcher::bindDocument(const std::shared_ptr<Doc> &document,
                               PickAction action) {
  const auto found =
      std::ranges::find_if(documentPickBindings, [&](const auto &b) {
        return b.second.action == action &&
               b.second.document.lock() == document;
      });
  if (found != documentPickBindings.end()) return found->first;
  if (nextPickBindingId == std::numeric_limits<std::uint32_t>::max())
    throw std::length_error("Document switcher picking bindings exhausted");
  const auto id = nextPickBindingId++;
  documentPickBindings.emplace(id, DocumentPickBinding{document, action});
  return id;
}

void DocumentSwitcher::releasePickSnapshots() {
  const auto removed = std::erase_if(pickSnapshots, [&](const auto &snapshot) {
    return snapshot != pickTargets && snapshot.use_count() == 1;
  });
  if (removed == 0) return;
  std::erase_if(documentPickBindings, [&](const auto &binding) {
    return std::ranges::none_of(pickSnapshots, [&](const auto &snapshot) {
      return std::ranges::find(*snapshot, binding.first) != snapshot->end();
    });
  });
}

void DocumentSwitcher::rebuild(FrameContext &ctx,
                               const ui::UiMetrics &nextMetrics) {
  metrics                = nextMetrics;
  theme                  = ctx.theme;
  const auto description = ui::scaledFontDescription(
      fontName, ui::FontRole::Caption, metrics, theme);
  if (!canvas || resolvedFontName != description) {
    canvas = std::make_unique<Canvas>(device, description);
    canvas->createPipeline(*pipeline, false);
    canvas->setIdentity(pickScope, 0);
    resolvedFontName = description;
  }
  canvas->clear();
  auto targets = std::make_shared<std::vector<std::uint32_t>>();
  documents.clear();
  currentTabs.clear();
  for (std::uint32_t index = 0; index < ctx.state.docs.size(); ++index) {
    const auto &doc = ctx.state.docs[index];
    documents.push_back(doc.get());
    if (!doc || doc->isClosing()) continue;
    currentTabs.push_back(
        {.docIndex      = index,
         .name          = formatDocTitle(doc->name(), index),
         .active        = index == activeIndex,
         .sourceName    = doc->name(),
         .selectBinding = bindDocument(doc, PickAction::Select),
         .closeBinding  = bindDocument(doc, PickAction::Close)});
  }
  const auto font    = text::FontManager::instance().getFont(description);
  const auto em      = font->metrics().lineHeight;
  const auto padding = std::max(0.0F, theme.paddingEm * em);
  const auto gap     = std::max(0.0F, theme.gapEm * em);
  const auto touch   = std::max(em, metrics.px(theme.type.minTouchPx));
  const auto safe    = metrics.pixelSafeArea();
  const auto height  = std::min(safe.height, std::max(touch, em + 2 * padding));
  const ui::Rect bar{safe.left, safe.bottom + safe.height - height, safe.width,
                     height};
  const auto actionWidth = std::min(bar.width, touch * 2 + gap);
  const auto available   = std::max(0.0F, bar.width - gap);
  layoutResult           = ui::split(
      bar, {.id = tabsId}, {.id = actionsId},
      {.firstShare = available > 0
                                   ? std::max(0.0F, available - actionWidth) / available
                                   : 0.0F,
                 .gap        = gap,
                 .parentId   = barId});
  layoutResult.boxes.push_back({barId, 0, bar, bar});
  const auto tabsBounds    = layoutResult.find(tabsId)->rect;
  const auto actionsBounds = layoutResult.find(actionsId)->rect;
  std::vector<ui::LayoutItem> items;
  for (const auto &tab : currentTabs) {
    const auto &intrinsic = shaping.fitted(tab.name, font, {});
    items.push_back(
        {.id        = (tab.docIndex << 1U) + 1U,
         .intrinsic = {intrinsic.widthPx + 2 * padding + touch, height},
         .minimum   = {touch * 2, height},
         .maximum   = {std::max(touch, em * 12), height},
         .grow      = 1,
         .focusable = true});
  }
  layoutResult.append(ui::flow(
      tabsBounds, items, {.wrap = false, .gap = gap, .parentId = tabsId}));
  const std::array<ui::LayoutItem, 2> actions{{{.id        = kManagerTag + 1U,
                                                .intrinsic = {touch, height},
                                                .grow      = 1,
                                                .focusable = true},
                                               {.id        = kNewDocTag + 1U,
                                                .intrinsic = {touch, height},
                                                .grow      = 1,
                                                .focusable = true}}};
  layoutResult.append(
      ui::flow(actionsBounds, actions,
               {.wrap = false, .gap = gap, .parentId = actionsId}));
  layoutResult.focusOrder.clear();
  canvas->pushClip(bar);
  const auto surface = ui::rgba(theme.colours.surface);
  const auto text    = ui::rgba(theme.colours.text);
  const auto muted   = ui::rgba(theme.colours.muted);
  const auto accent  = ui::rgba(theme.colours.accent);
  canvas->setTag(render::tagKindOverlay, 0);
  canvas->addRect(bar.left, bar.bottom, bar.width, bar.height, surface);
  for (auto &tab : currentTabs) {
    const auto id = (tab.docIndex << 1U) + 1U;
    auto *box = &*std::ranges::find(layoutResult.boxes, id, &ui::LayoutBox::id);
    const auto rect       = box->rect;
    const auto closeWidth = std::min(touch, rect.width * 0.5F);
    const ui::Rect close{rect.left + rect.width - closeWidth, rect.bottom,
                         closeWidth, rect.height};
    const ui::Rect label{rect.left, rect.bottom,
                         std::max(0.0F, rect.width - closeWidth), rect.height};
    box->contentRect       = textBox(label, padding, em);
    const auto labelBounds = box->contentRect;
    tab.x                  = rect.left;
    tab.y                  = rect.bottom;
    tab.width              = rect.width;
    tab.height             = rect.height;
    targets->push_back(tab.selectBinding);
    canvas->setTag(render::tagKindOverlay,
                   static_cast<std::uint32_t>(targets->size()));
    canvas->addRect(rect.left, rect.bottom, rect.width, rect.height, surface);
    canvas->addText(ctx.state, labelBounds, tab.name, tab.active ? text : muted,
                    surface, {}, &shaping);
    if (tab.active) {
      canvas->pushClip(rect);
      canvas->addLine(rect.left, rect.bottom + rect.height - 1,
                      rect.left + rect.width, rect.bottom + rect.height - 1,
                      metrics.px(2), accent);
      canvas->popClip();
    }
    const auto closeContent = textBox(close, 0, em);
    layoutResult.boxes.push_back({id + 1U, id, close, closeContent, true});
    if (rect.width > 0 && rect.height > 0)
      layoutResult.focusOrder.push_back(id);
    if (close.width > 0 && close.height > 0)
      layoutResult.focusOrder.push_back(id + 1U);
    targets->push_back(tab.closeBinding);
    canvas->setTag(render::tagKindOverlay,
                   static_cast<std::uint32_t>(targets->size()));
    canvas->addRect(close.left, close.bottom, close.width, close.height,
                    surface);
    canvas->addText(ctx.state, closeContent, "×", muted, surface,
                    {.align = TextAlign::Centre}, &shaping);
  }
  for (const auto tag : {kManagerTag, kNewDocTag}) {
    auto *box =
        &*std::ranges::find(layoutResult.boxes, tag + 1U, &ui::LayoutBox::id);
    box->contentRect = textBox(box->rect, 0, em);
    const auto rect  = box->rect;
    if (rect.width > 0 && rect.height > 0)
      layoutResult.focusOrder.push_back(tag + 1U);
    targets->push_back(bindDocument({}, tag == kManagerTag
                                            ? PickAction::Manager
                                            : PickAction::NewDocument));
    canvas->setTag(render::tagKindOverlay,
                   static_cast<std::uint32_t>(targets->size()));
    canvas->addRect(rect.left, rect.bottom, rect.width, rect.height, surface);
    canvas->addText(ctx.state, box->contentRect, tag == kManagerTag ? "=" : "+",
                    text, surface, {.align = TextAlign::Centre}, &shaping);
  }
  canvas->popClip();
  canvas->commit();
  ctx.state.glyphCache.flush();
  knownDocCount = ctx.state.docs.size();
  pickTargets   = std::move(targets);
  pickSnapshots.push_back(pickTargets);
  ++revision;
  builtRevision = revision;
}

void DocumentSwitcher::drawFrame(FrameContext &ctx) {
  const std::scoped_lock lock(guard);
  if (!visible || !device || !pipeline) return;
  if (pickScope == 0)
    pickScope = ctx.state.allocatePersistentOverlayPickScope();
  auto nextMetrics         = ctx.metrics;
  nextMetrics.screenWidth  = ctx.screenWidth;
  nextMetrics.screenHeight = ctx.screenHeight;
  nextMetrics.chrome       = ctx.chrome;
  bool changed             = documents.size() != ctx.state.docs.size();
  std::size_t live{};
  for (std::size_t index = 0; index < ctx.state.docs.size(); ++index) {
    const auto &doc = ctx.state.docs[index];
    changed |= index >= documents.size() || documents[index] != doc.get();
    if (!doc || doc->isClosing()) continue;
    changed |= live >= currentTabs.size() ||
               currentTabs[live].docIndex != index ||
               currentTabs[live].sourceName != doc->name();
    ++live;
  }
  changed |= live != currentTabs.size();
  if (live == 0) {
    if (!layoutResult.boxes.empty()) ++revision;
    layoutResult = {};
    currentTabs.clear();
    pickTargets.reset();
    releasePickSnapshots();
    builtRevision = 0;
    return;
  }
  if (!canvas || changed || builtRevision != revision ||
      metrics != nextMetrics || theme != ctx.theme)
    rebuild(ctx, nextMetrics);
  // Retired bindings live until request snapshots release their target vectors.
  releasePickSnapshots();
  ctx.state.bindOverlayWidgets(
      render::packTagIdentity(render::tagKindOverlay, pickScope, 0),
      pickTargets);
  const auto bar = layoutResult.bounds;
  ctx.chrome.top = std::max(ctx.chrome.top,
                            static_cast<float>(ctx.screenHeight) - bar.bottom);
  const auto projection =
      glm::ortho(0.0F, static_cast<float>(ctx.screenWidth), 0.0F,
                 static_cast<float>(ctx.screenHeight), -1.0F, 1.0F);
  canvas->draw(ctx.state, projection);
}

bool DocumentSwitcher::dispatch(std::uint32_t tag, std::size_t documentCount) {
  std::function<void()> callback;
  {
    const std::scoped_lock lock(guard);
    if (!visible) return false;
    if (tag == kManagerTag)
      callback = managerHandler;
    else if (tag == kNewDocTag)
      callback = newDocHandler;
    else {
      const auto index = tag >> 1U;
      if (tag == kNewDocTag + 1U || index >= documentCount) return false;
      if ((tag & 1U) != 0) {
        if (closeHandler)
          callback = [handler = closeHandler, index] { handler(index); };
      } else {
        activeIndex = index;
        ++revision;
        if (selectHandler)
          callback = [handler = selectHandler, index] { handler(index); };
      }
    }
  }
  if (callback) callback();
  return true;
}
bool DocumentSwitcher::picked(const render::PickingResult &pick,
                              RenderState &state) {
  if (pick.tag.kind != render::tagKindOverlay) return false;
  auto tag = pick.tag.clusterIndex;
  if (pick.requestId != 0) {
    const std::scoped_lock lock(guard);
    if (pickScope == 0 || pick.tag.docIndex != pickScope ||
        pick.tag.pageIndex != 0)
      return false;
    if (!pick.overlayWidgetId) return false;
    const auto binding = documentPickBindings.find(*pick.overlayWidgetId);
    if (binding == documentPickBindings.end()) return false;
    if (binding->second.action == PickAction::Manager)
      tag = kManagerTag;
    else if (binding->second.action == PickAction::NewDocument)
      tag = kNewDocTag;
    else {
      const auto document = binding->second.document.lock();
      if (!document || document->isClosing()) return false;
      const auto current = std::ranges::find(state.docs, document);
      if (current == state.docs.end()) return false;
      tag = (static_cast<std::uint32_t>(current - state.docs.begin()) << 1U) |
            (binding->second.action == PickAction::Close ? 1U : 0U);
    }
  }
  return dispatch(tag, state.docs.size());
}

void DocumentSwitcher::describe(a11y::Builder &into) {
  const std::scoped_lock lock(guard);
  if (!visible || currentTabs.empty()) return;
  std::vector<std::uint64_t> entries;
  const auto add = [&](std::uint32_t id, a11y::Role role,
                       const std::string &label) -> a11y::Node * {
    const auto *box = layoutResult.find(id);
    if (!box || box->rect.width <= 0 || box->rect.height <= 0) return nullptr;
    auto &node     = into.add(id, role);
    node.label     = label;
    node.bounds    = accessibleBounds(box->rect, metrics.screenHeight);
    node.actions   = a11y::bit(a11y::Action::Click);
    node.focusable = true;
    entries.push_back(into.id(id));
    return &node;
  };
  for (const auto &tab : currentTabs) {
    const auto id = (tab.docIndex << 1U) + 1U;
    if (auto *node = add(id, a11y::Role::ListItem, tab.name)) {
      node->toggled     = tab.active;
      node->value       = tab.active ? "selected" : "";
      node->description = tab.sourceName;
    }
    add(id + 1U, a11y::Role::Button, "Close " + tab.name);
  }
  add(kManagerTag + 1U, a11y::Role::Button, "Store Object Manager");
  add(kNewDocTag + 1U, a11y::Role::Button, "New Document");
  auto &bar    = into.add(barId, a11y::Role::List);
  bar.label    = "Open Documents";
  bar.bounds   = accessibleBounds(layoutResult.bounds, metrics.screenHeight);
  bar.children = std::move(entries);
  into.contribute(into.id(barId));
}
bool DocumentSwitcher::performAction(std::uint64_t nodeId, a11y::Action action,
                                     std::string_view) {
  const auto id = a11y::Ids::localOf(nodeId);
  std::size_t count{};
  {
    const std::scoped_lock lock(guard);
    if (!visible || action != a11y::Action::Click || id == 0 || id >= barId)
      return false;
    const auto *box = layoutResult.find(static_cast<std::uint32_t>(id));
    if (!box || !box->focusable || box->rect.width <= 0 ||
        box->rect.height <= 0)
      return false;
    count = knownDocCount;
  }
  return dispatch(static_cast<std::uint32_t>(id - 1U), count);
}
} // namespace gleditor

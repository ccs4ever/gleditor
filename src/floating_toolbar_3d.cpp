/**
 * @file floating_toolbar_3d.cpp
 * @brief Retained screen-space word-processing controls.
 */
#include <gleditor/floating_toolbar_3d.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

#include <glm/ext/matrix_clip_space.hpp>

#include <gleditor/doc.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/text/font.hpp>

namespace gleditor {
namespace {
constexpr std::uint32_t toolbarNode = 0x5000U;
bool knownButton(FloatingToolbar3D::ButtonId id) {
  using Id = FloatingToolbar3D::ButtonId;
  switch (id) {
  case Id::NewDoc:
  case Id::OpenFile:
  case Id::SaveDoc:
  case Id::CloseDoc:
  case Id::OverviewTray:
  case Id::Bold:
  case Id::Italic:
  case Id::Underline:
  case Id::Strikethrough:
  case Id::Heading1:
  case Id::Heading2:
  case Id::FontDec:
  case Id::FontInc:
  case Id::AlignLeft:
  case Id::AlignCenter:
  case Id::AlignRight:
  case Id::ListBullet:
  case Id::ListNumbered:
  case Id::CodeBlock:
    return true;
  default:
    return false;
  }
}
} // namespace
FloatingToolbar3D::FloatingToolbar3D(std::string aFontName)
    : fontName(std::move(aFontName)) {}
FloatingToolbar3D::~FloatingToolbar3D() = default;
void FloatingToolbar3D::deviceReady(
    render::RenderDevice &aDevice,
    const render::PipelineDesc &documentPipeline) {
  const std::scoped_lock lock(guard);
  device   = &aDevice;
  pipeline = documentPipeline;
  canvas.reset();
  drawnRevision = 0;
  pickScope     = 0;
}

std::shared_ptr<const ui::LayoutResult>
FloatingToolbar3D::prepareLayout(const ui::UiMetrics &aMetrics,
                                 const ui::Theme &aTheme) {
  const std::scoped_lock lock(guard);
  return prepareLayoutLocked(aMetrics, aTheme);
}

std::shared_ptr<const ui::LayoutResult>
FloatingToolbar3D::prepareLayoutLocked(const ui::UiMetrics &aMetrics,
                                       const ui::Theme &aTheme) {
  if (layout && preparedRevision == revision && metrics == aMetrics &&
      theme == aTheme)
    return layout;
  metrics = aMetrics;
  theme   = aTheme;
  resolvedFont =
      ui::scaledFontDescription(fontName, ui::FontRole::Label, metrics, theme);
  font = text::FontManager::instance().getFont(resolvedFont);
  // Define button items
  struct ButtonDef {
    ButtonId id;
    std::string label;
    std::string tooltip;
    bool active;
    bool separatorAfter;
  };

  const std::vector<ButtonDef> definitions = {
      // Operational
      {.id             = ButtonId::NewDoc,
       .label          = "+ New",
       .tooltip        = "New Document (Ctrl+N)",
       .active         = false,
       .separatorAfter = false},
      {.id             = ButtonId::OpenFile,
       .label          = "Open",
       .tooltip        = "Open File (Ctrl+O)",
       .active         = false,
       .separatorAfter = false},
      {.id             = ButtonId::SaveDoc,
       .label          = "Save",
       .tooltip        = "Save Document (Ctrl+S)",
       .active         = false,
       .separatorAfter = false},
      {.id             = ButtonId::CloseDoc,
       .label          = "Close",
       .tooltip        = "Close Document (Ctrl+W)",
       .active         = false,
       .separatorAfter = false},
      {.id             = ButtonId::OverviewTray,
       .label          = "3D View",
       .tooltip        = "Toggle 3D Carousel (F10)",
       .active         = false,
       .separatorAfter = true},

      // Formatting
      {.id             = ButtonId::Bold,
       .label          = "B",
       .tooltip        = "Bold (Ctrl+B)",
       .active         = isBold,
       .separatorAfter = false},
      {.id             = ButtonId::Italic,
       .label          = "I",
       .tooltip        = "Italic (Ctrl+I)",
       .active         = isItalic,
       .separatorAfter = false},
      {.id             = ButtonId::Underline,
       .label          = "U",
       .tooltip        = "Underline (Ctrl+U)",
       .active         = isUnderline,
       .separatorAfter = false},
      {.id             = ButtonId::Strikethrough,
       .label          = "S",
       .tooltip        = "Strikethrough (Ctrl+Shift+X)",
       .active         = isStrike,
       .separatorAfter = true},

      // Headings & Scale
      {.id             = ButtonId::Heading1,
       .label          = "H1",
       .tooltip        = "Heading 1",
       .active         = headingLevel == 1,
       .separatorAfter = false},
      {.id             = ButtonId::Heading2,
       .label          = "H2",
       .tooltip        = "Heading 2",
       .active         = headingLevel == 2,
       .separatorAfter = false},
      {.id             = ButtonId::FontDec,
       .label          = "A-",
       .tooltip        = "Decrease Font Size (Ctrl+-)",
       .active         = false,
       .separatorAfter = false},
      {.id             = ButtonId::FontInc,
       .label          = "A+",
       .tooltip        = "Increase Font Size (Ctrl+=)",
       .active         = false,
       .separatorAfter = true},

      // Alignment
      {.id             = ButtonId::AlignLeft,
       .label          = "Left",
       .tooltip        = "Align Left (Ctrl+L)",
       .active         = false,
       .separatorAfter = false},
      {.id             = ButtonId::AlignCenter,
       .label          = "Center",
       .tooltip        = "Align Center (Ctrl+E)",
       .active         = false,
       .separatorAfter = false},
      {.id             = ButtonId::AlignRight,
       .label          = "Right",
       .tooltip        = "Align Right (Ctrl+R)",
       .active         = false,
       .separatorAfter = true},

      // Structured Blocks
      {.id             = ButtonId::ListBullet,
       .label          = "* List",
       .tooltip        = "Bullet List (Ctrl+Shift+8)",
       .active         = false,
       .separatorAfter = false},
      {.id             = ButtonId::ListNumbered,
       .label          = "1. List",
       .tooltip        = "Numbered List (Ctrl+Shift+7)",
       .active         = false,
       .separatorAfter = false},
      {.id             = ButtonId::CodeBlock,
       .label          = "</>",
       .tooltip        = "Code Block (Ctrl+Alt+C)",
       .active         = false,
       .separatorAfter = false},
  };

  const auto safe    = metrics.pixelSafeArea();
  const auto line    = font->metrics().lineHeight;
  const auto padding = line * theme.paddingEm;
  const auto gap     = line * theme.gapEm;
  const auto touch =
      std::max(theme.type.minTouchPx, metrics.px(theme.type.minTouchPx));
  std::vector<ui::LayoutItem> items;
  float intrinsicWidth = padding * 2;
  for (const auto &def : definitions) {
    const auto &label = shaping.fitted(def.label, font, {});
    // Rounded box edges can lose a pixel at either side of fractional padding.
    const ui::Size size{
        std::ceil(std::max(touch, label.widthPx + padding * 2)) + 2,
        std::ceil(std::max(touch, line + padding * 2)) + 2};
    items.push_back({.id        = static_cast<std::uint32_t>(def.id),
                     .intrinsic = size,
                     .focusable = true,
                     .paddingPx = padding});
    intrinsicWidth += size.width + gap;
  }
  const auto barWidth = std::min(intrinsicWidth, safe.width);
  const auto outerPadding =
      std::min({padding, safe.width * .1F, safe.height * .1F});
  const ui::Rect initial{safe.left + (safe.width - barWidth) * .5F, safe.bottom,
                         barWidth, safe.height};
  const auto inset = [&](ui::Rect rect) {
    const auto horizontal = std::min(outerPadding, rect.width * .5F);
    const auto vertical   = std::min(outerPadding, rect.height * .5F);
    return ui::Rect{rect.left + horizontal, rect.bottom + vertical,
                    std::max(0.0F, rect.width - horizontal * 2),
                    std::max(0.0F, rect.height - vertical * 2)};
  };
  const auto available = inset(initial);
  std::size_t rows     = 1;
  float rowWidth       = 0;
  for (const auto &item : items) {
    const auto width = std::min(item.intrinsic.width, available.width);
    if (rowWidth > 0 && rowWidth + gap + width > available.width) {
      ++rows;
      rowWidth = 0;
    }
    rowWidth += (rowWidth > 0 ? gap : 0) + width;
  }
  const auto lineGap =
      std::min(gap, available.height / (2 * static_cast<float>(rows)));
  const auto rowBudget = std::max(
      0.0F, (available.height - lineGap * static_cast<float>(rows - 1)) /
                static_cast<float>(rows));
  // Preserve every action when a large type scale exhausts the viewport;
  // padding gives way before the label's own line height.
  for (auto &item : items) {
    item.intrinsic.height = std::min(item.intrinsic.height, rowBudget);
    item.paddingPx        = std::min(
        padding, std::max(0.0F, (item.intrinsic.height - line - 2) * .5F));
  }
  const auto measured =
      ui::flow(inset(initial), items,
               {.gap = gap, .lineGap = lineGap, .parentId = toolbarNode});
  auto bottom = initial.bottom + initial.height - outerPadding;
  for (const auto &box : measured.boxes)
    bottom = std::min(bottom, box.rect.bottom);
  const auto height =
      std::min(safe.height, std::max(0.0F, initial.bottom + initial.height -
                                               bottom + outerPadding));
  const auto bar = ui::clampToSafeArea(
      {initial.left, safe.bottom + safe.height - height, barWidth, height},
      safe);
  auto next = std::make_shared<ui::LayoutResult>(
      ui::flow(inset(bar), items,
               {.gap = gap, .lineGap = lineGap, .parentId = toolbarNode}));
  next->bounds = bar;
  next->boxes.insert(next->boxes.begin(), {toolbarNode, 0, bar, inset(bar)});
  currentButtons.clear();
  for (const auto &def : definitions) {
    const auto *box = next->find(static_cast<std::uint32_t>(def.id));
    if (!box) continue;
    ButtonLayout button{.id          = def.id,
                        .label       = def.label,
                        .tooltip     = def.tooltip,
                        .x           = box->rect.left,
                        .y           = box->rect.bottom,
                        .width       = box->rect.width,
                        .height      = box->rect.height,
                        .active      = def.active,
                        .isSeparator = def.separatorAfter};
    if (box->contentRect.width > 0 && box->contentRect.height > 0)
      button.fitted = shaping.fitted(def.label, font,
                                     {.maxWidthPx  = box->contentRect.width,
                                      .maxHeightPx = box->contentRect.height,
                                      .align       = TextAlign::Centre});
    currentButtons.push_back(std::move(button));
  }
  layout = std::move(next);
  ++revision;
  preparedRevision = revision;
  return layout;
}

void FloatingToolbar3D::clearPresentationLocked() {
  if (presentationAvailable || layout) {
    ++revision;
    ++pickContextRevision;
  }
  presentationAvailable = false;
  presentationDocument.reset();
  layout.reset();
  currentButtons.clear();
  pickingTargets.reset();
  pickingButtons.clear();
  preparedRevision = drawnRevision = 0;
}

void FloatingToolbar3D::rebuildPickingTargetsLocked() {
  if (pickingTargets && builtPickContext == pickContextRevision) return;
  auto targets = std::make_shared<std::vector<std::uint32_t>>(
      static_cast<std::uint32_t>(ButtonId::CodeBlock), 0);
  pickingButtons.clear();
  // A button's readback must retain the active-document context it was drawn
  // for, rather than acquire another document after a delayed result arrives.
  for (const auto &button : currentButtons) {
    if (nextPickingTarget == std::numeric_limits<std::uint32_t>::max())
      throw std::length_error("Toolbar picking identities exhausted");
    const auto token                                      = ++nextPickingTarget;
    (*targets)[static_cast<std::uint32_t>(button.id) - 1] = token;
    pickingButtons.emplace(token, button.id);
  }
  pickingTargets   = std::move(targets);
  builtPickContext = pickContextRevision;
}

void FloatingToolbar3D::drawFrame(FrameContext &ctx) {
  const std::scoped_lock lock(guard);
  if (!visible) return;
  if (ctx.state.docs.empty()) {
    clearPresentationLocked();
    return;
  }
  if (activeDoc >= ctx.state.docs.size()) {
    activeDoc = 0;
    ++pickContextRevision;
    ++revision;
  }
  const auto &doc = ctx.state.docs[activeDoc];
  if (!doc || doc->isClosing()) {
    clearPresentationLocked();
    return;
  }
  if (!device || !pipeline) return;
  if (!presentationAvailable || presentationDocument.lock() != doc) {
    presentationDocument  = doc;
    presentationAvailable = true;
    ++pickContextRevision;
    ++revision;
  }
  auto aMetrics         = ctx.metrics;
  aMetrics.screenWidth  = ctx.screenWidth;
  aMetrics.screenHeight = ctx.screenHeight;
  static_cast<void>(prepareLayoutLocked(aMetrics, ctx.theme));
  rebuildPickingTargetsLocked();
  if (pickScope == 0)
    pickScope = ctx.state.allocatePersistentOverlayPickScope();
  ctx.state.bindOverlayWidgets(
      render::packTagIdentity(render::tagKindOverlay, pickScope, 0),
      pickingTargets);
  if (!canvas || canvasFont != resolvedFont) {
    canvas = std::make_unique<Canvas>(device, resolvedFont);
    canvas->createPipeline(*pipeline, false);
    canvas->setIdentity(pickScope, 0);
    canvasFont    = resolvedFont;
    drawnRevision = 0;
  }
  if (drawnRevision != preparedRevision) {
    canvas->clear();
    canvas->pushClip(layout->bounds);
    const auto surface = ui::rgba(theme.colours.surface);
    const auto accent  = ui::rgba(theme.colours.accent);
    const auto border  = ui::rgba(theme.colours.border);
    const auto &bar    = layout->bounds;
    canvas->setTag(render::tagKindOverlay, 0);
    canvas->addRect(bar.left, bar.bottom, bar.width, bar.height, surface);
    const auto stroke = std::max(1.0F, metrics.px(1));
    canvas->addRect(bar.left, bar.bottom + bar.height - stroke, bar.width,
                    stroke, accent);
    for (const auto &button : currentButtons) {
      const auto *box = layout->find(static_cast<std::uint32_t>(button.id));
      if (!box || box->rect.width <= 0 || box->rect.height <= 0) continue;
      canvas->setTag(render::tagKindOverlay,
                     static_cast<std::uint32_t>(button.id));
      const auto fill = button.active ? accent : surface;
      canvas->addRect(box->rect.left, box->rect.bottom, box->rect.width,
                      box->rect.height, fill);
      if (button.active)
        canvas->addRect(box->rect.left, box->rect.bottom, box->rect.width,
                        stroke, border);
      auto textBox = box->contentRect;
      textBox.bottom += (textBox.height - button.fitted.heightPx) * .5F;
      textBox.height = button.fitted.heightPx;
      canvas->addText(ctx.state, textBox, button.fitted,
                      ui::rgba(theme.colours.text), fill);
      if (button.isSeparator)
        canvas->addRect(box->rect.left + box->rect.width - stroke,
                        box->rect.bottom, stroke, box->rect.height, border);
    }
    canvas->popClip();
    ctx.state.glyphCache.flush();
    canvas->commit();
    drawnRevision = preparedRevision;
  }
  const auto projection =
      glm::ortho(0.0F, static_cast<float>(ctx.screenWidth), 0.0F,
                 static_cast<float>(ctx.screenHeight), -1.0F, 1.0F);
  canvas->draw(ctx.state, projection, 0.96F);
}

bool FloatingToolbar3D::picked(const render::PickingResult &pick,
                               RenderState &state) {
  std::function<void(ButtonId, std::uint32_t)> handler;
  ButtonId id{};
  std::uint32_t document{};
  {
    const std::scoped_lock lock(guard);
    if (!visible || !presentationAvailable ||
        pick.tag.kind != render::tagKindOverlay)
      return false;
    if (pick.requestId != 0 &&
        (pickScope == 0 || pick.tag.docIndex != pickScope ||
         pick.tag.pageIndex != 0))
      return false;
    id = static_cast<ButtonId>(pick.tag.clusterIndex);
    if (pick.requestId != 0) {
      if (!pick.overlayWidgetId || builtPickContext != pickContextRevision)
        return true;
      const auto found            = pickingButtons.find(*pick.overlayWidgetId);
      const auto capturedDocument = presentationDocument.lock();
      if (found == pickingButtons.end() || found->second != id ||
          !capturedDocument || capturedDocument->isClosing() ||
          activeDoc >= state.docs.size() ||
          state.docs[activeDoc] != capturedDocument)
        return true;
    }
    if (!knownButton(id)) return false;
    if (layout) {
      const auto *box = layout->find(pick.tag.clusterIndex);
      if (!box || box->rect.width <= 0 || box->rect.height <= 0) return false;
    }
    handler  = actionHandler;
    document = activeDoc;
  }
  if (handler) handler(id, document);
  return true;
}

void FloatingToolbar3D::describe(a11y::Builder &into) {
  const std::scoped_lock lock(guard);
  if (!visible || !presentationAvailable || !layout || currentButtons.empty())
    return;
  const auto bounds = [&](const ui::Rect &rect) {
    return a11y::Rect{
        rect.left, metrics.screenHeight - rect.bottom - rect.height,
        rect.left + rect.width, metrics.screenHeight - rect.bottom};
  };
  std::vector<std::uint64_t> children;
  for (const auto &button : currentButtons) {
    const auto *box = layout->find(static_cast<std::uint32_t>(button.id));
    if (!box || box->rect.width <= 0 || box->rect.height <= 0) continue;
    const auto id = toolbarNode + static_cast<std::uint32_t>(button.id);
    auto &node    = into.add(id, a11y::Role::Button);
    node.label    = button.tooltip;
    node.toggled  = button.active;
    node.bounds   = bounds(box->rect);
    node.actions  = a11y::bit(a11y::Action::Click);
    children.push_back(into.id(id));
  }
  auto &bar    = into.add(toolbarNode, a11y::Role::Group);
  bar.label    = "3D Word Processing Controls";
  bar.bounds   = bounds(layout->bounds);
  bar.children = std::move(children);
  into.contribute(into.id(toolbarNode));
}

bool FloatingToolbar3D::performAction(std::uint64_t nodeId, a11y::Action action,
                                      std::string_view) {
  std::function<void(ButtonId, std::uint32_t)> handler;
  ButtonId id{};
  std::uint32_t document{};
  {
    const std::scoped_lock lock(guard);
    nodeId = a11y::Ids::localOf(nodeId);
    if (!visible || !presentationAvailable || nodeId < toolbarNode ||
        nodeId - toolbarNode > std::numeric_limits<std::uint32_t>::max())
      return false;
    id = static_cast<ButtonId>(nodeId - toolbarNode);
    if (!knownButton(id) || action != a11y::Action::Click) return false;
    if (layout) {
      const auto *box = layout->find(static_cast<std::uint32_t>(id));
      if (!box || box->rect.width <= 0 || box->rect.height <= 0) return false;
    }
    if (!actionHandler) return false;
    handler  = actionHandler;
    document = activeDoc;
  }
  handler(id, document);
  return true;
}
} // namespace gleditor

/**
 * @file doc_switcher.cpp
 * @brief Implementation of the document switcher top tab bar.
 */
#include <gleditor/doc_switcher.hpp>

#include <algorithm>
#include <filesystem>
#include <utility>

#include <glm/ext/matrix_clip_space.hpp>

#include <gleditor/canvas.hpp>
#include <gleditor/caret.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render_state.hpp>

namespace gleditor {

namespace {

constexpr float barHeight    = 32.0F;
constexpr float tabPaddingX  = 12.0F;
constexpr float closeButtonW = 20.0F;
constexpr float minTabWidth  = 90.0F;
constexpr float maxTabWidth  = 220.0F;

constexpr std::uint32_t barBackground = 0x14171DFAU; // Dark translucent
constexpr std::uint32_t barBorder     = 0x282D38FFU; // Bottom edge border
constexpr std::uint32_t tabActiveBg   = 0x2A3242FFU; // Active tab background
constexpr std::uint32_t tabInactiveBg = 0x1A1E26D0U; // Inactive tab background
constexpr std::uint32_t tabActiveBorder =
    0x5C8DFFFFU; // Active tab highlight line
constexpr std::uint32_t tabTextActive   = 0xFFFFFFFFU; // White text
constexpr std::uint32_t tabTextInactive = 0x9AA3B2FFU; // Muted text
constexpr std::uint32_t closeTextColour = 0x7E889BFFU; // Close button text

std::string formatDocTitle(const std::string &rawName,
                           const std::size_t index) {
  if (rawName.empty()) {
    return "Doc " + std::to_string(index + 1);
  }
  std::filesystem::path p(rawName);
  return p.filename().string();
}

} // namespace

DocumentSwitcher::DocumentSwitcher(std::string aFontName)
    : fontName(std::move(aFontName)) {}

DocumentSwitcher::~DocumentSwitcher() = default;

void DocumentSwitcher::deviceReady(
    render::RenderDevice &device,
    const render::PipelineDesc &documentPipeline) {
  canvas = std::make_unique<Canvas>(&device, fontName);
  canvas->createPipeline(documentPipeline, false);
}

void DocumentSwitcher::drawFrame(FrameContext &ctx) {
  if (!visible || nullptr == canvas || ctx.state.docs.empty()) {
    currentTabs.clear();
    return;
  }

  // Count active non-closing documents
  std::size_t activeCount = 0;
  for (const auto &doc : ctx.state.docs) {
    if (doc && !doc->isClosing()) {
      activeCount++;
    }
  }
  if (0 == activeCount) {
    currentTabs.clear();
    return;
  }

  const auto width  = static_cast<float>(ctx.screenWidth);
  const auto height = static_cast<float>(ctx.screenHeight);
  const auto ortho  = glm::ortho(0.0F, width, 0.0F, height, -1.0F, 1.0F);

  canvas->clear();
  currentTabs.clear();

  // Background bar across the top of the viewport
  const float barY = height - barHeight;
  canvas->addRect(0.0F, barY, width, barHeight, barBackground);
  canvas->addLine(0.0F, barY, width, barY, 1.0F, barBorder);

  float curX = 4.0F;

  for (std::uint32_t i = 0; i < ctx.state.docs.size(); ++i) {
    const auto &doc = ctx.state.docs[i];
    if (!doc || doc->isClosing()) {
      continue;
    }

    const std::string title = formatDocTitle(doc->name(), i);
    const auto textMetrics  = canvas->measureText(title);

    const float tabW =
        std::clamp(textMetrics.width + (2.0F * tabPaddingX) + closeButtonW,
                   minTabWidth, maxTabWidth);

    if (curX + tabW > width) {
      break; // Avoid overflowing the screen width
    }

    const bool isActive = (i == activeIndex);
    const float tabH    = barHeight - 2.0F;
    const float tabY    = barY + 2.0F;
    const auto tabBg    = isActive ? tabActiveBg : tabInactiveBg;

    TabInfo info;
    info.docIndex = i;
    info.name     = title;
    info.x        = curX;
    info.y        = tabY;
    info.width    = tabW;
    info.height   = tabH;
    info.active   = isActive;
    currentTabs.push_back(info);

    // Tab body: cluster tag has bit 0 = 0 (select)
    canvas->setTag(render::tagKindOverlay, (i << 1U) | 0U);
    canvas->addRect(curX, tabY, tabW, tabH, tabBg);

    if (isActive) {
      // Top accent line for active tab
      canvas->addLine(curX, height - 1.0F, curX + tabW, height - 1.0F, 2.0F,
                      tabActiveBorder);
    }

    // Tab label text
    canvas->setTextWidthLimit(
        static_cast<int>(tabW - closeButtonW - (2.0F * tabPaddingX)));
    canvas->addText(ctx.state, curX + tabPaddingX, height - 8.0F, title,
                    isActive ? tabTextActive : tabTextInactive, tabBg);

    // Close button [×]: cluster tag has bit 0 = 1 (close)
    const float closeX = curX + tabW - closeButtonW;
    canvas->setTag(render::tagKindOverlay, (i << 1U) | 1U);
    canvas->addText(ctx.state, closeX, height - 7.0F, "×", closeTextColour,
                    tabBg);

    curX += tabW + 2.0F;
  }

  canvas->commit();
  canvas->draw(ctx.state, ortho);
}

bool DocumentSwitcher::picked(const render::PickingResult &pick,
                              RenderState &state) {
  if (!visible || pick.tag.kind != render::tagKindOverlay) {
    return false;
  }

  const auto rawTag   = pick.tag.clusterIndex;
  const auto docIndex = rawTag >> 1U;
  const bool isClose  = (rawTag & 1U) != 0U;

  if (docIndex >= state.docs.size()) {
    return false;
  }

  if (isClose) {
    if (closeHandler) {
      closeHandler(docIndex);
    }
    return true;
  }

  // Select tab
  activeIndex = docIndex;
  revision++;
  if (selectHandler) {
    selectHandler(docIndex);
  }
  return true;
}

void DocumentSwitcher::describe(a11y::Builder &into) {
  if (!visible || currentTabs.empty()) {
    return;
  }
  constexpr std::uint64_t barId = 1;
  auto &bar                     = into.add(barId, a11y::Role::List);
  bar.label                     = "Open Documents";

  for (std::size_t i = 0; i < currentTabs.size(); ++i) {
    const auto &tab      = currentTabs[i];
    const auto tabNodeId = 100U + i;
    auto &node           = into.add(tabNodeId, a11y::Role::ListItem);
    node.label           = tab.name;
    node.value           = tab.active ? "selected" : "";
    node.toggled         = tab.active;
    node.actions         = a11y::bit(a11y::Action::Click);
    bar.children.push_back(into.id(tabNodeId));
  }
  into.contribute(into.id(barId));
}

} // namespace gleditor

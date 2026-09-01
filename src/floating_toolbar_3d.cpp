/**
 * @file floating_toolbar_3d.cpp
 * @brief Implementation of the floating 3D word-processing and operational HUD.
 */
#include <gleditor/floating_toolbar_3d.hpp>

#include <algorithm>
#include <format>
#include <utility>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <gleditor/canvas.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render_state.hpp>

namespace gleditor {

namespace {

constexpr float toolbarHeight   = 36.0F;
constexpr float btnPaddingX     = 7.0F;
constexpr float btnHeight       = 26.0F;
constexpr float separatorWidth  = 1.0F;
constexpr float separatorMargin = 6.0F;

// Color palette
constexpr std::uint32_t hudBackground = 0x121722F0U; // Dark frosted slate
constexpr std::uint32_t hudBorder     = 0x2A3448FFU; // Slate border
constexpr std::uint32_t hudTopAccent  = 0x06B6D4FFU; // Cyan accent
constexpr std::uint32_t btnDefaultBg  = 0x1B2332E0U; // Button default background
constexpr std::uint32_t btnActiveBg   = 0x0E4656F0U; // Active toggled button
constexpr std::uint32_t btnActiveBorder = 0x06B6D4FFU;
constexpr std::uint32_t textPrimary     = 0xF1F5F9FFU;
constexpr std::uint32_t textActive      = 0x38BDF8FFU;
constexpr std::uint32_t separatorColour = 0x334155AAU;

} // namespace

FloatingToolbar3D::FloatingToolbar3D(std::string aFontName)
    : fontName(std::move(aFontName)) {}

FloatingToolbar3D::~FloatingToolbar3D() = default;

void FloatingToolbar3D::deviceReady(
    render::RenderDevice &device,
    const render::PipelineDesc &documentPipeline) {
  canvas = std::make_unique<Canvas>(&device, fontName);
  canvas->createPipeline(documentPipeline, false);
}

void FloatingToolbar3D::drawFrame(FrameContext &ctx) {
  if (!visible || nullptr == canvas || ctx.state.docs.empty()) {
    currentButtons.clear();
    return;
  }

  if (activeDoc >= ctx.state.docs.size()) {
    activeDoc = 0;
  }
  const auto &doc = ctx.state.docs[activeDoc];
  if (!doc || doc->isClosing()) {
    currentButtons.clear();
    return;
  }

  const auto screenW = static_cast<float>(ctx.screenWidth);
  const auto screenH = static_cast<float>(ctx.screenHeight);
  const auto ortho   = glm::ortho(0.0F, screenW, 0.0F, screenH, -1.0F, 1.0F);

  canvas->clear();
  currentButtons.clear();

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
      {ButtonId::NewDoc, "+ New", "New Document (Ctrl+N)", false, false},
      {ButtonId::OpenFile, "Open", "Open File (Ctrl+O)", false, false},
      {ButtonId::SaveDoc, "Save", "Save Document (Ctrl+S)", false, false},
      {ButtonId::CloseDoc, "Close", "Close Document (Ctrl+W)", false, false},
      {ButtonId::OverviewTray, "3D View", "Toggle 3D Carousel (F10)", false,
       true},

      // Formatting
      {ButtonId::Bold, "B", "Bold (Ctrl+B)", isBold, false},
      {ButtonId::Italic, "I", "Italic (Ctrl+I)", isItalic, false},
      {ButtonId::Underline, "U", "Underline (Ctrl+U)", isUnderline, false},
      {ButtonId::Strikethrough, "S", "Strikethrough (Ctrl+Shift+X)", isStrike,
       true},

      // Headings & Scale
      {ButtonId::Heading1, "H1", "Heading 1", headingLevel == 1, false},
      {ButtonId::Heading2, "H2", "Heading 2", headingLevel == 2, false},
      {ButtonId::FontDec, "A-", "Decrease Font Size (Ctrl+-)", false, false},
      {ButtonId::FontInc, "A+", "Increase Font Size (Ctrl+=)", false, true},

      // Alignment
      {ButtonId::AlignLeft, "Left", "Align Left (Ctrl+L)", false, false},
      {ButtonId::AlignCenter, "Center", "Align Center (Ctrl+E)", false, false},
      {ButtonId::AlignRight, "Right", "Align Right (Ctrl+R)", false, true},

      // Structured Blocks
      {ButtonId::ListBullet, "* List", "Bullet List (Ctrl+Shift+8)", false,
       false},
      {ButtonId::ListNumbered, "1. List", "Numbered List (Ctrl+Shift+7)", false,
       false},
      {ButtonId::CodeBlock, "</>", "Code Block (Ctrl+Alt+C)", false, false},
  };

  // Measure total width
  float totalWidth = 16.0F; // Margins
  for (const auto &def : definitions) {
    const auto metrics = canvas->measureText(def.label);
    const float btnW   = std::max(24.0F, metrics.width + btnPaddingX * 2.0F);
    totalWidth += btnW + 4.0F;
    if (def.separatorAfter) {
      totalWidth += separatorMargin * 2.0F + separatorWidth;
    }
  }

  // Anchor toolbar horizontally centered, floating above the top margin of the viewport
  const float barY = screenH - toolbarHeight - 42.0F;
  const float barX = std::max(16.0F, (screenW - totalWidth) * 0.5F);

  // Background Frosted Plate
  canvas->addRect(barX, barY, totalWidth, toolbarHeight, hudBackground);
  // Top luminous accent line
  canvas->addLine(barX, barY + toolbarHeight - 1.0F, barX + totalWidth,
                  barY + toolbarHeight - 1.0F, 1.5F, hudTopAccent);
  // Bottom & side border
  canvas->addLine(barX, barY, barX + totalWidth, barY, 1.0F, hudBorder);
  canvas->addLine(barX, barY, barX, barY + toolbarHeight, 1.0F, hudBorder);
  canvas->addLine(barX + totalWidth, barY, barX + totalWidth,
                  barY + toolbarHeight, 1.0F, hudBorder);

  float curX = barX + 8.0F;
  const float curY =
      barY + (toolbarHeight - btnHeight) * 0.5F; // Center vertically

  for (const auto &def : definitions) {
    const auto metrics = canvas->measureText(def.label);
    const float btnW   = std::max(24.0F, metrics.width + btnPaddingX * 2.0F);

    const std::uint32_t bgCol = def.active ? btnActiveBg : btnDefaultBg;
    const std::uint32_t txtCol =
        def.active ? textActive : (def.label == "+ New" ? 0x38BDF8FFU : textPrimary);

    canvas->setTag(render::tagKindOverlay,
                   static_cast<std::uint32_t>(def.id));
    canvas->addRect(curX, curY, btnW, btnHeight, bgCol);

    if (def.active) {
      canvas->addLine(curX, curY, curX + btnW, curY, 1.5F, btnActiveBorder);
      canvas->addLine(curX, curY + btnHeight, curX + btnW, curY + btnHeight,
                      1.0F, btnActiveBorder);
    }

    // Centered label text
    const float textX = curX + (btnW - metrics.width) * 0.5F;
    const float textY = curY + (btnHeight + metrics.height) * 0.5F - 2.0F;
    canvas->addText(ctx.state, textX, textY, def.label, txtCol, bgCol);

    currentButtons.push_back(ButtonLayout{
        .id             = def.id,
        .label          = def.label,
        .tooltip        = def.tooltip,
        .x              = curX,
        .y              = curY,
        .width          = btnW,
        .height         = btnHeight,
        .active         = def.active,
        .isSeparator    = false,
    });

    curX += btnW + 4.0F;

    if (def.separatorAfter) {
      curX += separatorMargin;
      canvas->addLine(curX, curY + 2.0F, curX, curY + btnHeight - 2.0F,
                      separatorWidth, separatorColour);
      curX += separatorWidth + separatorMargin;
    }
  }

  canvas->commit();
  canvas->draw(ctx.state, ortho, 0.96F);
}

bool FloatingToolbar3D::picked(const render::PickingResult &pick,
                               RenderState &) {
  if (pick.tag.kind == render::tagKindOverlay && pick.tag.clusterIndex >= 100) {
    const auto btnId = static_cast<ButtonId>(pick.tag.clusterIndex);
    if (actionHandler) {
      actionHandler(btnId, activeDoc);
    }
    return true; // Consume pick to prevent caret relocation
  }
  return false;
}

void FloatingToolbar3D::describe(a11y::Builder &into) {
  if (!visible || currentButtons.empty()) {
    return;
  }
  constexpr std::uint64_t barId = 0x5000;
  auto &bar                     = into.add(barId, a11y::Role::Group);
  bar.label                     = "3D Word Processing Controls";

  for (const auto &btn : currentButtons) {
    const auto btnNodeId = 0x5000U + static_cast<std::uint64_t>(btn.id);
    auto &node           = into.add(btnNodeId, a11y::Role::Button);
    node.label           = btn.tooltip;
    node.toggled         = btn.active;
    node.actions         = a11y::bit(a11y::Action::Click);
    bar.children.push_back(into.id(btnNodeId));
  }

  into.contribute(into.id(barId));
}

bool FloatingToolbar3D::performAction(const std::uint64_t nodeId,
                                      const a11y::Action action,
                                      const std::string_view) {
  if (action == a11y::Action::Click || action == a11y::Action::Focus) {
    if (nodeId >= 0x5000) {
      const auto btnId = static_cast<ButtonId>(nodeId - 0x5000);
      if (actionHandler) {
        actionHandler(btnId, activeDoc);
        return true;
      }
    }
  }
  return false;
}

} // namespace gleditor

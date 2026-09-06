/**
 * @file radial_menu.cpp
 * @brief Implementation of the 3D Radial Marking Menu overlay.
 */
#include <gleditor/radial_menu.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <utility>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <gleditor/canvas.hpp>
#include <gleditor/caret.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render_state.hpp>

namespace gleditor {

namespace {

constexpr std::uint32_t colRadialBg     = 0x0F172AF0U; // Frosted dark plate
constexpr std::uint32_t colRadialBorder = 0x334155FFU; // Slate border
constexpr std::uint32_t colRadialAccent = 0x38BDF8FFU; // Sky cyan
constexpr std::uint32_t colSpoke        = 0x1E293B88U; // Subtle spoke divider
constexpr std::uint32_t colPodBg        = 0x1E293BE0U; // Pod background
constexpr std::uint32_t colPodActiveBg  = 0x0C4A6EE8U; // Active pod fill
constexpr std::uint32_t colPodBorder    = 0x475569FFU; // Pod border
constexpr std::uint32_t colPodText      = 0xF8FAFCFFU; // Bright glyph text
constexpr std::uint32_t colHubBg        = 0x0284C7E8U; // Hub plate fill
constexpr std::uint32_t colHubText      = 0xFFFFFFFFU; // Hub text

constexpr float kPodWidth  = 36.0F;
constexpr float kPodHeight = 30.0F;
constexpr float kHubSize   = 44.0F;

} // namespace

RadialConfig RadialConfig::createDefault() {
  RadialConfig cfg;
  cfg.radius      = 84.0F;
  cfg.innerRadius = 26.0F;

  RadialAction bold;
  bold.id     = "bold";
  bold.label  = "B";
  bold.desc   = "Bold (Format Link)";
  bold.action = "format:bold";

  RadialAction italic;
  italic.id     = "italic";
  italic.label  = "I";
  italic.desc   = "Italic (Format Link)";
  italic.action = "format:italic";

  RadialAction underline;
  underline.id     = "underline";
  underline.label  = "U";
  underline.desc   = "Underline (Format Link)";
  underline.action = "format:underline";

  RadialAction superAction;
  superAction.id     = "superscript";
  superAction.label  = "X²";
  superAction.desc   = "Superscript (Format Link)";
  superAction.action = "format:superscript";
  superAction.icon   = "X²";

  RadialAction subAction;
  subAction.id     = "subscript";
  subAction.label  = "X₂";
  subAction.desc   = "Subscript (Format Link)";
  subAction.action = "format:subscript";
  subAction.icon   = "X₂";

  RadialAction align;
  align.id     = "align";
  align.label  = "⇿";
  align.desc   = "Alignment Sub-Wheel";
  align.action = "subwheel:alignment";

  RadialAction aLeft;
  aLeft.id     = "align-left";
  aLeft.label  = "⇤";
  aLeft.desc   = "Align Left";
  aLeft.action = "align:left";

  RadialAction aCentre;
  aCentre.id     = "align-centre";
  aCentre.label  = "⇼";
  aCentre.desc   = "Align Centre";
  aCentre.action = "align:centre";

  RadialAction aRight;
  aRight.id     = "align-right";
  aRight.label  = "⇥";
  aRight.desc   = "Align Right";
  aRight.action = "align:right";

  RadialAction aJustify;
  aJustify.id     = "align-justify";
  aJustify.label  = "⇿";
  aJustify.desc   = "Justify";
  aJustify.action = "align:justify";

  align.subActions = {std::move(aLeft), std::move(aCentre), std::move(aRight),
                      std::move(aJustify)};

  RadialAction breakAction;
  breakAction.id     = "break";
  breakAction.label  = "✂";
  breakAction.desc   = "Page Break (OpKind::PageBreak)";
  breakAction.action = "op:pagebreak";

  RadialAction linkAction;
  linkAction.id     = "link";
  linkAction.label  = "🔗";
  linkAction.desc   = "Link Forge / Staging";
  linkAction.action = "link:forge";

  RadialAction transcludeAction;
  transcludeAction.id     = "transclude";
  transcludeAction.label  = "⎘";
  transcludeAction.desc   = "Transclude to New Document";
  transcludeAction.action = "op:transclude";

  RadialAction authorAction;
  authorAction.id     = "author";
  authorAction.label  = "👤";
  authorAction.desc   = "Author Provenance & Merkle Ledger";
  authorAction.action = "info:author";

  cfg.actions = {
      std::move(bold),             std::move(italic),
      std::move(underline),        std::move(superAction),
      std::move(subAction),        std::move(align),
      std::move(breakAction),      std::move(linkAction),
      std::move(transcludeAction), std::move(authorAction),
  };

  return cfg;
}

RadialMenu::RadialMenu(std::string aFontName)
    : fontName_(std::move(aFontName)), config_(RadialConfig::createDefault()) {}

RadialMenu::~RadialMenu() = default;

void RadialMenu::setConfig(RadialConfig aConfig) {
  config_       = std::move(aConfig);
  inSubWheel_   = false;
  revision_++;
}

void RadialMenu::setRadius(const float outer, const float inner) {
  config_.radius      = std::max(outer, 40.0F);
  config_.innerRadius = std::clamp(inner, 10.0F, config_.radius - 20.0F);
  revision_++;
}

void RadialMenu::open(const float screenX, const float screenY,
                      const std::uint32_t aDocIndex,
                      const std::uint32_t aCharOffset,
                      const std::uint32_t aCharLength) {
  centerX_          = screenX;
  centerY_          = screenY;
  targetDocIndex_   = aDocIndex;
  targetCharOffset_ = aCharOffset;
  targetCharLength_ = aCharLength;
  open_             = true;
  inSubWheel_       = false;
  rebuildLayout(lastScreenWidth_ > 0.0F ? lastScreenWidth_ : 1920.0F,
                lastScreenHeight_ > 0.0F ? lastScreenHeight_ : 1080.0F);
  revision_++;
}

void RadialMenu::openAtWindowCoords(const float windowX, const float windowY,
                                    const std::uint32_t aDocIndex,
                                    const std::uint32_t aCharOffset,
                                    const std::uint32_t aCharLength) {
  const float fallbackW = lastScreenWidth_ > 0.0F ? lastScreenWidth_ : 1920.0F;
  const float fallbackH =
      lastScreenHeight_ > 0.0F ? lastScreenHeight_ : 1080.0F;
  const float wx = (windowX <= 0.0F) ? (fallbackW * 0.5F) : windowX;
  const float wy = (windowY <= 0.0F) ? (fallbackH * 0.5F) : windowY;
  const float canvasY =
      (lastScreenHeight_ > 0.0F) ? (lastScreenHeight_ - wy) : (fallbackH - wy);
  open(wx, canvasY, aDocIndex, aCharOffset, aCharLength);
}

void RadialMenu::close() {
  if (open_) {
    open_       = false;
    inSubWheel_ = false;
    revision_++;
  }
}

void RadialMenu::toggle(const float screenX, const float screenY,
                        const std::uint32_t aDocIndex,
                        const std::uint32_t aCharOffset,
                        const std::uint32_t aCharLength) {
  if (open_) {
    close();
  } else {
    open(screenX, screenY, aDocIndex, aCharOffset, aCharLength);
  }
}

void RadialMenu::enterSubRadial(const std::size_t actionIndex) {
  if (actionIndex < config_.actions.size() &&
      !config_.actions[actionIndex].subActions.empty()) {
    inSubWheel_         = true;
    activeParentAction_ = actionIndex;
    rebuildLayout(lastScreenWidth_ > 0.0F ? lastScreenWidth_ : 1920.0F,
                  lastScreenHeight_ > 0.0F ? lastScreenHeight_ : 1080.0F);
    revision_++;
  }
}

void RadialMenu::exitSubRadial() {
  if (inSubWheel_) {
    inSubWheel_ = false;
    rebuildLayout(lastScreenWidth_ > 0.0F ? lastScreenWidth_ : 1920.0F,
                  lastScreenHeight_ > 0.0F ? lastScreenHeight_ : 1080.0F);
    revision_++;
  }
}

int RadialMenu::resolveSector(const float dx, const float dy,
                              const std::size_t count) noexcept {
  if (count == 0) {
    return -1;
  }
  // dy > 0 is Up in canvas coords, dx > 0 is Right
  const float phi = std::atan2(dy, dx); // [-pi, pi]
  constexpr float twoPi =
      2.0F * std::numbers::pi_v<float>;
  constexpr float halfPi = 0.5F * std::numbers::pi_v<float>;

  // Clockwise angle from North (+Y)
  float alpha = halfPi - phi;
  while (alpha < 0.0F) {
    alpha += twoPi;
  }
  while (alpha >= twoPi) {
    alpha -= twoPi;
  }

  const float sectorWidth = twoPi / static_cast<float>(count);
  const auto idx = static_cast<int>(
      std::floor((alpha + sectorWidth * 0.5F) / sectorWidth));
  return idx % static_cast<int>(count);
}

void RadialMenu::deviceReady(render::RenderDevice &device,
                             const render::PipelineDesc &documentPipeline) {
  canvas_ = std::make_unique<Canvas>(&device, fontName_);
  canvas_->createPipeline(documentPipeline, false);
}

bool RadialMenu::busy() const {
  return false;
}

void RadialMenu::rebuildLayout(const float screenW, const float screenH) {
  currentPods_.clear();

  // Convert SDL coords (0,0 top-left) to Canvas coords (0,0 bottom-left) if needed
  float cX = centerX_;
  float cY = centerY_;
  if (cY < 0.0F || cY > screenH) {
    cY = screenH * 0.5F;
  }
  if (cX < 0.0F || cX > screenW) {
    cX = screenW * 0.5F;
  }

  // Ensure menu stays within screen viewport
  const float r = config_.radius + kPodWidth * 0.5F + 8.0F;
  cX = std::clamp(cX, r, std::max(r, screenW - r));
  cY = std::clamp(cY, r, std::max(r, screenH - r));

  hubX_    = cX - kHubSize * 0.5F;
  hubY_    = cY - kHubSize * 0.5F;
  hubSize_ = kHubSize;

  const auto &actionList =
      inSubRadial() ? config_.actions[activeParentAction_].subActions
                    : config_.actions;
  const auto count = actionList.size();
  if (count == 0) {
    return;
  }

  const float rMid = (config_.innerRadius + config_.radius) * 0.5F;
  constexpr float twoPi = 2.0F * std::numbers::pi_v<float>;
  constexpr float halfPi = 0.5F * std::numbers::pi_v<float>;

  for (std::size_t i = 0; i < count; ++i) {
    const float angle =
        halfPi - static_cast<float>(i) * (twoPi / static_cast<float>(count));
    const float podCenterX = cX + rMid * std::cos(angle);
    const float podCenterY = cY + rMid * std::sin(angle);

    PodLayout pod;
    pod.actionIndex = i;
    pod.x           = podCenterX - kPodWidth * 0.5F;
    pod.y           = podCenterY - kPodHeight * 0.5F;
    pod.width       = kPodWidth;
    pod.height      = kPodHeight;
    pod.angle = angle;
    pod.tag   = kRadialTagBase + static_cast<std::uint32_t>(i);
    pod.label =
        !actionList[i].icon.empty() ? actionList[i].icon : actionList[i].label;
    pod.desc =
        !actionList[i].label.empty() ? actionList[i].label : actionList[i].desc;
    currentPods_.push_back(std::move(pod));
  }
}

void RadialMenu::drawFrame(FrameContext &ctx) {
  if (!open_ || !canvas_) {
    return;
  }

  const auto screenW = static_cast<float>(ctx.screenWidth);
  const auto screenH = static_cast<float>(ctx.screenHeight);
  lastScreenWidth_   = screenW;
  lastScreenHeight_  = screenH;
  const auto ortho   = glm::ortho(0.0F, screenW, 0.0F, screenH, -1.0F, 1.0F);

  canvas_->clear();
  rebuildLayout(screenW, screenH);

  // 1. Draw frosted plate backdrop and outer circular plate bounds
  const float cX = hubX_ + hubSize_ * 0.5F;
  const float cY = hubY_ + hubSize_ * 0.5F;
  canvas_->addRect(cX - config_.radius * 1.05F, cY - config_.radius * 1.05F,
                   config_.radius * 2.1F, config_.radius * 2.1F, colRadialBg);

  constexpr std::size_t kRingSegments = 32;
  constexpr float twoPi               = 2.0F * std::numbers::pi_v<float>;
  for (std::size_t i = 0; i < kRingSegments; ++i) {
    const float a1 = static_cast<float>(i) * (twoPi / kRingSegments);
    const float a2 = static_cast<float>(i + 1) * (twoPi / kRingSegments);
    const float x1 = cX + config_.radius * std::cos(a1);
    const float y1 = cY + config_.radius * std::sin(a1);
    const float x2 = cX + config_.radius * std::cos(a2);
    const float y2 = cY + config_.radius * std::sin(a2);
    canvas_->addLine(x1, y1, x2, y2, 1.5F, colRadialBorder);
  }

  // 2. Draw radial spokes fanning out to pods
  for (const auto &pod : currentPods_) {
    const float pX = pod.x + pod.width * 0.5F;
    const float pY = pod.y + pod.height * 0.5F;
    const float inX = cX + config_.innerRadius * std::cos(pod.angle);
    const float inY = cY + config_.innerRadius * std::sin(pod.angle);
    canvas_->addLine(inX, inY, pX, pY, 1.0F, colSpoke);
  }

  // 3. Draw action pods
  const auto &actionList =
      inSubRadial() ? config_.actions[activeParentAction_].subActions
                    : config_.actions;

  for (std::size_t i = 0; i < currentPods_.size() && i < actionList.size();
       ++i) {
    const auto &pod = currentPods_[i];
    const auto &act = actionList[i];

    canvas_->setTag(render::tagKindOverlay, pod.tag);
    const std::uint32_t bgCol = act.active ? colPodActiveBg : colPodBg;
    canvas_->addRect(pod.x, pod.y, pod.width, pod.height, bgCol);

    // Border
    canvas_->addLine(pod.x, pod.y, pod.x + pod.width, pod.y, 1.0F,
                     colPodBorder);
    canvas_->addLine(pod.x, pod.y + pod.height, pod.x + pod.width,
                     pod.y + pod.height, 1.0F, colPodBorder);
    canvas_->addLine(pod.x, pod.y, pod.x, pod.y + pod.height, 1.0F,
                     colPodBorder);
    canvas_->addLine(pod.x + pod.width, pod.y, pod.x + pod.width,
                     pod.y + pod.height, 1.0F, colPodBorder);

    // Label text
    const auto metrics = canvas_->measureText(pod.label);
    const float tX     = pod.x + (pod.width - metrics.width) * 0.5F;
    const float tY = pod.y + (pod.height + metrics.height) * 0.5F - 2.0F;
    const std::uint32_t txtCol = act.active ? colRadialAccent : colPodText;
    canvas_->addText(ctx.state, tX, tY, pod.label, txtCol, bgCol);
  }

  // 4. Central Hub Plate
  const std::uint32_t hubTag =
      inSubRadial() ? kRadialTagBack : kRadialTagHub;
  canvas_->setTag(render::tagKindOverlay, hubTag);
  canvas_->addRect(hubX_, hubY_, hubSize_, hubSize_, colHubBg);
  canvas_->addLine(hubX_, hubY_, hubX_ + hubSize_, hubY_, 1.5F,
                   colRadialAccent);
  canvas_->addLine(hubX_, hubY_ + hubSize_, hubX_ + hubSize_, hubY_ + hubSize_,
                   1.5F, colRadialAccent);
  canvas_->addLine(hubX_, hubY_, hubX_, hubY_ + hubSize_, 1.5F,
                   colRadialAccent);
  canvas_->addLine(hubX_ + hubSize_, hubY_, hubX_ + hubSize_, hubY_ + hubSize_,
                   1.5F, colRadialAccent);

  const std::string hubText = inSubRadial() ? "BACK" : "XUDU";
  const auto hubMetrics     = canvas_->measureText(hubText);
  const float htX           = hubX_ + (hubSize_ - hubMetrics.width) * 0.5F;
  const float htY = hubY_ + (hubSize_ + hubMetrics.height) * 0.5F - 2.0F;
  canvas_->addText(ctx.state, htX, htY, hubText, colHubText, colHubBg);

  canvas_->commit();
  canvas_->draw(ctx.state, ortho, 0.98F);
}

bool RadialMenu::picked(const render::PickingResult &pick, RenderState &state) {
  if (!open_) {
    if (openOnRightClick_ && pick.button == 3) {
      std::uint32_t targetDoc =
          pick.tag.docIndex < state.docs.size() ? pick.tag.docIndex : 0;
      std::uint32_t targetAt  = 0;
      std::uint32_t targetLen = 0;
      if (state.caret && state.caret->hasSelection() &&
          state.caret->documentIndex() == targetDoc) {
        targetAt  = state.caret->selectionStart();
        targetLen = state.caret->selectionEnd() - targetAt;
      } else if (targetDoc < state.docs.size() && state.docs[targetDoc]) {
        if (const auto off = state.docs[targetDoc]->offsetForPick(pick.tag)) {
          targetAt  = *off;
          targetLen = 0;
        }
      }
      const float pickCanvasY =
          (lastScreenHeight_ > 0.0F)
              ? (lastScreenHeight_ - static_cast<float>(pick.y))
              : static_cast<float>(pick.y);
      open(static_cast<float>(pick.x), pickCanvasY, targetDoc, targetAt,
           targetLen);
      return true;
    }
    return false;
  }

  // Check tagKindOverlay hits
  if (pick.tag.kind == render::tagKindOverlay) {
    const auto cluster = pick.tag.clusterIndex;

    // Hub or Back button
    if (cluster == kRadialTagBack) {
      exitSubRadial();
      return true;
    }
    if (cluster == kRadialTagHub) {
      close();
      return true;
    }

    // Pod tag hits
    for (const auto &pod : currentPods_) {
      if (cluster == pod.tag) {
        const auto &actionList =
            inSubRadial() ? config_.actions[activeParentAction_].subActions
                          : config_.actions;
        if (pod.actionIndex < actionList.size()) {
          const auto &act = actionList[pod.actionIndex];
          if (!act.subActions.empty()) {
            enterSubRadial(pod.actionIndex);
            return true;
          }
          if (act.onSelect) {
            act.onSelect();
          }
          if (actionHandler_) {
            actionHandler_(act.id, act.action, targetDocIndex_,
                           targetCharOffset_, targetCharLength_);
          }
          close();
          return true;
        }
      }
    }
  }

  // Ballistic angle resolution fallback
  const float cX = hubX_ + hubSize_ * 0.5F;
  const float cY = hubY_ + hubSize_ * 0.5F;
  // Convert pick.y from SDL coords to Canvas coords
  const float pickCanvasY =
      (lastScreenHeight_ > 0.0F) ? (lastScreenHeight_ - static_cast<float>(pick.y))
                                 : static_cast<float>(pick.y);
  const float dx   = static_cast<float>(pick.x) - cX;
  const float dy   = pickCanvasY - cY;
  const float dist = std::hypot(dx, dy);

  if (dist >= config_.innerRadius && dist <= config_.radius * 1.35F) {
    const int sector = resolveSector(dx, dy, currentPods_.size());
    if (sector >= 0 && static_cast<std::size_t>(sector) < currentPods_.size()) {
      const auto &pod = currentPods_[static_cast<std::size_t>(sector)];
      const auto &actionList =
          inSubRadial() ? config_.actions[activeParentAction_].subActions
                        : config_.actions;
      if (pod.actionIndex < actionList.size()) {
        const auto &act = actionList[pod.actionIndex];
        if (!act.subActions.empty()) {
          enterSubRadial(pod.actionIndex);
          return true;
        }
        if (act.onSelect) {
          act.onSelect();
        }
        if (actionHandler_) {
          actionHandler_(act.id, act.action, targetDocIndex_, targetCharOffset_,
                         targetCharLength_);
        }
        close();
        return true;
      }
    }
  } else if (dist < config_.innerRadius) {
    // Inside center hub
    if (inSubRadial()) {
      exitSubRadial();
    } else {
      close();
    }
    return true;
  }

  // Clicking outside radial menu dismisses it
  close();
  return false;
}

void RadialMenu::describe(a11y::Builder &into) {
  if (!open_) {
    return;
  }

  constexpr std::uint64_t barId = 0x8000;
  auto &bar                     = into.add(barId, a11y::Role::Group);
  bar.label                     = inSubRadial() ? "Alignment Sub-Menu"
                                                : "3D Radial Marking Menu";

  const auto &actionList =
      inSubRadial() ? config_.actions[activeParentAction_].subActions
                    : config_.actions;

  for (std::size_t i = 0; i < actionList.size(); ++i) {
    const auto &act   = actionList[i];
    const auto nodeId = 0x8000U + 100U + static_cast<std::uint64_t>(i);
    auto &node        = into.add(nodeId, a11y::Role::Button);
    node.label        = act.desc.empty() ? act.label : act.desc;
    node.actions      = a11y::bit(a11y::Action::Click);
    bar.children.push_back(into.id(nodeId));
  }

  const auto hubId = 0x8000U + 99U;
  auto &hubNode    = into.add(hubId, a11y::Role::Button);
  hubNode.label    = inSubRadial() ? "Back to Main Radial Menu"
                                   : "Close Radial Menu";
  hubNode.actions  = a11y::bit(a11y::Action::Click);
  bar.children.push_back(into.id(hubId));

  into.contribute(into.id(barId));
}

} // namespace gleditor

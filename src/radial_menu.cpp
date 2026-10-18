/**
 * @file radial_menu.cpp
 * @brief Implementation of the 3D Radial Marking Menu overlay.
 */
#include <gleditor/radial_menu.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <utility>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <gleditor/canvas.hpp>
#include <gleditor/caret.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/text/font.hpp>

namespace gleditor {

namespace {

constexpr std::uint32_t menuNode        = 0x8000U;
constexpr std::uint32_t hubNode         = menuNode + 99U;
constexpr std::uint32_t firstActionNode = menuNode + 100U;

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
      std::move(bold),
      std::move(italic),
      std::move(underline),
      std::move(superAction),
      std::move(subAction),
      std::move(align),
      std::move(breakAction),
      std::move(linkAction),
      std::move(transcludeAction),
      std::move(authorAction),
  };

  return cfg;
}

RadialMenu::RadialMenu(std::string aFontName)
    : fontName_(std::move(aFontName)), config_(RadialConfig::createDefault()) {}

RadialMenu::~RadialMenu() = default;

void RadialMenu::setConfig(RadialConfig aConfig) {
  const std::scoped_lock lock(guard_);
  config_     = std::move(aConfig);
  inSubWheel_ = false;
  revision_++;
  ++pickingRevision_;
}

void RadialMenu::setRadius(const float outer, const float inner) {
  const std::scoped_lock lock(guard_);
  config_.radius      = std::max(outer, 40.0F);
  config_.innerRadius = std::clamp(inner, 10.0F, config_.radius - 20.0F);
  revision_++;
}

void RadialMenu::open(const float screenX, const float screenY,
                      const std::uint32_t aDocIndex,
                      const std::uint32_t aCharOffset,
                      const std::uint32_t aCharLength) {
  const std::scoped_lock lock(guard_);
  openLocked(screenX, screenY, aDocIndex, aCharOffset, aCharLength);
}

void RadialMenu::openLocked(float screenX, float screenY,
                            std::uint32_t aDocIndex, std::uint32_t aCharOffset,
                            std::uint32_t aCharLength) {
  centerX_          = screenX;
  centerY_          = screenY;
  targetDocIndex_   = aDocIndex;
  targetCharOffset_ = aCharOffset;
  targetCharLength_ = aCharLength;
  open_             = true;
  ++pickingRevision_;
  activate();
  inSubWheel_ = false;
  rebuildLayout(lastScreenWidth_ > 0.0F ? lastScreenWidth_ : 1920.0F,
                lastScreenHeight_ > 0.0F ? lastScreenHeight_ : 1080.0F);
  revision_++;
}

void RadialMenu::openAtWindowCoords(const float windowX, const float windowY,
                                    const std::uint32_t aDocIndex,
                                    const std::uint32_t aCharOffset,
                                    const std::uint32_t aCharLength) {
  const std::scoped_lock lock(guard_);
  const float fallbackW = lastScreenWidth_ > 0.0F ? lastScreenWidth_ : 1920.0F;
  const float fallbackH =
      lastScreenHeight_ > 0.0F ? lastScreenHeight_ : 1080.0F;
  const float wx = (windowX <= 0.0F) ? (fallbackW * 0.5F) : windowX;
  const float wy = (windowY <= 0.0F) ? (fallbackH * 0.5F) : windowY;
  const float canvasY =
      (lastScreenHeight_ > 0.0F) ? (lastScreenHeight_ - wy) : (fallbackH - wy);
  openLocked(wx, canvasY, aDocIndex, aCharOffset, aCharLength);
}

void RadialMenu::close() {
  const std::scoped_lock lock(guard_);
  closeLocked();
}
void RadialMenu::closeLocked() {
  if (open_) {
    open_ = false;
    deactivate();
    inSubWheel_ = false;
    revision_++;
  }
}

void RadialMenu::toggle(const float screenX, const float screenY,
                        const std::uint32_t aDocIndex,
                        const std::uint32_t aCharOffset,
                        const std::uint32_t aCharLength) {
  const std::scoped_lock lock(guard_);
  if (open_) {
    closeLocked();
  } else {
    openLocked(screenX, screenY, aDocIndex, aCharOffset, aCharLength);
  }
}

void RadialMenu::enterSubRadial(const std::size_t actionIndex) {
  const std::scoped_lock lock(guard_);
  enterSubRadialLocked(actionIndex);
}
void RadialMenu::enterSubRadialLocked(const std::size_t actionIndex) {
  if (actionIndex < config_.actions.size() &&
      !config_.actions[actionIndex].subActions.empty()) {
    inSubWheel_         = true;
    activeParentAction_ = actionIndex;
    ++pickingRevision_;
    rebuildLayout(lastScreenWidth_ > 0.0F ? lastScreenWidth_ : 1920.0F,
                  lastScreenHeight_ > 0.0F ? lastScreenHeight_ : 1080.0F);
    revision_++;
  }
}

void RadialMenu::exitSubRadial() {
  const std::scoped_lock lock(guard_);
  exitSubRadialLocked();
}
void RadialMenu::exitSubRadialLocked() {
  if (inSubWheel_) {
    inSubWheel_ = false;
    ++pickingRevision_;
    rebuildLayout(lastScreenWidth_ > 0.0F ? lastScreenWidth_ : 1920.0F,
                  lastScreenHeight_ > 0.0F ? lastScreenHeight_ : 1080.0F);
    revision_++;
  }
}

std::optional<std::size_t>
RadialMenu::resolveSector(const float dx, const float dy,
                          const std::size_t count) noexcept {
  if (count == 0) {
    return std::nullopt;
  }
  // dy > 0 is Up in canvas coords, dx > 0 is Right
  const float phi        = std::atan2(dy, dx); // [-pi, pi]
  constexpr float twoPi  = 2.0F * std::numbers::pi_v<float>;
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
  const auto idx          = static_cast<std::size_t>(
      std::floor((alpha + sectorWidth * 0.5F) / sectorWidth));
  return idx % count;
}

void RadialMenu::deviceReady(render::RenderDevice &device,
                             const render::PipelineDesc &documentPipeline) {
  const std::scoped_lock lock(guard_);
  device_   = &device;
  pipeline_ = documentPipeline;
  canvas_.reset();
  drawnRevision_ = 0;
  pickScope_     = 0;
}

bool RadialMenu::busy() const { return false; }

void RadialMenu::rebuildLayout(const float screenW, const float screenH) {
  currentPods_.clear();
  auto metrics         = metrics_;
  metrics.screenWidth  = static_cast<int>(screenW);
  metrics.screenHeight = static_cast<int>(screenH);
  const auto safe      = metrics.pixelSafeArea();
  const auto line    = font_ ? font_->metrics().lineHeight
                             : metrics.fontPixels(ui::FontRole::Label, theme_);
  const auto padding = line * theme_.paddingEm;
  const auto border  = std::max(1.0F, metrics.px(1));
  const auto available =
      std::max(0.0F, std::min(safe.width, safe.height) * .5F);
  const auto plateRadius = std::min(
      available, std::max(metrics.px(config_.radius), line + padding) + border);
  outerRadiusPx_         = std::max(0.0F, plateRadius - border);
  const auto touchRadius = metrics.px(theme_.type.minTouchPx) * .5F;
  innerRadiusPx_         = std::clamp(
      std::max({metrics.px(config_.innerRadius), line * .9F, touchRadius}),
      0.0F, outerRadiusPx_ * .55F);
  hubRadius_ = innerRadiusPx_ * .85F;
  const auto placed =
      ui::clampToSafeArea({centerX_ - plateRadius, centerY_ - plateRadius,
                           plateRadius * 2, plateRadius * 2},
                          safe);
  const auto cX = placed.left + placed.width * .5F;
  const auto cY = placed.bottom + placed.height * .5F;
  hubSize_      = hubRadius_ * 2;
  hubX_         = cX - hubRadius_;
  hubY_         = cY - hubRadius_;
  auto next     = std::make_shared<ui::LayoutResult>();
  next->bounds  = placed;
  next->boxes.push_back({menuNode, 0, placed, placed});
  const ui::Rect hubRect{hubX_, hubY_, hubSize_, hubSize_};
  const auto hubContent =
      ui::clampToSafeArea({cX - hubRadius_ * .7F, cY - hubRadius_ * .7F,
                           hubRadius_ * 1.4F, hubRadius_ * 1.4F},
                          hubRect);
  next->boxes.push_back({hubNode, menuNode, hubRect, hubContent, true});
  next->focusOrder.push_back(hubNode);

  const auto &actionList = inSubRadialLocked()
                               ? config_.actions[activeParentAction_].subActions
                               : config_.actions;
  const auto count       = actionList.size();
  if (count == 0) {
    layout_ = std::move(next);
    return;
  }

  const float rMid       = (innerRadiusPx_ + outerRadiusPx_) * 0.5F;
  constexpr float twoPi  = 2.0F * std::numbers::pi_v<float>;
  constexpr float halfPi = 0.5F * std::numbers::pi_v<float>;

  const float sectorWidth  = twoPi / static_cast<float>(count);
  constexpr float gapAngle = 0.035F; // ~2 deg angular gap between wedge buttons
  const float wedgeRIn     = innerRadiusPx_;
  const float wedgeROut    = outerRadiusPx_;

  for (std::size_t i = 0; i < count; ++i) {
    const float angle =
        halfPi - static_cast<float>(i) * (twoPi / static_cast<float>(count));
    const float podCenterX = cX + rMid * std::cos(angle);
    const float podCenterY = cY + rMid * std::sin(angle);

    PodLayout pod;
    pod.actionIndex = i;
    pod.angle       = angle;
    pod.startAngle  = angle - sectorWidth * 0.5F + gapAngle * 0.5F;
    pod.endAngle    = angle + sectorWidth * 0.5F - gapAngle * 0.5F;
    pod.innerRadius = wedgeRIn;
    pod.outerRadius = wedgeROut;
    pod.tag         = kRadialTagBase + static_cast<std::uint32_t>(i);
    pod.label =
        !actionList[i].icon.empty() ? actionList[i].icon : actionList[i].label;
    pod.desc =
        !actionList[i].desc.empty() ? actionList[i].desc : actionList[i].label;
    float left = cX, right = cX, bottom = cY, top = cY;
    bool first         = true;
    const auto include = [&](float at, float radius) {
      const auto x = cX + radius * std::cos(at);
      const auto y = cY + radius * std::sin(at);
      if (first) {
        left = right = x;
        bottom = top = y;
        first        = false;
      } else {
        left   = std::min(left, x);
        right  = std::max(right, x);
        bottom = std::min(bottom, y);
        top    = std::max(top, y);
      }
    };
    for (const auto radius : {wedgeRIn, wedgeROut}) {
      include(pod.startAngle, radius);
      include(pod.endAngle, radius);
      for (int quarter = 0; quarter < 4; ++quarter) {
        const auto at = static_cast<float>(quarter) * halfPi;
        auto delta    = std::fmod(at - pod.startAngle + twoPi, twoPi);
        if (delta <= pod.endAngle - pod.startAngle) include(at, radius);
      }
    }
    const auto wedge =
        ui::clampToSafeArea({left, bottom, right - left, top - bottom}, placed);
    const auto textWidth = std::min(
        wedge.width,
        2 * rMid * std::sin(std::min(sectorWidth * .5F, halfPi)) * .8F);
    const auto textHeight =
        std::min(wedge.height, (wedgeROut - wedgeRIn) * .8F);
    const auto content = ui::clampToSafeArea({podCenterX - textWidth * .5F,
                                              podCenterY - textHeight * .5F,
                                              textWidth, textHeight},
                                             wedge);
    pod.x              = wedge.left;
    pod.y              = wedge.bottom;
    pod.width          = wedge.width;
    pod.height         = wedge.height;
    const auto id      = firstActionNode + static_cast<std::uint32_t>(i);
    next->boxes.push_back(
        {id, menuNode, wedge, content, true, actionList[i].enabled, menuNode});
    if (actionList[i].enabled) next->focusOrder.push_back(id);
    currentPods_.push_back(std::move(pod));
  }
  layout_ = std::move(next);
}

std::shared_ptr<const ui::LayoutResult>
RadialMenu::prepareLayout(const ui::UiMetrics &metrics,
                          const ui::Theme &theme) {
  const std::scoped_lock lock(guard_);
  return prepareLayoutLocked(metrics, theme);
}
std::shared_ptr<const ui::LayoutResult>
RadialMenu::prepareLayoutLocked(const ui::UiMetrics &metrics,
                                const ui::Theme &theme) {
  if (!layout_ || preparedRevision_ != revision_ || metrics_ != metrics ||
      theme_ != theme) {
    metrics_          = metrics;
    theme_            = theme;
    lastScreenWidth_  = static_cast<float>(metrics.screenWidth);
    lastScreenHeight_ = static_cast<float>(metrics.screenHeight);
    resolvedFont_ = ui::scaledFontDescription(fontName_, ui::FontRole::Label,
                                              metrics_, theme_);
    font_         = text::FontManager::instance().getFont(resolvedFont_);
    rebuildLayout(static_cast<float>(metrics_.screenWidth),
                  static_cast<float>(metrics_.screenHeight));
    for (auto &pod : currentPods_) {
      const auto *box = layout_->find(
          firstActionNode + static_cast<std::uint32_t>(pod.actionIndex));
      if (box && box->contentRect.width > 0 && box->contentRect.height > 0)
        pod.fitted = shaping_.fitted(pod.label, font_,
                                     {.maxWidthPx  = box->contentRect.width,
                                      .maxHeightPx = box->contentRect.height,
                                      .align       = TextAlign::Centre});
    }
    const auto *hub = layout_->find(hubNode);
    hubFitted_      = {};
    if (hub && hub->contentRect.width > 0 && hub->contentRect.height > 0)
      hubFitted_ = shaping_.fitted(inSubRadialLocked() ? "BACK" : "XUDU", font_,
                                   {.maxWidthPx  = hub->contentRect.width,
                                    .maxHeightPx = hub->contentRect.height,
                                    .align       = TextAlign::Centre});
    if (!pickingTargets_ || builtPickingRevision_ != pickingRevision_) {
      // A raw wedge index can mean a different action after configuration or
      // submenu changes while an asynchronous readback is still outstanding.
      const auto lastTag =
          currentPods_.empty()
              ? kRadialTagBack
              : std::max(kRadialTagBack, currentPods_.back().tag);
      auto targets = std::make_shared<std::vector<std::uint32_t>>(lastTag, 0);
      const auto token = [&] {
        if (nextPickingTarget_ == std::numeric_limits<std::uint32_t>::max())
          throw std::length_error("Radial action picking identities exhausted");
        return ++nextPickingTarget_;
      };
      for (const auto &pod : currentPods_) (*targets)[pod.tag - 1] = token();
      (*targets)[(inSubRadialLocked() ? kRadialTagBack : kRadialTagHub) - 1] =
          token();
      pickingTargets_       = std::move(targets);
      builtPickingRevision_ = pickingRevision_;
    }
    ++revision_;
    preparedRevision_ = revision_;
  }
  return layout_;
}

void RadialMenu::drawDisc(Canvas &canvas, const float cX, const float cY,
                          const float radius, const std::uint32_t fillCol,
                          const std::uint32_t borderCol,
                          const float borderWidth, const std::size_t slices) {
  if (radius <= 0.0F) {
    return;
  }
  const std::size_t nSlices = std::max<std::size_t>(8, slices);
  const float step          = (2.0F * radius) / static_cast<float>(nSlices);
  const float rSq           = radius * radius;

  for (std::size_t i = 0; i < nSlices; ++i) {
    const float y0    = -radius + static_cast<float>(i) * step;
    const float yMid  = y0 + 0.5F * step;
    const float remSq = rSq - yMid * yMid;
    if (remSq <= 0.0F) {
      continue;
    }
    const float wHalf = std::sqrt(remSq);
    canvas.addRect(cX - wHalf, cY + y0, 2.0F * wHalf, step + 1.0F, fillCol);
  }

  if (borderWidth > 0.0F && borderCol != 0) {
    constexpr std::size_t kSegments = 128;
    constexpr float twoPi           = 2.0F * std::numbers::pi_v<float>;
    for (std::size_t i = 0; i < kSegments; ++i) {
      const float a1 = static_cast<float>(i) * (twoPi / kSegments);
      const float a2 = static_cast<float>(i + 1) * (twoPi / kSegments);
      const float x1 = cX + radius * std::cos(a1);
      const float y1 = cY + radius * std::sin(a1);
      const float x2 = cX + radius * std::cos(a2);
      const float y2 = cY + radius * std::sin(a2);
      canvas.addLine(x1, y1, x2, y2, borderWidth, borderCol);
    }
  }
}

void RadialMenu::drawWedge(Canvas &canvas, const float cX, const float cY,
                           const float rIn, const float rOut,
                           const float aStart, const float aEnd,
                           const std::uint32_t fillCol,
                           const std::uint32_t borderCol,
                           const float borderWidth) {
  if (rOut <= rIn || rIn < 0.0F) {
    return;
  }
  constexpr float twoPi = 2.0F * std::numbers::pi_v<float>;

  float span = aEnd - aStart;
  while (span < 0.0F) {
    span += twoPi;
  }
  while (span >= twoPi) {
    span -= twoPi;
  }
  if (span <= 0.0F) {
    return;
  }

  const float sinStart = std::sin(aStart);
  const float cosStart = std::cos(aStart);
  const float sinEnd   = std::sin(aEnd);
  const float cosEnd   = std::cos(aEnd);

  float yMin =
      std::min({rIn * sinStart, rOut * sinStart, rIn * sinEnd, rOut * sinEnd});
  float yMax =
      std::max({rIn * sinStart, rOut * sinStart, rIn * sinEnd, rOut * sinEnd});

  auto inAngle = [&](const float angle) -> bool {
    float da = angle - aStart;
    while (da < 0.0F) {
      da += twoPi;
    }
    while (da >= twoPi) {
      da -= twoPi;
    }
    return da >= -1e-4F && da <= span + 1e-4F;
  };

  constexpr float halfPi = 0.5F * std::numbers::pi_v<float>;
  if (inAngle(halfPi)) {
    yMax = rOut;
  }
  if (inAngle(1.5F * std::numbers::pi_v<float>)) {
    yMin = -rOut;
  }

  constexpr float step = 1.0F;
  const auto nSteps =
      static_cast<std::size_t>(std::max(1.0F, std::ceil((yMax - yMin) / step)));

  for (std::size_t i = 0; i <= nSteps; ++i) {
    const float y = yMin + static_cast<float>(i) * step;
    if (y < -rOut || y > rOut) {
      continue;
    }

    std::vector<float> xs;
    xs.reserve(4);

    if (std::abs(y) <= rOut) {
      const float w = std::sqrt(std::max(0.0F, rOut * rOut - y * y));
      if (inAngle(std::atan2(y, w))) {
        xs.push_back(w);
      }
      if (w > 0.0F && inAngle(std::atan2(y, -w))) {
        xs.push_back(-w);
      }
    }

    if (std::abs(y) <= rIn) {
      const float w = std::sqrt(std::max(0.0F, rIn * rIn - y * y));
      if (inAngle(std::atan2(y, w))) {
        xs.push_back(w);
      }
      if (w > 0.0F && inAngle(std::atan2(y, -w))) {
        xs.push_back(-w);
      }
    }

    if (std::abs(sinStart) > 1e-4F) {
      const float r = y / sinStart;
      if (r >= rIn - 0.5F && r <= rOut + 0.5F) {
        xs.push_back(r * cosStart);
      }
    }

    if (std::abs(sinEnd) > 1e-4F) {
      const float r = y / sinEnd;
      if (r >= rIn - 0.5F && r <= rOut + 0.5F) {
        xs.push_back(r * cosEnd);
      }
    }

    if (xs.size() < 2) {
      continue;
    }

    std::ranges::sort(xs);
    std::vector<float> uniqueXs;
    uniqueXs.reserve(xs.size());
    for (const float x : xs) {
      if (uniqueXs.empty() || std::abs(x - uniqueXs.back()) > 0.5F) {
        uniqueXs.push_back(x);
      }
    }

    if (uniqueXs.size() == 2) {
      const float xL = uniqueXs[0];
      const float xR = uniqueXs[1];
      if (xR > xL) {
        canvas.addRect(cX + xL, cY + y - 0.5F * step, xR - xL, step + 1.0F,
                       fillCol);
      }
    } else if (uniqueXs.size() >= 3) {
      for (std::size_t j = 0; j + 1 < uniqueXs.size(); ++j) {
        const float xMid = (uniqueXs[j] + uniqueXs[j + 1]) * 0.5F;
        const float rSq  = xMid * xMid + y * y;
        if (rSq >= (rIn - 0.5F) * (rIn - 0.5F) &&
            rSq <= (rOut + 0.5F) * (rOut + 0.5F)) {
          if (inAngle(std::atan2(y, xMid))) {
            const float xL = uniqueXs[j];
            const float xR = uniqueXs[j + 1];
            if (xR > xL) {
              canvas.addRect(cX + xL, cY + y - 0.5F * step, xR - xL,
                             step + 1.0F, fillCol);
            }
          }
        }
      }
    }
  }

  if (borderWidth > 0.0F && borderCol != 0) {
    const std::size_t nInSteps =
        static_cast<std::size_t>(std::max(2.0F, std::ceil(rIn * span / 2.0F)));
    for (std::size_t s = 0; s < nInSteps; ++s) {
      const float a0 = aStart + span * (static_cast<float>(s) / nInSteps);
      const float a1 = aStart + span * (static_cast<float>(s + 1) / nInSteps);
      canvas.addLine(cX + rIn * std::cos(a0), cY + rIn * std::sin(a0),
                     cX + rIn * std::cos(a1), cY + rIn * std::sin(a1),
                     borderWidth, borderCol);
    }

    const std::size_t nOutSteps =
        static_cast<std::size_t>(std::max(2.0F, std::ceil(rOut * span / 2.0F)));
    for (std::size_t s = 0; s < nOutSteps; ++s) {
      const float a0 = aStart + span * (static_cast<float>(s) / nOutSteps);
      const float a1 = aStart + span * (static_cast<float>(s + 1) / nOutSteps);
      canvas.addLine(cX + rOut * std::cos(a0), cY + rOut * std::sin(a0),
                     cX + rOut * std::cos(a1), cY + rOut * std::sin(a1),
                     borderWidth, borderCol);
    }

    const std::size_t nRadSteps = static_cast<std::size_t>(
        std::max(1.0F, std::ceil((rOut - rIn) / 2.0F)));
    for (std::size_t s = 0; s < nRadSteps; ++s) {
      const float r0 = rIn + (rOut - rIn) * (static_cast<float>(s) / nRadSteps);
      const float r1 =
          rIn + (rOut - rIn) * (static_cast<float>(s + 1) / nRadSteps);
      canvas.addLine(cX + r0 * cosStart, cY + r0 * sinStart, cX + r1 * cosStart,
                     cY + r1 * sinStart, borderWidth, borderCol);
      canvas.addLine(cX + r0 * cosEnd, cY + r0 * sinEnd, cX + r1 * cosEnd,
                     cY + r1 * sinEnd, borderWidth, borderCol);
    }
  }
}

void RadialMenu::drawFrame(FrameContext &ctx) {
  const std::scoped_lock lock(guard_);
  lastScreenWidth_  = static_cast<float>(ctx.screenWidth);
  lastScreenHeight_ = static_cast<float>(ctx.screenHeight);
  if (!open_ || !device_ || !pipeline_) return;
  auto metrics         = ctx.metrics;
  metrics.screenWidth  = ctx.screenWidth;
  metrics.screenHeight = ctx.screenHeight;
  static_cast<void>(prepareLayoutLocked(metrics, ctx.theme));
  if (pickScope_ == 0)
    pickScope_ = ctx.state.allocatePersistentOverlayPickScope();
  ctx.state.bindOverlayWidgets(
      render::packTagIdentity(render::tagKindOverlay, pickScope_, 0),
      pickingTargets_);
  const auto ortho =
      glm::ortho(0.0F, lastScreenWidth_, 0.0F, lastScreenHeight_, -1.0F, 1.0F);
  if (!canvas_ || canvasFont_ != resolvedFont_) {
    canvas_ = std::make_unique<Canvas>(device_, resolvedFont_);
    canvas_->createPipeline(*pipeline_, false);
    canvas_->setIdentity(pickScope_, 0);
    canvasFont_    = resolvedFont_;
    drawnRevision_ = 0;
  }
  if (drawnRevision_ != preparedRevision_) {
    canvas_->clear();
    canvas_->pushClip(layout_->bounds);
    const auto cX      = hubX_ + hubSize_ * .5F;
    const auto cY      = hubY_ + hubSize_ * .5F;
    const auto surface = ui::rgba(theme_.colours.surface);
    const auto border  = ui::rgba(theme_.colours.border);
    const auto accent  = ui::rgba(theme_.colours.accent);
    canvas_->setTag(render::tagKindOverlay, 0);
    drawDisc(*canvas_, cX, cY, layout_->bounds.width * .5F, surface, border,
             std::max(1.0F, metrics_.px(1)), 128);
    const auto &actions = inSubRadialLocked()
                              ? config_.actions[activeParentAction_].subActions
                              : config_.actions;
    for (const auto &pod : currentPods_) {
      const auto *box = layout_->find(
          firstActionNode + static_cast<std::uint32_t>(pod.actionIndex));
      if (!box) continue;
      const auto &action = actions[pod.actionIndex];
      const bool active  = action.active || focusedNode_ == box->id;
      const auto fill    = active ? accent : surface;
      canvas_->setTag(render::tagKindOverlay, pod.tag);
      drawWedge(*canvas_, cX, cY, pod.innerRadius, pod.outerRadius,
                pod.startAngle, pod.endAngle, fill, border,
                std::max(1.0F, metrics_.px(1)));
      auto textBox = box->contentRect;
      textBox.bottom += (textBox.height - pod.fitted.heightPx) * .5F;
      textBox.height = pod.fitted.heightPx;
      canvas_->addText(ctx.state, textBox, pod.fitted,
                       ui::rgba(action.enabled ? theme_.colours.text
                                               : theme_.colours.disabled),
                       fill);
    }
    canvas_->setTag(render::tagKindOverlay,
                    inSubRadialLocked() ? kRadialTagBack : kRadialTagHub);
    drawDisc(*canvas_, cX, cY, hubRadius_, accent, border,
             std::max(1.0F, metrics_.px(1)), 32);
    if (const auto *hub = layout_->find(hubNode)) {
      auto textBox = hub->contentRect;
      textBox.bottom += (textBox.height - hubFitted_.heightPx) * .5F;
      textBox.height = hubFitted_.heightPx;
      canvas_->addText(ctx.state, textBox, hubFitted_,
                       ui::rgba(theme_.colours.text), accent);
    }
    canvas_->popClip();
    ctx.state.glyphCache.flush();
    canvas_->commit();
    drawnRevision_ = preparedRevision_;
  }
  canvas_->draw(ctx.state, ortho, 0.98F);
}

bool RadialMenu::finishDispatch(ActionDispatch dispatch) {
  if (dispatch.callback) dispatch.callback();
  return dispatch.handled;
}

RadialMenu::ActionDispatch RadialMenu::selectActionLocked(std::size_t index) {
  if (!open_) return false;
  const auto &actions = inSubRadialLocked()
                            ? config_.actions[activeParentAction_].subActions
                            : config_.actions;
  if (index >= actions.size()) return false;
  const auto action = actions[index];
  if (!action.enabled) return true;
  if (!action.subActions.empty()) {
    enterSubRadialLocked(index);
    return true;
  }
  const auto handler = actionHandler_;
  const auto doc = targetDocIndex_, offset = targetCharOffset_,
             length = targetCharLength_;
  closeLocked();
  return {true, [action, handler, doc, offset, length] {
            if (action.onSelect) action.onSelect();
            if (handler) handler(action.id, action.action, doc, offset, length);
          }};
}

bool RadialMenu::keyPressed(Key key, KeyMods) {
  ActionDispatch dispatch;
  {
    const std::scoped_lock lock(guard_);
    if (!open_) return false;
    if (key == Key::Escape) {
      closeLocked();
      return true;
    }
    if (key != Key::Return && key != Key::Space) return false;
    dispatch = activateNodeLocked(focusedNode_);
  }
  return finishDispatch(std::move(dispatch));
}

std::shared_ptr<const ui::LayoutResult> RadialMenu::focusLayout() const {
  const std::scoped_lock lock(guard_);
  return open_ ? layout_ : nullptr;
}
void RadialMenu::focusedNodeChanged(std::uint32_t id) {
  const std::scoped_lock lock(guard_);
  if (focusedNode_ != id) {
    focusedNode_ = id;
    ++revision_;
  }
}
bool RadialMenu::activateNode(std::uint32_t id) {
  ActionDispatch dispatch;
  {
    const std::scoped_lock lock(guard_);
    dispatch = activateNodeLocked(id);
  }
  return finishDispatch(std::move(dispatch));
}
RadialMenu::ActionDispatch RadialMenu::activateNodeLocked(std::uint32_t id) {
  if (!open_) return false;
  if (id == hubNode) {
    if (inSubRadialLocked())
      exitSubRadialLocked();
    else
      closeLocked();
    return true;
  }
  return id >= firstActionNode ? selectActionLocked(id - firstActionNode)
                               : ActionDispatch{};
}

bool RadialMenu::picked(const render::PickingResult &pick, RenderState &state) {
  ActionDispatch dispatch;
  {
    const std::scoped_lock lock(guard_);
    dispatch = pickedLocked(pick, state);
  }
  return finishDispatch(std::move(dispatch));
}
RadialMenu::ActionDispatch
RadialMenu::pickedLocked(const render::PickingResult &pick,
                         RenderState &state) {
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
      openLocked(static_cast<float>(pick.x), pickCanvasY, targetDoc, targetAt,
                 targetLen);
      return true;
    }
    return false;
  }

  // Check tagKindOverlay hits
  const bool ownTag = pick.requestId == 0 ||
                      (pickScope_ != 0 && pick.tag.docIndex == pickScope_ &&
                       pick.tag.pageIndex == 0);
  if (pick.tag.kind == render::tagKindOverlay && ownTag) {
    const auto cluster = pick.tag.clusterIndex;
    if (pick.requestId != 0 &&
        (!pickingTargets_ || builtPickingRevision_ != pickingRevision_ ||
         !pick.overlayWidgetId || cluster == 0 ||
         cluster > pickingTargets_->size() || *pick.overlayWidgetId == 0 ||
         (*pickingTargets_)[cluster - 1] != *pick.overlayWidgetId))
      return true;

    // Hub or Back button
    if (cluster == kRadialTagBack) {
      exitSubRadialLocked();
      return true;
    }
    if (cluster == kRadialTagHub) {
      closeLocked();
      return true;
    }

    for (const auto &pod : currentPods_) {
      if (cluster == pod.tag) return selectActionLocked(pod.actionIndex);
    }
  }

  // Ballistic angle resolution fallback
  const float cX = hubX_ + hubSize_ * 0.5F;
  const float cY = hubY_ + hubSize_ * 0.5F;
  // Convert pick.y from SDL coords to Canvas coords
  const float pickCanvasY =
      (lastScreenHeight_ > 0.0F)
          ? (lastScreenHeight_ - static_cast<float>(pick.y))
          : static_cast<float>(pick.y);
  const float dx   = static_cast<float>(pick.x) - cX;
  const float dy   = pickCanvasY - cY;
  const float dist = std::hypot(dx, dy);

  if (dist >= innerRadiusPx_ && dist <= outerRadiusPx_ * 1.35F) {
    if (const auto sector = resolveSector(dx, dy, currentPods_.size())) {
      return selectActionLocked(currentPods_[*sector].actionIndex);
    }
  } else if (dist < innerRadiusPx_ || dist <= hubRadius_) {
    // Inside center hub
    if (inSubRadialLocked()) {
      exitSubRadialLocked();
    } else {
      closeLocked();
    }
    return true;
  }

  // Clicking outside radial menu dismisses it
  closeLocked();
  return false;
}

void RadialMenu::describe(a11y::Builder &into) {
  const std::scoped_lock lock(guard_);
  if (!open_) return;
  std::vector<std::uint64_t> children;
  const auto bounds = [&](std::uint32_t id) -> std::optional<a11y::Rect> {
    if (!layout_) return std::nullopt;
    const auto *box = layout_->find(id);
    if (!box) return std::nullopt;
    const auto &rect = box->rect;
    const auto height =
        static_cast<double>(metrics_.screenHeight > 0 ? metrics_.screenHeight
                            : lastScreenHeight_ > 0   ? lastScreenHeight_
                                                      : 1080);
    return a11y::Rect{rect.left, height - rect.bottom - rect.height,
                      rect.left + rect.width, height - rect.bottom};
  };
  const auto &actions = inSubRadialLocked()
                            ? config_.actions[activeParentAction_].subActions
                            : config_.actions;
  for (std::size_t i = 0; i < actions.size(); ++i) {
    const auto &action = actions[i];
    const auto id      = firstActionNode + static_cast<std::uint32_t>(i);
    auto &node         = into.add(id, a11y::Role::Button);
    node.label         = action.desc.empty() ? action.label : action.desc;
    node.bounds        = bounds(id);
    node.focusable     = action.enabled;
    node.toggled       = action.active;
    if (action.enabled)
      node.actions =
          a11y::bit(a11y::Action::Click) | a11y::bit(a11y::Action::Focus);
    children.push_back(into.id(id));
  }
  auto &hub = into.add(hubNode, a11y::Role::Button);
  hub.label =
      inSubRadialLocked() ? "Back to Main Radial Menu" : "Close Radial Menu";
  hub.bounds  = bounds(hubNode);
  hub.actions = a11y::bit(a11y::Action::Click) | a11y::bit(a11y::Action::Focus);
  children.push_back(into.id(hubNode));
  auto &bar = into.add(menuNode, a11y::Role::Group);
  bar.label =
      inSubRadialLocked() ? "Alignment Sub-Menu" : "3D Radial Marking Menu";
  bar.bounds   = bounds(menuNode);
  bar.children = std::move(children);
  into.contribute(into.id(menuNode));
}

bool RadialMenu::performAction(std::uint64_t id, a11y::Action action,
                               std::string_view) {
  ActionDispatch dispatch;
  {
    const std::scoped_lock lock(guard_);
    id = a11y::Ids::localOf(id);
    if (id > std::numeric_limits<std::uint32_t>::max() || !open_ || !layout_)
      return false;
    const auto node = static_cast<std::uint32_t>(id);
    const auto *box = layout_->find(node);
    if (!box || !box->focusable || !box->enabled) return false;
    if (action == a11y::Action::Click) dispatch = activateNodeLocked(node);
    if (action == a11y::Action::Focus) {
      if (focusedNode_ != node) {
        focusedNode_ = node;
        ++revision_;
      }
      return true;
    }
  }
  return finishDispatch(std::move(dispatch));
}

} // namespace gleditor

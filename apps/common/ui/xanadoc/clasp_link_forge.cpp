/**
 * @file clasp_link_forge.cpp
 * @brief Tripartite Clasp Assembly Bench widget for N x M asymmetric
 * hyperlinking.
 */
#include "clasp_link_forge.hpp"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <stdexcept>
#include <string>

#include <gleditor/render/types.hpp>

namespace xanadu {

LinkForgeWidget::LinkForgeWidget()
    : presentation_({.id = 0x10000000U, .model = gleditor::ui::Panel{}}) {
  presentation_.setActionHandler(
      [this](const gleditor::ui::WidgetAction &action) {
        // A retained scene can outlive the selection or visibility session
        // that gave its controls meaning until the next frame prepares it.
        if (modelDirty_ || !presentation_.visible()) return;
        std::uint32_t tag{};
        if (action.action == "type") tag = kTagClaspTypeSelector;
        if (action.action == "tier") tag = kTagClaspTierSelector;
        if (action.action == "forge") tag = kTagClaspForgeButton;
        if (action.action == "clear-left") tag = kTagClaspClearLeft;
        if (action.action == "clear-right") tag = kTagClaspClearRight;
        if (tag && actionHandler_) actionHandler_(tag);
      });
}

void LinkForgeWidget::setGeometry(const float x, const float y,
                                  const float width,
                                  const float height) noexcept {
  x_      = x;
  y_      = y;
  width_  = width;
  height_ = height;
}

void LinkForgeWidget::dropLeft(PouchItem item) {
  leftSpans_.push_back(std::move(item));
  changed();
}

void LinkForgeWidget::dropRight(PouchItem item) {
  rightSpans_.push_back(std::move(item));
  changed();
}

void LinkForgeWidget::clearLeft() noexcept {
  leftSpans_.clear();
  changed();
}

void LinkForgeWidget::clearRight() noexcept {
  rightSpans_.clear();
  changed();
}

void LinkForgeWidget::cycleType() noexcept {
  changed();
  switch (selectedType_) {
  case LinkType::Comment:
    selectedType_ = LinkType::Illustration;
    break;
  case LinkType::Illustration:
    selectedType_ = LinkType::Disagreement;
    break;
  case LinkType::Disagreement:
    selectedType_ = LinkType::Authorship;
    break;
  case LinkType::Authorship:
    selectedType_ = LinkType::Quotation;
    break;
  case LinkType::Quotation:
    selectedType_ = LinkType::Dimension;
    break;
  case LinkType::Dimension:
    selectedType_ = LinkType::Format;
    break;
  default:
    selectedType_ = LinkType::Comment;
    break;
  }
}

void LinkForgeWidget::cycleTier() noexcept {
  changed();
  switch (selectedTier_) {
  case ProminenceTier::Author:
    selectedTier_ = ProminenceTier::Curated;
    break;
  case ProminenceTier::Curated:
    selectedTier_ = ProminenceTier::Public;
    break;
  default:
    selectedTier_ = ProminenceTier::Author;
    break;
  }
}

bool LinkForgeWidget::containsLeft(const float screenX,
                                   const float screenY) const noexcept {
  return presentation_.visible() && leftW_ > 0 && leftH_ > 0 &&
         screenX >= leftX_ && screenX <= (leftX_ + leftW_) &&
         screenY >= leftY_ && screenY <= (leftY_ + leftH_);
}

bool LinkForgeWidget::containsRight(const float screenX,
                                    const float screenY) const noexcept {
  return presentation_.visible() && rightW_ > 0 && rightH_ > 0 &&
         screenX >= rightX_ && screenX <= (rightX_ + rightW_) &&
         screenY >= rightY_ && screenY <= (rightY_ + rightH_);
}

bool LinkForgeWidget::forge(Session &session,
                            const std::uint32_t activeDocIndex,
                            const Store &source) {
  if (!canForge() || activeDocIndex >= session.views().size()) {
    return false;
  }

  Link link;
  link.type  = selectedType_;
  link.tier  = selectedTier_;
  link.owner = "local_author";

  link.left.reserve(leftSpans_.size());
  for (const auto &item : leftSpans_) {
    link.left.push_back(item.span);
  }

  link.right.reserve(rightSpans_.size());
  for (const auto &item : rightSpans_) {
    link.right.push_back(item.span);
  }

  auto &destination = session.store(session.storeIndexOf(activeDocIndex));
  // Preflight both sides together before carrying either: failed references
  // keep the bench intact and mint no partially authored link.
  auto all = link.left;
  all.insert(all.end(), link.right.begin(), link.right.end());
  const auto carried = carrySpans(source, destination, all);
  if (!carried) return false;
  const auto split = carried->begin() + link.left.size();
  link.left.assign(carried->begin(), split);
  link.right.assign(split, carried->end());
  session.addLink(activeDocIndex, std::move(link));
  clearLeft();
  clearRight();
  triggerBurst();
  return true;
}

bool LinkForgeWidget::picked(const std::uint32_t tag, Session &session,
                             const std::optional<std::uint32_t> activeDocIndex,
                             const Store &source) {
  switch (tag) {
  case kTagClaspTypeSelector:
    cycleType();
    return true;
  case kTagClaspTierSelector:
    cycleTier();
    return true;
  case kTagClaspClearLeft:
    clearLeft();
    return true;
  case kTagClaspClearRight:
    clearRight();
    return true;
  case kTagClaspForgeButton:
    return activeDocIndex && forge(session, *activeDocIndex, source);
  default:
    return false;
  }
}

void LinkForgeWidget::setActionHandler(
    std::function<void(std::uint32_t)> handler) {
  actionHandler_ = std::move(handler);
}

float LinkForgeWidget::preferredHeight(const gleditor::ui::UiMetrics &metrics,
                                       const gleditor::ui::Theme &theme,
                                       bool compact) const {
  if (heightMetrics_ && *heightMetrics_ == metrics && heightTheme_ &&
      *heightTheme_ == theme && heightCompact_ == compact)
    return preferredHeight_;
  using namespace gleditor::ui;
  const auto label = gleditor::text::FontManager::instance().getFont(
      metrics.fontDescription(FontRole::Label, theme));
  const auto caption = gleditor::text::FontManager::instance().getFont(
      metrics.fontDescription(FontRole::Caption, theme));
  const float labelLine     = label->metrics().lineHeight;
  const float captionLine   = caption->metrics().lineHeight;
  const float containerLine = compact ? captionLine : labelLine;
  const float padding       = containerLine * theme.paddingEm;
  const float gap           = labelLine * theme.gapEm;
  const float touch =
      std::max(theme.type.minTouchPx, metrics.px(theme.type.minTouchPx));
  const float labelHeight   = std::ceil(labelLine) + 2;
  const float captionHeight = std::ceil(captionLine) + 2;
  const float buttonHeight =
      std::ceil(std::max(touch, labelLine * (1 + 2 * theme.paddingEm))) + 2;
  const float captionButton =
      std::ceil(std::max(touch, captionLine * (1 + 2 * theme.paddingEm))) + 2;
  // Panel's intrinsic stack budget includes a trailing gap. Reserve that
  // same budget before sharing the drawer with independently fitted zones.
  const float slot =
      (compact ? 2 * captionHeight : 2 * labelHeight + captionHeight) +
      captionButton + 2 * padding + (compact ? 3 : 4) * gap;
  const float relation = 3 * buttonHeight + 2 * padding + 3 * gap;
  preferredHeight_ =
      std::ceil((compact ? captionHeight : labelHeight) +
                std::max(slot, relation) + 4 * padding + 2 * gap) +
      2;
  heightCompact_ = compact;
  heightMetrics_ = metrics;
  heightTheme_   = theme;
  return preferredHeight_;
}

void LinkForgeWidget::rebuildModel(const gleditor::ui::UiMetrics &metrics,
                                   const gleditor::ui::Theme &theme,
                                   gleditor::ui::Rect bounds) {
  using namespace gleditor::ui;
  if (nextIdentity_ > std::numeric_limits<WidgetId>::max() - 64)
    throw std::overflow_error("clasp widget identities exhausted");
  if (modelDirty_) nextIdentity_ += 32;
  auto localIdentity          = nextIdentity_;
  auto id                     = [&localIdentity] { return ++localIdentity; };
  const auto rootId           = id();
  const auto headingId        = id();
  const auto rowId            = id();
  const auto nextTypeIdentity = id();
  const auto nextTierIdentity = id();
  if (freshSelectors_) {
    typeIdentity_   = nextTypeIdentity;
    tierIdentity_   = nextTierIdentity;
    freshSelectors_ = false;
  }
  const auto font = gleditor::text::FontManager::instance().getFont(
      metrics.fontDescription(FontRole::Label, theme));
  const auto containerFont =
      compactMode_ ? gleditor::text::FontManager::instance().getFont(
                         metrics.fontDescription(FontRole::Caption, theme))
                   : font;
  const float pad    = containerFont->metrics().lineHeight * theme.paddingEm;
  const float gap    = font->metrics().lineHeight * theme.gapEm;
  const float column = std::max(
      0.0F,
      std::floor((bounds.width - std::ceil(pad) * 4 - std::ceil(gap) * 2 - 4) /
                 3));
  const auto caption = gleditor::text::FontManager::instance().getFont(
      metrics.fontDescription(FontRole::Caption, theme));
  const float labelHeight =
      metrics.logical(std::ceil(font->metrics().lineHeight) + 2);
  const float captionHeight =
      metrics.logical(std::ceil(caption->metrics().lineHeight) + 2);
  const float touch =
      std::max(theme.type.minTouchPx, metrics.px(theme.type.minTouchPx));
  const float buttonHeight = metrics.logical(
      std::ceil(std::max(touch, font->metrics().lineHeight *
                                    (1 + 2 * theme.paddingEm))) +
      2);
  const float captionButton = metrics.logical(
      std::ceil(std::max(touch, caption->metrics().lineHeight *
                                    (1 + 2 * theme.paddingEm))) +
      2);
  auto slot = [&](const std::vector<PouchItem> &items, bool left) {
    Widget panel{.id    = id(),
                 .model = Panel{},
                 .fontRole =
                     compactMode_ ? FontRole::Caption : FontRole::Label};
    if (left)
      leftIdentity_ = panel.id;
    else
      rightIdentity_ = panel.id;
    panel.preferred.width = metrics.logical(column);
    std::string source    = "Empty Slot";
    std::string detail;
    if (!items.empty()) {
      const auto &item = items.back();
      if (item.originKind == PouchOriginKind::ZigzagCell) {
        source = "Cell #" + std::to_string(item.originCell);
        detail = item.originRankCoord.empty() ? "d.1" : item.originRankCoord;
      } else {
        source = "Doc Card";
        detail = "OSMIC: " + std::to_string(item.span.length) + "B";
      }
      detail += " (" + std::to_string(items.size()) + " spans)";
    }
    if (compactMode_) {
      panel.children.push_back(
          {.id = id(),
           .model =
               Label{std::string(left ? "Homestead · " : "Toward · ") + source},
           .preferred = {0, captionHeight},
           .fontRole  = FontRole::Caption});
    } else {
      panel.children.push_back({.id    = id(),
                                .model = Label{left ? "Homestead" : "Toward"},
                                .preferred = {0, labelHeight}});
      panel.children.push_back({.id        = id(),
                                .model     = Label{std::move(source)},
                                .preferred = {0, labelHeight}});
    }
    panel.children.push_back(
        {.id        = id(),
         .model     = Label{std::move(detail), TextPurpose::Identifier},
         .preferred = {0, captionHeight},
         .fontRole  = FontRole::Caption});
    panel.children.push_back(
        {.id    = id(),
         .model = Button{left ? "Clear Homestead" : "Clear Toward",
                         left ? "clear-left" : "clear-right", !items.empty()},
         .preferred = {0, captionButton},
         .fontRole  = FontRole::Caption});
    return panel;
  };
  auto left  = slot(leftSpans_, true);
  auto right = slot(rightSpans_, false);
  Widget relation{.id    = id(),
                  .model = Panel{},
                  .fontRole =
                      compactMode_ ? FontRole::Caption : FontRole::Label};
  relation.preferred.width = metrics.logical(column);
  relation.children        = {
      {.id        = typeIdentity_,
       .model     = Button{std::string(linkTypeName(selectedType_)), "type"},
       .preferred = {0, buttonHeight}},
      {.id = tierIdentity_,
       .model =
           Button{"Tier: " + std::string(prominenceTierName(selectedTier_)),
                  "tier"},
       .preferred = {0, buttonHeight}},
      {.id        = id(),
       .model     = Button{canForge() ? "Forge Clasp" : "Empty Slots", "forge",
                           canForge()},
       .preferred = {0, buttonHeight},
       .defaultAction = true}};
  Widget row{
      .id       = rowId,
      .model    = ButtonFlow{},
      .children = {std::move(left), std::move(relation), std::move(right)},
      .fontRole = compactMode_ ? FontRole::Caption : FontRole::Label};
  presentation_.setModel(
      {.id    = rootId,
       .model = Panel{},
       .children =
           {{.id        = headingId,
             .model     = Label{"Clasp Assembly Bench"},
             .preferred = {0, compactMode_ ? captionHeight : labelHeight},
             .fontRole  = compactMode_ ? FontRole::Caption : FontRole::Label},
            std::move(row)},
       .fontRole = compactMode_ ? FontRole::Caption : FontRole::Label});
  modelDirty_ = false;
}

std::shared_ptr<const gleditor::ui::WidgetScene>
LinkForgeWidget::prepareBench(const gleditor::ui::UiMetrics &metrics,
                              const gleditor::ui::Theme &theme,
                              gleditor::ui::Rect requested, bool compact) {
  using namespace gleditor::ui;
  const auto bounds =
      clampToSafeArea(metrics.rounded(requested), metrics.pixelSafeArea());
  const bool placement = !preparedBounds_ ||
                         preparedBounds_->left != bounds.left ||
                         preparedBounds_->bottom != bounds.bottom ||
                         preparedBounds_->width != bounds.width ||
                         preparedBounds_->height != bounds.height;
  const bool style     = !preparedMetrics_ || *preparedMetrics_ != metrics ||
                         !preparedTheme_ || *preparedTheme_ != theme ||
                         !preparedCompact_ || *preparedCompact_ != compact;
  if (preparedCompact_ && *preparedCompact_ != compact) modelDirty_ = true;
  if (!modelDirty_ && !style && !placement) return snapshot();
  compactMode_              = compact;
  effectiveTheme_           = theme;
  auto minimumTheme         = theme;
  minimumTheme.paddingEm    = 0;
  minimumTheme.gapEm        = 0;
  const float fullHeight    = preferredHeight(metrics, theme, compact);
  const float minimumHeight = preferredHeight(metrics, minimumTheme, compact);
  const float decoration    = fullHeight - minimumHeight;
  const float ratio =
      decoration > 0
          ? std::clamp((bounds.height - minimumHeight - 2) / decoration, 0.0F,
                       1.0F)
          : 1;
  effectiveTheme_.paddingEm *= ratio;
  effectiveTheme_.gapEm *= ratio;
  const auto labelFont = gleditor::text::FontManager::instance().getFont(
      metrics.fontDescription(FontRole::Label, theme));
  const auto containerFont =
      compact ? gleditor::text::FontManager::instance().getFont(
                    metrics.fontDescription(FontRole::Caption, theme))
              : labelFont;
  const float line          = labelFont->metrics().lineHeight;
  const float containerLine = containerFont->metrics().lineHeight;
  // Panel and button padding both consume the column's text width. Keep at
  // least one line-height of horizontal space for a visible ellipsis.
  const float widthBudget = std::max(
      0.0F, bounds.width - 3 * line - 2 * line * effectiveTheme_.gapEm - 16);
  effectiveTheme_.paddingEm = std::min(
      effectiveTheme_.paddingEm, widthBudget / (10 * containerLine + 6 * line));
  // Placement and style changes preserve semantic IDs.
  rebuildModel(metrics, effectiveTheme_, bounds);
  if (placement) presentation_.setBounds(bounds);
  if (placement || style) {
    preparedBounds_  = bounds;
    preparedMetrics_ = metrics;
    preparedTheme_   = theme;
    preparedCompact_ = compact;
  }
  setGeometry(bounds.left, bounds.bottom, bounds.width, bounds.height);
  const auto scene  = presentation_.prepare(metrics, effectiveTheme_);
  const auto assign = [&](WidgetId identity, float &x, float &y, float &w,
                          float &h) {
    if (const auto *box = scene->layout.find(identity)) {
      x = box->rect.left;
      y = box->rect.bottom;
      w = box->rect.width;
      h = box->rect.height;
    }
  };
  assign(leftIdentity_, leftX_, leftY_, leftW_, leftH_);
  assign(rightIdentity_, rightX_, rightY_, rightW_, rightH_);
  return scene;
}

void LinkForgeWidget::deviceReady(render::RenderDevice &device,
                                  const render::PipelineDesc &pipeline) {
  presentation_.deviceReady(device, pipeline);
  animation_ = std::make_unique<gleditor::Canvas>(
      &device, gleditor::ui::UiMetrics{}.fontDescription(
                   gleditor::ui::FontRole::Body, gleditor::ui::defaultTheme()));
  animation_->createPipeline(pipeline);
}

void LinkForgeWidget::drawPrepared(gleditor::FrameContext &ctx) {
  gleditor::FrameContext shown{.state          = ctx.state,
                               .viewProjection = ctx.viewProjection,
                               .screenWidth    = ctx.screenWidth,
                               .screenHeight   = ctx.screenHeight,
                               .timeline       = ctx.timeline,
                               .chrome         = ctx.chrome,
                               .settledChrome  = ctx.settledChrome,
                               .metrics        = ctx.metrics,
                               .theme          = effectiveTheme_};
  presentation_.drawFrame(shown);
  if (!animation_ || !presentation_.visible() ||
      (!guideActive_ && burstTimer_ <= 0))
    return;
  animation_->clear();
  animation_->pushClip(ctx.metrics.pixelSafeArea());
  drawAnimation(*animation_);
  animation_->popClip();
  animation_->commit();
  animation_->draw(ctx.state,
                   glm::ortho(0.0F, static_cast<float>(ctx.screenWidth), 0.0F,
                              static_cast<float>(ctx.screenHeight)));
}

void LinkForgeWidget::drawAnimation(gleditor::Canvas &canvas) {
  canvas.setTag(render::tagKindOverlay, 0);
  const float stroke = preparedMetrics_ ? preparedMetrics_->px(2) : 2;
  const auto accent  = preparedTheme_
                           ? gleditor::ui::rgba(preparedTheme_->colours.accent)
                           : 0x38BDF8DD;
  if (guideActive_)
    canvas.addLine(guideOriginX_, guideOriginY_, guideTargetX_, guideTargetY_,
                   stroke, accent);
  if (burstTimer_ <= 0) return;
  const float progress = burstProgress();
  const float radius   = progress * width_ * 0.5F;
  const auto alpha  = static_cast<std::uint32_t>((1 - progress) * 255) & 0xFFU;
  const auto colour = (accent & 0xFFFFFF00U) | alpha;
  const float centerX = x_ + width_ * 0.5F, centerY = y_ + height_ * 0.5F;
  canvas.addLine(centerX - radius, centerY, centerX + radius, centerY, stroke,
                 colour);
  canvas.addLine(centerX, centerY - radius, centerX, centerY + radius, stroke,
                 colour);
}

} // namespace xanadu

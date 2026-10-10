/**
 * @file view_presenter.cpp
 * @brief Layout records to drawing, retained by stamp.
 */
#include "view_presenter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>

#include <gleditor/color.hpp>
#include <gleditor/draw_budget.hpp>
#include <gleditor/logging.hpp>
#include <gleditor/paths.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/text/fit.hpp>
#include <gleditor/translucent_list.hpp>

namespace xanadu::view {

namespace ui   = gleditor::ui;
namespace text = gleditor::text;

namespace {

constexpr const char *kCategory = "view.presenter";

/// One quad per placed thing is an overlay of the picking target; zero there
/// means nothing was drawn, so the first item is tag one.
constexpr std::uint32_t kFirstTag = 1;

/// Packed RGBA with its alpha scaled by @p opacity.
[[nodiscard]] std::uint32_t faded(const std::uint32_t rgba,
                                  const float opacity) noexcept {
  const auto alpha = static_cast<float>(rgba & 0xFFU) / 255.0F;
  return (rgba & 0xFFFFFF00U) |
         static_cast<std::uint32_t>(
             std::lround(std::clamp(alpha * opacity, 0.0F, 1.0F) * 255.0F));
}

[[nodiscard]] std::uint32_t themed(const glm::vec4 &colour,
                                   const float opacity) noexcept {
  return faded(ui::rgba(colour), opacity);
}

/// The slice focus's colour, as the table of reserved colours records it:
/// the one colour that means "here" on every slice presentation.
[[nodiscard]] std::uint32_t focusColour() noexcept {
  for (const auto &colour : reservedColours()) {
    if (colour.meaning == "slice focus") {
      return colour.rgba;
    }
  }
  return ui::rgba(ui::defaultTheme().colours.accent);
}

[[nodiscard]] bool isGhost(const PlacedItem &item) noexcept {
  return 0U != (item.flags & itemGhost);
}

} // namespace

std::string spokenMessage(const ViewMessage message,
                          const std::span<const MessageField> fields) {
  const auto words = messageText(message);
  std::string out;
  out.reserve(words.size());
  for (std::size_t at = 0; at < words.size();) {
    const auto open = words.find('{', at);
    if (std::string_view::npos == open) {
      out.append(words.substr(at));
      break;
    }
    const auto close = words.find('}', open);
    if (std::string_view::npos == close) {
      out.append(words.substr(at));
      break;
    }
    out.append(words.substr(at, open - at));
    const auto name  = words.substr(open + 1, close - open - 1);
    const auto field = std::ranges::find(fields, name, &MessageField::name);
    out.append(field == fields.end() ? words.substr(open, close - open + 1)
                                     : field->value);
    at = close + 1;
  }
  return out;
}

std::span<const float> dashPattern(const DashStyle style) noexcept {
  // In beam widths, drawn then open. Each reads apart from its neighbours in
  // U1's order at the widths edges are drawn at.
  static constexpr std::array<float, 2> kDash{6.0F, 3.0F};
  static constexpr std::array<float, 2> kDot{1.0F, 2.0F};
  static constexpr std::array<float, 6> kDashDotDot{6.0F, 2.0F, 1.0F,
                                                    2.0F, 1.0F, 2.0F};
  static constexpr std::array<float, 4> kDashDot{6.0F, 2.0F, 1.0F, 2.0F};
  static constexpr std::array<float, 4> kLongShort{10.0F, 3.0F, 3.0F, 3.0F};
  switch (style) {
  case DashStyle::Solid:
    return {};
  case DashStyle::Dash:
    return kDash;
  case DashStyle::Dot:
    return kDot;
  case DashStyle::DashDotDot:
    return kDashDotDot;
  case DashStyle::DashDot:
    return kDashDot;
  case DashStyle::LongShort:
    return kLongShort;
  }
  return {};
}

SlicePresenter::SlicePresenter(std::string fontOverride)
    : fontOverride_(std::move(fontOverride)) {
  setTypography(metrics_, ui::defaultTheme());
}

SlicePresenter::~SlicePresenter() = default;

void SlicePresenter::deviceReady(render::RenderDevice &device,
                                 const render::PipelineDesc &documentPipeline) {
  device_   = &device;
  pipeline_ = documentPipeline;
  beams_    = std::make_unique<gleditor::Beams>(&device);
  beams_->createPipeline(gleditor::assetPath("shaders"),
                         gleditor::assetPath("shaders/vulkan"), true);
  chrome_ = std::make_unique<gleditor::Canvas>(&device, chromeName_);
  chrome_->createPipeline(documentPipeline, false);
  planes_.clear();
  drawnStamp_.reset();
  chromeStamp_.reset();
}

bool SlicePresenter::setTypography(const ui::UiMetrics &metrics,
                                   const ui::Theme &theme) {
  const bool changed = !bodyFont_ || theme != theme_ ||
                       metrics.fontScale != metrics_.fontScale ||
                       metrics.userScale != metrics_.userScale ||
                       metrics.contentScale != metrics_.contentScale;
  metrics_ = metrics;
  if (!changed) {
    return false;
  }
  theme_       = theme;
  bodyName_    = ui::scaledFontDescription(fontOverride_, ui::FontRole::Body,
                                           metrics_, theme_);
  captionName_ = ui::scaledFontDescription(fontOverride_, ui::FontRole::Caption,
                                           metrics_, theme_);
  chromeName_  = ui::scaledFontDescription(fontOverride_, ui::FontRole::Label,
                                           metrics_, theme_);
  auto &fonts  = text::FontManager::instance();
  bodyFont_    = fonts.getFont(bodyName_);
  captionFont_ = fonts.getFont(captionName_);
  chromeFont_  = fonts.getFont(chromeName_);
  for (auto &plane : planes_) {
    plane.body->setFontDescription(bodyName_);
    plane.caption->setFontDescription(captionName_);
  }
  if (chrome_) {
    chrome_->setFontDescription(chromeName_);
  }
  ++typographyStamp_;
  chromeStamp_.reset();
  return true;
}

const text::FontFacePtr &
SlicePresenter::fontFor(const SubjectKind kind) const noexcept {
  return SubjectKind::Cell == kind || SubjectKind::ViewCell == kind
             ? bodyFont_
             : captionFont_;
}

const text::FittedText &
SlicePresenter::contentText(const std::string_view words,
                            const float maxWidth) {
  return shaping_.fitted(words, bodyFont_,
                         {.maxWidthPx = maxWidth,
                          .maxLines   = 0,
                          .overflow   = text::Overflow::Wrap});
}

const text::FittedText &SlicePresenter::oneLine(const std::string_view words,
                                                const text::FontFacePtr &font,
                                                const float maxWidth) {
  return shaping_.fitted(words, font,
                         {.maxWidthPx = maxWidth,
                          .maxLines   = 1,
                          .overflow   = text::Overflow::Ellipsis});
}

ContentExtent SlicePresenter::measure(const SubjectId subject,
                                      const float maxWidth,
                                      const std::string_view words) {
  const auto &font = fontFor(subject.kind);
  const float line = font ? font->metrics().lineHeight : 0.0F;
  if (words.empty() || !font) {
    return {.width = 0.0F, .height = line, .lineHeight = line, .lines = 1};
  }
  const auto &fitted = fontFor(subject.kind) == bodyFont_
                           ? contentText(words, maxWidth)
                           : oneLine(words, font, maxWidth);
  return {.width      = fitted.widthPx,
          .height     = std::max(fitted.heightPx, line),
          .lineHeight = line,
          .lines      = std::max<std::uint32_t>(1U, fitted.lines)};
}

SlicePresenter::Plane &SlicePresenter::planeAt(const float z) {
  const auto found = std::ranges::find(planes_, z, &Plane::z);
  if (found != planes_.end()) {
    return *found;
  }
  Plane plane{.z = z};
  plane.body = std::make_unique<gleditor::Canvas>(device_, bodyName_);
  plane.body->createPipeline(pipeline_, true);
  plane.caption = std::make_unique<gleditor::Canvas>(device_, captionName_);
  plane.caption->createPipeline(pipeline_, true);
  planes_.push_back(std::move(plane));
  // Farthest first, so a nearer plane's translucent cells blend over it.
  std::ranges::sort(planes_, {}, &Plane::z);
  return *std::ranges::find(planes_, z, &Plane::z);
}

void SlicePresenter::drawScene(gleditor::FrameContext &ctx,
                               const PresentedPlacement &placement,
                               const PresentedWords &words) {
  if (nullptr == device_ || !beams_) {
    return;
  }
  if (0U == pickScope_) {
    pickScope_ = ctx.state.allocatePersistentOverlayPickScope();
  }
  const glm::mat4 toClip = ctx.viewProjection * placement.toWorld;
  if (!drawnStamp_ || *drawnStamp_ != placement.stamp ||
      drawnTypography_ != typographyStamp_) {
    rebuild(ctx, toClip, placement, words);
  }
  // The picking scene is per frame: every frame names what it drew.
  for (std::size_t i = 0; i < targets_.size(); ++i) {
    if (targets_[i]) {
      ctx.state.bindOverlayPick(
          render::PickingTag{.kind      = render::tagKindOverlay,
                             .docIndex  = pickScope_,
                             .pageIndex = 0,
                             .clusterIndex =
                                 static_cast<std::uint32_t>(i) + kFirstTag},
          targets_[i]);
    }
  }
  project(toClip, {static_cast<float>(ctx.screenWidth),
                   static_cast<float>(ctx.screenHeight)});

  if (beams_->committed() > 0) {
    if (nullptr != ctx.translucent) {
      ctx.translucent->addBeams(*beams_, toClip, 1.0F, 0);
    } else {
      beams_->draw(ctx.state, toClip, 1.0F, 0);
    }
  }
  for (const auto &plane : planes_) {
    if (!plane.used) {
      continue;
    }
    const auto onPlane =
        toClip *
        glm::translate(glm::mat4{1.0F}, glm::vec3{0.0F, 0.0F, plane.z});
    plane.body->draw(ctx.state, onPlane, 1.0F);
    plane.caption->draw(ctx.state, onPlane, 1.0F);
  }
}

void SlicePresenter::rebuild(gleditor::FrameContext &ctx,
                             const glm::mat4 &toClip,
                             const PresentedPlacement &placement,
                             const PresentedWords &words) {
  ++rebuilds_;
  const auto &records = placement.records;
  const bool first    = !drawnStamp_.has_value();
  for (auto &plane : planes_) {
    plane.used = false;
    plane.body->clear();
    plane.caption->clear();
    plane.body->setIdentity(pickScope_, 0);
    plane.caption->setIdentity(pickScope_, 0);
  }
  beams_->clear();

  const auto items = records.items();
  drawn_.assign(items.begin(), items.end());
  drawnContent_.assign(items.size(), ContentMode::None);
  drawnGhost_.assign(items.size(), false);
  targets_.assign(items.size(), nullptr);
  picks_.clear();
  std::unordered_set<SubjectId> shown;

  const auto target = [&](const std::size_t index, const SubjectId subject,
                          const zigzag::CellRef cell) {
    auto made = std::make_shared<const render::PickSemanticTarget>(
        render::PickSemanticTarget{
            .documentId   = std::string(placement.documentId),
            .microversion = std::string(placement.version),
            .cellRef      = cell});
    picks_.emplace(made.get(), subject);
    targets_[index] = std::move(made);
  };
  const float pxPerLogical = metrics_.px(1.0F);
  const auto clears        = [&](const PlacedItem &item) {
    // The clip's hysteresis (§9.1.5): a cell not drawn last time must clear
    // the edge by the margin, so one resting there does not flicker.
    const auto mvp = toClip * glm::translate(glm::mat4{1.0F}, item.centre);
    const float perUnit =
        screenScaleAt(mvp, static_cast<float>(ctx.screenWidth));
    const float margin =
        perUnit > 0.0F ? placement.clipMarginPx * pxPerLogical / perUnit : 0.0F;
    return insideFrustum(mvp, (item.width / 2.0F) + margin,
                                (item.height / 2.0F) + margin);
  };

  for (std::size_t i = 0; i < items.size(); ++i) {
    const auto pose = placedPose(records, items[i]);
    if (!pose) {
      continue;
    }
    auto &item     = drawn_[i];
    item.centre    = pose->centre;
    auto &plane    = planeAt(item.centre.z);
    plane.used     = true;
    const auto tag = static_cast<std::uint32_t>(i) + kFirstTag;
    plane.body->setTag(render::tagKindOverlay, tag);
    plane.caption->setTag(render::tagKindOverlay, tag);

    switch (item.id.kind) {
    case SubjectKind::Cell: {
      const bool focus = 0U != (item.flags & itemFocus);
      bool ghost       = isGhost(item);
      if (!ghost && !focus && !first && !shownWithContent_.contains(item.id) &&
          !clears(item)) {
        ghost = true;
      }
      target(i, item.id, static_cast<zigzag::CellRef>(item.id.value));
      if (ghost) {
        auto stand =
            ghostOf(item, std::min(item.opacity, placement.ghostOpacity));
        drawGhost(plane, stand);
        drawnGhost_[i] = true;
        break;
      }
      shown.insert(item.id);
      const bool marked = std::ranges::any_of(
          placement.highlights, [&](const CellHighlight &highlight) {
            return highlight.cell == item.id.value;
          });
      const auto border = focus    ? focusColour()
                          : marked ? placement.highlightBorder
                                   : ui::rgba(theme_.colours.border);
      drawCell(ctx.state, plane, item, item.content,
               faded(border, item.opacity),
               focus ? PresenterStrokes::kFocusBorderPx
                     : PresenterStrokes::kCellBorderPx,
               words.text(item.id), placement);
      drawnContent_[i] = item.content;
      break;
    }
    case SubjectKind::Marker:
      if (isGhost(item)) {
        // A ghost marks the cell it stands for: clicking it goes there.
        target(i, item.id, static_cast<zigzag::CellRef>(item.id.value));
        drawGhost(plane, item);
        drawnGhost_[i] = true;
      }
      break;
    case SubjectKind::Label: {
      std::optional<std::uint32_t> rule;
      for (const auto &edge : records.edges()) {
        if (edge.label == i) {
          rule = faded(words.cue(edge.relation).colour, item.opacity);
          break;
        }
      }
      drawLabel(ctx.state, plane, item, words.text(item.id), rule);
      drawnContent_[i] = ContentMode::Full;
      break;
    }
    case SubjectKind::ViewCell:
    case SubjectKind::Page:
    case SubjectKind::Document:
    case SubjectKind::Badge:
      // Not drawn by a flat slice placement; a later presenter's (§8.10).
      break;
    }
  }
  for (const auto &edge : records.edges()) {
    addEdge(edge, placement, words);
  }

  for (auto &plane : planes_) {
    if (plane.used) {
      plane.body->commit();
      plane.caption->commit();
    }
  }
  beams_->commit();
  shownWithContent_ = std::move(shown);
  drawnStamp_       = placement.stamp;
  drawnTypography_  = typographyStamp_;
  projectedWith_    = glm::mat4{0.0F};
  GLEDITOR_LOG_DEBUG(kCategory, "rebuilt {} items, {} edges, {} planes",
                     items.size(), records.edges().size(), planes_.size());
}

void SlicePresenter::drawCell(RenderState &state, Plane &plane,
                              const PlacedItem &item, const ContentMode content,
                              const std::uint32_t border, const float borderPx,
                              const std::string_view words,
                              const PresentedPlacement &placement) {
  const float left   = item.centre.x - (item.width / 2.0F);
  const float bottom = item.centre.y - (item.height / 2.0F);
  const float top    = bottom + item.height;
  const auto surface = themed(theme_.colours.surface, item.opacity);
  const auto ink     = themed(theme_.colours.text, item.opacity);
  const float stroke = metrics_.px(borderPx);
  plane.body->addRect(left, bottom, item.width, item.height, surface);
  // Hard edges: a stored cell is a sharp rectangle (principle 1). Four bars
  // inside the box, so the corners meet square at any stroke.
  const float edge = std::min({stroke, item.width / 2.0F, item.height / 2.0F});
  plane.body->addRect(left, bottom, item.width, edge, border);
  plane.body->addRect(left, top - edge, item.width, edge, border);
  plane.body->addRect(left, bottom + edge, edge, item.height - (2.0F * edge),
                      border);
  plane.body->addRect(left + item.width - edge, bottom + edge, edge,
                      item.height - (2.0F * edge), border);

  const ui::Rect inner{
      left + placement.paddingX, bottom + placement.paddingY,
      std::max(0.0F, item.width - (2.0F * placement.paddingX)),
      std::max(0.0F, item.height - (2.0F * placement.paddingY))};
  if (words.empty() || inner.width <= 0.0F || inner.height <= 0.0F) {
    return;
  }
  switch (content) {
  case ContentMode::Full: {
    const auto &fitted = contentText(words, placement.contentMaxWidth);
    plane.body->addText(state, inner, fitted, ink, surface);
    break;
  }
  case ContentMode::Abbreviated: {
    const auto &fitted = oneLine(words, bodyFont_, inner.width);
    const float line   = bodyFont_->metrics().lineHeight;
    plane.body->addText(
        state,
        {inner.left, inner.bottom + inner.height - line, inner.width, line},
        fitted, ink, surface);
    break;
  }
  case ContentMode::Coarse: {
    // Bars where the lines would be: shape without contrast to lose (D2).
    const auto &fitted = contentText(words, placement.contentMaxWidth);
    const float line   = bodyFont_->metrics().lineHeight;
    const float bar    = line * PresenterStrokes::kCoarseBarShare;
    for (const auto &entry : fitted.shaping.lines) {
      const float lineTop   = inner.bottom + inner.height - entry.top;
      const float barBottom = lineTop - ((line + bar) / 2.0F);
      if (barBottom < inner.bottom) {
        break;
      }
      plane.body->addRect(inner.left + entry.left, barBottom,
                          std::min(entry.barWidth, inner.width), bar,
                          themed(theme_.colours.muted, item.opacity));
    }
    break;
  }
  case ContentMode::Badge:
  case ContentMode::None:
    break;
  }
}

void SlicePresenter::drawGhost(Plane &plane, const PlacedItem &item) {
  const float left   = item.centre.x - (item.width / 2.0F);
  const float bottom = item.centre.y - (item.height / 2.0F);
  const float right  = left + item.width;
  const float top    = bottom + item.height;
  const float seam =
      std::min(item.width, item.height) * PresenterStrokes::kGhostSeamShare;
  const float stroke = metrics_.px(PresenterStrokes::kGhostStrokePx);
  const auto colour  = themed(theme_.colours.muted, item.opacity);
  // Four sides with their corners left open: soft-edged and seamed, never a
  // square box (plan §5.1 principle 1).
  plane.body->addLine(left + seam, bottom, right - seam, bottom, stroke,
                      colour);
  plane.body->addLine(right, bottom + seam, right, top - seam, stroke, colour);
  plane.body->addLine(right - seam, top, left + seam, top, stroke, colour);
  plane.body->addLine(left, top - seam, left, bottom + seam, stroke, colour);
}

void SlicePresenter::drawLabel(RenderState &state, Plane &plane,
                               const PlacedItem &item,
                               const std::string_view words,
                               const std::optional<std::uint32_t> rule) {
  if (words.empty() || !captionFont_) {
    return;
  }
  const float left   = item.centre.x - (item.width / 2.0F);
  const float bottom = item.centre.y - (item.height / 2.0F);
  const auto &fitted = oneLine(words, captionFont_, item.width);
  plane.caption->addText(state, {left, bottom, item.width, item.height}, fitted,
                         themed(theme_.colours.text, item.opacity), 0U);
  if (rule) {
    const float thick = metrics_.px(PresenterStrokes::kTickRulePx);
    plane.caption->addRect(left, bottom - thick, item.width, thick, *rule);
  }
}

void SlicePresenter::addEdge(const PlacedEdge &edge,
                             const PresentedPlacement &placement,
                             const PresentedWords &words) {
  const auto cue     = words.cue(edge.relation);
  const auto colour  = faded(cue.colour, edge.opacity);
  const float width  = placement.edgeWidth;
  const auto run     = edge.b - edge.a;
  const float length = glm::length(run);
  if (length <= 0.0F || width <= 0.0F) {
    return;
  }
  const auto pattern = dashPattern(cue.dash);
  if (pattern.empty()) {
    beams_->add(edge.a, edge.b, width, colour, 0);
    return;
  }
  const auto along = run / length;
  float at         = 0.0F;
  for (std::size_t k = 0; at < length; k = (k + 1) % pattern.size()) {
    const float step = pattern[k] * width;
    if (0 == k % 2) {
      const float end = std::min(length, at + step);
      beams_->add(edge.a + (along * at), edge.a + (along * end), width, colour,
                  0, at / length, end / length);
    }
    at += step;
  }
}

void SlicePresenter::project(const glm::mat4 &toClip, const ui::Size viewport) {
  if (toClip == projectedWith_ && viewport == projectedViewport_ &&
      projected_.size() == drawn_.size()) {
    return;
  }
  projectedWith_     = toClip;
  projectedViewport_ = viewport;
  projected_.assign(drawn_.size(), std::nullopt);
  for (std::size_t i = 0; i < drawn_.size(); ++i) {
    const auto &item = drawn_[i];
    const ui::Rect box{item.centre.x - (item.width / 2.0F),
                       item.centre.y - (item.height / 2.0F), item.width,
                       item.height};
    projected_[i] = ui::projectPlane(
        box,
        toClip * glm::translate(glm::mat4{1.0F},
                                glm::vec3{0.0F, 0.0F, item.centre.z}),
        viewport);
  }
  ++projectionRevision_;
}

void SlicePresenter::drawChrome(gleditor::FrameContext &ctx,
                                const PresentedChrome &chrome) {
  if (!chrome_ || !chromeFont_) {
    return;
  }
  const auto width  = static_cast<float>(ctx.screenWidth);
  const auto height = static_cast<float>(ctx.screenHeight);
  if (!chromeStamp_ || *chromeStamp_ != chrome.stamp ||
      chromeMetrics_ != metrics_ || chromeBase_ != ctx.chrome) {
    chrome_->clear();
    chrome_->setTag(render::tagKindNone, 0);
    std::string trail;
    for (const auto &crumb : chrome.breadcrumbs) {
      trail += trail.empty() ? "Walked: " : " › ";
      trail += crumb;
    }
    struct Line {
      std::string_view words;
      glm::vec4 colour;
    };
    std::array<Line, 3> lines{Line{chrome.notice, theme_.colours.accent},
                              Line{trail, theme_.colours.text},
                              Line{chrome.hints, theme_.colours.muted}};
    const float padX = metrics_.px(chrome.paddingX);
    const float padY = metrics_.px(chrome.paddingY);
    const float line = std::ceil(chromeFont_->metrics().lineHeight);
    const auto count = std::ranges::count_if(
        lines, [](const Line &each) { return !each.words.empty(); });
    chromeClaim_ =
        0 == count ? 0.0F : (static_cast<float>(count) * line) + (2.0F * padY);
    if (chromeClaim_ > 0.0F) {
      const float bottom = ctx.chrome.bottom;
      const auto surface = ui::rgba(theme_.colours.surface);
      chrome_->addRect(0.0F, bottom, width, chromeClaim_, surface);
      chrome_->addLine(0.0F, bottom + chromeClaim_, width,
                       bottom + chromeClaim_,
                       metrics_.px(PresenterStrokes::kCellBorderPx),
                       ui::rgba(theme_.colours.border));
      float top = bottom + chromeClaim_ - padY;
      for (const auto &each : lines) {
        if (each.words.empty()) {
          continue;
        }
        const float room   = std::max(0.0F, width - (2.0F * padX));
        const auto &fitted = oneLine(each.words, chromeFont_, room);
        chrome_->addText(ctx.state, {padX, top - line, room, line}, fitted,
                         ui::rgba(each.colour), surface);
        top -= line;
      }
    }
    chrome_->commit();
    chromeStamp_   = chrome.stamp;
    chromeMetrics_ = metrics_;
    chromeBase_    = ctx.chrome;
  }
  ctx.chrome.bottom += chromeClaim_;
  if (chromeClaim_ > 0.0F) {
    const glm::mat4 ortho =
        glm::ortho( // NOLINT(readability-suspicious-call-argument)
            0.0F, width, 0.0F, height, -1.0F, 1.0F);
    chrome_->draw(ctx.state, ortho, 1.0F);
  }
}

std::optional<SubjectId>
SlicePresenter::subjectOf(const render::PickingResult &pick) const {
  if (render::tagKindOverlay != pick.tag.kind ||
      pick.tag.docIndex != pickScope_ || !pick.semanticTarget) {
    return std::nullopt;
  }
  const auto found = picks_.find(pick.semanticTarget.get());
  if (found == picks_.end()) {
    GLEDITOR_LOG_DEBUG(kCategory, "pick from an earlier layout dropped");
    return std::nullopt;
  }
  return found->second;
}

ContentMode SlicePresenter::drawnContent(const std::size_t index) const {
  return index < drawnContent_.size() ? drawnContent_[index]
                                      : ContentMode::None;
}

bool SlicePresenter::drawnAsGhost(const std::size_t index) const {
  return index < drawnGhost_.size() && drawnGhost_[index];
}

std::optional<gleditor::a11y::Rect>
SlicePresenter::screenBounds(const std::size_t index) const {
  if (index >= projected_.size() || !projected_[index]) {
    return std::nullopt;
  }
  const auto &box = projected_[index]->bounds;
  const double h  = projectedViewport_.height;
  return gleditor::a11y::Rect{box.left, h - box.bottom - box.height,
                              box.left + box.width, h - box.bottom};
}

} // namespace xanadu::view

/**
 * @file view_presenter.cpp
 * @brief Layout records to drawing, retained by stamp.
 */
#include "view_presenter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <string>
#include <tuple>
#include <utility>

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

/// SubjectIds in the order the shown set is kept in.
[[nodiscard]] bool subjectBefore(const SubjectId &a,
                                 const SubjectId &b) noexcept {
  return std::tie(a.kind, a.slot, a.value, a.epoch) <
         std::tie(b.kind, b.slot, b.value, b.epoch);
}

/// One quad per placed thing is an overlay of the picking target; zero there
/// means nothing was drawn, so the first item is tag one.
constexpr std::uint32_t kFirstTag = 1;

/// Packed RGBA with its alpha scaled by @p opacity: for what carries alpha,
/// which is a beam.
[[nodiscard]] std::uint32_t withAlpha(const std::uint32_t rgba,
                                      const float opacity) noexcept {
  const auto alpha = static_cast<float>(rgba & 0xFFU) / 255.0F;
  return (rgba & 0xFFFFFF00U) |
         static_cast<std::uint32_t>(
             std::lround(std::clamp(alpha * opacity, 0.0F, 1.0F) * 255.0F));
}

/// @p rgba faded by @p opacity into the scene behind it, which is black.
/// A canvas draws a quad's colour without its alpha -- the one opacity it has
/// is the whole draw's -- so a cell that fades, or a ghost that is faint, is
/// composed here, towards the colour the frame is cleared to. (The finding
/// behind this: every cell and ghost was drawn at full strength, the edge fade
/// of §9.1.5 never seen.)
[[nodiscard]] std::uint32_t faded(const std::uint32_t rgba,
                                  const float opacity) noexcept {
  const float keep  = std::clamp(opacity, 0.0F, 1.0F);
  std::uint32_t out = rgba & 0xFFU;
  for (const unsigned shift : {24U, 16U, 8U}) {
    const auto channel = static_cast<float>((rgba >> shift) & 0xFFU);
    out |= static_cast<std::uint32_t>(std::lround(channel * keep)) << shift;
  }
  return out;
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

/// Whether a line of @p length drawn at @p width in @p pattern is no more
/// than @p most dashes: a pattern's period is its lengths times the width, and
/// a pattern that is not a positive finite period is none.
[[nodiscard]] bool dashesWithinCap(const std::span<const float> pattern,
                                   const float width, const float length,
                                   const std::uint32_t most) noexcept {
  float period = 0.0F;
  for (const auto each : pattern) {
    period += each * width;
  }
  if (!std::isfinite(period) || period <= 0.0F || !std::isfinite(length)) {
    return false;
  }
  const double dashes = (static_cast<double>(length) / period) *
                        static_cast<double>((pattern.size() + 1) / 2);
  return dashes <= static_cast<double>(most);
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

std::span<const float> dashPattern(const ViewDrawConfig &draw,
                                   const DashStyle style) noexcept {
  switch (style) {
  case DashStyle::Solid:
    return {};
  case DashStyle::Dash:
    return draw.dashes[0];
  case DashStyle::Dot:
    return draw.dashes[1];
  case DashStyle::DashDotDot:
    return draw.dashes[2];
  case DashStyle::DashDot:
    return draw.dashes[3];
  case DashStyle::LongShort:
    return draw.dashes[4];
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
  // Tested against depth but not writing it: the beams are drawn among the
  // frame's translucent sheets, back to front, as the UI's other beams are.
  beams_->createPipeline(gleditor::assetPath("shaders"),
                         gleditor::assetPath("shaders/vulkan"), true, false);
  chrome_ = std::make_unique<gleditor::Canvas>(&device, chromeName_);
  chrome_->createPipeline(documentPipeline, false);
  compass_ = std::make_unique<gleditor::Canvas>(&device, chromeName_);
  compass_->createPipeline(documentPipeline, false);
  planes_.clear();
  drawnStamp_.reset();
  chromeStamp_.reset();
  compassStamp_.reset();
}

SlicePresenter *SlicePresenter::setDrawConfig(const ViewDrawConfig &draw) {
  if (draw == draw_) {
    return this;
  }
  draw_ = draw;
  drawnStamp_.reset();
  chromeStamp_.reset();
  compassStamp_.reset();
  return this;
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
  if (compass_) {
    compass_->setFontDescription(chromeName_);
  }
  ++typographyStamp_;
  chromeStamp_.reset();
  compassStamp_.reset();
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
  toClip_             = toClip;
  screenWidth_        = static_cast<float>(ctx.screenWidth);
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
  shownNext_.clear();

  // Which edge a label names, and where each label lies, found once rather
  // than by a scan of the edges for every label and every strand.
  labelRelation_.assign(items.size(), std::nullopt);
  for (const auto &edge : records.edges()) {
    if (edge.label && *edge.label < labelRelation_.size()) {
      labelRelation_[*edge.label] = edge.relation;
    }
  }
  labelBoxes_.clear();
  for (const auto &item : items) {
    if (SubjectKind::Label != item.id.kind) {
      continue;
    }
    const float clear = metrics_.px(draw_.strandLabelClearPx);
    labelBoxes_.push_back({.lo = {item.centre.x - (item.width / 2.0F) - clear,
                                  item.centre.y - (item.height / 2.0F) - clear},
                           .hi = {item.centre.x + (item.width / 2.0F) + clear,
                                  item.centre.y + (item.height / 2.0F) + clear},
                           .z  = item.centre.z});
  }

  const auto target = [&](const std::size_t index, const zigzag::CellRef cell) {
    targets_[index] = targetFor(cell, placement);
  };
  const float pxPerLogical = metrics_.px(1.0F);
  const auto wasShown      = [this](const SubjectId id) {
    return std::ranges::binary_search(shownWithContent_, id, subjectBefore);
  };
  const auto clears = [&](const PlacedItem &item) {
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
      if (!ghost && !focus && !first && !wasShown(item.id) && !clears(item)) {
        ghost = true;
      }
      target(i, static_cast<zigzag::CellRef>(item.id.value));
      if (ghost) {
        auto stand =
            ghostOf(item, std::min(item.opacity, placement.ghostOpacity));
        drawGhost(plane, stand);
        drawnGhost_[i] = true;
        break;
      }
      shownNext_.push_back(item.id);
      const bool marked = std::ranges::any_of(
          placement.highlights, [&](const CellHighlight &highlight) {
            return highlight.cell == item.id.value;
          });
      const auto border = focus    ? focusColour()
                          : marked ? placement.highlightBorder
                                   : ui::rgba(theme_.colours.border);
      drawCell(ctx.state, plane, item, item.content,
               faded(border, item.opacity),
               focus ? draw_.focusBorderPx : draw_.cellBorderPx,
               words.text(item.id), placement);
      drawnContent_[i] = item.content;
      break;
    }
    case SubjectKind::Marker:
      if (isGhost(item)) {
        // A ghost marks the cell it stands for: clicking it goes there.
        target(i, static_cast<zigzag::CellRef>(item.id.value));
        drawGhost(plane, item);
        drawnGhost_[i] = true;
      }
      break;
    case SubjectKind::Label: {
      std::optional<std::uint32_t> rule;
      if (labelRelation_[i]) {
        rule = faded(words.cue(*labelRelation_[i]).colour, item.opacity);
      }
      drawLabel(ctx.state, plane, item, words.text(item.id), rule,
                placement.contentMaxWidth);
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
  const float edgeUnits =
      screenScaleAt(toClip, static_cast<float>(ctx.screenWidth));
  for (const auto &edge : records.edges()) {
    addEdge(edge, placement, words, edgeUnits);
  }

  for (auto &plane : planes_) {
    if (plane.used) {
      plane.body->commit();
      plane.caption->commit();
    }
  }
  beams_->commit();
  std::ranges::sort(shownNext_, subjectBefore);
  shownWithContent_.swap(shownNext_);
  drawnStamp_      = placement.stamp;
  drawnTypography_ = typographyStamp_;
  projectedWith_   = glm::mat4{0.0F};
  GLEDITOR_LOG_DEBUG(kCategory, "rebuilt {} items, {} edges, {} planes",
                     items.size(), records.edges().size(), planes_.size());
}

float SlicePresenter::deviceStroke(const float px, const glm::vec3 &at) const {
  // Strokes are laid out in local units and the placement is drawn at
  // whatever scale the camera and the readable-text rule give it: a one-pixel
  // line in local units can land on a fraction of a device pixel, and a
  // vertical one then drops out between pixel centres altogether (M2 finding
  // B4: a cut cell's left edge was missing). Lengths here are the screen's.
  const float perUnit = screenScaleAt(
      toClip_ * glm::translate(glm::mat4{1.0F}, at), screenWidth_);
  const float wanted = metrics_.px(px);
  return std::isfinite(perUnit) && perUnit > 0.0F ? wanted / perUnit : wanted;
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
  const float stroke = deviceStroke(borderPx, item.centre);
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
    // Whole pixels, so a width that is only easing does not re-shape.
    const auto &fitted = oneLine(words, bodyFont_, std::floor(inner.width));
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
    const float bar    = line * draw_.coarseBarShare;
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

namespace {

/// One quarter of a rounded outline, as the rows of a staircase one stroke
/// tall: the canvas draws only upright quads, so a curve is the steps of one.
/// @p centre is the arc's centre; @p sx and @p sy say which quarter, by the
/// sign of the way it runs from there.
void quarterArc(std::vector<gleditor::ui::Rect> &out, const glm::vec2 centre,
                const float radius, const float sx, const float sy,
                const float stroke) {
  constexpr int kMostRows = 32;
  const int rows          = std::clamp(
      static_cast<int>(std::lround(radius / std::max(stroke, 1e-6F))), 2,
      kMostRows);
  const float rise = radius / static_cast<float>(rows);
  for (int row = 0; row < rows; ++row) {
    const float t0 = rise * static_cast<float>(row);
    const float t1 = t0 + rise;
    // The arc leaves the side at t = 0 (x = radius) and meets the top or
    // bottom at t = radius (x = 0).
    const float far  = std::sqrt(std::max(0.0F, (radius * radius) - (t0 * t0)));
    const float near = std::sqrt(std::max(0.0F, (radius * radius) - (t1 * t1)));
    const float reach = std::min(near, far - stroke);
    out.push_back({.left   = sx > 0.0F ? centre.x + reach : centre.x - far,
                   .bottom = sy > 0.0F ? centre.y + t0 : centre.y - t1,
                   .width  = far - reach,
                   .height = rise});
  }
}

} // namespace

std::vector<gleditor::ui::Rect> ghostOutlineRects(const gleditor::ui::Rect box,
                                                  const float radius,
                                                  const float stroke) {
  std::vector<gleditor::ui::Rect> out;
  const float left   = box.left;
  const float bottom = box.bottom;
  const float right  = left + box.width;
  const float top    = bottom + box.height;
  const float run    = std::max(0.0F, box.width - (2.0F * radius));
  const float rise   = std::max(0.0F, box.height - (2.0F * radius));
  // Each side runs a stroke into the arcs at both ends: two quads that only
  // touch can leave a pixel between them when the plane is scaled.
  const float lap = radius > 0.0F ? stroke : 0.0F;
  out.push_back({left + radius - lap, bottom, run + (2.0F * lap), stroke});
  out.push_back(
      {left + radius - lap, top - stroke, run + (2.0F * lap), stroke});
  out.push_back({left, bottom + radius - lap, stroke, rise + (2.0F * lap)});
  out.push_back(
      {right - stroke, bottom + radius - lap, stroke, rise + (2.0F * lap)});
  if (radius > 0.0F) {
    quarterArc(out, {left + radius, bottom + radius}, radius, -1.0F, -1.0F,
               stroke);
    quarterArc(out, {right - radius, bottom + radius}, radius, 1.0F, -1.0F,
               stroke);
    quarterArc(out, {left + radius, top - radius}, radius, -1.0F, 1.0F, stroke);
    quarterArc(out, {right - radius, top - radius}, radius, 1.0F, 1.0F, stroke);
  }
  return out;
}

glm::vec2 compassCornerOrigin(const std::size_t corner,
                              const gleditor::ui::Rect area,
                              const gleditor::ui::Size panel) {
  const float left =
      0 == corner % 2 ? area.left : area.left + area.width - panel.width;
  const float top =
      corner < 2 ? area.bottom + area.height : area.bottom + panel.height;
  return {left, top};
}

std::size_t
chooseCompassCorner(const std::span<const gleditor::ui::Rect> obstacles,
                    const gleditor::ui::Rect area,
                    const gleditor::ui::Size panel, const std::size_t current) {
  const auto covered = [&](const std::size_t corner) {
    const auto at = compassCornerOrigin(corner, area, panel);
    float total   = 0.0F;
    for (const auto &box : obstacles) {
      const float across = std::min(box.left + box.width, at.x + panel.width) -
                           std::max(box.left, at.x);
      const float down = std::min(box.bottom + box.height, at.y) -
                         std::max(box.bottom, at.y - panel.height);
      if (across > 0.0F && down > 0.0F) {
        total += across * down;
      }
    }
    return total;
  };
  std::size_t best = current < 4 ? current : 0;
  float least      = covered(best);
  for (std::size_t i = 0; i < 4 && least > 0.0F; ++i) {
    if (const auto area = covered(i); area < least) {
      least = area;
      best  = i;
    }
  }
  return best;
}

void SlicePresenter::drawGhost(Plane &plane, const PlacedItem &item) {
  const float left   = item.centre.x - (item.width / 2.0F);
  const float bottom = item.centre.y - (item.height / 2.0F);
  const float radius =
      std::min(item.width, item.height) * draw_.ghostCornerShare;
  GLEDITOR_LOG_TRACE(kCategory,
                     "ghost {} at {},{} size {}x{} opacity {} radius {}",
                     item.id.value, item.centre.x, item.centre.y, item.width,
                     item.height, item.opacity, radius);
  const float stroke = deviceStroke(draw_.ghostStrokePx, item.centre);
  const auto colour  = themed(theme_.colours.text, item.opacity);
  // Rounded where a stored cell is square (plan §5.1 principle 1, §5.5 #3).
  for (const auto &each : ghostOutlineRects(
           {left, bottom, item.width, item.height}, radius, stroke)) {
    plane.body->addRect(each.left, each.bottom, each.width, each.height,
                        colour);
  }
}

void SlicePresenter::drawLabel(RenderState &state, Plane &plane,
                               const PlacedItem &item,
                               const std::string_view words,
                               const std::optional<std::uint32_t> rule,
                               const float maxWidth) {
  if (words.empty() || !captionFont_) {
    return;
  }
  const float left   = item.centre.x - (item.width / 2.0F);
  const float bottom = item.centre.y - (item.height / 2.0F);
  // Shaped at the width the layout measured it at, not at the box's width
  // of this frame: a box that grows or shrinks as it fades would otherwise be
  // a new text to shape on every frame it moves.
  const auto &fitted = oneLine(words, captionFont_, maxWidth);
  // A chip of the surface colour for the words to stand on: a label has no
  // box of a cell's, and drawn straight onto a page beside the slice it had
  // black words on white or white on a page's ink (M2 gate 2, D5).
  const auto chip = themed(theme_.colours.surface, item.opacity);
  plane.caption->addRect(left, bottom, item.width, item.height, chip);
  plane.caption->addText(state, {left, bottom, item.width, item.height}, fitted,
                         themed(theme_.colours.text, item.opacity), chip);
  if (rule) {
    const float thick = deviceStroke(draw_.tickRulePx, item.centre);
    plane.caption->addRect(left, bottom - thick, item.width, thick, *rule);
  }
}

void SlicePresenter::addEdge(const PlacedEdge &edge,
                             const PresentedPlacement &placement,
                             const PresentedWords &words,
                             const float pxPerUnit) {
  const auto cue = words.cue(edge.relation);
  // A strand is the cue of its dimension and the shortest line a view draws:
  // it is held to a least opacity (against the cells it joins fading, not
  // against a ghost's) and a least width on the screen, whatever the scale
  // the placement is drawn at.
  const float opacity = edge.opacity > placement.ghostOpacity
                            ? std::max(edge.opacity, draw_.strandOpacityFloor)
                            : edge.opacity;
  const auto colour   = withAlpha(cue.colour, opacity);
  float width         = placement.edgeWidth;
  if (std::isfinite(pxPerUnit) && pxPerUnit > 0.0F) {
    width = std::max(width, metrics_.px(draw_.strandMinWidthPx) / pxPerUnit);
  }
  GLEDITOR_LOG_TRACE(kCategory, "strand width {} (layout {}, px per unit {})",
                     width, placement.edgeWidth, pxPerUnit);
  const auto run     = edge.b - edge.a;
  const float length = glm::length(run);
  if (length <= 0.0F || width <= 0.0F) {
    return;
  }
  addStrand(edge, run / length, length, width, colour,
            dashPattern(draw_, cue.dash), 0.0F, length, 0);
}

void SlicePresenter::addStrand(const PlacedEdge &edge, const glm::vec3 along,
                               const float length, const float width,
                               const std::uint32_t colour,
                               const std::span<const float> pattern,
                               const float from, const float to,
                               const std::size_t label) {
  if (to <= from) {
    return;
  }
  // The strand stops short of every label that lies across its way, on its
  // own plane: ticks name a link, and a line through the words is no cue.
  for (std::size_t i = label; i < labelBoxes_.size(); ++i) {
    const auto &box = labelBoxes_[i];
    if (box.z != edge.a.z || box.z != edge.b.z) {
      continue;
    }
    float enter  = from;
    float leave  = to;
    bool crosses = true;
    for (int axis = 0; axis < 2 && crosses; ++axis) {
      const float start = edge.a[axis];
      const float way   = along[axis];
      if (0.0F == way) {
        crosses = start > box.lo[axis] && start < box.hi[axis];
        continue;
      }
      float near = (box.lo[axis] - start) / way;
      float far  = (box.hi[axis] - start) / way;
      if (near > far) {
        std::swap(near, far);
      }
      enter   = std::max(enter, near);
      leave   = std::min(leave, far);
      crosses = enter < leave;
    }
    if (crosses) {
      addStrand(edge, along, length, width, colour, pattern, from, enter,
                i + 1);
      addStrand(edge, along, length, width, colour, pattern, leave, to, i + 1);
      return;
    }
  }
  addDashed(edge, along, length, width, colour, pattern, from, to);
}

void SlicePresenter::addDashed(const PlacedEdge &edge, const glm::vec3 along,
                               const float length, const float width,
                               const std::uint32_t colour,
                               const std::span<const float> pattern,
                               const float from, const float to) {
  // A strand that would need more dashes than view.draw.dashMaxCount is drawn
  // solid: a pattern of fine dashes on a long strand is a beam each, and the
  // cost of a frame is not the reader's to find out.
  if (pattern.empty() ||
      !dashesWithinCap(pattern, width, to, draw_.dashMaxCount)) {
    beams_->add(edge.a + (along * from), edge.a + (along * to), width, colour,
                0, from / length, to / length);
    return;
  }
  // The pattern runs from the strand's own start, so a strand cut short of a
  // label keeps the dashes it would have had.
  float at = 0.0F;
  for (std::size_t k = 0; at < to; k = (k + 1) % pattern.size()) {
    const float step = pattern[k] * width;
    if (0 == k % 2) {
      const float begin = std::max(from, at);
      const float end   = std::min(to, at + step);
      if (end > begin) {
        beams_->add(edge.a + (along * begin), edge.a + (along * end), width,
                    colour, 0, begin / length, end / length);
      }
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

std::vector<std::string>
SlicePresenter::hintRowsFitting(const std::string_view hints,
                                const float room) {
  constexpr std::string_view kSeparator = " | ";
  std::vector<std::string> rows;
  const auto widthOf = [this](const std::string_view words) {
    return oneLine(words, chromeFont_, 0.0F).widthPx;
  };
  // Items keep the order they were given, which is most used first; a row
  // takes as many as fit, so nothing is cut mid-word at the edge.
  for (std::size_t at = 0; at <= hints.size();) {
    const auto cut  = std::min(hints.find('\n', at), hints.size());
    const auto text = hints.substr(at, cut - at);
    at              = cut + 1;
    std::string row;
    for (std::size_t from = 0; from <= text.size();) {
      const auto stop = std::min(text.find(kSeparator, from), text.size());
      const auto item = text.substr(from, stop - from);
      from            = stop + kSeparator.size();
      if (item.empty()) {
        continue;
      }
      if (row.empty()) {
        row = item;
      } else if (const auto joined =
                     std::string{row}.append(kSeparator).append(item);
                 widthOf(joined) > room) {
        rows.push_back(std::move(row));
        row = item;
      } else {
        row = joined;
      }
    }
    if (!row.empty()) {
      rows.push_back(std::move(row));
    }
  }
  // What does not fit in view.chrome.hintRows rows is the end of the last,
  // which the line's ellipsis then marks.
  const std::size_t most = std::max<std::size_t>(1, draw_.hintRows);
  while (rows.size() > most) {
    rows[most - 1].append(kSeparator).append(rows.back());
    rows.pop_back();
  }
  return rows;
}

void SlicePresenter::drawChrome(gleditor::FrameContext &ctx,
                                const PresentedChrome &chrome) {
  if (!chrome_ || !chromeFont_) {
    return;
  }
  const auto width  = static_cast<float>(ctx.screenWidth);
  const auto height = static_cast<float>(ctx.screenHeight);
  if (!chromeStamp_ || *chromeStamp_ != chrome.stamp ||
      chromeMetrics_ != metrics_ || chromeBase_ != ctx.chrome ||
      chromeWidth_ != width) {
    chrome_->clear();
    chrome_->setTag(render::tagKindNone, 0);
    // A crumb with no words names nothing: a trail of them is no trail.
    std::string trail;
    for (const auto &crumb : chrome.breadcrumbs) {
      if (!crumb.empty()) {
        trail += trail.empty() ? "Walked: " : " › ";
        trail += crumb;
      }
    }
    const float padX = metrics_.px(chrome.paddingX);
    const float padY = metrics_.px(chrome.paddingY);
    const float line = std::ceil(chromeFont_->metrics().lineHeight);
    const float room = std::max(0.0F, width - (2.0F * padX));
    const auto rows  = hintRowsFitting(chrome.hints, room);
    struct Line {
      std::string_view words;
      glm::vec4 colour;
    };
    std::vector<Line> lines{Line{chrome.notice, theme_.colours.accent},
                            Line{trail, theme_.colours.text},
                            Line{chrome.status, theme_.colours.accent}};
    for (const auto &row : rows) {
      lines.push_back(Line{row, theme_.colours.muted});
    }
    const auto count = std::ranges::count_if(
        lines, [](const Line &each) { return !each.words.empty(); });
    chromeClaim_ =
        0 == count ? 0.0F : (static_cast<float>(count) * line) + (2.0F * padY);
    if (chromeClaim_ > 0.0F) {
      const float bottom = ctx.chrome.bottom;
      const auto surface = ui::rgba(theme_.colours.surface);
      chrome_->addRect(0.0F, bottom, width, chromeClaim_, surface);
      chrome_->addLine(0.0F, bottom + chromeClaim_, width,
                       bottom + chromeClaim_, metrics_.px(draw_.cellBorderPx),
                       ui::rgba(theme_.colours.border));
      float top = bottom + chromeClaim_ - padY;
      for (const auto &each : lines) {
        if (each.words.empty()) {
          continue;
        }
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
    chromeWidth_   = width;
  }
  ctx.chrome.bottom += chromeClaim_;
  if (chromeClaim_ > 0.0F) {
    const glm::mat4 ortho =
        glm::ortho( // NOLINT(readability-suspicious-call-argument)
            0.0F, width, 0.0F, height, -1.0F, 1.0F);
    chrome_->draw(ctx.state, ortho, 1.0F);
  }
}

namespace {

/// Whether the point (@p u, @p v), each in [-1, 1] across the glyph's square
/// and v up, is inside @p glyph. The marks are axis-aligned fills on the
/// canvas's grid because its lines are exact only when horizontal or
/// vertical; the proportions are what tell seven marks apart at 12 pixels.
[[nodiscard]] bool insideGlyph(const CueGlyph glyph, const double u,
                               const double v) noexcept {
  constexpr double kRingHole = 0.45;
  constexpr double kSquare   = 0.85;
  constexpr double kBar      = 0.25;
  constexpr double kDiagonal = 0.3;
  const double au            = std::abs(u);
  const double av            = std::abs(v);
  switch (glyph) {
  case CueGlyph::Circle:
    return (u * u) + (v * v) <= 1.0;
  case CueGlyph::Ring: {
    const double r = (u * u) + (v * v);
    return r <= 1.0 && r >= kRingHole * kRingHole;
  }
  case CueGlyph::Square:
    return au <= kSquare && av <= kSquare;
  case CueGlyph::Diamond:
    return au + av <= 1.0;
  case CueGlyph::Triangle:
    return au <= (1.0 - v) / 2.0;
  case CueGlyph::Plus:
    return au <= kBar || av <= kBar;
  case CueGlyph::Cross:
    return std::abs(au - av) <= kDiagonal;
  }
  return false;
}

/// @p glyph on a square @p size pixels across whose bottom left is
/// (@p left, @p bottom), as the runs of its rows.
void addGlyph(gleditor::Canvas &canvas, const CueGlyph glyph, const float left,
              const float bottom, const float size,
              const std::uint32_t colour) {
  const int cells  = std::max(1, static_cast<int>(std::lround(size)));
  const float step = size / static_cast<float>(cells);
  for (int row = 0; row < cells; ++row) {
    const double v = 1.0 - ((2.0 * (row + 0.5)) / cells);
    int runStart   = -1;
    for (int column = 0; column <= cells; ++column) {
      const double u    = ((2.0 * (column + 0.5)) / cells) - 1.0;
      const bool inside = column < cells && insideGlyph(glyph, u, v);
      if (inside && runStart < 0) {
        runStart = column;
      } else if (!inside && runStart >= 0) {
        canvas.addRect(left + (static_cast<float>(runStart) * step),
                       bottom + size - (static_cast<float>(row + 1) * step),
                       static_cast<float>(column - runStart) * step, step,
                       colour);
        runStart = -1;
      }
    }
  }
}

/// A line of @p thickness along y = @p middle from @p from to @p to, drawn
/// with @p style's pattern in units of the thickness; solid when it has none.
void addDashedRule(gleditor::Canvas &canvas, const ViewDrawConfig &draw,
                   const float from, const float to, const float middle,
                   const float thickness, const DashStyle style,
                   const std::uint32_t colour) {
  const auto pattern = dashPattern(draw, style);
  const float bottom = middle - (thickness / 2.0F);
  if (pattern.empty() ||
      !dashesWithinCap(pattern, thickness, to - from, draw.dashMaxCount)) {
    canvas.addRect(from, bottom, to - from, thickness, colour);
    return;
  }
  float at = from;
  for (std::size_t k = 0; at < to; k = (k + 1) % pattern.size()) {
    const float length = pattern[k] * thickness;
    if (0 == k % 2) {
      canvas.addRect(at, bottom, std::min(length, to - at), thickness, colour);
    }
    at += length;
  }
}

} // namespace

void SlicePresenter::drawCompass(gleditor::FrameContext &ctx,
                                 const PresentedCompass &compass) {
  if (!compass_ || !chromeFont_) {
    return;
  }
  const auto width  = static_cast<float>(ctx.screenWidth);
  const auto height = static_cast<float>(ctx.screenHeight);
  if (!compass.config.visible || compass.arms.empty()) {
    compassDrawn_ = false;
    compassStamp_.reset();
    return;
  }
  // What other contributors claimed last frame counts too: the tab bar is
  // drawn after this and has not claimed its band yet on the frame it appears.
  const gleditor::ScreenInsets clear{
      .top    = std::max(ctx.chrome.top, ctx.settledChrome.top),
      .bottom = std::max(ctx.chrome.bottom, ctx.settledChrome.bottom),
      .left   = std::max(ctx.chrome.left, ctx.settledChrome.left),
      .right  = std::max(ctx.chrome.right, ctx.settledChrome.right)};
  if (!compassStamp_ || *compassStamp_ != compass.stamp ||
      compassMetrics_ != metrics_ || compassBase_ != clear ||
      (compass.config.dodge && compassRevision_ != projectionRevision_)) {
    compass_->clear();
    compass_->setTag(render::tagKindNone, 0);
    const auto &cfg    = compass.config;
    const float glyph  = metrics_.px(cfg.glyphPx);
    const float swatch = metrics_.px(cfg.swatchPx);
    const float margin = metrics_.px(cfg.marginPx);
    const float pad    = metrics_.px(cfg.paddingPx);
    const float stroke = metrics_.px(cfg.strokePx);
    const float line   = std::ceil(chromeFont_->metrics().lineHeight);
    const float row    = std::max(line, glyph);
    const float gap    = pad;

    // A column for the point's name, then what it is drawn with, then what
    // it shows; the widest of each sets the column.
    float pointColumn = 0.0F;
    for (const auto &arm : compass.arms) {
      pointColumn =
          std::max(pointColumn, oneLine(arm.point, chromeFont_, width).widthPx);
    }
    const auto label = [](const CompassArm &arm) {
      if (!arm.bound()) {
        return std::string{"nothing"};
      }
      return arm.group ? std::format("{} ({})", arm.shows, arm.members)
                       : arm.shows;
    };
    float left = clear.left + margin;
    float top  = height - clear.top - margin;
    const float fixed =
        (2.0F * pad) + pointColumn + gap + swatch + gap + glyph + gap;
    const float room  = std::max(0.0F, width - left - margin - fixed);
    float labelColumn = 0.0F;
    for (const auto &arm : compass.arms) {
      labelColumn =
          std::max(labelColumn, oneLine(label(arm), chromeFont_, room).widthPx);
    }
    const float panelW = fixed + labelColumn;
    const auto rows    = static_cast<float>(compass.arms.size());
    const float panelH = (2.0F * pad) + (rows * row) + ((rows - 1.0F) * gap);
    if (cfg.dodge) {
      // The legend is drawn over the scene, so a cell's words under it are
      // hidden. It stays where it is until something is under it, then goes
      // to the corner with the least under it, top left first (M2 gate 2,
      // D2). The corners are the pane's, inside what others have claimed.
      std::vector<ui::Rect> under;
      for (std::size_t i = 0; i < projected_.size() && i < drawn_.size(); ++i) {
        if (projected_[i] && !drawnGhost_[i] &&
            ContentMode::None != drawnContent_[i]) {
          under.push_back(projected_[i]->bounds);
        }
      }
      const ui::Rect area{clear.left + margin, clear.bottom + margin,
                          width - clear.left - clear.right - (2.0F * margin),
                          height - clear.top - clear.bottom - (2.0F * margin)};
      compassCorner_ =
          chooseCompassCorner(under, area, {panelW, panelH}, compassCorner_);
      const auto origin =
          compassCornerOrigin(compassCorner_, area, {panelW, panelH});
      left = origin.x;
      top  = origin.y;
    }
    const float bottom = top - panelH;

    const auto surface = ui::rgba(theme_.colours.surface);
    const auto border  = ui::rgba(theme_.colours.border);
    const auto ink     = ui::rgba(theme_.colours.text);
    const auto muted   = ui::rgba(theme_.colours.muted);
    const float hair   = metrics_.px(draw_.cellBorderPx);
    compass_->addRect(left, bottom, panelW, panelH, surface);
    compass_->addRect(left, bottom, panelW, hair, border);
    compass_->addRect(left, top - hair, panelW, hair, border);
    compass_->addRect(left, bottom, hair, panelH, border);
    compass_->addRect(left + panelW - hair, bottom, hair, panelH, border);

    float rowTop = top - pad;
    for (const auto &arm : compass.arms) {
      const float rowBottom = rowTop - row;
      const float middle    = rowBottom + (row / 2.0F);
      float x               = left + pad;
      compass_->addText(
          ctx.state, {x, middle - (line / 2.0F), pointColumn, line},
          oneLine(arm.point, chromeFont_, pointColumn), ink, surface);
      x += pointColumn + gap;
      // Colour is never the only cue: the dash and the glyph repeat it, and an
      // empty point is drawn dotted and hollow.
      const auto colour = arm.cue ? arm.cue->colour : muted;
      const auto dash   = arm.cue ? arm.cue->dash : DashStyle::Dot;
      const auto mark   = arm.cue ? arm.cue->glyph : CueGlyph::Ring;
      addDashedRule(*compass_, draw_, x, x + swatch, middle, stroke, dash,
                    colour);
      x += swatch + gap;
      addGlyph(*compass_, mark, x, middle - (glyph / 2.0F), glyph, colour);
      x += glyph + gap;
      compass_->addText(ctx.state,
                        {x, middle - (line / 2.0F), labelColumn, line},
                        oneLine(label(arm), chromeFont_, labelColumn),
                        arm.bound() ? ink : muted, surface);
      rowTop = rowBottom - gap;
    }
    compass_->commit();
    compassStamp_    = compass.stamp;
    compassMetrics_  = metrics_;
    compassBase_     = clear;
    compassRevision_ = projectionRevision_;
    compassDrawn_    = true;
  }
  if (compassDrawn_) {
    const glm::mat4 ortho =
        glm::ortho( // NOLINT(readability-suspicious-call-argument)
            0.0F, width, 0.0F, height, -1.0F, 1.0F);
    compass_->draw(ctx.state, ortho, 1.0F);
  }
}

std::shared_ptr<const render::PickSemanticTarget>
SlicePresenter::targetFor(const zigzag::CellRef cell,
                          const PresentedPlacement &placement) {
  if (targetsDocument_ != placement.documentId ||
      targetsVersion_ != placement.version) {
    targetsByCell_.clear();
    targetsDocument_ = placement.documentId;
    targetsVersion_  = placement.version;
  }
  auto &held = targetsByCell_[cell];
  if (!held) {
    held = std::make_shared<const render::PickSemanticTarget>(
        render::PickSemanticTarget{
            .documentId   = std::string(placement.documentId),
            .microversion = std::string(placement.version),
            .cellRef      = cell});
  }
  return held;
}

std::optional<SubjectId>
SlicePresenter::subjectOf(const render::PickingResult &pick) const {
  if (render::tagKindOverlay != pick.tag.kind ||
      pick.tag.docIndex != pickScope_ || !pick.semanticTarget) {
    return std::nullopt;
  }
  // The item the tag names, if it is still the cell the pick landed on: a
  // pick requested before the layout changed names a slot that now holds
  // another cell, or none, and is dropped.
  if (pick.tag.clusterIndex >= kFirstTag) {
    const auto index = pick.tag.clusterIndex - kFirstTag;
    if (index < targets_.size() && targets_[index] == pick.semanticTarget) {
      return drawn_[index].id;
    }
  }
  GLEDITOR_LOG_DEBUG(kCategory, "pick from an earlier layout dropped");
  return std::nullopt;
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

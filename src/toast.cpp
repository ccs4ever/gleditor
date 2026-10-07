/**
 * @file toast.cpp
 * @brief Implementation of the transient notification overlay.
 */
#include <gleditor/toast.hpp> // IWYU pragma: associated

#include <choreograph/Choreograph.h> // for easeInOutQuad
#include <gleditor/animation.hpp>    // for toastFade

#include <algorithm>
#include <cmath>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <gleditor/canvas.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/text/fit.hpp>
#include <gleditor/text/font.hpp>
#include <gleditor/text/shaping_cache.hpp>
#include <glm/ext/matrix_clip_space.hpp>

namespace {

/// Panel colour per severity. The text stays near-white on all three; the
/// panel is what says how much the message matters, which reads at a glance
/// where a colour difference in eight-point text does not.
std::uint32_t panelColour(const render::DiagnosticSeverity severity,
                          const gleditor::ui::Theme &theme) {
  switch (severity) {
  case render::DiagnosticSeverity::Info:
    return gleditor::ui::rgba(theme.colours.surface);
  case render::DiagnosticSeverity::Warning:
    return 0x7A5610FFU;
  case render::DiagnosticSeverity::Error:
    return 0x7A2020FFU;
  }
  return gleditor::ui::rgba(theme.colours.surface);
}

} // namespace

ToastOverlay::ToastOverlay(render::RenderDevice *aDevice, std::string aFontName)
    : device(aDevice), fontName(std::move(aFontName)),
      shaping(std::make_unique<gleditor::text::ShapingCache>()) {}

ToastOverlay::~ToastOverlay() = default;

void ToastOverlay::createPipeline(const render::PipelineDesc &documentDesc) {
  pipeline = documentDesc;
  dirty    = true;
}

void ToastOverlay::dropOldest() {
  if (toasts.empty()) {
    return;
  }
  toasts.erase(toasts.begin());
  if (toasts.empty()) layoutResult = {};
  dirty = true;
  ++geometryRevision;
}

void ToastOverlay::post(const render::DiagnosticSeverity severity,
                        const std::string_view message, RenderState &) {
  if (message.empty()) return;
  while (toasts.size() >= maxVisible) {
    dropOldest();
  }

  Toast toast;
  toast.postedAt  = Clock::now();
  toast.expiresAt = toast.postedAt + lifetime;
  toast.message   = message;
  toast.severity  = severity;
  toast.serial    = ++posted;
  toasts.push_back(std::move(toast));
  dirty = true;
  ++geometryRevision;
}

void ToastOverlay::setPresentation(const gleditor::ui::UiMetrics &nextMetrics,
                                   const gleditor::ui::Theme &nextTheme,
                                   const std::uint16_t nextMaxLines,
                                   const float nextMaxWidthShare) {
  const auto lines = std::max<std::uint16_t>(1, nextMaxLines);
  const auto share = std::isfinite(nextMaxWidthShare)
                         ? std::clamp(nextMaxWidthShare, 0.0F, 1.0F)
                         : 0.75F;
  if (metrics != nextMetrics || theme != nextTheme || maxLines != lines ||
      maxWidthShare != share) {
    metrics       = nextMetrics;
    theme         = nextTheme;
    maxLines      = lines;
    maxWidthShare = share;
    dirty         = true;
  }
}

void ToastOverlay::rebuild(RenderState &state) {
  using namespace gleditor;
  const auto safe        = metrics.pixelSafeArea();
  const auto description = ui::scaledFontDescription(
      fontName, ui::FontRole::Caption, metrics, theme);
  const auto font  = text::FontManager::instance().getFont(description);
  const auto em    = font->metrics().lineHeight;
  const auto gap   = std::max(0.0F, theme.gapEm * em);
  const auto count = static_cast<float>(toasts.size());
  const auto rowHeight =
      count > 0.0F ? std::max(0.0F, safe.height - gap * (count - 1)) / count
                   : 0.0F;
  const auto padding    = std::min({std::max(0.0F, theme.paddingEm * em),
                                    rowHeight * 0.5F, safe.width * 0.5F});
  const auto width      = safe.width * maxWidthShare;
  const auto textWidth  = std::max(0.0F, width - 2.0F * padding);
  const auto textHeight = std::max(0.0F, rowHeight - 2.0F * padding);
  std::vector<ui::LayoutItem> items;
  std::vector<text::FittedText> labels;
  for (std::size_t index = 0; index < toasts.size(); ++index) {
    auto fitted = text::fit(toasts[index].message, font,
                            {.maxWidthPx  = std::max(1.0F, textWidth),
                             .maxHeightPx = std::max(1.0F, textHeight),
                             .maxLines    = maxLines,
                             .overflow    = text::Overflow::Wrap},
                            shaping.get());
    items.push_back(
        {.id        = static_cast<std::uint32_t>(index + 1),
         .intrinsic = {std::min(width, fitted.widthPx + 2 * padding),
                       std::min(rowHeight, fitted.heightPx + 2 * padding)},
         .paddingPx = padding});
    labels.push_back(std::move(fitted));
  }
  layoutResult = ui::stack(
      safe, items,
      {.gap = gap, .align = ui::Align::Start, .justify = ui::Justify::End});
  for (std::size_t index = 0; index < toasts.size(); ++index) {
    auto &toast     = toasts[index];
    const auto &box = layoutResult.boxes[index];
    toast.bounds    = box.rect;
    toast.canvas    = std::make_unique<Canvas>(device, description);
    toast.canvas->createPipeline(*pipeline, false);
    toast.canvas->pushClip(safe);
    const auto panel = panelColour(toast.severity, theme);
    toast.canvas->addRect(box.rect.left, box.rect.bottom, box.rect.width,
                          box.rect.height, panel);
    toast.canvas->addText(state, box.contentRect, labels[index],
                          ui::rgba(theme.colours.text), panel);
    toast.canvas->popClip();
    toast.canvas->commit();
  }
  state.glyphCache.flush();
  dirty = false;
  ++geometryRevision;
}

void ToastOverlay::describe(gleditor::a11y::Builder &into) {
  namespace a11y = gleditor::a11y;
  if (toasts.empty()) {
    // No node at all rather than an empty one. A log that is there but says
    // nothing is something for an assistive technology to land on while moving
    // through the window, and there is nothing in the corner of the screen for
    // it to be landing on.
    return;
  }

  auto &log = into.add(0, a11y::Role::Log);
  log.label = "notifications";
  // Announced when there is a pause rather than at once: a notification is by
  // definition the thing that must not interrupt. An error is the exception --
  // it is what the strict runs stop for.
  log.live = a11y::Live::Polite;
  for (const auto &toast : toasts) {
    // The serial rather than the position, so that a message keeps its
    // identity as older ones expire from under it -- and so that the same
    // words posted twice are two notifications and are announced twice.
    log.children.push_back(into.id(toast.serial));
  }
  into.contribute(into.id(0));

  for (const auto &toast : toasts) {
    auto &node         = into.add(toast.serial, a11y::Role::Label);
    node.label         = toast.message;
    node.value         = toast.message;
    const auto &bounds = toast.bounds;
    node.bounds =
        a11y::Rect{bounds.left,
                   static_cast<double>(metrics.screenHeight) - bounds.bottom -
                       bounds.height,
                   bounds.left + bounds.width,
                   static_cast<double>(metrics.screenHeight) - bounds.bottom};
    node.live = render::DiagnosticSeverity::Error == toast.severity
                    ? a11y::Live::Assertive
                    : a11y::Live::Polite;
  }
}

std::uint64_t ToastOverlay::accessibilityRevision() const {
  // Posting, expiry and responsive re-layout all change the accessible tree.
  return geometryRevision;
}

float ToastOverlay::fadeFactor(const Clock::time_point postedAt,
                               const Clock::time_point expiresAt,
                               const Clock::time_point now) {
  const auto seconds = [](const Clock::duration dur) {
    return std::chrono::duration<double>(dur).count();
  };
  // A toast dropped early to make room for a newer one can be asked about
  // after its expiry, and one can be posted with a lifetime shorter than two
  // fades; neither should produce an alpha outside [0, 1].
  if (now <= postedAt) {
    return 0.0F;
  }
  if (now >= expiresAt) {
    return 0.0F;
  }
  const double fade =
      std::min(gleditor::anim::toastFade, seconds(expiresAt - postedAt) / 2.0);
  if (fade <= 0.0) {
    return 1.0F;
  }
  const double in  = seconds(now - postedAt) / fade;
  const double out = seconds(expiresAt - now) / fade;
  const auto ramp  = static_cast<float>(std::min({in, out, 1.0}));
  return ch::easeInOutQuad(ramp);
}

bool ToastOverlay::fadingIn(const Clock::time_point now) const {
  return std::ranges::any_of(toasts, [now](const Toast &toast) {
    return now > toast.postedAt &&
           now < toast.postedAt + std::chrono::duration_cast<Clock::duration>(
                                      std::chrono::duration<double>(
                                          gleditor::anim::toastFade));
  });
}

void ToastOverlay::expire(const Clock::time_point now) {
  while (!toasts.empty() && toasts.front().expiresAt <= now) {
    dropOldest();
  }
}

void ToastOverlay::draw(RenderState &state, const int screenWidth,
                        const int screenHeight) {
  if (toasts.empty() || !pipeline) {
    return;
  }
  if (metrics.screenWidth != screenWidth ||
      metrics.screenHeight != screenHeight) {
    metrics.screenWidth  = screenWidth;
    metrics.screenHeight = screenHeight;
    dirty                = true;
  }
  if (dirty) rebuild(state);

  // Pixel coordinates with Y running up, so the corner offsets the vertex
  // stage derives point the same way they do in document space and the
  // bottom-up glyph atlas is sampled the right way round.
  const glm::mat4 projection =
      glm::ortho(0.0F, static_cast<float>(screenWidth), 0.0F,
                 static_cast<float>(screenHeight));

  // Newest nearest the corner, older ones stacked above it.
  const auto now = Clock::now();
  for (const auto &toast : std::ranges::reverse_view(toasts)) {
    toast.canvas->draw(state, projection,
                       fadeFactor(toast.postedAt, toast.expiresAt, now));
  }
}

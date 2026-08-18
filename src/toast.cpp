/**
 * @file toast.cpp
 * @brief Implementation of the transient notification overlay.
 */
#include <gleditor/toast.hpp> // IWYU pragma: associated

#include <choreograph/Choreograph.h> // for easeInOutQuad
#include <gleditor/animation.hpp>    // for toastFade

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/glyphcache/cache.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/text/font.hpp>
#include <gleditor/text/layout.hpp>

namespace {

/// Widest a toast is allowed to get. Longer text is ellipsised rather than
/// running off the window, which also bounds the panel dimensions written into
/// the 14-bit width field of Doc::VBORow::layerWidthHeight.
constexpr int maxTextWidthPx = 720;

/// Panel colour per severity. The text stays near-white on all three; the
/// panel is what says how much the message matters, which reads at a glance
/// where a colour difference in eight-point text does not.
unsigned int panelColour(const render::DiagnosticSeverity severity) {
  switch (severity) {
  case render::DiagnosticSeverity::Info:
    return Doc::VBORow::color3(38, 42, 54);
  case render::DiagnosticSeverity::Warning:
    return Doc::VBORow::color3(122, 86, 16);
  case render::DiagnosticSeverity::Error:
    return Doc::VBORow::color3(122, 32, 32);
  }
  return Doc::VBORow::color3(38, 42, 54);
}

/// View a row vector as the raw bytes the buffer pool wants.
std::span<const std::byte> asBytes(const std::vector<Doc::VBORow> &rows) {
  return {reinterpret_cast<const std::byte *>(rows.data()),
          rows.size() * sizeof(Doc::VBORow)};
}

/// Copy a matrix into the flat array the device uniform structs carry.
std::array<float, 16> toArray(const glm::mat4 &mat) {
  std::array<float, 16> out{};
  const auto *src = glm::value_ptr(mat);
  std::copy_n(src, out.size(), out.begin());
  return out;
}

} // namespace

ToastOverlay::ToastOverlay(render::RenderDevice *aDevice, std::string aFontName)
    : device(aDevice), fontName(std::move(aFontName)),
      pool(std::make_unique<BufferPool>(aDevice, sizeof(Doc::VBORow),
                                        initialPoolRows)) {}

ToastOverlay::~ToastOverlay() = default;

void ToastOverlay::createPipeline(const render::PipelineDesc &documentDesc) {
  render::PipelineDesc desc = documentDesc;
  desc.name                 = "toast";
  // The overlay is drawn last and must land on top of whatever the documents
  // put in the depth buffer. See PipelineDesc::depthTest.
  desc.depthTest = false;
  pipeline       = device->createPipeline(desc);
}

void ToastOverlay::dropOldest() {
  if (toasts.empty()) {
    return;
  }
  pool->release(toasts.front().backing);
  toasts.erase(toasts.begin());
}

void ToastOverlay::post(const render::DiagnosticSeverity severity,
                        const std::string_view message, RenderState &state) {
  auto font = gleditor::text::FontManager::instance().getFont(fontName);
  if (!font) {
    return;
  }

  gleditor::text::LayoutOptions opts{
      .maxWidthPx      = static_cast<float>(maxTextWidthPx),
      .maxHeightPx     = 0.0F,
      .singleParagraph = true,
      .ellipsize       = true,
  };
  auto shaping =
      gleditor::text::TextLayout::layoutSingleLine(message, font, opts);
  if (shaping.textWidthPx <= 0 || shaping.textHeightPx <= 0) {
    return;
  }

  const auto textWidth   = shaping.textWidthPx;
  const auto textHeight  = shaping.textHeightPx;
  const auto panelWidth  = static_cast<float>(textWidth) + (2 * padding);
  const auto panelHeight = static_cast<float>(textHeight) + (2 * padding);

  const auto panel = panelColour(severity);
  const auto text  = Doc::VBORow::color(236);

  std::vector<Doc::VBORow> rows;
  rows.push_back(Doc::VBORow{
      {panelWidth / 2.0F, panelHeight / 2.0F},
      Doc::VBORow::fill(panel, Doc::VBORow::onPaper),
      0,
      Doc::VBORow::box(0, static_cast<unsigned int>(panelWidth),
                       static_cast<unsigned int>(panelHeight),
                       render::tagKindOverlay),
      Doc::VBORow::paperAt(panel, 0)});

  rows.reserve(shaping.glyphs.size() + 1);
  for (const auto &g : shaping.glyphs) {
    const auto glyph  = state.glyphCache.put(g.chr, font);
    const auto width  = static_cast<float>(static_cast<int>(glyph.dims.width));
    const auto height = static_cast<float>(static_cast<int>(glyph.dims.height));
    if (0.0F == width || 0.0F == height) {
      continue;
    }

    const auto left = padding + g.clusterLeft;
    const auto top  = padding + static_cast<float>(textHeight) - g.clusterTop;

    rows.push_back(
        Doc::VBORow{{left + (width / 2.0F), top - (height / 2.0F)},
                    Doc::VBORow::ink(text, Doc::VBORow::onPaper, false),
                    Doc::VBORow::atlasAt(
                        static_cast<unsigned int>(glyph.texCoords.topLeft.x),
                        static_cast<unsigned int>(glyph.texCoords.topLeft.y)),
                    Doc::VBORow::box(static_cast<unsigned char>(glyph.layer),
                                     static_cast<unsigned int>(width),
                                     static_cast<unsigned int>(height),
                                     render::tagKindOverlay),
                    Doc::VBORow::paperAt(panel, 0)});
  }

  while (toasts.size() >= maxVisible) {
    dropOldest();
  }

  Toast toast;
  toast.instanceCount = static_cast<std::uint32_t>(rows.size());
  toast.backing       = pool->reserve(toast.instanceCount);
  toast.width         = panelWidth;
  toast.height        = panelHeight;
  toast.postedAt      = Clock::now();
  toast.expiresAt     = toast.postedAt + lifetime;
  toast.message       = message;
  toast.severity      = severity;
  toast.serial        = ++posted;
  pool->write(toast.backing, 0, asBytes(rows));
  toasts.push_back(toast);
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
    auto &node = into.add(toast.serial, a11y::Role::Label);
    node.value = toast.message;
    node.live  = render::DiagnosticSeverity::Error == toast.severity
                     ? a11y::Live::Assertive
                     : a11y::Live::Polite;
  }
}

std::uint64_t ToastOverlay::accessibilityRevision() const {
  // What has been said, and what is still saying it: posting bumps the first
  // and expiring changes the second, and nothing else about a toast moves.
  return (posted * 1000U) + toasts.size();
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
  if (toasts.empty() || !pipeline.valid()) {
    return;
  }

  // Pixel coordinates with Y running up, so the corner offsets the vertex
  // stage derives point the same way they do in document space and the
  // bottom-up glyph atlas is sampled the right way round.
  const glm::mat4 projection =
      glm::ortho(0.0F, static_cast<float>(screenWidth), 0.0F,
                 static_cast<float>(screenHeight));

  state.device->bindPipeline(pipeline);
  state.device->bindGlyphTexture(state.glyphCache.textureHandle());

  // Newest nearest the corner, older ones stacked above it.
  const auto now = Clock::now();
  float penY     = marginY;
  for (const auto &toast : std::ranges::reverse_view(toasts)) {
    const glm::mat4 model =
        glm::translate(glm::mat4(1.0F), glm::vec3(marginX, penY, 0.0F));
    const render::DrawUniforms uniforms{
        toArray(projection * model),
        fadeFactor(toast.postedAt, toast.expiresAt, now)};
    state.device->drawGlyphs(uniforms, pool->buffer(),
                             pool->byteOffset(toast.backing),
                             toast.instanceCount);
    penY += toast.height + gap;
  }
}

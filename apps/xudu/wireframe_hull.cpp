/**
 * @file wireframe_hull.cpp
 * @brief Implementation of progressive 3D streaming wireframe hull and swarm
 * telemetry.
 */
#include "wireframe_hull.hpp"
#include "world_card_presentation.hpp"
#include <chrono>

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <gleditor/doc.hpp>
#include <gleditor/render_state.hpp>

namespace xudu {
struct WireframeHullOverlay::Presentation {
  struct Entry {
    std::string key, title, status;
    std::optional<float> fraction;
    std::optional<std::size_t> document;
    world_cards::Card card;
    glm::mat4 matrix{1};
    gleditor::ui::Rect bounds;
    std::uint32_t id{};
    std::uint32_t fetched{}, total{};
    float opacity{1};
    bool retiring{}, deviceReady{}, visible{};
  };
  std::vector<std::unique_ptr<Entry>> entries;
  WorldCardConfig config{260, 46};
  render::RenderDevice *device{};
  render::PipelineDesc pipeline;
  gleditor::ui::Size viewport;
  std::uint32_t next{1};
  std::uint64_t revision{1}, drawnRevision{};
  gleditor::ui::Theme drawnTheme;
  std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};

WireframeHullOverlay::WireframeHullOverlay(RendererRef renderer,
                                           std::string fontName)
    : presentation_(std::make_unique<Presentation>()),
      renderer_(std::move(renderer)), fontName_(std::move(fontName)) {}

WireframeHullOverlay::~WireframeHullOverlay() = default;

void WireframeHullOverlay::deviceReady(render::RenderDevice &device,
                                       const render::PipelineDesc &pipeline) {
  canvas_ = std::make_unique<gleditor::Canvas>(
      &device, fontName_.empty() ? "Sans 12" : fontName_);
  canvas_->createPipeline(pipeline, false);
  presentation_->device        = &device;
  presentation_->pipeline      = pipeline;
  presentation_->drawnRevision = 0;
  for (auto &entry : presentation_->entries) entry->deviceReady = false;
}

bool WireframeHullOverlay::busy() const { return !dissolvingHulls_.empty(); }

void WireframeHullOverlay::startLoading(const std::size_t docIndex,
                                        std::string title, std::string infoHash,
                                        const std::uint32_t totalPieces) {
  for (auto &ld : loadingDocs_) {
    if (ld.docIndex == docIndex) {
      ld.title         = std::move(title);
      ld.infoHash      = std::move(infoHash);
      ld.totalPieces   = (totalPieces > 0 ? totalPieces : 1U);
      ld.piecesFetched = 0;
      return;
    }
  }

  loadingDocs_.push_back(WireframeProgress{
      .docIndex      = docIndex,
      .title         = std::move(title),
      .infoHash      = std::move(infoHash),
      .piecesFetched = 0,
      .totalPieces   = (totalPieces > 0 ? totalPieces : 1U),
      .shimmerPhase  = 0.0F,
  });
}

void WireframeHullOverlay::updateProgress(const std::size_t docIndex,
                                          const std::uint32_t piecesFetched) {
  for (auto &ld : loadingDocs_) {
    if (ld.docIndex == docIndex) {
      ld.piecesFetched = std::min(piecesFetched, ld.totalPieces);
      if (ld.isComplete()) {
        finishLoading(docIndex);
      }
      return;
    }
  }
}

void WireframeHullOverlay::finishLoading(const std::size_t docIndex) {
  for (auto it = loadingDocs_.begin(); it != loadingDocs_.end(); ++it) {
    if (it->docIndex == docIndex) {
      for (auto &entry : presentation_->entries)
        if (entry->document == docIndex) entry->retiring = true;
      loadingDocs_.erase(it);
      dissolvingHulls_.push_back(
          DissolvingHull{.docIndex = docIndex, .opacity = 1.0F});
      return;
    }
  }
}

bool WireframeHullOverlay::isLoading(
    const std::size_t docIndex) const noexcept {
  return std::ranges::any_of(loadingDocs_, [docIndex](const auto &ld) {
    return ld.docIndex == docIndex;
  });
}

void WireframeHullOverlay::setConfig(const WorldCardConfig &config) {
  presentation_->config = config;
}
void WireframeHullOverlay::setTelemetry(std::string key, std::string title,
                                        std::string status,
                                        std::optional<float> fraction) {
  if (fraction && !std::isfinite(*fraction))
    throw std::invalid_argument("Invalid telemetry progress");
  auto &p    = *presentation_;
  auto found = std::ranges::find_if(
      p.entries, [&](const auto &entry) { return entry->key == key; });
  if (found == p.entries.end()) {
    auto entry = std::make_unique<Presentation::Entry>();
    entry->key = std::move(key);
    entry->id  = p.next++;
    p.entries.push_back(std::move(entry));
    found = std::prev(p.entries.end());
  }
  auto &entry = **found;
  if (entry.title != title || entry.status != status ||
      entry.fraction != fraction)
    ++p.revision;
  entry.title  = std::move(title);
  entry.status = std::move(status);
  entry.fraction =
      fraction ? std::optional{std::clamp(*fraction, 0.F, 1.F)} : std::nullopt;
}
void WireframeHullOverlay::clearTelemetry(std::string_view key) {
  const auto removed =
      std::erase_if(presentation_->entries, [&](const auto &entry) {
        return entry->key == key && !entry->document;
      });
  if (removed) ++presentation_->revision;
}
std::uint64_t WireframeHullOverlay::accessibilityRevision() const {
  auto revision = presentation_->revision;
  for (const auto &entry : presentation_->entries)
    revision += entry->card.revision;
  return revision;
}
bool WireframeHullOverlay::performAction(std::uint64_t, gleditor::a11y::Action,
                                         std::string_view) {
  return false;
}
std::vector<std::shared_ptr<const gleditor::ui::WidgetScene>>
WireframeHullOverlay::snapshots() const {
  std::vector<std::shared_ptr<const gleditor::ui::WidgetScene>> result;
  for (const auto &entry : presentation_->entries)
    if (entry->visible && entry->card.panel.snapshot())
      result.push_back(entry->card.panel.snapshot());
  return result;
}
void WireframeHullOverlay::describe(gleditor::a11y::Builder &into) {
  for (const auto &entry : presentation_->entries)
    if (entry->visible)
      world_cards::describe(into, entry->card.panel, entry->matrix,
                            presentation_->viewport, entry->id, entry->title,
                            entry->status);
}
void WireframeHullOverlay::drawFrame(gleditor::FrameContext &ctx) {
  if (!canvas_) return;
  auto &p              = *presentation_;
  auto metrics         = ctx.metrics;
  metrics.screenWidth  = ctx.screenWidth;
  metrics.screenHeight = ctx.screenHeight;
  metrics.chrome       = ctx.chrome;
  p.viewport           = {static_cast<float>(ctx.screenWidth),
                          static_cast<float>(ctx.screenHeight)};
  const auto now       = std::chrono::steady_clock::now();
  const auto dt =
      std::clamp(std::chrono::duration<float>(now - p.last).count(), 0.F, .1F);
  p.last                            = now;
  constexpr float dissolvePerSecond = 3.F;
  for (auto &entry : p.entries)
    if (entry->retiring) {
      entry->opacity = std::max(0.F, entry->opacity - dt * dissolvePerSecond);
      ++p.revision;
    }
  std::erase_if(p.entries, [](const auto &entry) {
    return entry->retiring && entry->opacity <= 0;
  });
  for (auto &hull : dissolvingHulls_) hull.opacity -= dt * dissolvePerSecond;
  std::erase_if(dissolvingHulls_,
                [](const auto &hull) { return hull.opacity <= 0; });
  for (const auto &load : loadingDocs_) {
    auto found = std::ranges::find_if(p.entries, [&](const auto &entry) {
      return entry->document == load.docIndex;
    });
    if (found == p.entries.end()) {
      auto entry      = std::make_unique<Presentation::Entry>();
      entry->document = load.docIndex;
      entry->id       = p.next++;
      entry->title    = load.title;
      p.entries.push_back(std::move(entry));
      found = std::prev(p.entries.end());
    }
    auto &entry = **found;
    if (entry.fetched != load.piecesFetched ||
        entry.total != load.totalPieces || entry.title != load.title) {
      entry.title    = load.title;
      entry.status   = load.progressStatusText();
      entry.fraction = load.progressFraction();
      entry.fetched  = load.piecesFetched;
      entry.total    = load.totalPieces;
      ++p.revision;
    }
  }
  if (p.entries.empty()) return;
  for (auto &entry : p.entries) {
    if (!entry->deviceReady) {
      entry->card.panel.deviceReady(*p.device, p.pipeline, false);
      entry->deviceReady = true;
    }
    entry->card.configure(metrics, ctx.theme, p.config, fontName_);
    entry->card.content(entry->title, entry->status);
    const auto previousCard = entry->card.revision;
    entry->card.prepare();
    if (previousCard != entry->card.revision) ++p.revision;
    glm::vec2 center{p.viewport.width * .5F, p.viewport.height * .5F};
    if (entry->document && *entry->document < ctx.state.docs.size() &&
        ctx.state.docs[*entry->document]) {
      const auto mvp =
          ctx.viewProjection * ctx.state.docs[*entry->document]->modelMatrix();
      const auto clip = mvp * glm::vec4(0, 0, 0, 1);
      if (clip.w <= 0 || clip.z < -clip.w || clip.z > clip.w) {
        if (entry->visible) ++p.revision;
        entry->visible = false;
        continue;
      }
      center = {(clip.x / clip.w * .5F + .5F) * p.viewport.width,
                (clip.y / clip.w * .5F + .5F) * p.viewport.height};
    }
    const auto size   = entry->card.presentation.size;
    const auto bounds = gleditor::ui::clampToSafeArea(
        {center.x - size.width * .5F, center.y - size.height * .5F, size.width,
         size.height},
        metrics.pixelSafeArea());
    const auto matrix = world_cards::screenMatrix(bounds, p.viewport);
    if (entry->matrix != matrix) ++p.revision;
    if (!entry->visible) ++p.revision;
    entry->visible = true;
    entry->matrix  = matrix;
    entry->bounds  = bounds;
    entry->card.panel.draw(ctx.state, entry->matrix, p.viewport, {},
                           entry->opacity);
  }
  if (p.drawnRevision != p.revision || p.drawnTheme != ctx.theme) {
    canvas_->clear();
    canvas_->setTag(render::tagKindNone, 0);
    for (const auto &entry : p.entries) {
      if (!entry->visible) continue;
      const auto &r      = entry->bounds;
      const auto padding = entry->card.presentation.padding;
      auto accent        = ctx.theme.colours.accent;
      accent.a *= entry->opacity;
      const auto colour = gleditor::ui::rgba(accent);
      const auto stroke = std::max(1.F, padding * .25F);
      canvas_->addLine(r.left, r.bottom, r.left + r.width, r.bottom, stroke,
                       colour);
      canvas_->addLine(r.left + r.width, r.bottom, r.left + r.width,
                       r.bottom + r.height, stroke, colour);
      canvas_->addLine(r.left + r.width, r.bottom + r.height, r.left,
                       r.bottom + r.height, stroke, colour);
      canvas_->addLine(r.left, r.bottom + r.height, r.left, r.bottom, stroke,
                       colour);
      if (entry->fraction) {
        const auto bar = std::max(1.F, padding * .5F);
        auto border    = ctx.theme.colours.border;
        border.a *= entry->opacity;
        const auto track = gleditor::ui::rgba(border);
        canvas_->addRect(r.left + padding, r.bottom + padding,
                         r.width - padding * 2, bar, track);
        canvas_->addRect(r.left + padding, r.bottom + padding,
                         (r.width - padding * 2) * *entry->fraction, bar,
                         colour);
      }
    }
    canvas_->commit();
    p.drawnRevision = p.revision;
    p.drawnTheme    = ctx.theme;
  }
  canvas_->draw(ctx.state, glm::ortho(0.F, p.viewport.width, 0.F,
                                      p.viewport.height, -1.F, 1.F));
}
} // namespace xudu

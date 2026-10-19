/**
 * @file views.cpp
 * @brief Document presentation and views coordinator for Xudu.
 */
#include "views.hpp"
#include "publication_status_form.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include <gleditor/animation.hpp>
#include <gleditor/app.hpp>
#include <gleditor/caret.hpp>
#include <gleditor/doc_switcher.hpp>
#include <gleditor/form.hpp>
#include <gleditor/logging.hpp>
#include <gleditor/media.hpp>
#include <gleditor/media_stream.hpp>
#include <gleditor/media_widget.hpp>
#include <gleditor/mimetype.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/state.hpp>

#include "common/ui/hypertime_graph.hpp"
#include "common/ui/xanadoc/beams.hpp"
#include "common/ui/xanadoc/kinetic_tether_overlay.hpp"
#include "common/ui/xanadoc/overview_overlay.hpp"
#include "common/ui/xanadoc/pouch_drawer.hpp"
#include "common/ui/xanadoc/satelloid.hpp"
#include "common/ui/xanadoc/session.hpp"
#include "common/ui/xanadoc/wireframe_hull.hpp"
#include "common/xanadu/config.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/provenance.hpp"
#include "common/xanadu/spool.hpp"
#include "common/xanadu/swarm_catalog.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/transcopyright_crypto.hpp"
#include "common/xanadu/transcopyright_logic.hpp"

namespace xanadu {

Views::Views(Session &aSession, RendererRef aRenderer, HypertimeMap &aMap,
             ImageOverlay &aImages, gleditor::Form &aForm, AppStateRef aState,
             std::shared_ptr<gleditor::DocumentSwitcher> aSwitcher)
    : session(aSession), renderer(std::move(aRenderer)), map(aMap),
      images(aImages), form(aForm), state(std::move(aState)),
      switcher(std::move(aSwitcher)) {}

Views::~Views() = default;

void Views::editUiFontScale() {
  renderer->runWithState([this](RenderState &) {
    const auto values = getSetting(session.systemStore(SystemDocKind::UI),
                                   settings::kUiFontScale);
    if (values.size() != 1 || !std::holds_alternative<double>(values.front())) {
      renderer->push(
          RenderItemNotification("Could not read the UI font scale setting.",
                                 render::DiagnosticSeverity::Error));
      return;
    }

    gleditor::Form::Field scale;
    scale.label    = "UI font scale";
    scale.value    = std::format("{:.17g}", std::get<double>(values.front()));
    scale.hint     = "positive finite number, e.g. 1.5";
    scale.required = true;
    form.open(
        "UI font scale",
        "Scales application chrome typography. Document text size is separate.",
        {std::move(scale)},
        [this](const std::vector<gleditor::Form::Field> &answers) {
          if (answers.size() != 1) return;
          const auto input = answers.front().answer();
          double nextScale{};
          const auto parsed =
              std::from_chars(input.data(), input.data() + input.size(),
                              nextScale, std::chars_format::general);
          if (input.empty() || parsed.ec != std::errc{} ||
              parsed.ptr != input.data() + input.size() ||
              !std::isfinite(nextScale) || nextScale <= 0.0 ||
              nextScale >
                  static_cast<double>(std::numeric_limits<float>::max()) ||
              nextScale < static_cast<double>(
                              std::numeric_limits<float>::denorm_min()) ||
              !std::isfinite(static_cast<float>(nextScale)) ||
              static_cast<float>(nextScale) <= 0.0F) {
            renderer->push(RenderItemNotification(
                "UI font scale must be a positive finite number representable "
                "by the UI; no setting was changed.",
                render::DiagnosticSeverity::Error));
            return;
          }

          renderer->runWithState([this, nextScale](RenderState &) {
            try {
              auto &uiStore = session.systemStore(SystemDocKind::UI);
              const std::array<CellValue, 1> typedValue{nextScale};
              const auto version = SystemStoreModel::updateSetting(
                  uiStore, uiStore.primaryCurrentVersion(),
                  settings::kUiFontScale, typedValue);
              session.repointSystemDoc(SystemDocKind::UI, version);
            } catch (const std::exception &error) {
              renderer->push(RenderItemNotification(
                  std::string("Could not save UI font scale: ") + error.what(),
                  render::DiagnosticSeverity::Error));
            }
          });
        });
  });
}

void Views::deviceReady(render::RenderDevice &device,
                        const render::PipelineDesc &documentPipeline) {
  device_       = &device;
  documentDesc_ = documentPipeline;
}

void Views::drawFrame(gleditor::FrameContext &ctx) {
  chromeInsets_ = {
      .top    = std::max(ctx.chrome.top, ctx.settledChrome.top),
      .bottom = std::max(ctx.chrome.bottom, ctx.settledChrome.bottom),
      .left   = std::max(ctx.chrome.left, ctx.settledChrome.left),
      .right  = std::max(ctx.chrome.right, ctx.settledChrome.right)};
  bool quotationsReady  = false;
  bool quotationsFailed = false;
  for (const auto &result : session.takeContentRetentionNotifications()) {
    quotationsReady |= result.outcome.has_value();
    quotationsFailed |= !result.outcome.has_value();
  }
  if (quotationsReady)
    renderer->push(RenderItemNotification("Quoted content is ready offline."));
  if (quotationsFailed)
    renderer->push(RenderItemNotification(
        "Draft saved. Offline quotations unavailable. Check local storage, "
        "reopen the source and save again."));
  if (auto *subscriptions = session.activePublicationSubscriptions();
      subscriptions &&
      std::chrono::steady_clock::now() >= nextPublicationNotice_) {
    try {
      for (const auto &[id, notice] : subscriptions->takeNotifications()) {
        nextPublicationNotice_ =
            std::chrono::steady_clock::now() + ToastOverlay::lifetime;
        const auto link = MutableLink::parse(subscriptions->status(id).uri);
        renderer->push(RenderItemNotification(
            "Publication update: " + notice.title + " — " +
            link.key.hex().substr(0, 12) + " #" +
            std::to_string(notice.previousSequence) + " → #" +
            std::to_string(notice.sequence) + ". Review with Ctrl+Shift+U."));
      }
      publicationNoticeError_ = false;
    } catch (const std::exception &error) {
      if (!publicationNoticeError_) {
        renderer->push(RenderItemNotification(
            "Publication notifications unavailable. Review with Ctrl+Shift+U.",
            render::DiagnosticSeverity::Error));
        publicationNoticeError_ = true;
      }
      GLEDITOR_LOG_DEBUG("xudu.publication",
                         "Update notice delivery failed: {}", error.what());
    }
  }
  if (ctx.state.documentsVisible) frameForReading(ctx);
  if (pendingCamera_ && pendingCamera_()) {
    pendingCamera_ = {};
  }
}

void Views::frameForReading(const gleditor::FrameContext &ctx) {
  auto target = frameTarget_.lock();
  if (!target && !readingFramed_ && !ctx.state.docs.empty()) {
    target = ctx.state.docs.front();
  }
  if (!target || readableTextPx_ <= 0.0F) {
    return;
  }
  const auto &doc      = *target;
  const auto frame     = doc.pageFrame(0);
  const auto firstLine = doc.anchorFor(0);
  if (!frame || !firstLine) {
    return;
  }
  const float toWorld = glm::length(glm::vec3(frame->localToWorld[1]));
  std::scoped_lock locker(state->view);
  auto &view          = state->view;
  const auto distance = xanadu::readableCameraDistance(
      firstLine->height * toWorld, static_cast<float>(view.screenHeight),
      view.fov, readableTextPx_);
  if (!distance || view.screenWidth <= 0) {
    return;
  }
  const glm::vec3 topLeft(frame->localToWorld *
                          glm::vec4(frame->leftPx, frame->topPx, 0.0F, 1.0F));
  const glm::vec3 topRight(frame->localToWorld *
                           glm::vec4(frame->rightPx, frame->topPx, 0.0F, 1.0F));
  const float halfH = *distance * std::tan(glm::radians(view.fov) * 0.5F);
  const float halfW = halfH * static_cast<float>(view.screenWidth) /
                      static_cast<float>(view.screenHeight);
  const float x           = topRight.x - topLeft.x <= 2.0F * halfW
                                ? 0.5F * (topLeft.x + topRight.x)
                                : topLeft.x + halfW;
  const float chromeTopPx = std::max(ctx.chrome.top, ctx.settledChrome.top);
  const float worldPerPx = 2.0F * halfH / static_cast<float>(view.screenHeight);
  view.pos       = glm::vec3(x, topLeft.y - halfH + (chromeTopPx * worldPerPx),
                             topLeft.z + *distance);
  readingFramed_ = true;
  frameTarget_.reset();
}

void Views::frameNewestComparison() {
  renderer->runWithState([this](RenderState &rState) {
    if (rState.docs.size() < 2) return;
    const std::weak_ptr<Doc> earlier = rState.docs[rState.docs.size() - 2];
    const std::weak_ptr<Doc> updated = rState.docs.back();
    placeCameraWhenReady([this, earlier, updated] {
      const auto oldDoc = earlier.lock();
      const auto newDoc = updated.lock();
      if (!oldDoc || !newDoc) return true;
      // Shared-content beams first settle the pair. Fitting before their
      // alignment finishes lets its later camera move hide the earlier text.
      if (auto *timeline = renderer->animTimeline();
          timeline && !timeline->empty())
        return false;
      const auto oldFrame = oldDoc->pageFrame(0);
      const auto newFrame = newDoc->pageFrame(0);
      if (!oldFrame || !newFrame) return false;
      if (comparisonCameraReady_ && !comparisonCameraReady_()) return false;
      const auto corner = [](const Doc::PageFrame &frame, float x) {
        return glm::vec3(frame.localToWorld *
                         glm::vec4(x, frame.topPx, 0.0F, 1.0F));
      };
      const auto oldLeft  = corner(*oldFrame, oldFrame->leftPx);
      const auto oldRight = corner(*oldFrame, oldFrame->rightPx);
      const auto newLeft  = corner(*newFrame, newFrame->leftPx);
      const auto newRight = corner(*newFrame, newFrame->rightPx);
      const float left    = std::min(oldLeft.x, newLeft.x);
      const float right   = std::max(oldRight.x, newRight.x);
      std::scoped_lock locker(state->view);
      auto &view = state->view;
      if (view.screenWidth <= 0 || view.screenHeight <= 0) return false;
      const float aspect = static_cast<float>(view.screenWidth) /
                           static_cast<float>(view.screenHeight);
      const float distance =
          xanadu::framingDistance(right - left, 0.0F, view.fov, aspect);
      const float halfH = distance * std::tan(glm::radians(view.fov) * 0.5F);
      const float worldPerPx =
          2.0F * halfH / static_cast<float>(view.screenHeight);
      view.pos = glm::vec3(0.5F * (left + right),
                           std::max(oldLeft.y, newLeft.y) - halfH +
                               chromeInsets_.top * worldPerPx,
                           std::max(oldLeft.z, newLeft.z) + distance);
      return true;
    });
  });
}

void Views::keepInView(const Doc &doc, const std::uint32_t offset) {
  const auto anchor = doc.anchorFor(offset);
  if (!anchor) {
    return;
  }
  const auto found = doc.worldPoint(*anchor);
  if (!found) {
    return;
  }
  const auto point = *found;
  std::scoped_lock locker(state->view);
  auto &view = state->view;
  if (view.screenHeight <= 0 || view.pos.z <= point.z) {
    return;
  }
  constexpr float kComfort = 0.8F;
  const float halfH =
      (view.pos.z - point.z) * std::tan(glm::radians(view.fov) * 0.5F);
  const float halfW = halfH * static_cast<float>(view.screenWidth) /
                      static_cast<float>(view.screenHeight);
  const auto follow = [](float &camera, const float at, const float half) {
    const float reach = half * kComfort;
    if (at > camera + reach) {
      camera = at - reach;
    } else if (at < camera - reach) {
      camera = at + reach;
    }
  };
  follow(view.pos.x, point.x, halfW);
  follow(view.pos.y, point.y, halfH);
}

bool Views::keepRangeInView(const Doc &doc, const std::uint32_t start,
                            const std::uint32_t end) {
  const auto first = doc.anchorFor(start);
  if (!first) {
    GLEDITOR_LOG_DEBUG("xudu.links",
                       "range [{},{}) not ready: first anchor missing", start,
                       end);
    return false;
  }
  GLEDITOR_LOG_DEBUG("xudu.links",
                     "range [{},{}) first anchor ready page={} pageBuilt={} "
                     "local=({:.3f},{:.3f}) height={:.3f}",
                     start, end, first->pageIndex,
                     static_cast<bool>(doc.page(first->pageIndex)), first->x,
                     first->y, first->height);

  struct WorldBounds {
    std::array<glm::vec3, 4> corners{};
  };
  const auto cornersFor =
      [&](const Doc::Anchor &anchor, const float left, const float right,
          const float top, const float bottom) -> std::optional<WorldBounds> {
    const auto frame = doc.pageFrame(anchor.pageIndex);
    if (!frame) return std::nullopt;
    const glm::vec3 basisX(frame->localToWorld[0]);
    const glm::vec3 basisY(frame->localToWorld[1]);
    const glm::vec3 origin(frame->localToWorld[3]);
    GLEDITOR_LOG_DEBUG(
        "xudu.links",
        "pageFrame page={} basisX=({:.7f},{:.7f},{:.7f}) "
        "basisY=({:.7f},{:.7f},{:.7f}) origin=({:.5f},{:.5f},{:.5f})",
        anchor.pageIndex, basisX.x, basisX.y, basisX.z, basisY.x, basisY.y,
        basisY.z, origin.x, origin.y, origin.z);
    WorldBounds bounds;
    const std::array<glm::vec2, 4> local{
        {{left, top}, {right, top}, {right, bottom}, {left, bottom}}};
    for (std::size_t i = 0; i < local.size(); ++i) {
      bounds.corners[i] =
          glm::vec3(frame->localToWorld * glm::vec4(local[i], 0.0F, 1.0F));
    }
    return bounds;
  };
  const auto caretBounds = [&]() -> std::optional<WorldBounds> {
    const float halfWidth  = Caret::widthPixels * 0.5F;
    const float halfHeight = first->height * 0.5F;
    return cornersFor(*first, first->x - halfWidth, first->x + halfWidth,
                      first->y + halfHeight, first->y - halfHeight);
  };

  std::optional<WorldBounds> selectedBounds;
  bool hasExactRangeBounds = false;
  if (end > start) {
    const auto last = doc.anchorFor(end - 1);
    if (!last) {
      GLEDITOR_LOG_DEBUG("xudu.links",
                         "range [{},{}) not ready: last anchor missing", start,
                         end);
      return false;
    }
    GLEDITOR_LOG_DEBUG(
        "xudu.links",
        "range end anchor page={} local=({:.3f},{:.3f}) height={:.3f}",
        last->pageIndex, last->x, last->y, last->height);
    if (first->pageIndex == last->pageIndex) {
      if (const auto page = doc.page(first->pageIndex)) {
        if (const auto text = page->textBoundsFor(start, end)) {
          GLEDITOR_LOG_DEBUG("xudu.links",
                             "range bounds page={} local-x=[{:.3f},{:.3f}] "
                             "y=[{:.3f},{:.3f}] line={:.3f}",
                             first->pageIndex, text->left, text->right,
                             text->band.bottom, text->band.top,
                             text->band.lineHeight);
          selectedBounds      = cornersFor(*first, text->left, text->right,
                                           text->band.top, text->band.bottom);
          hasExactRangeBounds = selectedBounds.has_value();
        } else {
          GLEDITOR_LOG_DEBUG(
              "xudu.links",
              "range [{},{}) bounds unsupported on page={}; use caret", start,
              end, first->pageIndex);
        }
      }
    } else {
      GLEDITOR_LOG_DEBUG("xudu.links",
                         "range [{},{}) crosses pages {}..{}; use caret", start,
                         end, first->pageIndex, last->pageIndex);
    }
  }
  // A multipage, unsupported, or overlarge range keeps its exact beginning
  // readable and leaves the existing scrolling/typing camera policy intact.
  if (!selectedBounds) selectedBounds = caretBounds();
  if (!selectedBounds) return false;
  const auto overview = overviewBounds_ ? overviewBounds_() : std::nullopt;

  std::scoped_lock locker(state->view);
  auto &view = state->view;
  if (view.screenWidth <= 0 || view.screenHeight <= 0) {
    GLEDITOR_LOG_DEBUG("xudu.links",
                       "range [{},{}) camera viewport not ready: {}x{}", start,
                       end, view.screenWidth, view.screenHeight);
    return false;
  }
  const glm::vec3 oldCamera = view.pos;
  const auto readingArea =
      gleditor::ui::UiMetrics{.screenWidth  = view.screenWidth,
                              .screenHeight = view.screenHeight,
                              .chrome       = chromeInsets_,
                              .marginShare  = state->uiSafeMarginShare.load()}
          .pixelSafeArea();
  const float minX       = readingArea.left;
  const float maxX       = readingArea.left + readingArea.width;
  const float chromeMinY = readingArea.bottom;
  const float maxY       = readingArea.bottom + readingArea.height;
  const auto fitBounds   = [&](const WorldBounds &bounds, const float minY,
                             float &cameraX, float &cameraY) {
    const float midX = static_cast<float>(view.screenWidth) * 0.5F;
    const float midY = static_cast<float>(view.screenHeight) * 0.5F;
    float lowerX     = std::numeric_limits<float>::lowest();
    float upperX     = std::numeric_limits<float>::max();
    float lowerY     = std::numeric_limits<float>::lowest();
    float upperY     = std::numeric_limits<float>::max();
    for (const auto &corner : bounds.corners) {
      const float scale = 2.0F * (view.pos.z - corner.z) *
                          std::tan(glm::radians(view.fov) * 0.5F) /
                          static_cast<float>(view.screenHeight);
      if (scale <= 0.0F) return false;
      lowerX = std::max(lowerX, corner.x - (maxX - midX) * scale);
      upperX = std::min(upperX, corner.x - (minX - midX) * scale);
      lowerY = std::max(lowerY, corner.y - (maxY - midY) * scale);
      upperY = std::min(upperY, corner.y - (minY - midY) * scale);
    }
    if (lowerX > upperX || lowerY > upperY) return false;
    cameraX = std::clamp(cameraX, lowerX, upperX);
    cameraY = std::clamp(cameraY, lowerY, upperY);
    return true;
  };
  const auto overlapsOverview = [&](const WorldBounds &bounds,
                                    const float cameraX) {
    if (!overview) return false;
    float projectedLeft  = std::numeric_limits<float>::max();
    float projectedRight = std::numeric_limits<float>::lowest();
    const float midX     = static_cast<float>(view.screenWidth) * 0.5F;
    for (const auto &corner : bounds.corners) {
      const float scale = 2.0F * (view.pos.z - corner.z) *
                          std::tan(glm::radians(view.fov) * 0.5F) /
                          static_cast<float>(view.screenHeight);
      if (scale <= 0.0F) continue;
      const float x  = midX + (corner.x - cameraX) / scale;
      projectedLeft  = std::min(projectedLeft, x);
      projectedRight = std::max(projectedRight, x);
    }
    return projectedRight >= overview->left &&
           projectedLeft <= overview->left + overview->width;
  };
  const auto fitVisible = [&](const WorldBounds &bounds, float &cameraX,
                              float &cameraY) {
    if (!fitBounds(bounds, chromeMinY, cameraX, cameraY)) {
      GLEDITOR_LOG_DEBUG("xudu.links",
                         "range bounds cannot fit chrome rectangle "
                         "x=[{:.1f},{:.1f}] y=[{:.1f},{:.1f}]",
                         minX, maxX, chromeMinY, maxY);
      return false;
    }
    if (overlapsOverview(bounds, cameraX)) {
      const float overviewMinY =
          std::max(chromeMinY, overview->bottom + overview->height);
      GLEDITOR_LOG_DEBUG("xudu.links",
                         "projected range overlaps overview x=[{:.1f},{:.1f}], "
                         "reserve y>={:.1f}",
                         overview->left, overview->left + overview->width,
                         overviewMinY);
      if (!fitBounds(bounds, overviewMinY, cameraX, cameraY)) {
        GLEDITOR_LOG_DEBUG("xudu.links",
                           "range bounds cannot fit overview-safe rectangle");
        return false;
      }
    }
    return true;
  };

  float cameraX        = view.pos.x;
  float cameraY        = view.pos.y;
  const bool rangeFits = fitVisible(*selectedBounds, cameraX, cameraY);
  std::optional<WorldBounds> fallbackBounds;
  if (!rangeFits) {
    GLEDITOR_LOG_DEBUG("xudu.links",
                       "range fit failed; retry first caret rect "
                       "camera=({:.5f},{:.5f},{:.5f})",
                       oldCamera.x, oldCamera.y, oldCamera.z);
    const auto caret = caretBounds();
    fallbackBounds   = caret;
    cameraX          = view.pos.x;
    cameraY          = view.pos.y;
    if (!caret || !fitVisible(*caret, cameraX, cameraY)) {
      if (!caret) return false;
      const float centerX = (minX + maxX) * 0.5F;
      const auto frame    = doc.pageFrame(first->pageIndex);
      if (!frame) return false;
      const glm::vec3 caretCenter(frame->localToWorld *
                                  glm::vec4(first->x, first->y, 0.0F, 1.0F));
      const float scale = 2.0F * (view.pos.z - caretCenter.z) *
                          std::tan(glm::radians(view.fov) * 0.5F) /
                          static_cast<float>(view.screenHeight);
      if (scale <= 0.0F) return false;
      cameraX = caretCenter.x -
                (centerX - static_cast<float>(view.screenWidth) * 0.5F) * scale;
      const float minY =
          overlapsOverview(*caret, cameraX)
              ? std::max(chromeMinY, overview->bottom + overview->height)
              : chromeMinY;
      const float centerY = (minY + maxY) * 0.5F;
      cameraY =
          caretCenter.y -
          (centerY - static_cast<float>(view.screenHeight) * 0.5F) * scale;
    }
  }
  view.pos.x         = cameraX;
  view.pos.y         = cameraY;
  const auto &bounds = rangeFits ? *selectedBounds : *fallbackBounds;
  glm::vec3 worldLo(std::numeric_limits<float>::max());
  glm::vec3 worldHi(std::numeric_limits<float>::lowest());
  for (const auto &corner : bounds.corners) {
    worldLo = glm::min(worldLo, corner);
    worldHi = glm::max(worldHi, corner);
  }
  GLEDITOR_LOG_DEBUG(
      "xudu.links",
      "range framing outcome={} camera "
      "({:.5f},{:.5f},{:.5f})->({:.5f},{:.5f},{:.5f}) worldBounds "
      "x=[{:.5f},{:.5f}] y=[{:.5f},{:.5f}] z=[{:.5f},{:.5f}] "
      "insets=({:.1f},{:.1f},{:.1f},{:.1f}) overview={}",
      rangeFits && hasExactRangeBounds ? "range" : "caret", oldCamera.x,
      oldCamera.y, oldCamera.z, view.pos.x, view.pos.y, view.pos.z, worldLo.x,
      worldHi.x, worldLo.y, worldHi.y, worldLo.z, worldHi.z, chromeInsets_.left,
      chromeInsets_.right, chromeInsets_.bottom, chromeInsets_.top,
      static_cast<bool>(overview));
  return true;
}

std::optional<Doc::Anchor>
Views::widgetRectFor(const Doc &doc, const std::uint32_t docOffset) const {
  for (const auto &widget : mediaWidgets) {
    if (auto rect = widget->rectFor(doc, docOffset)) {
      return rect;
    }
  }
  return std::nullopt;
}

void Views::showOnly(const MicroversionId &version,
                     const std::size_t storeIndex) {
  const auto count = session.views().size();
  for (std::size_t i = 0; i < count; i++) {
    renderer->push(RenderItemCloseDoc());
  }
  renderer->runWithState([this](RenderState &) { session.clearViews(); });
  showAlongside(version, 0.0F, storeIndex);
}

void Views::syncMediaWidgets(RenderState &rState) {
  for (const auto &old : mediaWidgets) {
    renderer->removeFrameContributor(old.get());
    renderer->removePickObserver(old.get());
    state->accessibility->removeSource(old.get());
  }
  mediaWidgets.clear();
  images.clear();
  for (std::size_t dIdx = 0;
       dIdx < rState.docs.size() && dIdx < session.views().size(); ++dIdx) {
    if (!rState.docs[dIdx]) {
      continue;
    }
    const auto &vInfo = session.views()[dIdx];
    const auto &st    = session.store(vInfo.storeIndex);
    const auto spans  = session.mediaSpansFor(vInfo.version, vInfo.storeIndex);
    for (const auto &mSpan : spans) {
      const auto bytes = st.read(xanadu::PrimediaSpan{
          .scroll = mSpan.span.scroll,
          .start  = mSpan.span.start - mSpan.containerOffset,
          .length = mSpan.containerLength});
      if (mSpan.isImage) {
        const auto id = std::format("{}:{}:{}", mSpan.span.scroll,
                                    mSpan.span.start - mSpan.containerOffset,
                                    mSpan.containerLength);
        images.place(rState.docs[dIdx], mSpan.docOffset, id,
                     std::span<const std::uint8_t>(
                         reinterpret_cast<const std::uint8_t *>(bytes.data()),
                         bytes.size()),
                     gleditor::MimeType(mSpan.mime));
        continue;
      }
      auto widget = std::make_shared<gleditor::MediaWidget>();
      if (nullptr != device_) {
        widget->deviceReady(*device_, documentDesc_);
      }
      auto stream = std::make_shared<gleditor::MemoryMediaStream>(bytes);
      std::ignore = widget->loadFragment(
          gleditor::MediaResource::fromStream(stream, mSpan.label),
          gleditor::ByteRange{.start  = mSpan.containerOffset,
                              .length = mSpan.span.length},
          mSpan.containerLength);
      widget->setTitle(mSpan.label);
      widget->attachToDocument(rState.docs[dIdx], mSpan.docOffset);
      widget->setSize(mSpan.widgetWidth, mSpan.widgetHeight);
      widget->setVisible(true);
      renderer->addFrameContributor(widget.get());
      renderer->addPickObserver(widget.get());
      state->accessibility->addSource(widget);
      mediaWidgets.push_back(std::move(widget));
    }
  }
}

void Views::showAlongside(const MicroversionId &version, const float depthZ,
                          const std::size_t storeIndex,
                          const std::uint32_t focusedBirth) {
  renderer->push(RenderItemOpenDoc(
      session.sourceFor(version, storeIndex, focusedBirth), depthZ));
  renderer->runWithState(
      [this, version, storeIndex, focusedBirth](RenderState &rState) {
        if (rState.docs.empty()) {
          return;
        }
        primaryDocument_ = rState.docs.front();
        rState.docs.back()->addObserver(&session);
        session.viewOpened(version, storeIndex, focusedBirth);
        map.setCurrent(session.views().front().version);
        syncMediaWidgets(rState);
      });
}

std::optional<glm::mat4> Views::presentationTransform() const {
  auto document = presentationAnchor_.lock();
  if (!document) {
    document = primaryDocument_.lock();
  }
  if (!document) {
    return std::nullopt;
  }
  const auto frame = document->pageFrame(0);
  if (!frame) {
    return std::nullopt;
  }
  return frame->localToWorld *
         glm::translate(
             glm::mat4{1.0F},
             glm::vec3{frame->rightPx + frame->marginPx, 0.0F, 0.0F});
}

void Views::swingBackToSpan(const PouchItem &item) {
  renderer->runWithState([this, item](RenderState &rState) {
    const auto unavailable = [this](std::string reason) {
      state->showDialog(render::DiagnosticSeverity::Warning,
                        "Quotation source unavailable", std::move(reason));
    };
    if (!item.originSource) {
      unavailable("This quotation has no saved source location.");
      return;
    }
    const auto &source = *item.originSource;
    const auto storeIndex =
        session.storeIndexForAuthority(source.authority, source.version);
    if (!storeIndex) {
      unavailable("Open the original source store to return to this quotation. "
                  "The saved quotation can still be inserted.");
      return;
    }
    const auto &store = session.store(*storeIndex);
    if (!store.segmentedOps().contains(source.version)) {
      unavailable("The saved source version is unavailable.");
      return;
    }
    Version text;
    if (source.cell) {
      const auto slice = store.rebuildManifold(source.version);
      if (!slice.contains(*source.cell) || !cellSourceEntry) {
        unavailable("The saved source cell is unavailable.");
        return;
      }
      const auto content = slice.contentOf(*source.cell);
      text.insertSpans(
          0, std::vector<PrimediaSpan>(content.begin(), content.end()));
    } else {
      text = store.rebuild(source.version, source.focusedBirth);
    }
    if (source.end < source.start) {
      unavailable("The saved source range is invalid.");
      return;
    }
    const auto spans   = text.spansFor(source.start, source.end - source.start);
    const auto &pouch  = session.systemStore(SystemDocKind::Pouches);
    const auto matches = [&](const PrimediaSpan &span) {
      if (span.scroll == localScroll && item.span.scroll == localScroll &&
          span == item.span &&
          &store.userPermascroll() == &pouch.userPermascroll())
        return true;
      const auto a = globalise(store, span);
      const auto b = globalise(pouch, item.span);
      return a && b && *a == *b;
    };
    if (spans.size() != 1 || !matches(spans.front())) {
      unavailable("The saved source range does not match this quotation.");
      return;
    }
    if (sourceLeaving) sourceLeaving();
    if (source.cell) {
      const CellSite site{.store   = store.documentId(),
                          .version = source.version,
                          .cell    = *source.cell,
                          .range   = {source.start, source.end}};
      cellSourceEntry(rState, *storeIndex, site);
      if (sourceEntered) sourceEntered(site);
      return;
    }
    std::optional<std::size_t> docIndex;
    for (std::size_t i = 0; i < session.views().size(); ++i) {
      const auto &view = session.views()[i];
      if (view.storeIndex == *storeIndex && view.version == source.version &&
          view.focusedBirth == source.focusedBirth) {
        docIndex = i;
        break;
      }
    }
    if (!docIndex) {
      docIndex = session.views().size();
      showAlongside(source.version, 0.0F, *storeIndex, source.focusedBirth);
    }
    renderer->runWithState([this, source, storeIndex = *storeIndex,
                            doc = *docIndex](RenderState &rState) {
      if (doc >= rState.docs.size() || !rState.docs[doc] ||
          !renderer->editCaret() || doc >= session.views().size() ||
          session.storeIndexOf(doc) != storeIndex ||
          session.versionOf(doc) != source.version ||
          session.views()[doc].focusedBirth != source.focusedBirth)
        return;
      activateDocument(rState, static_cast<std::uint32_t>(doc));
      focusSpanAt(rState, doc, source.start, source.end);
      if (sourceEntered)
        sourceEntered(
            DocumentSite{.store   = session.store(storeIndex).documentId(),
                         .version = source.version,
                         .range   = {source.start, source.end}});
    });
  });
}

void Views::focusSpan(const zigzag::CellRef /*cell*/,
                      const PrimediaSpan &span) {
  renderer->runWithState([this, span](RenderState &rState) {
    if (session.views().empty()) {
      return;
    }
    if (span.length > 0) {
      for (std::size_t docIdx = 0; docIdx < session.views().size(); ++docIdx) {
        const auto &vInfo = session.views()[docIdx];
        const auto &st    = session.store(vInfo.storeIndex);
        const auto ver    = st.rebuild(vInfo.version);
        const auto occs   = ver.occurrencesOf(span);
        if (!occs.empty()) {
          const auto &occ   = occs.front();
          auto *const caret = renderer->editCaret();
          if (caret) {
            caret->placeAt(static_cast<std::uint32_t>(docIdx), occ.start);
            caret->extendTo(occ.end);
          }
          if (docIdx < rState.docs.size() && rState.docs[docIdx]) {
            const auto &doc = rState.docs[docIdx];
            if (const auto anch = doc->anchorFor(occ.start)) {
              if (const auto wp =
                      doc->worldPoint(anch->pageIndex, anch->x, anch->y)) {
                if (state) {
                  std::scoped_lock locker(state->view);
                  state->view.pos.x = wp->x;
                  state->view.pos.y = wp->y;
                }
              }
            }
          }
          return;
        }
      }
    }

    for (std::size_t docIdx = 0; docIdx < session.views().size(); ++docIdx) {
      const auto &vInfo = session.views()[docIdx];
      const auto &st    = session.store(vInfo.storeIndex);
      for (const auto &[linkId, link] : st.links()) {
        for (const auto &lSpan : link.left) {
          const auto occs = st.rebuild(vInfo.version).occurrencesOf(lSpan);
          if (!occs.empty()) {
            auto *const caret = renderer->editCaret();
            if (caret) {
              caret->placeAt(static_cast<std::uint32_t>(docIdx),
                             occs.front().start);
              caret->extendTo(occs.front().end);
            }
            return;
          }
        }
      }
    }
  });
}

void Views::focusSpan(const std::size_t docIndex, const std::uint32_t charStart,
                      const std::uint32_t charEnd) {
  renderer->runWithState(
      [this, docIndex, charStart, charEnd](RenderState &rState) {
        focusSpanAt(rState, docIndex, charStart, charEnd);
      });
}

void Views::focusSpanAt(RenderState &rState, const std::size_t docIndex,
                        const std::uint32_t charStart,
                        const std::uint32_t charEnd) {
  GLEDITOR_LOG_DEBUG("xudu.links", "focus document {} range [{},{})", docIndex,
                     charStart, charEnd);
  if (docIndex >= session.views().size() || docIndex >= rState.docs.size() ||
      !rState.docs[docIndex])
    return;
  // Endpoint travel uses the same document activation as a tab selection.
  // Its exact-range camera belongs to the caller, rather than the page-zero
  // framing scheduled by ordinary activation.
  activateDocument(rState, static_cast<std::uint32_t>(docIndex));
  cancelReadingFrame();
  auto *const caret = renderer->editCaret();
  if (caret) {
    caret->placeAt(static_cast<std::uint32_t>(docIndex), charStart);
    caret->extendTo(charEnd);
  }
  const auto &doc = rState.docs[docIndex];
  if (const auto anchor = doc->anchorFor(charStart)) {
    if (const auto point =
            doc->worldPoint(anchor->pageIndex, anchor->x, anchor->y)) {
      if (state) {
        std::scoped_lock lock(state->view);
        state->view.pos.x = point->x;
        state->view.pos.y = point->y;
      }
    }
  }
}

void Views::focusContent(std::vector<PrimediaSpan> content) {
  renderer->runWithState([this, content = std::move(content)](RenderState &) {
    for (std::size_t docIdx = 0; docIdx < session.views().size(); ++docIdx) {
      const auto found = xanadu::contentOccurrences(
          session.views()[docIdx].pieces.pieces(), content);
      if (!found.empty()) {
        focusSpan(docIdx, found.front().start, found.front().end);
        return;
      }
    }
    GLEDITOR_LOG_DEBUG("xudu.links",
                       "activated cell content is in no open document");
  });
}

void Views::back() {
  renderer->runWithState([this](RenderState &) {
    if (session.views().empty()) {
      return;
    }
    const auto here = session.views().front().version;
    if (here.isZero()) {
      std::cout << "xudu: already at the null document\n";
      return;
    }
    const auto there = here.parent();
    std::cout << "xudu: " << here.str() << " -> " << there.str() << "\n";
    showOnly(there, session.views().front().storeIndex);
  });
}

void Views::forward() {
  renderer->runWithState([this](RenderState &) {
    if (session.views().empty()) {
      return;
    }
    const auto sIdx     = session.views().front().storeIndex;
    const auto here     = session.views().front().version;
    const auto children = session.store(sIdx).children(here);
    if (children.empty()) {
      std::cout << "xudu: " << here.str() << " has no successor\n";
      return;
    }
    std::cout << "xudu: " << here.str() << " -> " << children.front().str();
    if (children.size() > 1) {
      std::cout << " (of " << children.size() << " futures)";
    }
    std::cout << "\n";
    showOnly(children.front(), sIdx);
  });
}

void Views::scrubHistory(const bool backward) {
  renderer->runWithState([this, backward](RenderState &rState) {
    if (session.views().empty() || rState.docs.empty()) {
      return;
    }
    auto *const caret = renderer->editCaret();
    const auto docIdx = (nullptr != caret && caret->active() &&
                         caret->documentIndex() < rState.docs.size())
                            ? caret->documentIndex()
                            : 0U;

    auto &doc           = *rState.docs[docIdx];
    const bool scrubbed = backward ? session.scrubBackward(docIdx, doc, 1)
                                   : session.scrubForward(docIdx, doc, 1);
    if (scrubbed) {
      syncMediaWidgets(rState);
      const auto newVer = session.versionOf(docIdx);
      const auto hist   = session.historyOf(docIdx);
      const auto it     = std::ranges::find(hist, newVer);
      const auto step =
          (it != hist.end())
              ? static_cast<std::size_t>(std::distance(hist.begin(), it) + 1)
              : 0U;
      std::cout << "xudu: hypertime scrub -> " << newVer.str() << " (" << step
                << "/" << hist.size() << ")\n";
    }
  });
}

void Views::deleteSelection() {
  withCaret([](RenderState &rState, const Where &where, Caret *caret) {
    auto &doc  = *rState.docs[where.doc];
    auto start = where.start;
    if (!where.hasRange) {
      start = gleditor::stepCharacter(doc.contents(), where.start, false);
    }
    if (start < where.end) {
      doc.erase(rState, start, where.end - start, caret);
    }
  });
}

void Views::deleteForward() {
  withCaret([](RenderState &rState, const Where &where, Caret *caret) {
    auto &doc = *rState.docs[where.doc];
    auto end  = where.end;
    if (!where.hasRange) {
      end = gleditor::stepCharacter(doc.contents(), where.end, true);
    }
    if (where.start < end) {
      doc.erase(rState, where.start, end - where.start, caret);
    }
  });
}

void Views::moveCaret(const gleditor::CaretMotion motion, const bool extend) {
  withCaret([this, motion, extend](RenderState &rState, const Where &where,
                                   Caret *caret) {
    const auto &doc = *rState.docs[where.doc];
    std::uint32_t target{};
    if (!extend && where.hasRange && gleditor::CaretMotion::Left == motion) {
      target = where.start;
    } else if (!extend && where.hasRange &&
               gleditor::CaretMotion::Right == motion) {
      target = where.end;
    } else {
      target = gleditor::caretTarget(doc, caret->byteOffset(), motion);
    }
    if (extend) {
      if (!caret->hasSelection()) {
        caret->anchorSelection();
      }
      caret->extendTo(target);
    } else {
      caret->placeAt(where.doc, target);
    }
    keepInView(doc, target);
  });
}

void Views::transcludeSelection() {
  withCaret([this](RenderState &, const Where &where, Caret *) {
    if (!where.hasRange) {
      std::cout << "xudu: select something to transclude first\n";
      return;
    }
    const auto from   = session.versionOf(where.doc);
    const auto sIdx   = session.storeIndexOf(where.doc);
    const auto quoted = session.store(sIdx).transclude(
        MicroversionId{}, 0, from, where.start, where.end - where.start);
    std::cout << "xudu: transcluded [" << where.start << "," << where.end
              << ") of " << from.str() << " into " << quoted.str() << "\n";
    showAlongside(quoted, 0.0F, sIdx);
  });
}

void Views::linkSelection() {
  withCaret([this](RenderState &, const Where &where, Caret *) {
    if (!where.hasRange) {
      std::cout << "xudu: select something to xanalink first\n";
      return;
    }
    const auto version = session.versionOf(where.doc);
    const auto sIdx    = session.storeIndexOf(where.doc);
    auto spans         = session.store(sIdx).rebuild(version).spansFor(
        where.start, where.end - where.start);

    if (!pending) {
      pending = Pending{.doc        = where.doc,
                        .storeIndex = sIdx,
                        .start      = where.start,
                        .end        = where.end,
                        .spans      = std::move(spans)};
      std::cout << "xudu: xanalink from doc " << where.doc << " ["
                << where.start << "," << where.end
                << ") -- select the other end and press ctrl-l again\n";
      return;
    }

    xanadu::Link link;
    link.type       = xanadu::LinkType::Comment;
    link.owner      = "you";
    const auto left = carrySpans(session.store(pending->storeIndex),
                                 session.store(sIdx), pending->spans);
    if (!left) {
      state->showDialog(render::DiagnosticSeverity::Warning,
                        "Link source unavailable", left.error());
      return;
    }
    link.left        = *left;
    link.right       = std::move(spans);
    const auto after = session.addLink(where.doc, link);
    std::cout << "xudu: link doc " << pending->doc << " [" << pending->start
              << "," << pending->end << ") -> doc " << where.doc << " ["
              << where.start << "," << where.end << ") at " << after.str()
              << "\n";
    pending.reset();
  });
}

void Views::cancelLink() {
  renderer->runWithState([this](RenderState &) {
    if (!pending) {
      std::cout << "xudu: no link waiting for its other end\n";
      return;
    }
    std::cout << "xudu: dropped the link begun at doc " << pending->doc << " ["
              << pending->start << "," << pending->end << ")\n";
    pending.reset();
  });
}

void Views::addCellToPendingLink(const std::span<const PrimediaSpan> content,
                                 const std::size_t sourceStoreIndex) {
  if (!pending) {
    std::cout << "xudu: select a document passage with Ctrl+L first\n";
    return;
  }
  if (content.empty()) {
    std::cout << "xudu: focused cell has no content to link\n";
    return;
  }
  const auto carried = carrySpans(session.store(sourceStoreIndex),
                                  session.store(pending->storeIndex), content);
  if (!carried) {
    state->showDialog(render::DiagnosticSeverity::Warning,
                      "Link source unavailable", carried.error());
    return;
  }
  pending->right.insert(pending->right.end(), carried->begin(), carried->end());
  ++pending->rightCells;
  std::cout << "xudu: added cell " << pending->rightCells
            << " to the pending link; choose another or finish the link\n";
}

void Views::finishCellLink() {
  if (pending && (pending->doc >= session.views().size() ||
                  session.storeIndexOf(pending->doc) != pending->storeIndex)) {
    pending.reset();
    std::cout << "xudu: link destination was closed; begin a new link\n";
    return;
  }
  if (!pending || pending->right.empty()) {
    std::cout << "xudu: select a passage and add at least one cell first\n";
    return;
  }
  xanadu::Link link;
  link.type        = xanadu::LinkType::Comment;
  link.owner       = "you";
  link.left        = std::move(pending->spans);
  link.right       = std::move(pending->right);
  const auto after = session.addLink(pending->doc, std::move(link));
  std::cout << "xudu: linked document passage to " << pending->rightCells
            << " cell(s) at " << after.str() << "\n";
  pending.reset();
}

void Views::publishCurrent(const std::string &salt) {
  renderer->runWithState([this, salt](RenderState &) {
    if (session.views().empty()) {
      std::cout << "xudu: nothing open to publish\n";
      state->showDialog(render::DiagnosticSeverity::Warning,
                        "Nothing to publish",
                        "No document is open. Open one, and what is under "
                        "the caret is what gets published.");
      return;
    }
    auto *const caret   = renderer->editCaret();
    const auto which    = nullptr != caret && caret->active() &&
                               caret->documentIndex() < session.views().size()
                              ? caret->documentIndex()
                              : 0U;
    const auto version  = session.versionOf(which);
    const auto storeIdx = session.storeIndexOf(which);
    const auto who      = session.author();

    using Field = gleditor::Form::Field;
    using Kind  = gleditor::Form::Kind;

    Field keys;
    keys.label    = "Signing key";
    keys.value    = {};
    keys.hint     = "no signing key in the keyring";
    keys.required = false;
    keys.kind     = Kind::Choice;

    for (const auto &key : signingKeys(session.settings().signing())) {
      keys.options.push_back(key.describe());
      keys.optionValues.push_back(key.fingerprint);
      const bool wanted = who.gpgKey.empty()
                              ? key.preferred
                              : key.fingerprint.ends_with(who.gpgKey) ||
                                    key.identity.contains(who.gpgKey);
      if (wanted) {
        keys.chosen = keys.options.size() - 1;
      }
    }
    if (keys.options.empty()) {
      std::cout << "xudu: no signing key in the keyring\n";
      state->showDialog(
          render::DiagnosticSeverity::Error, "No signing key",
          "The keyring has no secret key to sign an authorship record with, "
          "and a document is signed before it is sealed. Make one with `gpg "
          "--quick-generate-key`, or point gpg_home in " +
              xanadu::configPath() + " at the keyring that holds yours.");
      return;
    }

    std::vector<Field> asked;
    Field fName;
    fName.label = "Name";
    fName.value = salt.empty() ? std::string{"document"} : salt;
    fName.hint  = "one word; publishing again under it is a further state of "
                  "this document";
    fName.required = true;
    asked.push_back(std::move(fName));

    Field fTitle;
    fTitle.label    = "Title";
    fTitle.value    = salt.empty() ? std::string{"document"} : salt;
    fTitle.hint     = "what this document is called";
    fTitle.required = true;
    asked.push_back(std::move(fTitle));

    Field fAuthor;
    fAuthor.label    = "Author";
    fAuthor.value    = who.name;
    fAuthor.hint     = "who is publishing this";
    fAuthor.required = true;
    asked.push_back(std::move(fAuthor));

    Field fEmail;
    fEmail.label    = "Email";
    fEmail.value    = who.email;
    fEmail.hint     = "how to reach them";
    fEmail.required = true;
    asked.push_back(std::move(fEmail));

    asked.push_back(std::move(keys));

    Field fPass;
    fPass.label    = "Passphrase";
    fPass.value    = {};
    fPass.hint     = "only if the agent is not holding it";
    fPass.required = false;
    fPass.kind     = Kind::Secret;
    asked.push_back(std::move(fPass));

    Field toggle;
    toggle.label          = "";
    toggle.value          = {};
    toggle.hint           = {};
    toggle.required       = false;
    toggle.kind           = Kind::Toggle;
    toggle.revealsSecrets = true;
    asked.push_back(std::move(toggle));

    Field fRights;
    fRights.label    = "Rights";
    fRights.value    = {};
    fRights.hint     = "how others may use this; optional";
    fRights.required = false;
    asked.push_back(std::move(fRights));

    Field fNote;
    fNote.label    = "Note";
    fNote.value    = {};
    fNote.hint     = "anything else worth recording; optional";
    fNote.required = false;
    asked.push_back(std::move(fNote));

    Field fTopics;
    fTopics.label = "Topics";
    fTopics.hint  = "comma-separated discovery tags, e.g. Ideas, Fiction";
    asked.push_back(std::move(fTopics));

    Field editions;
    editions.label        = "Editions";
    editions.kind         = Kind::Choice;
    editions.options      = {"Keep all existing editions"};
    editions.optionValues = {"keep"};
    auto editionHead      = session.store(storeIdx).structureHead();
    if (editionHead.isZero()) editionHead = session.store(storeIdx).latest();
    std::string review;
    for (const auto &edition : session.store(storeIdx).editions(editionHead)) {
      const auto target = edition.targetVersion.str();
      editions.options.push_back("Repoint " + edition.name + " (" + target +
                                 ") to " + version.str());
      editions.optionValues.push_back(
          session.store(storeIdx).segmentedOps().idOf(edition.cell).str());
      if (!review.empty()) review += "; ";
      review += edition.name + " → " + target;
    }
    editions.options.push_back("Add a named edition for " + version.str());
    editions.optionValues.push_back("new");
    editions.hint = review.empty()
                        ? "No editions designated; keeping them is allowed"
                        : "Existing: " + review;
    asked.push_back(std::move(editions));
    Field editionName;
    editionName.label = "New edition";
    editionName.hint  = "name required only when adding an edition above";
    asked.push_back(std::move(editionName));

    Field destination;
    destination.label         = "Destination";
    destination.kind          = Kind::Choice;
    destination.submitOnEnter = true;
    destination.options       = {"Local publication"};
    destination.optionValues  = {"local"};
    if (session.testPublicationSwarmEnabled()) {
      destination.options.push_back("Test swarm — mock identity verification");
      destination.optionValues.push_back("test-swarm");
    }
    asked.push_back(std::move(destination));

    form.open(
        "Publish " + version.str() + " — author scroll " +
            std::to_string(session.store(storeIdx).primedia().size()) +
            " bytes",
        "All store history and scrolls; private settings withheld.",
        std::move(asked),
        [this, version, which, storeIdx](const std::vector<Field> &answers) {
          publishAnswers(version, which, storeIdx, answers);
        });
  });
}

void Views::publishAnswers(const MicroversionId &version,
                           const std::uint32_t which,
                           const std::size_t storeIdx,
                           const std::vector<gleditor::Form::Field> &answers) {
  Session::PublishRequest request;
  request.salt          = answers[0].answer();
  request.title         = answers[1].answer();
  request.author.name   = answers[2].answer();
  request.author.email  = answers[3].answer();
  request.author.gpgKey = answers[4].answer();
  request.passphrase    = answers[5].answer();
  if (!answers[7].answer().empty()) {
    request.extra.emplace_back("rights", answers[7].answer());
  }
  if (!answers[8].answer().empty()) {
    request.extra.emplace_back("note", answers[8].answer());
  }

  if (answers.size() > 9)
    request.topics = publicationTopics(answers[9].answer());
  if (answers.size() > 12) {
    const auto editionAction = answers[10].answer();
    if (editionAction == "new") {
      request.newEditionName = answers[11].answer();
      if (request.newEditionName.empty()) {
        state->showDialog(render::DiagnosticSeverity::Error,
                          "Edition name required",
                          "Name the new edition before publishing.");
        return;
      }
    } else if (editionAction != "keep") {
      request.editionToRepoint = MicroversionId::parse(editionAction);
    }
    request.announce = answers[12].answer() == "test-swarm";
  }

  renderer->runWithState(
      [this, version, which, storeIdx, request](RenderState &) {
        try {
          const auto path = session.publishDocument(version, request, storeIdx);
          std::cout << "xudu: prepared doc " << which << " as " << path
                    << "; inspect Publication status with Ctrl+Shift+P\n";
        } catch (const std::exception &err) {
          std::cout << "xudu: cannot publish: " << err.what() << "\n";
          state->showDialog(render::DiagnosticSeverity::Error,
                            "Could not publish " + version.str(), err.what());
        }
      });
}

void Views::publicationStatus() {
  renderer->runWithState([this](RenderState &) {
    try {
      const auto statuses = session.publicationOutbox().statuses();
      if (statuses.empty()) {
        state->showDialog(render::DiagnosticSeverity::Info,
                          "Publication status",
                          "No publications have been queued in this profile.");
        return;
      }
      using Field = gleditor::Form::Field;
      Field jobs;
      jobs.label = "Publication";
      jobs.kind  = gleditor::Form::Kind::Choice;
      for (const auto &status : statuses) {
        auto description = status.title + " #" +
                           std::to_string(status.sequence) + ": " +
                           std::string(publicationPhaseName(status.phase));
        if (status.identity == PublicationIdentity::MockVerified)
          description += " (mock verification)";
        if (!status.error.empty()) description += " — " + status.error;
        jobs.options.push_back(std::move(description));
        jobs.optionValues.push_back(status.id);
      }
      jobs.chosen = statuses.size() - 1;
      Field action;
      action.label         = "Action";
      action.kind          = gleditor::Form::Kind::Choice;
      action.submitOnEnter = true;
      action.options       = {"Refresh status", "Retry selected publication",
                              "Close"};
      action.optionValues  = {"refresh", "retry", "close"};
      form.open("Publication status",
                "Published: DHT acknowledged. Local ready: files verified.",
                {std::move(jobs), std::move(action)},
                [this](const std::vector<Field> &answers) {
                  if (answers[1].answer() == "close") return;
                  if (answers[1].answer() == "retry") {
                    const auto id = answers[0].answer();
                    renderer->runWithState([this, id](RenderState &) {
                      try {
                        session.publicationOutbox().retry(id);
                        std::cout << "xudu: publication retry queued\n";
                      } catch (const std::exception &error) {
                        state->showDialog(render::DiagnosticSeverity::Error,
                                          "Publication retry", error.what());
                      }
                    });
                  }
                  publicationStatus();
                });
    } catch (const std::exception &error) {
      state->showDialog(render::DiagnosticSeverity::Error, "Publication status",
                        error.what());
    }
  });
}

void Views::saveCurrent() {
  renderer->runWithState([this](RenderState &) {
    if (session.views().empty()) {
      session.saveAll();
      return;
    }
    auto *const caret = renderer->editCaret();
    auto which        = switcher ? switcher->activeDocIndex() : 0U;
    if (nullptr != caret && caret->active() &&
        caret->documentIndex() < session.views().size()) {
      which = caret->documentIndex();
    }
    if (which >= session.views().size()) {
      session.saveAll();
      return;
    }
    const auto storeIdx = session.storeIndexOf(which);
    if (session.isTemporaryStore(storeIdx)) {
      using Field         = gleditor::Form::Field;
      namespace fs        = std::filesystem;
      std::string curDir  = fs::current_path().string();
      std::string defName = "doc_" + std::to_string(storeIdx) + ".xanadoc";

      std::vector<Field> fields;
      Field fFolder;
      fFolder.label    = "Folder";
      fFolder.value    = curDir;
      fFolder.hint     = "directory where the xanadoc folder will live";
      fFolder.required = true;
      fields.push_back(std::move(fFolder));

      Field fName;
      fName.label    = "Name";
      fName.value    = defName;
      fName.hint     = "name of the xanadoc folder";
      fName.required = true;
      fields.push_back(std::move(fName));

      form.open(
          "Preserve Temporary Xanadoc",
          "Designate a permanent directory and name for this temporary store",
          std::move(fields),
          [this, storeIdx](const std::vector<Field> &answers) {
            preserveAnswers(storeIdx, answers);
          });
    } else {
      session.save(storeIdx);
      std::cout << "xudu: saved draft to " << session.path(storeIdx) << "\n";
      if (!session.store(storeIdx).scrolls().empty())
        renderer->push(RenderItemNotification(
            "Draft saved. Preparing offline quotations…"));
    }
  });
}

void Views::preserveAnswers(const std::size_t storeIdx,
                            const std::vector<gleditor::Form::Field> &answers) {
  namespace fs             = std::filesystem;
  const std::string folder = answers[0].answer();
  const std::string name   = answers[1].answer();
  const fs::path targetDir = fs::path(folder) / name;

  renderer->runWithState([this, storeIdx, targetDir](RenderState &) {
    try {
      namespace fs = std::filesystem;
      fs::create_directories(targetDir);
      session.saveTo(storeIdx, targetDir);
      session.setStorePath(storeIdx, targetDir.string(), false);
      std::cout << "xudu: preserved temporary store to " << targetDir.string()
                << "\n";
    } catch (const std::exception &err) {
      std::cout << "xudu: cannot preserve store: " << err.what() << "\n";
      state->showDialog(render::DiagnosticSeverity::Error,
                        "Could not preserve xanadoc", err.what());
    }
  });
}

void Views::closeDocument(const std::uint32_t docIndex) {
  session.flushUncommitted(docIndex);
  renderer->push(RenderItemCloseDoc(docIndex));
  renderer->runWithState([this, docIndex](RenderState &rState) {
    pending.reset();
    session.viewClosed(docIndex);
    if (!session.views().empty()) {
      map.setCurrent(session.views().front().version);
    }
    syncMediaWidgets(rState);
  });
}

void Views::closeActive() {
  renderer->runWithState([this](RenderState &rState) {
    if (rState.docs.empty()) {
      return;
    }
    auto *const caret = renderer->editCaret();
    auto which        = switcher ? switcher->activeDocIndex() : 0U;
    if (nullptr != caret && caret->active() &&
        caret->documentIndex() < rState.docs.size()) {
      which = caret->documentIndex();
    }
    if (which < rState.docs.size()) {
      closeDocument(which);
    }
  });
}

void Views::activateNewest() {
  renderer->runWithState([this](RenderState &rState) {
    if (!rState.docs.empty()) {
      activateDocument(rState,
                       static_cast<std::uint32_t>(rState.docs.size() - 1));
    }
  });
}

void Views::activateDocument(RenderState &rState, const std::uint32_t index) {
  if (index >= rState.docs.size() || !rState.docs[index]) {
    return;
  }
  if (switcher) {
    switcher->setActiveDocIndex(index);
  }
  if (auto *const caret = renderer->editCaret(); caret) {
    caret->placeAt(index, 0);
  }
  frameTarget_ = rState.docs[index];
  if (documentActivated) documentActivated(rState, index);
}

std::size_t Views::newDocument() {
  const auto storeIndex = session.createNewStore("");
  showAlongside(MicroversionId{}, 0.0F, storeIndex);
  activateNewest();
  std::cout << "xudu: created new sovereign document (store " << storeIndex
            << ")\n";
  return storeIndex;
}

xanadu::ReadingPlace Views::currentPlace() const {
  xanadu::ReadingPlace place;
  const auto *const caret = renderer->editCaret();
  for (std::size_t i = 0; i < session.views().size(); ++i) {
    const auto &view = session.views()[i];
    xanadu::DocumentPlace document{.storePath = session.path(view.storeIndex),
                                   .version   = view.version.str()};
    if (nullptr != caret && caret->active() && caret->documentIndex() == i) {
      document.caret  = caret->byteOffset();
      document.anchor = !caret->hasSelection() ? caret->byteOffset()
                        : caret->byteOffset() == caret->selectionStart()
                            ? caret->selectionEnd()
                            : caret->selectionStart();
      place.active    = i;
    }
    place.documents.push_back(std::move(document));
  }
  std::scoped_lock locker(state->view);
  place.camera = std::array<double, 4>{state->view.pos.x, state->view.pos.y,
                                       state->view.pos.z, state->view.fov};
  return place;
}

void Views::restorePlace(const xanadu::ReadingPlace &place,
                         std::vector<std::optional<std::uint32_t>> opened,
                         std::vector<std::uint32_t> lengths) {
  renderer->runWithState([this, place, opened = std::move(opened),
                          lengths = std::move(lengths)](RenderState &rState) {
    if (place.camera) {
      // The saved view, not a fresh reading framing of the first page.
      readingFramed_ = true;
      frameTarget_.reset();
      std::scoped_lock locker(state->view);
      const auto &[x, y, z, fov] = *place.camera;
      state->view.pos = glm::vec3(static_cast<float>(x), static_cast<float>(y),
                                  static_cast<float>(z));
      state->view.fov = static_cast<float>(fov);
    }
    if (!place.active || *place.active >= opened.size() ||
        !opened[*place.active]) {
      return;
    }
    const auto index = *opened[*place.active];
    if (index >= rState.docs.size()) {
      return;
    }
    const auto &saved = place.documents[*place.active];
    const auto length = *place.active < lengths.size() ? lengths[*place.active]
                                                       : std::uint32_t{0};
    if (switcher) {
      switcher->setActiveDocIndex(index);
    }
    if (auto *const caret = renderer->editCaret(); caret) {
      caret->placeAt(index, std::min(saved.anchor, length));
      if (saved.anchor != saved.caret) {
        caret->anchorSelection();
        caret->extendTo(std::min(saved.caret, length));
      }
    }
  });
}

void Views::spawnTranscludedDocument(const TetherPayload &payload,
                                     const float /*screenX*/,
                                     const float /*screenY*/) {
  if (!payload.originSource || payload.spans.empty()) return;
  const auto source = payload.sourceStoreIndex;
  if (!source || *source >= session.storeCount() ||
      session.store(*source).documentId().str() !=
          payload.originSource->authority) {
    state->showDialog(render::DiagnosticSeverity::Warning,
                      "Quotation source unavailable",
                      "The source store is no longer open.");
    return;
  }
  try {
    const auto destination = session.createNewStore("");
    auto &store            = session.store(destination);
    const auto carried =
        carrySpans(session.store(*source), store, payload.spans);
    if (!carried) {
      state->showDialog(render::DiagnosticSeverity::Warning,
                        "Quotation source unavailable", carried.error());
      return;
    }
    auto version     = store.latest();
    std::uint32_t at = 0;
    for (const auto &span : *carried) {
      version = store.insertSpan(version, at, span);
      at += static_cast<std::uint32_t>(span.length);
    }
    session.save(destination);
    showAlongside(version, 0.0F, destination);
    activateNewest();
    std::cout << "xudu: spawned transcluded document version " << version.str()
              << " from origin version " << payload.originVersion.str() << " ["
              << payload.originCharStart << ", " << payload.originCharEnd
              << ")\n";
  } catch (const std::exception &error) {
    state->showDialog(render::DiagnosticSeverity::Warning,
                      "Quotation unavailable", error.what());
  }
}

void Views::summonPublication(const PublicationEntry &entry) {
  openDocumentFromPath(entry.bep46Uri);
}

void Views::discoverPublications(const std::string &query) {
  renderer->runWithState([this, query](RenderState &) {
    using Field      = gleditor::Form::Field;
    const auto start = [this](const std::string &value, bool author) {
      try {
        const auto id = session.publicationDiscovery().submit(value, author);
        publicationDiscoveryStatus(id);
      } catch (const std::exception &error) {
        Field back;
        back.label         = "Action";
        back.kind          = gleditor::Form::Kind::Choice;
        back.options       = {"Back to discovery"};
        back.submitOnEnter = true;
        form.open("Discovery unavailable", error.what(), {std::move(back)},
                  [this](const auto &) { discoverPublications(); });
      }
    };
    if (!query.empty()) {
      const auto author = query.starts_with("author:");
      start(author ? query.substr(7) : query, author);
      return;
    }
    Field mode;
    mode.label        = "Find by";
    mode.kind         = gleditor::Form::Kind::Choice;
    mode.options      = {"Topic keyword", "Follow publishing key"};
    mode.optionValues = {"topic", "author"};
    Field input;
    input.label = "Topic or publishing key";
    input.hint  = "Ideas, or the author's 64-hex public key";
    Field action;
    action.label         = "Action";
    action.kind          = gleditor::Form::Kind::Choice;
    action.options       = {"Start discovery", "Close"};
    action.optionValues  = {"start", "close"};
    action.submitOnEnter = true;
    form.open("Discover publications",
              "Signed metadata; author enrollment is not verified.",
              {std::move(mode), std::move(input), std::move(action)},
              [start](const auto &answers) {
                if (answers[2].answer() == "start")
                  start(answers[1].answer(), answers[0].answer() == "author");
              });
  });
}

void Views::publicationDiscoveryStatus(const std::string &id) {
  renderer->runWithState([this, id](RenderState &) {
    using Field = gleditor::Form::Field;
    try {
      const auto status = session.publicationDiscovery().status(id);
      Field action;
      action.label         = "Action";
      action.kind          = gleditor::Form::Kind::Choice;
      action.options       = {"Refresh progress", "Open selected publication",
                              "Retry discovery", "Close"};
      action.optionValues  = {"refresh", "open", "retry", "close"};
      action.submitOnEnter = true;
      Field result;
      result.label = "Publication";
      result.kind  = gleditor::Form::Kind::Choice;
      std::string note;
      if (publicationCatalog_)
        for (const auto &key : session.publicationDiscovery().followedAuthors())
          publicationCatalog_->followAuthor(key);
      for (const auto &catalog : status.catalogs) {
        if (publicationCatalog_)
          publicationCatalog_->ingestAuthorCatalog(catalog);
        for (const auto &entry : catalog.entries) {
          if (entry.kind != CatalogEntryKind::Document) continue;
          if (!status.author && std::ranges::find(entry.topics, status.query) ==
                                    entry.topics.end())
            continue;
          const auto publication = catalogPublicationEntry(catalog, entry);
          result.options.push_back(
              entry.title + " — " + catalog.publisher.hex().substr(0, 12) +
              " #" + std::to_string(entry.sequence) + " (catalog #" +
              std::to_string(catalog.sequence) + ")");
          result.optionValues.push_back(publication.bep46Uri);
        }
      }
      switch (status.phase) {
      case DiscoveryPhase::Queued:
        note = "queued";
        break;
      case DiscoveryPhase::Searching:
        note = "searching peers";
        break;
      case DiscoveryPhase::Ready:
        note = "signed metadata received; enrollment unchecked";
        break;
      case DiscoveryPhase::Failed:
        note = status.error;
        break;
      }
      note += status.author ? " — author " + status.query.substr(0, 12)
                            : " — topic " + status.query;
      if (result.options.empty()) {
        result.options      = {"No accepted publication metadata"};
        result.optionValues = {""};
      }
      form.open("Publication discovery", std::move(note),
                {std::move(action), std::move(result)},
                [this, id](const auto &answers) {
                  const auto action = answers[0].answer();
                  if (action == "close") return;
                  if (action == "open" && !answers[1].answer().empty()) {
                    openDocumentFromPath(answers[1].answer());
                    return;
                  }
                  if (action == "retry") {
                    const auto status =
                        session.publicationDiscovery().status(id);
                    (void)session.publicationDiscovery().submit(status.query,
                                                                status.author);
                  }
                  publicationDiscoveryStatus(id);
                });
    } catch (const std::exception &error) {
      Field back;
      back.label         = "Action";
      back.kind          = gleditor::Form::Kind::Choice;
      back.options       = {"Back to discovery"};
      back.submitOnEnter = true;
      form.open("Discovery unavailable", error.what(), {std::move(back)},
                [this](const auto &) { discoverPublications(); });
    }
  });
}

void Views::publicationDownloadStatus(const std::string &id) {
  renderer->runWithState([this, id](RenderState &) {
    try {
      const auto downloaded = session.publicationInbox().status(id);
      using Field           = gleditor::Form::Field;
      const auto note       = publicationDownloadSummary(downloaded);
      if (wireframeOverlay_) {
        const auto fraction =
            downloaded.dependencyCount
                ? std::optional<float>{static_cast<float>(
                                           downloaded.completedDependencies) /
                                       static_cast<float>(
                                           downloaded.dependencyCount)}
                : std::nullopt;
        wireframeOverlay_->setTelemetry(id,
                                        downloaded.title.empty()
                                            ? "Publication download"
                                            : downloaded.title,
                                        note, fraction);
      }
      openPublicationDownloadForm(
          form, downloaded,
          [this, id](PublicationDownloadAction action) {
            if (action == PublicationDownloadAction::Close) {
              renderer->runWithState([this, id](RenderState &) {
                if (wireframeOverlay_) wireframeOverlay_->clearTelemetry(id);
              });
              return;
            }
            renderer->runWithState([this, id, action](RenderState &) {
              try {
                if (action == PublicationDownloadAction::Open) {
                  const auto [index, version] =
                      session.openDownloadedPublication(id);
                  if (wireframeOverlay_) wireframeOverlay_->clearTelemetry(id);
                  showAlongside(version, 0.0F, index);
                  activateNewest();
                  return;
                }
                if (action == PublicationDownloadAction::Retry)
                  session.publicationInbox().retry(id);
                if (action == PublicationDownloadAction::Cancel)
                  session.publicationInbox().cancel(id);
                if (action == PublicationDownloadAction::DiscardPartialData)
                  session.publicationInbox().discardPartialData(id);
                if (action == PublicationDownloadAction::Subscribe) {
                  const auto subscription =
                      session.publicationSubscriptions().subscribe(id);
                  publicationUpdates(subscription);
                  return;
                }
                publicationDownloadStatus(id);
              } catch (const std::exception &error) {
                // Keep failures in the drawn, accessible form; native
                // message boxes are suppressed during headless runs and
                // hide retry controls.
                Field back;
                back.label         = "Action";
                back.kind          = gleditor::Form::Kind::Choice;
                back.options       = {"Back to download"};
                back.submitOnEnter = true;
                form.open("Publication action failed", error.what(),
                          {std::move(back)}, [this, id](const auto &) {
                            publicationDownloadStatus(id);
                          });
              }
            });
          },
          [this, id] {
            renderer->runWithState([this, id](RenderState &) {
              if (wireframeOverlay_) wireframeOverlay_->clearTelemetry(id);
            });
          });
    } catch (const std::exception &error) {
      using Field = gleditor::Form::Field;
      Field back;
      back.label         = "Action";
      back.kind          = gleditor::Form::Kind::Choice;
      back.options       = {"Back to Open"};
      back.submitOnEnter = true;
      form.open("Downloads unavailable", error.what(), {std::move(back)},
                [this](const auto &) { openDocumentPalette(); });
    }
  });
}

void Views::publicationUpdates(const std::string &selected,
                               std::int64_t sequence) {
  renderer->runWithState([this, selected, sequence](RenderState &) {
    using Field = gleditor::Form::Field;
    try {
      auto &subscriptions = session.publicationSubscriptions();
      const auto statuses = subscriptions.statuses();
      Field action;
      action.label         = "Action";
      action.kind          = gleditor::Form::Kind::Choice;
      action.options       = {"Refresh updates",
                              "Open update alongside earlier version",
                              "Open earlier version",
                              "Mark update reviewed",
                              "Check selected subscription now",
                              "Pause selected notifications",
                              "Resume selected notifications",
                              "Close"};
      action.optionValues  = {"refresh", "open",  "earlier", "ack",
                              "check",   "pause", "resume",  "close"};
      action.submitOnEnter = true;
      Field followed;
      followed.label = "Subscription";
      followed.kind  = gleditor::Form::Kind::Choice;
      Field updates;
      updates.label    = "Verified update";
      updates.kind     = gleditor::Form::Kind::Choice;
      std::string note = "No subscriptions. Choose Notify me of updates in a "
                         "completed download.";
      for (const auto &status : statuses) {
        const auto link = MutableLink::parse(status.uri);
        followed.options.push_back(status.title + " — " +
                                   link.key.hex().substr(0, 12) + " #" +
                                   std::to_string(status.sequence));
        followed.optionValues.push_back(status.id);
        if (status.id == selected)
          followed.chosen = followed.options.size() - 1;
        for (const auto &notice : status.notices) {
          if (notice.acknowledged) continue;
          updates.options.push_back(notice.title + " — " +
                                    link.key.hex().substr(0, 12) + " #" +
                                    std::to_string(notice.previousSequence) +
                                    " → #" + std::to_string(notice.sequence));
          updates.optionValues.push_back(status.id + "@" +
                                         std::to_string(notice.sequence));
          if (status.id == selected && notice.sequence == sequence)
            updates.chosen = updates.options.size() - 1;
        }
      }
      if (!statuses.empty()) {
        const auto &status = statuses[followed.chosen];
        note = std::string(publicationSubscriptionPhaseName(status.phase));
        if (!status.error.empty()) note += ": " + status.error;
      } else {
        followed.options      = {"No notification subscriptions"};
        followed.optionValues = {""};
      }
      if (updates.options.empty()) {
        updates.options      = {"No unreviewed verified updates"};
        updates.optionValues = {""};
      } else {
        const auto value  = updates.optionValues[updates.chosen];
        const auto split  = value.find('@');
        const auto status = subscriptions.status(value.substr(0, split));
        const auto seq    = std::stoll(value.substr(split + 1));
        const auto found  = std::ranges::find(
            status.notices, seq, &PublicationUpdateNotice::sequence);
        note = "Verified update — earlier " + found->previousVersion.str() +
               " → " + found->version.str();
      }
      form.open(
          "Publication updates", std::move(note),
          {std::move(action), std::move(followed), std::move(updates)},
          [this](const auto &answers) {
            const auto action   = answers[0].answer();
            const auto selected = answers[1].answer();
            const auto update   = answers[2].answer();
            if (action == "close") return;
            try {
              auto &subscriptions = session.publicationSubscriptions();
              std::string id;
              std::int64_t seq{-1};
              if (!update.empty()) {
                const auto split = update.find('@');
                id               = update.substr(0, split);
                seq              = std::stoll(update.substr(split + 1));
              }
              if (action == "open" || action == "earlier" || action == "ack") {
                if (id.empty())
                  throw std::logic_error("No verified update selected");
                const auto status = subscriptions.status(id);
                const auto found  = std::ranges::find(
                    status.notices, seq, &PublicationUpdateNotice::sequence);
                if (found == status.notices.end())
                  throw std::logic_error("Update is no longer pending");
                if (action != "ack") {
                  if (action == "open") {
                    const auto [earlier, version] =
                        session.openDownloadedPublication(
                            found->previousSnapshotId);
                    showAlongside(version, 0.0F, earlier);
                  }
                  const auto [index, version] =
                      session.openDownloadedPublication(
                          action == "open" ? found->snapshotId
                                           : found->previousSnapshotId);
                  showAlongside(version, 0.0F, index);
                  activateNewest();
                  if (action == "open") {
                    frameNewestComparison();
                    subscriptions.acknowledge(id, seq);
                  }
                  return;
                }
                subscriptions.acknowledge(id, seq);
              }
              if (action == "check") subscriptions.checkNow(selected);
              if (action == "pause" || action == "resume")
                subscriptions.setEnabled(selected, action == "resume");
              publicationUpdates(selected, seq);
            } catch (const std::exception &error) {
              Field back;
              back.label         = "Action";
              back.kind          = gleditor::Form::Kind::Choice;
              back.options       = {"Back to updates"};
              back.submitOnEnter = true;
              form.open("Update unavailable", error.what(), {std::move(back)},
                        [this, selected](const auto &) {
                          publicationUpdates(selected);
                        });
            }
          });
    } catch (const std::exception &error) {
      Field close;
      close.label         = "Action";
      close.kind          = gleditor::Form::Kind::Choice;
      close.options       = {"Close"};
      close.submitOnEnter = true;
      form.open("Updates unavailable", error.what(), {std::move(close)},
                [](const auto &) {});
    }
  });
}

void Views::insertSpanAtCaret(const PrimediaSpan &span) {
  withCaret([this, span](RenderState &rState, const Where &where, Caret *) {
    insertSpanAt(rState, where.doc, where.start, span);
  });
}

void Views::transcludeSpansAtCaret(std::vector<PrimediaSpan> spans,
                                   const Store &source) {
  withCaret([this, source = &source, spans = std::move(spans)](
                RenderState &rState, const Where &where, Caret *) {
    transcludeSpansAt(rState, where.doc, where.start, *source, spans);
  });
}

void Views::transcludeSpansAt(RenderState &rState, const std::uint32_t doc,
                              const std::uint32_t at, const Store &source,
                              const std::span<const PrimediaSpan> spans) {
  if (doc >= session.views().size()) return;
  const auto carried =
      carrySpans(source, session.store(session.storeIndexOf(doc)), spans);
  if (!carried) {
    state->showDialog(render::DiagnosticSeverity::Warning,
                      "Quotation source unavailable", carried.error());
    return;
  }
  auto position = at;
  for (const auto &span : *carried) {
    insertSpanAt(rState, doc, position, span);
    position += static_cast<std::uint32_t>(span.length);
  }
}

void Views::insertSpanAt(RenderState &rState, const std::uint32_t doc,
                         const std::uint32_t at, const PrimediaSpan &span) {
  if (doc >= rState.docs.size() || 0 == span.length) {
    return;
  }
  session.flushUncommitted(doc);
  const auto prod = session.insertSpan(doc, at, span);
  if (const auto src = session.sourceFor(prod, session.storeIndexOf(doc))) {
    rState.docs[doc]->load(*src);
    syncMediaWidgets(rState);
  }
  if (auto *const caret = renderer->editCaret(); caret) {
    caret->placeAt(doc, at + static_cast<std::uint32_t>(span.length));
  }
  std::cout << "xudu: transcluded " << span.length << " bytes into doc " << doc
            << " at " << at << "\n";
}

void Views::insertPageBreak(const std::uint32_t docIndex,
                            const std::uint32_t charOffset) {
  renderer->runWithState([this, docIndex, charOffset](RenderState &rState) {
    if (docIndex >= rState.docs.size()) {
      return;
    }
    const auto prod = session.insertBreak(docIndex, charOffset);
    if (const auto src =
            session.sourceFor(prod, session.storeIndexOf(docIndex))) {
      rState.docs[docIndex]->load(*src);
      syncMediaWidgets(rState);
    }
    std::cout << "xudu: page break inserted at doc " << docIndex << " offset "
              << charOffset << "\n";
  });
}

void Views::insertPageBreakAtCaret() {
  withCaret([this](RenderState &rState, const Where &where, Caret *) {
    if (where.doc >= rState.docs.size()) {
      return;
    }
    const auto prod = session.insertBreak(where.doc, where.start);
    if (const auto src =
            session.sourceFor(prod, session.storeIndexOf(where.doc))) {
      rState.docs[where.doc]->load(*src);
      syncMediaWidgets(rState);
    }
    std::cout << "xudu: page break inserted at doc " << where.doc << " offset "
              << where.start << "\n";
  });
}

void Views::exportOsmic() {
  renderer->runWithState([this](RenderState &) {
    if (session.views().empty()) {
      session.saveOsmicTextAll();
      return;
    }
    auto *const caret   = renderer->editCaret();
    const auto which    = (nullptr != caret && caret->active() &&
                        caret->documentIndex() < session.views().size())
                              ? caret->documentIndex()
                              : 0U;
    const auto storeIdx = session.storeIndexOf(which);
    session.store(storeIdx).saveOsmicText(session.path(storeIdx));
    std::cout << "xudu: exported osmic text for doc " << which << " ("
              << session.path(storeIdx) << ")\n";
  });
}

void Views::importFile(const std::string &filePath) {
  renderer->runWithState([this, filePath](RenderState &rState) {
    if (session.views().empty()) {
      return;
    }
    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
      std::cout << "xudu: cannot read import file: " << filePath << "\n";
      return;
    }
    const std::string bytes((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
    auto *const caret   = renderer->editCaret();
    const auto docIdx   = (nullptr != caret && caret->active() &&
                         caret->documentIndex() < session.views().size())
                              ? caret->documentIndex()
                              : 0U;
    const auto at       = (nullptr != caret && caret->active() &&
                     caret->documentIndex() == docIdx)
                              ? caret->byteOffset()
                              : 0U;
    const auto detected = gleditor::MimeDetector::detectFile(filePath);
    const auto mime =
        detected.empty() ? "text/plain;charset=utf-8" : detected.essence();
    const auto prod = session.insertMedia(docIdx, at, bytes, mime, filePath);
    if (docIdx < rState.docs.size()) {
      if (const auto src =
              session.sourceFor(prod, session.storeIndexOf(docIdx))) {
        rState.docs[docIdx]->load(*src);
        syncMediaWidgets(rState);
      }
    }
    std::cout << "xudu: imported file " << filePath << " into doc " << docIdx
              << " at offset " << at << " -> version " << prod.str() << "\n";
  });
}

void Views::setWireframeOverlay(WireframeHullOverlay *overlay) noexcept {
  wireframeOverlay_ = overlay;
}

void Views::unlockTranscopyright(const std::size_t storeIdx,
                                 const PrimediaSpan &span) {
  if (session.unlockTranscopyright(storeIdx, span)) {
    renderer->runWithState([this, storeIdx](RenderState &rState) {
      for (std::size_t dIdx = 0;
           dIdx < rState.docs.size() && dIdx < session.views().size(); ++dIdx) {
        if (!rState.docs[dIdx]) {
          continue;
        }
        const auto &vInfo = session.views()[dIdx];
        if (vInfo.storeIndex == storeIdx) {
          const auto src = session.sourceFor(vInfo.version, vInfo.storeIndex);
          rState.docs[dIdx]->load(*src);
        }
      }
      syncMediaWidgets(rState);
    });
  }
}

void Views::unlockTranscopyrightAtCaret() {
  renderer->runWithState([this](RenderState &rState) {
    auto *const caret = rState.caret;
    if (!caret) {
      return;
    }
    const auto docIdx = caret->documentIndex();
    if (docIdx >= session.views().size()) {
      return;
    }
    const auto &openView = session.views()[docIdx];
    const auto holes = session.holesForView(static_cast<std::uint32_t>(docIdx));
    const auto caretPos = caret->byteOffset();
    for (const auto &hole : holes) {
      if (hole.isLocked()) {
        if ((caretPos >= hole.charOffset &&
             caretPos <= hole.charOffset + hole.length) ||
            (caret->hasSelection() &&
             caret->selectionStart() < hole.charOffset + hole.length &&
             caret->selectionEnd() > hole.charOffset)) {
          unlockTranscopyright(openView.storeIndex, hole.span);
          break;
        }
      }
    }
  });
}

void Views::openDocumentPalette() {
  using Field = gleditor::Form::Field;
  using Kind  = gleditor::Form::Kind;

  Field choiceField;
  choiceField.label = "Document";
  choiceField.hint  = "select a store, cached publication, or system xanadoc";
  choiceField.kind  = Kind::Choice;

  for (std::uint8_t k = 0;
       k < static_cast<std::uint8_t>(xanadu::SystemDocKind::Count); ++k) {
    const auto kind = static_cast<xanadu::SystemDocKind>(k);
    const auto uri  = std::string(xanadu::systemDocUri(kind));
    std::string desc;
    switch (kind) {
    case xanadu::SystemDocKind::Keymap:
      desc = "Keyboard shortcuts and bindings";
      break;
    case xanadu::SystemDocKind::Settings:
      desc = "Typography and editor preferences";
      break;
    case xanadu::SystemDocKind::Layout:
      desc = "Multi-column layout and ribbons";
      break;
    case xanadu::SystemDocKind::UI:
      desc = "Chrome, status bar, and notifications";
      break;
    case xanadu::SystemDocKind::Pouches:
      desc = "Drop zones and persistent span storage";
      break;
    default:
      break;
    }
    std::string option = "⚙ ";
    option += uri;
    option += " — ";
    option += desc;
    choiceField.options.push_back(std::move(option));
    choiceField.optionValues.push_back(uri);
  }

  std::string downloadError;
  try {
    for (const auto &download : session.publicationInbox().statuses()) {
      const auto identity =
          download.pin ? (download.pin->title.empty()
                              ? std::string("Untitled publication")
                              : download.pin->title) +
                             " / " + download.pin->version.str() + " #" +
                             std::to_string(download.pin->sequence) + " [" +
                             download.pin->hash.hex() + "]"
                       : download.uri;
      choiceField.options.push_back(
          "[Download] " + identity + " — " +
          std::string(publicationDownloadPhaseName(download.phase)));
      choiceField.optionValues.push_back("__download__" + download.id);
    }
  } catch (const std::exception &error) {
    downloadError = "Downloads unavailable: " + std::string(error.what());
  }

  namespace fs = std::filesystem;
  std::error_code ec;
  const auto curPath = fs::current_path(ec);
  if (!ec) {
    for (const auto &dirEntry : fs::directory_iterator(curPath, ec)) {
      if (dirEntry.is_regular_file() &&
          dirEntry.path().extension() == ".xanadoc") {
        choiceField.options.push_back("[Publication] " +
                                      dirEntry.path().filename().string());
        choiceField.optionValues.push_back(dirEntry.path().string());
      }
      if (dirEntry.is_directory()) {
        const auto &p = dirEntry.path();
        if (fs::exists(p / "ops.nodes") || fs::exists(p / "store.tables") ||
            p.extension() == ".xanadoc") {
          const auto dirName = p.filename().string();
          choiceField.options.push_back("[Local] " + dirName);
          choiceField.optionValues.push_back(p.string());
        }
      }
    }
  }

  choiceField.options.emplace_back("Custom path or file...");
  choiceField.optionValues.emplace_back("__custom__");

  Field customPathField;
  customPathField.label = "Custom path";
  customPathField.hint  = "store, signed .xanadoc, or author publication link";

  std::vector<Field> fields;
  fields.push_back(std::move(choiceField));
  fields.push_back(std::move(customPathField));

  form.open(
      "Open Document or System Xanadoc",
      downloadError.empty()
          ? "Select a system xanadoc, local store, or file to open alongside"
          : downloadError,
      std::move(fields), [this](const std::vector<Field> &answers) {
        std::string chosen           = answers[0].answer();
        const std::string customPath = answers[1].answer();
        if (chosen == "__custom__" || !customPath.empty()) {
          chosen = customPath;
        }
        if (chosen.empty()) {
          return;
        }
        openDocumentFromPath(chosen);
      });
}

void Views::openDocumentFromPath(const std::string &chosen) {
  if (chosen.starts_with("__download__")) {
    publicationDownloadStatus(
        chosen.substr(std::string_view("__download__").size()));
    return;
  }
  if (MutableLink::looksLikeMutableLink(chosen)) {
    try {
      const auto id =
          session.publicationInbox().submit(MutableLink::parse(chosen));
      publicationDownloadStatus(id);
    } catch (const std::exception &error) {
      using Field = gleditor::Form::Field;
      Field back;
      back.label         = "Action";
      back.kind          = gleditor::Form::Kind::Choice;
      back.options       = {"Choose another publication"};
      back.submitOnEnter = true;
      form.open("Could not download publication", error.what(),
                {std::move(back)},
                [this](const auto &) { openDocumentPalette(); });
    }
    return;
  }
  if (const auto kind = xanadu::systemDocKindFromUri(chosen)) {
    const auto sIdx = session.systemStoreIndex(*kind);
    auto &sysStore  = session.store(sIdx);
    const auto head = sysStore.primaryCurrentVersion();
    showAlongside(head, 0.0F, sIdx);
    activateNewest();
    std::cout << "xudu: opened system document " << chosen << " (store " << sIdx
              << ")\n";
    return;
  }

  namespace fs = std::filesystem;
  const fs::path p(chosen);
  if (fs::is_regular_file(p) && p.extension() == ".xanadoc") {
    try {
      const auto [sIdx, version] = session.readPublication(chosen);
      showAlongside(version, 0.0F, sIdx);
      activateNewest();
    } catch (const std::exception &err) {
      state->showDialog(render::DiagnosticSeverity::Error,
                        "Could not open publication", err.what());
    }
  } else if (fs::exists(p / "ops.nodes") || fs::exists(p / "store.tables") ||
             fs::is_directory(p)) {
    try {
      const auto sIdx = session.loadAuxiliaryStore(chosen);
      auto &st        = session.store(sIdx);
      const auto head = st.primaryCurrentVersion();
      showAlongside(head, 0.0F, sIdx);
      activateNewest();
      std::cout << "xudu: opened store " << chosen << " (store " << sIdx
                << ")\n";
    } catch (const std::exception &err) {
      state->showDialog(render::DiagnosticSeverity::Error,
                        "Could not open xanadoc", err.what());
    }
  } else if (fs::exists(p)) {
    try {
      const auto [sIdx, imported] = session.importFileToTemporaryStore(chosen);
      showAlongside(imported, 0.0F, sIdx);
      activateNewest();
      std::cout << "xudu: imported file " << chosen << " to temporary store "
                << sIdx << "\n";
    } catch (const std::exception &err) {
      state->showDialog(render::DiagnosticSeverity::Error,
                        "Could not import file", err.what());
    }
  } else {
    state->showDialog(render::DiagnosticSeverity::Error, "Path not found",
                      "The specified path does not exist: " + chosen);
  }
}

void Views::selectDoc(const std::uint32_t index) {
  renderer->runWithState([this, index](RenderState &rState) {
    session.flushUncommitted();
    activateDocument(rState, index);
  });
}

void Views::nextDoc() {
  renderer->runWithState([this](RenderState &rState) {
    if (rState.docs.empty()) {
      return;
    }
    const auto total = static_cast<std::uint32_t>(rState.docs.size());
    const auto cur   = switcher ? switcher->activeDocIndex() : 0U;
    const auto next  = (cur + 1U) % total;
    selectDoc(next);
  });
}

void Views::prevDoc() {
  renderer->runWithState([this](RenderState &rState) {
    if (rState.docs.empty()) {
      return;
    }
    const auto total = static_cast<std::uint32_t>(rState.docs.size());
    const auto cur   = switcher ? switcher->activeDocIndex() : 0U;
    const auto prev  = (cur + total - 1U) % total;
    selectDoc(prev);
  });
}

void Views::printHistory() {
  renderer->runWithState([this](RenderState &) {
    const auto here = session.views().empty() ? MicroversionId{}
                                              : session.views().front().version;
    const auto sIdx =
        session.views().empty() ? 0 : session.views().front().storeIndex;
    const auto &st = session.store(sIdx);
    std::cout << "xudu: " << st.opCount() << " operations, "
              << st.primedia().size() << " bytes of primedia\n";
    for (const auto &id : st.allVersions()) {
      const auto op = st.getOp(id);
      std::cout << (id == here ? "  * " : "    ") << id.str() << "  "
                << (op.has_value() ? xanadu::opKindName(op->kind) : "?")
                << "\n";
    }
  });
}

void Views::setOnionSkin(const bool enabled) {
  renderer->runWithState([this, enabled](RenderState &rState) {
    if (onionSkinMode_ == enabled) {
      return;
    }
    onionSkinMode_ = enabled;
    if (onionSkinMode_) {
      auto *const caret = renderer->editCaret();
      if (caret && caret->active() &&
          caret->documentIndex() < rState.docs.size()) {
        activeOnionIdx_ = caret->documentIndex();
      } else if (switcher) {
        activeOnionIdx_ = switcher->activeDocIndex();
      }
      arrangeOnionSkin(rState);
      state->showDialog(render::DiagnosticSeverity::Info, "3D Onion Skin",
                        "Onion skin mode active: scroll wheel cycles " +
                            std::to_string(rState.docs.size()) + " documents.");
    } else {
      arrangeAlongside(rState);
    }
  });
}

void Views::toggleOnionSkin() { setOnionSkin(!onionSkinMode_); }

void Views::arrangeOnionSkin(RenderState &rState) {
  if (rState.docs.empty()) {
    return;
  }
  const auto total  = rState.docs.size();
  const auto active = activeOnionIdx_ % total;
  for (std::size_t i = 0; i < total; ++i) {
    if (!rState.docs[i]) {
      continue;
    }
    const auto k              = (i - active + total) % total;
    const auto kF             = static_cast<float>(k);
    const glm::vec3 targetPos = kF * kDefaultOnionSkinPolicy.offsetPerVersion;
    const float targetOpacity =
        (k == 0) ? 1.0F
                 : std::max(kDefaultOnionSkinPolicy.minimumOpacity,
                            1.0F - kDefaultOnionSkinPolicy.opacityStep * kF);

    auto *const tl = renderer->animTimeline();
    if (tl) {
      rState.docs[i]->animateMoveTo(*tl, targetPos, gleditor::anim::docArrival);
      rState.docs[i]->animateOpacity(*tl, targetOpacity,
                                     gleditor::anim::docArrival);
    } else {
      rState.docs[i]->setModel(glm::translate(glm::mat4(1.0F), targetPos));
      rState.docs[i]->setImmediateOpacity(targetOpacity);
    }
  }
}

void Views::arrangeAlongside(RenderState &rState) {
  for (std::size_t i = 0; i < rState.docs.size(); ++i) {
    if (!rState.docs[i]) {
      continue;
    }
    const auto slot = AbstractRenderer::documentSlot(i);
    auto *const tl  = renderer->animTimeline();
    if (tl) {
      rState.docs[i]->animateMoveTo(*tl, slot, gleditor::anim::docArrival);
      rState.docs[i]->animateOpacity(*tl, 1.0F, gleditor::anim::docArrival);
    } else {
      rState.docs[i]->setModel(glm::translate(glm::mat4(1.0F), slot));
      rState.docs[i]->setImmediateOpacity(1.0F);
    }
  }
}

void Views::cycleOnionSkin(const int delta) {
  renderer->runWithState([this, delta](RenderState &rState) {
    if (rState.docs.empty()) {
      return;
    }
    const int n = static_cast<int>(rState.docs.size());
    int nextIdx = (static_cast<int>(activeOnionIdx_) + delta) % n;
    if (nextIdx < 0) {
      nextIdx += n;
    }
    activeOnionIdx_ = static_cast<std::size_t>(nextIdx);
    arrangeOnionSkin(rState);
    if (switcher) {
      switcher->setActiveDocIndex(static_cast<std::uint32_t>(activeOnionIdx_));
    }
    auto *const caret = renderer->editCaret();
    if (caret) {
      caret->placeAt(static_cast<std::uint32_t>(activeOnionIdx_), 0);
    }
  });
}

} // namespace xanadu

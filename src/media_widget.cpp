/**
 * @file media_widget.cpp
 * @brief Implementation of the document-embedded interactive media player UI
 *        widget.
 */
#include <gleditor/media_widget.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <format>
#include <iomanip>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <utility>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <gleditor/canvas.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/image_cache.hpp>
#include <gleditor/media.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/text/fit.hpp>
#include <glm/ext/vector_float4.hpp>

namespace gleditor {

namespace {

constexpr std::uint32_t cardBg       = 0x161920F2U; // Dark translucent
constexpr std::uint32_t cardBorder   = 0x2E3442FFU; // Subtle border
constexpr std::uint32_t cardAccent   = 0x5C8DFFFFU; // Active top highlight
constexpr std::uint32_t buttonBg     = 0x242A36FFU; // Button background
constexpr std::uint32_t buttonBorder = 0x3E475CFFU; // Button border
constexpr std::uint32_t textDim      = 0x9AA3B2FFU; // Secondary text
constexpr std::uint32_t textAccent   = 0x5C8DFFFFU; // Playing text
constexpr std::uint32_t textWarn     = 0xF5A623FFU; // Paused text
constexpr std::uint32_t progressBg   = 0x2A313FFFU; // Progress bar track
constexpr std::uint32_t videoAreaBg  = 0x0E1116FFU; // Video frame background

// Full semantic IDs retain a card's namespace; GPU cluster tags hold only
// child offsets and resolve through the captured scene's private scope.
constexpr std::uint32_t firstMediaWidgetId = 1U;
std::atomic<std::uint32_t> nextMediaWidgetId{firstMediaWidgetId};

std::string formatTime(const float totalSeconds) {
  if (totalSeconds < 0.0F || std::isnan(totalSeconds) ||
      std::isinf(totalSeconds)) {
    return "00:00";
  }
  const auto secs = static_cast<int>(
      std::min(static_cast<double>(totalSeconds),
               static_cast<double>(std::numeric_limits<int>::max())));
  const int mins = secs / 60;
  const int rem  = secs % 60;
  std::ostringstream oss;
  oss << std::setfill('0') << std::setw(2) << mins << ":" << std::setw(2)
      << rem;
  return oss.str();
}

int displayedSeconds(const float value) {
  if (!std::isfinite(value) || value < 0.0F) return 0;
  return static_cast<int>(
      std::min(static_cast<double>(value),
               static_cast<double>(std::numeric_limits<int>::max())));
}

} // namespace

MediaWidget::MediaWidget(std::string aFontName,
                         std::shared_ptr<MediaPlayer> aPlayer)
    : fontName_(std::move(aFontName)), player_(std::move(aPlayer)),
      widgetId_(nextMediaWidgetId.fetch_add(1U, std::memory_order_relaxed)),
      tagBase_(widgetId_ << tagSubElementBits) {
  if (nullptr == player_) {
    player_ = std::make_shared<MediaPlayer>();
  }
  initClickables();
}

MediaWidget::~MediaWidget() {
  if (nullptr != device_ && videoTexture_.valid()) {
    device_->destroyTexture(videoTexture_);
  }
}

void MediaWidget::initClickables() {
  clickables_.clear();
  clickables_.registerControl("play", tagPlay, "▶", "Play",
                              [this] { startPlayback(); });
  clickables_.registerControl("pause", tagPause, "⏸", "Pause", [this] {
    if (player_) {
      player_->pause();
    }
  });
  clickables_.registerControl("stop", tagStop, "⏹", "Stop", [this] {
    if (player_) {
      player_->stop();
    }
  });
  clickables_.registerControl(
      "volume", tagVolume,
      [this] { return (player_ && player_->isMuted()) ? "🔈" : "🔊"; },
      [this] { return (player_ && player_->isMuted()) ? "Unmute" : "Mute"; },
      [this] {
        if (player_) {
          player_->setMuted(!player_->isMuted());
        }
      });
  clickables_.registerControl(
      "speed", tagSpeed,
      [this]() -> std::string {
        const float rate = playbackRate();
        if (std::abs(rate - 0.25F) < 0.05F) {
          return "0.25×";
        }
        if (std::abs(rate - 0.5F) < 0.05F) {
          return "0.5×";
        }
        if (std::abs(rate - 1.0F) < 0.05F) {
          return "1.0×";
        }
        if (std::abs(rate - 1.5F) < 0.05F) {
          return "1.5×";
        }
        if (std::abs(rate - 2.0F) < 0.05F) {
          return "2.0×";
        }
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << rate << "×";
        return ss.str();
      },
      [this] {
        std::ostringstream ss;
        ss << "Speed " << std::fixed << std::setprecision(1) << playbackRate()
           << "x";
        return ss.str();
      },
      [this] {
        const float cur = playbackRate();
        float nextRate  = 1.0F;
        if (cur < 0.4F) {
          nextRate = 0.5F;
          // A 6-way speed cycle (0.5, 1.0, 1.5, 2.0, 0.25, back to 1.0);
          // this step and the final else below both land on 1.0F, which is
          // coincidence of the cycle's values, not a copy-paste duplicate.
        } else if (cur < 0.9F) { // NOLINT(bugprone-branch-clone)
          nextRate = 1.0F;
        } else if (cur < 1.4F) {
          nextRate = 1.5F;
        } else if (cur < 1.9F) {
          nextRate = 2.0F;
        } else if (cur < 2.5F) {
          nextRate = 0.25F;
        } else {
          nextRate = 1.0F;
        }
        setPlaybackRate(nextRate);
      },
      a11y::Role::Button, 42.0F);
}

void MediaWidget::setPlaybackRate(const float rate) {
  if (player_) {
    player_->setPlaybackRate(rate);
  }
  revision_++;
}

float MediaWidget::playbackRate() const {
  return player_ ? player_->playbackRate() : 1.0F;
}

void MediaWidget::setPlayer(std::shared_ptr<MediaPlayer> aPlayer) {
  player_ = std::move(aPlayer);
  initClickables();
  geometryRevision_++;
  revision_++;
}

MediaLoad MediaWidget::load(const MediaResourcePtr &resource) {
  if (nullptr == player_) {
    return std::unexpected{MediaError::NoPlayer};
  }
  if (nullptr == resource) {
    return std::unexpected{MediaError::InvalidResource};
  }
  if (title_.empty()) {
    title_ = resource->name();
  }
  revision_++;
  geometryRevision_++;
  return player_->load(resource);
}

MediaLoad MediaWidget::loadFragment(const MediaResourcePtr &resource,
                                    const ByteRange &fragment,
                                    const std::uint64_t containerLength) {
  pendingFragment_.reset();
  if (auto loaded = load(resource); !loaded) {
    return loaded;
  }
  if (containerLength > 0 && fragment.length < containerLength) {
    pendingFragment_        = fragment;
    pendingContainerLength_ = containerLength;
    applyPendingFragment();
  }
  return {};
}

void MediaWidget::applyPendingFragment() {
  if (!pendingFragment_.has_value() || nullptr == player_) {
    return;
  }
  const auto duration = player_->durationSeconds();
  if (duration <= 0.0F) {
    return; // LibVLC has not finished parsing the container's metadata yet.
  }
  const auto range =
      fragmentTimeRange(*pendingFragment_, pendingContainerLength_, duration);
  if (!range.empty()) {
    player_->setTimeRange(range.startSeconds, range.endSeconds);
  }
  pendingFragment_.reset();
}

void MediaWidget::startPlayback() {
  if (nullptr == player_) {
    return;
  }
  player_->play();
  awaitingPlaybackStart_ = true;
}

void MediaWidget::attachToDocument(std::shared_ptr<Doc> aDoc,
                                   const std::uint32_t byteOffset) {
  doc_          = std::move(aDoc);
  docOffset_    = byteOffset;
  explicitPage_ = false;
  screenSpace_  = false;
  revision_++;
}

void MediaWidget::attachToPage(std::shared_ptr<Doc> aDoc,
                               const std::uint32_t pageIndex, const float x,
                               const float y) {
  doc_          = std::move(aDoc);
  pageIndex_    = pageIndex;
  pageX_        = x;
  pageY_        = y;
  explicitPage_ = true;
  screenSpace_  = false;
  revision_++;
}

void MediaWidget::detachFromDocument() {
  doc_.reset();
  revision_++;
}

void MediaWidget::setWorldPosition(const glm::vec3 &worldPos) {
  worldPos_ = worldPos;
  doc_.reset();
  screenSpace_ = false;
  revision_++;
}

void MediaWidget::setScreenPosition(const float x, const float y) {
  screenX_ = x;
  screenY_ = y;
  doc_.reset();
  screenSpace_ = true;
  revision_++;
}

void MediaWidget::setSize(const float width, const float height) {
  width_  = std::max(120.0F, width);
  height_ = std::max(60.0F, height);
  geometryRevision_++;
  revision_++;
}

void MediaWidget::setVisible(const bool visible) {
  visible_ = visible;
  revision_++;
}

void MediaWidget::setTitle(std::string title) {
  title_ = std::move(title);
  geometryRevision_++;
  revision_++;
}

void MediaWidget::deviceReady(render::RenderDevice &device,
                              const render::PipelineDesc &documentPipeline) {
  device_       = &device;
  pipelineDesc_ = documentPipeline;
  drawnFont_    = ui::scaledFontDescription(fontName_, ui::FontRole::Caption,
                                            ui::UiMetrics{}, ui::defaultTheme());
  canvas_       = std::make_unique<Canvas>(&device, drawnFont_);
  liveCanvas_   = std::make_unique<Canvas>(&device, drawnFont_);
  drawnScreenSpace_ = screenSpace_;
  // Embedded in 3D world space uses depth testing; screen overlay turns it off
  canvas_->createPipeline(documentPipeline, !screenSpace_);
  liveCanvas_->createPipeline(documentPipeline, !screenSpace_);
  font_         = text::FontManager::instance().getFont(drawnFont_);
  chromeBuilds_ = 0;
  liveBuiltFor_ = 0;
  pickState_    = nullptr;
}

void MediaWidget::updateVideoTexture() {
  if (nullptr == device_ || nullptr == player_ ||
      !player_->isNewFrameAvailable()) {
    return;
  }
  const auto frame = player_->latestFrame();
  if (!frame || frame->width <= 0 || frame->height <= 0) {
    return;
  }

  if (frame->width != videoFrameWidth_ || frame->height != videoFrameHeight_) {
    if (videoTexture_.valid()) {
      device_->destroyTexture(videoTexture_);
      videoTexture_ = {};
    }
    const int texSize = std::max(frame->width, frame->height);
    videoTexture_ =
        device_->createTextureArray(texSize, 1, render::TextureFormat::RGBA8);
    videoFrameWidth_  = frame->width;
    videoFrameHeight_ = frame->height;
  }
  if (!videoTexture_.valid()) {
    return;
  }

  device_->updateTextureLayer(
      videoTexture_, 0, 0, 0, frame->width, frame->height,
      std::span<const std::byte>(
          reinterpret_cast<const std::byte *>(frame->rgba.data()),
          static_cast<std::size_t>(frame->width) * frame->height * 4));
}

bool MediaWidget::busy() const {
  if (player_ != nullptr) {
    return awaitingPlaybackStart_ ||
           player_->state() == PlaybackState::Opening ||
           player_->state() == PlaybackState::Playing ||
           player_->state() == PlaybackState::Buffering;
  }
  return false;
}

std::optional<MediaWidget::Corner> MediaWidget::bottomLeftOf() const {
  if (nullptr == doc_) {
    return std::nullopt;
  }
  float anchorX         = 0.0F;
  std::uint32_t pageIdx = 0;
  // Filled in below, in pixels, up-positive and measured from the page's
  // own centre -- the same space Page::getModel()'s translation lands in,
  // so pageCenterY + effectiveY*pixelsToWorld needs no further correction.
  float effectiveY = 0.0F;

  if (explicitPage_) {
    pageIdx = pageIndex_;
    anchorX = pageX_;
    // pageY_ is a caller-given distance down from the page's top margin
    // (attachToPage()'s own convention -- see main.cpp's --video/--audio
    // placement), which is a different origin from anchor->y below, though
    // the same up-positive direction: converting means locating the top
    // edge in this same centre-relative pixel space, half the page's own
    // height above centre, then stepping down by pageY_.
    const auto pageObj = doc_->page(pageIdx);
    const float halfHeightPixels =
        (pageObj.has_value()) ? (pageObj->heightPixels() / 2.0F) : 50.0F;
    effectiveY = halfHeightPixels - pageY_;
  } else {
    // The layout engine already decided where this widget's LayoutBox
    // landed -- Doc::boxFor() hands back that box's own bottom-left corner
    // directly, in the same page-pixel space Corner is in, so there is no
    // anchorGapPx arithmetic left to redo here (it is baked into the box's
    // own reserved space via LayoutBox::marginPx, set once by whichever
    // TextSource anchored this offset).
    const auto box = doc_->boxFor(docOffset_);
    if (!box.has_value()) {
      return std::nullopt;
    }
    pageIdx    = box->pageIndex;
    anchorX    = box->x;
    effectiveY = box->y;
  }
  return Corner{.pageIndex = pageIdx, .x = anchorX, .y = effectiveY};
}

std::optional<Doc::Anchor>
MediaWidget::rectFor(const Doc &doc, const std::uint32_t docOffset) const {
  if (explicitPage_ || doc_.get() != &doc || docOffset_ != docOffset) {
    return std::nullopt;
  }
  const auto corner = bottomLeftOf();
  if (!corner.has_value()) {
    return std::nullopt;
  }
  Doc::Anchor rect;
  rect.pageIndex = corner->pageIndex;
  rect.x         = corner->x + (width_ * 0.5F);
  rect.y         = corner->y + (height_ * 0.5F);
  rect.height    = height_;
  return rect;
}

void MediaWidget::drawFrame(FrameContext &ctx) {
  if (!ctx.state.documentsVisible || !visible_ || nullptr == canvas_ ||
      nullptr == player_) {
    return;
  }
  if (awaitingPlaybackStart_ && (player_->state() != PlaybackState::Stopped ||
                                 player_->isNewFrameAvailable())) {
    awaitingPlaybackStart_ = false;
  }
  applyPendingFragment();

  const auto now = std::chrono::steady_clock::now();
  if (lastDrawTime_.time_since_epoch().count() > 0) {
    const float dt = std::chrono::duration<float>(now - lastDrawTime_).count();
    if (dt > 0.0F && dt < 1.0F) {
      player_->update(dt);
    }
  }
  lastDrawTime_ = now;

  glm::mat4 transform{1.0F};

  if (screenSpace_) {
    const auto screenW = static_cast<float>(ctx.screenWidth);
    const auto screenH = static_cast<float>(ctx.screenHeight);
    const auto ortho   = glm::ortho(0.0F, screenW, 0.0F, screenH, -1.0F, 1.0F);
    transform = glm::translate(ortho, glm::vec3{screenX_, screenY_, 0.0F});
  } else if (doc_ != nullptr) {
    if (doc_->isClosing()) {
      return;
    }
    const auto corner = bottomLeftOf();
    if (!corner.has_value()) {
      return;
    }
    const auto pageIdx     = corner->pageIndex;
    const float anchorX    = corner->x;
    const float effectiveY = corner->y;

    const auto pageObj      = doc_->page(pageIdx);
    const float pageCenterY = (pageObj.has_value())
                                  ? pageObj->getModel()[3][1]
                                  : (-100.0F * static_cast<float>(pageIdx));

    const auto docModel = doc_->modelMatrix();
    // Scale from widget layout pixel space to document world space
    const auto widgetModel =
        glm::translate(
            docModel,
            glm::vec3{anchorX * Doc::pixelsToWorld,
                      pageCenterY + (effectiveY * Doc::pixelsToWorld), 0.05F}) *
        glm::scale(glm::mat4(1.0F),
                   glm::vec3{Doc::pixelsToWorld, Doc::pixelsToWorld, 1.0F});
    transform = ctx.viewProjection * widgetModel;
  } else {
    // Standalone world-space position
    const auto worldModel =
        glm::translate(glm::mat4(1.0F), worldPos_) *
        glm::scale(glm::mat4(1.0F),
                   glm::vec3{Doc::pixelsToWorld, Doc::pixelsToWorld, 1.0F});
    transform = ctx.viewProjection * worldModel;
  }

  if (player_->hasVideo()) updateVideoTexture();
  const auto progress = player_->progressFraction();
  scrubber_.setFraction(std::isfinite(progress) ? progress : 0.0F);
  const auto progressStep = static_cast<int>(scrubber_.fraction() * 1000.0);
  const ChromeState next{
      .geometry = geometryRevision_,
      .playback = player_->state(),
      .muted    = player_->isMuted(),
      .video    = player_->hasVideo(),
      .frame    = videoTexture_.valid(),
      .rate     = playbackRate(),
      .seconds  = displayedSeconds(player_->positionSeconds()),
      .duration = displayedSeconds(player_->durationSeconds()),
  };
  const bool presentationChanged =
      metrics_ != ctx.metrics || theme_ != ctx.theme;
  if (chromeBuilds_ == 0 || presentationChanged ||
      drawnScreenSpace_ != screenSpace_) {
    const auto requestedFont = ui::scaledFontDescription(
        fontName_, ui::FontRole::Caption, ctx.metrics, ctx.theme);
    if (requestedFont != drawnFont_ || drawnScreenSpace_ != screenSpace_) {
      drawnFont_        = requestedFont;
      drawnScreenSpace_ = screenSpace_;
      canvas_           = std::make_unique<Canvas>(device_, drawnFont_);
      liveCanvas_       = std::make_unique<Canvas>(device_, drawnFont_);
      canvas_->createPipeline(pipelineDesc_, !screenSpace_);
      liveCanvas_->createPipeline(pipelineDesc_, !screenSpace_);
      font_         = text::FontManager::instance().getFont(drawnFont_);
      liveBuiltFor_ = 0;
    }
    metrics_ = ctx.metrics;
    theme_   = ctx.theme;
  }
  if (pickState_ != &ctx.state) {
    pickState_ = &ctx.state;
    pickScope_ = ctx.state.allocatePersistentOverlayPickScope();
  }
  if (!pickTargets_) {
    auto targets = std::make_shared<std::vector<std::uint32_t>>(tagSeekMax);
    for (std::uint32_t offset = 1; offset <= tagSeekMax; offset++)
      (*targets)[offset - 1] = tagBase_ + offset;
    pickTargets_ = std::move(targets);
  }
  canvas_->setIdentity(pickScope_, 0);
  liveCanvas_->setIdentity(pickScope_, 0);
  ctx.state.bindOverlayWidgets(
      render::packTagIdentity(render::tagKindOverlay, pickScope_, 0),
      pickTargets_);
  if (chromeBuilds_ == 0 || presentationChanged || next != chromeState_ ||
      liveBuiltFor_ == 0) {
    chromeState_ = next;
    canvas_->clear();
    canvas_->pushClip({0.0F, 0.0F, width_, height_});
    layout_ = {.bounds = {0.0F, 0.0F, width_, height_}};
    layout_.boxes.push_back({.id          = tagBase_,
                             .rect        = layout_.bounds,
                             .contentRect = layout_.bounds});
    const float lineHeight = font_->metrics().lineHeight;
    const float padding =
        std::min(std::max(0.0F, theme_.paddingEm * lineHeight),
                 std::min(width_, height_) * 0.1F);
    const float gap =
        std::min(std::max(0.0F, theme_.gapEm * lineHeight), padding);
    const float available = std::max(0.0F, width_ - 2.0F * padding);
    const auto textBox    = [&](ui::Rect box, std::string_view label,
                             std::uint32_t foreground, std::uint32_t background,
                             text::EllipsisAt at = text::EllipsisAt::End) {
      if (box.width <= 0.0F || box.height < lineHeight) return;
      box.bottom += (box.height - lineHeight) * 0.5F;
      box.height         = lineHeight;
      const auto &fitted = shaping_.fitted(
          label, font_,
          {.maxWidthPx = box.width, .maxHeightPx = box.height, .at = at});
      canvas_->addText(ctx.state, box, fitted, foreground, background);
    };

    // 1. Background Card Frame & Borders
    canvas_->setTag(render::tagKindOverlay, 0);
    canvas_->addRect(0.0F, 0.0F, width_, height_, cardBg);
    canvas_->addLine(0.0F, 0.0F, width_, 0.0F, 1.0F, cardBorder);
    canvas_->addLine(0.0F, height_, width_, height_, 1.0F, cardBorder);
    canvas_->addLine(0.0F, 0.0F, 0.0F, height_, 1.0F, cardBorder);
    canvas_->addLine(width_, 0.0F, width_, height_, 1.0F, cardBorder);
    canvas_->addLine(0.0F, height_ - 1.0F, width_, height_ - 1.0F, 2.0F,
                     cardAccent);

    // 2. Header: Title & Playback State Badge
    const auto state = chromeState_.playback;
    std::string stateBadge;
    std::uint32_t stateColor = textDim;

    switch (state) {
    case PlaybackState::Playing:
      stateBadge = "▶ Playing";
      stateColor = textAccent;
      break;
    case PlaybackState::Paused:
      stateBadge = "⏸ Paused";
      stateColor = textWarn;
      break;
    case PlaybackState::Buffering:
    case PlaybackState::Opening:
      stateBadge = "⟳ Loading";
      stateColor = textAccent;
      break;
    case PlaybackState::Ended:
      stateBadge = "⏹ Ended";
      stateColor = textDim;
      break;
    case PlaybackState::Error:
      stateBadge = "⚠ Error";
      stateColor = 0xFF5555FFU;
      break;
    case PlaybackState::Stopped:
    default:
      stateBadge = "⏹ Stopped";
      stateColor = textDim;
      break;
    }

    const auto badgeWidth    = shaping_.fitted(stateBadge, font_, {}).widthPx;
    const float headerHeight = std::min(
        std::ceil(lineHeight) + 2, std::max(0.0F, height_ - 2.0F * padding));
    const std::array<ui::LayoutItem, 2> headerItems{{
        {.id        = tagBase_ + tagSeekMax + 1U,
         .intrinsic = {available, lineHeight},
         .grow      = 1.0F},
        {.id        = tagBase_ + tagSeekMax + 2U,
         .intrinsic = {badgeWidth, lineHeight},
         .minimum   = {badgeWidth, lineHeight}},
    }};
    const auto header = ui::flow(
        {padding, height_ - padding - headerHeight, available, headerHeight},
        headerItems, {.wrap = false, .gap = gap, .align = ui::Align::Stretch});
    layout_.append(header);
    textBox(header.boxes[0].rect, title_.empty() ? "Media Player" : title_,
            ui::rgba(theme_.colours.text), cardBg);
    textBox(header.boxes[1].rect, stateBadge, stateColor, cardBg);

    const float buttonHeight = std::min(
        std::max(lineHeight + 2.0F * gap, metrics_.px(theme_.type.minTouchPx)),
        std::max(0.0F, height_ - 2.0F * padding - headerHeight - gap));
    const std::string timeStr = formatTime(chromeState_.seconds) + " / " +
                                formatTime(chromeState_.duration);
    const auto timeWidth = shaping_.fitted(timeStr, font_, {}).widthPx;
    std::vector<ui::LayoutItem> controls;
    controls.reserve(clickables_.controls().size() + 1);
    for (const auto &ctrl : clickables_.controls()) {
      controls.push_back(
          {.id        = tagBase_ + ctrl.tagOffset,
           .intrinsic = {std::max(metrics_.px(ctrl.width),
                                  metrics_.px(theme_.type.minTouchPx)),
                         buttonHeight},
           .focusable = true,
           .paddingPx = gap * 0.5F});
    }
    controls.push_back({.id        = tagBase_ + tagSeekBase,
                        .intrinsic = {timeWidth, buttonHeight},
                        .grow      = 1.0F,
                        .focusable = true});
    const auto footer =
        ui::flow({padding, padding, available, buttonHeight}, controls,
                 {.wrap = false, .gap = gap, .align = ui::Align::Stretch});
    layout_.append(footer);
    const auto &seekBox = footer.boxes.back().rect;
    seekRect_ = {seekBox.left, seekBox.bottom + lineHeight + gap * 0.5F,
                 seekBox.width,
                 std::max(0.0F, seekBox.height - lineHeight - gap * 0.5F)};
    viewport_ = {padding, padding + buttonHeight + gap, available,
                 std::max(0.0F, height_ - 2.0F * padding - buttonHeight -
                                    headerHeight - 2.0F * gap)};

    // 3. Video Viewport / Audio Badge Area
    if (viewport_.height > 0.0F) {
      const auto background = chromeState_.video ? videoAreaBg : cardBg;
      canvas_->addRect(viewport_.left, viewport_.bottom, viewport_.width,
                       viewport_.height, background);
      if (!chromeState_.video || !chromeState_.frame) {
        textBox(viewport_,
                chromeState_.video ? "🎬 Video Surface" : "♪ Audio Media Track",
                ui::rgba(theme_.colours.muted), background);
      }
    }

    // 4. Interactive Control Buttons: Play, Pause, Stop, Volume (and registered
    // controls)
    for (const auto &ctrl : clickables_.controls()) {
      const auto *box = layout_.find(tagBase_ + ctrl.tagOffset);
      if (!box) continue;
      const float btnX = box->rect.left;
      const float btnY = box->rect.bottom;
      const float btnW = box->rect.width;
      const float btnH = box->rect.height;
      canvas_->setTag(render::tagKindOverlay, ctrl.tagOffset);
      canvas_->addRect(btnX, btnY, btnW, btnH, buttonBg);
      canvas_->addLine(btnX, btnY, btnX + btnW, btnY, 1.0F, buttonBorder);
      canvas_->addLine(btnX, btnY + btnH, btnX + btnW, btnY + btnH, 1.0F,
                       buttonBorder);
      const std::string label = ctrl.getLabel ? ctrl.getLabel() : ctrl.id;
      textBox(box->contentRect, label, ui::rgba(theme_.colours.text), buttonBg);
    }

    // 5. Progress & Seek Bar
    // Pick tags belong to the horizontal location, allowing a click to seek to
    // that location. The track is retained; progress does not rebuild its tags.
    const auto segments = static_cast<unsigned int>(
        std::clamp(std::floor(seekRect_.width), 1.0F, 1001.0F));
    for (unsigned int i = 0; i < segments; i++) {
      const auto start = static_cast<float>(
          std::floor(static_cast<double>(i) * seekRect_.width / segments));
      const auto end = static_cast<float>(
          std::floor(static_cast<double>(i + 1) * seekRect_.width / segments));
      const auto fraction =
          segments > 1
              ? static_cast<unsigned int>(
                    (static_cast<float>(i) / static_cast<float>(segments - 1)) *
                    1000.0F)
              : 0U;
      canvas_->setTag(render::tagKindOverlay, tagSeekBase + fraction);
      canvas_->addRect(seekRect_.left + start, seekRect_.bottom, end - start,
                       seekRect_.height, progressBg);
    }
    canvas_->setTag(render::tagKindOverlay, tagSeekBase);
    textBox({seekBox.left, seekBox.bottom, seekBox.width,
             std::min(lineHeight, seekBox.height)},
            timeStr, ui::rgba(theme_.colours.muted), cardBg);
    canvas_->popClip();
    canvas_->commit();
    controlBounds_.resize(clickables_.controls().size());
    ++chromeBuilds_;
    ++revision_;
  }
  if (liveBuiltFor_ != chromeBuilds_ || liveProgress_ != progressStep ||
      liveVideoWidth_ != videoFrameWidth_ ||
      liveVideoHeight_ != videoFrameHeight_) {
    liveProgress_ = progressStep;
    rebuildLive();
  }
  updateAccessibilityBounds(transform, ctx.screenWidth, ctx.screenHeight);
  canvas_->draw(ctx.state, transform);
  liveCanvas_->draw(ctx.state, transform);
}

void MediaWidget::rebuildLive() {
  liveCanvas_->clear();
  liveCanvas_->pushClip({0.0F, 0.0F, width_, height_});
  liveCanvas_->setTag(render::tagKindOverlay, 0);
  if (chromeState_.video && videoTexture_.valid() && videoFrameWidth_ > 0 &&
      videoFrameHeight_ > 0 && viewport_.width > 0.0F &&
      viewport_.height > 0.0F) {
    const auto texSize =
        static_cast<float>(std::max(videoFrameWidth_, videoFrameHeight_));
    const float aspect = static_cast<float>(videoFrameWidth_) /
                         static_cast<float>(videoFrameHeight_);
    const float drawWidth =
        std::min(viewport_.width, viewport_.height * aspect);
    const float drawHeight = drawWidth / aspect;
    ImageResource frame;
    frame.width   = videoFrameWidth_;
    frame.height  = videoFrameHeight_;
    frame.layer   = 0;
    frame.u1      = static_cast<float>(videoFrameWidth_) / texSize;
    frame.v1      = static_cast<float>(videoFrameHeight_) / texSize;
    frame.texture = videoTexture_;
    liveCanvas_->addImage(viewport_.left + (viewport_.width - drawWidth) * 0.5F,
                          viewport_.bottom +
                              (viewport_.height - drawHeight) * 0.5F,
                          drawWidth, drawHeight, frame, 0xFFFFFFFFU);
  }
  if (seekRect_.width > 0.0F && seekRect_.height > 0.0F) {
    const float fraction = static_cast<float>(liveProgress_) / 1000.0F;
    const float fill     = seekRect_.width * fraction;
    const auto segments  = static_cast<unsigned int>(
        std::clamp(std::floor(seekRect_.width), 1.0F, 1001.0F));
    for (unsigned int i = 0; i < segments; i++) {
      const auto left = static_cast<float>(
          std::floor(static_cast<double>(i) * seekRect_.width / segments));
      const float right =
          std::min(static_cast<float>(std::floor(static_cast<double>(i + 1) *
                                                 seekRect_.width / segments)),
                   fill);
      if (left >= right) break;
      const auto pickFraction =
          segments > 1
              ? static_cast<unsigned int>(
                    (static_cast<float>(i) / static_cast<float>(segments - 1)) *
                    1000.0F)
              : 0U;
      liveCanvas_->setTag(render::tagKindOverlay, tagSeekBase + pickFraction);
      liveCanvas_->addRect(seekRect_.left + left, seekRect_.bottom,
                           right - left, seekRect_.height,
                           ui::rgba(theme_.colours.accent));
    }
    if (fraction > 0.0F) {
      liveCanvas_->setTag(render::tagKindOverlay,
                          tagSeekBase +
                              static_cast<std::uint32_t>(liveProgress_));
      const float knobWidth = std::min(4.0F, seekRect_.width);
      liveCanvas_->addRect(
          std::clamp(seekRect_.left + fill - knobWidth * 0.5F, seekRect_.left,
                     seekRect_.left + seekRect_.width - knobWidth),
          seekRect_.bottom, knobWidth, seekRect_.height,
          ui::rgba(theme_.colours.text));
    }
  }
  liveCanvas_->popClip();
  liveCanvas_->commit();
  liveBuiltFor_    = chromeBuilds_;
  liveVideoWidth_  = videoFrameWidth_;
  liveVideoHeight_ = videoFrameHeight_;
}

void MediaWidget::updateAccessibilityBounds(const glm::mat4 &transform,
                                            const int width, const int height) {
  const auto project = [&](const ui::Rect rect) -> std::optional<a11y::Rect> {
    std::optional<a11y::Rect> bounds;
    for (const float x : {rect.left, rect.left + rect.width}) {
      for (const float y : {rect.bottom, rect.bottom + rect.height}) {
        const auto point = transform * glm::vec4{x, y, 0.0F, 1.0F};
        if (point.w <= 0.0F || !std::isfinite(point.w)) return std::nullopt;
        const double screenX = (point.x / point.w + 1.0) * width * 0.5;
        const double screenY = (1.0 - point.y / point.w) * height * 0.5;
        if (!std::isfinite(screenX) || !std::isfinite(screenY))
          return std::nullopt;
        if (!bounds) {
          bounds = a11y::Rect{screenX, screenY, screenX, screenY};
        } else {
          bounds->left   = std::min(bounds->left, screenX);
          bounds->right  = std::max(bounds->right, screenX);
          bounds->top    = std::min(bounds->top, screenY);
          bounds->bottom = std::max(bounds->bottom, screenY);
        }
      }
    }
    return bounds;
  };
  bool changed      = false;
  const auto assign = [&](std::optional<a11y::Rect> &held,
                          const ui::Rect rect) {
    const auto next = project(rect);
    if (held != next) {
      held    = next;
      changed = true;
    }
  };
  assign(rootBounds_, layout_.bounds);
  if (const auto *seek = layout_.find(tagBase_ + tagSeekBase))
    assign(seekBounds_, seek->rect);
  std::size_t index = 0;
  for (const auto &ctrl : clickables_.controls()) {
    if (const auto *box = layout_.find(tagBase_ + ctrl.tagOffset))
      assign(controlBounds_[index], box->rect);
    ++index;
  }
  if (changed) ++revision_;
}

bool MediaWidget::picked(const render::PickingResult &pick,
                         [[maybe_unused]] RenderState &state) {
  if (!visible_ || pick.tag.kind != render::tagKindOverlay || !player_) {
    return false;
  }

  std::uint32_t tag{};
  if (pick.tag.docIndex == pickScope_ && pick.tag.pageIndex == 0 &&
      pickScope_ != 0) {
    if (pick.tag.clusterIndex == 0) return true;
    if (pick.requestId != 0) {
      if (!pick.overlayWidgetId) return false;
      tag = *pick.overlayWidgetId;
    } else {
      if (pick.tag.clusterIndex > tagSeekMax || !pickTargets_) return false;
      tag = (*pickTargets_)[pick.tag.clusterIndex - 1];
    }
  } else {
    // Direct synthetic calls predate captured scenes and use full semantic
    // tags. A real asynchronous result must always match this card's scope.
    if (pick.requestId != 0 || pick.tag.docIndex != 0 ||
        pick.tag.pageIndex != 0 || pick.overlayWidgetId)
      return false;
    tag = pick.tag.clusterIndex;
  }
  if (tag < tagBase_ || tag > tagBase_ + tagSeekMax || nullptr == player_) {
    return false;
  }

  const auto offset = tag - tagBase_;

  if (clickables_.dispatch(offset)) {
    revision_++;
    return true;
  }
  if (offset >= tagSeekBase && offset <= tagSeekMax) {
    const auto fraction = static_cast<float>(offset - tagSeekBase) / 1000.0F;
    scrubber_.setFraction(fraction);
    player_->seekFraction(static_cast<float>(scrubber_.fraction()));
    revision_++;
    return true;
  }

  // Click on background card consumes click without moving document caret
  return true;
}

void MediaWidget::describe(a11y::Builder &into) {
  if (!visible_ || nullptr == player_) {
    return;
  }

  const auto rootId = static_cast<std::uint64_t>(tagBase_);
  std::vector<std::uint64_t> children;
  children.reserve(clickables_.controls().size() + 1);

  std::size_t index = 0;
  for (const auto &ctrl : clickables_.controls()) {
    const auto ctrlId = rootId + ctrl.tagOffset;
    auto &ctrlNode    = into.add(ctrlId, ctrl.role);
    ctrlNode.label    = ctrl.getA11yLabel ? ctrl.getA11yLabel() : ctrl.id;
    ctrlNode.actions  = a11y::bit(a11y::Action::Click);
    if (index < controlBounds_.size()) ctrlNode.bounds = controlBounds_[index];
    ++index;
    children.push_back(into.id(ctrlId));
  }

  // Seek / Position Info
  const auto seekId = rootId + tagSeekBase;
  auto &seekNode    = into.add(seekId, a11y::Role::Label);
  seekNode.label    = "Playback Position";
  seekNode.value    = formatTime(player_->positionSeconds()) + " of " +
                   formatTime(player_->durationSeconds());
  seekNode.actions =
      a11y::bit(a11y::Action::Click) | a11y::bit(a11y::Action::SetValue);
  seekNode.bounds = seekBounds_;
  children.push_back(into.id(seekId));

  auto &mediaNode    = into.add(rootId, a11y::Role::Group);
  mediaNode.label    = title_.empty() ? "Media Player" : title_;
  mediaNode.bounds   = rootBounds_;
  mediaNode.children = std::move(children);

  into.contribute(into.id(rootId));
}

bool MediaWidget::performAction(const std::uint64_t nodeId,
                                const a11y::Action action,
                                const std::string_view value) {
  if (nullptr == player_) {
    return false;
  }
  const auto rootId = static_cast<std::uint64_t>(tagBase_);
  const auto local  = a11y::Ids::localOf(nodeId);
  if (local == rootId + tagSeekBase && action == a11y::Action::SetValue) {
    if (value.empty()) return false;
    double fraction{};
    const auto parsed =
        std::from_chars(value.data(), value.data() + value.size(), fraction);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        !std::isfinite(fraction))
      return false;
    ui::Scrubber control;
    control.setFraction(fraction);
    player_->seekFraction(static_cast<float>(control.fraction()));
    ++revision_;
    return true;
  }
  if (action != a11y::Action::Click) return false;
  if (local >= rootId && local <= rootId + tagSubElementMask) {
    const auto offset = static_cast<std::uint32_t>(local - rootId);
    if (clickables_.dispatch(offset)) {
      revision_++;
      return true;
    }
  }
  return false;
}

} // namespace gleditor

/**
 * @file media_widget.cpp
 * @brief Implementation of the document-embedded interactive media player UI
 *        widget.
 */
#include <gleditor/media_widget.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <iomanip>
#include <iostream>
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

namespace gleditor {

namespace {

constexpr std::uint32_t cardBg       = 0x161920F2U; // Dark translucent
constexpr std::uint32_t cardBorder   = 0x2E3442FFU; // Subtle border
constexpr std::uint32_t cardAccent   = 0x5C8DFFFFU; // Active top highlight
constexpr std::uint32_t buttonBg     = 0x242A36FFU; // Button background
constexpr std::uint32_t buttonBorder = 0x3E475CFFU; // Button border
constexpr std::uint32_t buttonText   = 0xFFFFFFFFU; // Button label
constexpr std::uint32_t textDim      = 0x9AA3B2FFU; // Secondary text
constexpr std::uint32_t textAccent   = 0x5C8DFFFFU; // Playing text
constexpr std::uint32_t textWarn     = 0xF5A623FFU; // Paused text
constexpr std::uint32_t progressBg   = 0x2A313FFFU; // Progress bar track
constexpr std::uint32_t progressBar  = 0x5C8DFFFFU; // Elapsed progress
constexpr std::uint32_t videoAreaBg  = 0x0E1116FFU; // Video frame background

std::string formatTime(const float totalSeconds) {
  if (totalSeconds < 0.0F || std::isnan(totalSeconds) ||
      std::isinf(totalSeconds)) {
    return "00:00";
  }
  const auto secs = static_cast<int>(totalSeconds);
  const int mins  = secs / 60;
  const int rem   = secs % 60;
  std::ostringstream oss;
  oss << std::setfill('0') << std::setw(2) << mins << ":" << std::setw(2)
      << rem;
  return oss.str();
}

} // namespace

MediaWidget::MediaWidget(std::string aFontName,
                         std::shared_ptr<MediaPlayer> aPlayer)
    : fontName_(std::move(aFontName)), player_(std::move(aPlayer)) {
  if (nullptr == player_) {
    player_ = std::make_shared<MediaPlayer>();
  }
}

MediaWidget::~MediaWidget() {
  if (nullptr != device_ && videoTexture_.valid()) {
    device_->destroyTexture(videoTexture_);
  }
}

void MediaWidget::setPlayer(std::shared_ptr<MediaPlayer> aPlayer) {
  player_ = std::move(aPlayer);
  revision_++;
}

bool MediaWidget::load(MediaResourcePtr resource) {
  if (player_ != nullptr && resource != nullptr) {
    if (title_.empty()) {
      title_ = resource->name();
    }
    revision_++;
    return player_->load(std::move(resource));
  }
  return false;
}

bool MediaWidget::loadFragment(MediaResourcePtr resource,
                               const ByteRange &fragment,
                               const std::uint64_t containerLength) {
  pendingFragment_.reset();
  if (!load(std::move(resource))) {
    return false;
  }
  if (containerLength > 0 && fragment.length < containerLength) {
    pendingFragment_        = fragment;
    pendingContainerLength_ = containerLength;
  }
  return true;
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
  revision_++;
}

void MediaWidget::setVisible(const bool visible) {
  visible_ = visible;
  revision_++;
}

void MediaWidget::setTitle(std::string title) {
  title_ = std::move(title);
  revision_++;
}

void MediaWidget::deviceReady(render::RenderDevice &device,
                              const render::PipelineDesc &documentPipeline) {
  device_ = &device;
  canvas_ = std::make_unique<Canvas>(&device, fontName_);
  // Embedded in 3D world space uses depth testing; screen overlay turns it off
  canvas_->createPipeline(documentPipeline, !screenSpace_);
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
    const auto *const pageObj = doc_->page(pageIdx);
    const float halfHeightPixels =
        (pageObj != nullptr) ? (pageObj->heightPixels() / 2.0F) : 50.0F;
    effectiveY = halfHeightPixels - pageY_;
  } else {
    const auto anchor = doc_->anchorFor(docOffset_);
    if (!anchor.has_value()) {
      return std::nullopt;
    }
    pageIdx = anchor->pageIndex;
    anchorX = anchor->x;
    // Page::caretGeometry (src/doc.cpp) hands back Y increasing upward from
    // the page's own centre -- posY = originY - (top + height/2), with
    // originY the page's top edge in that same up-positive space -- so
    // moving below the anchor line is *subtracting* a pixel offset here,
    // not negating and re-adding one as if Y increased downward from the
    // page's top margin.
    effectiveY = anchor->y - (height_ + anchorGapPx);
  }
  return Corner{pageIdx, anchorX, effectiveY};
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
  if (!visible_ || nullptr == canvas_ || nullptr == player_) {
    return;
  }
  if (awaitingPlaybackStart_ && (player_->state() != PlaybackState::Stopped ||
                                 player_->isNewFrameAvailable())) {
    awaitingPlaybackStart_ = false;
  }
  applyPendingFragment();

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

    const auto *const pageObj = doc_->page(pageIdx);
    const float pageCenterY   = (pageObj != nullptr)
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
    canvas_->setIdentity(doc_->documentIndex(), pageIdx);
  } else {
    // Standalone world-space position
    const auto worldModel =
        glm::translate(glm::mat4(1.0F), worldPos_) *
        glm::scale(glm::mat4(1.0F),
                   glm::vec3{Doc::pixelsToWorld, Doc::pixelsToWorld, 1.0F});
    transform = ctx.viewProjection * worldModel;
  }

  canvas_->clear();

  // 1. Background Card Frame & Borders
  canvas_->setTag(render::tagKindOverlay, tagBase_);
  canvas_->addRect(0.0F, 0.0F, width_, height_, cardBg);
  canvas_->addLine(0.0F, 0.0F, width_, 0.0F, 1.0F, cardBorder);
  canvas_->addLine(0.0F, height_, width_, height_, 1.0F, cardBorder);
  canvas_->addLine(0.0F, 0.0F, 0.0F, height_, 1.0F, cardBorder);
  canvas_->addLine(width_, 0.0F, width_, height_, 1.0F, cardBorder);
  canvas_->addLine(0.0F, height_ - 1.0F, width_, height_ - 1.0F, 2.0F,
                   cardAccent);

  // 2. Header: Title & Playback State Badge
  std::string displayTitle = title_.empty() ? "Media Player" : title_;
  canvas_->setTextWidthLimit(static_cast<int>(width_ - 110.0F));
  canvas_->addText(ctx.state, 12.0F, height_ - 10.0F, displayTitle, 0xFFFFFFFFU,
                   cardBg);
  canvas_->setTextWidthLimit(0);

  const auto state = player_->state();
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

  canvas_->addText(ctx.state, width_ - 95.0F, height_ - 10.0F, stateBadge,
                   stateColor, cardBg);

  // 3. Video Viewport / Audio Badge Area
  const float mediaAreaLeft   = 12.0F;
  const float mediaAreaBottom = 54.0F;
  const float mediaAreaWidth  = width_ - chromeWidthPx;
  const float mediaAreaHeight = height_ - chromeHeightPx;

  if (mediaAreaHeight > 10.0F) {
    if (player_->hasVideo()) {
      canvas_->addRect(mediaAreaLeft, mediaAreaBottom, mediaAreaWidth,
                       mediaAreaHeight, videoAreaBg);
      updateVideoTexture();
      if (videoTexture_.valid() && videoFrameWidth_ > 0 &&
          videoFrameHeight_ > 0) {
        // Letterboxed within the viewport rather than stretched: a frame
        // whose aspect does not match the card would otherwise distort.
        const float texSize =
            static_cast<float>(std::max(videoFrameWidth_, videoFrameHeight_));
        const float frameAspect = static_cast<float>(videoFrameWidth_) /
                                  static_cast<float>(videoFrameHeight_);
        const float areaAspect  = mediaAreaWidth / mediaAreaHeight;
        float drawW             = mediaAreaWidth;
        float drawH             = mediaAreaHeight;
        if (frameAspect > areaAspect) {
          drawH = mediaAreaWidth / frameAspect;
        } else {
          drawW = mediaAreaHeight * frameAspect;
        }
        const float drawX = mediaAreaLeft + ((mediaAreaWidth - drawW) / 2.0F);
        const float drawY =
            mediaAreaBottom + ((mediaAreaHeight - drawH) / 2.0F);

        ImageResource frameResource;
        frameResource.width   = videoFrameWidth_;
        frameResource.height  = videoFrameHeight_;
        frameResource.layer   = 0;
        frameResource.u0      = 0.0F;
        frameResource.v0      = 0.0F;
        frameResource.u1      = static_cast<float>(videoFrameWidth_) / texSize;
        frameResource.v1      = static_cast<float>(videoFrameHeight_) / texSize;
        frameResource.texture = videoTexture_;
        canvas_->addImage(drawX, drawY, drawW, drawH, frameResource,
                          0xFFFFFFFFU);
      } else {
        canvas_->addText(ctx.state, mediaAreaLeft + 8.0F,
                         mediaAreaBottom + (mediaAreaHeight / 2.0F) + 6.0F,
                         "🎬 Video Surface", textDim, videoAreaBg);
      }
    } else {
      canvas_->addRect(mediaAreaLeft, mediaAreaBottom, mediaAreaWidth,
                       mediaAreaHeight, 0x1A202BF0U);
      canvas_->addText(ctx.state, mediaAreaLeft + 8.0F,
                       mediaAreaBottom + (mediaAreaHeight / 2.0F) + 6.0F,
                       "♪ Audio Media Track", textDim, 0x1A202BF0U);
    }
  }

  // 4. Interactive Control Buttons: Play, Pause, Stop, Volume
  const float btnY = 12.0F;
  const float btnH = 26.0F;
  const float btnW = 32.0F;
  float btnX       = 12.0F;

  // Play button [▶]
  canvas_->setTag(render::tagKindOverlay, tagBase_ + tagPlay);
  canvas_->addRect(btnX, btnY, btnW, btnH, buttonBg);
  canvas_->addLine(btnX, btnY, btnX + btnW, btnY, 1.0F, buttonBorder);
  canvas_->addLine(btnX, btnY + btnH, btnX + btnW, btnY + btnH, 1.0F,
                   buttonBorder);
  canvas_->addText(ctx.state, btnX + 11.0F, btnY + btnH - 6.0F, "▶", buttonText,
                   buttonBg);
  btnX += btnW + 6.0F;

  // Pause button [⏸]
  canvas_->setTag(render::tagKindOverlay, tagBase_ + tagPause);
  canvas_->addRect(btnX, btnY, btnW, btnH, buttonBg);
  canvas_->addLine(btnX, btnY, btnX + btnW, btnY, 1.0F, buttonBorder);
  canvas_->addLine(btnX, btnY + btnH, btnX + btnW, btnY + btnH, 1.0F,
                   buttonBorder);
  canvas_->addText(ctx.state, btnX + 10.0F, btnY + btnH - 6.0F, "⏸", buttonText,
                   buttonBg);
  btnX += btnW + 6.0F;

  // Stop button [⏹]
  canvas_->setTag(render::tagKindOverlay, tagBase_ + tagStop);
  canvas_->addRect(btnX, btnY, btnW, btnH, buttonBg);
  canvas_->addLine(btnX, btnY, btnX + btnW, btnY, 1.0F, buttonBorder);
  canvas_->addLine(btnX, btnY + btnH, btnX + btnW, btnY + btnH, 1.0F,
                   buttonBorder);
  canvas_->addText(ctx.state, btnX + 10.0F, btnY + btnH - 6.0F, "⏹", buttonText,
                   buttonBg);
  btnX += btnW + 6.0F;

  // Volume button [🔊 / 🔈]
  canvas_->setTag(render::tagKindOverlay, tagBase_ + tagVolume);
  canvas_->addRect(btnX, btnY, btnW, btnH, buttonBg);
  canvas_->addLine(btnX, btnY, btnX + btnW, btnY, 1.0F, buttonBorder);
  canvas_->addLine(btnX, btnY + btnH, btnX + btnW, btnY + btnH, 1.0F,
                   buttonBorder);
  canvas_->addText(ctx.state, btnX + 7.0F, btnY + btnH - 6.0F,
                   player_->isMuted() ? "🔈" : "🔊", buttonText, buttonBg);
  btnX += btnW + 12.0F;

  // 5. Progress & Seek Bar
  const float barLeft   = btnX;
  const float barRight  = width_ - 12.0F;
  const float barWidth  = std::max(20.0F, barRight - barLeft);
  const float barBottom = 26.0F;
  const float barHeight = 8.0F;

  const float progress = player_->progressFraction();
  const auto curSecs   = player_->positionSeconds();
  const auto durSecs   = player_->durationSeconds();

  // Draw seek bar with interactive picking segments
  canvas_->setTag(render::tagKindOverlay,
                  tagBase_ + tagSeekBase +
                      static_cast<std::uint32_t>(progress * 1000.0F));
  canvas_->addRect(barLeft, barBottom, barWidth, barHeight, progressBg);

  if (progress > 0.0F) {
    const float fillW = barWidth * progress;
    canvas_->addRect(barLeft, barBottom, fillW, barHeight, progressBar);
    // Playhead knob
    canvas_->addRect(barLeft + fillW - 2.0F, barBottom - 2.0F, 4.0F,
                     barHeight + 4.0F, 0xFFFFFFFFU);
  }

  // Time readout label: "00:15 / 01:30"
  const std::string timeStr = formatTime(curSecs) + " / " + formatTime(durSecs);
  canvas_->addText(ctx.state, barLeft, barBottom - 2.0F, timeStr, textDim,
                   cardBg);

  canvas_->commit();
  canvas_->draw(ctx.state, transform);
}

bool MediaWidget::picked(const render::PickingResult &pick,
                         [[maybe_unused]] RenderState &state) {
  if (!visible_ || pick.tag.kind != render::tagKindOverlay) {
    return false;
  }

  const auto tag = pick.tag.clusterIndex;
  if (tag < tagBase_ || tag > tagBase_ + tagSeekMax || nullptr == player_) {
    return false;
  }

  const auto offset = tag - tagBase_;

  if (offset == tagPlay) {
    startPlayback();
    revision_++;
    return true;
  }
  if (offset == tagPause) {
    player_->pause();
    revision_++;
    return true;
  }
  if (offset == tagStop) {
    player_->stop();
    revision_++;
    return true;
  }
  if (offset == tagVolume) {
    player_->setMuted(!player_->isMuted());
    revision_++;
    return true;
  }
  if (offset >= tagSeekBase && offset <= tagSeekMax) {
    const auto fraction = static_cast<float>(offset - tagSeekBase) / 1000.0F;
    player_->seekFraction(fraction);
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
  auto &mediaNode   = into.add(rootId, a11y::Role::Group);
  mediaNode.label   = title_.empty() ? "Media Player" : title_;

  // Play Button
  const auto playId = rootId + tagPlay;
  auto &playNode    = into.add(playId, a11y::Role::Button);
  playNode.label    = "Play";
  playNode.actions  = a11y::bit(a11y::Action::Click);
  mediaNode.children.push_back(into.id(playId));

  // Pause Button
  const auto pauseId = rootId + tagPause;
  auto &pauseNode    = into.add(pauseId, a11y::Role::Button);
  pauseNode.label    = "Pause";
  pauseNode.actions  = a11y::bit(a11y::Action::Click);
  mediaNode.children.push_back(into.id(pauseId));

  // Stop Button
  const auto stopId = rootId + tagStop;
  auto &stopNode    = into.add(stopId, a11y::Role::Button);
  stopNode.label    = "Stop";
  stopNode.actions  = a11y::bit(a11y::Action::Click);
  mediaNode.children.push_back(into.id(stopId));

  // Volume Button
  const auto volId = rootId + tagVolume;
  auto &volNode    = into.add(volId, a11y::Role::Button);
  volNode.label    = player_->isMuted() ? "Unmute" : "Mute";
  volNode.actions  = a11y::bit(a11y::Action::Click);
  mediaNode.children.push_back(into.id(volId));

  // Seek / Position Info
  const auto seekId = rootId + tagSeekBase;
  auto &seekNode    = into.add(seekId, a11y::Role::Label);
  seekNode.label    = "Playback Position";
  seekNode.value    = formatTime(player_->positionSeconds()) + " of " +
                      formatTime(player_->durationSeconds());
  seekNode.actions  = a11y::bit(a11y::Action::Click);
  mediaNode.children.push_back(into.id(seekId));

  into.contribute(into.id(rootId));
}

bool MediaWidget::performAction(const std::uint64_t nodeId,
                                const a11y::Action action,
                                [[maybe_unused]] const std::string_view value) {
  if (action != a11y::Action::Click || nullptr == player_) {
    return false;
  }
  const auto rootId = static_cast<std::uint64_t>(tagBase_);
  if (nodeId == rootId + tagPlay) {
    startPlayback();
    revision_++;
    return true;
  }
  if (nodeId == rootId + tagPause) {
    player_->pause();
    revision_++;
    return true;
  }
  if (nodeId == rootId + tagStop) {
    player_->stop();
    revision_++;
    return true;
  }
  if (nodeId == rootId + tagVolume) {
    player_->setMuted(!player_->isMuted());
    revision_++;
    return true;
  }
  return false;
}

} // namespace gleditor

/**
 * @file media.cpp
 * @brief Implementation of generic media resources, streams, and player via
 *        LibVLC.
 */
#include <gleditor/media.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <vlc/vlc.h>

namespace gleditor {

const char *playbackStateName(const PlaybackState state) {
  switch (state) {
  case PlaybackState::Stopped:
    return "Stopped";
  case PlaybackState::Playing:
    return "Playing";
  case PlaybackState::Paused:
    return "Paused";
  case PlaybackState::Opening:
    return "Opening";
  case PlaybackState::Buffering:
    return "Buffering";
  case PlaybackState::Ended:
    return "Ended";
  case PlaybackState::Error:
    return "Error";
  }
  return "Stopped";
}

TimeRange fragmentTimeRange(const ByteRange &fragment,
                            const std::uint64_t containerLength,
                            const float durationSeconds) {
  if (0 == containerLength || durationSeconds <= 0.0F) {
    return {};
  }
  const auto total = static_cast<float>(containerLength);
  const auto start =
      static_cast<float>(std::min(fragment.start, containerLength));
  const auto end =
      static_cast<float>(std::min(fragment.end(), containerLength));
  return TimeRange{(start / total) * durationSeconds,
                   (end / total) * durationSeconds};
}

// -- MediaResource ------------------------------------------------------------

MediaResource::MediaResource(const Type type, std::string name,
                             std::string location, MediaStreamPtr stream)
    : type_(type), name_(std::move(name)), location_(std::move(location)),
      stream_(std::move(stream)) {}

std::shared_ptr<MediaResource> MediaResource::fromFile(std::string filePath) {
  std::filesystem::path p(filePath);
  std::string name = p.filename().string();
  auto stream      = std::make_shared<FileMediaStream>(filePath);
  return std::shared_ptr<MediaResource>(new MediaResource(
      Type::File, std::move(name), std::move(filePath), std::move(stream)));
}

std::shared_ptr<MediaResource> MediaResource::fromLocation(std::string mrl) {
  std::string name = mrl;
  return std::shared_ptr<MediaResource>(new MediaResource(
      Type::Location, std::move(name), std::move(mrl), nullptr));
}

std::shared_ptr<MediaResource> MediaResource::fromStream(MediaStreamPtr stream,
                                                         std::string label) {
  if (label.empty()) {
    label = "Stream";
  }
  return std::shared_ptr<MediaResource>(
      new MediaResource(Type::Stream, std::move(label), "", std::move(stream)));
}

bool MediaResource::isValid() const {
  switch (type_) {
  case Type::File:
    return !location_.empty() && (stream_ && stream_->size() > 0);
  case Type::Location:
    return !location_.empty();
  case Type::Stream:
    return stream_ && stream_->size() > 0;
  }
  return false;
}

std::shared_ptr<MediaResource>
MediaResource::subspan(const std::uint64_t offset,
                       const std::uint64_t length) const {
  if (stream_) {
    auto slicedStream = stream_->subspan(offset, length);
    return fromStream(slicedStream, name_ + " [slice]");
  }
  if (type_ == Type::File) {
    auto slicedStream =
        std::make_shared<FileMediaStream>(location_, offset, length);
    return fromStream(slicedStream, name_ + " [slice]");
  }
  return nullptr;
}

// -- MediaPlayer::Impl --------------------------------------------------------

struct MediaPlayer::Impl {
  libvlc_instance_t *vlcInstance{nullptr};
  libvlc_media_player_t *mediaPlayer{nullptr};
  libvlc_media_t *vlcMedia{nullptr};
  std::unique_ptr<VlcStreamState> streamState;

  MediaResourcePtr currentResource;
  std::optional<TimeRange> timeRange;
  std::optional<ByteRange> byteRange;
  bool looping{false};
  int volumeLevel{100};
  bool muted{false};

  // Fallback / simulated clock mode for headless or unit test environments
  bool dummyMode{false};
  float simulatedPosition{0.0F};
  float simulatedDuration{60.0F};
  PlaybackState simulatedState{PlaybackState::Stopped};

  // Video frame reception
  mutable std::mutex videoMutex;
  std::vector<std::uint32_t> videoBuffer;
  std::shared_ptr<VideoFrame> frame;
  bool frameAvailable{false};
  int frameWidth{0};
  int frameHeight{0};

  explicit Impl(const bool dummyAudio) : dummyMode(dummyAudio) {
    if (!dummyMode) {
      std::vector<const char *> args = {
          "--no-video-title-show",
          "--quiet",
      };
      vlcInstance = libvlc_new(static_cast<int>(args.size()), args.data());
      if (nullptr == vlcInstance) {
        dummyMode = true;
      }
    }
  }

  ~Impl() {
    releaseMedia();
    if (nullptr != vlcInstance) {
      libvlc_release(vlcInstance);
      vlcInstance = nullptr;
    }
  }

  void releaseMedia() {
    if (nullptr != mediaPlayer) {
      libvlc_media_player_stop(mediaPlayer);
      libvlc_media_player_release(mediaPlayer);
      mediaPlayer = nullptr;
    }
    if (nullptr != vlcMedia) {
      libvlc_media_release(vlcMedia);
      vlcMedia = nullptr;
    }
    streamState.reset();
  }

  static void *videoLock(void *opaque, void **planes) {
    auto *self = static_cast<Impl *>(opaque);
    self->videoMutex.lock();
    planes[0] = self->videoBuffer.data();
    return nullptr;
  }

  static void videoUnlock(void *opaque, [[maybe_unused]] void *picture,
                          [[maybe_unused]] void *const *planes) {
    auto *self = static_cast<Impl *>(opaque);
    self->videoMutex.unlock();
  }

  static void videoDisplay(void *opaque, [[maybe_unused]] void *picture) {
    auto *self = static_cast<Impl *>(opaque);
    std::lock_guard<std::mutex> lock(self->videoMutex);
    if (self->frameWidth > 0 && self->frameHeight > 0 &&
        !self->videoBuffer.empty()) {
      auto newFrame        = std::make_shared<VideoFrame>();
      newFrame->width      = self->frameWidth;
      newFrame->height     = self->frameHeight;
      newFrame->rgba       = self->videoBuffer;
      self->frame          = newFrame;
      self->frameAvailable = true;
    }
  }

  static unsigned videoFormatSetup(void **opaque, char *chroma, unsigned *width,
                                   unsigned *height, unsigned *pitches,
                                   unsigned *lines) {
    auto *self = static_cast<Impl *>(*opaque);
    std::memcpy(chroma, "RV32", 4); // 32-bit RGBA
    self->frameWidth  = static_cast<int>(*width);
    self->frameHeight = static_cast<int>(*height);
    *pitches          = (*width) * 4;
    *lines            = *height;

    std::lock_guard<std::mutex> lock(self->videoMutex);
    self->videoBuffer.resize(static_cast<std::size_t>(*width) * (*height));
    return 1;
  }

  static void videoFormatCleanup([[maybe_unused]] void *opaque) {}

  bool load(MediaResourcePtr res) {
    releaseMedia();
    currentResource = res;
    if (!res || !res->isValid()) {
      simulatedState = PlaybackState::Error;
      return false;
    }

    if (dummyMode || nullptr == vlcInstance) {
      simulatedPosition = 0.0F;
      simulatedState    = PlaybackState::Stopped;
      if (res->stream()) {
        simulatedDuration = static_cast<float>(res->stream()->size()) / 1000.0F;
        if (simulatedDuration < 1.0F) {
          simulatedDuration = 30.0F;
        }
      } else {
        simulatedDuration = 60.0F;
      }
      return true;
    }

    if (res->type() == MediaResource::Type::Stream) {
      streamState = std::make_unique<VlcStreamState>(res->stream(), 0);
      vlcMedia    = libvlc_media_new_callbacks(
          vlcInstance, VlcStreamState::open, VlcStreamState::read,
          VlcStreamState::seek, VlcStreamState::close, streamState.get());
    } else if (res->type() == MediaResource::Type::File) {
      vlcMedia = libvlc_media_new_path(vlcInstance, res->location().c_str());
    } else {
      vlcMedia =
          libvlc_media_new_location(vlcInstance, res->location().c_str());
    }

    if (nullptr == vlcMedia) {
      simulatedState = PlaybackState::Error;
      return false;
    }

    mediaPlayer = libvlc_media_player_new_from_media(vlcMedia);
    if (nullptr == mediaPlayer) {
      simulatedState = PlaybackState::Error;
      return false;
    }

    libvlc_video_set_callbacks(mediaPlayer, videoLock, videoUnlock,
                               videoDisplay, this);
    libvlc_video_set_format_callbacks(mediaPlayer, videoFormatSetup,
                                      videoFormatCleanup);

    libvlc_audio_set_volume(mediaPlayer, volumeLevel);
    libvlc_audio_set_mute(mediaPlayer, muted ? 1 : 0);
    return true;
  }

  bool play() {
    if (dummyMode || nullptr == mediaPlayer) {
      if (timeRange && (simulatedPosition < timeRange->startSeconds ||
                        simulatedPosition >= timeRange->endSeconds)) {
        simulatedPosition = timeRange->startSeconds;
      }
      simulatedState = PlaybackState::Playing;
      return true;
    }

    if (timeRange) {
      const auto pos = positionSeconds();
      if (pos < timeRange->startSeconds || pos >= timeRange->endSeconds) {
        seek(timeRange->startSeconds);
      }
    }
    return 0 == libvlc_media_player_play(mediaPlayer);
  }

  void pause() {
    if (dummyMode || nullptr == mediaPlayer) {
      if (simulatedState == PlaybackState::Playing) {
        simulatedState = PlaybackState::Paused;
      }
      return;
    }
    libvlc_media_player_pause(mediaPlayer);
  }

  void stop() {
    if (dummyMode || nullptr == mediaPlayer) {
      simulatedState    = PlaybackState::Stopped;
      simulatedPosition = timeRange ? timeRange->startSeconds : 0.0F;
      return;
    }
    libvlc_media_player_stop(mediaPlayer);
    if (timeRange) {
      seek(timeRange->startSeconds);
    }
  }

  void seek(const float seconds) {
    float target = seconds;
    if (timeRange) {
      target = timeRange->clamp(target);
    }

    if (dummyMode || nullptr == mediaPlayer) {
      simulatedPosition = std::max(0.0F, target);
      return;
    }
    const auto ms =
        static_cast<libvlc_time_t>(std::max(0.0F, target * 1000.0F));
    libvlc_media_player_set_time(mediaPlayer, ms);
  }

  void seekFraction(const float fraction) {
    const auto dur = durationSeconds();
    if (dur > 0.0F) {
      if (timeRange) {
        const auto target =
            timeRange->startSeconds + (fraction * timeRange->duration());
        seek(target);
      } else {
        seek(fraction * dur);
      }
    }
  }

  void setVolume(const int percent) {
    volumeLevel = std::clamp(percent, 0, 100);
    if (mediaPlayer != nullptr) {
      libvlc_audio_set_volume(mediaPlayer, volumeLevel);
    }
  }

  [[nodiscard]] int volume() const {
    if (mediaPlayer != nullptr) {
      return libvlc_audio_get_volume(mediaPlayer);
    }
    return volumeLevel;
  }

  void setMuted(const bool mute) {
    muted = mute;
    if (mediaPlayer != nullptr) {
      libvlc_audio_set_mute(mediaPlayer, muted ? 1 : 0);
    }
  }

  [[nodiscard]] bool isMuted() const {
    if (mediaPlayer != nullptr) {
      return libvlc_audio_get_mute(mediaPlayer) != 0;
    }
    return muted;
  }

  [[nodiscard]] PlaybackState state() const {
    if (dummyMode || nullptr == mediaPlayer) {
      return simulatedState;
    }
    const auto vlcState = libvlc_media_player_get_state(mediaPlayer);
    switch (vlcState) {
    case libvlc_NothingSpecial:
    case libvlc_Stopped:
      return PlaybackState::Stopped;
    case libvlc_Opening:
      return PlaybackState::Opening;
    case libvlc_Buffering:
      return PlaybackState::Buffering;
    case libvlc_Playing:
      return PlaybackState::Playing;
    case libvlc_Paused:
      return PlaybackState::Paused;
    case libvlc_Ended:
      return PlaybackState::Ended;
    case libvlc_Error:
      return PlaybackState::Error;
    }
    return PlaybackState::Stopped;
  }

  [[nodiscard]] float positionSeconds() const {
    if (dummyMode || nullptr == mediaPlayer) {
      return simulatedPosition;
    }
    const auto ms = libvlc_media_player_get_time(mediaPlayer);
    return ms >= 0 ? static_cast<float>(ms) / 1000.0F : 0.0F;
  }

  [[nodiscard]] float durationSeconds() const {
    if (dummyMode || nullptr == mediaPlayer) {
      return simulatedDuration;
    }
    const auto ms = libvlc_media_player_get_length(mediaPlayer);
    return ms > 0 ? static_cast<float>(ms) / 1000.0F : 0.0F;
  }

  [[nodiscard]] float progressFraction() const {
    const auto dur = durationSeconds();
    if (dur <= 0.0F) {
      return 0.0F;
    }
    if (timeRange) {
      const auto pos = positionSeconds();
      return std::clamp((pos - timeRange->startSeconds) / timeRange->duration(),
                        0.0F, 1.0F);
    }
    return std::clamp(positionSeconds() / dur, 0.0F, 1.0F);
  }

  void update(const float dtSeconds) {
    if (dummyMode && simulatedState == PlaybackState::Playing) {
      simulatedPosition += dtSeconds;
      const auto maxLimit =
          timeRange ? timeRange->endSeconds : simulatedDuration;
      if (simulatedPosition >= maxLimit) {
        if (looping) {
          simulatedPosition = timeRange ? timeRange->startSeconds : 0.0F;
        } else {
          simulatedPosition = maxLimit;
          simulatedState    = PlaybackState::Ended;
        }
      }
      return;
    }

    if (nullptr != mediaPlayer && timeRange &&
        state() == PlaybackState::Playing) {
      const auto pos = positionSeconds();
      if (pos >= timeRange->endSeconds) {
        if (looping) {
          seek(timeRange->startSeconds);
        } else {
          pause();
          seek(timeRange->startSeconds);
        }
      }
    }
  }
};

// -- MediaPlayer --------------------------------------------------------------

MediaPlayer::MediaPlayer(const bool dummyAudio)
    : impl(std::make_unique<Impl>(dummyAudio)) {}

MediaPlayer::~MediaPlayer() = default;

bool MediaPlayer::load(MediaResourcePtr resource) {
  return impl->load(std::move(resource));
}

void MediaPlayer::unload() { impl->releaseMedia(); }

bool MediaPlayer::play() { return impl->play(); }
void MediaPlayer::pause() { impl->pause(); }
void MediaPlayer::stop() { impl->stop(); }

void MediaPlayer::toggle() {
  if (state() == PlaybackState::Playing) {
    pause();
  } else {
    play();
  }
}

void MediaPlayer::seek(const float seconds) { impl->seek(seconds); }
void MediaPlayer::seekFraction(const float fraction) {
  impl->seekFraction(fraction);
}

void MediaPlayer::setVolume(const int percent) { impl->setVolume(percent); }
int MediaPlayer::volume() const { return impl->volume(); }

void MediaPlayer::setMuted(const bool mute) { impl->setMuted(mute); }
bool MediaPlayer::isMuted() const { return impl->isMuted(); }

PlaybackState MediaPlayer::state() const { return impl->state(); }
float MediaPlayer::positionSeconds() const { return impl->positionSeconds(); }
float MediaPlayer::durationSeconds() const { return impl->durationSeconds(); }
float MediaPlayer::progressFraction() const { return impl->progressFraction(); }

void MediaPlayer::setTimeRange(const float startSeconds,
                               const float endSeconds) {
  impl->timeRange = TimeRange{std::max(0.0F, startSeconds),
                              std::max(startSeconds, endSeconds)};
  if (impl->positionSeconds() < impl->timeRange->startSeconds ||
      impl->positionSeconds() > impl->timeRange->endSeconds) {
    impl->seek(impl->timeRange->startSeconds);
  }
}

void MediaPlayer::setByteRange(const std::uint64_t startByte,
                               const std::uint64_t length) {
  impl->byteRange = ByteRange{startByte, length};
}

void MediaPlayer::clearRange() {
  impl->timeRange.reset();
  impl->byteRange.reset();
}

bool MediaPlayer::hasRangeConstraint() const {
  return impl->timeRange.has_value() || impl->byteRange.has_value();
}

std::optional<TimeRange> MediaPlayer::activeTimeRange() const {
  return impl->timeRange;
}

std::optional<ByteRange> MediaPlayer::activeByteRange() const {
  return impl->byteRange;
}

void MediaPlayer::setLooping(const bool loop) { impl->looping = loop; }
bool MediaPlayer::isLooping() const { return impl->looping; }

bool MediaPlayer::hasVideo() const {
  if (impl->dummyMode) {
    return false;
  }
  if (nullptr == impl->mediaPlayer) {
    return false;
  }
  return libvlc_media_player_has_vout(impl->mediaPlayer) > 0 ||
         impl->frameWidth > 0;
}

int MediaPlayer::videoWidth() const { return impl->frameWidth; }
int MediaPlayer::videoHeight() const { return impl->frameHeight; }

float MediaPlayer::aspectRatio() const {
  if (impl->frameWidth > 0 && impl->frameHeight > 0) {
    return static_cast<float>(impl->frameWidth) /
           static_cast<float>(impl->frameHeight);
  }
  return MediaPlayer::defaultAspect;
}

std::shared_ptr<VideoFrame> MediaPlayer::latestFrame() const {
  std::lock_guard<std::mutex> lock(impl->videoMutex);
  impl->frameAvailable = false;
  return impl->frame;
}

bool MediaPlayer::isNewFrameAvailable() const {
  std::lock_guard<std::mutex> lock(impl->videoMutex);
  return impl->frameAvailable;
}

void MediaPlayer::update(const float dtSeconds) { impl->update(dtSeconds); }

MediaResourcePtr MediaPlayer::currentResource() const {
  return impl->currentResource;
}

} // namespace gleditor

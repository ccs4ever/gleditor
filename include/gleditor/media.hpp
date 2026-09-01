/**
 * @file media.hpp
 * @brief High-level media resource and player interface powered by LibVLC.
 *
 * Provides broad format audio and video decoding, stream slicing,
 * range-bounded playback, and RGBA video frame callbacks with a clean,
 * domain-neutral API.
 */
#ifndef GLEDITOR_MEDIA_H
#define GLEDITOR_MEDIA_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gleditor/media_stream.hpp>

namespace gleditor {

/**
 * @enum PlaybackState
 * @brief Current playback status of a media player.
 */
enum class PlaybackState : std::uint8_t {
  Stopped,
  Playing,
  Paused,
  Opening,
  Buffering,
  Ended,
  Error,
};

[[nodiscard]] const char *playbackStateName(PlaybackState state);

/**
 * @struct VideoFrame
 * @brief A decoded RGBA video frame buffer.
 */
struct VideoFrame {
  std::vector<std::uint32_t> rgba;
  int width{0};
  int height{0};
  std::uint64_t timestampMs{0};
};

/**
 * @class MediaResource
 * @brief Represents an audio or video media source (file, URL/MRL, or
 *        byte-accurate stream).
 */
class MediaResource {
public:
  enum class Type : std::uint8_t {
    File,
    Location,
    Stream,
  };

  static std::shared_ptr<MediaResource> fromFile(std::string filePath);
  static std::shared_ptr<MediaResource> fromLocation(std::string mrl);
  static std::shared_ptr<MediaResource> fromStream(MediaStreamPtr stream,
                                                   std::string label = "");

  [[nodiscard]] Type type() const { return type_; }
  [[nodiscard]] const std::string &name() const { return name_; }
  [[nodiscard]] const std::string &location() const { return location_; }
  [[nodiscard]] MediaStreamPtr stream() const { return stream_; }
  [[nodiscard]] bool isValid() const;

  /**
   * @brief Create a sliced sub-resource covering [offset, offset + length).
   */
  [[nodiscard]] std::shared_ptr<MediaResource>
  subspan(std::uint64_t offset, std::uint64_t length) const;

private:
  MediaResource(Type type, std::string name, std::string location,
                MediaStreamPtr stream);

  Type type_{Type::File};
  std::string name_;
  std::string location_;
  MediaStreamPtr stream_;
};

using MediaResourcePtr = std::shared_ptr<MediaResource>;

/**
 * @class MediaPlayer
 * @brief Audio and video media player driving LibVLC with support for
 *        byte-accurate range constraints and video frame extraction.
 */
class MediaPlayer {
public:
  explicit MediaPlayer(bool dummyAudio = false);
  ~MediaPlayer();

  MediaPlayer(const MediaPlayer &)            = delete;
  MediaPlayer &operator=(const MediaPlayer &) = delete;
  MediaPlayer(MediaPlayer &&)                 = delete;
  MediaPlayer &operator=(MediaPlayer &&)      = delete;

  bool load(MediaResourcePtr resource);
  void unload();

  bool play();
  void pause();
  void stop();
  void toggle();

  void seek(float seconds);
  void seekFraction(float fraction);

  void setVolume(int percent); // 0 to 100
  [[nodiscard]] int volume() const;

  void setMuted(bool mute);
  [[nodiscard]] bool isMuted() const;

  [[nodiscard]] PlaybackState state() const;
  [[nodiscard]] float positionSeconds() const;
  [[nodiscard]] float durationSeconds() const;
  [[nodiscard]] float progressFraction() const;

  void setTimeRange(float startSeconds, float endSeconds);
  void setByteRange(std::uint64_t startByte, std::uint64_t length);
  void clearRange();
  [[nodiscard]] bool hasRangeConstraint() const;
  [[nodiscard]] std::optional<TimeRange> activeTimeRange() const;
  [[nodiscard]] std::optional<ByteRange> activeByteRange() const;

  void setLooping(bool loop);
  [[nodiscard]] bool isLooping() const;

  // Video properties & frame access
  [[nodiscard]] bool hasVideo() const;
  [[nodiscard]] int videoWidth() const;
  [[nodiscard]] int videoHeight() const;
  [[nodiscard]] float aspectRatio() const;
  [[nodiscard]] std::shared_ptr<VideoFrame> latestFrame() const;
  [[nodiscard]] bool isNewFrameAvailable() const;

  /**
   * @brief Advance time tracking and enforce range bounds.
   */
  void update(float dtSeconds);

  [[nodiscard]] MediaResourcePtr currentResource() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

using MediaPlayerPtr = std::shared_ptr<MediaPlayer>;

} // namespace gleditor

#endif // GLEDITOR_MEDIA_H

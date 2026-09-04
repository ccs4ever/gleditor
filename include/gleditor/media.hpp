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
 * @brief Where in playback time @p fragment of a @p containerLength-byte file
 *        falls, given that file's own @p durationSeconds.
 *
 * A transcluded audio or video fragment is addressed in bytes -- an offset
 * and a length into the file it was cut from -- but MediaPlayer only honours
 * a *time* range (setByteRange() is not wired to anything; see its own
 * comment). This is the byte-to-time translation that makes that fragment
 * playable: linear in the file's own byte size, which assumes roughly
 * constant bitrate. Exact for PCM WAV, an honest approximation for most
 * encoded audio and video, and wrong for a file whose bitrate genuinely
 * varies a lot -- which is a real limitation worth knowing about rather than
 * a rare enough case to ignore, but not one this pass solves; the transcluded
 * fragment is at least in roughly the right place rather than not
 * constrained at all.
 *
 * Returns an empty range (duration() <= 0) when @p containerLength or
 * @p durationSeconds is not yet known, which a caller polling for it -- a
 * duration LibVLC has not finished parsing yet -- can use as "not ready".
 */
[[nodiscard]] TimeRange fragmentTimeRange(const ByteRange &fragment,
                                          std::uint64_t containerLength,
                                          float durationSeconds);

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

  /// Assumed until a real frame reports otherwise -- aspectRatio() falls
  /// back to it, and anything sizing a video's space before playback starts
  /// (a document's reserved placeholder height, an unplayed widget's
  /// initial size) has nothing else to go on either.
  static constexpr float defaultAspect = 16.0F / 9.0F;

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

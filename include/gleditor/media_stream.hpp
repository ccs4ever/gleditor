/**
 * @file media_stream.hpp
 * @brief Byte-accurate stream slicing and playback ranges for media.
 *
 * Media playback in a hypertext or visual slice system needs byte-level
 * and time-level sub-ranges: quoting an audio or video snippet without copying
 * the underlying file requires reading exact slices from memory, files, or
 * custom stream caches.
 *
 * This header provides domain-neutral range structures and stream interfaces
 * that can feed raw bytes directly into decoders and players with zero
 * disk copies.
 */
#ifndef GLEDITOR_MEDIA_STREAM_H
#define GLEDITOR_MEDIA_STREAM_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gleditor {

/**
 * @struct ByteRange
 * @brief A half-open interval [start, start + length) in a byte stream.
 */
struct ByteRange {
  std::uint64_t start{0};
  std::uint64_t length{0};

  [[nodiscard]] constexpr std::uint64_t end() const { return start + length; }
  [[nodiscard]] constexpr bool empty() const { return 0 == length; }

  [[nodiscard]] constexpr bool contains(const std::uint64_t offset) const {
    return offset >= start && offset < end();
  }

  [[nodiscard]] constexpr bool contains(const ByteRange &other) const {
    return other.start >= start && other.end() <= end();
  }

  [[nodiscard]] constexpr ByteRange intersect(const ByteRange &other) const {
    const auto from = std::max(start, other.start);
    const auto to   = std::min(end(), other.end());
    return to > from ? ByteRange{from, to - from} : ByteRange{from, 0};
  }

  [[nodiscard]] constexpr ByteRange slice(const std::uint64_t offset,
                                          const std::uint64_t count) const {
    const auto from = std::min(offset, length);
    return {start + from, std::min(count, length - from)};
  }

  constexpr bool operator==(const ByteRange &) const = default;
};

/**
 * @struct TimeRange
 * @brief A temporal interval [startSeconds, endSeconds] for media playback.
 */
struct TimeRange {
  float startSeconds{0.0F};
  float endSeconds{0.0F};

  [[nodiscard]] constexpr float duration() const {
    return endSeconds > startSeconds ? endSeconds - startSeconds : 0.0F;
  }

  [[nodiscard]] constexpr bool empty() const { return duration() <= 0.0F; }

  [[nodiscard]] constexpr bool contains(const float seconds) const {
    return seconds >= startSeconds && seconds <= endSeconds;
  }

  [[nodiscard]] constexpr float clamp(const float seconds) const {
    return std::clamp(seconds, startSeconds, endSeconds);
  }

  constexpr bool operator==(const TimeRange &) const = default;
};

/**
 * @class MediaStream
 * @brief Abstract interface for random-access byte-level stream reading and
 *        subspan slicing.
 */
class MediaStream : public std::enable_shared_from_this<MediaStream> {
public:
  virtual ~MediaStream() = default;

  /**
   * @brief Read up to dst.size() bytes starting at stream offset.
   * @param offset Byte offset within this stream.
   * @param dst Buffer to receive the bytes.
   * @return Actual number of bytes read (0 on EOF or invalid offset).
   */
  [[nodiscard]] virtual std::size_t read(std::uint64_t offset,
                                         std::span<std::byte> dst) = 0;

  /// Total size in bytes of this stream.
  [[nodiscard]] virtual std::uint64_t size() const = 0;

  /**
   * @brief Create a sliced view of this stream covering [offset, offset +
   * length).
   */
  [[nodiscard]] virtual std::shared_ptr<MediaStream>
  subspan(std::uint64_t offset, std::uint64_t length) = 0;

  /// Helper to read a byte chunk into a vector.
  [[nodiscard]] std::vector<std::byte> readBytes(std::uint64_t offset,
                                                 std::size_t count);

  /// Helper to read a byte chunk as a string.
  [[nodiscard]] std::string readString(std::uint64_t offset, std::size_t count);
};

using MediaStreamPtr = std::shared_ptr<MediaStream>;

/**
 * @class MemoryMediaStream
 * @brief An in-memory media stream backed by a shared byte buffer.
 *
 * Slicing creates a zero-copy sub-window referencing the same shared buffer.
 */
class MemoryMediaStream : public MediaStream {
public:
  explicit MemoryMediaStream(std::vector<std::byte> bytes);
  explicit MemoryMediaStream(std::string_view text);
  MemoryMediaStream(std::shared_ptr<const std::vector<std::byte>> buffer,
                    std::uint64_t offset, std::uint64_t length);

  [[nodiscard]] std::size_t read(std::uint64_t offset,
                                 std::span<std::byte> dst) override;
  [[nodiscard]] std::uint64_t size() const override { return range.length; }
  [[nodiscard]] std::shared_ptr<MediaStream>
  subspan(std::uint64_t offset, std::uint64_t length) override;

  [[nodiscard]] const std::byte *data() const;

private:
  std::shared_ptr<const std::vector<std::byte>> storage;
  ByteRange range;
};

/**
 * @class FileMediaStream
 * @brief A file-backed media stream supporting random-access reads and sub-span
 *        slicing.
 */
class FileMediaStream : public MediaStream {
public:
  explicit FileMediaStream(std::string filePath);
  FileMediaStream(std::string filePath, std::uint64_t fileOffset,
                  std::uint64_t length);

  [[nodiscard]] std::size_t read(std::uint64_t offset,
                                 std::span<std::byte> dst) override;
  [[nodiscard]] std::uint64_t size() const override { return range.length; }
  [[nodiscard]] std::shared_ptr<MediaStream>
  subspan(std::uint64_t offset, std::uint64_t length) override;

  [[nodiscard]] const std::string &path() const { return path_; }

private:
  std::string path_;
  ByteRange range;
};

/**
 * @class CallbackMediaStream
 * @brief Media stream driven by a functional callback.
 *
 * Allows applications to supply dynamic or cached bytes (e.g. from piece
 * stores or spool rings) without intermediate disk files.
 */
class CallbackMediaStream : public MediaStream {
public:
  using ReadCallback = std::function<std::size_t(std::uint64_t offset,
                                                 std::span<std::byte> dst)>;

  CallbackMediaStream(ReadCallback callback, std::uint64_t streamSize);
  CallbackMediaStream(ReadCallback callback, std::uint64_t streamSize,
                      std::uint64_t baseOffset, std::uint64_t length);

  [[nodiscard]] std::size_t read(std::uint64_t offset,
                                 std::span<std::byte> dst) override;
  [[nodiscard]] std::uint64_t size() const override { return range.length; }
  [[nodiscard]] std::shared_ptr<MediaStream>
  subspan(std::uint64_t offset, std::uint64_t length) override;

private:
  ReadCallback readCb;
  ByteRange range;
};

/**
 * @struct VlcStreamState
 * @brief Cursor state used when binding a MediaStream to LibVLC's custom I/O
 * callbacks.
 */
struct VlcStreamState {
  MediaStreamPtr stream;
  std::uint64_t cursor{0};

  static int open(void *opaque, void **dataptr, std::uint64_t *sizep);
  static std::ptrdiff_t read(void *opaque, unsigned char *buf, std::size_t len);
  static int seek(void *opaque, std::uint64_t offset);
  static void close(void *opaque);
};

} // namespace gleditor

#endif // GLEDITOR_MEDIA_STREAM_H

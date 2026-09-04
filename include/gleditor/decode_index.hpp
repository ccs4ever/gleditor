/**
 * @file decode_index.hpp
 * @brief Whether, and how, a compressed media file can be partially decoded
 *        without decoding it from the start.
 *
 * A byte range inside a compressed stream names no region of anything: the
 * bytes at that offset cannot be decoded without whatever state a decoder
 * built up getting there. `tools/decode-index-spike.cpp` prototyped what a
 * "decode index" would need to mean, per format, before this could be
 * load-bearing anywhere (see design/decode-index-spike.md for the full
 * reasoning and the dead ends that did not work). This header promotes the
 * mechanisms that spike proved -- PNG, JPEG, video, MP3, not WebP, which has
 * no viable mechanism at all -- into a real, tested component. Zstd is a
 * fifth, different case again: unlike the other four, whether a given zstd
 * file is seekable at all depends on whether it was *encoded* with zstd's
 * own seekable format (`ZSTD_seekable_*`, vendored from
 * `thirdparty/zstd/contrib/seekable_format` -- not part of the core zstd
 * library or any distro package). Most zstd files in the wild are not, so
 * `reencodeZstdSeekable()` exists to convert one before it becomes part of
 * something sealed/published, rather than only reporting on files that
 * already happen to qualify. FLAC is the same shape as zstd -- a native
 * SEEKTABLE metadata block most encoders do not bother writing, or write
 * sparsely -- so `reencodeFlacSeekable()` exists for the same reason. TIFF
 * is different again: it is already a randomly-accessible container by
 * design (strips/tiles each have their own recorded byte offset, no
 * container-level index to add at all), but an encoder that writes one
 * giant strip per image -- confirmed to happen in practice, not assumed --
 * leaves nothing useful to seek within. `reencodeTiffSeekable()` forces a
 * small `RowsPerStrip` to fix that.
 *
 * Deliberately not attempted here: wiring this into
 * Store/ScrollSegment/PrimediaSpan. Building the index and resolving a
 * transcluded span through it are two different pieces of work, and this is
 * only the first.
 */
#ifndef GLEDITOR_DECODE_INDEX_HPP
#define GLEDITOR_DECODE_INDEX_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <gleditor/mimetype.hpp>

namespace gleditor {

/// Which format buildDecodeIndex() recognised @p bytes as, or Unsupported
/// for a MIME type this component has no answer for at all (including
/// every image/video/audio type other than the seven below -- WebP among
/// them, since Phase 15's own investigation found no viable mechanism for
/// it to report).
enum class DecodeIndexFormat : std::uint8_t {
  Png,
  Jpeg,
  Video,
  Mp3,
  Zstd,
  Flac,
  Tiff,
  Unsupported,
};

/**
 * @brief One durable, serializable seek point.
 *
 * @p uncompressedPosition is in the format's own canonical coordinate --
 * a row index for PNG/JPEG/TIFF, a decoded frame or sample-frame index for
 * video/MP3/FLAC -- and @p compressedByteOffset is where in the original
 * file's bytes library-provided seeking (av_seek_frame(),
 * jpeg_skip_scanlines(), FLAC__stream_decoder_seek_absolute(),
 * TIFFReadEncodedStrip()) can resume from to reach it. Both fields are
 * plain data: safe to write to disk and read back in a different process,
 * unlike PNG's own restart mechanism -- see PngCheckpoints for why that one
 * is different.
 */
struct SeekPoint {
  std::uint64_t uncompressedPosition{};
  std::uint64_t compressedByteOffset{};
};

/**
 * @brief What buildDecodeIndex() found out about one file.
 *
 * @p seekPoints being empty does not mean the file cannot be seeked within
 * -- @p seekable says whether some partial-decode mechanism exists at all,
 * and @p durableIndex says whether @p seekPoints is that mechanism's
 * complete, storable answer. A file can be seekable without a durable
 * index (MP3: FFmpeg's own av_seek_frame() plus a decoder warm-up margin
 * works, but FFmpeg's MP3 demuxer builds no index to extract -- confirmed
 * empirically, not assumed, while building this) or seekable via a
 * mechanism this type cannot carry at all (PNG: see PngCheckpoints). Zstd
 * and FLAC are the other direction: `seekable`/`durableIndex` are both
 * false for an ordinary zstd file (no seekable-format frames) or a FLAC
 * file with no SEEKTABLE metadata block, not because no mechanism exists,
 * but because none was used at encode time -- see `reencodeZstdSeekable()`
 * and `reencodeFlacSeekable()` for the fix. TIFF is seekable without any
 * encode-time cooperation at all -- strip/tile offsets are just part of the
 * format -- but a single-strip file (confirmed to happen in practice) has
 * only one seek point, at the very start, which is not usefully seekable;
 * see `reencodeTiffSeekable()`. Tiled TIFFs report `seekable` true (
 * TIFFReadEncodedTile() still works) but `durableIndex` false: a tile's
 * position is two-dimensional, and this component's flat, single-scalar
 * `uncompressedPosition` has no way to carry that without conflating
 * tiles that start at the same row -- scoped out rather than modelled
 * awkwardly.
 */
struct DecodeIndex {
  DecodeIndexFormat format{DecodeIndexFormat::Unsupported};
  /// Total extent in the canonical coordinate: rows for PNG/JPEG, decoded
  /// frames for video, decoded MP3 frames for Mp3. Zero when unknown or
  /// unsupported.
  std::uint64_t uncompressedExtent{};
  /// Durable seek points, if any -- see the type's own comment for what an
  /// empty vector does and does not imply.
  std::vector<SeekPoint> seekPoints;
  /// Whether *some* mechanism exists to resume decoding this file partway
  /// through, by any means this component knows -- not necessarily via
  /// @p seekPoints. False for an unrecognised format, a format this build
  /// was compiled without the optional dependency for, or a file this
  /// component's mechanism does not apply to (a JPEG with no restart
  /// markers still decodes, just never partially).
  bool seekable{false};
  /// Whether @p seekPoints alone -- durable, storable bytes -- are
  /// sufficient to resume decoding later, with no other in-process state
  /// required. True for JPEG (restart markers) and video (container
  /// index). False for PNG (see PngCheckpoints, the in-process-only
  /// companion) and MP3 (seekable via FFmpeg's own estimation, but with no
  /// index to extract and store).
  bool durableIndex{false};
};

/**
 * @brief Builds a decode index for @p bytes, given its MIME type.
 *
 * Never throws for an unrecognised or unsupported format or MIME type --
 * returns a default-constructed DecodeIndex (format Unsupported, seekable
 * false) instead, the same way an empty/malformed @p bytes does. A build
 * missing the optional dependency a format needs (zlib for Png, libjpeg
 * for Jpeg, libavformat/libavcodec/libavutil for Video and Mp3, libzstd for
 * Zstd, libFLAC++ for Flac, libtiff for Tiff) reports the same way -- check
 * `seekable` before relying on anything else in the result.
 */
[[nodiscard]] DecodeIndex buildDecodeIndex(std::span<const std::uint8_t> bytes,
                                           const MimeType &mime);

/**
 * @brief Decompresses @p zstdBytes and recompresses it into zstd's own
 *        seekable format, so a later buildDecodeIndex() call on the result
 *        reports seekable/durable with a real seek table.
 *
 * A real transcode, not a header rewrite: the seekable format's frame
 * boundaries are decided at compress time (`ZSTD_seekable_compressStream()`
 * ends a frame every @p maxFrameSize uncompressed bytes), so producing them
 * for content already compressed some other way means decoding it in full
 * once and compressing it again. Costs what a full recompression costs --
 * meant to be called once, before content becomes part of something
 * sealed/published, not on every read.
 *
 * @p maxFrameSize trades compression ratio for seek granularity the same
 * way PngCheckpoints::build()'s rowInterval does: smaller frames mean a
 * seek decodes less throwaway data but compress worse, since each frame
 * starts its own independent compression context with no cross-frame
 * back-references. The zstd seekable format's own documentation calls
 * frames under 1KB "really tiny" and warns they hurt ratio considerably;
 * this default is chosen to stay well clear of that, not measured against
 * this codebase's own content.
 *
 * Returns nullopt if @p zstdBytes does not decompress as zstd at all, or if
 * this build lacks libzstd (GLEDITOR_HAVE_DECODE_INDEX_ZSTD) -- the same
 * "check before relying on it" contract as the rest of this header. Bytes
 * already in the seekable format round-trip through this unchanged in
 * effect (replaced with a fresh seek table at the same @p maxFrameSize),
 * not detected and skipped, since re-deriving the same answer costs nothing
 * additional to check for it separately.
 */
[[nodiscard]] std::optional<std::vector<std::uint8_t>>
reencodeZstdSeekable(std::span<const std::uint8_t> zstdBytes,
                     std::uint32_t maxFrameSize = 1U << 20);

/**
 * @brief Decompresses @p flacBytes and recompresses it with a fresh,
 *        generously-spaced SEEKTABLE metadata block, so a later
 *        buildDecodeIndex() call on the result reports seekable/durable.
 *
 * FLAC already has a native seek table concept (unlike gzip, which needed
 * zran, or WebP, which has no mechanism at all) -- the gap is that most
 * encoders either omit it or space it sparsely, since it costs bytes for a
 * benefit only a seeking reader gets. Placeholder points
 * (`FLAC::Metadata::SeekTable::template_append_spaced_points_by_samples()`)
 * get resolved to real byte offsets by the encoder itself as it writes
 * frames -- confirmed empirically, not assumed: `seek_absolute()` on a
 * freshly round-tripped file lands on samples that are byte-identical to
 * the same position reached by linear decode from the start.
 *
 * A real transcode, like `reencodeZstdSeekable()`: full decode to PCM, full
 * re-encode with the new seek table. Only STREAMINFO and the seek table are
 * carried across -- VORBIS_COMMENT tags, embedded pictures, and other
 * metadata blocks the source file may have are not preserved in this pass,
 * a named gap rather than a silent one. Meant to be called once, before an
 * asset becomes part of something sealed/published, not on every read.
 *
 * @p secondsPerSeekPoint controls how generously the new table is spaced --
 * smaller values mean more points (a seek decodes less throwaway audio
 * before reaching the target, at the cost of a slightly larger seek table
 * and a few more bytes of lost cross-frame compression at each extra split
 * point). "Generous" per the request that led to this: seek tables in the
 * wild are commonly spaced 10s or more apart; the default here is denser.
 *
 * Returns nullopt if @p flacBytes does not decode as FLAC at all, or if
 * this build lacks libFLAC++ (GLEDITOR_HAVE_DECODE_INDEX_FLAC).
 */
[[nodiscard]] std::optional<std::vector<std::uint8_t>>
reencodeFlacSeekable(std::span<const std::uint8_t> flacBytes,
                     float secondsPerSeekPoint = 0.5F);

/**
 * @brief Decodes @p tiffBytes and re-encodes it with a small, forced
 *        RowsPerStrip, so a later buildDecodeIndex() call on the result
 *        reports a durable index with many strips instead of one.
 *
 * Unlike zstd/FLAC, TIFF needs no new container concept -- strips already
 * are the seek mechanism, recorded via the format's own StripOffsets/
 * StripByteCounts tags. The gap this closes is encoder behaviour, not a
 * missing format feature: confirmed empirically that a single explicit
 * `RowsPerStrip` covering the whole image (an encoder some tools genuinely
 * produce) yields exactly one strip, and that an isolated
 * `TIFFReadEncodedStrip()` call against a *fresh* file handle -- no prior
 * strip read, no state carried -- returns bytes identical to that same
 * strip in a full top-to-bottom decode, proving strips need no cross-strip
 * state to resume from (true for both LZW and Deflate/ZIP, confirmed
 * separately).
 *
 * A real transcode, like the zstd/FLAC functions above: full decode via
 * `TIFFReadRGBAImageOriented()` (which normalises whatever photometric
 * interpretation, bit depth, and planar configuration the source used into
 * 8-bit RGBA, the one shape this component re-encodes), then a full
 * re-encode as Deflate-compressed, contiguous-planar RGBA with the new,
 * small `RowsPerStrip`. Other tags the source file carried (colour
 * profiles, EXIF/GPS metadata, alternate photometric interpretations) are
 * not preserved, a named gap rather than a silent one, the same choice
 * `reencodeFlacSeekable()` makes for VORBIS_COMMENT/pictures.
 *
 * @p rowsPerStrip trades compression ratio and per-strip overhead for seek
 * granularity, the same as `PngCheckpoints::build()`'s `rowInterval` --
 * smaller values mean more, smaller strips. Defaulted small ("generous,"
 * per the request that led to this) rather than left at libtiff's own
 * default heuristic (which targets a roughly constant *compressed* size
 * per strip, not a constant row count, and produces exactly the coarse,
 * barely-seekable output this function exists to fix for a large image).
 *
 * Returns nullopt if @p tiffBytes does not decode as TIFF at all, or if
 * this build lacks libtiff (GLEDITOR_HAVE_DECODE_INDEX_TIFF).
 */
[[nodiscard]] std::optional<std::vector<std::uint8_t>>
reencodeTiffSeekable(std::span<const std::uint8_t> tiffBytes,
                     std::uint32_t rowsPerStrip = 8);

/**
 * @class PngCheckpoints
 * @brief In-process-only restart checkpoints for a PNG, at row granularity.
 *
 * PNG's Up/Paeth scanline filters are differential across rows, so
 * resuming decode at row N needs the zlib decompressor's own internal
 * state at that point (via inflateCopy(), a live, heap-owned clone -- see
 * decodeCheckpoint()'s own comment for why it must be heap-allocated) plus
 * row N-1's already-unfiltered bytes. That state has no portable byte
 * representation zlib exposes publicly, unlike JPEG's restart markers or a
 * video container's own index: it exists only for as long as this object
 * does, in this process. A build wanting a durable, storable PNG index
 * would need the full zran technique (Z_BLOCK-aligned checkpoints plus
 * inflatePrime() for leftover sub-byte state) -- investigated while
 * scoping this component, found to need meaningfully more work than a
 * dictionary-based shortcut (which fails outright at an arbitrary,
 * non-block-aligned byte offset: confirmed empirically, not assumed) --
 * and is not attempted here.
 *
 * This is why PngCheckpoints is a distinct type from DecodeIndex rather
 * than a field on it: "durable bytes" and "this-process-only handles"
 * must not be confusable at the type level.
 */
class PngCheckpoints {
public:
  PngCheckpoints(const PngCheckpoints &)            = delete;
  PngCheckpoints &operator=(const PngCheckpoints &) = delete;
  PngCheckpoints(PngCheckpoints &&) noexcept;
  PngCheckpoints &operator=(PngCheckpoints &&) noexcept;
  ~PngCheckpoints();

  /**
   * @brief Builds checkpoints for @p pngBytes, every @p rowInterval rows.
   *
   * Requires an 8-bit-per-channel, non-interlaced, truecolour+alpha (PNG
   * colour type 6) image -- the one shape this component handles;
   * interlaced, lower-bit-depth, greyscale and palette PNGs return
   * nullopt, same as a build without zlib or a file that fails to parse
   * as PNG at all. See design/decode-index-spike.md for why this scope
   * limit was chosen deliberately rather than discovered as a gap.
   */
  [[nodiscard]] static std::optional<PngCheckpoints>
  build(std::span<const std::uint8_t> pngBytes, std::uint32_t rowInterval = 8);

  [[nodiscard]] std::uint32_t width() const { return width_; }
  [[nodiscard]] std::uint32_t height() const { return height_; }
  [[nodiscard]] std::size_t checkpointCount() const;

  /**
   * @brief Decodes rows [@p fromRow, @p toRow) from the nearest checkpoint
   *        at or before @p fromRow, discarding any rows before @p fromRow
   *        that had to be decoded to reach it.
   *
   * Each byte-identical row is exactly `width() * 4` bytes (RGBA8),
   * concatenated in row order. Returns nullopt if @p fromRow is before this
   * object's first checkpoint (row 0 always has one, so this can only
   * happen for a genuinely out-of-range request) or the range is otherwise
   * invalid.
   */
  [[nodiscard]] std::optional<std::vector<std::uint8_t>>
  decodeRows(std::uint32_t fromRow, std::uint32_t toRow) const;

private:
  PngCheckpoints();

  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::uint32_t width_{};
  std::uint32_t height_{};
};

} // namespace gleditor

#endif // GLEDITOR_DECODE_INDEX_HPP

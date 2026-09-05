/**
 * @file decode_index.cpp
 * @brief Implementation of gleditor/decode_index.hpp.
 *
 * Ports tools/decode-index-spike.cpp's own proven mechanisms rather than
 * re-deriving them: the PNG checkpoint/inflateCopy() approach, JPEG's
 * reliance on libjpeg-turbo's own restart-interval field, and FFmpeg's
 * container index for video. Each format's code is independently gated by
 * its own optional dependency macro (GLEDITOR_HAVE_DECODE_INDEX_ZLIB/
 * _LIBJPEG/_LIBAV/_ZSTD/_FLAC/_TIFF), so a build with only some of zlib,
 * libjpeg, libav*, libzstd, flac++ and libtiff still gets partial coverage
 * -- see the Makefile's own comment on how these are detected. Zstd
 * additionally needs thirdparty/zstd/contrib/seekable_format's two
 * vendored .c files (zstd_seekable.h declares the ZSTD_seekable_* API they
 * implement) -- see .gitmodules' own comment on why that submodule exists.
 * FLAC and TIFF need no vendoring at all: FLAC's SEEKTABLE metadata block
 * and TIFF's strip/tile offset tags are both part of each format's own
 * spec, and libFLAC++/libtiff implement reading and writing them directly.
 */
#include <gleditor/decode_index.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>

#ifdef GLEDITOR_HAVE_DECODE_INDEX_ZLIB
#include <zlib.h>
#endif

#ifdef GLEDITOR_HAVE_DECODE_INDEX_LIBJPEG
#include <cstdio> // jpeglib.h expects <stdio.h>'s FILE/size_t already visible.
extern "C" {
#include <jpeglib.h>
}
#endif

#ifdef GLEDITOR_HAVE_DECODE_INDEX_LIBAV
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}
#endif

#ifdef GLEDITOR_HAVE_DECODE_INDEX_ZSTD
#include <zstd.h>
#include <zstd_seekable.h>
#endif

#ifdef GLEDITOR_HAVE_DECODE_INDEX_FLAC
#include <FLAC++/decoder.h>
#include <FLAC++/encoder.h>
#endif

#ifdef GLEDITOR_HAVE_DECODE_INDEX_TIFF
#include <tiffio.h>
#endif

namespace gleditor {

// -- PNG ------------------------------------------------------------------

#ifdef GLEDITOR_HAVE_DECODE_INDEX_ZLIB

namespace {

std::uint32_t readU32Be(const std::uint8_t *p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) |
         (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

struct PngHeader {
  std::uint32_t width{};
  std::uint32_t height{};
};

/// Parses just enough of a PNG to hand back its IHDR fields and every IDAT
/// chunk's bytes concatenated in file order. Requires 8-bit-per-channel,
/// non-interlaced truecolour+alpha (colour type 6) -- the one shape this
/// component handles; anything else returns nullopt, the same as a
/// corrupt or non-PNG buffer. Chunk CRCs are not checked: a caller handing
/// this component bytes it already believes are a PNG (from Store, or a
/// test fixture) is trusting them the same way ImageCache::decodeImageBuffer()
/// already does.
std::optional<PngHeader> parsePng(std::span<const std::uint8_t> file,
                                  std::vector<std::uint8_t> &outIdat) {
  static const std::uint8_t signature[8] = {0x89, 'P',  'N',  'G',
                                            0x0D, 0x0A, 0x1A, 0x0A};
  if (file.size() < 8 || 0 != std::memcmp(file.data(), signature, 8)) {
    return std::nullopt;
  }

  std::optional<PngHeader> header;
  std::size_t pos = 8;
  while (pos + 12 <= file.size()) {
    const auto length = readU32Be(&file[pos]);
    const std::string_view type(reinterpret_cast<const char *>(&file[pos + 4]),
                                4);
    const std::size_t dataStart = pos + 8;
    if (dataStart + length + 4 > file.size()) {
      break;
    }
    if ("IHDR" == type && length >= 13) {
      PngHeader h;
      h.width               = readU32Be(&file[dataStart]);
      h.height              = readU32Be(&file[dataStart + 4]);
      const auto bitDepth   = file[dataStart + 8];
      const auto colourType = file[dataStart + 9];
      const auto interlace  = file[dataStart + 12];
      if (8 != bitDepth || 6 != colourType || 0 != interlace) {
        return std::nullopt;
      }
      header = h;
    } else if ("IDAT" == type) {
      outIdat.insert(outIdat.end(), file.begin() + static_cast<long>(dataStart),
                     file.begin() + static_cast<long>(dataStart + length));
    } else if ("IEND" == type) {
      break;
    }
    pos = dataStart + length + 4; // + 4 for the CRC this does not check.
  }
  return header;
}

std::uint8_t paethPredictor(const int a, const int b, const int c) {
  const int p  = a + b - c;
  const int pa = std::abs(p - a);
  const int pb = std::abs(p - b);
  const int pc = std::abs(p - c);
  if (pa <= pb && pa <= pc) {
    return static_cast<std::uint8_t>(a);
  }
  return static_cast<std::uint8_t>(pb <= pc ? b : c);
}

/// PNG's five per-byte scanline predictors -- the one thing zlib does not
/// and cannot abstract away, since it is not a compression algorithm.
void unfilterRow(const std::uint8_t filterType, std::vector<std::uint8_t> &row,
                 const std::vector<std::uint8_t> &prevRow) {
  constexpr int bpp = 4; // RGBA8: exactly what colour type 6 / bit depth 8 is.
  for (std::size_t i = 0; i < row.size(); ++i) {
    const int a = (i >= bpp) ? row[i - bpp] : 0;
    const int b = prevRow.empty() ? 0 : prevRow[i];
    const int c = (i >= bpp) ? (prevRow.empty() ? 0 : prevRow[i - bpp]) : 0;
    switch (filterType) {
    case 0: // None
      break;
    case 1: // Sub
      row[i] = static_cast<std::uint8_t>(row[i] + a);
      break;
    case 2: // Up
      row[i] = static_cast<std::uint8_t>(row[i] + b);
      break;
    case 3: // Average
      row[i] = static_cast<std::uint8_t>(row[i] + ((a + b) / 2));
      break;
    case 4: // Paeth
      row[i] = static_cast<std::uint8_t>(row[i] + paethPredictor(a, b, c));
      break;
    default:
      break; // Malformed filter byte; leave the row as inflate() produced it.
    }
  }
}

std::vector<std::uint8_t> inflateOneRow(z_stream &strm,
                                        const std::size_t rowStride) {
  std::vector<std::uint8_t> row(rowStride);
  strm.next_out  = row.data();
  strm.avail_out = static_cast<uInt>(row.size());
  while (strm.avail_out > 0) {
    const auto ret = inflate(&strm, Z_NO_FLUSH);
    if (Z_OK != ret && Z_STREAM_END != ret) {
      throw std::runtime_error("inflate() failed");
    }
    if (Z_STREAM_END == ret && strm.avail_out > 0) {
      throw std::runtime_error("PNG stream ended mid-row");
    }
  }
  return row;
}

} // namespace

/// One checkpoint: a heap-owned clone of the live inflate stream's state
/// (inflateCopy()) plus the previous scanline's already-unfiltered bytes.
/// Heap-allocated rather than a z_stream held by value: zlib-ng's internal
/// inflate state keeps its own back-pointer to the z_stream struct's
/// address, checked by inflateEnd(). A z_stream held by value inside a
/// struct kept in a std::vector gets bytewise-copied to a new address
/// every time the vector reallocates, desyncing that back-pointer;
/// inflateEnd() then returns Z_STREAM_ERROR without freeing anything.
/// Found via AddressSanitizer while building the original spike, not
/// assumed away -- see design/decode-index-spike.md.
struct PngCheckpoints::Impl {
  struct Checkpoint {
    std::uint32_t rowIndex{};
    std::unique_ptr<z_stream> strm;
    std::vector<std::uint8_t> prevRow;

    explicit Checkpoint(const std::uint32_t row, const z_stream &live,
                        std::vector<std::uint8_t> prev)
        : rowIndex(row), strm(std::make_unique<z_stream>()),
          prevRow(std::move(prev)) {
      strm->zalloc = Z_NULL;
      strm->zfree  = Z_NULL;
      strm->opaque = Z_NULL;
      if (Z_OK != inflateCopy(strm.get(), const_cast<z_stream *>(&live))) {
        throw std::runtime_error("inflateCopy failed");
      }
    }
    Checkpoint(const Checkpoint &)                = delete;
    Checkpoint &operator=(const Checkpoint &)     = delete;
    Checkpoint(Checkpoint &&) noexcept            = default;
    Checkpoint &operator=(Checkpoint &&) noexcept = default;
    ~Checkpoint() {
      if (strm) {
        inflateEnd(strm.get());
      }
    }
  };

  std::vector<Checkpoint> checkpoints;
  // Every checkpoint's cloned z_stream keeps zlib's own next_in/avail_in
  // pointing straight into these bytes -- inflateCopy() copies the z_stream
  // struct, not what it points at. Owned here, alongside the checkpoints
  // that reference it, rather than as a build()-local temporary: a stack
  // buffer of the same bytes would already be dangling by the time a later
  // decodeRows() call dereferenced it. Found the same way the z_stream
  // heap-allocation bug was in the original spike -- by exercising a
  // checkpoint after the call that built it returned, not just inline.
  std::vector<std::uint8_t> idat;
};

#else // !GLEDITOR_HAVE_DECODE_INDEX_ZLIB

// Without zlib, PngCheckpoints::build() always returns nullopt (see below),
// so no instance with a real Impl is ever observable -- but the pImpl
// destructor/move-ops still need *some* complete Impl type to compile
// against, hence this empty stand-in.
struct PngCheckpoints::Impl {};

#endif // GLEDITOR_HAVE_DECODE_INDEX_ZLIB

// Ctor/dtor/move: unconditional. They only touch impl_ through
// std::unique_ptr<Impl>, never zlib types directly, so they compile the
// same way regardless of which Impl (real or empty stand-in) is in scope.
PngCheckpoints::PngCheckpoints() : impl_(std::make_unique<Impl>()) {}
PngCheckpoints::PngCheckpoints(PngCheckpoints &&) noexcept            = default;
PngCheckpoints &PngCheckpoints::operator=(PngCheckpoints &&) noexcept = default;
PngCheckpoints::~PngCheckpoints()                                     = default;

std::size_t PngCheckpoints::checkpointCount() const {
#ifdef GLEDITOR_HAVE_DECODE_INDEX_ZLIB
  return impl_->checkpoints.size();
#else
  return 0;
#endif
}

#ifdef GLEDITOR_HAVE_DECODE_INDEX_ZLIB

std::optional<PngCheckpoints>
PngCheckpoints::build(const std::span<const std::uint8_t> pngBytes,
                      const std::uint32_t rowInterval) {
  PngCheckpoints result;
  const auto header = parsePng(pngBytes, result.impl_->idat);
  if (!header.has_value()) {
    return std::nullopt;
  }
  const auto width             = header->width;
  const auto height            = header->height;
  const std::size_t rowStride  = 1 + (static_cast<std::size_t>(width) * 4);
  const std::uint32_t interval = std::max<std::uint32_t>(1, rowInterval);

  // next_in points into result.impl_->idat rather than a build()-local
  // vector: every checkpoint below clones this z_stream, and the clone's
  // own next_in is copied byte-for-byte, still pointing here -- it has to
  // keep pointing at memory this PngCheckpoints object owns for its whole
  // lifetime, not a buffer that goes away when build() returns.
  z_stream strm{};
  strm.zalloc   = Z_NULL;
  strm.zfree    = Z_NULL;
  strm.opaque   = Z_NULL;
  strm.next_in  = result.impl_->idat.data();
  strm.avail_in = static_cast<uInt>(result.impl_->idat.size());
  if (Z_OK != inflateInit(&strm)) {
    return std::nullopt;
  }

  result.width_  = width;
  result.height_ = height;
  std::vector<std::uint8_t> prevRow;
  try {
    for (std::uint32_t row = 0; row < height; ++row) {
      if (0 == row % interval) {
        result.impl_->checkpoints.emplace_back(row, strm, prevRow);
      }
      auto filtered         = inflateOneRow(strm, rowStride);
      const auto filterType = filtered.front();
      std::vector<std::uint8_t> pixels(filtered.begin() + 1, filtered.end());
      unfilterRow(filterType, pixels, prevRow);
      prevRow = std::move(pixels);
    }
  } catch (const std::runtime_error &) {
    inflateEnd(&strm);
    return std::nullopt;
  }
  inflateEnd(&strm);
  return result;
}

std::optional<std::vector<std::uint8_t>>
PngCheckpoints::decodeRows(const std::uint32_t fromRow,
                           const std::uint32_t toRow) const {
  if (fromRow >= toRow || toRow > height_) {
    return std::nullopt;
  }
  const Impl::Checkpoint *nearest = nullptr;
  for (const auto &cp : impl_->checkpoints) {
    if (cp.rowIndex <= fromRow &&
        (nullptr == nearest || cp.rowIndex > nearest->rowIndex)) {
      nearest = &cp;
    }
  }
  if (nullptr == nearest) {
    return std::nullopt;
  }

  const std::size_t rowStride = 1 + (static_cast<std::size_t>(width_) * 4);
  z_stream resumeStrm{};
  resumeStrm.zalloc = Z_NULL;
  resumeStrm.zfree  = Z_NULL;
  resumeStrm.opaque = Z_NULL;
  if (Z_OK != inflateCopy(&resumeStrm, nearest->strm.get())) {
    return std::nullopt;
  }

  std::vector<std::uint8_t> resumePrevRow = nearest->prevRow;
  std::vector<std::uint8_t> result;
  result.reserve((static_cast<std::size_t>(toRow) - fromRow) *
                 (static_cast<std::size_t>(width_) * 4));
  bool ok = true;
  try {
    for (std::uint32_t row = nearest->rowIndex; row < toRow; ++row) {
      auto filtered         = inflateOneRow(resumeStrm, rowStride);
      const auto filterType = filtered.front();
      std::vector<std::uint8_t> pixels(filtered.begin() + 1, filtered.end());
      unfilterRow(filterType, pixels, resumePrevRow);
      resumePrevRow = pixels;
      if (row >= fromRow) {
        result.insert(result.end(), pixels.begin(), pixels.end());
      }
    }
  } catch (const std::runtime_error &) {
    ok = false;
  }
  inflateEnd(&resumeStrm);
  if (!ok) {
    return std::nullopt;
  }
  return result;
}

namespace {

/// buildDecodeIndex()'s PNG path: a cheap IHDR-only check (no
/// decompression at all) reporting whether PngCheckpoints::build() would
/// succeed on this buffer, without actually building any checkpoints --
/// that is a separate, heavier call a caller makes once it knows it wants
/// to seek. seekPoints stays empty and durableIndex false unconditionally:
/// see PngCheckpoints's own comment for why PNG has no durable index at
/// all in this pass.
DecodeIndex buildPngIndex(const std::span<const std::uint8_t> bytes) {
  DecodeIndex result;
  result.format = DecodeIndexFormat::Png;
  std::vector<std::uint8_t> idat;
  const auto header = parsePng(bytes, idat);
  if (!header.has_value()) {
    return result;
  }
  result.uncompressedExtent = header->height;
  result.seekable           = true;
  result.durableIndex       = false;
  return result;
}

} // namespace

#else // !GLEDITOR_HAVE_DECODE_INDEX_ZLIB

std::optional<PngCheckpoints>
PngCheckpoints::build(std::span<const std::uint8_t> /*pngBytes*/,
                      std::uint32_t /*rowInterval*/) {
  return std::nullopt;
}

std::optional<std::vector<std::uint8_t>>
PngCheckpoints::decodeRows(std::uint32_t /*fromRow*/,
                           std::uint32_t /*toRow*/) const {
  return std::nullopt;
}

#endif // GLEDITOR_HAVE_DECODE_INDEX_ZLIB

// -- JPEG -------------------------------------------------------------------

#ifdef GLEDITOR_HAVE_DECODE_INDEX_LIBJPEG

namespace {

/// libjpeg-turbo's own jpeg_skip_scanlines() already handles seeking
/// through a JPEG's restart markers (see design/decode-index-spike.md);
/// there is no bespoke checkpoint structure to build here at all. This
/// just asks libjpeg whether the file declared a restart interval
/// (cinfo.restart_interval, populated from the file's own DRI marker by
/// jpeg_read_header() -- the library's own answer, not a manual byte
/// parse of the marker) and reports rows/seekability from that.
DecodeIndex buildJpegIndex(const std::span<const std::uint8_t> bytes) {
  DecodeIndex result;
  result.format = DecodeIndexFormat::Jpeg;

  jpeg_decompress_struct cinfo{};
  jpeg_error_mgr jerr{};
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, bytes.data(), static_cast<unsigned long>(bytes.size()));
  if (JPEG_HEADER_OK != jpeg_read_header(&cinfo, TRUE)) {
    jpeg_destroy_decompress(&cinfo);
    return result;
  }
  result.uncompressedExtent = cinfo.image_height;
  // seekable/durableIndex only when the encoder actually emitted restart
  // markers -- jpeg_skip_scanlines() still decodes correctly without them,
  // just by decoding every skipped row internally, which is not a partial
  // decode and should not be reported as one.
  result.seekable     = cinfo.restart_interval > 0;
  result.durableIndex = result.seekable;
  jpeg_destroy_decompress(&cinfo);
  return result;
}

} // namespace

#endif // GLEDITOR_HAVE_DECODE_INDEX_LIBJPEG

// -- Video / MP3 (FFmpeg) ---------------------------------------------------

#ifdef GLEDITOR_HAVE_DECODE_INDEX_LIBAV

namespace {

/// A read/seek callback pair over an in-memory buffer, so buildDecodeIndex()
/// can take bytes (matching every other format here, and ImageCache's own
/// decodeImageBuffer() convention) rather than requiring a file path the
/// way tools/decode-index-spike.cpp's own AvContext did.
struct MemoryReader {
  const std::uint8_t *data;
  std::size_t size;
  std::size_t pos{0};
};

int memoryRead(void *opaque, std::uint8_t *buf, const int bufSize) {
  auto *const reader          = static_cast<MemoryReader *>(opaque);
  const std::size_t remaining = reader->size - reader->pos;
  const std::size_t toRead    = std::min<std::size_t>(bufSize, remaining);
  if (0 == toRead) {
    return AVERROR_EOF;
  }
  std::memcpy(buf, reader->data + reader->pos, toRead);
  reader->pos += toRead;
  return static_cast<int>(toRead);
}

std::int64_t memorySeek(void *opaque, const std::int64_t offset,
                        const int whence) {
  auto *const reader = static_cast<MemoryReader *>(opaque);
  if (AVSEEK_SIZE == whence) {
    return static_cast<std::int64_t>(reader->size);
  }
  std::int64_t newPos = 0;
  if (SEEK_SET == whence) {
    newPos = offset;
  } else if (SEEK_CUR == whence) {
    newPos = static_cast<std::int64_t>(reader->pos) + offset;
  } else if (SEEK_END == whence) {
    newPos = static_cast<std::int64_t>(reader->size) + offset;
  } else {
    return -1;
  }
  if (newPos < 0) {
    return -1;
  }
  reader->pos = static_cast<std::size_t>(newPos);
  return newPos;
}

/// Frees an AVFormatContext opened via openMemoryFormat() plus its custom
/// AVIOContext, in the order FFmpeg's own examples free them -- avformat's
/// AVFMT_FLAG_CUSTOM_IO tells it not to free `pb` itself, so the caller
/// must, separately, after the format context is closed.
struct AvFormatHandle {
  AVFormatContext *fmt{nullptr};
  AVIOContext *avio{nullptr};

  AvFormatHandle()                                  = default;
  AvFormatHandle(const AvFormatHandle &)            = delete;
  AvFormatHandle &operator=(const AvFormatHandle &) = delete;
  AvFormatHandle(AvFormatHandle &&other) noexcept
      : fmt(other.fmt), avio(other.avio) {
    other.fmt  = nullptr;
    other.avio = nullptr;
  }
  AvFormatHandle &operator=(AvFormatHandle &&other) noexcept {
    if (this != &other) {
      this->~AvFormatHandle();
      fmt        = other.fmt;
      avio       = other.avio;
      other.fmt  = nullptr;
      other.avio = nullptr;
    }
    return *this;
  }
  ~AvFormatHandle() {
    if (nullptr != fmt) {
      avformat_close_input(&fmt);
    }
    if (nullptr != avio) {
      av_freep(&avio->buffer);
      avio_context_free(&avio);
    }
  }
};

/// MemoryReader must outlive the returned AvFormatHandle (the AVIOContext
/// keeps a raw pointer to it as its opaque callback argument) -- callers
/// keep both on the stack together, reader declared first so it outlives
/// the handle during teardown too.
std::optional<AvFormatHandle> openMemoryFormat(MemoryReader &reader) {
  constexpr int ioBufferSize = 4096;
  auto *ioBuffer = static_cast<unsigned char *>(av_malloc(ioBufferSize));
  if (nullptr == ioBuffer) {
    return std::nullopt;
  }
  AvFormatHandle handle;
  handle.avio = avio_alloc_context(ioBuffer, ioBufferSize, 0, &reader,
                                   memoryRead, nullptr, memorySeek);
  if (nullptr == handle.avio) {
    av_freep(&ioBuffer);
    return std::nullopt;
  }
  handle.fmt     = avformat_alloc_context();
  handle.fmt->pb = handle.avio;
  handle.fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
  if (avformat_open_input(&handle.fmt, "", nullptr, nullptr) < 0) {
    // avformat_open_input() frees fmt itself on failure; avoid a double
    // free by clearing it here before AvFormatHandle's destructor runs.
    handle.fmt = nullptr;
    return std::nullopt;
  }
  if (avformat_find_stream_info(handle.fmt, nullptr) < 0) {
    return std::nullopt;
  }
  return handle;
}

/// Shared by Video and Mp3: open @p bytes, find the first stream of
/// @p mediaType, and enumerate whatever seek index FFmpeg itself built for
/// it. durableIndex is computed from what was actually found, not assumed
/// per format -- confirmed empirically that MP4/H.264 gets a full index
/// for free (avformat_index_get_entry()) while MP3 gets none at all (its
/// own demuxer builds no index; av_seek_frame() still works there via
/// bitrate-position estimation, which is why `seekable` does not depend on
/// `seekPoints` being non-empty).
DecodeIndex buildAvIndex(std::span<const std::uint8_t> bytes,
                         const AVMediaType mediaType,
                         const DecodeIndexFormat format) {
  DecodeIndex result;
  result.format = format;

  MemoryReader reader{bytes.data(), bytes.size()};
  auto handle = openMemoryFormat(reader);
  if (!handle.has_value()) {
    return result;
  }

  int streamIndex = -1;
  for (unsigned i = 0; i < handle->fmt->nb_streams; ++i) {
    if (mediaType == handle->fmt->streams[i]->codecpar->codec_type) {
      streamIndex = static_cast<int>(i);
      break;
    }
  }
  if (streamIndex < 0) {
    return result;
  }
  result.seekable = true;

  auto *const stream = handle->fmt->streams[streamIndex];
  for (int i = 0;; ++i) {
    const auto *const entry = avformat_index_get_entry(stream, i);
    if (nullptr == entry) {
      break;
    }
    result.seekPoints.push_back(
        SeekPoint{static_cast<std::uint64_t>(entry->timestamp),
                  static_cast<std::uint64_t>(entry->pos)});
  }
  result.durableIndex       = !result.seekPoints.empty();
  result.uncompressedExtent = result.seekPoints.size();
  return result;
}

} // namespace

std::optional<std::pair<std::uint32_t, std::uint32_t>>
peekVideoSize(const std::span<const std::uint8_t> videoBytes) {
  MemoryReader reader{videoBytes.data(), videoBytes.size()};
  auto handle = openMemoryFormat(reader);
  if (!handle.has_value()) {
    return std::nullopt;
  }
  for (unsigned i = 0; i < handle->fmt->nb_streams; ++i) {
    const auto *const params = handle->fmt->streams[i]->codecpar;
    if (AVMEDIA_TYPE_VIDEO == params->codec_type && params->width > 0 &&
        params->height > 0) {
      return std::make_pair(static_cast<std::uint32_t>(params->width),
                            static_cast<std::uint32_t>(params->height));
    }
  }
  return std::nullopt;
}

#else // !GLEDITOR_HAVE_DECODE_INDEX_LIBAV

std::optional<std::pair<std::uint32_t, std::uint32_t>>
peekVideoSize(std::span<const std::uint8_t> /*videoBytes*/) {
  return std::nullopt;
}

#endif // GLEDITOR_HAVE_DECODE_INDEX_LIBAV

// -- Zstd ---------------------------------------------------------------

#ifdef GLEDITOR_HAVE_DECODE_INDEX_ZSTD

namespace {

/// Move-only RAII for ZSTD_seekable*, the same explicit-move-plus-deleted-
/// copy discipline AvFormatHandle above uses -- a destructor alone would
/// let the implicitly-generated copy constructor bitwise-copy the pointer
/// and double-free it, the exact bug that lesson was learned from
/// originally (see AvContext's own comment, and design/decode-index-spike.md).
struct ZstdSeekableHandle {
  ZSTD_seekable *zs{nullptr};

  ZstdSeekableHandle()                                      = default;
  ZstdSeekableHandle(const ZstdSeekableHandle &)            = delete;
  ZstdSeekableHandle &operator=(const ZstdSeekableHandle &) = delete;
  ZstdSeekableHandle(ZstdSeekableHandle &&other) noexcept : zs(other.zs) {
    other.zs = nullptr;
  }
  ZstdSeekableHandle &operator=(ZstdSeekableHandle &&other) noexcept {
    if (this != &other) {
      this->~ZstdSeekableHandle();
      zs       = other.zs;
      other.zs = nullptr;
    }
    return *this;
  }
  ~ZstdSeekableHandle() {
    if (nullptr != zs) {
      ZSTD_seekable_free(zs);
    }
  }
};

/// Whether @p bytes is in zstd's own seekable format --
/// ZSTD_seekable_initBuff() validates the trailing seek table itself (footer
/// magic, checksum), so a plain (non-seekable) zstd file or a truncated/corrupt
/// one is rejected the same way, not distinguished further: either way there is
/// no seek table to report seek points from.
DecodeIndex buildZstdIndex(const std::span<const std::uint8_t> bytes) {
  DecodeIndex result;
  result.format = DecodeIndexFormat::Zstd;

  ZstdSeekableHandle handle;
  handle.zs = ZSTD_seekable_create();
  if (nullptr == handle.zs) {
    return result;
  }
  if (ZSTD_isError(
          ZSTD_seekable_initBuff(handle.zs, bytes.data(), bytes.size()))) {
    return result;
  }

  const unsigned frameCount = ZSTD_seekable_getNumFrames(handle.zs);
  result.seekPoints.reserve(frameCount);
  for (unsigned i = 0; i < frameCount; ++i) {
    result.seekPoints.push_back(
        SeekPoint{ZSTD_seekable_getFrameDecompressedOffset(handle.zs, i),
                  ZSTD_seekable_getFrameCompressedOffset(handle.zs, i)});
  }
  if (frameCount > 0) {
    result.uncompressedExtent =
        ZSTD_seekable_getFrameDecompressedOffset(handle.zs, frameCount - 1) +
        ZSTD_seekable_getFrameDecompressedSize(handle.zs, frameCount - 1);
  }
  result.seekable     = true;
  result.durableIndex = true;
  return result;
}

} // namespace

std::optional<std::vector<std::uint8_t>>
reencodeZstdSeekable(const std::span<const std::uint8_t> zstdBytes,
                     const std::uint32_t maxFrameSize) {
  // Decompress fully first, via the plain streaming API rather than trusting
  // any declared content size: the input might already be in the seekable
  // format (whose own per-frame sizes are not one whole-file size), or have
  // no declared size at all. Confirmed empirically that plain
  // ZSTD_decompressStream() correctly and transparently skips a seekable
  // file's own trailing seek-table frames (they are ordinary zstd
  // "skippable frames," which the format itself requires any decoder to
  // pass over) -- re-encoding an already-seekable file is meant to work via
  // this same path, not a special case.
  auto *const dstream = ZSTD_createDStream();
  if (nullptr == dstream) {
    return std::nullopt;
  }
  if (ZSTD_isError(ZSTD_initDStream(dstream))) {
    ZSTD_freeDStream(dstream);
    return std::nullopt;
  }
  std::vector<std::uint8_t> decompressed;
  std::vector<std::uint8_t> ioBuf(1U << 17);
  ZSTD_inBuffer din{zstdBytes.data(), zstdBytes.size(), 0};
  bool decodeOk = true;
  do {
    ZSTD_outBuffer dout{ioBuf.data(), ioBuf.size(), 0};
    if (ZSTD_isError(ZSTD_decompressStream(dstream, &dout, &din))) {
      decodeOk = false;
      break;
    }
    decompressed.insert(decompressed.end(), ioBuf.begin(),
                        ioBuf.begin() + static_cast<long>(dout.pos));
  } while (din.pos < din.size);
  ZSTD_freeDStream(dstream);
  if (!decodeOk) {
    return std::nullopt;
  }

  auto *const cstream = ZSTD_seekable_createCStream();
  if (nullptr == cstream) {
    return std::nullopt;
  }
  if (ZSTD_isError(ZSTD_seekable_initCStream(
          cstream, ZSTD_CLEVEL_DEFAULT, /*checksumFlag=*/1, maxFrameSize))) {
    ZSTD_seekable_freeCStream(cstream);
    return std::nullopt;
  }
  std::vector<std::uint8_t> result;
  ZSTD_inBuffer cin{decompressed.data(), decompressed.size(), 0};
  bool compressOk = true;
  while (compressOk && cin.pos < cin.size) {
    ZSTD_outBuffer cout{ioBuf.data(), ioBuf.size(), 0};
    if (ZSTD_isError(ZSTD_seekable_compressStream(cstream, &cout, &cin))) {
      compressOk = false;
      break;
    }
    result.insert(result.end(), ioBuf.begin(),
                  ioBuf.begin() + static_cast<long>(cout.pos));
  }
  if (compressOk) {
    std::size_t remaining;
    do {
      ZSTD_outBuffer cout{ioBuf.data(), ioBuf.size(), 0};
      remaining = ZSTD_seekable_endStream(cstream, &cout);
      if (ZSTD_isError(remaining)) {
        compressOk = false;
        break;
      }
      result.insert(result.end(), ioBuf.begin(),
                    ioBuf.begin() + static_cast<long>(cout.pos));
    } while (remaining > 0);
  }
  ZSTD_seekable_freeCStream(cstream);
  if (!compressOk) {
    return std::nullopt;
  }
  return result;
}

#else // !GLEDITOR_HAVE_DECODE_INDEX_ZSTD

std::optional<std::vector<std::uint8_t>>
reencodeZstdSeekable(std::span<const std::uint8_t> /*zstdBytes*/,
                     std::uint32_t /*maxFrameSize*/) {
  return std::nullopt;
}

#endif // GLEDITOR_HAVE_DECODE_INDEX_ZSTD

// -- FLAC ---------------------------------------------------------------

#ifdef GLEDITOR_HAVE_DECODE_INDEX_FLAC

namespace {

/// Shared read-side plumbing for both buildFlacIndex() (metadata only, via
/// process_until_end_of_metadata()) and reencodeFlacSeekable() (a full
/// decode, via process_until_end_of_stream()) -- one class serves both
/// since write_callback() is simply never invoked on the metadata-only
/// path, the same way JPEG's restart-interval check never calls
/// jpeg_read_scanlines().
class MemoryFlacDecoder : public FLAC::Decoder::Stream {
public:
  explicit MemoryFlacDecoder(const std::span<const std::uint8_t> bytes)
      : bytes_(bytes) {}

  FLAC__uint64 totalSamples{};
  std::uint32_t sampleRate{};
  std::uint32_t channels{};
  std::uint32_t bitsPerSample{};
  bool sawStreamInfo{false};
  /// Raw seek points exactly as the SEEKTABLE block stores them --
  /// stream_offset relative to the first audio frame, not yet an absolute
  /// file offset. buildFlacIndex() adds that base once, after decoding
  /// stops; nothing in this class needs to know it.
  std::vector<SeekPoint> rawSeekPoints;
  /// One vector per channel, matching FLAC__StreamEncoder::process()'s own
  /// expected input shape -- storing interleaved and de-interleaving again
  /// at encode time would just be wasted work reencodeFlacSeekable() has
  /// no reason to do.
  std::vector<std::vector<FLAC__int32>> pcm;

protected:
  ::FLAC__StreamDecoderReadStatus read_callback(FLAC__byte buffer[],
                                                std::size_t *bytes) override {
    const std::size_t remaining = bytes_.size() - pos_;
    const std::size_t toRead    = std::min(*bytes, remaining);
    if (0 == toRead) {
      *bytes = 0;
      return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }
    std::memcpy(buffer, bytes_.data() + pos_, toRead);
    pos_ += toRead;
    *bytes = toRead;
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
  }
  ::FLAC__StreamDecoderSeekStatus
  seek_callback(const FLAC__uint64 absoluteByteOffset) override {
    if (absoluteByteOffset > bytes_.size()) {
      return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    }
    pos_ = static_cast<std::size_t>(absoluteByteOffset);
    return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
  }
  ::FLAC__StreamDecoderTellStatus
  tell_callback(FLAC__uint64 *absoluteByteOffset) override {
    *absoluteByteOffset = pos_;
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
  }
  ::FLAC__StreamDecoderLengthStatus
  length_callback(FLAC__uint64 *streamLength) override {
    *streamLength = bytes_.size();
    return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
  }
  bool eof_callback() override { return pos_ >= bytes_.size(); }

  ::FLAC__StreamDecoderWriteStatus
  write_callback(const ::FLAC__Frame *frame,
                 const FLAC__int32 *const buffer[]) override {
    if (pcm.size() != frame->header.channels) {
      pcm.assign(frame->header.channels, {});
    }
    for (std::uint32_t ch = 0; ch < frame->header.channels; ++ch) {
      pcm[ch].insert(pcm[ch].end(), buffer[ch],
                     buffer[ch] + frame->header.blocksize);
    }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
  }

  void metadata_callback(const ::FLAC__StreamMetadata *metadata) override {
    if (FLAC__METADATA_TYPE_STREAMINFO == metadata->type) {
      sawStreamInfo = true;
      totalSamples  = metadata->data.stream_info.total_samples;
      sampleRate    = metadata->data.stream_info.sample_rate;
      channels      = metadata->data.stream_info.channels;
      bitsPerSample = metadata->data.stream_info.bits_per_sample;
    } else if (FLAC__METADATA_TYPE_SEEKTABLE == metadata->type) {
      const auto &table = metadata->data.seek_table;
      rawSeekPoints.reserve(table.num_points);
      for (std::uint32_t i = 0; i < table.num_points; ++i) {
        const auto &pt = table.points[i];
        // A well-formed file's own table should never still have
        // placeholders, but a hand-crafted or truncated one might --
        // skipped rather than reported as a bogus seek point.
        if (FLAC__STREAM_METADATA_SEEKPOINT_PLACEHOLDER == pt.sample_number) {
          continue;
        }
        rawSeekPoints.push_back(SeekPoint{pt.sample_number, pt.stream_offset});
      }
    }
  }

  // Decode errors already surface as init()/process_*() returning false;
  // there is nothing else useful to do with the status code here.
  void error_callback(::FLAC__StreamDecoderErrorStatus /*status*/) override {}

private:
  std::span<const std::uint8_t> bytes_;
  std::size_t pos_{0};
};

/// Write-side plumbing for reencodeFlacSeekable() -- a growable in-memory
/// output buffer with real seek/tell support, which is specifically what
/// lets the encoder resolve seek-table placeholders to real byte offsets
/// after the fact (FLAC__stream_encoder_set_metadata()'s own documentation:
/// "if output seeking is possible").
class MemoryFlacEncoder : public FLAC::Encoder::Stream {
public:
  std::vector<std::uint8_t> output;

protected:
  ::FLAC__StreamEncoderWriteStatus
  write_callback(const FLAC__byte buffer[], const std::size_t bytes,
                 std::uint32_t /*samples*/,
                 std::uint32_t /*currentFrame*/) override {
    if (pos_ + bytes > output.size()) {
      output.resize(pos_ + bytes);
    }
    std::memcpy(output.data() + pos_, buffer, bytes);
    pos_ += bytes;
    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
  }
  ::FLAC__StreamEncoderSeekStatus
  seek_callback(const FLAC__uint64 absoluteByteOffset) override {
    pos_ = static_cast<std::size_t>(absoluteByteOffset);
    return FLAC__STREAM_ENCODER_SEEK_STATUS_OK;
  }
  ::FLAC__StreamEncoderTellStatus
  tell_callback(FLAC__uint64 *absoluteByteOffset) override {
    *absoluteByteOffset = pos_;
    return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
  }

private:
  std::size_t pos_{0};
};

DecodeIndex buildFlacIndex(const std::span<const std::uint8_t> bytes) {
  DecodeIndex result;
  result.format = DecodeIndexFormat::Flac;

  MemoryFlacDecoder decoder(bytes);
  decoder.set_metadata_respond(FLAC__METADATA_TYPE_SEEKTABLE);
  if (FLAC__STREAM_DECODER_INIT_STATUS_OK != decoder.init()) {
    return result;
  }
  if (!decoder.process_until_end_of_metadata() || !decoder.sawStreamInfo) {
    return result;
  }
  result.uncompressedExtent = decoder.totalSamples;

  // FLAC's own SeekPoint::stream_offset is relative to the start of the
  // first audio frame, not the file -- get_decode_position() right after
  // process_until_end_of_metadata() returns is exactly that boundary (the
  // byte the decoder will read next), which is what makes stream_offset
  // usable as a byte offset into the *original* file bytes this component
  // hands back rather than an offset nothing else can interpret.
  FLAC__uint64 audioStartOffset = 0;
  if (!decoder.rawSeekPoints.empty() &&
      decoder.get_decode_position(&audioStartOffset)) {
    result.seekPoints.reserve(decoder.rawSeekPoints.size());
    for (const auto &pt : decoder.rawSeekPoints) {
      result.seekPoints.push_back(SeekPoint{
          pt.uncompressedPosition, audioStartOffset + pt.compressedByteOffset});
    }
    result.durableIndex = true;
  }
  // Seekable either way: libFLAC's own seek_absolute() works via frame sync
  // codes even with no seek table at all (slower, but correct -- confirmed
  // empirically, not assumed, the same way MP3's bitrate-estimation seeking
  // was). A seek table just makes that seek cheaper.
  result.seekable = true;
  return result;
}

} // namespace

std::optional<std::vector<std::uint8_t>>
reencodeFlacSeekable(const std::span<const std::uint8_t> flacBytes,
                     const float secondsPerSeekPoint) {
  MemoryFlacDecoder decoder(flacBytes);
  if (FLAC__STREAM_DECODER_INIT_STATUS_OK != decoder.init()) {
    return std::nullopt;
  }
  if (!decoder.process_until_end_of_stream() || !decoder.sawStreamInfo ||
      0 == decoder.sampleRate || decoder.pcm.empty()) {
    return std::nullopt;
  }

  const auto samplesPerPoint = std::max<FLAC__uint64>(
      1, static_cast<FLAC__uint64>(secondsPerSeekPoint *
                                   static_cast<float>(decoder.sampleRate)));
  FLAC::Metadata::SeekTable seekTable;
  seekTable.template_append_spaced_points_by_samples(
      static_cast<std::uint32_t>(samplesPerPoint), decoder.totalSamples);
  seekTable.template_sort(/*compact=*/true);

  MemoryFlacEncoder encoder;
  encoder.set_channels(decoder.channels);
  encoder.set_bits_per_sample(decoder.bitsPerSample);
  encoder.set_sample_rate(decoder.sampleRate);
  encoder.set_total_samples_estimate(decoder.totalSamples);
  // Prototype's own const conversion operator is the only public way to
  // reach the underlying ::FLAC__StreamMetadata* from the C++ wrapper;
  // set_metadata() deep-copies the block rather than retaining the
  // pointer, so hand it a const-cast one rather than a mutable copy.
  ::FLAC__StreamMetadata *metadataBlocks[1] = {
      const_cast<::FLAC__StreamMetadata *>(
          static_cast<const ::FLAC__StreamMetadata *>(seekTable))};
  encoder.set_metadata(metadataBlocks, 1);
  if (FLAC__STREAM_ENCODER_INIT_STATUS_OK != encoder.init()) {
    return std::nullopt;
  }

  std::vector<const FLAC__int32 *> channelPointers(decoder.channels);
  for (std::uint32_t ch = 0; ch < decoder.channels; ++ch) {
    channelPointers[ch] = decoder.pcm[ch].data();
  }
  const bool ok =
      encoder.process(channelPointers.data(),
                      static_cast<std::uint32_t>(decoder.pcm[0].size()));
  encoder.finish();
  if (!ok) {
    return std::nullopt;
  }
  return std::move(encoder.output);
}

#else // !GLEDITOR_HAVE_DECODE_INDEX_FLAC

namespace {
DecodeIndex buildFlacIndex(std::span<const std::uint8_t> /*bytes*/) {
  return DecodeIndex{};
}
} // namespace

std::optional<std::vector<std::uint8_t>>
reencodeFlacSeekable(std::span<const std::uint8_t> /*flacBytes*/,
                     float /*secondsPerSeekPoint*/) {
  return std::nullopt;
}

#endif // GLEDITOR_HAVE_DECODE_INDEX_FLAC

// -- TIFF -----------------------------------------------------------------

#ifdef GLEDITOR_HAVE_DECODE_INDEX_TIFF

namespace {

/// Read-only in-memory source for TIFFClientOpen(). Never writes -- the
/// write callback exists only because libtiff's C API takes one
/// unconditionally, and is never invoked for an "r"-mode handle.
struct MemoryTiffReader {
  std::span<const std::uint8_t> bytes;
  std::size_t pos{0};
};

tmsize_t tiffReaderRead(thandle_t handle, void *buf, const tmsize_t size) {
  auto *const reader          = static_cast<MemoryTiffReader *>(handle);
  const std::size_t remaining = reader->bytes.size() - reader->pos;
  const std::size_t toRead =
      std::min<std::size_t>(static_cast<std::size_t>(size), remaining);
  std::memcpy(buf, reader->bytes.data() + reader->pos, toRead);
  reader->pos += toRead;
  return static_cast<tmsize_t>(toRead);
}
tmsize_t tiffReaderWrite(thandle_t, void *, tmsize_t) { return -1; }
toff_t tiffReaderSeek(thandle_t handle, const toff_t offset, const int whence) {
  auto *const reader  = static_cast<MemoryTiffReader *>(handle);
  std::int64_t newPos = 0;
  if (SEEK_SET == whence) {
    newPos = static_cast<std::int64_t>(offset);
  } else if (SEEK_CUR == whence) {
    newPos = static_cast<std::int64_t>(reader->pos) + offset;
  } else if (SEEK_END == whence) {
    newPos = static_cast<std::int64_t>(reader->bytes.size()) + offset;
  }
  if (newPos < 0) {
    return static_cast<toff_t>(-1);
  }
  reader->pos = static_cast<std::size_t>(newPos);
  return static_cast<toff_t>(reader->pos);
}
int tiffReaderClose(thandle_t) { return 0; }
toff_t tiffReaderSize(thandle_t handle) {
  return static_cast<toff_t>(
      static_cast<MemoryTiffReader *>(handle)->bytes.size());
}

/// Growable in-memory sink for TIFFClientOpen(). Never reads -- symmetric
/// reasoning to MemoryTiffReader's unused write callback.
struct MemoryTiffWriter {
  std::vector<std::uint8_t> data;
  std::size_t pos{0};
};

tmsize_t tiffWriterRead(thandle_t, void *, tmsize_t) { return -1; }
tmsize_t tiffWriterWrite(thandle_t handle, void *buf, const tmsize_t size) {
  auto *const writer      = static_cast<MemoryTiffWriter *>(handle);
  const auto sizeUnsigned = static_cast<std::size_t>(size);
  if (writer->pos + sizeUnsigned > writer->data.size()) {
    writer->data.resize(writer->pos + sizeUnsigned);
  }
  std::memcpy(writer->data.data() + writer->pos, buf, sizeUnsigned);
  writer->pos += sizeUnsigned;
  return size;
}
toff_t tiffWriterSeek(thandle_t handle, const toff_t offset, const int whence) {
  auto *const writer  = static_cast<MemoryTiffWriter *>(handle);
  std::int64_t newPos = 0;
  if (SEEK_SET == whence) {
    newPos = static_cast<std::int64_t>(offset);
  } else if (SEEK_CUR == whence) {
    newPos = static_cast<std::int64_t>(writer->pos) + offset;
  } else if (SEEK_END == whence) {
    newPos = static_cast<std::int64_t>(writer->data.size()) + offset;
  }
  if (newPos < 0) {
    return static_cast<toff_t>(-1);
  }
  writer->pos = static_cast<std::size_t>(newPos);
  return static_cast<toff_t>(writer->pos);
}
int tiffWriterClose(thandle_t) { return 0; }
toff_t tiffWriterSize(thandle_t handle) {
  return static_cast<toff_t>(
      static_cast<MemoryTiffWriter *>(handle)->data.size());
}

/// Move-only RAII for the libtiff C handle -- the same explicit-move-plus-
/// deleted-copy discipline AvFormatHandle/ZstdSeekableHandle above use.
struct TiffHandle {
  TIFF *tif{nullptr};

  TiffHandle()                              = default;
  TiffHandle(const TiffHandle &)            = delete;
  TiffHandle &operator=(const TiffHandle &) = delete;
  TiffHandle(TiffHandle &&other) noexcept : tif(other.tif) {
    other.tif = nullptr;
  }
  TiffHandle &operator=(TiffHandle &&other) noexcept {
    if (this != &other) {
      this->~TiffHandle();
      tif       = other.tif;
      other.tif = nullptr;
    }
    return *this;
  }
  ~TiffHandle() {
    if (nullptr != tif) {
      TIFFClose(tif);
    }
  }
};

/// @p reader must outlive the returned handle -- TIFFClientOpen() keeps a
/// raw pointer to it as every callback's opaque argument, the same
/// constraint AvFormatHandle's own MemoryReader documents.
std::optional<TiffHandle> openMemoryTiffForRead(MemoryTiffReader &reader) {
  TiffHandle handle;
  handle.tif = TIFFClientOpen(
      "gleditor-decode-index", "r", &reader, tiffReaderRead, tiffReaderWrite,
      tiffReaderSeek, tiffReaderClose, tiffReaderSize, nullptr, nullptr);
  if (nullptr == handle.tif) {
    return std::nullopt;
  }
  return handle;
}

DecodeIndex buildTiffIndex(const std::span<const std::uint8_t> bytes) {
  DecodeIndex result;
  result.format = DecodeIndexFormat::Tiff;

  MemoryTiffReader reader{bytes};
  auto handle = openMemoryTiffForRead(reader);
  if (!handle.has_value()) {
    return result;
  }
  auto *const tif = handle->tif;

  std::uint32_t height = 0;
  TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height);
  result.uncompressedExtent = height;
  // TIFFReadEncodedStrip()/TIFFReadEncodedTile() always work -- strip/tile
  // offsets are just part of the format, not something an encoder opts
  // into. See this function's own header comment for why tiled TIFFs stop
  // here rather than populating seekPoints.
  result.seekable = true;

  if (TIFFIsTiled(tif)) {
    return result;
  }

  std::uint32_t rowsPerStrip = 0;
  TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &rowsPerStrip);
  const auto numStrips        = TIFFNumberOfStrips(tif);
  std::uint64_t *stripOffsets = nullptr;
  if (0 == numStrips ||
      !TIFFGetField(tif, TIFFTAG_STRIPOFFSETS, &stripOffsets)) {
    return result;
  }
  result.seekPoints.reserve(numStrips);
  for (std::uint32_t i = 0; i < numStrips; ++i) {
    result.seekPoints.push_back(SeekPoint{
        static_cast<std::uint64_t>(i) * rowsPerStrip, stripOffsets[i]});
  }
  result.durableIndex = true;
  return result;
}

} // namespace

std::optional<std::vector<std::uint8_t>>
reencodeTiffSeekable(const std::span<const std::uint8_t> tiffBytes,
                     const std::uint32_t rowsPerStrip) {
  MemoryTiffReader reader{tiffBytes};
  auto srcHandle = openMemoryTiffForRead(reader);
  if (!srcHandle.has_value()) {
    return std::nullopt;
  }

  std::uint32_t width = 0, height = 0;
  TIFFGetField(srcHandle->tif, TIFFTAG_IMAGEWIDTH, &width);
  TIFFGetField(srcHandle->tif, TIFFTAG_IMAGELENGTH, &height);
  if (0 == width || 0 == height) {
    return std::nullopt;
  }

  // TIFFReadRGBAImageOriented() normalises whatever photometric
  // interpretation, bit depth, and planar configuration the source used
  // into 8-bit RGBA -- the one shape this function re-encodes, the same
  // scope-narrowing choice PngCheckpoints makes for colour type 6.
  std::vector<std::uint32_t> rgba(static_cast<std::size_t>(width) * height);
  const bool decodedOk = TIFFReadRGBAImageOriented(
      srcHandle->tif, width, height, rgba.data(), ORIENTATION_TOPLEFT, 0);
  srcHandle.reset(); // Close the source before opening the destination writer.
  if (!decodedOk) {
    return std::nullopt;
  }

  MemoryTiffWriter writer;
  {
    TiffHandle out;
    out.tif = TIFFClientOpen("gleditor-decode-index", "w", &writer,
                             tiffWriterRead, tiffWriterWrite, tiffWriterSeek,
                             tiffWriterClose, tiffWriterSize, nullptr, nullptr);
    if (nullptr == out.tif) {
      return std::nullopt;
    }
    TIFFSetField(out.tif, TIFFTAG_IMAGEWIDTH, width);
    TIFFSetField(out.tif, TIFFTAG_IMAGELENGTH, height);
    TIFFSetField(out.tif, TIFFTAG_SAMPLESPERPIXEL, 4);
    TIFFSetField(out.tif, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(out.tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(out.tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(out.tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    // ADOBE_DEFLATE (not the legacy COMPRESSION_DEFLATE identifier) is what
    // libtiff itself recommends -- found via its own warning while
    // scoping this, not assumed.
    TIFFSetField(out.tif, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
    // The 4th sample is alpha, not an unrelated extra channel -- found via
    // libtiff's own warning while scoping this (an unset ExtraSamples tag
    // leaves the alpha channel's meaning ambiguous per the TIFF spec, not
    // just noisy) and fixed rather than suppressed.
    const std::uint16_t extraSamples[] = {EXTRASAMPLE_UNASSALPHA};
    TIFFSetField(out.tif, TIFFTAG_EXTRASAMPLES, 1, extraSamples);
    TIFFSetField(out.tif, TIFFTAG_ROWSPERSTRIP,
                 std::max<std::uint32_t>(1, std::min(rowsPerStrip, height)));

    std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * 4);
    bool writeOk = true;
    for (std::uint32_t y = 0; y < height && writeOk; ++y) {
      std::memcpy(row.data(), rgba.data() + static_cast<std::size_t>(y) * width,
                  row.size());
      writeOk = TIFFWriteScanline(out.tif, row.data(), y, 0) >= 0;
    }
    if (!writeOk) {
      return std::nullopt;
    }
  } // out's destructor closes the file, flushing the final directory.

  return std::move(writer.data);
}

#else // !GLEDITOR_HAVE_DECODE_INDEX_TIFF

namespace {
DecodeIndex buildTiffIndex(std::span<const std::uint8_t> /*bytes*/) {
  return DecodeIndex{};
}
} // namespace

std::optional<std::vector<std::uint8_t>>
reencodeTiffSeekable(std::span<const std::uint8_t> /*tiffBytes*/,
                     std::uint32_t /*rowsPerStrip*/) {
  return std::nullopt;
}

#endif // GLEDITOR_HAVE_DECODE_INDEX_TIFF

// -- Dispatch ---------------------------------------------------------------

DecodeIndex buildDecodeIndex(const std::span<const std::uint8_t> bytes,
                             const MimeType &mime) {
  const auto essence = mime.essence();

  if ("image/png" == essence) {
#ifdef GLEDITOR_HAVE_DECODE_INDEX_ZLIB
    return buildPngIndex(bytes);
#else
    return DecodeIndex{};
#endif
  }
  if ("image/jpeg" == essence) {
#ifdef GLEDITOR_HAVE_DECODE_INDEX_LIBJPEG
    return buildJpegIndex(bytes);
#else
    return DecodeIndex{};
#endif
  }
  if ("video" == mime.type()) {
#ifdef GLEDITOR_HAVE_DECODE_INDEX_LIBAV
    return buildAvIndex(bytes, AVMEDIA_TYPE_VIDEO, DecodeIndexFormat::Video);
#else
    return DecodeIndex{};
#endif
  }
  if ("audio/mpeg" == essence || "audio/mp3" == essence) {
#ifdef GLEDITOR_HAVE_DECODE_INDEX_LIBAV
    return buildAvIndex(bytes, AVMEDIA_TYPE_AUDIO, DecodeIndexFormat::Mp3);
#else
    return DecodeIndex{};
#endif
  }
  if ("application/zstd" == essence) {
#ifdef GLEDITOR_HAVE_DECODE_INDEX_ZSTD
    return buildZstdIndex(bytes);
#else
    return DecodeIndex{};
#endif
  }
  if ("audio/flac" == essence || "audio/x-flac" == essence) {
    return buildFlacIndex(bytes);
  }
  if ("image/tiff" == essence) {
    return buildTiffIndex(bytes);
  }
  return DecodeIndex{};
}

} // namespace gleditor

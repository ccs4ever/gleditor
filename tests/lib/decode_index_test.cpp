/**
 * @file decode_index_test.cpp
 * @brief Unit tests for gleditor::buildDecodeIndex() and PngCheckpoints.
 *
 * Always compiled, matching test_image_cache.cpp's precedent for an optional
 * capability (SDL_image there, zlib/libjpeg/libav* here): every format-
 * specific test checks the returned DecodeIndex::format first and skips
 * (not fails) when it comes back Unsupported, which is what buildDecodeIndex()
 * reports for a build missing that format's optional dependency -- see
 * decode_index.cpp's own dispatch. Fixtures are the same ones
 * tools/decode-index-spike.cpp already verified these mechanisms against.
 */
#include <gleditor/decode_index.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#ifdef GLEDITOR_HAVE_DECODE_INDEX_ZSTD
#include <zstd.h>
#endif

#ifdef GLEDITOR_HAVE_DECODE_INDEX_FLAC
#include <FLAC++/decoder.h>
#endif

#ifdef GLEDITOR_HAVE_DECODE_INDEX_TIFF
#include <tiffio.h>
#endif

namespace gleditor {
namespace {

std::vector<std::uint8_t> readFile(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return {};
  }
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}

#ifdef GLEDITOR_HAVE_DECODE_INDEX_ZSTD
/// Plain streaming decode, used only to verify reencodeZstdSeekable()'s
/// output round-trips to the original content -- not something
/// decode_index.hpp itself exposes, since it builds an index rather than
/// being a general-purpose decompressor.
std::vector<std::uint8_t> decompressZstd(std::span<const std::uint8_t> bytes) {
  auto *const dstream = ZSTD_createDStream();
  ZSTD_initDStream(dstream);
  std::vector<std::uint8_t> result;
  std::vector<std::uint8_t> buf(1U << 16);
  ZSTD_inBuffer in{bytes.data(), bytes.size(), 0};
  do {
    ZSTD_outBuffer out{buf.data(), buf.size(), 0};
    ZSTD_decompressStream(dstream, &out, &in);
    result.insert(result.end(), buf.begin(),
                  buf.begin() + static_cast<long>(out.pos));
  } while (in.pos < in.size);
  ZSTD_freeDStream(dstream);
  return result;
}
#endif

#ifdef GLEDITOR_HAVE_DECODE_INDEX_FLAC
/// Plain full decode via libFLAC++ directly, used only to verify
/// reencodeFlacSeekable()'s output round-trips to the original PCM -- not
/// something decode_index.hpp itself exposes.
class PlainFlacDecoder : public FLAC::Decoder::Stream {
public:
  explicit PlainFlacDecoder(std::span<const std::uint8_t> bytes)
      : bytes_(bytes) {}
  std::vector<FLAC__int32>
      pcm; // First channel only -- these fixtures are mono.

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
  ::FLAC__StreamDecoderWriteStatus
  write_callback(const ::FLAC__Frame *frame,
                 const FLAC__int32 *const buffer[]) override {
    pcm.insert(pcm.end(), buffer[0], buffer[0] + frame->header.blocksize);
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
  }
  void metadata_callback(const ::FLAC__StreamMetadata *) override {}
  void error_callback(::FLAC__StreamDecoderErrorStatus) override {}

private:
  std::span<const std::uint8_t> bytes_;
  std::size_t pos_{0};
};

std::vector<FLAC__int32> decodeFlac(std::span<const std::uint8_t> bytes) {
  PlainFlacDecoder decoder(bytes);
  decoder.init();
  decoder.process_until_end_of_stream();
  decoder.finish();
  return decoder.pcm;
}
#endif

#ifdef GLEDITOR_HAVE_DECODE_INDEX_TIFF
/// Plain in-memory TIFF decode via libtiff directly, used only to verify
/// reencodeTiffSeekable()'s output round-trips to the original pixels --
/// not something decode_index.hpp itself exposes.
struct TiffMemReader {
  std::span<const std::uint8_t> bytes;
  std::size_t pos{0};
};

tmsize_t tiffTestRead(thandle_t handle, void *buf, tmsize_t size) {
  auto *const reader          = static_cast<TiffMemReader *>(handle);
  const std::size_t remaining = reader->bytes.size() - reader->pos;
  const std::size_t toRead =
      std::min<std::size_t>(static_cast<std::size_t>(size), remaining);
  std::memcpy(buf, reader->bytes.data() + reader->pos, toRead);
  reader->pos += toRead;
  return static_cast<tmsize_t>(toRead);
}
tmsize_t tiffTestWrite(thandle_t, void *, tmsize_t) { return -1; }
toff_t tiffTestSeek(thandle_t handle, toff_t offset, int whence) {
  auto *const reader  = static_cast<TiffMemReader *>(handle);
  std::int64_t newPos = 0;
  if (SEEK_SET == whence) {
    newPos = static_cast<std::int64_t>(offset);
  } else if (SEEK_CUR == whence) {
    newPos = static_cast<std::int64_t>(reader->pos) + offset;
  } else if (SEEK_END == whence) {
    newPos = static_cast<std::int64_t>(reader->bytes.size()) + offset;
  }
  reader->pos = static_cast<std::size_t>(newPos);
  return static_cast<toff_t>(reader->pos);
}
int tiffTestClose(thandle_t) { return 0; }
toff_t tiffTestSize(thandle_t handle) {
  return static_cast<toff_t>(
      static_cast<TiffMemReader *>(handle)->bytes.size());
}

std::vector<std::uint32_t> decodeTiffRgba(std::span<const std::uint8_t> bytes,
                                          std::uint32_t &width,
                                          std::uint32_t &height) {
  TiffMemReader reader{bytes};
  TIFF *tif = TIFFClientOpen("test", "r", &reader, tiffTestRead, tiffTestWrite,
                             tiffTestSeek, tiffTestClose, tiffTestSize, nullptr,
                             nullptr);
  if (nullptr == tif) {
    return {};
  }
  TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width);
  TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height);
  std::vector<std::uint32_t> rgba(static_cast<std::size_t>(width) * height);
  const bool ok = TIFFReadRGBAImageOriented(tif, width, height, rgba.data(),
                                            ORIENTATION_TOPLEFT, 0);
  TIFFClose(tif);
  return ok ? rgba : std::vector<std::uint32_t>{};
}
#endif

TEST(DecodeIndexTest, UnrecognisedMimeTypeIsUnsupported) {
  const std::vector<std::uint8_t> bytes{'h', 'e', 'l', 'l', 'o'};
  const auto index = buildDecodeIndex(bytes, MimeType::TextPlain);
  EXPECT_EQ(index.format, DecodeIndexFormat::Unsupported);
  EXPECT_FALSE(index.seekable);
  EXPECT_FALSE(index.durableIndex);
  EXPECT_TRUE(index.seekPoints.empty());
}

TEST(DecodeIndexTest, EmptyBufferNeverThrowsAndIsNotSeekable) {
  const std::vector<std::uint8_t> empty;
  EXPECT_NO_THROW({
    const auto index = buildDecodeIndex(empty, MimeType::ImagePng);
    EXPECT_FALSE(index.seekable);
  });
}

TEST(DecodeIndexTest, PngReportsSeekableButNoDurableIndex) {
  const auto bytes = readFile("tests/samples/sample_image.png");
  ASSERT_FALSE(bytes.empty()) << "fixture missing";

  const auto index = buildDecodeIndex(bytes, MimeType::ImagePng);
  if (DecodeIndexFormat::Unsupported == index.format) {
    GTEST_SKIP()
        << "built without zlib support (GLEDITOR_HAVE_DECODE_INDEX_ZLIB)";
  }
  EXPECT_EQ(index.format, DecodeIndexFormat::Png);
  EXPECT_TRUE(index.seekable);
  // PNG's row filters are differential, so there is no portable byte
  // representation of a resume point -- see PngCheckpoints's own comment for
  // why this is a real, permanent limitation rather than a gap.
  EXPECT_FALSE(index.durableIndex);
  EXPECT_TRUE(index.seekPoints.empty());
  EXPECT_EQ(index.uncompressedExtent, 64U); // sample_image.png is 64x64.
}

TEST(PngCheckpointsTest, BuildFailsOnNonPngBytes) {
  const std::vector<std::uint8_t> notPng{'n', 'o', 't', ' ', 'a',
                                         ' ', 'p', 'n', 'g'};
  EXPECT_FALSE(PngCheckpoints::build(notPng).has_value());
}

TEST(PngCheckpointsTest, CheckpointsExistAndResumeMatchesLinearDecode) {
  const auto bytes = readFile("tests/samples/sample_image.png");
  ASSERT_FALSE(bytes.empty()) << "fixture missing";

  auto checkpoints = PngCheckpoints::build(bytes, /*rowInterval=*/8);
  if (!checkpoints.has_value()) {
    GTEST_SKIP()
        << "built without zlib support (GLEDITOR_HAVE_DECODE_INDEX_ZLIB)";
  }
  EXPECT_EQ(checkpoints->width(), 64U);
  EXPECT_EQ(checkpoints->height(), 64U);
  // Rows 0, 8, 16, ..., 56 -- eight checkpoints over a 64-row image at
  // interval 8, row 0 always included.
  EXPECT_EQ(checkpoints->checkpointCount(), 8U);

  const auto full = checkpoints->decodeRows(0, 64);
  ASSERT_TRUE(full.has_value());
  const std::size_t rowBytes = 64U * 4U; // RGBA8.
  EXPECT_EQ(full->size(), 64U * rowBytes);

  // Resuming from the checkpoint at row 32 must produce byte-identical
  // output to the matching slice of a full, linear decode -- the property
  // that makes a checkpoint a real resume point rather than a guess.
  const auto resumed = checkpoints->decodeRows(32, 64);
  ASSERT_TRUE(resumed.has_value());
  EXPECT_EQ(resumed->size(), 32U * rowBytes);
  EXPECT_TRUE(std::equal(resumed->begin(), resumed->end(),
                         full->begin() + static_cast<long>(32 * rowBytes)));
}

TEST(PngCheckpointsTest, OutOfRangeRowsReturnNullopt) {
  const auto bytes = readFile("tests/samples/sample_image.png");
  ASSERT_FALSE(bytes.empty()) << "fixture missing";

  auto checkpoints = PngCheckpoints::build(bytes);
  if (!checkpoints.has_value()) {
    GTEST_SKIP()
        << "built without zlib support (GLEDITOR_HAVE_DECODE_INDEX_ZLIB)";
  }
  EXPECT_FALSE(checkpoints->decodeRows(10, 5).has_value()); // fromRow >= toRow
  EXPECT_FALSE(checkpoints->decodeRows(0, 1000).has_value()); // toRow > height
}

TEST(DecodeIndexTest, JpegWithRestartMarkersIsSeekableAndDurable) {
  const auto bytes = readFile("tests/samples/sample_image_restart.jpg");
  ASSERT_FALSE(bytes.empty()) << "fixture missing";

  const auto index = buildDecodeIndex(bytes, MimeType::ImageJpeg);
  if (DecodeIndexFormat::Unsupported == index.format) {
    GTEST_SKIP()
        << "built without libjpeg support (GLEDITOR_HAVE_DECODE_INDEX_LIBJPEG)";
  }
  EXPECT_EQ(index.format, DecodeIndexFormat::Jpeg);
  // The fixture was generated with a restart interval specifically so this
  // is true -- restart markers reset every DC predictor to zero by the
  // format's own definition, so jpeg_skip_scanlines() needs no bespoke
  // checkpoint of its own, unlike PNG.
  EXPECT_TRUE(index.seekable);
  EXPECT_TRUE(index.durableIndex);
  EXPECT_GT(index.uncompressedExtent, 0U);
  EXPECT_TRUE(index.seekPoints.empty()); // the restart markers *are* the
                                         // index; nothing to extract here.
}

TEST(DecodeIndexTest, VideoGetsADurableContainerIndex) {
  const auto bytes = readFile("tests/samples/sample_video_seekable.mp4");
  ASSERT_FALSE(bytes.empty()) << "fixture missing";

  const auto index = buildDecodeIndex(bytes, MimeType{"video/mp4"});
  if (DecodeIndexFormat::Unsupported == index.format) {
    GTEST_SKIP()
        << "built without libav* support (GLEDITOR_HAVE_DECODE_INDEX_LIBAV)";
  }
  EXPECT_EQ(index.format, DecodeIndexFormat::Video);
  EXPECT_TRUE(index.seekable);
  // FFmpeg's MP4 demuxer builds a full AVIndexEntry table for free -- unlike
  // MP3's, confirmed empirically while scoping this component (see
  // design/decode-index-spike.md).
  EXPECT_TRUE(index.durableIndex);
  EXPECT_FALSE(index.seekPoints.empty());
  EXPECT_EQ(index.uncompressedExtent, index.seekPoints.size());
}

TEST(DecodeIndexTest, Mp3IsSeekableButBuildsNoExtractableIndex) {
  const auto bytes = readFile("tests/samples/sample_audio_seekable.mp3");
  ASSERT_FALSE(bytes.empty()) << "fixture missing";

  const auto index = buildDecodeIndex(bytes, MimeType{"audio/mpeg"});
  if (DecodeIndexFormat::Unsupported == index.format) {
    GTEST_SKIP()
        << "built without libav* support (GLEDITOR_HAVE_DECODE_INDEX_LIBAV)";
  }
  EXPECT_EQ(index.format, DecodeIndexFormat::Mp3);
  // av_seek_frame() still works via bitrate-position estimation, so this is
  // seekable -- but FFmpeg's MP3 demuxer builds no AVIndexEntry table to
  // extract, a real, distinct case from both PNG's and JPEG's.
  EXPECT_TRUE(index.seekable);
  EXPECT_FALSE(index.durableIndex);
  EXPECT_TRUE(index.seekPoints.empty());
}

TEST(DecodeIndexTest, PlainZstdIsNotSeekable) {
  const auto bytes = readFile("tests/samples/sample_text.zst");
  ASSERT_FALSE(bytes.empty()) << "fixture missing";

  const auto index = buildDecodeIndex(bytes, MimeType{"application/zstd"});
  if (DecodeIndexFormat::Unsupported == index.format) {
    GTEST_SKIP()
        << "built without libzstd support (GLEDITOR_HAVE_DECODE_INDEX_ZSTD)";
  }
  EXPECT_EQ(index.format, DecodeIndexFormat::Zstd);
  // Most zstd files in the wild are not compressed with the seekable format
  // -- this fixture deliberately is not, so the honest answer is "no
  // mechanism was used at encode time," not "no mechanism exists at all"
  // (see reencodeZstdSeekable()).
  EXPECT_FALSE(index.seekable);
  EXPECT_FALSE(index.durableIndex);
  EXPECT_TRUE(index.seekPoints.empty());
}

TEST(DecodeIndexTest, SeekableZstdReportsDurableIndexWithSeekPoints) {
  const auto bytes = readFile("tests/samples/sample_text_seekable.zst");
  ASSERT_FALSE(bytes.empty()) << "fixture missing";

  const auto index = buildDecodeIndex(bytes, MimeType{"application/zstd"});
  if (DecodeIndexFormat::Unsupported == index.format) {
    GTEST_SKIP()
        << "built without libzstd support (GLEDITOR_HAVE_DECODE_INDEX_ZSTD)";
  }
  EXPECT_EQ(index.format, DecodeIndexFormat::Zstd);
  EXPECT_TRUE(index.seekable);
  // Unlike PNG/MP3, a seekable-format zstd file's seek table gives both
  // compressed and decompressed frame offsets directly -- no extra decode
  // pass needed to fill in uncompressedPosition, unlike a plain multi-frame
  // zstd file's own frame headers (investigated and found insufficient
  // before choosing to vendor the seekable format instead).
  EXPECT_TRUE(index.durableIndex);
  ASSERT_FALSE(index.seekPoints.empty());
  EXPECT_EQ(index.uncompressedExtent,
            4606957U); // tests/samples/kjv.txt's size.

  // Every entry's offsets must strictly increase -- a basic sanity check on
  // the extracted table, not just its non-emptiness.
  for (std::size_t i = 1; i < index.seekPoints.size(); ++i) {
    EXPECT_LT(index.seekPoints[i - 1].compressedByteOffset,
              index.seekPoints[i].compressedByteOffset);
    EXPECT_LT(index.seekPoints[i - 1].uncompressedPosition,
              index.seekPoints[i].uncompressedPosition);
  }
}

TEST(DecodeIndexTest, ReencodeZstdSeekableFailsOnNonZstdBytes) {
  const std::vector<std::uint8_t> notZstd{'n', 'o', 't', ' ',
                                          'z', 's', 't', 'd'};
  EXPECT_FALSE(reencodeZstdSeekable(notZstd).has_value());
}

TEST(DecodeIndexTest, ReencodeZstdSeekableRoundTripsContentAndBecomesDurable) {
  const auto plainBytes = readFile("tests/samples/sample_text.zst");
  ASSERT_FALSE(plainBytes.empty()) << "fixture missing";

  const auto reencoded = reencodeZstdSeekable(plainBytes);
  if (!reencoded.has_value()) {
    GTEST_SKIP()
        << "built without libzstd support (GLEDITOR_HAVE_DECODE_INDEX_ZSTD)";
  }

  const auto index = buildDecodeIndex(*reencoded, MimeType{"application/zstd"});
  EXPECT_EQ(index.format, DecodeIndexFormat::Zstd);
  EXPECT_TRUE(index.seekable);
  EXPECT_TRUE(index.durableIndex);
  EXPECT_FALSE(index.seekPoints.empty());

#ifdef GLEDITOR_HAVE_DECODE_INDEX_ZSTD
  const auto originalContent = readFile("tests/samples/kjv.txt");
  ASSERT_FALSE(originalContent.empty()) << "fixture missing";
  EXPECT_EQ(decompressZstd(*reencoded), originalContent);
#endif
}

TEST(DecodeIndexTest, PlainFlacHasNoDurableIndexButIsStillSeekable) {
  const auto bytes = readFile("tests/samples/sample_audio.flac");
  ASSERT_FALSE(bytes.empty()) << "fixture missing";

  const auto index = buildDecodeIndex(bytes, MimeType{"audio/flac"});
  if (DecodeIndexFormat::Unsupported == index.format) {
    GTEST_SKIP()
        << "built without libFLAC++ support (GLEDITOR_HAVE_DECODE_INDEX_FLAC)";
  }
  EXPECT_EQ(index.format, DecodeIndexFormat::Flac);
  // This fixture was encoded with --no-seektable specifically -- libFLAC's
  // own seek_absolute() still works via frame sync codes even so (confirmed
  // empirically while scoping this, the same way MP3's bitrate-estimation
  // seeking was), it is just not a durable, extractable table.
  EXPECT_TRUE(index.seekable);
  EXPECT_FALSE(index.durableIndex);
  EXPECT_TRUE(index.seekPoints.empty());
  EXPECT_EQ(index.uncompressedExtent, 529200U); // 12s at 44100Hz mono.
}

TEST(DecodeIndexTest, SeekableFlacReportsDurableIndexWithSeekPoints) {
  const auto bytes = readFile("tests/samples/sample_audio_seekable.flac");
  ASSERT_FALSE(bytes.empty()) << "fixture missing";

  const auto index = buildDecodeIndex(bytes, MimeType{"audio/flac"});
  if (DecodeIndexFormat::Unsupported == index.format) {
    GTEST_SKIP()
        << "built without libFLAC++ support (GLEDITOR_HAVE_DECODE_INDEX_FLAC)";
  }
  EXPECT_EQ(index.format, DecodeIndexFormat::Flac);
  EXPECT_TRUE(index.seekable);
  EXPECT_TRUE(index.durableIndex);
  ASSERT_FALSE(index.seekPoints.empty());
  EXPECT_EQ(index.uncompressedExtent, 529200U);

  // Every entry's offsets must strictly increase, and the first point must
  // resolve to sample 0 at the very start of the audio (stream_offset 0
  // relative to the first frame) -- a basic sanity check on the extracted
  // table and the audio-start-offset arithmetic that makes it absolute.
  EXPECT_EQ(index.seekPoints.front().uncompressedPosition, 0U);
  for (std::size_t i = 1; i < index.seekPoints.size(); ++i) {
    EXPECT_LT(index.seekPoints[i - 1].compressedByteOffset,
              index.seekPoints[i].compressedByteOffset);
    EXPECT_LT(index.seekPoints[i - 1].uncompressedPosition,
              index.seekPoints[i].uncompressedPosition);
  }
}

TEST(DecodeIndexTest, ReencodeFlacSeekableFailsOnNonFlacBytes) {
  const std::vector<std::uint8_t> notFlac{'n', 'o', 't', ' ',
                                          'f', 'l', 'a', 'c'};
  EXPECT_FALSE(reencodeFlacSeekable(notFlac).has_value());
}

TEST(DecodeIndexTest, ReencodeFlacSeekableRoundTripsContentAndBecomesDurable) {
  const auto plainBytes = readFile("tests/samples/sample_audio.flac");
  ASSERT_FALSE(plainBytes.empty()) << "fixture missing";

  const auto reencoded = reencodeFlacSeekable(plainBytes);
  if (!reencoded.has_value()) {
    GTEST_SKIP()
        << "built without libFLAC++ support (GLEDITOR_HAVE_DECODE_INDEX_FLAC)";
  }

  const auto index = buildDecodeIndex(*reencoded, MimeType{"audio/flac"});
  EXPECT_EQ(index.format, DecodeIndexFormat::Flac);
  EXPECT_TRUE(index.seekable);
  EXPECT_TRUE(index.durableIndex);
  EXPECT_FALSE(index.seekPoints.empty());

#ifdef GLEDITOR_HAVE_DECODE_INDEX_FLAC
  EXPECT_EQ(decodeFlac(*reencoded), decodeFlac(plainBytes));
#endif
}

TEST(DecodeIndexTest, SingleStripTiffHasOnlyOneSeekPoint) {
  const auto bytes = readFile("tests/samples/sample_image.tif");
  ASSERT_FALSE(bytes.empty()) << "fixture missing";

  const auto index = buildDecodeIndex(bytes, MimeType{"image/tiff"});
  if (DecodeIndexFormat::Unsupported == index.format) {
    GTEST_SKIP()
        << "built without libtiff support (GLEDITOR_HAVE_DECODE_INDEX_TIFF)";
  }
  EXPECT_EQ(index.format, DecodeIndexFormat::Tiff);
  // This fixture was forced (via tiffcp -r) to one strip covering the whole
  // image -- strip offsets are still part of the format, so this is
  // seekable/durable in principle, but a single seek point (at row 0) is
  // not usefully seekable at all. reencodeTiffSeekable() exists for this.
  EXPECT_TRUE(index.seekable);
  EXPECT_TRUE(index.durableIndex);
  ASSERT_EQ(index.seekPoints.size(), 1U);
  EXPECT_EQ(index.seekPoints.front().uncompressedPosition, 0U);
  EXPECT_EQ(index.uncompressedExtent, 512U); // sample_image.tif is 256x512.
}

TEST(DecodeIndexTest, SeekableTiffReportsManyStripsWithSeekPoints) {
  const auto bytes = readFile("tests/samples/sample_image_seekable.tif");
  ASSERT_FALSE(bytes.empty()) << "fixture missing";

  const auto index = buildDecodeIndex(bytes, MimeType{"image/tiff"});
  if (DecodeIndexFormat::Unsupported == index.format) {
    GTEST_SKIP()
        << "built without libtiff support (GLEDITOR_HAVE_DECODE_INDEX_TIFF)";
  }
  EXPECT_EQ(index.format, DecodeIndexFormat::Tiff);
  EXPECT_TRUE(index.seekable);
  EXPECT_TRUE(index.durableIndex);
  EXPECT_EQ(index.uncompressedExtent, 512U);
  // 512 rows / 8 rows-per-strip (this function's own default) = 64 strips.
  ASSERT_EQ(index.seekPoints.size(), 64U);

  // Every entry's offsets must strictly increase, and the first point must
  // start at row 0.
  EXPECT_EQ(index.seekPoints.front().uncompressedPosition, 0U);
  for (std::size_t i = 1; i < index.seekPoints.size(); ++i) {
    EXPECT_LT(index.seekPoints[i - 1].compressedByteOffset,
              index.seekPoints[i].compressedByteOffset);
    EXPECT_LT(index.seekPoints[i - 1].uncompressedPosition,
              index.seekPoints[i].uncompressedPosition);
  }
}

TEST(DecodeIndexTest, ReencodeTiffSeekableFailsOnNonTiffBytes) {
  const std::vector<std::uint8_t> notTiff{'n', 'o', 't', ' ',
                                          't', 'i', 'f', 'f'};
  EXPECT_FALSE(reencodeTiffSeekable(notTiff).has_value());
}

TEST(DecodeIndexTest, ReencodeTiffSeekableRoundTripsContentAndBecomesDurable) {
  const auto plainBytes = readFile("tests/samples/sample_image.tif");
  ASSERT_FALSE(plainBytes.empty()) << "fixture missing";

  const auto reencoded = reencodeTiffSeekable(plainBytes);
  if (!reencoded.has_value()) {
    GTEST_SKIP()
        << "built without libtiff support (GLEDITOR_HAVE_DECODE_INDEX_TIFF)";
  }

  const auto index = buildDecodeIndex(*reencoded, MimeType{"image/tiff"});
  EXPECT_EQ(index.format, DecodeIndexFormat::Tiff);
  EXPECT_TRUE(index.seekable);
  EXPECT_TRUE(index.durableIndex);
  EXPECT_GT(index.seekPoints.size(), 1U);

#ifdef GLEDITOR_HAVE_DECODE_INDEX_TIFF
  std::uint32_t widthBefore = 0, heightBefore = 0;
  std::uint32_t widthAfter = 0, heightAfter = 0;
  const auto before = decodeTiffRgba(plainBytes, widthBefore, heightBefore);
  const auto after  = decodeTiffRgba(*reencoded, widthAfter, heightAfter);
  ASSERT_FALSE(before.empty());
  ASSERT_FALSE(after.empty());
  EXPECT_EQ(widthBefore, widthAfter);
  EXPECT_EQ(heightBefore, heightAfter);
  EXPECT_EQ(before, after);
#endif
}

} // namespace
} // namespace gleditor

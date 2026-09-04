/**
 * @file decode-index-spike.cpp
 * @brief Verify gleditor::decode_index (include/gleditor/decode_index.hpp)
 *        against real files: design/decode-index-spike.md.
 *
 * Phase 5 of the multimedia pipeline plan named the problem and deliberately
 * left it unprototyped: a byte range inside a compressed stream (PNG IDAT, an
 * MP4 sample) names no region of anything, because the bytes at that offset
 * cannot be decoded without whatever state a decoder built up getting there.
 * This file originally prototyped the mechanisms itself; PNG's checkpoint/
 * inflateCopy() logic and JPEG/video/MP3's index-metadata detection now live
 * in gleditor::decode_index (a real library component, not a duplicate) and
 * this file calls it rather than reimplementing it -- the same PASS/FAIL
 * checks as before, now proving the shipped implementation.
 *
 * Four checks, matching four genuinely different shapes the problem takes
 * (see design/decode-index-spike.md for the full reasoning):
 *
 * - pngDecodeIndexSpike(): PNG has no native random access -- its IDAT is one
 *   zlib stream, and the Up/Paeth scanline filters are differential across
 *   rows -- so gleditor::PngCheckpoints builds real zran-style checkpoints
 *   (zlib's own inflateCopy(), the documented pattern for exactly this) over
 *   a real PNG; this restarts decoding from one and diffs the result against
 *   a full decode.
 * - avDecodeIndexSpike(): audio/video seeking is already solved generically
 *   by FFmpeg across every container/codec it supports, so no restart logic
 *   is written here either -- gleditor::buildDecodeIndex() reports whether
 *   FFmpeg built a durable container index, and this still calls
 *   av_seek_frame() directly (a public FFmpeg API in its own right, not
 *   something the library wraps) to diff the decoded frame against the same
 *   frame reached by linear decode from the start.
 * - jpegDecodeIndexSpike(): JPEG's restart markers reset every DC predictor
 *   to zero by the format's own definition, so resuming needs *no* carried
 *   state at all -- gleditor::buildDecodeIndex() reports whether this file's
 *   own DRI marker declared one, and this still calls libjpeg-turbo's own
 *   jpeg_skip_scanlines() directly (again a public API in its own right) to
 *   fast-forward through them and diff against a full decode.
 *
 * WebP has no library equivalent at all -- webpDecodeIndexSpike() stays
 * exactly as it was, a self-contained investigation into why no viable
 * partial-decode mechanism exists for it (see the function's own comment).
 *
 * Deliberately not attempted here: wiring any of these mechanisms into
 * Store/ScrollSegment/PrimediaSpan. This is a standalone verification tool
 * (see the Makefile's decode-index-spike target, modelled on
 * layout-latency-probe), not part of `all` and not part of `make test`.
 */
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <webp/decode.h>

#include <gleditor/decode_index.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/rational.h>
// jpeglib.h expects <stdio.h>'s FILE/size_t already visible (its own docs
// say to include it first); <cstdio> above satisfies that on this
// toolchain, the same way the rest of this file relies on glibc also
// populating the global namespace rather than only std::.
#include <jpeglib.h>
}

namespace {

bool allPassed = true;

void report(const bool passed, const std::string &what) {
  std::cout << (passed ? "PASS: " : "FAIL: ") << what << "\n";
  allPassed = allPassed && passed;
}

// -- PNG spike ---------------------------------------------------------------

/// Both the checkpoint/inflateCopy() mechanism and the parsing it needs now
/// live in gleditor::PngCheckpoints (include/gleditor/decode_index.hpp,
/// src/decode_index.cpp) -- ported from what this function used to contain
/// itself, not reimplemented. This calls buildDecodeIndex() for the index-
/// metadata check, then PngCheckpoints for the actual restart-and-diff
/// demonstration the original spike made.
bool pngDecodeIndexSpike(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    report(false, "png spike: could not open " + path);
    return false;
  }
  const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());

  const auto index =
      gleditor::buildDecodeIndex(file, gleditor::MimeType::ImagePng);
  if (gleditor::DecodeIndexFormat::Png != index.format) {
    report(false, "png spike: buildDecodeIndex() did not recognise " + path +
                      " as PNG");
    return false;
  }
  report(index.seekable,
         "png spike: buildDecodeIndex() reports PNG as seekable");
  // PNG's row filters are differential, so there is no portable byte
  // representation of a resume point -- see PngCheckpoints's own comment.
  report(!index.durableIndex,
         "png spike: buildDecodeIndex() correctly reports PNG has no "
         "durable index (PngCheckpoints is the in-process-only companion)");

  auto checkpoints = gleditor::PngCheckpoints::build(file);
  if (!checkpoints.has_value()) {
    report(false, "png spike: PngCheckpoints::build() failed on " + path);
    return false;
  }
  const auto width  = checkpoints->width();
  const auto height = checkpoints->height();
  std::cout << "png spike: " << path << " is " << width << "x" << height
            << " RGBA8, " << checkpoints->checkpointCount() << " checkpoints\n";

  const auto full = checkpoints->decodeRows(0, height);
  if (!full.has_value()) {
    report(false, "png spike: full decodeRows() failed");
    return false;
  }

  // Same range this spike always seeked to: the back half of the image (the
  // point of a checkpoint is only interesting once a full decode would have
  // been wasteful work to get there), offset a few rows past a checkpoint
  // boundary on purpose -- landing exactly on one would prove less than
  // decoding a handful of thrown-away rows forward from an *earlier*
  // checkpoint before reaching the wanted range, which is what a real
  // partial read actually needs to do.
  const std::uint32_t from =
      height > 4 ? ((height * 3) / 4) + std::min<std::uint32_t>(3, height / 16)
                 : height - 1;
  const std::uint32_t to =
      std::min(height, from + std::max<std::uint32_t>(2, height / 8));
  std::cout << "png spike: seeking rows [" << from << "," << to << ")\n";

  const auto partial = checkpoints->decodeRows(from, to);
  if (!partial.has_value()) {
    report(false, "png spike: decodeRows(" + std::to_string(from) + "," +
                      std::to_string(to) + ") failed");
    return false;
  }

  const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
  const bool bytesMatch      = std::equal(
      partial->begin(), partial->end(),
      full->begin() +
          static_cast<long>(static_cast<std::size_t>(from) * rowBytes));
  report(bytesMatch, "png spike: rows [" + std::to_string(from) + "," +
                         std::to_string(to) +
                         ") reached via PngCheckpoints::decodeRows() are "
                         "byte-identical to a full top-to-bottom decode");
  return bytesMatch && index.seekable && !index.durableIndex;
}

// -- Audio/video spike --------------------------------------------------------

/// Frees whatever avformat_open_input()/avcodec_open2() allocated, in the
/// order they need freeing in, regardless of which step failed. Shared by
/// the video and MP3 spikes -- only which AVMEDIA_TYPE openStream() looks
/// for differs.
struct AvContext {
  AVFormatContext *fmt{nullptr};
  AVCodecContext *codec{nullptr};
  int streamIndex{-1};

  AvContext()                             = default;
  AvContext(const AvContext &)            = delete;
  AvContext &operator=(const AvContext &) = delete;
  // Move-only, and explicitly so: openStream() returns this by value into an
  // optional<AvContext>, which without a declared move constructor falls
  // back to the implicitly-generated *copy* constructor (a destructor alone
  // does not suppress that) -- bitwise-copying fmt/codec, after which the
  // original's destructor frees them and the caller is left holding
  // dangling pointers. Declaring move (and deleting copy, so this can never
  // silently regress) is what makes returning it by value actually safe.
  AvContext(AvContext &&other) noexcept
      : fmt(other.fmt), codec(other.codec), streamIndex(other.streamIndex) {
    other.fmt   = nullptr;
    other.codec = nullptr;
  }
  AvContext &operator=(AvContext &&other) noexcept {
    if (this != &other) {
      this->~AvContext();
      fmt         = other.fmt;
      codec       = other.codec;
      streamIndex = other.streamIndex;
      other.fmt   = nullptr;
      other.codec = nullptr;
    }
    return *this;
  }
  ~AvContext() {
    if (nullptr != codec) {
      avcodec_free_context(&codec);
    }
    if (nullptr != fmt) {
      avformat_close_input(&fmt);
    }
  }
};

std::optional<AvContext> openStream(const std::string &path,
                                    const AVMediaType mediaType,
                                    const char *const label) {
  AvContext ctx;
  if (avformat_open_input(&ctx.fmt, path.c_str(), nullptr, nullptr) < 0) {
    std::cerr << label << " spike: could not open " << path << "\n";
    return std::nullopt;
  }
  if (avformat_find_stream_info(ctx.fmt, nullptr) < 0) {
    std::cerr << label << " spike: could not read stream info from " << path
              << "\n";
    return std::nullopt;
  }
  for (unsigned i = 0; i < ctx.fmt->nb_streams; ++i) {
    if (mediaType == ctx.fmt->streams[i]->codecpar->codec_type) {
      ctx.streamIndex = static_cast<int>(i);
      break;
    }
  }
  if (ctx.streamIndex < 0) {
    std::cerr << label << " spike: no matching stream in " << path << "\n";
    return std::nullopt;
  }
  const auto *const params  = ctx.fmt->streams[ctx.streamIndex]->codecpar;
  const auto *const decoder = avcodec_find_decoder(params->codec_id);
  if (nullptr == decoder) {
    std::cerr << label << " spike: no decoder for codec " << params->codec_id
              << "\n";
    return std::nullopt;
  }
  ctx.codec = avcodec_alloc_context3(decoder);
  if (nullptr == ctx.codec ||
      avcodec_parameters_to_context(ctx.codec, params) < 0 ||
      avcodec_open2(ctx.codec, decoder, nullptr) < 0) {
    std::cerr << label << " spike: could not open decoder\n";
    return std::nullopt;
  }
  return ctx;
}

std::optional<AvContext> openVideo(const std::string &path) {
  return openStream(path, AVMEDIA_TYPE_VIDEO, "av");
}

std::optional<AvContext> openAudio(const std::string &path) {
  return openStream(path, AVMEDIA_TYPE_AUDIO, "mp3");
}

/// Decodes frames from wherever @p ctx's demuxer is currently positioned,
/// stopping at the first frame whose pts is >= @p targetPts -- the same
/// "decode forward to the target" step whether that position is the start
/// of the file (the linear reference decode) or wherever av_seek_frame()
/// landed (the seek spike). Returns nullopt if the target is never reached.
std::optional<std::vector<std::uint8_t>>
decodeForwardTo(AvContext &ctx, const std::int64_t targetPts) {
  AVPacket *packet = av_packet_alloc();
  AVFrame *frame   = av_frame_alloc();
  std::optional<std::vector<std::uint8_t>> result;

  while (av_read_frame(ctx.fmt, packet) >= 0) {
    if (packet->stream_index != ctx.streamIndex) {
      av_packet_unref(packet);
      continue;
    }
    if (avcodec_send_packet(ctx.codec, packet) >= 0) {
      while (avcodec_receive_frame(ctx.codec, frame) >= 0) {
        if (frame->pts >= targetPts) {
          const auto height = static_cast<std::size_t>(frame->height);
          std::vector<std::uint8_t> pixels;
          // Every plane the pixel format uses (YUV420P: Y, U, V), packed
          // tightly regardless of the decoder's own row padding
          // (linesize can exceed width) -- so two decodes that agree on
          // content but not on incidental buffer layout still compare equal.
          for (int plane = 0;
               plane < AV_NUM_DATA_POINTERS && nullptr != frame->data[plane];
               ++plane) {
            const auto planeHeight =
                (0 == plane) ? height : (height + 1) / 2; // 4:2:0 chroma
            const auto planeWidth =
                (0 == plane) ? static_cast<std::size_t>(frame->width)
                             : (static_cast<std::size_t>(frame->width) + 1) / 2;
            for (std::size_t y = 0; y < planeHeight; ++y) {
              const auto *const row =
                  frame->data[plane] +
                  (static_cast<std::size_t>(frame->linesize[plane]) * y);
              pixels.insert(pixels.end(), row, row + planeWidth);
            }
          }
          result = std::move(pixels);
          break;
        }
      }
    }
    av_packet_unref(packet);
    if (result.has_value()) {
      break;
    }
  }

  av_frame_free(&frame);
  av_packet_free(&packet);
  return result;
}

bool avDecodeIndexSpike(const std::string &path) {
  // buildDecodeIndex() reports whether FFmpeg built a durable seek index for
  // this container -- av_seek_frame()/decodeForwardTo() below still call
  // FFmpeg directly to demonstrate the actual seek-and-decode, since
  // decode_index.hpp reports index metadata only, not a decode helper (see
  // this file's own top comment).
  std::ifstream indexIn(path, std::ios::binary);
  if (!indexIn) {
    report(false,
           "av spike: could not open " + path + " for buildDecodeIndex()");
    return false;
  }
  const std::vector<std::uint8_t> indexBytes(
      (std::istreambuf_iterator<char>(indexIn)),
      std::istreambuf_iterator<char>());
  const auto index =
      gleditor::buildDecodeIndex(indexBytes, gleditor::MimeType{"video/mp4"});
  if (gleditor::DecodeIndexFormat::Video != index.format) {
    report(false, "av spike: buildDecodeIndex() did not recognise " + path +
                      " as video");
    return false;
  }
  report(index.seekable,
         "av spike: buildDecodeIndex() reports video as seekable");
  // FFmpeg's MP4 demuxer builds a full AVIndexEntry table for free -- unlike
  // MP3's, confirmed empirically while scoping this component.
  report(index.durableIndex,
         "av spike: buildDecodeIndex() reports a durable container index");
  report(!index.seekPoints.empty(),
         "av spike: buildDecodeIndex() extracted " +
             std::to_string(index.seekPoints.size()) +
             " AVIndexEntry seek points");

  // Canonical uncompressed coordinate for video: frame number at the
  // stream's own native rate. Frame 15 of a 10fps clip is t=1.5s, reached
  // by decoding forward from the keyframe at t=1.0s -- not the file's first
  // frame -- which is what makes this spike actually exercise a seek rather
  // than merely a longer linear decode.
  constexpr int targetFrameIndex = 15;
  // Frame *duration* (seconds per tick), not frame rate: av_rescale_q()
  // rescales a value from one timebase into another, and treating
  // targetFrameIndex as a pts already expressed in "1 tick = 1/10 second"
  // units is what turns a frame count into the stream's own time_base
  // directly, with no separate inversion step.
  constexpr AVRational frameDuration{1, 10};

  auto linearCtx = openVideo(path);
  if (!linearCtx.has_value()) {
    report(false, "av spike: could not open " + path + " for linear decode");
    return false;
  }
  const auto *const stream = linearCtx->fmt->streams[linearCtx->streamIndex];
  const auto targetPts =
      av_rescale_q(targetFrameIndex, frameDuration, stream->time_base);

  const auto linearFrame = decodeForwardTo(*linearCtx, targetPts);
  if (!linearFrame.has_value()) {
    report(false, "av spike: linear decode never reached frame " +
                      std::to_string(targetFrameIndex));
    return false;
  }

  auto seekCtx = openVideo(path);
  if (!seekCtx.has_value()) {
    report(false, "av spike: could not open " + path + " for seek decode");
    return false;
  }
  if (av_seek_frame(seekCtx->fmt, seekCtx->streamIndex, targetPts,
                    AVSEEK_FLAG_BACKWARD) < 0) {
    report(false, "av spike: av_seek_frame failed");
    return false;
  }
  avcodec_flush_buffers(seekCtx->codec);
  const auto seekFrame = decodeForwardTo(*seekCtx, targetPts);
  if (!seekFrame.has_value()) {
    report(false, "av spike: seek decode never reached frame after seeking");
    return false;
  }

  std::cout << "av spike: " << path << " -- target frame " << targetFrameIndex
            << " (pts " << targetPts << "), " << linearFrame->size()
            << " bytes/frame\n";
  const bool matches = *linearFrame == *seekFrame;
  report(matches,
         "av spike: frame " + std::to_string(targetFrameIndex) +
             " reached via av_seek_frame() to the nearest keyframe is "
             "pixel-identical to the same frame reached by linear decode "
             "from the start");
  return matches && index.seekable && index.durableIndex &&
         !index.seekPoints.empty();
}

// -- MP3 spike
// -----------------------------------------------------------------

/// Decodes forward from wherever @p ctx's demuxer is positioned, discarding
/// the first @p framesToSkip decoded frames entirely, then concatenating the
/// PCM samples (mono float32, matching the fixture) of the next
/// @p framesToCollect frames. One function for both paths in
/// mp3DecodeIndexSpike(): the linear path skips forward to the same nominal
/// position the seek path lands on, so the two are directly comparable.
std::optional<std::vector<float>> decodeAudioFrames(AvContext &ctx,
                                                    const int framesToSkip,
                                                    const int framesToCollect) {
  AVPacket *packet = av_packet_alloc();
  AVFrame *frame   = av_frame_alloc();
  std::vector<float> result;
  int frameCount = 0;
  bool done      = false;

  while (!done && av_read_frame(ctx.fmt, packet) >= 0) {
    if (packet->stream_index == ctx.streamIndex &&
        avcodec_send_packet(ctx.codec, packet) >= 0) {
      while (avcodec_receive_frame(ctx.codec, frame) >= 0) {
        if (frameCount >= framesToSkip) {
          const auto *const samples =
              reinterpret_cast<const float *>(frame->data[0]);
          result.insert(result.end(), samples, samples + frame->nb_samples);
          if (frameCount >= framesToSkip + framesToCollect - 1) {
            done = true;
          }
        }
        ++frameCount;
        if (done) {
          break;
        }
      }
    }
    av_packet_unref(packet);
  }

  av_frame_free(&frame);
  av_packet_free(&packet);
  if (!done) {
    return std::nullopt;
  }
  return result;
}

bool mp3DecodeIndexSpike(const std::string &path) {
  // buildDecodeIndex() reports the honest MP3 answer: seekable (FFmpeg's
  // own bitrate-position estimation works) but no extractable index, unlike
  // MP4 above -- confirmed empirically while scoping this component, not
  // assumed. av_seek_frame() below still calls FFmpeg directly for the same
  // reason avDecodeIndexSpike() does.
  std::ifstream indexIn(path, std::ios::binary);
  if (!indexIn) {
    report(false,
           "mp3 spike: could not open " + path + " for buildDecodeIndex()");
    return false;
  }
  const std::vector<std::uint8_t> indexBytes(
      (std::istreambuf_iterator<char>(indexIn)),
      std::istreambuf_iterator<char>());
  const auto index =
      gleditor::buildDecodeIndex(indexBytes, gleditor::MimeType{"audio/mpeg"});
  if (gleditor::DecodeIndexFormat::Mp3 != index.format) {
    report(false, "mp3 spike: buildDecodeIndex() did not recognise " + path +
                      " as MP3");
    return false;
  }
  report(index.seekable,
         "mp3 spike: buildDecodeIndex() reports MP3 as seekable");
  report(!index.durableIndex,
         "mp3 spike: buildDecodeIndex() correctly reports MP3 has no "
         "extractable index (FFmpeg's own MP3 demuxer builds none)");
  report(index.seekPoints.empty(),
         "mp3 spike: buildDecodeIndex() leaves seekPoints empty for MP3");

  // Canonical uncompressed coordinate for audio: PCM sample-frame index at
  // the stream's own native rate, same as the design doc's general audio
  // convention. Translated to an MP3 *frame* index via 1152 samples/frame,
  // exact for this fixture (MPEG-1 Layer III, fixed 128kbit/s CBR at
  // 44.1kHz) -- a VBR encode can vary frame size with the bitrate, which
  // this spike does not attempt to handle.
  constexpr int targetFrameIndex   = 85;
  constexpr int samplesPerMp3Frame = 1152;

  auto linearCtx = openAudio(path);
  if (!linearCtx.has_value()) {
    report(false, "mp3 spike: could not open " + path + " for linear decode");
    return false;
  }
  const auto *const stream = linearCtx->fmt->streams[linearCtx->streamIndex];
  const auto targetPts     = av_rescale_q(
      targetFrameIndex * samplesPerMp3Frame,
      AVRational{1, linearCtx->codec->sample_rate}, stream->time_base);

  // Found empirically, not assumed: seeking mid-stream and decoding
  // immediately gives silence for the first two frames and a transitional
  // (neither-silent-nor-correct) third, before output becomes bit-exact --
  // libmp3lame/mp3float's own decoder priming delay. Confirmed by printing
  // frame-by-frame PCM from both a linear and a seeked decode side by side
  // while investigating; 3 is what this fixture and decoder need, not a
  // number this spike derives from anything -- a real integration would
  // need this from the codec/format rather than a hardcoded constant.
  constexpr int warmupFrames  = 3;
  constexpr int compareFrames = 5;

  const auto linearSamples = decodeAudioFrames(
      *linearCtx, targetFrameIndex + warmupFrames, compareFrames);
  if (!linearSamples.has_value()) {
    report(false, "mp3 spike: linear decode never reached frame " +
                      std::to_string(targetFrameIndex + warmupFrames));
    return false;
  }

  auto seekCtx = openAudio(path);
  if (!seekCtx.has_value()) {
    report(false, "mp3 spike: could not open " + path + " for seek decode");
    return false;
  }
  if (av_seek_frame(seekCtx->fmt, seekCtx->streamIndex, targetPts,
                    AVSEEK_FLAG_BACKWARD) < 0) {
    report(false, "mp3 spike: av_seek_frame failed");
    return false;
  }
  avcodec_flush_buffers(seekCtx->codec);
  const auto seekSamples =
      decodeAudioFrames(*seekCtx, warmupFrames, compareFrames);
  if (!seekSamples.has_value()) {
    report(false, "mp3 spike: seek decode never reached frame after seeking");
    return false;
  }

  std::cout << "mp3 spike: " << path << " -- target frame " << targetFrameIndex
            << " (pts " << targetPts << "), " << warmupFrames
            << " warm-up frames discarded after seeking, "
            << linearSamples->size() << " samples compared\n";
  const bool matches = *linearSamples == *seekSamples;
  report(matches,
         "mp3 spike: audio reached via av_seek_frame() plus " +
             std::to_string(warmupFrames) +
             " discarded warm-up frames is sample-identical to the same "
             "position reached by linear decode from the start");
  return matches && index.seekable && !index.durableIndex &&
         index.seekPoints.empty();
}

// -- JPEG spike
// ----------------------------------------------------------------

/// Decodes rows [@p skipRows, @p skipRows + @p wantRows) of a JPEG at @p path
/// via libjpeg-turbo's own jpeg_skip_scanlines() -- a public API added
/// specifically for this ("Might be useful... to randomly access one or
/// more scanlines"), not something this spike wrote itself. Internally it
/// fast-forwards whole MCU rows using the file's own restart markers when
/// present, falling back to an ordinary (slower, but still correct) decode
/// of the skipped rows when they are not -- either way the *caller* writes
/// no format-specific restart logic at all. @p skipRows == 0 decodes a full
/// image and doubles as the "linear decode" side of the comparison.
std::vector<std::uint8_t> decodeJpegRows(const std::string &path,
                                         const std::uint32_t skipRows,
                                         const std::uint32_t wantRows,
                                         int &width) {
  std::FILE *const file = std::fopen(path.c_str(), "rb");
  if (nullptr == file) {
    throw std::runtime_error("could not open " + path);
  }
  jpeg_decompress_struct cinfo{};
  jpeg_error_mgr jerr{};
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_decompress(&cinfo);
  jpeg_stdio_src(&cinfo, file);
  jpeg_read_header(&cinfo, TRUE);
  jpeg_start_decompress(&cinfo);
  width = static_cast<int>(cinfo.output_width);
  const std::size_t stride =
      static_cast<std::size_t>(cinfo.output_width) * cinfo.output_components;

  if (skipRows > 0) {
    const auto actuallySkipped = jpeg_skip_scanlines(&cinfo, skipRows);
    if (actuallySkipped != skipRows) {
      jpeg_destroy_decompress(&cinfo);
      std::fclose(file);
      throw std::runtime_error("jpeg_skip_scanlines skipped " +
                               std::to_string(actuallySkipped) + " of " +
                               std::to_string(skipRows) + " requested rows");
    }
  }

  std::vector<std::uint8_t> rows(stride * wantRows);
  std::uint32_t got = 0;
  while (got < wantRows && cinfo.output_scanline < cinfo.output_height) {
    auto *rowPtr = rows.data() + (static_cast<std::size_t>(got) * stride);
    jpeg_read_scanlines(&cinfo, &rowPtr, 1);
    ++got;
  }
  // libjpeg expects either every remaining scanline read or explicitly
  // skipped before finishing -- jpeg_skip_scanlines() again, rather than
  // this spike deciding for itself what "abandon the rest of the image"
  // should look like.
  if (cinfo.output_scanline < cinfo.output_height) {
    jpeg_skip_scanlines(&cinfo, cinfo.output_height - cinfo.output_scanline);
  }
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  std::fclose(file);

  if (got != wantRows) {
    throw std::runtime_error("decoded " + std::to_string(got) + " of " +
                             std::to_string(wantRows) + " wanted rows");
  }
  return rows;
}

bool jpegDecodeIndexSpike(const std::string &path) {
  // buildDecodeIndex() reports whether libjpeg-turbo's own
  // cinfo.restart_interval says this file has restart markers at all --
  // decodeJpegRows() below still calls jpeg_skip_scanlines() directly to
  // demonstrate fast-forwarding through them, since that is a public
  // libjpeg-turbo API in its own right, not something decode_index.hpp
  // wraps (see this file's own top comment for why).
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    report(false, "jpeg spike: could not open " + path);
    return false;
  }
  const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
  const auto index =
      gleditor::buildDecodeIndex(file, gleditor::MimeType::ImageJpeg);
  if (gleditor::DecodeIndexFormat::Jpeg != index.format) {
    report(false, "jpeg spike: buildDecodeIndex() did not recognise " + path +
                      " as JPEG");
    return false;
  }
  report(index.seekable,
         "jpeg spike: buildDecodeIndex() reports this restart-marker JPEG "
         "as seekable");
  report(index.durableIndex,
         "jpeg spike: buildDecodeIndex() reports JPEG's own restart markers "
         "as a durable index (nothing to extract into seekPoints)");
  report(index.seekPoints.empty(),
         "jpeg spike: buildDecodeIndex() leaves seekPoints empty for JPEG "
         "-- the restart markers already in the file *are* the index");

  // Canonical uncompressed coordinate for this spike: row index into the
  // decoder's own output raster (RGB8, no subsampling in this fixture) --
  // deliberately the same "row of an image" coordinate the PNG spike uses,
  // even though nothing here needs to reconcile the two: a real
  // integration addressing both PNG and JPEG spans would.
  constexpr std::uint32_t targetRow = 88;
  constexpr std::uint32_t wantRows  = 8;

  int fullWidth   = 0;
  const auto full = decodeJpegRows(path, 0, targetRow + wantRows, fullWidth);

  int partialWidth   = 0;
  const auto partial = decodeJpegRows(path, targetRow, wantRows, partialWidth);

  if (fullWidth != partialWidth) {
    report(false, "jpeg spike: width mismatch between full and skip decode");
    return false;
  }
  std::cout << "jpeg spike: " << path << " is " << fullWidth
            << "px wide, comparing rows [" << targetRow << ","
            << (targetRow + wantRows) << ")\n";

  const std::size_t stride = static_cast<std::size_t>(fullWidth) * 3;
  const auto *const fullTarget =
      full.data() + (static_cast<std::size_t>(targetRow) * stride);
  const bool matches =
      0 == std::memcmp(fullTarget, partial.data(), stride * wantRows);
  report(matches,
         "jpeg spike: rows [" + std::to_string(targetRow) + "," +
             std::to_string(targetRow + wantRows) +
             ") reached via jpeg_skip_scanlines() (fast-forwarding via this "
             "file's own restart markers) are byte-identical to the same "
             "rows from a full top-to-bottom decode");
  return matches && index.seekable && index.durableIndex &&
         index.seekPoints.empty();
}

// -- WebP spike
// ----------------------------------------------------------------

/// Wall-clock time to decode @p path, optionally cropped to
/// [@p cropTop, @p cropTop + @p cropHeight) of rows -- @p cropHeight == 0
/// requests a full decode. Returns nullopt on decode failure.
std::optional<std::pair<std::vector<std::uint8_t>, double>>
decodeWebp(const std::vector<std::uint8_t> &fileBytes, const int cropTop,
           const int cropHeight) {
  WebPDecoderConfig config;
  if (!WebPInitDecoderConfig(&config)) {
    return std::nullopt;
  }
  // WebPInitDecoderConfig() zero-initialises the whole struct, and
  // MODE_RGB (3 bytes/pixel) is enum value 0 -- so leaving colorspace
  // unset silently means 3, not the 4-bytes/pixel RGBA a reader would
  // otherwise assume. Set explicitly so the stride math below is obviously
  // correct rather than dependent on an enum's numeric value.
  config.output.colorspace = MODE_RGBA;
  if (VP8_STATUS_OK !=
      WebPGetFeatures(fileBytes.data(), fileBytes.size(), &config.input)) {
    return std::nullopt;
  }
  if (cropHeight > 0) {
    config.options.use_cropping = 1;
    config.options.crop_left    = 0;
    config.options.crop_top     = cropTop;
    config.options.crop_width   = config.input.width;
    config.options.crop_height  = cropHeight;
  }

  const auto start  = std::chrono::steady_clock::now();
  const auto status = WebPDecode(fileBytes.data(), fileBytes.size(), &config);
  const auto elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  if (VP8_STATUS_OK != status) {
    WebPFreeDecBuffer(&config.output);
    return std::nullopt;
  }

  const auto *const rgba = config.output.u.RGBA.rgba;
  const std::size_t bytes =
      static_cast<std::size_t>(config.output.width) * config.output.height * 4;
  std::vector<std::uint8_t> pixels(rgba, rgba + bytes);
  WebPFreeDecBuffer(&config.output);
  return std::make_pair(std::move(pixels), elapsed);
}

bool webpDecodeIndexSpike(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    report(false, "webp spike: could not open " + path);
    return false;
  }
  const std::vector<std::uint8_t> fileBytes(
      (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  int width  = 0;
  int height = 0;
  if (!WebPGetInfo(fileBytes.data(), fileBytes.size(), &width, &height)) {
    report(false, "webp spike: " + path + " is not a valid WebP file");
    return false;
  }
  std::cout << "webp spike: " << path << " is " << width << "x" << height
            << "\n";

  const auto full = decodeWebp(fileBytes, 0, 0);
  if (!full.has_value()) {
    report(false, "webp spike: full decode failed");
    return false;
  }
  constexpr int cropRows = 8;
  const int cropTop      = std::max(0, height - cropRows);
  const auto cropped     = decodeWebp(fileBytes, cropTop, cropRows);
  if (!cropped.has_value()) {
    report(false, "webp spike: cropped decode failed");
    return false;
  }

  // Correctness: libwebp's own cropping option is a real, supported public
  // API (WebPDecoderConfig::options.use_cropping), so -- unlike PNG or
  // JPEG -- no bespoke restart/checkpoint logic was written to get a
  // correct cropped region out of a lossless WebP. This holds specifically
  // for *lossless* WebP; investigating a lossy (VP8) fixture the same way
  // during this spike's own development found the cropped region was
  // close to but not byte-identical with the corresponding rows of a full
  // decode (small differences at the crop boundary, most likely from
  // boundary-dependent loop-filtering or chroma upsampling context that
  // differs between "this row is the image's real edge" and "this row is
  // merely where cropping happened to stop") -- not committed as a
  // fixture or assertion here, since it would not be a stable, meaningful
  // check; see design/decode-index-spike.md.
  const std::size_t stride = static_cast<std::size_t>(width) * 4;
  const auto *const fullTarget =
      full->first.data() + (static_cast<std::size_t>(cropTop) * stride);
  const bool matches =
      0 == std::memcmp(fullTarget, cropped->first.data(), stride * cropRows);
  report(matches,
         "webp spike: rows [" + std::to_string(cropTop) + "," +
             std::to_string(cropTop + cropRows) +
             ") reached via lossless WebP's own use_cropping option are "
             "byte-identical to the same rows from a full decode");

  // Informational, not gating -- and deliberately not the basis for the
  // "no savings" conclusion below, because at this fixture's tiny scale
  // (166 bytes on disk) both decodes are sub-millisecond and dominated by
  // fixed per-call overhead (allocation, VP8LDecoder setup), not by actual
  // pixel work; the ratio printed here can and does vary run to run,
  // sometimes favouring the crop, which would be easy to misread as real
  // savings if this were the only evidence. It is not: investigating this
  // spike used a separate, uncommitted 2048x2048 fixture large enough for
  // decode time to actually dominate, where full and cropped decode came
  // out statistically indistinguishable (~0.21s vs ~0.20s for an 8-row
  // crop). WebP (both variants) decodes the entire image internally
  // regardless of what region the caller asked for; there is no public API
  // analogous to zlib's inflateCopy() or JPEG's restart markers to build a
  // real checkpoint on top of, so the honest conclusion for this format is
  // "no library-provided, or otherwise practical, partial-decode fast
  // path" -- whole-media transclusion only, exactly the fallback the
  // plan's own Phase 5 text named for a format where the index proves too
  // costly.
  std::cout << "webp spike: full decode " << full->second * 1000.0
            << " ms, cropped decode " << cropped->second * 1000.0
            << " ms (too small to measure real savings either way -- see "
               "this spike's own comment)\n";

  return matches;
}

} // namespace

int main() {
  const bool pngOk = pngDecodeIndexSpike("tests/samples/sample_image.png");
  const bool avOk =
      avDecodeIndexSpike("tests/samples/sample_video_seekable.mp4");
  const bool jpegOk =
      jpegDecodeIndexSpike("tests/samples/sample_image_restart.jpg");
  const bool mp3Ok =
      mp3DecodeIndexSpike("tests/samples/sample_audio_seekable.mp3");
  const bool webpOk = webpDecodeIndexSpike("tests/samples/sample_image.webp");

  std::cout << "\n"
            << (allPassed ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED") << "\n";
  return (pngOk && avOk && jpegOk && mp3Ok && webpOk && allPassed) ? 0 : 1;
}

/**
 * @file gif_decoder.cpp
 * @brief Implementation of GifDecoder via giflib.
 */
#include <gleditor/gif_decoder.hpp>

#include <algorithm>
#include <cstring>

#include <gleditor/decode_index.hpp>

#ifdef GLEDITOR_HAVE_DECODE_INDEX_GIF
#include <gif_lib.h>
#endif

namespace gleditor {

const GifFrame GifDecoder::emptyFrame_{};

bool GifDecoder::isAnimated(const std::span<const std::uint8_t> bytes) {
  return isAnimatedGif(bytes);
}

std::optional<std::pair<int, int>>
GifDecoder::peekSize(const std::span<const std::uint8_t> bytes) {
  const auto sz = peekGifSize(bytes);
  if (!sz.has_value()) {
    return std::nullopt;
  }
  return std::make_pair(static_cast<int>(sz->first),
                        static_cast<int>(sz->second));
}

#ifdef GLEDITOR_HAVE_DECODE_INDEX_GIF

namespace {

struct MemoryReader {
  const std::uint8_t *data;
  std::size_t size;
  std::size_t offset;
};

int gifMemoryRead(GifFileType *gif, GifByteType *buf, int len) {
  auto *src = static_cast<MemoryReader *>(gif->UserData);
  if (src->offset >= src->size) {
    return 0;
  }
  const auto toRead =
      std::min(static_cast<std::size_t>(len), src->size - src->offset);
  std::memcpy(buf, src->data + src->offset, toRead);
  src->offset += toRead;
  return static_cast<int>(toRead);
}

} // namespace

std::unique_ptr<GifDecoder>
GifDecoder::decode(const std::span<const std::uint8_t> bytes) {
  if (bytes.size() < 13) {
    return nullptr;
  }

  MemoryReader reader{bytes.data(), bytes.size(), 0};
  int err                = 0;
  GifFileType *const gif = DGifOpen(&reader, gifMemoryRead, &err);
  if (nullptr == gif) {
    return nullptr;
  }

  if (DGifSlurp(gif) != GIF_OK || gif->ImageCount <= 0 || gif->SWidth <= 0 ||
      gif->SHeight <= 0) {
    DGifCloseFile(gif, &err);
    return nullptr;
  }

  auto decoder          = std::unique_ptr<GifDecoder>(new GifDecoder());
  decoder->width_       = gif->SWidth;
  decoder->height_      = gif->SHeight;
  const auto canvasSize = static_cast<std::size_t>(decoder->width_) *
                          static_cast<std::size_t>(decoder->height_);

  // Working canvas state in little-endian RGBA32
  std::vector<std::uint32_t> canvas(canvasSize, 0U);
  std::vector<std::uint32_t> previousCanvas(canvasSize, 0U);

  float currentTimestamp = 0.0F;

  for (int i = 0; i < gif->ImageCount; ++i) {
    const auto &img = gif->SavedImages[i];
    GraphicsControlBlock gcb{};
    DGifSavedExtensionToGCB(gif, i, &gcb);

    const float frameDuration =
        (gcb.DelayTime > 0) ? (static_cast<float>(gcb.DelayTime) * 0.01F)
                            : 0.10F;

    const ColorMapObject *const colorMap = (img.ImageDesc.ColorMap != nullptr)
                                               ? img.ImageDesc.ColorMap
                                               : gif->SColorMap;

    // Snapshot canvas before rendering this frame for DISPOSE_PREVIOUS
    if (gcb.DisposalMode == DISPOSE_PREVIOUS) {
      previousCanvas = canvas;
    }

    const int left = std::clamp(img.ImageDesc.Left, 0, decoder->width_);
    const int top  = std::clamp(img.ImageDesc.Top, 0, decoder->height_);
    const int w    = std::min(img.ImageDesc.Width, decoder->width_ - left);
    const int h    = std::min(img.ImageDesc.Height, decoder->height_ - top);

    if (colorMap != nullptr && img.RasterBits != nullptr && w > 0 && h > 0) {
      for (int y = 0; y < h; ++y) {
        const auto srcRow = static_cast<std::size_t>(y) * img.ImageDesc.Width;
        const auto dstRow =
            static_cast<std::size_t>(top + y) * decoder->width_ + left;
        for (int x = 0; x < w; ++x) {
          const auto colorIndex = static_cast<int>(img.RasterBits[srcRow + x]);
          if (colorIndex != gcb.TransparentColor &&
              colorIndex < colorMap->ColorCount) {
            const auto &c = colorMap->Colors[colorIndex];
            // Little-endian RGBA32: Byte 0=R, 1=G, 2=B, 3=A
            canvas[dstRow + x] = 0xFF000000U |
                                 (static_cast<std::uint32_t>(c.Blue) << 16U) |
                                 (static_cast<std::uint32_t>(c.Green) << 8U) |
                                 static_cast<std::uint32_t>(c.Red);
          }
        }
      }
    }

    GifFrame frame;
    frame.rgba             = canvas;
    frame.durationSeconds  = frameDuration;
    frame.timestampSeconds = currentTimestamp;
    currentTimestamp += frameDuration;
    decoder->frames_.push_back(std::move(frame));

    // Apply disposal mode for subsequent frame rendering
    if (gcb.DisposalMode == DISPOSE_BACKGROUND) {
      for (int y = 0; y < h; ++y) {
        const auto dstRow =
            static_cast<std::size_t>(top + y) * decoder->width_ + left;
        std::fill_n(canvas.begin() + dstRow, w, 0U);
      }
    } else if (gcb.DisposalMode == DISPOSE_PREVIOUS) {
      canvas = previousCanvas;
    }
    // DISPOSE_DO_NOT and DISPOSAL_UNSPECIFIED: canvas stays as rendered
  }

  decoder->totalDuration_ = currentTimestamp;
  DGifCloseFile(gif, &err);
  return decoder;
}

#else // !GLEDITOR_HAVE_DECODE_INDEX_GIF

std::unique_ptr<GifDecoder>
GifDecoder::decode(std::span<const std::uint8_t> /*bytes*/) {
  return nullptr;
}

#endif // GLEDITOR_HAVE_DECODE_INDEX_GIF

const GifFrame &GifDecoder::frameAt(const float seconds) const noexcept {
  if (frames_.empty()) {
    return emptyFrame_;
  }
  if (seconds <= 0.0F) {
    return frames_.front();
  }
  if (seconds >= totalDuration_) {
    return frames_.back();
  }

  // Linear scan or binary search through frame timestamps
  for (std::size_t i = 0; i < frames_.size(); ++i) {
    if (seconds < frames_[i].timestampSeconds + frames_[i].durationSeconds) {
      return frames_[i];
    }
  }
  return frames_.back();
}

const GifFrame &GifDecoder::frame(const std::size_t index) const noexcept {
  if (index < frames_.size()) {
    return frames_[index];
  }
  return emptyFrame_;
}

} // namespace gleditor

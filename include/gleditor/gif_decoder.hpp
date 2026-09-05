/**
 * @file gif_decoder.hpp
 * @brief Memory-buffered animated GIF decoding and frame compositing via
 * giflib.
 */
#ifndef GLEDITOR_GIF_DECODER_HPP
#define GLEDITOR_GIF_DECODER_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace gleditor {

/**
 * @struct GifFrame
 * @brief A single decoded and composited RGBA frame of an animated GIF.
 */
struct GifFrame {
  std::vector<std::uint32_t> rgba;
  float durationSeconds{0.1F};
  float timestampSeconds{0.0F};
};

/**
 * @class GifDecoder
 * @brief Decodes an animated GIF stream into composited RGBA frames with
 * timing.
 */
class GifDecoder {
public:
  ~GifDecoder() = default;

  GifDecoder(const GifDecoder &)            = delete;
  GifDecoder &operator=(const GifDecoder &) = delete;
  GifDecoder(GifDecoder &&)                 = default;
  GifDecoder &operator=(GifDecoder &&)      = default;

  /**
   * @brief Decode an in-memory GIF buffer.
   * @return Decoded GifDecoder instance, or nullptr on parse/decode failure.
   */
  [[nodiscard]] static std::unique_ptr<GifDecoder>
  decode(std::span<const std::uint8_t> bytes);

  /**
   * @brief Check whether @p bytes is an animated (multi-frame) GIF.
   */
  [[nodiscard]] static bool isAnimated(std::span<const std::uint8_t> bytes);

  /**
   * @brief Quick header peek for canvas width and height.
   */
  [[nodiscard]] static std::optional<std::pair<int, int>>
  peekSize(std::span<const std::uint8_t> bytes);

  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int height() const noexcept { return height_; }
  [[nodiscard]] float duration() const noexcept { return totalDuration_; }
  [[nodiscard]] std::size_t frameCount() const noexcept {
    return frames_.size();
  }

  /**
   * @brief Get the frame active at time @p seconds.
   */
  [[nodiscard]] const GifFrame &frameAt(float seconds) const noexcept;

  /**
   * @brief Direct indexed frame access.
   */
  [[nodiscard]] const GifFrame &frame(std::size_t index) const noexcept;

private:
  GifDecoder() = default;

  int width_{0};
  int height_{0};
  float totalDuration_{0.0F};
  std::vector<GifFrame> frames_;
  static const GifFrame emptyFrame_;
};

} // namespace gleditor

#endif // GLEDITOR_GIF_DECODER_HPP

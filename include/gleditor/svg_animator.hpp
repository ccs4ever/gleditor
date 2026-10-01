#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

#include <gleditor/decode_error.hpp>

namespace gleditor {

/**
 * @brief Playback engine for animated SVGs (SMIL) and vector animations
 * (ThorVG).
 *
 * Provides frame rasterization at arbitrary timestamps, dimension querying,
 * and duration extraction.
 */
class SvgAnimator {
public:
  virtual ~SvgAnimator() = default;

  /**
   * @brief Check if @p bytes represent an animatable vector/SVG stream.
   */
  [[nodiscard]] static bool isAnimated(std::span<const std::uint8_t> bytes);

  /**
   * @brief Parse and create an SvgAnimator instance from @p bytes.
   *
   * @return the animator, or why there is none: not a valid document, one
   *         with nothing to animate, or a build without ThorVG.
   */
  [[nodiscard]] static std::expected<std::unique_ptr<SvgAnimator>, DecodeError>
  load(std::span<const std::uint8_t> bytes);

  [[nodiscard]] virtual int width() const      = 0;
  [[nodiscard]] virtual int height() const     = 0;
  [[nodiscard]] virtual float duration() const = 0;

  /**
   * @brief Rasterize the frame at @p seconds into @p rgbaOut (ABGR8888S /
   * RGBA32).
   *
   * @param seconds Playback time offset in seconds.
   * @param rgbaOut Destination pixel buffer, resized to width * height
   * uint32_t.
   * @return True on success, false on error.
   */
  virtual bool renderFrame(float seconds,
                           std::vector<std::uint32_t> &rgbaOut) = 0;
};

} // namespace gleditor

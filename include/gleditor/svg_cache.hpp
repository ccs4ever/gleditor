/**
 * @file svg_cache.hpp
 * @brief Static SVG decoding via ThorVG and GPU texture caching.
 */
#ifndef GLEDITOR_SVG_CACHE_HPP
#define GLEDITOR_SVG_CACHE_HPP

#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>

#include <gleditor/image_cache.hpp>
#include <gleditor/render/device.hpp>

namespace gleditor {

/**
 * @class SvgCache
 * @brief Rasterizes SVG documents through ThorVG and uploads the result to a
 *        dedicated GPU texture, producing the same ImageResource type raster
 *        images already use.
 *
 * Unlike ImageCache, each resource gets its own texture array (one layer,
 * sized to the SVG's own intrinsic pixel size) rather than a shared
 * shelf-packed atlas -- see design/decode-index-spike.md's SVG addendum for
 * why: an OpenGL/OpenGL ES device renders directly into that texture via
 * ThorVG's GlCanvas, which needs a whole framebuffer attachment to itself,
 * not an arbitrary sub-rectangle of a larger shared layer. A Vulkan device
 * (or a GL device where the fast path fails for any reason) falls back to
 * rasterizing on the CPU via ThorVG's SwCanvas and uploading the bytes the
 * same way MediaWidget already uploads video frames.
 *
 * Only static SVG: no `tvg::Animation` support here (see the class's own
 * follow-up phase). A malformed SVG, or one whose intrinsic size cannot be
 * determined, is reported as std::nullopt rather than a zero-sized resource,
 * the same failure convention decodeImageBuffer() and DecodedImage::valid()
 * already use.
 */
class SvgCache {
public:
  explicit SvgCache(render::RenderDevice *device);
  ~SvgCache();

  SvgCache(const SvgCache &)            = delete;
  SvgCache &operator=(const SvgCache &) = delete;
  SvgCache(SvgCache &&)                 = delete;
  SvgCache &operator=(SvgCache &&)      = delete;

  /**
   * @brief Ensure an SVG is rasterized and uploaded to its own GPU texture.
   * @param id Unique identifier (path, URI, or hash) -- a repeat call with an
   *        id already cached returns the cached resource rather than
   *        rasterizing again, so a transcluded SVG referenced from several
   *        spans is only ever rasterized once.
   * @param bytes The SVG document's own bytes (XML text).
   */
  std::optional<ImageResource> loadBuffer(const std::string &id,
                                          std::span<const std::uint8_t> bytes);

  /// Find an already cached resource.
  [[nodiscard]] std::optional<ImageResource> find(const std::string &id) const;

  /**
   * @brief Intrinsic size only, in pixels -- no rasterization, no GPU
   *        texture, no GL context required. Safe to call off the render
   *        thread. Used for placeholder-height sizing, the same role
   *        DecodedImage::aspectRatio() plays for raster images.
   */
  [[nodiscard]] static std::optional<std::pair<float, float>>
  peekSize(std::span<const std::uint8_t> bytes);

private:
  /// Rasterizes and uploads; returns nullopt on any failure. Tries the GL
  /// fast path first when the device is GL/GLES, falling back to the CPU
  /// path -- see svg_cache.cpp.
  std::optional<ImageResource> rasterize(std::span<const std::uint8_t> bytes,
                                         float width, float height);

  render::RenderDevice *device_{nullptr};
  std::unordered_map<std::string, ImageResource> cache_;
  mutable std::mutex mutex_;
};

} // namespace gleditor

#endif // GLEDITOR_SVG_CACHE_HPP

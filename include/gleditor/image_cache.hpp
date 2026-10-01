/**
 * @file image_cache.hpp
 * @brief Image decoding, caching, and GPU RGBA8 texture atlas management.
 */
#ifndef GLEDITOR_IMAGE_CACHE_HPP
#define GLEDITOR_IMAGE_CACHE_HPP

#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <gleditor/decode_error.hpp>
#include <gleditor/mimetype.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render/types.hpp>

namespace gleditor {

/**
 * @struct DecodedImage
 * @brief Raw RGBA32 bitmap decoded from memory or file.
 */
struct DecodedImage {
  int width{0};
  int height{0};
  std::vector<std::uint8_t> rgba; // 4 bytes per pixel (R, G, B, A)

  [[nodiscard]] bool valid() const {
    return width > 0 && height > 0 &&
           rgba.size() >= static_cast<std::size_t>(width) *
                              static_cast<std::size_t>(height) * 4;
  }

  [[nodiscard]] float aspectRatio() const {
    return height > 0 ? static_cast<float>(width) / static_cast<float>(height)
                      : 1.0F;
  }
};

/**
 * @struct ImageResource
 * @brief Reference to an image loaded onto the GPU atlas.
 */
struct ImageResource {
  std::string id;
  int width{0};
  int height{0};
  int layer{-1};
  float u0{0.0F};
  float v0{0.0F};
  float u1{1.0F};
  float v1{1.0F};
  render::TextureHandle texture{};

  [[nodiscard]] bool valid() const {
    return width > 0 && height > 0 && layer >= 0 && texture.valid();
  }

  [[nodiscard]] float aspectRatio() const {
    return height > 0 ? static_cast<float>(width) / static_cast<float>(height)
                      : 1.0F;
  }
};

/// A decoded bitmap, or why there is none.
using DecodeResult = std::expected<DecodedImage, DecodeError>;

/**
 * @brief Decode an image from an in-memory buffer.
 */
[[nodiscard]] DecodeResult
decodeImageBuffer(std::span<const std::uint8_t> bytes,
                  const MimeType &mime = MimeType{});
[[nodiscard]] DecodeResult decodeImageBuffer(std::string_view bytes,
                                             const MimeType &mime = MimeType{});

/**
 * @brief Decode an image from a file path.
 */
[[nodiscard]] DecodeResult decodeImageFile(const std::string &filePath);

/**
 * @class ImageCache
 * @brief Manages RGBA8 GPU texture array atlas and image resources.
 */
class ImageCache {
public:
  explicit ImageCache(render::RenderDevice *device, int atlasSize = 2048,
                      int maxLayers = 16);
  ~ImageCache();

  ImageCache(const ImageCache &)            = delete;
  ImageCache &operator=(const ImageCache &) = delete;
  ImageCache(ImageCache &&)                 = delete;
  ImageCache &operator=(ImageCache &&)      = delete;

  /**
   * @brief Ensure an image is loaded and uploaded to the GPU atlas.
   * @param id Unique identifier (path, URI, or hash).
   * @param image Decoded bitmap.
   */
  ImageResource put(const std::string &id, const DecodedImage &image);

  /**
   * @brief Load image from file path and upload to GPU atlas.
   */
  std::optional<ImageResource> loadFile(const std::string &filePath);

  /**
   * @brief Load image from buffer and upload to GPU atlas.
   */
  std::optional<ImageResource> loadBuffer(const std::string &id,
                                          std::span<const std::uint8_t> bytes,
                                          const MimeType &mime = MimeType{});

  /**
   * @brief Find an already cached image resource.
   */
  [[nodiscard]] std::optional<ImageResource> find(const std::string &id) const;

  /**
   * @brief Access the underlying GPU texture array handle.
   */
  [[nodiscard]] render::TextureHandle texture() const { return atlasHandle_; }

  [[nodiscard]] int atlasSize() const { return atlasSize_; }
  [[nodiscard]] int layerCount() const { return allocatedLayers_; }

private:
  /// Upload @p decoded under @p id, or log why there is nothing to upload.
  std::optional<ImageResource> uploaded(const std::string &id,
                                        const DecodeResult &decoded);

  void ensureAtlasAllocated();

  render::RenderDevice *device_{nullptr};
  int atlasSize_{2048};
  int maxLayers_{16};
  int allocatedLayers_{0};
  render::TextureHandle atlasHandle_{};

  struct AtlasLayer {
    int layerIndex{0};
    int currentX{0};
    int currentY{0};
    int rowHeight{0};
  };

  std::vector<AtlasLayer> layers_;
  std::unordered_map<std::string, ImageResource> cache_;
  mutable std::mutex mutex_;
};

} // namespace gleditor

#endif // GLEDITOR_IMAGE_CACHE_HPP

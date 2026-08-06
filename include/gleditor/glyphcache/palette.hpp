/**
 * @file palette.hpp
 * @brief Glyph texture palette that packs glyph bitmaps into lanes and layers.
 *
 * GlyphPalette manages a single layer within a device array texture used by
 * the glyph cache. It organizes glyphs into GlyphLane rows and uploads glyph
 * coverage data into the appropriate sub-rect of that layer. Nothing here
 * knows which graphics API is in use; uploads go through RenderDevice.
 */
#ifndef GLYPH_PALETTE_H
#define GLYPH_PALETTE_H

#include <algorithm>                     // for for_each
#include <compare>                       // for partial_ordering
#include <cstddef>                       // for byte
#include <gleditor/glyphcache/lane.hpp>  // for GlyphLane
#include <gleditor/glyphcache/types.hpp> // for Rect, operator<<, TextureCo...
#include <gleditor/log.hpp>              // for Loggable, operator<<
#include <gleditor/render/types.hpp>     // for TextureHandle
#include <optional>                      // for optional
#include <span>                          // for span
#include <utility>                       // for to_underlying, move
#include <vector>                        // for vector

namespace render {
class RenderDevice;
}

enum class Length : int;

/**
 * @class GlyphPalette
 * @brief Packs glyph rectangles into lanes within a single texture layer.
 */
class GlyphPalette : public Loggable {
private:
  int layer;
  Rect paletteDims;
  render::RenderDevice *device;
  render::TextureHandle texture;
  Length usedHeight{};
  std::vector<GlyphLane> lanes;

  auto getBestLane(const Rect &charBox);

protected:
  void print(std::ostream &ost) const override {
    ost << "GlyphPalette(lanes: ";
    std::ranges::for_each(lanes.cbegin(), lanes.cend(),
                          [&ost](const auto &lane) { ost << lane << "\n"; });
    ost << ", w: " << paletteDims.width << ", h: " << paletteDims.height
        << ", availH: " << availHeight() << ")";
  }

public:
  /**
   * @brief Construct a palette bound to one layer of a device array texture.
   * @param paletteDims Dimensions of the layer.
   * @param aDevice Device uploads are issued through. Not owned.
   * @param aTexture Array texture this palette writes into.
   * @param aLayer Index of the layer within that array texture.
   */
  GlyphPalette(const Rect &paletteDims, render::RenderDevice *aDevice,
               const render::TextureHandle aTexture, const int aLayer)
      : layer(aLayer), paletteDims(paletteDims), device(aDevice),
        texture(aTexture) {}
  ~GlyphPalette() override = default;
  GlyphPalette(GlyphPalette &&oth) noexcept
      : layer(oth.layer), paletteDims(oth.paletteDims), device(oth.device),
        texture(oth.texture), usedHeight(oth.usedHeight),
        lanes(std::move(oth.lanes)) {}
  GlyphPalette &operator=(GlyphPalette &&oth) noexcept {
    if (this == &oth) {
      return *this;
    }

    layer       = oth.layer;
    paletteDims = oth.paletteDims;
    device      = oth.device;
    texture     = oth.texture;
    usedHeight  = oth.usedHeight;
    lanes       = std::move(oth.lanes);

    return *this;
  }
  GlyphPalette(const GlyphPalette &oth)            = default;
  GlyphPalette &operator=(const GlyphPalette &oth) = default;

  /**
   * @brief Height remaining in the palette layer that is not yet occupied by
   *        lanes. The free width can be found on a per-GlyphLane basis.
   */
  [[nodiscard]] Length availHeight() const {
    return Length{std::to_underlying(paletteDims.height) -
                  std::to_underlying(usedHeight)};
  }
  /**
   * @brief Check if a rectangle can be placed in this palette.
   *
   * Either there must be enough remaining height to create a new lane of the
   * rectangle's height, or there must exist an existing lane that can fit it.
   */
  bool canFit(const Rect &rect);
  /**
   * @brief Insert a glyph rectangle and upload its coverage data.
   * @param charBox Dimensions of the rectangle to insert.
   * @param data Tightly packed single-channel coverage, `width * height` bytes.
   *             May be empty for a zero-area box.
   * @return Normalized texture coordinates of the inserted rectangle, or
   *         nullopt if no space remains.
   */
  std::optional<TextureCoords> put(const Rect &charBox,
                                   std::span<const std::byte> data);

  /// Index of the array texture layer this palette owns.
  [[nodiscard]] int layerIndex() const { return layer; }

  /**
   * @brief Order palettes by most full to least full to improve packing.
   */
  friend std::partial_ordering operator<=>(const GlyphPalette &left,
                                           const GlyphPalette &right);
  /// Equality based on used height.
  friend bool operator==(const GlyphPalette &left, const GlyphPalette &right);
};

#endif // GLYPH_PALETTE_H
// vi: set sw=2 sts=2 ts=2 et:

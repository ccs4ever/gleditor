/**
 * @file palette.hpp
 * @brief Glyph texture palette that packs glyph bitmaps into lanes and layers.
 *
 * GlyphPalette manages a single 2D layer within a GL texture array used by the
 * glyph cache. It organizes glyphs into GlyphLane rows and uploads glyph bitmap
 * data into the appropriate sub-rect of the texture.
 */
#ifndef GLYPH_PALETTE_H
#define GLYPH_PALETTE_H

#include "gleditor/gl/gl.hpp"

#include <algorithm>                     // for __for_each_fn, for_each
#include <atomic>                        // for atomic
#include <compare>                       // for partial_ordering
#include <gleditor/glyphcache/lane.hpp>  // for GlyphLane
#include <gleditor/glyphcache/types.hpp> // for Rect, operator<<, TextureCo...
#include <gleditor/log.hpp>              // for Loggable, operator<<
#include <memory>                        // for shared_ptr
#include <optional>                      // for optional
#include <utility>                       // for to_underlying, move
#include <vector>                        // for vector

enum class Length : int;

/**
 * @class GlyphPalette
 * @brief Packs glyph rectangles into lanes within a single texture layer.
 */
class GlyphPalette : public Loggable {
private:
  int layer;
  Rect paletteDims;
  std::shared_ptr<GL> gl;
  Length usedHeight{};
  std::vector<GlyphLane> lanes;

  static std::atomic<int> layerCount;

  auto getBestLane(const Rect &charBox);

protected:
  void print(std::ostream &ost) const override {
    ost << "GlyphPalette(lanes: ";
    std::ranges::for_each(lanes.cbegin(), lanes.cend(),
                          [&ost](const auto& lane) { ost << lane << "\n"; });
    ost << ", w: " << paletteDims.width << ", h: " << paletteDims.height
        << ", availH: " << availHeight() << ")";
  }

public:
  GlyphPalette(Rect paletteDims, std::shared_ptr<GL> &ogl)
      : layer(layerCount.fetch_add(1)), paletteDims(paletteDims), gl{ogl} {}
  ~GlyphPalette() override = default;
  GlyphPalette(GlyphPalette &&oth) noexcept
      : layer(oth.layer), paletteDims(oth.paletteDims), gl(oth.gl),
        usedHeight(oth.usedHeight), lanes(std::move(oth.lanes)) {}
  GlyphPalette &operator=(GlyphPalette &&oth) noexcept {
    if (this == &oth) {
      return *this;
    }

    layer       = oth.layer;
    paletteDims = oth.paletteDims;
    usedHeight  = oth.usedHeight;
    lanes       = std::move(oth.lanes);

    return *this;
  }
  GlyphPalette(const GlyphPalette &oth)            = default;
  GlyphPalette &operator=(const GlyphPalette &oth) = default;

  /** Get the height of the remaining free space that isn't occupied
        by characters. The free width can be found on a per-GlyphLane basis
        by iterating over freeLanes. **/
  /**
   * @brief Height remaining in the palette layer that is not yet occupied by lanes.
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
   * @brief Insert a glyph rectangle and upload its pixel data to the GL texture.
   * @param charBox Dimensions of the rectangle to insert.
   * @param data Pointer to pixel data (format compatible with GL_BGRA/GL_UNSIGNED_BYTE).
   * @return Normalized texture coordinates of the inserted rectangle, or nullopt if no space.
   */
  std::optional<TextureCoords> put(const Rect &charBox,
                                   const unsigned char *data);

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

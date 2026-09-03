/**
 * @file lane.hpp
 * @brief Packing lane for glyph placement within a texture palette.
 *
 * Defines GlyphLane, which manages horizontal placement of glyphs within a
 * single row (lane) of a palette texture, tracking used width and providing
 * utilities to test and insert glyphs.
 */
#ifndef GLEDITOR_GLYPHCACHE_LANE_H
#define GLEDITOR_GLYPHCACHE_LANE_H

#include <algorithm>                     // for min
#include <compare>                       // for partial_ordering
#include <gleditor/glyphcache/types.hpp> // for operator<<, Rect, Point
#include <gleditor/log.hpp>              // for Loggable
#include <stdexcept>                     // for invalid_argument
#include <utility>                       // for to_underlying

namespace gleditor {

enum class Length : int;

/**
 * @class GlyphLane
 * @brief Represents a horizontal packing lane within a glyph texture palette.
 *
 * A GlyphLane tracks remaining width and fixed maximum character height for its
 * row inside a palette texture. It supports quick fit checks and inserting new
 * glyph rectangles, returning their top-left location within the palette.
 */
class GlyphLane : public Loggable {

private:
  Length maxCharHeight;
  Offset paletteYOffset;
  Length paletteWidth, usedWidth{};

protected:
  void print(std::ostream &ost) const override {
    ost << "GlyphLane(pw: " << paletteWidth << ", charH: " << maxCharHeight
        << ", pYOff: " << paletteYOffset << ", availW: " << availWidth() << ")";
  }

public:
  /**
   * @brief Construct a lane at the given palette Y offset with the specified
   * box width/height.
   * @param paletteYOffset Vertical offset of the lane within the palette
   * texture.
   * @param box Width of the lane (box.width) and max character height
   * (box.height).
   */
  GlyphLane(const Offset paletteYOffset, const Rect &box)
      : maxCharHeight(box.height), paletteYOffset(paletteYOffset),
        paletteWidth(box.width) {}
  ~GlyphLane() override = default;

  /**
   * @brief Check whether the lane can fit a glyph rectangle.
   * @param charBox Dimensions of the glyph rectangle.
   * @return true if both width and height constraints are satisfied.
   */
  [[nodiscard]] bool canFit(const Rect &charBox) const {
    return availWidth() >= charBox.width && maxCharHeight >= charBox.height;
  }
  /**
   * @brief Extend the lane to a wider palette.
   *
   * Glyphs fill from x = 0, so a wider layer only ever adds free space at the
   * right-hand end; nothing already in the lane moves. Narrowing is not
   * supported and would strand whatever sat beyond the new edge.
   */
  void widen(const Length newWidth) {
    if (std::to_underlying(newWidth) > std::to_underlying(paletteWidth)) {
      paletteWidth = newWidth;
    }
  }

  /**
   * @brief Remaining horizontal capacity of the lane.
   */
  [[nodiscard]] Length availWidth() const {
    return Length{std::to_underlying(paletteWidth) -
                  std::to_underlying(usedWidth)};
  }
  /**
   * @brief Insert a glyph of the given width into the lane.
   * @param charWidth Width of the glyph to insert.
   * @return Top-left location within the palette where the glyph should be
   * placed.
   * @throws std::invalid_argument if the glyph width exceeds available width.
   */
  Point put(const Length &charWidth) {
    if (std::to_underlying(charWidth) > std::to_underlying(availWidth())) {
      throw std::invalid_argument(
          "Character width too large or small to hold in lane");
    }
    const auto ret =
        Point{Offset{std::to_underlying(usedWidth)}, paletteYOffset};
    // Rounded up rather than advanced exactly, so the next glyph starts on a
    // mip block boundary as this one did. What that buys is in
    // gleditor::glyphAlignment: a glyph's minified appearance stops depending
    // on where in the atlas it happened to land, which is what made the same
    // scene render differently from one run to the next.
    usedWidth =
        Length{std::min(std::to_underlying(paletteWidth),
                        std::to_underlying(usedWidth) +
                            alignedToMipBlock(std::to_underlying(charWidth)))};
    return ret;
  }

  /// order by shortest to tallest lanes so that new chars can be quickly
  /// filtered
  friend std::partial_ordering operator<=>(const GlyphLane &left,
                                           const GlyphLane &right);
  friend bool operator==(const GlyphLane &left, const GlyphLane &right);
};

} // namespace gleditor

#endif // GLEDITOR_GLYPHCACHE_LANE_H

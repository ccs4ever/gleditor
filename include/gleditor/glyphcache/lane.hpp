/**
 * @file lane.hpp
 * @brief Packing lane for glyph placement within a texture palette.
 *
 * Defines GlyphLane, which manages horizontal placement of glyphs within a
 * single row (lane) of a palette texture, tracking used width and providing
 * utilities to test and insert glyphs.
 */
#ifndef GLYPH_LANE_H
#define GLYPH_LANE_H

#include <gleditor/glyphcache/types.hpp>  // for operator<<, Rect, Point
#include <gleditor/log.hpp>               // for Loggable
#include <compare>                        // for partial_ordering
#include <stdexcept>                      // for invalid_argument
#include <utility>                        // for to_underlying

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
   * @brief Construct a lane at the given palette Y offset with the specified box width/height.
   * @param paletteYOffset Vertical offset of the lane within the palette texture.
   * @param box Width of the lane (box.width) and max character height (box.height).
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
   * @brief Remaining horizontal capacity of the lane.
   */
  [[nodiscard]] Length availWidth() const {
    return Length{std::to_underlying(paletteWidth) -
                  std::to_underlying(usedWidth)};
  }
  /**
   * @brief Insert a glyph of the given width into the lane.
   * @param charWidth Width of the glyph to insert.
   * @return Top-left location within the palette where the glyph should be placed.
   * @throws std::invalid_argument if the glyph width exceeds available width.
   */
  Point put(const Length &charWidth) {
    if (std::to_underlying(charWidth) > std::to_underlying(availWidth())) {
      throw std::invalid_argument(
          "Character width too large or small to hold in lane");
    }
    const auto ret = Point{Offset{std::to_underlying(usedWidth)}, paletteYOffset};
    // std::cout << "put usedWidth: " << usedWidth << "\n";
    usedWidth =
        Length{std::to_underlying(usedWidth) + std::to_underlying(charWidth)};
    // std::cout << "put usedWidth after: " << usedWidth << "\n";
    return ret;
  }

  /// order by shortest to tallest lanes so that new chars can be quickly
  /// filtered
  friend std::partial_ordering operator<=>(const GlyphLane &left,
                                           const GlyphLane &right);
  friend bool operator==(const GlyphLane &left, const GlyphLane &right);
};

#endif // GLYPH_LANE_H
// vi: set sw=2 sts=2 ts=2 et:

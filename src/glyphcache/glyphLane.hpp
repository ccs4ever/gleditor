#ifndef GLYPH_LANE_H
#define GLYPH_LANE_H

#include <compare>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

// #include "../custom_concepts.hpp"
#include "../log.hpp"
#include "types.hpp"

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
  GlyphLane(Offset paletteYOffset, const Rect &box)
      : maxCharHeight(box.height), paletteYOffset(paletteYOffset),
        paletteWidth(box.width) {}
  ~GlyphLane() override = default;

  [[nodiscard]] bool canFit(const Rect &charBox) const {
    return availWidth() >= charBox.width && maxCharHeight >= charBox.height;
  }
  [[nodiscard]] Length availWidth() const {
    return Length{std::to_underlying(paletteWidth) -
                  std::to_underlying(usedWidth)};
  }
  Point put(const Length &charWidth) {
    if (std::to_underlying(charWidth) > std::to_underlying(availWidth())) {
      throw std::invalid_argument(
          "Character width too large or small to hold in lane");
    }
    auto ret = Point{Offset{std::to_underlying(usedWidth)}, paletteYOffset};
    usedWidth =
        Length{std::to_underlying(usedWidth) + std::to_underlying(charWidth)};
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

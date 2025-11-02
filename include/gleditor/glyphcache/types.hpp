/**
 * @file types.hpp
 * @brief Fundamental geometry and texture coordinate types used by the glyph cache.
 *
 * Provides small POD structs for integer and floating point points/rectangles,
 * character extents and texture coordinates, along with stream helpers and
 * equality operators. These types are shared across glyphcache components.
 */
#ifndef GLEDITOR_GLYPHCACHE_TYPES_H
#define GLEDITOR_GLYPHCACHE_TYPES_H

#include <iostream>
#include <utility>

enum class Length : int;  ///< Integer length unit used for glyph cache packing
enum class Offset : int;  ///< Integer offset unit used for glyph cache packing

/**
 * @brief Integer 2D point (uses Offset units).
 */
struct Point {
  Offset x;
  Offset y;
};

/**
 * @brief Floating-point 2D point used for normalized texture coordinates.
 */
struct PointF {
  float x;
  float y;
};

/**
 * @brief Integer rectangle defined by width and height (Length units).
 */
struct Rect {
  Length width;
  Length height;
};

/**
 * @brief Floating-point rectangle, typically normalized to [0,1] for textures.
 */
struct RectF {
  float width;
  float height;
};

/**
 * @brief Character extents in the palette: top-left position and box size.
 */
struct CharExtents {
  Point topLeft;
  Rect box;
};

/**
 * @brief Texture coordinates of a placed glyph: normalized top-left and size.
 */
struct TextureCoords {
  PointF topLeft;
  RectF box;
};

/// Stream helpers for basic units and structs.
inline std::ostream &operator<<(std::ostream &out, const Length &len) {
  out << std::to_underlying(len);
  return out;
}
inline std::ostream &operator<<(std::ostream &out, const Offset &off) {
  out << std::to_underlying(off);
  return out;
}
inline bool operator==(const Point &left, const Point &right) {
  return left.x == right.x && left.y == right.y;
}
inline std::ostream &operator<<(std::ostream &out, const Point &point) {
  out << "Point(x: " << point.x << ", y: " << point.y << ")";
  return out;
}
inline std::ostream &operator<<(std::ostream &out, const Rect &rect) {
  out << "Rect(w: " << rect.width << ", h: " << rect.height << ")";
  return out;
}
inline std::ostream &operator<<(std::ostream &out, const CharExtents &ext) {
  out << "CharExtents(topLeft: " << ext.topLeft << ", box: " << ext.box << ")";
  return out;
}

#endif // GLEDITOR_GLYPHCACHE_TYPES_H

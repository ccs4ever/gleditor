#ifndef GLEDITOR_GLYPHCACHE_TYPES_H
#define GLEDITOR_GLYPHCACHE_TYPES_H

#include <iostream>
#include <utility>


enum class Length : int;
enum class Offset : int;

struct Point {
  Offset x;
  Offset y;
};

struct PointF {
  float x;
  float y;
};

struct Rect {
  Length width;
  Length height;
};

struct RectF {
  float width;
  float height;
};

struct CharExtents {
  Point topLeft;
  Rect box;
};

struct TextureCoords {
  PointF topLeft;
  RectF box;
};

inline std::ostream &operator<<(std::ostream &out, const Length &len) {
  out << std::to_underlying(len);
  return out;
}
inline std::ostream &operator<<(std::ostream &out, const Offset &off) {
  out << std::to_underlying(off);
  return out;
}
inline bool operator==(const Point& left, const Point &right) {
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

/**
 * @file types.hpp
 * @brief Fundamental geometry and texture coordinate types used by the glyph
 * cache.
 *
 * Provides small POD structs for integer and floating point points/rectangles,
 * character extents and texture coordinates, along with stream helpers and
 * equality operators. These types are shared across glyphcache components.
 */
#ifndef GLEDITOR_GLYPHCACHE_TYPES_H
#define GLEDITOR_GLYPHCACHE_TYPES_H

#include <cstdint>
#include <iostream>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gleditor {

enum class Length : int; ///< Integer length unit used for glyph cache packing
enum class Offset : int; ///< Integer offset unit used for glyph cache packing

/**
 * @brief A presentation attribute a glyph can be rasterised with.
 *
 * Where a cache key normally names a cluster and a font, this is the third
 * axis: the same cluster in the same font rasterises to a different bitmap
 * once italic, bold, underline and so on are layered onto it. GlyphCache::put()
 * takes a set of these rather than one bool per attribute, because the number
 * of combinations is combinatorial and nothing calling put() should have to
 * enumerate them -- a caller just says which of these apply to this glyph,
 * the same way regardless of how many.
 *
 * Superscript and subscript name only the reduced size a glyph rasterises
 * at. The vertical shift that places a raised or lowered glyph on its line
 * is layout-time positioning of an already-rasterised quad, not something
 * this bakes into the bitmap.
 */
enum class Decoration : std::uint8_t {
  Bold,
  Italic,
  Underline,
  Overline,
  Strikethrough,
  Superscript,
  Subscript,
};

/// One bit per Decoration, so a run of text can carry which apply without
/// the per-glyph cost of an unordered_set: PageShaping keeps one of these
/// per glyph of a page that may hold thousands, where GlyphCache::put()'s
/// set is built once per distinct combination actually rasterised.
using DecorationMask = std::uint8_t;

[[nodiscard]] constexpr DecorationMask decorationBit(const Decoration d) {
  return static_cast<DecorationMask>(1U << std::to_underlying(d));
}

[[nodiscard]] constexpr bool hasDecoration(const DecorationMask mask,
                                           const Decoration d) {
  return 0 != (mask & decorationBit(d));
}

/**
 * @brief The name a Decoration is spelled as on the command line and in
 *        diagnostics -- --type's "[name,name]text" prefix reads these back.
 *
 * Kept here rather than deferred to xudu: parsing --type happens in the base
 * library, before any xudu-specific code exists to ask. xudu's
 * FormatAttribute has its own name table for the same words (see
 * format.hpp), and the two are kept in sync by hand rather than shared,
 * since the two enums exist specifically to stay independent of one
 * another.
 */
[[nodiscard]] constexpr const char *decorationName(const Decoration d) {
  switch (d) {
  case Decoration::Bold:
    return "bold";
  case Decoration::Italic:
    return "italic";
  case Decoration::Underline:
    return "underline";
  case Decoration::Overline:
    return "overline";
  case Decoration::Strikethrough:
    return "strikethrough";
  case Decoration::Superscript:
    return "superscript";
  case Decoration::Subscript:
    return "subscript";
  }
  return "bold";
}

/// The Decoration @p name spells, or nullopt for anything else -- including
/// case variants and abbreviations, which are not accepted so that a typo in
/// a script is reported rather than silently doing nothing.
[[nodiscard]] inline std::optional<Decoration>
decorationNamed(const std::string_view name) {
  for (const auto d :
       {Decoration::Bold, Decoration::Italic, Decoration::Underline,
        Decoration::Overline, Decoration::Strikethrough,
        Decoration::Superscript, Decoration::Subscript}) {
    if (name == decorationName(d)) {
      return d;
    }
  }
  return std::nullopt;
}

/// @p mask, expanded into the set form GlyphCache::put() takes.
[[nodiscard]] inline std::unordered_set<Decoration>
decorationSetFor(const DecorationMask mask) {
  std::unordered_set<Decoration> out;
  for (const auto d :
       {Decoration::Bold, Decoration::Italic, Decoration::Underline,
        Decoration::Overline, Decoration::Strikethrough,
        Decoration::Superscript, Decoration::Subscript}) {
    if (hasDecoration(mask, d)) {
      out.insert(d);
    }
  }
  return out;
}

/**
 * @brief A byte range of some text, and which decorations apply to glyphs
 *        whose cluster starts within it.
 *
 * Ranges are expressed relative to whatever text they were handed alongside
 * -- a whole document's offsets for TextSource::decoratedRanges(), a single
 * page's slice for LayoutOptions::decoratedRanges -- and it is the caller
 * translating between the two, the same way Doc::layoutFrom() already
 * translates absolute forced-break offsets into a slice-relative clamp.
 */
struct DecoratedRange {
  std::uint32_t start{};
  std::uint32_t end{}; ///< Half-open: covers [start, end).
  DecorationMask decorations{};

  /// Needed by anything keying a cache on the layout options a page was
  /// shaped with, since the decorations change the result.
  [[nodiscard]] bool operator==(const DecoratedRange &) const = default;
};

/**
 * @brief A byte range that must not be split across a page boundary.
 *
 * An embedded media placeholder's reserved blank lines, say, where a widget
 * drawn over that space expects the whole reserved rectangle to land on one
 * page. [@p start, @p end) is exactly (end - start) consecutive newline
 * characters (the shape every placeholder in this codebase takes), so its
 * own height at a layoutPage() call's line pitch is computable from its
 * length alone, with no separate pixel height to keep in sync -- only its
 * *width* needs to be carried separately, since a blank line has no glyphs
 * of its own for layoutPage()'s glyph-width scan to see. Same offset
 * convention as DecoratedRange: relative to whatever text it is handed
 * alongside, translated by the caller (Doc::layoutFrom()) between a whole
 * document's offsets and a single page's slice.
 */
struct AtomicRange {
  std::uint32_t start{};
  std::uint32_t end{}; ///< Half-open: covers [start, end).
  /// How wide the widget eventually drawn over this range actually is, so
  /// layoutPage() can widen the page it lands on to match -- without this,
  /// a page's own background quad (sized from the widest *rendered text*
  /// line) has no way to know a media item wider than any surrounding text
  /// needs more room, and the media draws past the quad's own right edge.
  /// Zero -- the default -- asks for no widening at all.
  float minWidthPx{0.0F};

  [[nodiscard]] bool operator==(const AtomicRange &) const = default;
};

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

} // namespace gleditor

#endif // GLEDITOR_GLYPHCACHE_TYPES_H

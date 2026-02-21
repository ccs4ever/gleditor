/**
 * @file palette.cpp
 * @brief Implementation of GlyphPalette lane management and GL uploads.
 */
#include <gleditor/glyphcache/palette.hpp>  // IWYU pragma: associated
//#include <GL/glew.h>                      // for GLint, GL_BGRA, GL_TEXTURE_...
#include <GL/gl.h>
#include <gleditor/glyphcache/types.hpp>  // for Rect, Point, TextureCoords
#include <gleditor/glyphcache/lane.hpp>   // for GlyphLane, operator<=>
#include <atomic>                         // for atomic
#include <compare>                        // for strong_ordering, partial_or...
#include <algorithm>                      // for __sort_fn, sort
#include <iterator>                       // for prev
#include <optional>                       // for optional, make_optional
#include <utility>                        // for to_underlying
#include <ranges>                         // for __find_if_fn, find_if
#include <vector>                         // for vector

enum class Length : int;
enum class Offset : int;

using std::make_optional;
using std::optional;

std::atomic<int> GlyphPalette::layerCount{};

auto GlyphPalette::getBestLane(const Rect &charBox) {
  const auto val = std::ranges::find_if(
      lanes, [&charBox](const auto &lane) { return lane.canFit(charBox); });
  if (lanes.cend() != val || availHeight() < charBox.height)
    return val + 0;
  // std::cout << "creating lane\n";
  lanes.emplace_back(Offset{std::to_underlying(usedHeight)},
                     Rect{paletteDims.width, charBox.height});
  // std::cout << "put usedHeight: " << usedHeight << "\n";
  usedHeight = Length{std::to_underlying(usedHeight) +
                      std::to_underlying(charBox.height)};
  // std::cout << "put usedHeight after: " << usedHeight << "\n";
  return std::prev(lanes.end());
}
bool GlyphPalette::canFit(const Rect &rect) {
  return paletteDims.width >= rect.width &&
         (availHeight() >= rect.height || getBestLane(rect) != lanes.cend());
}

optional<TextureCoords> GlyphPalette::put(const Rect &charBox,
                                          const unsigned char *data) {
  const auto lane = getBestLane(charBox);
  if (lanes.cend() == lane) {
    return std::nullopt;
  }
  auto [x, y] = lane->put(charBox.width);
  // std::cerr << std::format("lane point: {}/{} layer: {} box: {}/{}\n",
  // int(point.x), int(point.y), layer, int(charBox.width),
  // int(charBox.height));
  const auto yOffset = std::to_underlying(paletteDims.height) - std::to_underlying(y) - std::to_underlying(charBox.height);
  gl->texSubImage3D(GL_TEXTURE_2D_ARRAY, 0,
    std::to_underlying(x), yOffset, layer,
    std::to_underlying(charBox.width), std::to_underlying(charBox.height),
    1, GL_BGRA, GL_UNSIGNED_BYTE, data);
  std::ranges::sort(lanes);
  const auto wid = static_cast<float>(paletteDims.width);
  const auto hgt = static_cast<float>(paletteDims.height);
  return make_optional(TextureCoords{
      PointF{static_cast<float>(x) / wid, (static_cast<float>(yOffset) / hgt)},
      RectF{static_cast<float>(charBox.width) / wid, static_cast<float>(charBox.height) / hgt}});
}

[[nodiscard]] std::partial_ordering operator<=>(const GlyphPalette &left,
                                                const GlyphPalette &right) {
  return right.usedHeight <=> left.usedHeight;
}
[[nodiscard]] bool operator==(const GlyphPalette &left,
                              const GlyphPalette &right) {
  return right.usedHeight == left.usedHeight;
}
// vi: set sw=2 sts=2 ts=2 et:

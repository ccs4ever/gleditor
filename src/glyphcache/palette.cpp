/**
 * @file palette.cpp
 * @brief Implementation of GlyphPalette lane management and texture uploads.
 */
#include <gleditor/glyphcache/palette.hpp> // IWYU pragma: associated

#include <algorithm>                     // for sort
#include <compare>                       // for strong_ordering, partial_or...
#include <cstddef>                       // for byte
#include <gleditor/glyphcache/lane.hpp>  // for GlyphLane, operator<=>
#include <gleditor/glyphcache/types.hpp> // for Rect, Point, TextureCoords
#include <gleditor/render/device.hpp>    // for RenderDevice
#include <iterator>                      // for prev
#include <optional>                      // for optional, make_optional
#include <ranges>                        // for find_if
#include <span>                          // for span
#include <utility>                       // for to_underlying
#include <vector>                        // for vector

enum class Length : int;
enum class Offset : int;

using std::make_optional;
using std::optional;

auto GlyphPalette::getBestLane(const Rect &charBox) {
  const auto val = std::ranges::find_if(
      lanes, [&charBox](const auto &lane) { return lane.canFit(charBox); });
  if (lanes.cend() != val || availHeight() < charBox.height) {
    return val + 0;
  }
  lanes.emplace_back(Offset{std::to_underlying(usedHeight)},
                     Rect{paletteDims.width, charBox.height});
  usedHeight = Length{std::to_underlying(usedHeight) +
                      std::to_underlying(charBox.height)};
  return std::prev(lanes.end());
}

bool GlyphPalette::canFit(const Rect &rect) {
  return paletteDims.width >= rect.width &&
         (availHeight() >= rect.height || getBestLane(rect) != lanes.cend());
}

optional<TextureCoords>
GlyphPalette::put(const Rect &charBox, const std::span<const std::byte> data) {
  const auto lane = getBestLane(charBox);
  if (lanes.cend() == lane) {
    return std::nullopt;
  }
  auto [x, y] = lane->put(charBox.width);

  // Lanes grow downwards from the top of the palette while texture rows are
  // addressed from the bottom, so the lane offset is mirrored before upload.
  const auto yOffset = std::to_underlying(paletteDims.height) -
                       std::to_underlying(y) -
                       std::to_underlying(charBox.height);

  if (nullptr != device && !data.empty()) {
    device->updateTextureLayer(texture, layer, std::to_underlying(x), yOffset,
                               std::to_underlying(charBox.width),
                               std::to_underlying(charBox.height), data);
  }

  std::ranges::sort(lanes);

  const auto wid = static_cast<float>(paletteDims.width);
  const auto hgt = static_cast<float>(paletteDims.height);
  return make_optional(TextureCoords{
      PointF{static_cast<float>(x) / wid, static_cast<float>(yOffset) / hgt},
      RectF{static_cast<float>(charBox.width) / wid,
            static_cast<float>(charBox.height) / hgt}});
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

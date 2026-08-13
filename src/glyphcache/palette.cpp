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

  if (nullptr != device && !data.empty()) {
    device->updateTextureLayer(texture, layer, std::to_underlying(x),
                               std::to_underlying(y),
                               std::to_underlying(charBox.width),
                               std::to_underlying(charBox.height), data);
  }

  std::ranges::sort(lanes);

  // Texels, not a fraction of the texture. The atlas grows as glyphs arrive,
  // and a fraction would mean every glyph already written into a document's
  // vertex buffer pointed somewhere else the moment it did. Texels stay put --
  // growth only ever adds room above and to the right -- so the shader divides
  // by the texture's size at sampling time instead.
  return make_optional(TextureCoords{
      PointF{static_cast<float>(std::to_underlying(x)),
             static_cast<float>(std::to_underlying(y))},
      RectF{static_cast<float>(std::to_underlying(charBox.width)),
            static_cast<float>(std::to_underlying(charBox.height))}});
}

void GlyphPalette::grow(const Rect &newDims,
                        const render::TextureHandle aTexture) {
  // Lanes stack upwards from y = 0 and fill rightwards from x = 0, so a bigger
  // layer is purely additional room: nothing already placed moves, which is
  // what lets the glyphs be re-uploaded where they already were.
  paletteDims = newDims;
  texture     = aTexture;
  for (auto &lane : lanes) {
    lane.widen(newDims.width);
  }
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

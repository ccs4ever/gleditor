#pragma once

#include <cstddef>
#include <cstdint>
#include <gleditor/text/layout.hpp>
#include <optional>
#include <string_view>

namespace gleditor::text {

class ShapingCache;

enum class Overflow : std::uint8_t { Clip, Ellipsis, Wrap };
enum class EllipsisAt : std::uint8_t { End, Middle, Start };

/// Absent dimensions and maxLines mean unbounded. Clip and Ellipsis are
/// single-line policies; Wrap limits flowing text and ellipsizes its last line.
struct TextFit {
  std::optional<float> maxWidthPx{};
  std::optional<float> maxHeightPx{};
  std::optional<std::size_t> maxLines{1};
  Overflow overflow{Overflow::Ellipsis};
  EllipsisAt at{EllipsisAt::End};
  gleditor::TextAlign align{gleditor::TextAlign::Left};

  [[nodiscard]] bool operator==(const TextFit &) const = default;
};

struct FittedText {
  PageShaping shaping;
  float widthPx{};
  float heightPx{};
  std::size_t lines{};
  bool truncated{};
  /// Length of the retained source prefix. Start/Middle may also retain a
  /// suffix; its bytes are deliberately not included in this prefix count.
  std::size_t visibleBytes{};
};

/// Fits complete graphemes and HarfBuzz clusters, keeping original source
/// offsets in the result. Synthetic ellipsis clusters have byteLength zero.
/// Width measures typographic advances. Clip retains the intersecting edge
/// cluster for Canvas to crop; visibleBytes counts only fully retained source.
/// Renderers also clip physical ink, including italic overhang, to the box.
[[nodiscard]] FittedText fit(std::string_view text, const FontFacePtr &font,
                             const TextFit &constraints,
                             ShapingCache *cache = nullptr);

} // namespace gleditor::text

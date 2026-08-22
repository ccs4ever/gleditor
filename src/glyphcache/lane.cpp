/**
 * @file lane.cpp
 * @brief Implementation of GlyphLane packing and comparisons.
 */
#include <compare>
#include <gleditor/glyphcache/lane.hpp> // IWYU pragma: associated

namespace gleditor {

[[nodiscard]] std::partial_ordering operator<=>(const GlyphLane &left,
                                                const GlyphLane &right) {
  return left.maxCharHeight <=> right.maxCharHeight;
}
[[nodiscard]] bool operator==(const GlyphLane &left, const GlyphLane &right) {
  return left.maxCharHeight == right.maxCharHeight;
}

} // namespace gleditor

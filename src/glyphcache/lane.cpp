#include <gleditor/glyphcache/lane.hpp>
#include <compare>

[[nodiscard]] std::partial_ordering operator<=>(const GlyphLane &left,
                                                const GlyphLane &right) {
  return left.maxCharHeight <=> right.maxCharHeight;
}
[[nodiscard]] bool operator==(const GlyphLane &left, const GlyphLane &right) {
  return left.maxCharHeight == right.maxCharHeight;
}

#include "glyphLane.hpp"

[[nodiscard]] std::partial_ordering operator<=>(const GlyphLane &left,
                                                const GlyphLane &right) {
  return left.maxCharHeight <=> right.maxCharHeight;
}
[[nodiscard]] bool operator==(const GlyphLane &left, const GlyphLane &right) {
  return left.maxCharHeight == right.maxCharHeight;
}

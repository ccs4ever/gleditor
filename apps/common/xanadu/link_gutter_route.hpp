#ifndef XANADU_LINK_GUTTER_ROUTE_HPP
#define XANADU_LINK_GUTTER_ROUTE_HPP
#include <algorithm>
#include <array>
#include <cmath>
#include <glm/vec3.hpp>

namespace xanadu {
/// Three quadratic spans with horizontal attachments and tangent-continuous
/// joins along a shared gutter. End turns remain local even for distant pages.
inline std::array<glm::vec3, 7> gutterRoute(glm::vec3 from, glm::vec3 to,
                                            float corridor, float lineHeight) {
  const float dy   = to.y - from.y;
  const float turn = std::copysign(
      std::min(std::max(lineHeight, 0.F), std::abs(dy) * .25F), dy);
  return {from,
          glm::vec3(corridor, from.y, from.z),
          glm::vec3(corridor, from.y + turn, from.z),
          glm::vec3(corridor, (from.y + to.y) * .5F, (from.z + to.z) * .5F),
          glm::vec3(corridor, to.y - turn, to.z),
          glm::vec3(corridor, to.y, to.z),
          to};
}
} // namespace xanadu
#endif

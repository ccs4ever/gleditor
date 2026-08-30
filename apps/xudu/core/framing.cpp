#include "framing.hpp"

#include <gleditor/spatial.hpp>

namespace xudu {

PageStackExtent pageStackExtent(const std::vector<float> &pageHeightsWorld,
                                const float pageGapWorld) {
  if (pageHeightsWorld.empty()) {
    return {};
  }
  const auto lastIndex = pageHeightsWorld.size() - 1;
  return PageStackExtent{pageHeightsWorld.front() / 2.0F,
                         -(pageGapWorld * static_cast<float>(lastIndex)) -
                             (pageHeightsWorld.back() / 2.0F)};
}

float centroidY(const PageStackExtent &extent) {
  return (extent.topWorld + extent.bottomWorld) / 2.0F;
}

float centroidAlignmentDeltaY(const PageStackExtent &a,
                              const PageStackExtent &b) {
  return centroidY(a) - centroidY(b);
}

float framingDistance(const float worldWidth, const float worldHeight,
                      const float fovYDegrees, const float aspect,
                      const float margin) {
  return gleditor::spatial::framingDistance(worldWidth, worldHeight,
                                            fovYDegrees, aspect, margin);
}

float framingFov(const float worldWidth, const float worldHeight,
                 const float distance, const float aspect, const float margin) {
  return gleditor::spatial::framingFov(worldWidth, worldHeight, distance,
                                       aspect, margin);
}

std::size_t bandStrandCount(const float spanWorld, const float beamWidthWorld,
                            const float pitch, const std::size_t limit) {
  // A link end with no vertical reach -- both of its anchors on one line -- is
  // one strand, and so is anything asked for with a width or a pitch that
  // cannot be divided by.
  if (limit < 2 || !(beamWidthWorld > 0.0F) || !(pitch > 0.0F) ||
      !(spanWorld > 0.0F)) {
    return 1;
  }
  const auto wanted = static_cast<std::size_t>(
                          std::lround(spanWorld / (beamWidthWorld * pitch))) +
                      1;
  return std::clamp<std::size_t>(wanted, 1, limit);
}

std::vector<glm::vec3> bypassRoute(const glm::vec3 &from, const glm::vec3 &to,
                                   const float depth,
                                   const std::size_t segments) {
  if (segments < 1) {
    return {from, to};
  }
  // The control point is pulled twice as far back as the dip that is wanted:
  // a quadratic curve reaches half way to its control point at the middle, so
  // twice the depth there puts the middle of the route exactly at depth.
  const glm::vec3 control =
      (0.5F * (from + to)) + glm::vec3(0.0F, 0.0F, 2.0F * depth);

  std::vector<glm::vec3> route;
  route.reserve(segments + 1);
  for (std::size_t i = 0; i <= segments; i++) {
    const float t = static_cast<float>(i) / static_cast<float>(segments);
    const float u = 1.0F - t;
    route.push_back((u * u * from) + (2.0F * u * t * control) + (t * t * to));
  }
  return route;
}

} // namespace xudu

#include "framing.hpp"

#include <algorithm>
#include <cmath>

#include <gleditor/spatial.hpp>

namespace xanadu {

PageStackExtent pageStackExtent(const std::vector<float> &pageHeightsWorld,
                                const float pageGapWorld) {
  if (pageHeightsWorld.empty()) {
    return {};
  }
  const auto lastIndex = pageHeightsWorld.size() - 1;
  return PageStackExtent{.topWorld = pageHeightsWorld.front() / 2.0F,
                         .bottomWorld =
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
    route.push_back(
        gleditor::spatial::evaluateQuadraticBezier(from, control, to, t));
  }
  return route;
}

std::vector<glm::vec3> morphicRoute(const glm::vec3 &from, const glm::vec3 &to,
                                    const glm::vec3 &fromTangent,
                                    const glm::vec3 &toTangent,
                                    const std::size_t segments) {
  if (segments < 1) {
    return {from, to};
  }

  const float dist = glm::distance(from, to);
  if (dist <= 0.0F) {
    return {from, to};
  }

  // Scale tangents proportionally to arc distance (standard Hermite
  // parameterization)
  const float scale = 0.5F * dist;

  glm::vec3 m0 = fromTangent;
  if (glm::dot(m0, m0) > 0.0F) {
    m0 = glm::normalize(m0) * scale;
  } else {
    m0 = glm::vec3(to.x >= from.x ? 1.0F : -1.0F, 0.0F, 0.0F) * scale;
  }

  glm::vec3 m1 = toTangent;
  if (glm::dot(m1, m1) > 0.0F) {
    m1 = glm::normalize(m1) * scale;
  } else {
    m1 = glm::vec3(to.x >= from.x ? 1.0F : -1.0F, 0.0F, 0.0F) * scale;
  }

  std::vector<glm::vec3> route;
  route.reserve(segments + 1);

  for (std::size_t i = 0; i <= segments; ++i) {
    const float t  = static_cast<float>(i) / static_cast<float>(segments);
    const float t2 = t * t;
    const float t3 = t2 * t;

    // Cubic Hermite basis functions
    const float h00 = 2.0F * t3 - 3.0F * t2 + 1.0F;
    const float h10 = t3 - 2.0F * t2 + t;
    const float h01 = -2.0F * t3 + 3.0F * t2;
    const float h11 = t3 - t2;

    const glm::vec3 pt = h00 * from + h10 * m0 + h01 * to + h11 * m1;
    route.push_back(pt);
  }

  return route;
}

} // namespace xanadu

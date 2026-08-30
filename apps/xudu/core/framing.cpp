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

} // namespace xudu

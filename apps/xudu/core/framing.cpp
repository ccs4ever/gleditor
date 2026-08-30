#include "framing.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace xudu {

namespace {

constexpr float degreesToRadians(const float degrees) {
  return degrees * std::numbers::pi_v<float> / 180.0F;
}

constexpr float radiansToDegrees(const float radians) {
  return radians * 180.0F / std::numbers::pi_v<float>;
}

} // namespace

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
  const auto halfHeight = (worldHeight * margin) / 2.0F;
  const auto halfWidth  = (worldWidth * margin) / 2.0F;
  const auto tanHalfFov = std::tan(degreesToRadians(fovYDegrees) / 2.0F);
  const auto forHeight  = halfHeight / tanHalfFov;
  const auto forWidth   = halfWidth / (tanHalfFov * aspect);
  return std::max(forHeight, forWidth);
}

float framingFov(const float worldWidth, const float worldHeight,
                 const float distance, const float aspect, const float margin) {
  const auto halfHeight = (worldHeight * margin) / 2.0F;
  const auto halfWidth  = (worldWidth * margin) / 2.0F;
  const auto forHeight  = 2.0F * std::atan(halfHeight / distance);
  const auto forWidth   = 2.0F * std::atan(halfWidth / (distance * aspect));
  return radiansToDegrees(std::max(forHeight, forWidth));
}

} // namespace xudu

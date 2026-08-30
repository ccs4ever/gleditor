/**
 * @file spatial.hpp
 * @brief 3D camera frustum, projection, and framing math utilities.
 */
#ifndef GLEDITOR_SPATIAL_H
#define GLEDITOR_SPATIAL_H

#include <algorithm>
#include <cmath>
#include <numbers>

#include <glm/ext/matrix_float4x4.hpp>
#include <glm/ext/vector_float2.hpp>
#include <glm/ext/vector_float3.hpp>
#include <glm/ext/vector_float4.hpp>

namespace gleditor::spatial {

namespace detail {

constexpr float degreesToRadians(const float degrees) {
  return degrees * std::numbers::pi_v<float> / 180.0F;
}

constexpr float radiansToDegrees(const float radians) {
  return radians * 180.0F / std::numbers::pi_v<float>;
}

} // namespace detail

/**
 * @brief Whether @p worldPoint is inside the view frustum.
 *
 * @param viewProjection Combined projection * view matrix.
 * @param worldPoint World-space coordinates to test.
 */
inline bool onScreen(const glm::mat4 &viewProjection,
                     const glm::vec3 &worldPoint) {
  const auto clip = viewProjection * glm::vec4(worldPoint, 1.0F);
  if (clip.w <= 0.0F) {
    return false;
  }
  const auto ndcX = clip.x / clip.w;
  const auto ndcY = clip.y / clip.w;
  const auto ndcZ = clip.z / clip.w;
  return ndcX >= -1.0F && ndcX <= 1.0F && ndcY >= -1.0F && ndcY <= 1.0F &&
         ndcZ >= -1.0F && ndcZ <= 1.0F;
}

/**
 * @brief Project a 3D world-space coordinate to 2D screen pixels.
 *
 * @param viewProjection Combined projection * view matrix.
 * @param worldPoint 3D point in world space.
 * @param screenWidth Width of viewport in pixels.
 * @param screenHeight Height of viewport in pixels.
 * @return 2D screen coordinate in pixels (origin at bottom-left).
 */
inline glm::vec2 projectToScreen(const glm::mat4 &viewProjection,
                                 const glm::vec3 &worldPoint,
                                 const float screenWidth,
                                 const float screenHeight) {
  const auto clip = viewProjection * glm::vec4(worldPoint, 1.0F);
  if (clip.w <= 0.0001F) {
    return {0.0F, 0.0F};
  }
  const float ndcX = clip.x / clip.w;
  const float ndcY = clip.y / clip.w;

  const float screenX = (ndcX * 0.5F + 0.5F) * screenWidth;
  const float screenY = (ndcY * 0.5F + 0.5F) * screenHeight;
  return {screenX, screenY};
}

/**
 * @brief Camera distance along forward axis to fit a worldWidth x worldHeight
 *        box in a symmetric perspective frustum of vertical FOV @p fovYDegrees
 *        and aspect ratio @p aspect.
 *
 * @param margin Multiplier before fitting (1.0 = exact fit, > 1.0 = margin).
 */
inline float framingDistance(const float worldWidth, const float worldHeight,
                             const float fovYDegrees, const float aspect,
                             const float margin = 1.0F) {
  const auto halfHeight = (worldHeight * margin) / 2.0F;
  const auto halfWidth  = (worldWidth * margin) / 2.0F;
  const auto tanHalfFov =
      std::tan(detail::degreesToRadians(fovYDegrees) / 2.0F);
  const auto forHeight = halfHeight / tanHalfFov;
  const auto forWidth  = halfWidth / (tanHalfFov * aspect);
  return std::max(forHeight, forWidth);
}

/**
 * @brief Vertical FOV in degrees that fits a worldWidth x worldHeight box from
 *        a camera at distance @p distance.
 */
inline float framingFov(const float worldWidth, const float worldHeight,
                        const float distance, const float aspect,
                        const float margin = 1.0F) {
  const auto halfHeight = (worldHeight * margin) / 2.0F;
  const auto halfWidth  = (worldWidth * margin) / 2.0F;
  const auto forHeight  = 2.0F * std::atan(halfHeight / distance);
  const auto forWidth   = 2.0F * std::atan(halfWidth / (distance * aspect));
  return detail::radiansToDegrees(std::max(forHeight, forWidth));
}

} // namespace gleditor::spatial

#endif // GLEDITOR_SPATIAL_H

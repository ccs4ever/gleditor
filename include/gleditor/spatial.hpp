/**
 * @file spatial.hpp
 * @brief 3D camera frustum, projection, and framing math utilities.
 */
#ifndef GLEDITOR_SPATIAL_H
#define GLEDITOR_SPATIAL_H

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>

#include <glm/ext/matrix_double4x4.hpp>
#include <glm/ext/matrix_float4x4.hpp>
#include <glm/ext/vector_double3.hpp>
#include <glm/ext/vector_double4.hpp>
#include <glm/ext/vector_float2.hpp>
#include <glm/ext/vector_float3.hpp>
#include <glm/ext/vector_float4.hpp>
#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

namespace gleditor::spatial {

namespace detail {

constexpr float degreesToRadians(const float degrees) {
  return degrees * std::numbers::pi_v<float> / 180.0F;
}

constexpr float radiansToDegrees(const float radians) {
  return radians * 180.0F / std::numbers::pi_v<float>;
}

/// Below this |cos| between a ray and a plane they are treated as parallel:
/// the hit would lie so far along a grazing ray that a single-precision
/// parameter has no digits left to say where.
inline constexpr float kParallelCosine = 1e-6F;

/// A segment whose squared length is below this is treated as a point.
inline constexpr float kDegenerateLengthSq = 1e-12F;

} // namespace detail

/**
 * @brief Evaluate a quadratic Bezier curve at parameter @p t in [0.0, 1.0].
 *
 * @tparam T Vector type (e.g. glm::vec2, glm::vec3).
 * @param p0 Start point.
 * @param p1 Control point.
 * @param p2 End point.
 * @param t Curve parameter in [0.0, 1.0].
 */
template <typename T>
constexpr T evaluateQuadraticBezier(const T &p0, const T &p1, const T &p2,
                                    const float t) noexcept {
  const float inv = 1.0F - t;
  return (inv * inv * p0) + (2.0F * inv * t * p1) + (t * t * p2);
}

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
 * The form with a sentinel: a point behind the camera comes back as (0, 0),
 * which a caller cannot tell from the bottom-left corner. It stays for its
 * callers; new code uses projectToViewport().
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

/// A rectangle of the target in bottom-up pixels: a pane, or all of it.
struct Viewport {
  float left{}, bottom{}, width{}, height{};
};

/// A half-line in world space.
struct Ray {
  glm::vec3 origin{};
  glm::vec3 direction{}; ///< Unit length.
};

/**
 * @brief Pointer coordinates (window units, top-down) to bottom-up pixels.
 *
 * Pointer events arrive in window units with the origin at the top left;
 * projection produces drawable pixels with the origin at the bottom left. The
 * two differ by the platform's content scale and by which way y runs.
 */
[[nodiscard]] constexpr glm::vec2
windowToPixels(const float x, const float y, const float windowHeight,
               const float contentScale) noexcept {
  return {x * contentScale, (windowHeight - y) * contentScale};
}

/**
 * @brief The pixel a world point lands on, and its depth.
 *
 * Works in the backend-neutral clip space the renderer builds, where depth
 * runs over [-1, 1] after the divide. Each device adapts that to its own
 * convention -- Vulkan flips y and reverses z -- on its own copy of the
 * matrix, so nothing here may.
 *
 * @return (x, y) in bottom-up pixels of the target, and z, the normalised
 *         depth: in [-1, 1] between the near and far planes. Nothing for a
 *         point on or behind the camera plane, where the divide would flip or
 *         lose the result.
 */
[[nodiscard]] inline std::optional<glm::vec3>
projectToViewport(const glm::mat4 &worldToClip, const glm::vec3 &world,
                  const Viewport &viewport) noexcept {
  const auto clip = worldToClip * glm::vec4(world, 1.0F);
  if (clip.w <= 0.0F) {
    return std::nullopt;
  }
  const glm::vec3 ndc = glm::vec3(clip) / clip.w;
  return glm::vec3{
      viewport.left + (((ndc.x * 0.5F) + 0.5F) * viewport.width),
      viewport.bottom + (((ndc.y * 0.5F) + 0.5F) * viewport.height), ndc.z};
}

/**
 * @brief The world-space ray through a pixel: projectToViewport() backwards.
 *
 * The ray starts on the near plane and points at the far one. The inverse is
 * taken in double precision because the default near and far planes are five
 * orders of magnitude apart, and a single-precision inverse of that matrix
 * loses the digits a far point needs.
 *
 * @param pixel Bottom-up pixels of the target, as windowToPixels() gives.
 * @return Nothing if the matrix cannot be inverted or the viewport is empty.
 */
[[nodiscard]] inline std::optional<Ray>
unprojectToRay(const glm::mat4 &worldToClip, const glm::vec2 pixel,
               const Viewport &viewport) noexcept {
  if (!(viewport.width > 0.0F) || !(viewport.height > 0.0F)) {
    return std::nullopt;
  }
  const glm::dmat4 forward{worldToClip};
  const double determinant = glm::determinant(forward);
  if (determinant == 0.0 || !std::isfinite(determinant)) {
    return std::nullopt;
  }
  const glm::dmat4 inverse = glm::inverse(forward);
  const double ndcX =
      (static_cast<double>(pixel.x - viewport.left) / viewport.width * 2.0) -
      1.0;
  const double ndcY =
      (static_cast<double>(pixel.y - viewport.bottom) / viewport.height * 2.0) -
      1.0;
  const glm::dvec4 nearClip = inverse * glm::dvec4(ndcX, ndcY, -1.0, 1.0);
  const glm::dvec4 farClip  = inverse * glm::dvec4(ndcX, ndcY, 1.0, 1.0);
  if (nearClip.w == 0.0 || farClip.w == 0.0) {
    return std::nullopt;
  }
  const glm::dvec3 nearPoint = glm::dvec3(nearClip) / nearClip.w;
  const glm::dvec3 farPoint  = glm::dvec3(farClip) / farClip.w;
  const glm::dvec3 towards   = farPoint - nearPoint;
  const double length        = glm::length(towards);
  if (!(length > 0.0) || !std::isfinite(length)) {
    return std::nullopt;
  }
  return Ray{.origin    = glm::vec3(nearPoint),
             .direction = glm::vec3(towards / length)};
}

/**
 * @brief Where a ray meets the plane through @p point with normal @p normal.
 *
 * @return Nothing when the ray runs parallel to the plane, or the plane lies
 *         behind the ray's origin.
 */
[[nodiscard]] inline std::optional<glm::vec3>
intersectPlane(const Ray &ray, const glm::vec3 &point,
               const glm::vec3 &normal) noexcept {
  const float normalLength = glm::length(normal);
  const float facing       = glm::dot(normal, ray.direction);
  if (!(normalLength > 0.0F) ||
      std::abs(facing) <= detail::kParallelCosine * normalLength) {
    return std::nullopt;
  }
  const float along = glm::dot(point - ray.origin, normal) / facing;
  if (along < 0.0F) {
    return std::nullopt;
  }
  return ray.origin + (along * ray.direction);
}

/**
 * @brief Where a ray meets a flat box centred on a plane's origin.
 *
 * The box spans [-halfWidth, halfWidth] x [-halfHeight, halfHeight] in the
 * plane's own x and y, at its z = 0. The ray is taken into the plane's frame
 * rather than the box into the world's, so a scaled plane is measured in its
 * own units.
 *
 * @return The hit in the plane's own coordinates. Nothing if the ray misses,
 *         runs parallel, or the plane's matrix cannot be inverted.
 */
[[nodiscard]] inline std::optional<glm::vec2>
intersectQuad(const Ray &ray, const glm::mat4 &planeToWorld,
              const float halfWidth, const float halfHeight) noexcept {
  const float determinant = glm::determinant(planeToWorld);
  if (determinant == 0.0F || !std::isfinite(determinant)) {
    return std::nullopt;
  }
  const glm::mat4 worldToPlane = glm::inverse(planeToWorld);
  const glm::vec3 origin{worldToPlane * glm::vec4(ray.origin, 1.0F)};
  const glm::vec3 direction{worldToPlane * glm::vec4(ray.direction, 0.0F)};
  if (std::abs(direction.z) <=
      detail::kParallelCosine * glm::length(direction)) {
    return std::nullopt;
  }
  // An affine map keeps the ray's parameter, so a hit behind the origin here
  // is behind it in the world too.
  const float along = -origin.z / direction.z;
  if (along < 0.0F) {
    return std::nullopt;
  }
  const glm::vec2 hit = glm::vec2(origin) + (along * glm::vec2(direction));
  if (std::abs(hit.x) > halfWidth || std::abs(hit.y) > halfHeight) {
    return std::nullopt;
  }
  return hit;
}

/**
 * @brief The shortest distance between a ray and a segment.
 *
 * For picking something thin, an edge drawn as a line, by how near the
 * pointer's ray passes it. The closest pair is found on the two infinite lines
 * and then clamped: the segment's parameter to [0, 1] and the ray's to
 * [0, inf), each clamp re-solving the other parameter. That is exact because
 * the squared distance is convex in both parameters.
 */
[[nodiscard]] inline float distanceToSegment(const Ray &ray,
                                             const glm::vec3 &from,
                                             const glm::vec3 &to) noexcept {
  const glm::vec3 span     = to - from;
  const glm::vec3 offset   = ray.origin - from;
  const float rayLengthSq  = glm::dot(ray.direction, ray.direction);
  const float spanLengthSq = glm::dot(span, span);
  const float spanOffset   = glm::dot(span, offset);
  const float rayOffset    = glm::dot(ray.direction, offset);
  const auto onRayFor      = [rayLengthSq](const float towards) {
    return rayLengthSq > 0.0F ? std::max(0.0F, towards / rayLengthSq) : 0.0F;
  };

  float onRay  = onRayFor(-rayOffset);
  float onSpan = 0.0F;
  if (spanLengthSq > detail::kDegenerateLengthSq) {
    const float cross       = glm::dot(ray.direction, span);
    const float denominator = (rayLengthSq * spanLengthSq) - (cross * cross);
    if (denominator > 0.0F) {
      onRay =
          std::max(0.0F, ((cross * spanOffset) - (rayOffset * spanLengthSq)) /
                             denominator);
    } else {
      onRay = 0.0F;
    }
    onSpan = ((cross * onRay) + spanOffset) / spanLengthSq;
    if (onSpan < 0.0F) {
      onSpan = 0.0F;
      onRay  = onRayFor(-rayOffset);
    } else if (onSpan > 1.0F) {
      onSpan = 1.0F;
      onRay  = onRayFor(cross - rayOffset);
    }
  }
  return glm::length((ray.origin + (onRay * ray.direction)) -
                     (from + (onSpan * span)));
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

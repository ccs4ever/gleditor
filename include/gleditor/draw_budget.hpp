/**
 * @file draw_budget.hpp
 * @brief What a frame is allowed to spend on drawing documents, and what it
 *        spent.
 *
 * Its own header because both ends of the decision need it: Doc and Page apply
 * it, and the renderer sets it and reports the result. doc.hpp already includes
 * renderer.hpp, so neither of those can include the other.
 */
#ifndef GLEDITOR_DRAW_BUDGET_H
#define GLEDITOR_DRAW_BUDGET_H

#include <algorithm>
#include <array>
#include <cstdint>

#include <glm/ext/matrix_float4x4.hpp>
#include <glm/ext/vector_float2.hpp>
#include <glm/ext/vector_float4.hpp>
#include <glm/geometric.hpp>
#include <limits>

/**
 * @brief How much of the frame a page is allowed to cost, and what it takes to
 *        decide that.
 *
 * A page's cost is decided from how big it lands on screen, not from how far
 * away it is: distance alone says nothing without the field of view and the
 * size of the drawable.
 */
struct DrawBudget {
  /// Drawable width in pixels, which is what turns a clip-space extent into a
  /// number of screen pixels.
  float screenWidth{800.0F};
  /**
   * @brief Screen pixels per layout pixel below which a page is drawn coarsely.
   *
   * One layout pixel is roughly one pixel of a glyph, so this is "how many
   * screen pixels a glyph feature gets". Zero disables the coarse path.
   */
  float coarseBelow{};
  /// Whether pages outside the view frustum are skipped.
  bool cull{true};
};

/// What one frame's collection decided, for reporting. Culling that is not
/// counted is culling nobody can check.
struct DrawStats {
  std::uint32_t pages{};    ///< Pages considered.
  std::uint32_t culled{};   ///< Skipped as entirely outside the view.
  std::uint32_t coarse{};   ///< Drawn as solid bars rather than glyphs.
  std::uint32_t detailed{}; ///< Drawn glyph by glyph.
};

/**
 * @brief True when a flat box, centred on the model-space origin, lies entirely
 *        outside the view.
 *
 * @param mvp    projection * view * model for the box.
 * @param halfW,halfH Half the box's extent in model units.
 * @param depth  How far in front of the box's plane its contents reach.
 *
 * Rejected only when every corner falls outside the same clip plane, which is
 * the conservative form of the test: a box straddling two planes without
 * crossing the view is kept and drawn, costing a draw rather than a wrong
 * frame. The comparisons are made in clip space, before the perspective
 * divide, so a corner behind the camera needs no special case -- dividing by a
 * negative w is what would flip a sign and cull something visible.
 *
 * Only the four side planes are tested: a document is spread sideways and
 * downwards rather than in depth, so testing depth would cost for nothing
 * measurable. (It would not be backend-dependent: the matrix here is in the
 * neutral clip space, -w <= z <= w, and only the Vulkan device's own copy is
 * rewritten to reversed Z.)
 */
inline bool outsideFrustum(const glm::mat4 &mvp, const float halfW,
                           const float halfH, const float depth) {
  const std::array<glm::vec4, 8> corners = {
      mvp * glm::vec4(-halfW, -halfH, 0.0F, 1.0F),
      mvp * glm::vec4(halfW, -halfH, 0.0F, 1.0F),
      mvp * glm::vec4(-halfW, halfH, 0.0F, 1.0F),
      mvp * glm::vec4(halfW, halfH, 0.0F, 1.0F),
      mvp * glm::vec4(-halfW, -halfH, depth, 1.0F),
      mvp * glm::vec4(halfW, -halfH, depth, 1.0F),
      mvp * glm::vec4(-halfW, halfH, depth, 1.0F),
      mvp * glm::vec4(halfW, halfH, depth, 1.0F)};

  const auto allOutside = [&corners](auto beyond) {
    return std::ranges::all_of(corners, beyond);
  };
  return allOutside([](const glm::vec4 &pos) { return pos.x < -pos.w; }) ||
         allOutside([](const glm::vec4 &pos) { return pos.x > pos.w; }) ||
         allOutside([](const glm::vec4 &pos) { return pos.y < -pos.w; }) ||
         allOutside([](const glm::vec4 &pos) { return pos.y > pos.w; });
}

/**
 * @brief True when a flat box, centred on the model-space origin, lies wholly
 *        within the view: every corner in front of the camera and inside all
 *        six clip planes.
 *
 * @param mvp    projection * view * model for the box.
 * @param halfW,halfH Half the box's extent in model units.
 *
 * Unlike outsideFrustum() this tests depth too. That is safe here because the
 * test is made in the backend-neutral clip space the renderer builds, where
 * the volume is -w <= z <= w; Vulkan's reversed depth is applied later, inside
 * its device, to its own copy of the matrix. A box that crosses the near
 * plane is cut by it, so it is not inside.
 */
[[nodiscard]] inline bool insideFrustum(const glm::mat4 &mvp, const float halfW,
                                        const float halfH) noexcept {
  const std::array<glm::vec4, 4> corners = {
      mvp * glm::vec4(-halfW, -halfH, 0.0F, 1.0F),
      mvp * glm::vec4(halfW, -halfH, 0.0F, 1.0F),
      mvp * glm::vec4(-halfW, halfH, 0.0F, 1.0F),
      mvp * glm::vec4(halfW, halfH, 0.0F, 1.0F)};
  return std::ranges::all_of(corners, [](const glm::vec4 &pos) {
    return pos.w > 0.0F && pos.x >= -pos.w && pos.x <= pos.w &&
           pos.y >= -pos.w && pos.y <= pos.w && pos.z >= -pos.w &&
           pos.z <= pos.w;
  });
}

/**
 * @brief Screen pixels one model unit covers at the origin of @p mvp.
 *
 * Derived from the transform rather than from a distance: something is small on
 * screen because of where the camera is, how wide the field of view is and how
 * large the drawable is, and only the combination says whether a glyph still
 * has pixels to be drawn with.
 *
 * Returns infinity on the camera plane, where the projection says nothing
 * useful -- which reads as "very close", and so as full detail.
 */
inline float screenScaleAt(const glm::mat4 &mvp, const float screenWidth) {
  const auto centre = mvp * glm::vec4(0.0F, 0.0F, 0.0F, 1.0F);
  const auto clipW  = centre.w < 0.0F ? -centre.w : centre.w;
  if (clipW < 1e-6F) {
    return std::numeric_limits<float>::infinity();
  }
  // A model offset d lands at clip position mvp*d; dividing by the centre's w
  // gives the normalised device offset, which spans the drawable over the range
  // [-1, 1] -- hence the half width.
  const auto step = mvp * glm::vec4(1.0F, 0.0F, 0.0F, 0.0F);
  return glm::length(glm::vec2(step.x, step.y)) / clipW * 0.5F * screenWidth;
}

#endif // GLEDITOR_DRAW_BUDGET_H

/**
 * @file framing.hpp
 * @brief Where two documents of possibly different lengths belong relative to
 *        each other, and how far back a camera has to sit to hold both.
 *
 * A single link has one pair of ends to level on, which is what
 * xanadu::LinkBeams::align() used to do directly: read the two anchors' world Y
 * and offset one document by the difference. That stops being a sensible rule
 * once several links run between the same two documents -- a many-to-many
 * mesh has no one anchor pair that is "the" one to level on, and whichever
 * link happened to scroll into view first would decide it arbitrarily. The
 * rule that generalises is levelling the documents' vertical centres instead,
 * which is a property of the two page stacks and not of any single link.
 *
 * Both this and the camera distance needed to fit two such documents side by
 * side are geometry over page counts and page heights -- no glyph, no device.
 * That is why they live here rather than beside the drawing: the same reason
 * link_layout.hpp gives for placeLinks().
 */
#ifndef XUDU_FRAMING_H
#define XUDU_FRAMING_H

#include <cstddef>
#include <optional>
#include <vector>

#include <glm/ext/vector_float2.hpp>
#include <glm/ext/vector_float3.hpp>

namespace xanadu {

/// The vertical reach of one document's stacked pages, in world units,
/// measured from the document's own local origin -- before whatever moves the
/// document as a whole (its model matrix) is applied.
struct PageStackExtent {
  /// Top edge of the first page.
  float topWorld{};
  /// Bottom edge of the last page.
  float bottomWorld{};
};

/**
 * @brief The vertical extent of a document whose pages stack pageGapWorld
 *        apart, top to top, each pageGapWorld * static_cast<float>(index)
 *        below the one before -- see Doc::newPage(), where the stacking
 *        distance this must be called with lives as Doc::pageGapWorld.
 * @param pageHeightsWorld Each page's height in world units, first to last.
 *        Only the first and last matter -- a page stack's top comes from its
 *        first page and its bottom from its last -- but the whole list is
 *        taken so a caller reading heights off real pages does not have to
 *        pick which ones to keep.
 */
[[nodiscard]] PageStackExtent
pageStackExtent(const std::vector<float> &pageHeightsWorld, float pageGapWorld);

/// Where a page stack is centred vertically, in its own local space.
[[nodiscard]] float centroidY(const PageStackExtent &extent);

/**
 * @brief World-space Y to move document B by so its vertical centre lines up
 *        with document A's.
 *
 * $\Delta Y = Y_{midA} - Y_{midB}$: added to B's current Y, it makes B's
 * centroid coincide with A's regardless of how many pages either holds or how
 * the two differ in length.
 */
[[nodiscard]] float centroidAlignmentDeltaY(const PageStackExtent &a,
                                            const PageStackExtent &b);

/**
 * @brief Camera distance along its forward axis for a worldWidth x
 *        worldHeight box, centred on the look point, to fill a symmetric
 *        perspective frustum of vertical field of view fovYDegrees and
 *        aspect ratio aspect.
 * @param margin Multiplies the box before fitting; 1 fits it exactly to the
 *        frustum edges, greater than 1 leaves headroom around it.
 */
[[nodiscard]] float framingDistance(float worldWidth, float worldHeight,
                                    float fovYDegrees, float aspect,
                                    float margin = 1.0F);

/**
 * @brief The inverse of framingDistance(): the vertical field of view that
 *        fits a worldWidth x worldHeight box from a camera already distance
 *        away.
 *
 * What picks the `--fov` an orchestration test asks the binary to run with,
 * given a camera distance it does not otherwise move (see
 * AppState::ViewPerspective::resetPos()) and a box worked out from the page
 * counts the test built the documents with.
 */
[[nodiscard]] float framingFov(float worldWidth, float worldHeight,
                               float distance, float aspect,
                               float margin = 1.0F);

/**
 * @brief How many strands a link is drawn with, given the taller of its two
 *        ends.
 *
 * A link end is a range of bytes and so a range of lines, not a point, and
 * what is drawn between two of them is a band. The count comes from the taller
 * end, so that end is drawn at its full reach rather than reduced to whatever
 * the other end happens to be, and the strands are spaced @p pitch beam widths
 * apart there: comfortably more than one, so they stay clear of each other
 * where the band is at its tallest. Ribbons that merely abut overlap in a thin
 * seam and print a strip of doubled alpha down it, which reads as stripes
 * running the length of the band rather than as one connection.
 *
 * @param spanWorld Vertical reach of the taller end, in world units.
 * @param beamWidthWorld Width one strand is drawn at.
 * @param pitch Spacing between strands, in beam widths.
 * @param limit Most strands to use. A link between two whole pages would
 *        otherwise ask for one per line of text.
 * @return At least one, at most @p limit.
 */
[[nodiscard]] std::size_t bandStrandCount(float spanWorld, float beamWidthWorld,
                                          float pitch, std::size_t limit);

/**
 * @brief The route a beam takes to pass behind whatever stands between its
 *        two ends.
 *
 * A quadratic curve dipping to @p depth at its midpoint and meeting both ends
 * exactly, sampled into @p segments straight runs. Three straight pieces
 * through two corners would do as much geometrically and did, but the middle
 * of one runs almost straight away from the camera, and a ribbon that lies in
 * the plane of the pages has next to no width left when it is seen end on --
 * so what should have read as one beam going the long way round read as two
 * stubs with a gap between them.
 *
 * @param depth How far behind the page plane the middle of the route sits,
 *        as an offset in Z. Negative goes away from the camera.
 * @return @p segments + 1 points, ends included; @p from and @p to alone when
 *         @p segments is zero.
 */
[[nodiscard]] std::vector<glm::vec3> bypassRoute(const glm::vec3 &from,
                                                 const glm::vec3 &to,
                                                 float depth,
                                                 std::size_t segments);

/**
 * @brief Morphic 3D butterfly ribbon route connecting 2D document text lines
 *        to 3D Zigzag cell boundaries.
 *
 * Uses a cubic Hermite spline in 3D space with boundary tangent matching:
 * - Start tangent aligns coplanar with the document page plane.
 * - End tangent aligns with the cell's surface normal / docking orientation.
 *
 * Prevents ribbon collapse and singularities when endpoints span disparate
 * depth planes (e.g. Z = 0 to Z = -40).
 *
 * @param from Document anchor or start point in world space.
 * @param to Cell anchor or target point in world space.
 * @param fromTangent Departure direction vector.
 * @param toTangent Arrival direction vector.
 * @param segments Number of linear subdivision segments (default 16).
 * @return segments + 1 points forming the 3D spline.
 */
[[nodiscard]] std::vector<glm::vec3> morphicRoute(const glm::vec3 &from,
                                                  const glm::vec3 &to,
                                                  const glm::vec3 &fromTangent,
                                                  const glm::vec3 &toTangent,
                                                  std::size_t segments = 16);

/**
 * @brief How far back a camera sits for a line of text @p lineHeightWorld
 *        tall to draw @p readablePx tall, on a screen @p screenHeightPx high
 *        seen through a vertical field of view of @p fovDegrees.
 *
 * The other half of framing: fitting everything is one answer to "how far
 * back", reading is another, and a reader asked to read wants this one. The
 * perspective relation is exact for text facing the camera at that distance.
 *
 * @return Nothing when any input could not describe a real view.
 */
[[nodiscard]] std::optional<float> readableCameraDistance(float lineHeightWorld,
                                                          float screenHeightPx,
                                                          float fovDegrees,
                                                          float readablePx);

/**
 * @brief A world-space rectangle fitted into a panel: uniform scale, centred,
 *        aspect kept -- the overview panel's one piece of geometry.
 *
 * Both spaces are y-up. A zero-sized world rectangle maps to the panel's
 * centre rather than dividing by zero.
 */
struct OverviewFit {
  glm::vec2 worldCentre{};
  glm::vec2 panelCentre{};
  /// Panel pixels per world unit.
  float scale{1.0F};

  [[nodiscard]] static OverviewFit fit(glm::vec2 worldMin, glm::vec2 worldMax,
                                       glm::vec2 panelMin, glm::vec2 panelSize);

  [[nodiscard]] glm::vec2 toPanel(const glm::vec2 world) const noexcept {
    return panelCentre + ((world - worldCentre) * scale);
  }
  [[nodiscard]] glm::vec2 toWorld(const glm::vec2 panel) const noexcept {
    return worldCentre + ((panel - panelCentre) / scale);
  }
};

} // namespace xanadu

#endif // XUDU_FRAMING_H

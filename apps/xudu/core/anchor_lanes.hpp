/**
 * @file anchor_lanes.hpp
 * @brief Sharing a document's margin between the link anchors that want it.
 *
 * A beam leaves a document at the margin beside the passage it is attached to,
 * and the mark left there -- the anchor -- is the only thing that says which
 * passage a beam belongs to once several of them leave the same edge. So two
 * anchors covering the same lines cannot both have the margin: one drawn over
 * the other says one link where there are two, and the colour that survives is
 * whichever happened to be drawn second.
 *
 * They are given lanes instead: slices of the margin's width, side by side,
 * each anchor keeping its whole vertical reach. Which lanes are needed is
 * exactly the colouring of an interval graph, and dividing the margin up is
 * arithmetic over one width -- neither needs a page, a glyph or a device,
 * which is why both are here rather than beside the drawing, for the reason
 * framing.hpp gives for the geometry it holds.
 *
 * The alternative -- dividing an anchor's own height between the links
 * crossing it -- was what this replaced, and it lies about the thing the
 * anchor exists to say: a link over four lines drawn a quarter as tall as the
 * passage it names looks like a link to one line.
 */
#ifndef XUDU_ANCHOR_LANES_H
#define XUDU_ANCHOR_LANES_H

#include <cstddef>
#include <vector>

namespace xudu {

/// The vertical reach of one link end at a document's margin, in world units.
/// @c top is the greater: an anchor is a range of lines, top edge of the first
/// to bottom edge of the last.
struct AnchorExtent {
  float top{};
  float bottom{};
};

/// Which slice of the margin one anchor was given, and how many the margin is
/// divided into where that anchor is. Both are wanted: the count is what makes
/// neighbouring anchors agree on how wide a slice is, so the lanes tile the
/// margin instead of each anchor sizing itself from its own crowd.
struct AnchorLane {
  int lane{};
  int lanes{1};
};

/// Most lanes a margin is divided into. Four distinct colours across a margin
/// is already at the limit of what reads as four rather than as a smear.
inline constexpr int marginLaneLimit = 4;

/**
 * @brief Hand out a lane to every anchor along one edge of one document.
 *
 * Anchors that do not overlap share a lane freely -- two links attached to
 * passages at opposite ends of a page both belong flush against the page
 * edge, and stepping the second one inwards would say they were competing for
 * the same lines when they are not. Anchors that do overlap never share one.
 *
 * @param extents The anchors, in the order lanes should be handed out in: the
 *        most prominent first, since lane 0 is the one flush with the page
 *        edge. The answer is in the same order.
 * @param laneLimit Most lanes to use. Anchors past it share the innermost
 *        lane: a fifth colour laid over a fourth is a smaller lie than a lane
 *        too thin to have a colour at all.
 * @return One entry per entry of @p extents. Every @c lanes within a
 *         connected group of overlapping anchors is the same, so their slices
 *         line up.
 */
[[nodiscard]] std::vector<AnchorLane>
assignAnchorLanes(const std::vector<AnchorExtent> &extents,
                  int laneLimit = marginLaneLimit);

/// Where one lane sits across a margin, as distances measured inwards from the
/// page edge. @c fromPageEdge is the nearer of the two.
struct MarginLane {
  float fromPageEdge{};
  float toPageEdge{};
};

/**
 * @brief The slice of a margin belonging to one lane.
 *
 * Lane 0 starts flush with the page edge and the last lane ends flush with the
 * text, so however many lanes there are they fill the margin exactly and
 * nothing overhangs the page -- which is the whole reason an anchor is drawn
 * in the margin rather than over the text or outside the paper.
 *
 * @param marginWidth Width of the margin in world units. Zero or less gives an
 *        empty slice.
 * @param kerf Gap left between neighbouring lanes, in the same units, so two
 *        colours meeting do not read as one. Trimmed to half a lane's width
 *        when it would otherwise leave nothing to draw, and never taken out of
 *        the two flush outer edges.
 */
[[nodiscard]] MarginLane marginLane(float marginWidth, int lane, int lanes,
                                    float kerf);

} // namespace xudu

#endif // XUDU_ANCHOR_LANES_H

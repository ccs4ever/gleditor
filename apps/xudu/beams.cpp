/**
 * @file beams.cpp
 * @brief Implementation of the drawn butterfly.
 */
#include "beams.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <iostream>
#include <map>
#include <utility>
#include <vector>

#include <glm/ext/vector_float4.hpp>
#include <glm/trigonometric.hpp>

#include <gleditor/animation.hpp>
#include <gleditor/caret.hpp>
#include <gleditor/draw_budget.hpp>
#include <gleditor/paths.hpp>
#include <gleditor/render/constants.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/spatial.hpp>

#include "common/xanadu/enfilade/spanfilade.hpp"
#include "xudu/core/anchor_lanes.hpp"
#include "xudu/core/framing.hpp"
#include "xudu/satelloid.hpp"
#include "xudu/tenuous_tether.hpp"

namespace xudu {

namespace {

/// Beam thickness as a fraction of the line height at the anchor. A beam is
/// meant to read as attached to a line of text rather than as a pipe running
/// between two buildings.
constexpr float beamWidthOfLine = 1.25F;

/// How near a document has to be to where it is being brought before moving it
/// is not worth the animation, in world units.
constexpr float alreadyAligned = 1.0F;

/// Half-width assumed for a document whose first page has not built yet, in
/// world units. It is the declared default page geometry, not an unrelated
/// guessed width; real Page measurements replace it as soon as they exist.
constexpr float fallbackDocHalfWidth = Doc::defaultPageWidthWorld() / 2.0F;

/// Height used during the same asynchronous first-page interval.
constexpr float fallbackDocHeight = Doc::defaultPageHeightWorld();

/// Aspect ratio assumed when the view has not reported a screen size yet.
constexpr float fallbackAspect = 4.0F / 3.0F;

/// Closest the camera is allowed to sit once auto-framed, in world units.
/// Without a floor, a scene with one small page would compute a zFit near
/// zero and put the camera inside the content it is trying to frame.
constexpr float minCameraDistance = 50.0F;

/// Farthest the camera is allowed to back up once auto-framed, in world
/// units. Every document sits at Z = 0 (see the docSlots loop below), and
/// the perspective projection's far clip plane is 10000 world units out
/// (src/renderer.cpp's glm::perspective call) -- a zFit past that would
/// back the camera up beyond where it can still see Z = 0 at all, clipping
/// every document out of the frustum rather than merely framing them
/// tightly. A tall enough envelope (many pages stacked, or several
/// documents at very different lengths) can compute a zFit past that
/// plane; clamping here trades a perfectly tight fit for the content
/// staying visible at all, which is the more useful failure mode.
constexpr float maxCameraDistance = 9500.0F;

/// Smallest world-space span the camera is framed to, in either axis. A
/// single small page would otherwise compute a span near zero and the
/// camera would zoom in absurdly tight.
constexpr float minFramingSpan = 10.0F;

/// Margin added around the computed bounding envelope before framing it, as
/// a fraction of the envelope's own size, floored at a minimum in world
/// units so a tiny envelope still gets breathing room.
constexpr float framingMarginFraction = 0.10F;
constexpr float framingMarginFloorX   = 4.0F;
constexpr float framingMarginFloorY   = 6.0F;

/// How near the camera has to be to where it is being taken before the ease is
/// treated as arrived, in world units. It is then put exactly there and handed
/// back; see the snap in drawFrame() for why exactly matters.
constexpr float cameraArrived = 0.05F;

} // namespace

LinkBeams::LinkBeams(Session &aSession, RendererRef aRenderer)
    : session(aSession), renderer(std::move(aRenderer)) {}

LinkBeams::~LinkBeams() = default;

void LinkBeams::deviceReady(render::RenderDevice &device,
                            const render::PipelineDesc &documentPipeline) {
  beams = std::make_unique<gleditor::Beams>(&device);
  // Depth tested, so a beam passing behind a page is behind it. A beam that
  // was always in front would be a beam nothing could be occluded by, and the
  // arrangement of the documents in space is what it is drawn to show.
  beams->createPipeline(gleditor::assetPath("shaders"),
                        documentPipeline.spirvDir, true);
}

void LinkBeams::rebuildStrands(RenderState &state) {
  const auto &views = session.views();
  std::vector<const Version *> versions;
  versions.reserve(std::min(views.size(), state.docs.size()));
  for (std::size_t i = 0; i < views.size() && i < state.docs.size(); i++) {
    versions.push_back(&views[i].pieces);
  }

  std::vector<LinkedPair> placed;
  std::vector<HalfLink> unplaced;
  if (!manifoldViews_.empty()) {
    UniversalViewContext uctx;
    uctx.docViews      = versions;
    uctx.manifoldViews = manifoldViews_;
    uctx.manifoldFoci  = manifoldFoci_;
    placeLinks(session.store().links(), uctx, placed, unplaced);
    strands.clear();
    strands.reserve(placed.size());
    for (const auto &one : placed) {
      strands.push_back(Strand{
          .link = one.link,
          .type = one.type,
          .tier = one.tier,
          .from = one.from,
          .to   = one.to,
      });
    }
    dangling.clear();
    dangling.reserve(unplaced.size());
    for (auto &one : unplaced) {
      dangling.push_back(Dangling{std::move(one), false});
    }

    std::vector<TransclusionPair> tPairs;
    // Spanfilade is the canonical interval index for shared primedia. Keep
    // discovery in the same path for document/cell contexts so beam staging
    // cannot diverge from the reference implementation.
    const auto spanfilade = xanadu::enfilade::Spanfilade::fromContext(uctx);
    spanfilade.placeTransclusions(uctx, tPairs);
    transclusionStrands.clear();
    transclusionStrands.reserve(tPairs.size());
    for (const auto &tp : tPairs) {
      transclusionStrands.push_back(TransclusionStrand{
          .from = tp.from,
          .to   = tp.to,
          .span = tp.span,
      });
    }
    if (beamConfig_.loomBundlingEnabled) {
      looms_ = detectTransclusionLooms(uctx, tPairs);
    } else {
      looms_.clear();
    }
  } else {
    looms_.clear();
    placeLinks(session.store().links(), versions, placed, unplaced);
    strands.clear();
    strands.reserve(placed.size());
    for (const auto &one : placed) {
      strands.push_back(Strand{
          .link = one.link,
          .type = one.type,
          .tier = one.tier,
          .from = one.from,
          .to   = one.to,
      });
    }
    dangling.clear();
    dangling.reserve(unplaced.size());
    for (auto &one : unplaced) {
      dangling.push_back(Dangling{std::move(one), false});
    }

    std::vector<TransclusionPair> tPairs;
    const auto spanfilade = xanadu::enfilade::Spanfilade::fromViews(versions);
    spanfilade.placeTransclusions(versions, tPairs);
    transclusionStrands.clear();
    transclusionStrands.reserve(tPairs.size());
    for (const auto &tp : tPairs) {
      transclusionStrands.push_back(TransclusionStrand{
          .from = tp.from,
          .to   = tp.to,
          .span = tp.span,
      });
    }
  }
}

void LinkBeams::resolveAnchors(RenderState &state) {
  const auto anchorIn =
      [&state, this](const LinkEnd &end,
                     std::uint32_t offset) -> std::optional<Doc::Anchor> {
    if (end.isCell() || end.doc >= state.docs.size()) {
      return std::nullopt;
    }
    // A link landing inside a media span's reserved placeholder range
    // anchors to that widget's own rectangle instead of a text-geometry
    // point on the blank filler line -- see MediaRectResolver's own comment.
    // Falls through to the ordinary text anchor for anything the resolver
    // does not recognise as media, which is every plain-text offset and
    // (until it is set) every build that predates this.
    if (mediaRectResolver && end.doc < session.views().size()) {
      const auto &view = session.views()[end.doc];
      if (auto rect = mediaRectResolver(*state.docs[end.doc], view.storeIndex,
                                        view.version, offset)) {
        return rect;
      }
    }
    return state.docs[end.doc]->anchorFor(offset);
  };

  for (auto &strand : strands) {
    if (strand.from.isCell()) {
      if (!strand.fromCellAnchor && cellAnchorResolver) {
        strand.fromCellAnchor = cellAnchorResolver(strand.from.cell());
      }
    } else {
      if (!strand.fromAnchor) {
        strand.fromAnchor = anchorIn(strand.from, strand.from.start);
      }
      if (!strand.fromEndAnchor) {
        const auto endOff    = (strand.from.end > strand.from.start)
                                   ? (strand.from.end - 1)
                                   : strand.from.start;
        strand.fromEndAnchor = anchorIn(strand.from, endOff);
      }
    }

    if (strand.to.isCell()) {
      if (!strand.toCellAnchor && cellAnchorResolver) {
        strand.toCellAnchor = cellAnchorResolver(strand.to.cell());
      }
    } else {
      if (!strand.toAnchor) {
        strand.toAnchor = anchorIn(strand.to, strand.to.start);
      }
      if (!strand.toEndAnchor) {
        const auto endOff  = (strand.to.end > strand.to.start)
                                 ? (strand.to.end - 1)
                                 : strand.to.start;
        strand.toEndAnchor = anchorIn(strand.to, endOff);
      }
    }
  }

  for (auto &tStrand : transclusionStrands) {
    if (tStrand.from.isCell()) {
      if (!tStrand.fromCellAnchor && cellAnchorResolver) {
        tStrand.fromCellAnchor = cellAnchorResolver(tStrand.from.cell());
      }
    } else {
      if (!tStrand.fromAnchor) {
        tStrand.fromAnchor = anchorIn(tStrand.from, tStrand.from.start);
      }
      if (!tStrand.fromEndAnchor) {
        const auto endOff     = (tStrand.from.end > tStrand.from.start)
                                    ? (tStrand.from.end - 1)
                                    : tStrand.from.start;
        tStrand.fromEndAnchor = anchorIn(tStrand.from, endOff);
      }
    }

    if (tStrand.to.isCell()) {
      if (!tStrand.toCellAnchor && cellAnchorResolver) {
        tStrand.toCellAnchor = cellAnchorResolver(tStrand.to.cell());
      }
    } else {
      if (!tStrand.toAnchor) {
        tStrand.toAnchor = anchorIn(tStrand.to, tStrand.to.start);
      }
      if (!tStrand.toEndAnchor) {
        const auto endOff   = (tStrand.to.end > tStrand.to.start)
                                  ? (tStrand.to.end - 1)
                                  : tStrand.to.start;
        tStrand.toEndAnchor = anchorIn(tStrand.to, endOff);
      }
    }
  }
}

std::optional<glm::vec3> LinkBeams::edgePoint(const Doc &doc,
                                              const Doc::Anchor &anchor,
                                              const bool towardsRight,
                                              const bool atTextBorder,
                                              const float yOffsetPixels) {
  const auto *const page = doc.page(anchor.pageIndex);
  if (nullptr == page) {
    return std::nullopt;
  }
  // Margin on the side the other document is on, asked of the page rather
  // than reconstructed from a copy of the layout's margin: a margin that
  // changed in the library and not here would put every anchor over the text
  // or off the paper, and nothing would say so.
  float edge = 0.0F;
  if (towardsRight) {
    edge = atTextBorder ? page->textRightPixels() : page->rightPixels();
  } else {
    edge = atTextBorder ? page->textLeftPixels() : page->leftPixels();
  }
  return doc.worldPoint(anchor.pageIndex, edge, anchor.y + yOffsetPixels);
}

std::optional<LinkBeams::Edge>
LinkBeams::edgeOf(const Doc &doc, const std::optional<Doc::Anchor> &startAnchor,
                  const std::optional<Doc::Anchor> &endAnchor,
                  const bool towardsRight) {
  if (!startAnchor) {
    return std::nullopt;
  }
  // Which of the two anchors is the upper one decides which line's top edge
  // the anchor starts at and which line's bottom edge it ends at, so the two
  // are settled before any point is asked for. The end anchor is where the
  // link's last byte fell, which on a link running over several lines is lower
  // down and on a link within one line is the same place. Either is usable;
  // only a page that has not built yet is not.
  const Doc::Anchor *upper = &*startAnchor;
  const Doc::Anchor *lower = &*startAnchor;
  if (endAnchor) {
    const auto first = edgePoint(doc, *startAnchor, towardsRight, false);
    const auto last  = edgePoint(doc, *endAnchor, towardsRight, false);
    if (first && last) {
      const bool startIsUpper = first->y >= last->y;
      upper                   = startIsUpper ? &*startAnchor : &*endAnchor;
      lower                   = startIsUpper ? &*endAnchor : &*startAnchor;
    }
  }

  // An anchor sits at the middle of its line -- Page::caretGeometry() returns
  // the caret's centre -- so the distance between the first and the last is a
  // line short of what the link covers, and for a link inside one line it is
  // nothing at all. Half a line at each end makes the anchor the passage's
  // own reach: the commonest link of all, attached within a single line, is
  // then one line tall rather than invisible. The half-line is asked for in
  // the page's pixels and converted by the page's own matrix, since that is
  // the only thing that knows what a pixel is worth in the world.
  const float upHalf   = upper->height * 0.5F;
  const float downHalf = -lower->height * 0.5F;

  const auto pageTop    = edgePoint(doc, *upper, towardsRight, false, upHalf);
  const auto pageBottom = edgePoint(doc, *lower, towardsRight, false, downHalf);
  const auto textTop    = edgePoint(doc, *upper, towardsRight, true, upHalf);
  const auto textBottom = edgePoint(doc, *lower, towardsRight, true, downHalf);
  if (!pageTop || !pageBottom || !textTop || !textBottom) {
    return std::nullopt;
  }
  return Edge{
      .top        = *pageTop,
      .bottom     = *pageBottom,
      .textTop    = *textTop,
      .textBottom = *textBottom,
      .lineHeight = std::max(upper->height, lower->height),
  };
}

std::optional<LinkBeams::Edge> LinkBeams::edgeOf(const CellAnchor &anchor,
                                                 const bool towardsRight) {
  const float halfW = anchor.width * 0.5F;
  const float halfH = anchor.height * 0.5F;
  const float x =
      towardsRight ? (anchor.position.x + halfW) : (anchor.position.x - halfW);
  const glm::vec3 top(x, anchor.position.y + halfH, anchor.position.z);
  const glm::vec3 bottom(x, anchor.position.y - halfH, anchor.position.z);
  return Edge{
      .top        = top,
      .bottom     = bottom,
      .textTop    = top,
      .textBottom = bottom,
      .lineHeight = anchor.lineHeight > 0.0F ? anchor.lineHeight : 16.0F,
  };
}

float LinkBeams::drawnHalfExtent(const Edge &edge, const float stubMinOfLine) {
  const float lineWorld = edge.lineHeight * Doc::pixelsToWorld;
  return std::max(std::abs(edge.top.y - edge.bottom.y) * 0.5F,
                  lineWorld * stubMinOfLine * 0.5F);
}

AnchorExtent LinkBeams::drawnExtent(const Edge &edge,
                                    const float stubMinOfLine) {
  const float middle = 0.5F * (edge.top.y + edge.bottom.y);
  const float half   = drawnHalfExtent(edge, stubMinOfLine);
  return AnchorExtent{.top = middle + half, .bottom = middle - half};
}

std::uint32_t LinkBeams::fade(const std::uint32_t colour, const float factor) {
  const auto alpha =
      static_cast<float>(colour & 0xFFU) * std::clamp(factor, 0.0F, 1.0F);
  return (colour & 0xFFFFFF00U) |
         static_cast<std::uint32_t>(std::lround(alpha));
}

void LinkBeams::band(const Edge &nearSide, const Edge &farSide,
                     const std::size_t documentsApart,
                     const std::uint32_t colour, const std::uint32_t tag,
                     const float phase) {
  const float baseWidth = std::max(nearSide.lineHeight, farSide.lineHeight) *
                          Doc::pixelsToWorld * beamWidthOfLine;
  const float nearSpan  = std::abs(nearSide.top.y - nearSide.bottom.y);
  const float farSpan   = std::abs(farSide.top.y - farSide.bottom.y);

  // How many strands comes from the taller of the two ends, so that end is
  // drawn at its full reach rather than reduced to whatever the other end
  // happens to be; see bandStrandCount() for why they are spaced rather than
  // packed. Each strand keeps the beam's own text-scaled weight. Towards the
  // shorter end they converge and, past the point where the spacing falls
  // below the width, overlap -- smoothly, because the spacing closes
  // monotonically, so the band gathers into a denser attachment rather than
  // breaking into stripes.
  const float wide = std::max(nearSpan, farSpan);
  const auto count =
      bandStrandCount(wide, baseWidth, beamConfig_.bandStrandPitch,
                      beamConfig_.bandStrandLimit);

  const float depth = std::max(
      beamConfig_.bypassDepthPerDoc *
          static_cast<float>(documentsApart > 1 ? documentsApart - 1 : 0),
      beamConfig_.bypassDepthLimit);

  // The outermost strand is pulled in by half a beam width at each end, so
  // the band fills the passage exactly instead of overhanging it by half a
  // ribbon at the top and the bottom -- which is where the anchor marking the
  // same passage ends, and a band that reached past its own anchor claimed a
  // line of text the link is not attached to.
  const auto within = [baseWidth](const Edge &edge) {
    const float span = std::abs(edge.top.y - edge.bottom.y);
    const float pull =
        span > 0.0F ? std::min(0.5F, (baseWidth * 0.5F) / span) : 0.0F;
    return std::pair{glm::mix(edge.top, edge.bottom, pull),
                     glm::mix(edge.bottom, edge.top, pull)};
  };
  const auto [nearTop, nearBottom] = within(nearSide);
  const auto [farTop, farBottom]   = within(farSide);

  const float zDiff     = std::abs(nearSide.top.z - farSide.top.z);
  const bool useMorphic = (zDiff >= 2.0F);

  for (std::size_t k = 0; k < count; k++) {
    const float where =
        count > 1 ? static_cast<float>(k) / static_cast<float>(count - 1)
                  : 0.5F;
    const auto p1 = glm::mix(nearTop, nearBottom, where);
    const auto p2 = glm::mix(farTop, farBottom, where);
    // The two that bound the band keep the link's own colour; the ones filling
    // it are dimmer, so the band reads as one relation with a reach rather
    // than as a fistful of separate ones.
    const auto strandColour = (0 == k || count - 1 == k)
                                  ? colour
                                  : fade(colour, beamConfig_.bandFillAlpha);

    if (useMorphic) {
      const glm::vec3 fromTan(p2.x >= p1.x ? 1.0F : -1.0F, 0.0F, 0.0F);
      const glm::vec3 toTan(p2.x >= p1.x ? 1.0F : -1.0F, 0.0F, 0.0F);
      const auto path =
          morphicRoute(p1, p2, fromTan, toTan, beamConfig_.bypassSegments);
      beams->addPath(path, baseWidth, strandColour, tag);
      continue;
    }

    if (documentsApart <= 1) {
      beams->add(p1, p2, baseWidth, strandColour, tag, 0.0F - phase,
                 1.0F - phase);
      continue;
    }
    // A document stands between these two, so the beam goes behind it rather
    // than through its text; see bypassRoute().
    beams->addPath(bypassRoute(p1, p2, depth, beamConfig_.bypassSegments),
                   baseWidth, strandColour, tag);
  }
}

void LinkBeams::drawMarginAnchorLane(const Edge &edge,
                                     const std::uint32_t colour,
                                     const std::uint32_t tag, const bool farEnd,
                                     const int laneIndex, const int laneCount,
                                     const bool isActive) {
  const float half   = drawnHalfExtent(edge, beamConfig_.stubMinOfLine);
  const auto pageMid = 0.5F * (edge.top + edge.bottom);
  const auto textMid = 0.5F * (edge.textTop + edge.textBottom);
  const float at     = farEnd ? 1.0F : 0.0F;

  // Which way the margin runs and how wide it is come from the two points the
  // edge already holds: one at the page's border and one at the text's. A page
  // narrow enough for those to coincide has no margin to draw in, and the
  // anchor is skipped rather than drawn somewhere invented.
  const glm::vec3 marginVec = pageMid - textMid;
  const float marginWidth   = glm::length(marginVec);
  if (!(marginWidth > 0.0F) || !(half > 0.0F)) {
    return;
  }
  const glm::vec3 marginDir = marginVec / marginWidth;

  const auto slice =
      marginLane(marginWidth, laneIndex, laneCount, beamConfig_.marginKerf);
  const float thickness = slice.toPageEdge - slice.fromPageEdge;
  if (!(thickness > 0.0F)) {
    return;
  }
  // Measured inwards from the page edge, which is where lane 0 sits: an
  // anchor is flush with the paper's border and grows towards the text, so a
  // margin holding one link and a margin holding four both start in the same
  // place and the outermost bar is always the most prominent one.
  const auto centre =
      pageMid - (marginDir * (0.5F * (slice.fromPageEdge + slice.toPageEdge)));
  const glm::vec3 up(0.0F, half, 0.0F);

  // The link being followed is drawn at full strength and the rest a shade
  // under it, which is the only difference between the lanes beyond their
  // colours -- widening the active one would break the tiling that keeps the
  // others where their neighbours expect them.
  const auto drawColour = isActive ? (colour | 0xFFU) : fade(colour, 0.92F);
  beams->add(centre - up, centre + up, thickness, drawColour, tag, at, at);
}

bool LinkBeams::onScreen(const glm::mat4 &viewProjection,
                         const glm::vec3 &point) {
  return gleditor::spatial::onScreen(viewProjection, point);
}

bool LinkBeams::ribbonMaybeOnScreen(const glm::mat4 &viewProjection,
                                    const glm::vec3 &a, const glm::vec3 &b,
                                    const float inflateWorld) {
  const glm::vec3 mid((a.x + b.x) * 0.5F, (a.y + b.y) * 0.5F,
                      (a.z + b.z) * 0.5F);
  const glm::vec3 halfExtent(std::abs(a.x - b.x) * 0.5F + inflateWorld,
                             std::abs(a.y - b.y) * 0.5F + inflateWorld,
                             std::abs(a.z - b.z) * 0.5F + inflateWorld);
  // outsideFrustum() wants its box's z corners at {0, depth} rather than
  // symmetric about the centre, so the model translation is shifted back by
  // halfExtent.z to make the box actually centred on mid.
  glm::mat4 model(1.0F);
  model[3] = glm::vec4(mid.x, mid.y, mid.z - halfExtent.z, 1.0F);
  return !outsideFrustum(viewProjection * model, halfExtent.x, halfExtent.y,
                         halfExtent.z * 2.0F);
}

void LinkBeams::recordFirstBeamCrossing(const gleditor::FrameContext &ctx,
                                        const Edge &nearEdge,
                                        const Edge &farEdge) {
  if (firstBeamCrossingRecorded_) {
    return;
  }
  // The ribbon's own centreline -- midway between each edge's top and
  // bottom -- rather than a corner, since that is what a reader actually
  // sees sweep across the screen. No inflation: unlike
  // updatePriorityOffsets()'s use of this same test, both edges are exact here
  // (this strand already resolved and is about to be drawn), so there is no
  // approximation to absorb.
  const glm::vec3 nearMid = (nearEdge.top + nearEdge.bottom) * 0.5F;
  const glm::vec3 farMid  = (farEdge.top + farEdge.bottom) * 0.5F;
  if (!ribbonMaybeOnScreen(ctx.viewProjection, nearMid, farMid, 0.0F)) {
    return;
  }
  firstBeamCrossingRecorded_ = true;
  const auto elapsed =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - ctx.state.loopStart)
          .count();
  std::cout << std::format(
      "[TIMING] First beam crossing viewport drawn: {:.2f} ms\n", elapsed);
}

void LinkBeams::updatePriorityOffsets(RenderState &state,
                                      const glm::mat4 &viewProjection) const {
  // "About a page height" is approximateAnchorFor()'s own contract for how
  // far its world point can be from the page's actual, unbuilt content --
  // see its doc comment on Doc.
  const float inflateWorld =
      (gleditor::letterPage.heightPx + Doc::pageGapPx) * Doc::pixelsToWorld;

  std::vector<std::vector<std::uint32_t>> perDocOffsets(state.docs.size());

  const auto approxWorldFor =
      [&state](const LinkEnd &end, const std::optional<Doc::Anchor> &exact,
               const std::optional<CellAnchor> &cellAnchor)
      -> std::optional<glm::vec3> {
    if (end.isCell()) {
      return cellAnchor ? std::optional(cellAnchor->position) : std::nullopt;
    }
    if (end.doc >= state.docs.size() || nullptr == state.docs[end.doc]) {
      return std::nullopt;
    }
    const auto &doc = *state.docs[end.doc];
    if (exact) {
      return doc.worldPoint(*exact);
    }
    const auto approx = doc.approximateAnchorFor(end.start);
    return approx ? doc.worldPoint(*approx) : std::nullopt;
  };

  // Shared between strands and transclusion strands: both carry the same
  // from/to/*Anchor/*CellAnchor fields, just on two different struct types.
  const auto considerStrand = [&](const auto &strand) {
    const bool fromNeeds = strand.from.isDocument() && !strand.fromAnchor;
    const bool toNeeds   = strand.to.isDocument() && !strand.toAnchor;
    if (!fromNeeds && !toNeeds) {
      // Both ends already resolved (or neither is a document end) -- nothing
      // this strand could ask a document to prioritise.
      return;
    }

    const auto fromPos =
        approxWorldFor(strand.from, strand.fromAnchor, strand.fromCellAnchor);
    const auto toPos =
        approxWorldFor(strand.to, strand.toAnchor, strand.toCellAnchor);

    if (fromPos && toPos) {
      if (!ribbonMaybeOnScreen(viewProjection, *fromPos, *toPos,
                               inflateWorld)) {
        return;
      }
    } else if (!((strand.to.isCell() && !toPos && fromPos) ||
                 (strand.from.isCell() && !fromPos && toPos))) {
      // Neither a ribbon to test nor the cell-not-placed-yet fallback below
      // applies -- nothing resolvable to say yet (e.g. this document has not
      // shaped anything at all).
      return;
    }
    // A cell end that has not been placed reaches here with only the
    // document end resolved: see design/priority-page-building.md's "Cell
    // endpoints" section. There is no ribbon to test, so the strand is
    // treated as possibly crossing and its document end is pushed anyway.

    if (fromNeeds) {
      perDocOffsets[strand.from.doc].push_back(strand.from.start);
    }
    if (toNeeds) {
      perDocOffsets[strand.to.doc].push_back(strand.to.start);
    }
  };

  for (const auto &strand : strands) {
    considerStrand(strand);
  }
  for (const auto &tStrand : transclusionStrands) {
    considerStrand(tStrand);
  }

  for (std::size_t d = 0; d < state.docs.size(); ++d) {
    if (state.docs[d]) {
      state.docs[d]->setPriorityOffsets(perDocOffsets[d]);
    }
  }
}

void LinkBeams::align(const Strand &strand, RenderState &state,
                      ch::Timeline &timeline) {
  if (!strand.fromAnchor || !strand.toAnchor) {
    return;
  }
  alignPair(strand.from.doc, strand.to.doc, *strand.fromAnchor,
            *strand.toAnchor, strand.fromEndAnchor, strand.toEndAnchor,
            strand.link, state, timeline);
}

void LinkBeams::alignTransclusion(const TransclusionStrand &tStrand,
                                  RenderState &state, ch::Timeline &timeline) {
  if (!tStrand.fromAnchor || !tStrand.toAnchor) {
    return;
  }
  alignPair(tStrand.from.doc, tStrand.to.doc, *tStrand.fromAnchor,
            *tStrand.toAnchor, tStrand.fromEndAnchor, tStrand.toEndAnchor,
            std::nullopt, state, timeline);
}

void LinkBeams::alignPair(std::size_t fromDocIdx, std::size_t toDocIdx,
                          const Doc::Anchor &fromAnchor,
                          const Doc::Anchor &toAnchor,
                          const std::optional<Doc::Anchor> &fromEndAnchor,
                          const std::optional<Doc::Anchor> &toEndAnchor,
                          std::optional<std::uint64_t> linkId,
                          RenderState &state, ch::Timeline &timeline) {
  if (fromDocIdx >= state.docs.size() || toDocIdx >= state.docs.size()) {
    return;
  }
  const auto &near = state.docs[fromDocIdx];
  const auto &far  = state.docs[toDocIdx];

  // Everything below is worked out against where the documents are going
  // rather than where they are. A document part way through an earlier move
  // is not at its resting place, and levelling against where it happens to
  // have got to makes the answer depend on which frame this ran on: a scene
  // with several links settled somewhere slightly different every time it was
  // opened, because the aligns landed a different number of frames apart.
  // getModel() is the destination animateMoveTo() recorded as it started, so a
  // run of alignments composes to one arrangement however they are spread out.
  const auto atRest = [](const Doc &doc, const glm::vec3 &point) {
    return point + glm::vec3(doc.getModel()[3]) -
           glm::vec3(doc.modelMatrix()[3]);
  };
  const auto restingPoint =
      [&atRest](const Doc &doc,
                const Doc::Anchor &anchor) -> std::optional<glm::vec3> {
    const auto point = doc.worldPoint(anchor);
    return point ? std::optional{atRest(doc, *point)} : std::nullopt;
  };

  const auto nearAt = restingPoint(*near, fromAnchor);
  const auto farAt  = restingPoint(*far, toAnchor);
  if (!nearAt || !farAt) {
    return;
  }

  const glm::vec3 nearPos(near->getModel()[3]);
  const glm::vec3 farPos(far->getModel()[3]);

  const auto *const nearPage = near->page(fromAnchor.pageIndex);
  const auto *const farPage  = far->page(toAnchor.pageIndex);
  float nearHalfWidth        = fallbackDocHalfWidth;
  float farHalfWidth         = fallbackDocHalfWidth;
  if (nullptr != nearPage && nullptr != farPage) {
    nearHalfWidth = (nearPage->widthPixels() * 0.5F) * Doc::pixelsToWorld;
    farHalfWidth  = (farPage->widthPixels() * 0.5F) * Doc::pixelsToWorld;
  }

  // Gather all strands belonging to the same link connecting this document pair
  // to compute the vertical centroid of the connected span ranges across all
  // pages.
  float nearMinY = nearAt->y;
  float nearMaxY = nearAt->y;
  float farMinY  = farAt->y;
  float farMaxY  = farAt->y;

  const auto reach = [&restingPoint](const Doc &doc,
                                     const std::optional<Doc::Anchor> &anchor,
                                     float &lowest, float &highest) {
    if (!anchor) {
      return;
    }
    if (const auto pt = restingPoint(doc, *anchor)) {
      lowest  = std::min(lowest, pt->y);
      highest = std::max(highest, pt->y);
    }
  };

  reach(*near, fromEndAnchor, nearMinY, nearMaxY);
  reach(*far, toEndAnchor, farMinY, farMaxY);

  for (auto &s : strands) {
    if ((linkId && s.link == *linkId) ||
        (s.from.doc == fromDocIdx && s.to.doc == toDocIdx)) {
      s.aligned = true;
      reach(*near, s.fromAnchor, nearMinY, nearMaxY);
      reach(*near, s.fromEndAnchor, nearMinY, nearMaxY);
      reach(*far, s.toAnchor, farMinY, farMaxY);
      reach(*far, s.toEndAnchor, farMinY, farMaxY);
    }
  }

  for (auto &ts : transclusionStrands) {
    if (ts.from.doc == fromDocIdx && ts.to.doc == toDocIdx) {
      ts.aligned = true;
      reach(*near, ts.fromAnchor, nearMinY, nearMaxY);
      reach(*near, ts.fromEndAnchor, nearMinY, nearMaxY);
      reach(*far, ts.toAnchor, farMinY, farMaxY);
      reach(*far, ts.toEndAnchor, farMinY, farMaxY);
    }
  }

  const float nearCenterY = 0.5F * (nearMinY + nearMaxY);
  const float farCenterY  = 0.5F * (farMinY + farMaxY);
  const float deltaY      = nearCenterY - farCenterY;

  const auto farDocIdx = toDocIdx;

  // Whether document d sits on the background plane rather than the
  // foreground row: opened via --background (RenderItemOpenDoc::depthZ),
  // and not the document this call is itself bringing forward. farDocIdx
  // is deliberately excluded even when its current Z is still negative --
  // it is mid-transition to Z = 0 below, and treating it as background
  // here would leave docSlots with no entry for the very document that
  // needs one.
  const auto isBackground = [&](const std::size_t d) {
    return d != farDocIdx && glm::vec3(state.docs[d]->getModel()[3]).z < 0.0F;
  };

  // Sequential non-overlapping horizontal layout of the foreground row.
  // Background documents (a corpus shown for context, not brought into the
  // row) do not consume a slot and are not repositioned by it -- see the
  // two loops below that both skip them the same way.
  std::vector<float> docSlots(state.docs.size(), 0.0F);
  float currX           = 0.0F;
  bool anyForegroundYet = false;
  for (std::size_t d = 0; d < state.docs.size(); ++d) {
    if (isBackground(d)) {
      docSlots[d] = glm::vec3(state.docs[d]->getModel()[3]).x;
      continue;
    }
    float halfW = fallbackDocHalfWidth;
    if (const auto *p = state.docs[d]->page(0)) {
      halfW = (p->widthPixels() * 0.5F) * Doc::pixelsToWorld;
    }
    if (anyForegroundYet) {
      float prevHalfW = fallbackDocHalfWidth;
      for (std::size_t prev = d; prev-- > 0;) {
        if (isBackground(prev)) {
          continue;
        }
        if (const auto *pPrev = state.docs[prev]->page(0)) {
          prevHalfW = (pPrev->widthPixels() * 0.5F) * Doc::pixelsToWorld;
        }
        break;
      }
      currX += prevHalfW + halfW + render::kDefaultDocumentGap;
    }
    anyForegroundYet = true;
    docSlots[d]      = currX;
  }

  // Update physical tension layout bodies and constraints
  tensionEngine_.clear();
  for (std::size_t d = 0; d < state.docs.size(); ++d) {
    if (!state.docs[d]) {
      continue;
    }
    TensionBody body;
    body.docIndex = d;
    const glm::vec3 cur(state.docs[d]->getModel()[3]);
    body.position        = cur;
    body.restingPosition = cur;
    float halfW          = fallbackDocHalfWidth;
    float heightW        = fallbackDocHeight;
    if (const auto *p = state.docs[d]->page(0)) {
      halfW   = (p->widthPixels() * 0.5F) * Doc::pixelsToWorld;
      heightW = p->heightPixels() * Doc::pixelsToWorld;
    }
    body.width        = halfW * 2.0F;
    body.height       = heightW;
    body.isForeground = !isBackground(d);
    body.isFlying     = (d == farDocIdx && farPos.z < -5.0F);
    body.pinned       = (d == fromDocIdx);
    tensionEngine_.setBody(body);
  }

  TensionConstraint constraint;
  constraint.fromDoc     = fromDocIdx;
  constraint.toDoc       = toDocIdx;
  constraint.nearAnchorY = nearCenterY - nearPos.y;
  constraint.farAnchorY  = farCenterY - farPos.y;
  constraint.targetGap   = render::kDefaultDocumentGap;
  constraint.prominence  = 1.0F;
  constraint.active      = true;
  tensionEngine_.addConstraint(constraint);

  if (physicsEnabled_) {
    constexpr float dt = 0.016F;
    for (int step = 0; step < 25; ++step) {
      tensionEngine_.step(dt);
    }
    for (std::size_t d = 0; d < state.docs.size(); ++d) {
      if (const auto *b = tensionEngine_.findBody(d)) {
        if (!isBackground(d)) {
          docSlots[d] = b->position.x;
        }
      }
    }
  }

  const glm::vec3 target{docSlots[farDocIdx], farPos.y + deltaY, 0.0F};

  // Register tenuous parent tether if document flew from background plane
  if (tetherOverlay_ != nullptr && farPos.z < -5.0F) {
    FlyingTetherAnchor anchor;
    anchor.docIndex   = farDocIdx;
    anchor.originPos  = farPos;
    anchor.currentPos = target;
    anchor.width      = farHalfWidth * 2.0F;
    anchor.height     = farPage ? (farPage->heightPixels() * Doc::pixelsToWorld)
                                : fallbackDocHeight;
    anchor.colour     = 0x38BDF855; // Ethereal cyan with ~33% alpha
    anchor.active     = true;
    tetherOverlay_->setTether(anchor);
  }

  if (glm::distance(target, farPos) >= alreadyAligned) {
    if (linkId) {
      std::cout << "xudu: link " << *linkId << " aligns centroid of doc "
                << toDocIdx << " with doc " << fromDocIdx << "\n";
    } else {
      std::cout << "xudu: transclusion aligns centroid of doc " << toDocIdx
                << " with doc " << fromDocIdx << "\n";
    }
    // The document being brought over is the subject of the move, so it is the
    // one that takes longest and starts first. Everything else in this
    // function is timed against it; see gleditor::anim::sworphSubject.
    far->animateMoveTo(timeline, target, gleditor::anim::sworphSubject);
  }

  for (std::size_t d = 0; d < state.docs.size(); ++d) {
    if (d == farDocIdx || isBackground(d)) {
      continue;
    }
    const glm::vec3 cur(state.docs[d]->getModel()[3]);
    if (std::abs(cur.x - docSlots[d]) >= alreadyAligned) {
      // Held a moment, then shorter than the document it is making room for,
      // so the row reads as answering the arrival rather than as every
      // document on screen sliding at once -- which is what giving them all
      // the same timing looked like, and what made a sworph read as the whole
      // view jumping.
      state.docs[d]->animateMoveTo(
          timeline, glm::vec3(docSlots[d], cur.y, cur.z),
          gleditor::anim::sworphRow, gleditor::anim::sworphRowDelay);
    }
  }

  // Dynamic Camera Auto-Framing: calculate world bounding envelope across
  // every page of every foreground document -- deliberately not the
  // background ones, which are shown for context rather than being what
  // the camera is asked to fit; a large corpus sitting behind everything
  // would otherwise force the same wide, distant framing whether or not
  // any of it is actually what is meant to be read right now.
  if (renderer && renderer->appState()) {
    float minX = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float minY = std::numeric_limits<float>::max();
    float maxY = std::numeric_limits<float>::lowest();

    for (std::size_t d = 0; d < state.docs.size(); ++d) {
      const auto &doc = state.docs[d];
      if (!doc || isBackground(d)) {
        continue;
      }
      const glm::vec3 curPos(state.docs[d]->getModel()[3]);
      const glm::vec3 docOffset =
          (d == farDocIdx) ? (target - curPos)
                           : glm::vec3(docSlots[d] - curPos.x, 0.0F, 0.0F);

      for (std::size_t p = 0;; ++p) {
        const auto *page = doc->page(p);
        if (!page) {
          break;
        }
        const float halfW = page->widthPixels() * 0.5F;
        const float halfH = page->heightPixels() * 0.5F;

        const auto corner = [&](const float pageX, const float pageY) {
          const auto point =
              doc->worldPoint(static_cast<std::uint32_t>(p), pageX, pageY);
          if (!point) {
            return;
          }
          const glm::vec3 pt = atRest(*doc, *point) + docOffset;
          minX               = std::min(minX, pt.x);
          maxX               = std::max(maxX, pt.x);
          minY               = std::min(minY, pt.y);
          maxY               = std::max(maxY, pt.y);
        };
        corner(-halfW, halfH);
        corner(halfW, -halfH);
      }
    }

    // Also encompass all beam anchors and connecting geometry
    minX = std::min({minX, nearPos.x - nearHalfWidth, target.x - farHalfWidth});
    maxX = std::max({maxX, nearPos.x + nearHalfWidth, target.x + farHalfWidth});
    minY = std::min({minY, nearMinY, farMinY + deltaY});
    maxY = std::max({maxY, nearMaxY, farMaxY + deltaY});

    // Framing margins so documents and beams never clip the screen edges: a
    // fraction of the envelope's own size, floored at a minimum for a small
    // envelope that would otherwise get almost no margin at all.
    const float marginX =
        std::max((maxX - minX) * framingMarginFraction, framingMarginFloorX);
    const float marginY =
        std::max((maxY - minY) * framingMarginFraction, framingMarginFloorY);
    minX -= marginX;
    maxX += marginX;
    minY -= marginY;
    maxY += marginY;

    const float spanW = std::max(maxX - minX, minFramingSpan);
    const float spanH = std::max(maxY - minY, minFramingSpan);
    const glm::vec3 center(0.5F * (minX + maxX), 0.5F * (minY + maxY), 0.0F);

    std::scoped_lock locker(renderer->appState()->view);
    auto &view             = renderer->appState()->view;
    const float aspect     = (view.screenHeight > 0 && view.screenWidth > 0)
                                 ? static_cast<float>(view.screenWidth) /
                                       static_cast<float>(view.screenHeight)
                                 : fallbackAspect;
    const float fovRad     = glm::radians(view.fov);
    const float tanHalfFov = std::tan(fovRad * 0.5F);
    const float zFit       = std::max(spanH / (2.0F * tanHalfFov),
                                      spanW / (2.0F * aspect * tanHalfFov));

    const glm::vec3 newCameraPos(
        center.x, center.y,
        std::clamp(zFit, minCameraDistance, maxCameraDistance));
    // Seeded from where the camera actually is, then ramped like a document
    // move (Doc::animateMoveTo): a direct assignment here would make the
    // camera jump to every new framing target instantly while every document
    // in the same scene glides there, which reads as the whole view snapping
    // rather than settling. Re-seeded whenever an ease is not already
    // running, so a framing that follows a pan starts from where the reader
    // left the camera rather than from wherever the last one ended.
    if (!cameraDriving) {
      cameraTarget  = view.pos;
      cameraDriving = true;
    }
    cameraGoal = newCameraPos;
    // Last to move and slowest of the three, so the frame closes around an
    // arrangement that has already been made rather than chasing one still
    // being made.
    timeline.apply(&cameraTarget)
        .then<ch::Hold>(cameraTarget(), gleditor::anim::cameraSettleDelay)
        .then<ch::RampTo>(newCameraPos, gleditor::anim::cameraSettle,
                          ch::EaseInOutQuad());
  }
}

void LinkBeams::sworphCameraTo(const glm::vec3 &targetPos,
                               ch::Timeline &timeline) {
  if (!renderer || !renderer->appState()) {
    return;
  }
  {
    std::scoped_lock locker(renderer->appState()->view);
    if (!cameraDriving) {
      cameraTarget  = renderer->appState()->view.pos;
      cameraDriving = true;
    }
  }
  cameraGoal = targetPos;
  timeline.apply(&cameraTarget)
      .then<ch::RampTo>(targetPos, gleditor::anim::cameraSettle,
                        ch::EaseInOutQuad());
}

void LinkBeams::alignCellSatelloid(const Strand &strand, RenderState &state) {
  const bool docAtFrom = strand.from.isDocument();
  const auto docIdx    = docAtFrom ? strand.from.doc : strand.to.doc;
  const auto cellRef   = docAtFrom ? strand.to.cell() : strand.from.cell();
  const auto &cellAnch =
      docAtFrom ? strand.toCellAnchor : strand.fromCellAnchor;
  const auto &docAnch = docAtFrom ? strand.fromAnchor : strand.toAnchor;

  if (docIdx >= state.docs.size() || !cellAnch || !docAnch) {
    return;
  }

  const auto &doc = state.docs[docIdx];
  if (!doc) {
    return;
  }

  const auto anchorWorld =
      doc->worldPoint(docAnch->pageIndex, docAnch->x, docAnch->y);
  if (!anchorWorld) {
    return;
  }

  const glm::vec3 docPos(doc->getModel()[3]);
  const float docHalfW = (doc->page(0) ? (doc->page(0)->widthPixels() * 0.5F)
                                       : fallbackDocHalfWidth) *
                         Doc::pixelsToWorld;

  // 1. Setup / update TensionBody for cell in tension layout engine
  TensionBody cellBody;
  cellBody.targetId        = static_cast<std::size_t>(cellRef);
  cellBody.targetKind      = LinkTargetKind::ZigzagCell;
  cellBody.restingPosition = cellAnch->position;
  cellBody.position        = cellAnch->position;
  cellBody.width           = 24.0F;
  cellBody.height          = 14.0F;
  cellBody.isForeground    = true;
  cellBody.isFlying        = true;
  cellBody.mass            = tensionEngine_.params().satelloidMass;
  cellBody.pinned          = false;

  if (const auto *existing = tensionEngine_.findCellBody(cellRef)) {
    cellBody.position = existing->position;
    cellBody.velocity = existing->velocity;
  }

  tensionEngine_.setBody(cellBody);

  // Document body (pinned as reference)
  TensionBody docBody;
  docBody.docIndex        = docIdx;
  docBody.targetKind      = LinkTargetKind::Document;
  docBody.position        = docPos;
  docBody.restingPosition = docPos;
  docBody.width           = docHalfW * 2.0F;
  docBody.height = doc->page(0)
                       ? (doc->page(0)->heightPixels() * Doc::pixelsToWorld)
                       : fallbackDocHeight;
  docBody.isForeground = true;
  docBody.pinned       = true;
  tensionEngine_.setBody(docBody);

  // 2. Add collinear alignment constraint
  TensionConstraint constraint;
  constraint.fromDoc     = docIdx;
  constraint.fromKind    = LinkTargetKind::Document;
  constraint.toTarget    = static_cast<std::size_t>(cellRef);
  constraint.toKind      = LinkTargetKind::ZigzagCell;
  constraint.nearAnchorY = anchorWorld->y - docPos.y;
  constraint.farAnchorY  = 0.0F;
  constraint.targetGap   = tensionEngine_.params().satelloidGap;
  constraint.prominence  = 1.0F;
  constraint.active      = true;
  tensionEngine_.addConstraint(constraint);

  // 3. Step physics simulation if enabled
  if (physicsEnabled_) {
    constexpr float dt = 0.016F;
    for (int step = 0; step < 20; ++step) {
      tensionEngine_.step(dt);
    }
  }

  const auto *solved = tensionEngine_.findCellBody(cellRef);
  const glm::vec3 solvedPos =
      solved ? solved->position
             : glm::vec3(docPos.x + docHalfW +
                             tensionEngine_.params().satelloidGap,
                         anchorWorld->y, 0.0F);

  // 4. Update SatelloidOverlay
  if (satelloidOverlay_ != nullptr) {
    CellSatelloid sat;
    sat.cellRef    = cellRef;
    sat.originPos  = cellAnch->position;
    sat.currentPos = solvedPos;
    sat.targetPos =
        glm::vec3(docPos.x + docHalfW + tensionEngine_.params().satelloidGap,
                  anchorWorld->y, 0.0F);
    sat.width       = 24.0F;
    sat.height      = 14.0F;
    sat.dimName     = "d.sequence";
    sat.accentColor = 0x38BDF8FF; // Cyan
    sat.alpha       = 1.0F;
    sat.active      = true;
    satelloidOverlay_->setSatelloid(sat);
  }

  // 5. Register tenuous parent tether in TenuousTetherOverlay
  if (tetherOverlay_ != nullptr && cellAnch->position.z < -5.0F) {
    FlyingTetherAnchor tether;
    tether.targetId   = static_cast<std::size_t>(cellRef);
    tether.targetKind = LinkTargetKind::ZigzagCell;
    tether.cellRef    = cellRef;
    tether.originPos  = cellAnch->position;
    tether.currentPos = solvedPos;
    tether.width      = 24.0F;
    tether.height     = 14.0F;
    tether.colour     = 0x38BDF844;
    tether.active     = true;
    tetherOverlay_->setTether(tether);
  }
}

bool LinkBeams::danglingOutstanding(const RenderState &state) const {
  // Exactly the ones openDangling() will act on -- the same test it makes. A
  // half-link it would skip on every frame is not work outstanding, and
  // counting one would leave the render loop waiting for a frame that has
  // nothing left to do: a run that never settles and never quits.
  return nullptr != opener &&
         std::ranges::any_of(dangling, [&state](const Dangling &one) {
           return !one.looked && one.link.here.isDocument() &&
                  one.link.here.doc < state.docs.size();
         });
}

bool LinkBeams::openDangling(RenderState &state) {
  if (!opener) {
    return false;
  }
  // Every version already on screen, so that the search does not offer back a
  // document that is open -- which for a link whose ends are both quoted from
  // one state is otherwise the first thing it would find.
  std::vector<MicroversionId> open;
  open.reserve(session.views().size());
  for (const auto &view : session.views()) {
    open.push_back(view.version);
  }

  for (auto &waiting : dangling) {
    if (waiting.looked || !waiting.link.here.isDocument() ||
        waiting.link.here.doc >= state.docs.size()) {
      continue;
    }
    // Once per link: the search rebuilds states until one matches, which is
    // not something to do again next frame if it found nothing.
    waiting.looked     = true;
    const auto showing = session.versionShowing(waiting.link.elsewhere, open);
    if (!showing) {
      continue;
    }
    std::cout << "xudu: link " << waiting.link.link << " reaches "
              << showing->str() << ", opening it\n";
    opener(*showing);
    // One a frame. Opening a document is a load and a page build, and the
    // strands are worked out again when it lands, which is when the next one
    // can be judged -- so there is more to come.
    return true;
  }
  // Every half-link that could be looked for has been.
  return false;
}

void LinkBeams::traverse(const Strand &strand, RenderState &state) {
  auto *const caret = renderer->editCaret();
  if (nullptr == caret) {
    return;
  }
  // The far end is whichever one the caret is not already in. Following a link
  // from the end you are at is the useful direction, and it is the only one
  // the reader can have meant.
  const bool atFrom = caret->active() && strand.from.isDocument() &&
                      caret->documentIndex() == strand.from.doc;
  const auto &there = atFrom ? strand.to : strand.from;
  if (there.isCell()) {
    std::cout << "xudu: follow link " << strand.link << " to cell #"
              << there.cell() << "\n";
    if (satelloidOverlay_ != nullptr) {
      satelloidOverlay_->triggerPulse(there.cell());
    }
    alignCellSatelloid(strand, state);
    return;
  }
  if (there.doc >= state.docs.size()) {
    return;
  }
  caret->placeAt(there.doc, there.start);
  std::cout << "xudu: follow link " << strand.link << " to doc " << there.doc
            << " [" << there.start << "," << there.end << ")\n";
}

bool LinkBeams::picked(const render::PickingResult &pick, RenderState &state) {
  if (render::tagKindBeam != pick.tag.kind) {
    return false;
  }
  // Ours whatever happens next: a beam was clicked, and the click must not
  // fall through to the page behind it.
  if (pick.tag.clusterIndex >= strands.size()) {
    const auto tIndex = pick.tag.clusterIndex - strands.size();
    if (tIndex < transclusionStrands.size()) {
      hoveredTransclusion_                = tIndex;
      transclusionStrands[tIndex].aligned = false;
    }
    return true;
  }
  const auto &strand = strands[pick.tag.clusterIndex];
  traverse(strand, state);
  // Following a link is a request to see both ends of it, which is the one
  // case where the far document is moved whether or not the sworph is on.
  strands[pick.tag.clusterIndex].aligned = false;
  return true;
}

void LinkBeams::drawFrame(gleditor::FrameContext &ctx) {
  session.tick();
  if (nullptr == beams || !visible) {
    // Nothing will be drawn and so nothing will be moved. Saying so rather
    // than leaving the flag where it was matters: --no-beams would otherwise
    // leave the render loop waiting forever for a sworph that is never going
    // to happen.
    unsettled = false;
    return;
  }
  auto &state = ctx.state;

  // Carries the camera towards wherever align() last framed, every frame --
  // not only the frame align() itself ran on. cameraTarget is a Choreograph
  // output the timeline eases on its own schedule; this is what reads back
  // whatever step the ease has reached and puts it where the camera actually
  // looks. Before the first alignment cameraDriving is false and this is a
  // no-op, so a scene nobody has sworphed in never moves the camera -- and it
  // goes false again the moment the ease arrives, which is what gives the
  // camera back to whoever is at the keyboard.
  if (cameraDriving && renderer && renderer->appState()) {
    // Snapped to the goal on the last step rather than left wherever the ease
    // had reached when it came within the threshold. The camera is where a
    // capture is taken from, and stopping a fraction of a world unit short of
    // the same place every time -- a different fraction depending on how the
    // frames happened to fall -- moves every glyph on screen by a fraction of
    // a pixel and makes two runs of the same scene different pictures.
    const bool arrived =
        glm::distance(cameraTarget(), cameraGoal) <= cameraArrived;
    const auto now = arrived ? cameraGoal : cameraTarget();
    {
      const std::scoped_lock locker(renderer->appState()->view);
      renderer->appState()->view.pos = now;
    }
    cameraDriving = !arrived;
  }

  // A strand no longer waits for every open document to finish before it may
  // draw -- resolveAnchors() below already tolerates an anchor whose page is
  // not built yet (Doc::anchorFor() answers nullopt for one), and the
  // per-strand fromValid/toValid checks that follow already skip drawing and
  // aligning a strand until its own two ends are ready. What still needs
  // deciding here is only whether *settling* should wait -- see
  // anyStrandStillLoading below.

  bool docTransformsChanged = false;
  if (lastDocTransforms.size() != state.docs.size() ||
      lastDocOpacities.size() != state.docs.size()) {
    docTransformsChanged = true;
    lastDocTransforms.resize(state.docs.size(), glm::mat4(0.0F));
    lastDocOpacities.resize(state.docs.size(), -1.0F);
  }
  for (std::size_t i = 0; i < state.docs.size(); i++) {
    if (state.docs[i]) {
      const auto curMat = state.docs[i]->modelMatrix();
      const auto curOp  = state.docs[i]->currentOpacity();
      if (curMat != lastDocTransforms[i] || curOp != lastDocOpacities[i]) {
        docTransformsChanged = true;
        lastDocTransforms[i] = curMat;
        lastDocOpacities[i]  = curOp;
      }
    }
  }

  const bool topologyChanged =
      (session.generation() != builtFor || state.docs.size() != builtDocs);
  if (topologyChanged) {
    builtFor  = session.generation();
    builtDocs = state.docs.size();
    rebuildStrands(state);
    strandsRebuilt = true;
  }

  // Clicks arriving through an assistive technology are queued rather than
  // executed on the spot: they come from another thread, and following a link
  // moves the caret and might open a document.
  {
    std::vector<std::uint64_t> asked;
    {
      const std::scoped_lock locker(askedGuard);
      asked.swap(askedToFollow);
    }
    for (const auto link : asked) {
      const auto found = std::ranges::find(strands, link, &Strand::link);
      if (strands.end() != found) {
        traverse(*found, state);
      }
    }
  }

  // Advance pulse phase every frame for live photonic traveling wave packets
  pulsePhase = std::fmod(pulsePhase + 0.02F, 1.0F);

  if (docTransformsChanged || topologyChanged || strandsRebuilt || unsettled ||
      !strands.empty() || !transclusionStrands.empty()) {
    resolveAnchors(state);
    updatePriorityOffsets(state, ctx.viewProjection);

    beams->clear();
    bool moved        = false;
    bool stillToAlign = false;

    // A strand's document endpoint may still be waiting on a page its
    // document has not built yet -- worth reserving a frame for, since it
    // will resolve once that page lands. An endpoint whose document *is*
    // fully loaded and still has no anchor, or a cell endpoint (whose
    // readiness this class cannot observe the way page-build completion is
    // observed -- see design/priority-page-building.md's "Cell endpoints"
    // section), never holds up settling: counting one would leave the render
    // loop waiting for a frame that has nothing left to do, the same hazard
    // danglingOutstanding()'s own comment describes for half-links.
    bool anyStrandStillLoading      = false;
    const auto endpointStillLoading = [&state](const LinkEnd &end) {
      return end.isDocument() && end.doc < state.docs.size() &&
             nullptr != state.docs[end.doc] &&
             !state.docs[end.doc]->isFullyLoaded();
    };

    struct MarginAnchor {
      Edge edge;
      std::uint32_t colour{};
      std::uint32_t tagId{};
      bool farEnd{};
      bool isActive{};
      std::size_t docIndex{};
      bool towardsRight{};
      std::uint64_t linkId{};
      ProminenceTier tier{};
      LinkType type{};
      /// Whether this anchor belongs to an emergent transclusion rather than
      /// to a link. A transclusion has no link id to be told apart by, so
      /// without this every transclusion along one margin would answer to the
      /// same one and be joined into a single multi-span link that nobody
      /// made.
      bool transclusion{};
    };

    std::vector<MarginAnchor> allAnchors;
    allAnchors.reserve((strands.size() + transclusionStrands.size()) * 2);

    for (std::size_t i = 0; i < strands.size(); i++) {
      auto &strand = strands[i];

      const bool fromValid =
          strand.from.isCell()
              ? strand.fromCellAnchor.has_value()
              : (strand.fromAnchor.has_value() &&
                 strand.from.doc < state.docs.size() &&
                 state.docs[strand.from.doc]->currentOpacity() > 0.001F);
      const bool toValid =
          strand.to.isCell()
              ? strand.toCellAnchor.has_value()
              : (strand.toAnchor.has_value() &&
                 strand.to.doc < state.docs.size() &&
                 state.docs[strand.to.doc]->currentOpacity() > 0.001F);

      if (!fromValid || !toValid) {
        if ((!strand.fromAnchor && endpointStillLoading(strand.from)) ||
            (!strand.toAnchor && endpointStillLoading(strand.to))) {
          anyStrandStillLoading = true;
        }
        continue;
      }

      const glm::vec3 fromPos =
          strand.from.isCell()
              ? strand.fromCellAnchor->position
              : glm::vec3(state.docs[strand.from.doc]->getModel()[3]);
      const glm::vec3 toPos =
          strand.to.isCell()
              ? strand.toCellAnchor->position
              : glm::vec3(state.docs[strand.to.doc]->getModel()[3]);

      const bool rightwards = toPos.x >= fromPos.x;
      const auto nearEdge =
          strand.from.isCell()
              ? edgeOf(*strand.fromCellAnchor, rightwards)
              : edgeOf(*state.docs[strand.from.doc], strand.fromAnchor,
                       strand.fromEndAnchor, rightwards);
      const auto farEdge =
          strand.to.isCell()
              ? edgeOf(*strand.toCellAnchor, !rightwards)
              : edgeOf(*state.docs[strand.to.doc], strand.toAnchor,
                       strand.toEndAnchor, !rightwards);
      if (!nearEdge || !farEdge) {
        continue;
      }

      const std::size_t docSpan =
          (strand.from.isDocument() && strand.to.isDocument())
              ? (strand.from.doc > strand.to.doc
                     ? (strand.from.doc - strand.to.doc)
                     : (strand.to.doc - strand.from.doc))
              : 1;

      const float fromOpacity =
          strand.from.isCell() ? 1.0F
                               : state.docs[strand.from.doc]->currentOpacity();
      const float toOpacity = strand.to.isCell()
                                  ? 1.0F
                                  : state.docs[strand.to.doc]->currentOpacity();
      const auto docAlpha   = std::min(fromOpacity, toOpacity);
      const auto colour     = fade(
          linkColourWithInstanceShift(strand.link, strand.type, strand.tier),
          docAlpha);
      const auto tagId = static_cast<std::uint32_t>(i);
      const bool isAct = (activeLink && *activeLink == strand.link);
      const float linkPhase =
          std::fmod(pulsePhase + linkPhaseOffset(strand.link), 1.0F);

      band(*nearEdge, *farEdge, docSpan, colour, tagId, linkPhase);
      recordFirstBeamCrossing(ctx, *nearEdge, *farEdge);

      if (strand.from.isDocument()) {
        const std::uint32_t marginCol =
            strand.to.isCell() ? 0x38BDF8FF : colour;
        allAnchors.push_back(MarginAnchor{
            .edge         = *nearEdge,
            .colour       = marginCol,
            .tagId        = tagId,
            .farEnd       = false,
            .isActive     = isAct,
            .docIndex     = strand.from.doc,
            .towardsRight = rightwards,
            .linkId       = strand.link,
            .tier         = strand.tier,
            .type         = strand.type,
        });
      }

      if (strand.to.isDocument()) {
        const std::uint32_t marginCol =
            strand.from.isCell() ? 0x38BDF8FF : colour;
        allAnchors.push_back(MarginAnchor{
            .edge         = *farEdge,
            .colour       = marginCol,
            .tagId        = tagId,
            .farEnd       = true,
            .isActive     = isAct,
            .docIndex     = strand.to.doc,
            .towardsRight = !rightwards,
            .linkId       = strand.link,
            .tier         = strand.tier,
            .type         = strand.type,
        });

        // Tenuous connection: subtle elastic tether ribbon connecting flying
        // page to background origin
        const auto &toDoc = state.docs[strand.to.doc];
        if (toDoc && glm::vec3(toDoc->getModel()[3]).z < 0.0F) {
          const glm::vec3 originPos(0.0F, glm::vec3(toDoc->getModel()[3]).y,
                                    -60.0F);
          const glm::vec3 currentPos(toDoc->getModel()[3]);
          const auto tetherCol =
              fade(linkColour(strand.type, ProminenceTier::Public),
                   0.22F * docAlpha);
          beams->add(originPos, currentPos, 1.6F, tetherCol, tagId, 0.0F, 1.0F);
        }
      }

      if (sworph && !strand.aligned) {
        if (strand.from.isDocument() && strand.to.isDocument()) {
          if (moved) {
            stillToAlign = true;
          } else {
            strand.aligned = true;
            align(strand, state, ctx.timeline);
            moved = true;
          }
        } else if (strand.from.isCell() || strand.to.isCell()) {
          strand.aligned = true;
          alignCellSatelloid(strand, state);
        }
      }
    }

    // Render pure transclusion beams (solid continuous identity prisms between
    // identical primedia spans across documents)
    for (std::size_t i = 0; i < transclusionStrands.size(); i++) {
      auto &tStrand = transclusionStrands[i];

      const bool fromValid =
          tStrand.from.isCell()
              ? tStrand.fromCellAnchor.has_value()
              : (tStrand.fromAnchor.has_value() &&
                 tStrand.from.doc < state.docs.size() &&
                 state.docs[tStrand.from.doc]->currentOpacity() > 0.001F);
      const bool toValid =
          tStrand.to.isCell()
              ? tStrand.toCellAnchor.has_value()
              : (tStrand.toAnchor.has_value() &&
                 tStrand.to.doc < state.docs.size() &&
                 state.docs[tStrand.to.doc]->currentOpacity() > 0.001F);

      if (!fromValid || !toValid) {
        if ((!tStrand.fromAnchor && endpointStillLoading(tStrand.from)) ||
            (!tStrand.toAnchor && endpointStillLoading(tStrand.to))) {
          anyStrandStillLoading = true;
        }
        continue;
      }

      const glm::vec3 fromPos =
          tStrand.from.isCell()
              ? tStrand.fromCellAnchor->position
              : glm::vec3(state.docs[tStrand.from.doc]->getModel()[3]);
      const glm::vec3 toPos =
          tStrand.to.isCell()
              ? tStrand.toCellAnchor->position
              : glm::vec3(state.docs[tStrand.to.doc]->getModel()[3]);

      const bool rightwards = toPos.x >= fromPos.x;
      const auto nearEdge =
          tStrand.from.isCell()
              ? edgeOf(*tStrand.fromCellAnchor, rightwards)
              : edgeOf(*state.docs[tStrand.from.doc], tStrand.fromAnchor,
                       tStrand.fromEndAnchor, rightwards);
      const auto farEdge =
          tStrand.to.isCell()
              ? edgeOf(*tStrand.toCellAnchor, !rightwards)
              : edgeOf(*state.docs[tStrand.to.doc], tStrand.toAnchor,
                       tStrand.toEndAnchor, !rightwards);
      if (!nearEdge || !farEdge) {
        continue;
      }

      const std::size_t docSpan =
          (tStrand.from.isDocument() && tStrand.to.isDocument())
              ? (tStrand.from.doc > tStrand.to.doc
                     ? (tStrand.from.doc - tStrand.to.doc)
                     : (tStrand.to.doc - tStrand.from.doc))
              : 1;

      const float fromOpacity =
          tStrand.from.isCell()
              ? 1.0F
              : state.docs[tStrand.from.doc]->currentOpacity();
      const float toOpacity =
          tStrand.to.isCell() ? 1.0F
                              : state.docs[tStrand.to.doc]->currentOpacity();
      const auto docAlpha = std::min(fromOpacity, toOpacity);

      // Check if transcluded span is withheld or transcopyright-locked
      std::uint32_t baseBeamColour = 0xFFD700FFU; // Default Identity Gold
      float phase                  = 0.0F;

      std::optional<ResolveResult> res;
      if (tStrand.from.isDocument() &&
          tStrand.from.doc < session.views().size()) {
        const auto sIdx = session.storeIndexOf(tStrand.from.doc);
        res             = session.store(sIdx).resolve(tStrand.span);
      } else if (tStrand.to.isDocument() &&
                 tStrand.to.doc < session.views().size()) {
        const auto sIdx = session.storeIndexOf(tStrand.to.doc);
        res             = session.store(sIdx).resolve(tStrand.span);
      } else if (session.storeCount() > 0) {
        res = session.store().resolve(tStrand.span);
      }

      if (res.has_value()) {
        if (res->isWithheld()) {
          baseBeamColour = 0x1F2937FFU; // Obsidian Redaction Beam
        } else if (res->isLocked()) {
          baseBeamColour = 0xF59E0BFFU; // Transcopyright Amber Gold Beam
          phase          = pulsePhase;  // Active photonic energy pulse
        }
      }

      bool inLoom = false;
      if (beamConfig_.loomBundlingEnabled) {
        for (const auto &loom : looms_) {
          if (std::find(loom.strandIndices.begin(), loom.strandIndices.end(),
                        i) != loom.strandIndices.end()) {
            inLoom = true;
            break;
          }
        }
      }

      float alphaFactor = docAlpha;
      if (inLoom) {
        const bool isHovered =
            (hoveredTransclusion_ && *hoveredTransclusion_ == i);
        alphaFactor *=
            (isHovered ? beamConfig_.loomHoverAlpha : beamConfig_.loomAlpha);
      }

      const auto colour = fade(baseBeamColour, alphaFactor);
      const auto tagId  = static_cast<std::uint32_t>(strands.size() + i);

      // Transclusion beams are solid, continuous volumetric identity bands
      band(*nearEdge, *farEdge, docSpan, colour, tagId, phase);
      recordFirstBeamCrossing(ctx, *nearEdge, *farEdge);

      if (tStrand.from.isDocument()) {
        allAnchors.push_back(MarginAnchor{
            .edge         = *nearEdge,
            .colour       = colour,
            .tagId        = tagId,
            .farEnd       = false,
            .isActive     = false,
            .docIndex     = tStrand.from.doc,
            .towardsRight = rightwards,
            .linkId       = 0,
            .tier         = ProminenceTier::Author,
            .type         = LinkType::Other,
            .transclusion = true,
        });
      }

      if (tStrand.to.isDocument()) {
        allAnchors.push_back(MarginAnchor{
            .edge         = *farEdge,
            .colour       = colour,
            .tagId        = tagId,
            .farEnd       = true,
            .isActive     = false,
            .docIndex     = tStrand.to.doc,
            .towardsRight = !rightwards,
            .linkId       = 0,
            .tier         = ProminenceTier::Author,
            .type         = LinkType::Other,
            .transclusion = true,
        });
      }

      if (sworph && !tStrand.aligned && tStrand.from.isDocument() &&
          tStrand.to.isDocument()) {
        if (moved) {
          stillToAlign = true;
        } else {
          tStrand.aligned = true;
          alignTransclusion(tStrand, state, ctx.timeline);
          moved = true;
        }
      }
    }

    // Every anchor along one edge of one document shares that edge's margin.
    // Which of them can have it flush against the paper, and how the rest are
    // stepped inwards from there, is decided per edge rather than per anchor:
    // two anchors that overlap must not be handed the same lane, and two that
    // do not overlap must not be pushed apart for nothing.
    for (std::size_t d = 0; d < state.docs.size(); ++d) {
      for (const bool towardsRight : {true, false}) {
        std::vector<std::size_t> sideAnchors;
        for (std::size_t a = 0; a < allAnchors.size(); ++a) {
          if (allAnchors[a].docIndex == d &&
              allAnchors[a].towardsRight == towardsRight) {
            sideAnchors.push_back(a);
          }
        }
        if (sideAnchors.empty()) {
          continue;
        }

        // The order lanes are handed out in, most prominent first: the link
        // being followed, then the tier that vouched for it, then the link's
        // own id and its place among the anchors. The last two carry no
        // meaning beyond being the same from one frame to the next -- without
        // them two anchors of equal standing would swap lanes whenever the
        // strands were found in a different order, and the margin would
        // flicker between two arrangements that are both correct.
        std::ranges::sort(
            sideAnchors, [&allAnchors](std::size_t a, std::size_t b) {
              const auto &oa = allAnchors[a];
              const auto &ob = allAnchors[b];
              if (oa.isActive != ob.isActive) {
                return oa.isActive;
              }
              if (oa.tier != ob.tier) {
                return static_cast<int>(oa.tier) < static_cast<int>(ob.tier);
              }
              if (oa.linkId != ob.linkId) {
                return oa.linkId < ob.linkId;
              }
              return a < b;
            });

        std::vector<AnchorExtent> extents;
        extents.reserve(sideAnchors.size());
        for (const auto which : sideAnchors) {
          extents.push_back(
              drawnExtent(allAnchors[which].edge, beamConfig_.stubMinOfLine));
        }
        const auto lanes = assignAnchorLanes(extents, marginLaneLimit);

        for (std::size_t k = 0; k < sideAnchors.size(); ++k) {
          const auto &anchor = allAnchors[sideAnchors[k]];
          drawMarginAnchorLane(anchor.edge, anchor.colour, anchor.tagId,
                               anchor.farEnd, lanes[k].lane, lanes[k].lanes,
                               anchor.isActive);
        }

        // Multi-span links: if a single link has several disjoint anchor spans
        // on the same document edge, a vertical spine connects them, so the
        // margin says they are one relation rather than several. Keyed on
        // which end of the link an anchor is as well as on the link, since a
        // link with both of its ends on one edge is two attachments and not
        // one span in two pieces -- and transclusions, which have no link id,
        // are left out rather than all answering to zero.
        std::map<std::pair<std::uint64_t, bool>, std::vector<std::size_t>>
            linkGroups;
        for (const auto which : sideAnchors) {
          const auto &anchor = allAnchors[which];
          if (anchor.transclusion) {
            continue;
          }
          linkGroups[{anchor.linkId, anchor.farEnd}].push_back(which);
        }

        for (auto &[key, group] : linkGroups) {
          if (group.size() <= 1) {
            continue;
          }
          std::ranges::sort(group, [&allAnchors](std::size_t a, std::size_t b) {
            return allAnchors[a].edge.top.y > allAnchors[b].edge.top.y;
          });

          for (std::size_t g = 0; g + 1 < group.size(); ++g) {
            const auto &upper = allAnchors[group[g]];
            const auto &lower = allAnchors[group[g + 1]];
            const auto pTop   = upper.edge.bottom;
            const auto pBot   = lower.edge.top;
            if (pTop.y > pBot.y + 0.05F) {
              const float spineWidth =
                  std::max(0.12F, upper.edge.lineHeight * Doc::pixelsToWorld *
                                      beamWidthOfLine *
                                      beamConfig_.stubWidthOfBeam * 0.45F);
              const auto spineColour = upper.isActive
                                           ? (upper.colour | 0xFFU)
                                           : fade(upper.colour, 0.85F);
              const float at         = upper.farEnd ? 1.0F : 0.0F;
              beams->add(pBot, pTop, spineWidth, spineColour, upper.tagId, at,
                         at);
            }
          }
        }
      }
    }

    unsettled = stillToAlign || anyStrandStillLoading;

    beams->commit();
    strandsRebuilt = false;
  }

  // Document and page zero, with no kind: the kind is the beam pipeline's,
  // and which beam it is rides in the beam's own tag.
  beams->draw(state, ctx.viewProjection, 1.0F,
              render::packTagIdentity(0, 0, 0));
}

void LinkBeams::describe(gleditor::a11y::Builder &into) {
  namespace a11y = gleditor::a11y;
  if (strands.empty()) {
    return;
  }

  auto &group = into.add(0, a11y::Role::List);
  group.label = "links between the open documents";
  for (std::size_t which = 0; which < strands.size(); which++) {
    group.children.push_back(into.id(strands[which].link + 1));
  }
  into.contribute(into.id(0));

  for (std::size_t which = 0; which < strands.size(); which++) {
    const auto &strand = strands[which];
    // Numbered by the link rather than by its position, so that a link keeps
    // its identity as others are found and lost around it -- and so that what
    // comes back names a link this can look up. Link ids are small sequential
    // counters from the store, well inside the forty-eight bits a node id
    // leaves for them.
    auto &node = into.add(strand.link + 1, a11y::Role::Link);
    // Named by what it connects rather than by what it looks like. A beam is
    // a coloured line and its colour is its type, which is exactly the kind
    // of thing that has to be said in words for anybody who is not looking at
    // it.
    node.label = std::string{linkTypeName(strand.type)} + " link, document " +
                 std::to_string(strand.from.doc) + " to document " +
                 std::to_string(strand.to.doc);
    node.description = "bytes " + std::to_string(strand.from.start) + " to " +
                       std::to_string(strand.from.end) + ", and bytes " +
                       std::to_string(strand.to.start) + " to " +
                       std::to_string(strand.to.end);
    node.focusable   = true;
    node.actions =
        a11y::bit(a11y::Action::Focus) | a11y::bit(a11y::Action::Click);
  }
}

std::uint64_t LinkBeams::accessibilityRevision() const {
  // Not the strand count alone: two links can be replaced by two others
  // without the count moving. `described` is bumped wherever they are found
  // again.
  return described;
}

bool LinkBeams::performAction(const std::uint64_t nodeId,
                              const gleditor::a11y::Action action,
                              const std::string_view /*value*/) {
  namespace a11y = gleditor::a11y;
  if (a11y::Action::Click != action) {
    // Focus alone moves nothing: a beam is not somewhere the caret can be,
    // and claiming to have focused it would be a lie an assistive technology
    // acts on.
    return false;
  }
  const auto link = a11y::Ids::localOf(nodeId);
  if (0 == link) {
    return false;
  }
  // Under no lock and against nothing: the strand list is the render
  // thread's, so what is recorded is the link's own identity and the lookup
  // happens there. A link that has gone by then is simply not found.
  const std::scoped_lock locker(askedGuard);
  askedToFollow.push_back(link - 1);
  return true;
}

} // namespace xudu

/**
 * @file beams.cpp
 * @brief Implementation of the drawn butterfly.
 */
#include "beams.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <utility>

#include <glm/ext/vector_float4.hpp>
#include <glm/trigonometric.hpp>

#include <gleditor/animation.hpp>
#include <gleditor/caret.hpp>
#include <gleditor/paths.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/spatial.hpp>

#include "xudu/core/framing.hpp"

namespace xudu {

namespace {

/// Beam thickness as a fraction of the line height at the anchor. A beam is
/// meant to read as attached to a line of text rather than as a pipe running
/// between two buildings.
constexpr float beamWidthOfLine = 0.45F;

/// How near a document has to be to where it is being brought before moving it
/// is not worth the animation, in world units.
constexpr float alreadyAligned = 1.0F;

/// Space left between two documents brought alongside each other, in world
/// units. Enough for the beam to be a beam and not a join.
constexpr float documentGap = 6.0F;

/// Half-width assumed for a document whose first page has not built yet, in
/// world units -- just enough that layout math has something to work with
/// before the real width is known.
constexpr float fallbackDocHalfWidth = 10.0F;

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

/// Most strands one band is drawn with. A link between two whole pages would
/// otherwise ask for one strand per line of text at both ends, which is a
/// hundred ribbons saying what five say just as well.
constexpr std::size_t bandStrandLimit = 5;

/// Space between a band's strands, in beam widths. Comfortably more than one,
/// so the strands stay clear of each other where the band is at its tallest:
/// ribbons that abut only just overlap, and a strip of doubled alpha down
/// every seam prints as stripes running the length of the band. Separated,
/// they read as what they are -- a few threads spanning the passage -- and
/// the only place they gather is the end where the link is attached to less
/// text, which is where the eye should be going anyway.
constexpr float bandStrandPitch = 2.2F;

/// Alpha the strands inside a band are drawn at, relative to the two that
/// bound it. The edges are what say how far the passage reaches; the fill says
/// the space between them is one relation and not several.
constexpr float bandFillAlpha = 0.55F;

/// Width of the bracket drawn down the margin at each end of a link, relative
/// to the beam's own width.
constexpr float stubWidthOfBeam = 1.35F;

/// Shortest bracket drawn at a link end, as a fraction of the line height
/// there. A link attached to a single line has no vertical extent to draw, and
/// a bracket of no length is not drawn at all -- the vertex stage collapses a
/// beam whose ends coincide.
constexpr float stubMinOfLine = 0.9F;

/// How far behind the page plane a beam dips to pass a document standing
/// between its two ends, per document passed, and the deepest it may go. Deep
/// enough to clear the pages, shallow enough that the dip still reads as the
/// same beam rather than as something disappearing off the back of the scene.
constexpr float bypassDepthPerDoc = -20.0F;
constexpr float bypassDepthLimit  = -120.0F;

/// Points the bypass curve is drawn with. Enough that the dip reads as a curve
/// rather than as a beam broken into three pieces at two corners -- which is
/// what it was, and what left the middle piece running nearly straight away
/// from the camera where a ribbon in the page plane has almost no width to be
/// seen edge-on.
constexpr std::size_t bypassSegments = 9;

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
  placeLinks(session.store().links(), versions, placed, unplaced);

  strands.clear();
  strands.reserve(placed.size());
  for (const auto &one : placed) {
    strands.push_back(Strand{
        one.link, one.type, one.tier, one.from, one.to, {}, {}, {}, {}, false});
  }
  dangling.clear();
  dangling.reserve(unplaced.size());
  for (auto &one : unplaced) {
    dangling.push_back(Dangling{std::move(one), false});
  }
}

void LinkBeams::resolveAnchors(RenderState &state) {
  for (auto &strand : strands) {
    const auto anchorIn =
        [&state](const LinkEnd &end,
                 std::uint32_t offset) -> std::optional<Doc::Anchor> {
      if (end.doc >= state.docs.size()) {
        return std::nullopt;
      }
      return state.docs[end.doc]->anchorFor(offset);
    };
    // Retried until it answers rather than given up on: a document opened this
    // frame has no pages yet, and the link is no less real for that.
    if (!strand.fromAnchor) {
      strand.fromAnchor = anchorIn(strand.from, strand.from.start);
    }
    if (!strand.toAnchor) {
      strand.toAnchor = anchorIn(strand.to, strand.to.start);
    }
    if (!strand.fromEndAnchor) {
      const auto endOff    = (strand.from.end > strand.from.start)
                                 ? (strand.from.end - 1)
                                 : strand.from.start;
      strand.fromEndAnchor = anchorIn(strand.from, endOff);
    }
    if (!strand.toEndAnchor) {
      const auto endOff  = (strand.to.end > strand.to.start)
                               ? (strand.to.end - 1)
                               : strand.to.start;
      strand.toEndAnchor = anchorIn(strand.to, endOff);
    }
  }
}

std::optional<glm::vec3> LinkBeams::edgePoint(const Doc &doc,
                                              const Doc::Anchor &anchor,
                                              const bool towardsRight) {
  const auto *const page = doc.page(anchor.pageIndex);
  if (nullptr == page) {
    return std::nullopt;
  }
  // The margin on the side the other document is on, level with the anchor.
  // Leaving from the text itself would run the beam back across the page it
  // came from before it got anywhere.
  const auto edge = towardsRight ? page->leftPixels() + page->widthPixels()
                                 : page->leftPixels();
  return doc.worldPoint(anchor.pageIndex, edge, anchor.y);
}

std::optional<LinkBeams::Edge>
LinkBeams::edgeOf(const Doc &doc, const std::optional<Doc::Anchor> &startAnchor,
                  const std::optional<Doc::Anchor> &endAnchor,
                  const bool towardsRight) {
  if (!startAnchor) {
    return std::nullopt;
  }
  const auto first = edgePoint(doc, *startAnchor, towardsRight);
  if (!first) {
    return std::nullopt;
  }
  Edge out{*first, *first, startAnchor->height};
  // The end anchor is where the link's last byte fell, which on a link running
  // over several lines is lower down and on a link within one line is the same
  // place. Either is a usable edge; only a page that has not built yet is not.
  if (endAnchor) {
    if (const auto last = edgePoint(doc, *endAnchor, towardsRight)) {
      out.top        = first->y >= last->y ? *first : *last;
      out.bottom     = first->y >= last->y ? *last : *first;
      out.lineHeight = std::max(startAnchor->height, endAnchor->height);
    }
  }
  return out;
}

std::uint32_t LinkBeams::fade(const std::uint32_t colour, const float factor) {
  const auto alpha =
      static_cast<float>(colour & 0xFFU) * std::clamp(factor, 0.0F, 1.0F);
  return (colour & 0xFFFFFF00U) |
         static_cast<std::uint32_t>(std::lround(alpha));
}

void LinkBeams::band(const Edge &nearSide, const Edge &farSide,
                     const std::size_t documentsApart,
                     const std::uint32_t colour, const std::uint32_t tag) {
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
      bandStrandCount(wide, baseWidth, bandStrandPitch, bandStrandLimit);

  const float depth = std::max(
      bypassDepthPerDoc *
          static_cast<float>(documentsApart > 1 ? documentsApart - 1 : 0),
      bypassDepthLimit);

  for (std::size_t k = 0; k < count; k++) {
    const float where =
        count > 1 ? static_cast<float>(k) / static_cast<float>(count - 1)
                  : 0.5F;
    const auto p1 = glm::mix(nearSide.top, nearSide.bottom, where);
    const auto p2 = glm::mix(farSide.top, farSide.bottom, where);
    // The two that bound the band keep the link's own colour; the ones filling
    // it are dimmer, so the band reads as one relation with a reach rather
    // than as a fistful of separate ones.
    const auto strandColour =
        (0 == k || count - 1 == k) ? colour : fade(colour, bandFillAlpha);

    if (documentsApart <= 1) {
      beams->add(p1, p2, baseWidth, strandColour, tag);
      continue;
    }
    // A document stands between these two, so the beam goes behind it rather
    // than through its text; see bypassRoute().
    beams->addPath(bypassRoute(p1, p2, depth, bypassSegments), baseWidth,
                   strandColour, tag);
  }
}

void LinkBeams::anchorStub(const Edge &edge, const std::uint32_t colour,
                           const std::uint32_t tag, const bool farEnd) {
  const float lineWorld = edge.lineHeight * Doc::pixelsToWorld;
  const float half      = std::max(std::abs(edge.top.y - edge.bottom.y) * 0.5F,
                                   lineWorld * stubMinOfLine * 0.5F);
  const auto middle     = 0.5F * (edge.top + edge.bottom);
  const glm::vec3 up(0.0F, half, 0.0F);
  // Held at one end of the route rather than spanning it, so the bracket is as
  // bright as the beam is where it meets it: the fade along a beam's length is
  // there to say which way the link points, and a bracket that faded down its
  // own height would say something about the page instead.
  const float at = farEnd ? 1.0F : 0.0F;
  beams->add(middle - up, middle + up,
             lineWorld * beamWidthOfLine * stubWidthOfBeam, colour, tag, at,
             at);
}

bool LinkBeams::onScreen(const glm::mat4 &viewProjection,
                         const glm::vec3 &point) {
  return gleditor::spatial::onScreen(viewProjection, point);
}

void LinkBeams::align(const Strand &strand, RenderState &state,
                      ch::Timeline &timeline) {
  if (strand.from.doc >= state.docs.size() ||
      strand.to.doc >= state.docs.size() || !strand.fromAnchor ||
      !strand.toAnchor) {
    return;
  }
  const auto &near = state.docs[strand.from.doc];
  const auto &far  = state.docs[strand.to.doc];

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

  const auto nearAt = restingPoint(*near, *strand.fromAnchor);
  const auto farAt  = restingPoint(*far, *strand.toAnchor);
  if (!nearAt || !farAt) {
    return;
  }

  const glm::vec3 nearPos(near->getModel()[3]);
  const glm::vec3 farPos(far->getModel()[3]);

  const auto *const nearPage = near->page(strand.fromAnchor->pageIndex);
  const auto *const farPage  = far->page(strand.toAnchor->pageIndex);
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

  reach(*near, strand.fromEndAnchor, nearMinY, nearMaxY);
  reach(*far, strand.toEndAnchor, farMinY, farMaxY);

  for (auto &s : strands) {
    if (s.link == strand.link && s.from.doc == strand.from.doc &&
        s.to.doc == strand.to.doc) {
      s.aligned = true;
      reach(*near, s.fromAnchor, nearMinY, nearMaxY);
      reach(*near, s.fromEndAnchor, nearMinY, nearMaxY);
      reach(*far, s.toAnchor, farMinY, farMaxY);
      reach(*far, s.toEndAnchor, farMinY, farMaxY);
    }
  }

  const float nearCenterY = 0.5F * (nearMinY + nearMaxY);
  const float farCenterY  = 0.5F * (farMinY + farMaxY);
  const float deltaY      = nearCenterY - farCenterY;

  const auto farDocIdx = strand.to.doc;

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
      currX += prevHalfW + halfW + documentGap;
    }
    anyForegroundYet = true;
    docSlots[d]      = currX;
  }

  const glm::vec3 target{docSlots[farDocIdx], farPos.y + deltaY, 0.0F};

  if (glm::distance(target, farPos) >= alreadyAligned) {
    std::cout << "xudu: link " << strand.link << " aligns centroid of doc "
              << strand.to.doc << " with doc " << strand.from.doc << "\n";
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

bool LinkBeams::danglingOutstanding(const RenderState &state) const {
  // Exactly the ones openDangling() will act on -- the same test it makes. A
  // half-link it would skip on every frame is not work outstanding, and
  // counting one would leave the render loop waiting for a frame that has
  // nothing left to do: a run that never settles and never quits.
  return nullptr != opener &&
         std::ranges::any_of(dangling, [&state](const Dangling &one) {
           return !one.looked && one.link.here.doc < state.docs.size();
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
    if (waiting.looked || waiting.link.here.doc >= state.docs.size()) {
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
  const bool atFrom =
      caret->active() && caret->documentIndex() == strand.from.doc;
  const auto &there = atFrom ? strand.to : strand.from;
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

  // Wait until all open documents have completed building their pages before
  // resolving anchors and performing centroid alignment and auto-framing.
  for (const auto &doc : state.docs) {
    if (!doc || !doc->isFullyLoaded()) {
      // Still building, so nothing has been placed yet and every link is still
      // owed a look. Saying so is what reserves the frame on which the last
      // page lands: that frame is the first on which a sworph can be decided
      // and, with nothing else left pending by then, the one the render loop
      // would otherwise have called settled and quit on.
      unsettled = true;
      return;
    }
  }

  bool docTransformsChanged = false;
  if (lastDocTransforms.size() != state.docs.size()) {
    docTransformsChanged = true;
    lastDocTransforms.resize(state.docs.size());
  }
  for (std::size_t i = 0; i < state.docs.size(); i++) {
    if (state.docs[i]) {
      const auto &curMat = state.docs[i]->getModel();
      if (curMat != lastDocTransforms[i]) {
        docTransformsChanged = true;
        lastDocTransforms[i] = curMat;
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
  if (docTransformsChanged || topologyChanged || strandsRebuilt || unsettled) {
    resolveAnchors(state);

    beams->clear();
    bool moved        = false;
    bool stillToAlign = false;

    for (std::size_t i = 0; i < strands.size(); i++) {
      auto &strand = strands[i];
      if (!strand.fromAnchor || !strand.toAnchor ||
          strand.from.doc >= state.docs.size() ||
          strand.to.doc >= state.docs.size()) {
        continue;
      }
      const auto &from = state.docs[strand.from.doc];
      const auto &to   = state.docs[strand.to.doc];

      const auto rightwards =
          glm::vec3(to->getModel()[3]).x >= glm::vec3(from->getModel()[3]).x;
      const auto nearEdge =
          edgeOf(*from, strand.fromAnchor, strand.fromEndAnchor, rightwards);
      const auto farEdge =
          edgeOf(*to, strand.toAnchor, strand.toEndAnchor, !rightwards);
      if (!nearEdge || !farEdge) {
        continue;
      }

      const std::size_t docSpan = strand.from.doc > strand.to.doc
                                      ? (strand.from.doc - strand.to.doc)
                                      : (strand.to.doc - strand.from.doc);

      const auto docAlpha =
          std::min(from->currentOpacity(), to->currentOpacity());
      const auto colour = fade(linkColour(strand.type, strand.tier), docAlpha);
      const auto tagId  = static_cast<std::uint32_t>(i);

      band(*nearEdge, *farEdge, docSpan, colour, tagId);
      anchorStub(*nearEdge, colour, tagId, false);
      anchorStub(*farEdge, colour, tagId, true);

      if (sworph && !strand.aligned) {
        if (moved) {
          stillToAlign = true;
        } else {
          strand.aligned = true;
          align(strand, state, ctx.timeline);
          moved = true;
        }
      }
    }

    const bool stillToOpen =
        sworph && (moved ? danglingOutstanding(state) : openDangling(state));

    unsettled = stillToAlign || stillToOpen;

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

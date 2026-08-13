/**
 * @file beams.cpp
 * @brief Implementation of the drawn butterfly.
 */
#include "beams.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <utility>

#include <glm/ext/vector_float4.hpp>

#include <gleditor/caret.hpp>
#include <gleditor/paths.hpp>
#include <gleditor/render_state.hpp>

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
    strands.push_back(Strand{one.link, one.type, one.from, one.to, {}, {},
                             false});
  }
  dangling.clear();
  dangling.reserve(unplaced.size());
  for (auto &one : unplaced) {
    dangling.push_back(Dangling{std::move(one), false});
  }
}

void LinkBeams::resolveAnchors(RenderState &state) {
  for (auto &strand : strands) {
    const auto anchorIn = [&state](const LinkEnd &end)
        -> std::optional<Doc::Anchor> {
      if (end.doc >= state.docs.size()) {
        return std::nullopt;
      }
      return state.docs[end.doc]->anchorFor(end.start);
    };
    // Retried until it answers rather than given up on: a document opened this
    // frame has no pages yet, and the link is no less real for that.
    if (!strand.fromAnchor) {
      strand.fromAnchor = anchorIn(strand.from);
    }
    if (!strand.toAnchor) {
      strand.toAnchor = anchorIn(strand.to);
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

bool LinkBeams::onScreen(const glm::mat4 &viewProjection,
                         const glm::vec3 &point) {
  const auto clip = viewProjection * glm::vec4(point, 1.0F);
  if (clip.w <= 0.0F) {
    return false;
  }
  const auto ndcX = clip.x / clip.w;
  const auto ndcY = clip.y / clip.w;
  return ndcX >= -1.0F && ndcX <= 1.0F && ndcY >= -1.0F && ndcY <= 1.0F;
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

  const auto nearAt = near->worldPoint(*strand.fromAnchor);
  const auto farAt  = far->worldPoint(*strand.toAnchor);
  if (!nearAt || !farAt) {
    return;
  }

  const glm::vec3 nearPos(near->modelMatrix()[3]);
  const glm::vec3 farPos(far->modelMatrix()[3]);

  // Side by side with a gap between them, rather than one resting place over.
  // The point of moving it is that both ends of the link can be seen at once,
  // and a fixed distance cannot promise that: two wide documents a slot apart
  // overlap, and two narrow ones are further apart than they need to be. Edge
  // to edge is as close as they can be without touching, which is as much of
  // both as any view can hold.
  const auto *const nearPage = near->page(strand.fromAnchor->pageIndex);
  const auto *const farPage  = far->page(strand.toAnchor->pageIndex);
  auto apart                 = AbstractRenderer::docSpacing;
  if (nullptr != nearPage && nullptr != farPage) {
    // Pages are centred on their own origin, so what separates two documents
    // that just clear each other is half of each, plus the gap.
    apart = (((nearPage->widthPixels() + farPage->widthPixels()) / 2.0F) *
             Doc::pixelsToWorld) +
            documentGap;
  }
  // On the side the far document is already on: moving it across the near one
  // to get there would be a longer journey to the same relation.
  const auto side = farPos.x >= nearPos.x ? 1.0F : -1.0F;
  const glm::vec3 target{
      nearPos.x + (side * apart),
      // Lifted or dropped by however far apart the two ends currently are, so
      // that afterwards they are level and the beam between them is a straight
      // run rather than a diagonal across two documents.
      farPos.y + (nearAt->y - farAt->y), nearPos.z};

  if (glm::distance(target, farPos) < alreadyAligned) {
    return;
  }
  std::cout << "xudu: link " << strand.link << " brings doc " << strand.to.doc
            << " alongside doc " << strand.from.doc << "\n";
  far->animateMoveTo(timeline, target);
}

void LinkBeams::openDangling(RenderState &state) {
  if (!opener) {
    return;
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
    // can be judged.
    return;
  }
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
  if (nullptr == beams) {
    return;
  }
  auto &state = ctx.state;

  if (session.generation() != builtFor || state.docs.size() != builtDocs) {
    builtFor  = session.generation();
    builtDocs = state.docs.size();
    rebuildStrands(state);
  }
  if (!visible) {
    return;
  }
  resolveAnchors(state);

  beams->clear();
  // At most one document is moved per frame, so that a document with several
  // links into it is not asked to be in two places at once.
  bool moved = false;

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
        glm::vec3(to->modelMatrix()[3]).x >= glm::vec3(from->modelMatrix()[3]).x;
    const auto fromAt = edgePoint(*from, *strand.fromAnchor, rightwards);
    const auto toAt   = edgePoint(*to, *strand.toAnchor, !rightwards);
    if (!fromAt || !toAt) {
      continue;
    }

    // Thickness follows the text: a beam is a relation between two lines, and
    // one drawn at a fixed width would swamp small text and vanish in large.
    const auto width =
        strand.fromAnchor->height * Doc::pixelsToWorld * beamWidthOfLine;
    beams->add(*fromAt, *toAt, width, linkColour(strand.type),
               static_cast<std::uint32_t>(i));

    // A link coming into view is what brings its far document over. Judged on
    // the near end rather than on both, because the far one being off screen
    // is exactly the situation this exists to fix.
    if (sworph && !moved && !strand.aligned &&
        onScreen(ctx.viewProjection, *fromAt)) {
      strand.aligned = true;
      align(strand, state, ctx.timeline);
      moved = true;
    }
  }

  if (sworph && !moved) {
    openDangling(state);
  }

  beams->commit();
  // Document and page zero, with no kind: the kind is the beam pipeline's, and
  // which beam it is rides in the beam's own tag.
  beams->draw(state, ctx.viewProjection, 1.0F,
              render::packTagIdentity(0, 0, 0));
}

} // namespace xudu

// vi: set sw=2 sts=2 ts=2 et:

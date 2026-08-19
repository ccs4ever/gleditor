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
    strands.push_back(
        Strand{one.link, one.type, one.from, one.to, {}, {}, false});
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
        [&state](const LinkEnd &end) -> std::optional<Doc::Anchor> {
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

  const auto *const nearPage = near->page(strand.fromAnchor->pageIndex);
  const auto *const farPage  = far->page(strand.toAnchor->pageIndex);
  auto apart                 = AbstractRenderer::docSpacing;
  float nearHalfWidth        = 10.0F;
  float farHalfWidth         = 10.0F;
  if (nullptr != nearPage && nullptr != farPage) {
    nearHalfWidth = (nearPage->widthPixels() * 0.5F) * Doc::pixelsToWorld;
    farHalfWidth  = (farPage->widthPixels() * 0.5F) * Doc::pixelsToWorld;
    apart         = nearHalfWidth + farHalfWidth + documentGap;
  }

  // Gather all strands belonging to the same link connecting this document pair
  // to compute the vertical centroid of the connected span ranges.
  float nearMinY = nearAt->y;
  float nearMaxY = nearAt->y;
  float farMinY  = farAt->y;
  float farMaxY  = farAt->y;

  for (auto &s : strands) {
    if (s.link == strand.link && s.from.doc == strand.from.doc &&
        s.to.doc == strand.to.doc) {
      s.aligned = true;
      if (s.fromAnchor) {
        if (const auto pt = near->worldPoint(*s.fromAnchor)) {
          nearMinY = std::min(nearMinY, pt->y);
          nearMaxY = std::max(nearMaxY, pt->y);
        }
      }
      if (s.toAnchor) {
        if (const auto pt = far->worldPoint(*s.toAnchor)) {
          farMinY = std::min(farMinY, pt->y);
          farMaxY = std::max(farMaxY, pt->y);
        }
      }
    }
  }

  const float nearCenterY = 0.5F * (nearMinY + nearMaxY);
  const float farCenterY  = 0.5F * (farMinY + farMaxY);
  const float deltaY      = nearCenterY - farCenterY;

  const auto nearDocIdx = strand.from.doc;
  const auto farDocIdx  = strand.to.doc;

  float totalApart = 0.0F;
  if (farDocIdx > nearDocIdx) {
    for (std::size_t d = nearDocIdx; d < farDocIdx && d + 1 < state.docs.size();
         ++d) {
      float d1Half = 10.0F;
      float d2Half = 10.0F;
      if (const auto *p1 = state.docs[d]->page(0)) {
        d1Half = (p1->widthPixels() * 0.5F) * Doc::pixelsToWorld;
      }
      if (const auto *p2 = state.docs[d + 1]->page(0)) {
        d2Half = (p2->widthPixels() * 0.5F) * Doc::pixelsToWorld;
      }
      totalApart += d1Half + d2Half + documentGap;
    }
  } else if (nearDocIdx > farDocIdx) {
    for (std::size_t d = farDocIdx; d < nearDocIdx && d + 1 < state.docs.size();
         ++d) {
      float d1Half = 10.0F;
      float d2Half = 10.0F;
      if (const auto *p1 = state.docs[d]->page(0)) {
        d1Half = (p1->widthPixels() * 0.5F) * Doc::pixelsToWorld;
      }
      if (const auto *p2 = state.docs[d + 1]->page(0)) {
        d2Half = (p2->widthPixels() * 0.5F) * Doc::pixelsToWorld;
      }
      totalApart += d1Half + d2Half + documentGap;
    }
  }
  if (totalApart <= 0.0F) {
    totalApart = apart;
  }

  const auto side = farDocIdx >= nearDocIdx ? 1.0F : -1.0F;
  const glm::vec3 target{nearPos.x + (side * totalApart), farPos.y + deltaY,
                         nearPos.z};

  if (glm::distance(target, farPos) >= alreadyAligned) {
    std::cout << "xudu: link " << strand.link << " aligns centroid of doc "
              << strand.to.doc << " with doc " << strand.from.doc << "\n";
    far->animateMoveTo(timeline, target);
  }

  // Dynamic Camera Auto-Framing: calculate world bounding envelope across all
  // open documents and fit view
  if (renderer && renderer->appState()) {
    float minX = std::min(nearPos.x - nearHalfWidth, target.x - farHalfWidth);
    float maxX = std::max(nearPos.x + nearHalfWidth, target.x + farHalfWidth);
    float minY = std::min(nearMinY, farMinY + deltaY);
    float maxY = std::max(nearMaxY, farMaxY + deltaY);

    for (std::size_t d = 0; d < state.docs.size(); ++d) {
      if (d == farDocIdx) {
        continue;
      }
      const glm::vec3 docPos(state.docs[d]->modelMatrix()[3]);
      float halfW = 10.0F;
      if (const auto *p = state.docs[d]->page(0)) {
        halfW = (p->widthPixels() * 0.5F) * Doc::pixelsToWorld;
      }
      minX = std::min(minX, docPos.x - halfW);
      maxX = std::max(maxX, docPos.x + halfW);
    }
    minX -= 4.0F;
    maxX += 4.0F;
    minY -= 6.0F;
    maxY += 6.0F;

    const float spanW = std::max(maxX - minX, 10.0F);
    const float spanH = std::max(maxY - minY, 10.0F);
    const glm::vec3 center(0.5F * (minX + maxX), 0.5F * (minY + maxY), 0.0F);

    std::scoped_lock locker(renderer->appState()->view);
    auto &view             = renderer->appState()->view;
    const float aspect     = (view.screenHeight > 0 && view.screenWidth > 0)
                                 ? static_cast<float>(view.screenWidth) /
                                       static_cast<float>(view.screenHeight)
                                 : 1.333F;
    const float fovRad     = glm::radians(view.fov);
    const float tanHalfFov = std::tan(fovRad * 0.5F);
    const float zFit       = std::max(spanH / (2.0F * tanHalfFov),
                                      spanW / (2.0F * aspect * tanHalfFov));

    view.pos = glm::vec3(center.x, center.y, std::max(zFit, 50.0F));
  }
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
    described++;
  }

  // Whatever an assistive technology asked to follow. Here rather than where
  // it was asked, because following a link moves the caret and may open a
  // document, and both are this thread's work.
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

    const auto rightwards = glm::vec3(to->modelMatrix()[3]).x >=
                            glm::vec3(from->modelMatrix()[3]).x;
    const auto fromAt     = edgePoint(*from, *strand.fromAnchor, rightwards);
    const auto toAt       = edgePoint(*to, *strand.toAnchor, !rightwards);
    if (!fromAt || !toAt) {
      continue;
    }

    // Thickness follows the text: a beam is a relation between two lines, and
    // one drawn at a fixed width would swamp small text and vanish in large.
    const auto width =
        strand.fromAnchor->height * Doc::pixelsToWorld * beamWidthOfLine;

    const std::size_t docSpan = strand.from.doc > strand.to.doc
                                    ? (strand.from.doc - strand.to.doc)
                                    : (strand.to.doc - strand.from.doc);

    if (docSpan <= 1) {
      // Adjacent documents: direct foreground beam
      beams->add(*fromAt, *toAt, width, linkColour(strand.type),
                 static_cast<std::uint32_t>(i));
    } else {
      // Non-adjacent documents: route through background depth layer (Z < 0)
      // to pass behind intermediate documents without occluding text.
      constexpr float bypassDepth = -20.0F;
      const float gapOffset       = rightwards ? 2.0F : -2.0F;
      const glm::vec3 mid1(fromAt->x + gapOffset, fromAt->y, bypassDepth);
      const glm::vec3 mid2(toAt->x - gapOffset, toAt->y, bypassDepth);

      beams->add(*fromAt, mid1, width, linkColour(strand.type),
                 static_cast<std::uint32_t>(i));
      beams->add(mid1, mid2, width, linkColour(strand.type),
                 static_cast<std::uint32_t>(i));
      beams->add(mid2, *toAt, width, linkColour(strand.type),
                 static_cast<std::uint32_t>(i));
    }

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
    // Named by what it connects rather than by what it looks like. A beam is a
    // coloured line and its colour is its type, which is exactly the kind of
    // thing that has to be said in words for anybody who is not looking at it.
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
    // Focus alone moves nothing: a beam is not somewhere the caret can be, and
    // claiming to have focused it would be a lie an assistive technology acts
    // on.
    return false;
  }
  const auto link = a11y::Ids::localOf(nodeId);
  if (0 == link) {
    return false;
  }
  // Under no lock and against nothing: the strand list is the render thread's,
  // so what is recorded is the link's own identity and the lookup happens
  // there. A link that has gone by then is simply not found.
  const std::scoped_lock locker(askedGuard);
  askedToFollow.push_back(link - 1);
  return true;
}

} // namespace xudu

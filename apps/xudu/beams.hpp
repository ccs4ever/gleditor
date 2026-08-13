/**
 * @file beams.hpp
 * @brief The butterfly, drawn.
 *
 * Nelson's pictures of Xanadu are nearly always the same picture: two
 * documents side by side with connections drawn between them, span to span.
 * The connections are the point. A link is not a jump from one document to
 * another, leaving the first behind; it is a visible relation between two
 * pieces of content, and both ends of it are meant to be on screen at once --
 * "the whole point is to see both ends".
 *
 * The library draws documents and, since the beam primitive, ribbons between
 * two places in the world. It knows nothing about links. What is here is the
 * part that does: finding where each end of a link has come to rest in the
 * documents that are open, drawing a beam between them, following one when it
 * is clicked, and bringing the far document alongside so that both ends can be
 * seen at all.
 *
 * A link attaches to content rather than to a position, so where its ends are
 * is a question about the open versions and changes only when they do. That is
 * what the caching here is: expensive to answer, asked when something changes,
 * and turned into geometry every frame by a matrix multiply.
 */
#ifndef XUDU_BEAMS_H
#define XUDU_BEAMS_H

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <gleditor/beams.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/renderer.hpp>

#include "core/link_layout.hpp"
#include "core/microversion.hpp"
#include "core/ops.hpp"
#include "session.hpp"

namespace xudu {

/**
 * @class LinkBeams
 * @brief Links between open documents, as beams that can be followed.
 *
 * Registered as both a frame contributor and a pick observer: it draws the
 * beams and it is offered the clicks that land on them, which is what makes a
 * link something you can follow rather than something you can look at.
 */
class LinkBeams : public gleditor::FrameContributor,
                  public gleditor::PickObserver {
public:
  /**
   * @param aOpen How a version not currently on screen is put there. Called on
   *        the render thread when a link's far end is in a document nobody has
   *        opened -- which is the ordinary case for a link, since a link is
   *        made to content and not to whatever happens to be open.
   */
  using Opener = std::function<void(const MicroversionId &)>;

  LinkBeams(Session &aSession, RendererRef aRenderer);
  ~LinkBeams() override;

  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &documentPipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;

  void setOpener(Opener aOpener) { opener = std::move(aOpener); }

  /// Whether beams are drawn at all. They are, by default: a link nobody can
  /// see is most of what Xanadu was arguing against.
  void setVisible(const bool shown) { visible = shown; }
  [[nodiscard]] bool isVisible() const { return visible; }
  void toggle() { visible = !visible; }

  /**
   * @brief Whether a link coming into view brings its far document to it.
   *
   * On by default. Off is for reading with the documents where you put them:
   * the beams still draw, and following one by clicking it still moves the
   * document, because that was asked for explicitly.
   */
  void setSworph(const bool on) { sworph = on; }
  [[nodiscard]] bool sworphing() const { return sworph; }

  /// Links that would be drawn if the documents holding them are built. For
  /// tests and for reporting what a frame did.
  [[nodiscard]] std::size_t strandCount() const { return strands.size(); }

private:
  /**
   * @brief One link with both ends placed: one beam.
   *
   * The anchors are page-local and are worked out once, because finding one
   * needs the page's shaping; where the pages are is asked for every frame,
   * which is a matrix multiply.
   */
  struct Strand {
    std::uint64_t link{};
    LinkType type{LinkType::Comment};
    LinkEnd from;
    LinkEnd to;
    std::optional<Doc::Anchor> fromAnchor;
    std::optional<Doc::Anchor> toAnchor;
    /// Whether the far document has already been brought alongside for this
    /// link. Once per link per generation: a document the reader has since
    /// moved should stay moved.
    bool aligned{};
  };

  /// A link with one end on screen and the other in no open document, and
  /// whether the search for a document showing that end has been made.
  struct Dangling {
    HalfLink link;
    bool looked{};
  };

  /// Work out where every link's ends are among the open documents.
  void rebuildStrands(RenderState &state);
  /// Ask the pages where the anchors are, for strands that do not know yet.
  void resolveAnchors(RenderState &state);
  /// Bring the far document of @p strand alongside the near one, lined up so
  /// that both ends of the link are level with each other.
  void align(const Strand &strand, RenderState &state, ch::Timeline &timeline);
  /// Open a document showing the far end of a link that has none on screen.
  void openDangling(RenderState &state);
  /// Put the caret on the far end of @p strand, whichever end is not the one
  /// the caret is in.
  void traverse(const Strand &strand, RenderState &state);

  /// World point a beam leaves a document from: the page margin on the side
  /// the other document is on, level with the anchor.
  [[nodiscard]] static std::optional<glm::vec3>
  edgePoint(const Doc &doc, const Doc::Anchor &anchor, bool towardsRight);

  /// Whether @p point is inside the view, which is what "the link became
  /// visible" means.
  [[nodiscard]] static bool onScreen(const glm::mat4 &viewProjection,
                                     const glm::vec3 &point);

  Session &session;
  RendererRef renderer;
  Opener opener;
  std::unique_ptr<gleditor::Beams> beams;
  std::vector<Strand> strands;
  std::vector<Dangling> dangling;
  /// The generation the strands were worked out for, and how many documents
  /// were open then. Either changing means finding them again.
  std::uint64_t builtFor{};
  std::size_t builtDocs{static_cast<std::size_t>(-1)};
  bool visible{true};
  bool sworph{true};
};

} // namespace xudu

#endif // XUDU_BEAMS_H
// vi: set sw=2 sts=2 ts=2 et:

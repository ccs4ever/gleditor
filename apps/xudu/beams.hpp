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
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/beams.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/renderer.hpp>

#include "xudu/core/link_layout.hpp"
#include "xudu/core/microversion.hpp"
#include "xudu/core/ops.hpp"
#include "xudu/session.hpp"

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
                  public gleditor::PickObserver,
                  public gleditor::a11y::Source {
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

  /**
   * @brief Whether a sworph is still owed.
   *
   * The render loop calls a frame settled when nothing is pending, and a
   * settled frame is what a screenshot captures and what --profile quits on.
   * Bringing a document alongside is decided inside drawFrame(), on the first
   * frame where every document has finished building -- which is exactly the
   * frame the loop would otherwise have called settled. Without this, a run
   * whose links move anything quits in the instant before it moves, and
   * captures the arrangement the sworph was about to replace.
   */
  [[nodiscard]] bool busy() const override { return unsettled; }

  // -- gleditor::a11y::Source ------------------------------------------------
  //
  // A beam is a line drawn between two passages. Nothing about a line is
  // reachable without seeing it, which makes this the part of Xudu that most
  // needs saying rather than drawing: the links are the hypertext.
  void describe(gleditor::a11y::Builder &into) override;
  [[nodiscard]] std::uint64_t accessibilityRevision() const override;
  bool performAction(std::uint64_t nodeId, gleditor::a11y::Action action,
                     std::string_view value) override;

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
    /// How prominent the link claims to be. Carried so that a beam is drawn
    /// at the same strength as the passages it joins: Session::decorate()
    /// shades those with linkColour(type, tier), and a beam that ignored the
    /// tier would draw a link nobody has vouched for as boldly as the
    /// author's own.
    ProminenceTier tier{ProminenceTier::Author};
    LinkEnd from;
    LinkEnd to;
    std::optional<Doc::Anchor> fromAnchor;
    std::optional<Doc::Anchor> toAnchor;
    std::optional<Doc::Anchor> fromEndAnchor;
    std::optional<Doc::Anchor> toEndAnchor;
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
  /// Whether any half-link is still waiting to be looked for; see busy().
  [[nodiscard]] bool danglingOutstanding(const RenderState &state) const;
  /**
   * @brief Open a document showing the far end of a link that has none on
   *        screen.
   * @return Whether one was opened, and so whether there may be more to look
   *         for once it has landed and the strands have been worked out again.
   */
  bool openDangling(RenderState &state);
  /// Put the caret on the far end of @p strand, whichever end is not the one
  /// the caret is in.
  void traverse(const Strand &strand, RenderState &state);

  /// World point a beam leaves a document from: the page margin on the side
  /// the other document is on, level with the anchor.
  [[nodiscard]] static std::optional<glm::vec3>
  edgePoint(const Doc &doc, const Doc::Anchor &anchor, bool towardsRight);

  /**
   * @brief Where one end of a link meets its document's margin, top and
   *        bottom.
   *
   * A link end is a range of bytes, not a point: it may run over several lines
   * and its two anchors are the first and the last. Both are wanted, and in a
   * known order, because what is drawn between two ends is a band with an edge
   * at each -- see @ref band.
   */
  struct Edge {
    glm::vec3 top{};
    glm::vec3 bottom{};
    /// Line height at the anchor, in world units. What sets the beam's weight,
    /// so that a relation between two lines of text is drawn at the scale of
    /// the text rather than at some fixed size that swamps small type and
    /// disappears in large.
    float lineHeight{};
  };

  /// Both ends of one link end at its document's margin, or nothing if the
  /// pages holding it are not built yet.
  [[nodiscard]] static std::optional<Edge>
  edgeOf(const Doc &doc, const std::optional<Doc::Anchor> &startAnchor,
         const std::optional<Doc::Anchor> &endAnchor, bool towardsRight);

  /**
   * @brief Draw the connection between two link ends as a band of strands.
   *
   * One ribbon between two points said "this byte relates to that byte", which
   * is not what a link says: both of its ends are ranges. Drawing the two
   * edges and then both diagonals -- which is what this did -- crosses them in
   * the middle, and a link between a two-line passage and a one-line one came
   * out as a bow tie with a twist in it rather than as a connection.
   *
   * A band instead: strands spread evenly across each end, spaced at
   * xudu::bandStrandCount()'s pitch across the taller of the two so that they
   * stay clear of each other there, and converging on the shorter one. Equal
   * ends give parallel strands; unequal ones give a spread that gathers into
   * the end attached to less text.
   */
  void band(const Edge &nearSide, const Edge &farSide,
            std::size_t documentsApart, std::uint32_t colour,
            std::uint32_t tag);

  /// A short bracket down the page margin covering one end of a link. Which
  /// page of a stack a link attaches to is otherwise only implied by where a
  /// beam happens to meet it, which at any distance is a guess.
  void anchorStub(const Edge &edge, std::uint32_t colour, std::uint32_t tag,
                  bool farEnd);

  /// @p colour with its alpha scaled by @p factor, for a beam that has to be
  /// as faint as the documents it runs between.
  [[nodiscard]] static std::uint32_t fade(std::uint32_t colour, float factor);

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

  /// Where the camera is easing towards, once align() has computed a new
  /// framing target. Seeded from the camera's actual position whenever no ease
  /// is already running (see cameraDriving), so a move starts from wherever
  /// the camera is -- including where the reader last left it -- rather than
  /// from a default-constructed origin or from the end of the last framing.
  /// The same seed-then-ramp pattern Doc::animateArrival() uses for documents
  /// themselves.
  ch::Output<glm::vec3> cameraTarget;
  /// Where the current ease is taking the camera. Compared against
  /// cameraTarget() to notice that it has arrived; see cameraDriving.
  glm::vec3 cameraGoal{};
  /**
   * @brief Whether the camera is being carried by an ease this class started.
   *
   * False until the first alignment, so a scene nobody has sworphed in never
   * moves the camera at all -- and false again as soon as the ease has
   * arrived, which is what hands the camera back. Writing the camera every
   * frame forever, which is what this used to do, silently undid every pan and
   * zoom the reader made after the first link came into view: the keystroke
   * moved the camera and the next frame moved it back.
   */
  bool cameraDriving{false};

  /// Whether the last frame left a document to bring over or a far end to look
  /// for. True to begin with, because the first frame has every link still to
  /// consider; see busy().
  bool unsettled{true};

  /// Which beam an assistive technology asked to follow, if any. Filled on the
  /// event thread and taken on the render thread, where following one means
  /// moving the caret and possibly opening a document.
  mutable std::mutex askedGuard;
  std::vector<std::uint64_t> askedToFollow;
  /// Bumped whenever the strands change, so the description is rebuilt then
  /// and not every frame.
  std::uint64_t described{1};
};

} // namespace xudu

#endif // XUDU_BEAMS_H

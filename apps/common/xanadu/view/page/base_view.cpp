#include "common/xanadu/view/page/base_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <gleditor/logging.hpp>

#include "common/xanadu/view/page/coalesce.hpp"

namespace xanadu::view {
namespace {

constexpr const char *kLayoutCategory = "view.layout";

[[nodiscard]] float passageCentre(const LinkEnd &end) noexcept {
  return 0.5F * (end.top + end.bottom);
}

/// The extent of a set of boxes in placement space.
struct Box {
  float left{std::numeric_limits<float>::max()};
  float right{std::numeric_limits<float>::lowest()};
  float bottom{std::numeric_limits<float>::max()};
  float top{std::numeric_limits<float>::lowest()};

  void include(const glm::vec3 centre, const float width, const float height) {
    left   = std::min(left, centre.x - (0.5F * width));
    right  = std::max(right, centre.x + (0.5F * width));
    bottom = std::min(bottom, centre.y - (0.5F * height));
    top    = std::max(top, centre.y + (0.5F * height));
  }
};

/// Whether a line of @p lineHeight at @p centre projects at least the
/// pane's least readable height (§10.1). A pane that states no least height
/// reads everything.
[[nodiscard]] bool legible(const PaneFrame &frame, const glm::vec3 centre,
                           const float lineHeight) noexcept {
  if (!(frame.minReadableLinePx > 0.0F)) {
    return true;
  }
  const glm::vec4 low = frame.localToClip * glm::vec4(centre, 1.0F);
  const glm::vec4 high =
      frame.localToClip *
      glm::vec4(centre + glm::vec3(0.0F, lineHeight, 0.0F), 1.0F);
  if (!(low.w > 0.0F) || !(high.w > 0.0F)) {
    return false;
  }
  const float px =
      std::abs((high.y / high.w) - (low.y / low.w)) * 0.5F * frame.heightPx;
  return px >= frame.minReadableLinePx;
}

/// How much of placement space the pane can show with a line of
/// @p lineHeight still readable: the camera may draw back until such a line
/// projects at the pane's least readable height, and no further (§10.3.2:
/// the layout never shrinks a page). Empty when the pane sets no limit.
[[nodiscard]] std::optional<glm::vec2> readableReach(const PaneFrame &frame,
                                                     const float lineHeight) {
  if (!(frame.minReadableLinePx > 0.0F) || !(frame.widthPx > 0.0F) ||
      !(frame.heightPx > 0.0F) || !(lineHeight > 0.0F)) {
    return std::nullopt;
  }
  const float scale = lineHeight / frame.minReadableLinePx;
  return glm::vec2(frame.widthPx * scale, frame.heightPx * scale);
}

} // namespace

struct BaseView::Work {
  struct Document {
    float width{}, height{}; // the column
    std::size_t firstPage{}; // into pages
    std::uint32_t count{};   // pages known
    bool background{};
    bool paginating{};
    bool participant{}; // holds an end of the active link
    bool joined{};      // a context document brought into the row
    bool flew{};        // documents sub-view: leaves a ghost
    glm::vec3 rest{};   // frame centre with no link active
    glm::vec3 centre{}; // frame centre in this layout
  };
  struct Page {
    PageFacts facts;
    float top{}; // from its column's top
    float opacity{1.0F};
    bool participant{};
    bool flew{};                    // leaves a ghost and a tether
    std::optional<glm::vec3> flown; // its own place, when not its frame's
    std::optional<Band> window;
  };
  /// What the coalescer moves: one page, or a whole document.
  struct Body {
    std::uint32_t document{};
    std::optional<std::uint32_t> page; // empty: the whole document
    std::optional<std::uint32_t> end;  // the end it is tied by
    glm::vec3 home{};
    float width{}, height{};
    float top{}; // of the body, from its column's top
    float lineHeight{};
  };

  std::vector<Document> documents;
  std::vector<Page> pages;
  std::vector<std::uint32_t> usable; // ends on pages pagination has reached
  std::vector<Body> bodies;          // the anchor's first
  std::vector<CoalesceBody> solved;
  std::vector<CoalesceTie> ties;
  std::vector<SubjectId> subjects; // what the link brought, for transition()
  std::optional<std::uint32_t> anchor; // into the active link's ends
  BaseUnit unit{};

  void arrange(const PageLayoutInput &in, const BaseViewSettings &s);
  void emit(const PageLayoutInput &in, const BaseViewSettings &s,
            LayoutSink &out) const;

private:
  void measure(const PageCatalog &catalog, const BaseViewSettings &s);
  void placeRows(const BaseViewSettings &s);
  [[nodiscard]] bool findEnds(const std::optional<ActiveLink> &active);
  void coalescePages(const PageLayoutInput &in, const BaseViewSettings &s);
  void coalesceDocuments(const PageLayoutInput &in, const BaseViewSettings &s);
  void settle(const BaseViewSettings &s);

  [[nodiscard]] Page &pageAt(const PageRef ref) {
    return pages[documents[ref.document].firstPage + ref.page];
  }
  [[nodiscard]] const Page &pageAt(const PageRef ref) const {
    return pages[documents[ref.document].firstPage + ref.page];
  }
  /// A page's centre in its document's frame, from the flow.
  [[nodiscard]] static glm::vec3 local(const Document &doc, const Page &page) {
    return {0.0F, (0.5F * doc.height) - page.top - (0.5F * page.facts.height),
            0.0F};
  }
  /// Where @p end's passage stands, its page at home in this layout's row.
  [[nodiscard]] float passageY(const LinkEnd &end) const {
    const auto &doc  = documents[end.page.document];
    const auto &page = pageAt(end.page);
    return doc.centre.y + local(doc, page).y + (0.5F * page.facts.height) -
           passageCentre(end);
  }
  /// A passage's centre from the top of @p body's box.
  [[nodiscard]] float heightIn(const Body &body, const LinkEnd &end) const {
    return pageAt(end.page).top - body.top + passageCentre(end);
  }
  /// The usable end on @p document, and on @p page if given, whose passage
  /// stands nearest the anchor's in height at home: the tie that moves it
  /// least. Ties go to the earlier end.
  [[nodiscard]] std::optional<std::uint32_t>
  nearestEnd(std::span<const LinkEnd> ends, std::uint32_t document,
             std::optional<std::uint32_t> page) const;
  void solveFirst(std::span<const LinkEnd> ends, std::size_t whole,
                  const CoalesceStrategy &strategy, float gap);
  [[nodiscard]] std::size_t fittingPrefix(std::size_t whole,
                                          std::optional<glm::vec2> reach) const;
  void placeWindows(std::span<const LinkEnd> ends, std::size_t whole,
                    const BaseViewSettings &s);
};

void BaseView::Work::measure(const PageCatalog &catalog,
                             const BaseViewSettings &s) {
  documents.clear();
  pages.clear();
  const auto count = catalog.documents();
  for (std::uint32_t d = 0; d < count; ++d) {
    const auto facts = catalog.document(d);
    Document doc{.firstPage  = pages.size(),
                 .count      = facts.pages,
                 .background = facts.background,
                 .paginating = facts.paginating};
    float y = 0.0F;
    for (std::uint32_t p = 0; p < facts.pages; ++p) {
      const auto page = catalog.page({.document = d, .page = p});
      if (p == 0) {
        doc.width = page.width;
      } else {
        y += s.base.pageGap;
      }
      pages.push_back(
          {.facts   = page,
           .top     = y,
           .opacity = facts.background ? s.pages.backgroundOpacity : 1.0F});
      y += page.height;
    }
    doc.height = y;
    documents.push_back(doc);
  }
  placeRows(s);
  for (auto &doc : documents) {
    doc.rest = doc.centre;
  }
}

/// Foreground documents, and context documents brought into the row, stand
/// in list order, the first centred on zero and each next one documentGap
/// past the last, as LinkBeams' docSlots has them. Context documents stand in
/// a row of their own behind it.
void BaseView::Work::placeRows(const BaseViewSettings &s) {
  struct Cursor {
    std::optional<float> x;
    float width{};
  };
  Cursor row;
  Cursor back;
  for (auto &doc : documents) {
    const bool inRow = !doc.background || doc.joined;
    auto &at         = inRow ? row : back;
    const float x    = at.x ? *at.x + (0.5F * at.width) + s.base.documentGap +
                               (0.5F * doc.width)
                            : 0.0F;
    at.x             = x;
    at.width         = doc.width;
    doc.centre       = {x, -0.5F * doc.height,
                  inRow ? 0.0F : -s.pages.backgroundDepth};
  }
}

bool BaseView::Work::findEnds(const std::optional<ActiveLink> &active) {
  usable.clear();
  anchor.reset();
  if (!active) {
    return false;
  }
  const auto ends = active->ends;
  for (std::uint32_t i = 0; i < ends.size(); ++i) {
    const auto ref = ends[i].page;
    if (ref.document < documents.size() &&
        ref.page < documents[ref.document].count) {
      usable.push_back(i);
    }
  }
  if (!std::ranges::contains(usable, active->anchor)) {
    GLEDITOR_LOG_DEBUG(kLayoutCategory,
                       "base view at rest: the anchor end {} of {} is on no "
                       "page known yet",
                       active->anchor, ends.size());
    return false;
  }
  anchor = active->anchor;
  for (const auto i : usable) {
    const auto ref                      = ends[i].page;
    documents[ref.document].participant = true;
    pageAt(ref).participant             = true;
  }
  return true;
}

std::optional<std::uint32_t>
BaseView::Work::nearestEnd(const std::span<const LinkEnd> ends,
                           const std::uint32_t document,
                           const std::optional<std::uint32_t> page) const {
  const float target = passageY(ends[*anchor]);
  std::optional<std::uint32_t> best;
  float bestDistance = 0.0F;
  for (const auto i : usable) {
    const auto ref = ends[i].page;
    if (ref.document != document || (page && ref.page != *page)) {
      continue;
    }
    const float distance = std::abs(passageY(ends[i]) - target);
    if (!best || distance < bestDistance) {
      best         = i;
      bestDistance = distance;
    }
  }
  return best;
}

/// Solves the anchor with the first @p whole bodies after it, each body with
/// an end tied to the anchor's passage.
void BaseView::Work::solveFirst(const std::span<const LinkEnd> ends,
                                const std::size_t whole,
                                const CoalesceStrategy &strategy,
                                const float gap) {
  solved.clear();
  ties.clear();
  const auto &anchorBody = bodies.front();
  const float fromHeight = heightIn(anchorBody, ends[*anchor]);
  for (std::size_t i = 0; i <= whole; ++i) {
    const auto &body = bodies[i];
    solved.push_back(
        {.page   = {.document = body.document, .page = body.page.value_or(0)},
         .home   = body.home,
         .width  = body.width,
         .height = body.height,
         .pinned = i == 0});
    if (i > 0 && body.end) {
      ties.push_back({.from       = 0,
                      .to         = static_cast<std::uint32_t>(i),
                      .fromHeight = fromHeight,
                      .toHeight   = heightIn(body, ends[*body.end]),
                      .gap        = gap});
    }
  }
  strategy.solve(solved, ties);
}

/// How many of the first @p whole bodies after the anchor, in tie order,
/// can stand whole with it and still be read in the pane.
std::size_t
BaseView::Work::fittingPrefix(const std::size_t whole,
                              const std::optional<glm::vec2> reach) const {
  if (!reach) {
    return whole;
  }
  Box box;
  for (std::size_t i = 0; i <= whole; ++i) {
    box.include(solved[i].position, solved[i].width, solved[i].height);
    if (box.right - box.left > reach->x || box.top - box.bottom > reach->y) {
      return i == 0 ? 0 : i - 1;
    }
  }
  return whole;
}

/// The pages of the link fly; a document every page of which takes part
/// moves whole (§10.3.2). Bodies are ordered by how near the anchor they
/// stand at home, which is the order they stay whole in while they fit;
/// the rest are shown as windows.
void BaseView::Work::coalescePages(const PageLayoutInput &in,
                                   const BaseViewSettings &s) {
  const auto ends      = in.active->ends;
  const auto anchorRef = ends[*anchor].page;

  const auto movesWhole = [&](const std::uint32_t d) {
    const auto &doc = documents[d];
    if (doc.paginating || doc.count == 0) {
      return false;
    }
    for (std::uint32_t p = 0; p < doc.count; ++p) {
      if (!pages[doc.firstPage + p].participant) {
        return false;
      }
    }
    return true;
  };
  const auto documentBody = [&](const std::uint32_t d) {
    const auto &doc = documents[d];
    return Body{.document = d,
                .home     = doc.centre,
                .width    = doc.width,
                .height   = doc.height};
  };
  const auto pageBody = [&](const PageRef ref) {
    const auto &doc  = documents[ref.document];
    const auto &page = pageAt(ref);
    return Body{.document = ref.document,
                .page     = ref.page,
                .home     = doc.centre + local(doc, page),
                .width    = page.facts.width,
                .height   = page.facts.height,
                .top      = page.top};
  };
  const auto tied = [&](Body body, const std::optional<std::uint32_t> end) {
    body.end = end;
    if (end) {
      body.lineHeight = pageAt(ends[*end].page).facts.lineHeight;
    }
    return body;
  };

  bodies.clear();
  const bool anchorWhole = movesWhole(anchorRef.document);
  bodies.push_back(
      tied(anchorWhole ? documentBody(anchorRef.document) : pageBody(anchorRef),
           anchor));
  for (std::uint32_t d = 0; d < documents.size(); ++d) {
    if (!documents[d].participant) {
      continue;
    }
    if (movesWhole(d)) {
      if (d != anchorRef.document) {
        bodies.push_back(
            tied(documentBody(d), nearestEnd(ends, d, std::nullopt)));
      }
      continue;
    }
    for (std::uint32_t p = 0; p < documents[d].count; ++p) {
      const PageRef ref{.document = d, .page = p};
      if (!pageAt(ref).participant ||
          (anchorWhole && d == anchorRef.document) || ref == anchorRef) {
        continue;
      }
      bodies.push_back(tied(pageBody(ref), nearestEnd(ends, d, p)));
    }
  }

  const glm::vec3 origin = bodies.front().home;
  const auto order       = [&origin](const Body &body) {
    return std::tuple{std::abs(body.home.x - origin.x),
                      std::abs(body.home.y - origin.y), body.end};
  };
  std::ranges::stable_sort(bodies.begin() + 1, bodies.end(), {}, order);

  float line = 0.0F;
  for (const auto &body : bodies) {
    if (body.lineHeight > 0.0F && (!(line > 0.0F) || body.lineHeight < line)) {
      line = body.lineHeight;
    }
  }
  const auto reach = readableReach(in.frame, line);

  // Each pass drops the bodies that did not fit and solves again, so the
  // ones left close up; a pass after which every one fits ends it.
  const TensionCoalesce strategy(s.physics, s.base);
  std::size_t whole = bodies.size() - 1;
  solveFirst(ends, whole, strategy, s.base.coalesceGap);
  for (auto fit = fittingPrefix(whole, reach); fit != whole;
       fit      = fittingPrefix(whole, reach)) {
    whole = fit;
    solveFirst(ends, whole, strategy, s.base.coalesceGap);
  }

  for (std::size_t i = 1; i <= whole; ++i) {
    const auto &body = bodies[i];
    const glm::vec3 to{solved[i].position.x, solved[i].position.y,
                       s.base.liftDepth};
    if (body.page) {
      pageAt({.document = body.document, .page = *body.page}).flown = to;
    } else {
      documents[body.document].centre = to;
    }
  }
  placeWindows(ends, whole, s);
}

/// Bodies past the first @p whole are shown as windows: only the band round
/// a passage, stacked in tie order on the side of the group their page's
/// home is on, the first with its passage level with the anchor's. A document
/// that would have moved whole is shown a window per page.
void BaseView::Work::placeWindows(const std::span<const LinkEnd> ends,
                                  const std::size_t whole,
                                  const BaseViewSettings &s) {
  if (whole + 1 >= bodies.size()) {
    return;
  }
  Box group;
  for (std::size_t i = 0; i <= whole; ++i) {
    group.include(solved[i].position, solved[i].width, solved[i].height);
  }
  const float anchorX = bodies.front().home.x;
  const float anchorY = passageY(ends[*anchor]);
  struct Stack {
    float edge{};
    std::optional<float> nextTop;
  };
  Stack left{.edge = group.left};
  Stack right{.edge = group.right};

  const auto place = [&](const PageRef ref, const std::uint32_t endIndex) {
    const auto &doc = documents[ref.document];
    auto &page      = pageAt(ref);
    const auto &end = ends[endIndex];
    const float context =
        static_cast<float>(s.base.bandContext) * page.facts.lineHeight;
    const Band band{.top = std::max(0.0F, end.top - context),
                    .bottom =
                        std::min(page.facts.height, end.bottom + context)};
    const bool leftwards = (doc.centre + local(doc, page)).x < anchorX;
    auto &stack          = leftwards ? left : right;
    const float pageTop  = stack.nextTop ? *stack.nextTop + band.top
                                         : anchorY + passageCentre(end);
    stack.nextTop        = pageTop - band.bottom - s.base.pageGap;
    const float reach    = s.base.coalesceGap + (0.5F * page.facts.width);
    page.flown =
        glm::vec3(leftwards ? stack.edge - reach : stack.edge + reach,
                  pageTop - (0.5F * page.facts.height), s.base.liftDepth);
    page.window = band;
  };

  for (std::size_t i = whole + 1; i < bodies.size(); ++i) {
    const auto &body = bodies[i];
    if (body.page) {
      if (body.end) {
        place({.document = body.document, .page = *body.page}, *body.end);
      }
      continue;
    }
    for (std::uint32_t p = 0; p < documents[body.document].count; ++p) {
      if (const auto end = nearestEnd(ends, body.document, p)) {
        place({.document = body.document, .page = p}, *end);
      }
    }
  }
}

/// Every document of the row is a body and the anchor's is pinned, as
/// LinkBeams loads its engine; a context document holding an end joins the
/// row in its list place, and those after it make room.
void BaseView::Work::coalesceDocuments(const PageLayoutInput &in,
                                       const BaseViewSettings &s) {
  const auto ends = in.active->ends;
  const auto a    = ends[*anchor].page.document;
  bool joined     = false;
  for (std::uint32_t d = 0; d < documents.size(); ++d) {
    auto &doc  = documents[d];
    doc.joined = doc.background && doc.participant && d != a;
    joined     = joined || doc.joined;
  }
  if (joined) {
    placeRows(s);
  }

  const auto body = [&](const std::uint32_t d,
                        const std::optional<std::uint32_t> end) {
    const auto &doc = documents[d];
    return Body{.document = d,
                .end      = end,
                .home     = doc.centre,
                .width    = doc.width,
                .height   = doc.height};
  };
  bodies.clear();
  bodies.push_back(body(a, anchor));
  for (std::uint32_t d = 0; d < documents.size(); ++d) {
    const auto &doc = documents[d];
    if (d == a || (doc.background && !doc.joined)) {
      continue;
    }
    bodies.push_back(body(d, doc.participant ? nearestEnd(ends, d, std::nullopt)
                                             : std::nullopt));
  }

  const TensionCoalesce strategy(s.physics, s.base);
  solveFirst(ends, bodies.size() - 1, strategy, s.base.coalesceGap);
  for (std::size_t i = 1; i < bodies.size(); ++i) {
    documents[bodies[i].document].centre = solved[i].position;
  }
}

/// What moved, what dims, and what leaves a ghost, once every place is
/// decided.
void BaseView::Work::settle(const BaseViewSettings &s) {
  subjects.clear();
  const bool active      = anchor.has_value();
  const bool byDocuments = unit == BaseUnit::Documents;
  for (std::uint32_t d = 0; d < documents.size(); ++d) {
    auto &doc            = documents[d];
    const bool docMoved  = doc.centre != doc.rest;
    const bool isAnchors = active && d == bodies.front().document;
    if (docMoved && doc.participant) {
      subjects.push_back(SubjectId::document(d));
    }
    doc.flew = byDocuments && docMoved && doc.participant && !isAnchors;
    for (std::uint32_t p = 0; p < doc.count; ++p) {
      auto &page        = pages[doc.firstPage + p];
      const auto at     = page.flown.value_or(doc.centre + local(doc, page));
      const bool moved  = at != doc.rest + local(doc, page);
      const bool taking = page.participant || (byDocuments && doc.participant);
      if (moved && taking) {
        subjects.push_back(subjectOf({.document = d, .page = p}));
      }
      page.flew = !byDocuments && moved && taking;
      if (active && !taking && !doc.background) {
        page.opacity = s.base.contextOpacity;
      } else if (taking) {
        page.opacity = 1.0F;
      }
    }
  }
}

void BaseView::Work::arrange(const PageLayoutInput &in,
                             const BaseViewSettings &s) {
  unit = in.subview == static_cast<std::uint32_t>(BaseUnit::Documents)
             ? BaseUnit::Documents
             : BaseUnit::Pages;
  bodies.clear();
  measure(in.catalog, s);
  if (findEnds(in.active)) {
    if (unit == BaseUnit::Documents) {
      coalesceDocuments(in, s);
    } else {
      coalescePages(in, s);
    }
  }
  settle(s);
}

void BaseView::Work::emit(const PageLayoutInput &in, const BaseViewSettings &s,
                          LayoutSink &out) const {
  for (std::uint32_t d = 0; d < documents.size(); ++d) {
    const auto &doc  = documents[d];
    const auto frame = out.push(PlacedFrame{.id     = SubjectId::document(d),
                                            .centre = doc.centre,
                                            .width  = doc.width,
                                            .height = doc.height});
    for (std::uint32_t p = 0; p < doc.count; ++p) {
      const PageRef ref{.document = d, .page = p};
      const auto &page = pages[doc.firstPage + p];
      const auto at    = page.flown.value_or(doc.centre + local(doc, page));
      std::uint32_t flags{};
      if (page.facts.marked()) {
        flags |= itemMarked;
      }
      if (in.cursor.page == ref) {
        flags |= itemFocus;
      }
      out.push(PlacedItem{
          .id      = subjectOf(ref),
          .centre  = at - doc.centre,
          .width   = page.facts.width,
          .height  = page.facts.height,
          .opacity = page.opacity,
          .content = legible(in.frame, at, page.facts.lineHeight)
                         ? ContentMode::Full
                         : ContentMode::Coarse,
          .flags   = flags,
          .frame   = frame,
          .window  = page.window,
      });
    }
  }

  // V-R33: what flew leaves an empty outline at home, tied to where it went.
  const std::uint64_t relation = in.active ? in.active->key.id : 0U;
  const auto leave = [&](const SubjectId ghost, const SubjectId subject,
                         const glm::vec3 home, const glm::vec3 at,
                         const float width, const float height) {
    out.push(PlacedItem{.id      = ghost,
                        .centre  = home,
                        .width   = width,
                        .height  = height,
                        .opacity = s.base.contextOpacity,
                        .content = ContentMode::None,
                        .flags   = itemGhost | itemViewOnly});
    out.push(PlacedEdge{.from     = ghost,
                        .to       = subject,
                        .a        = home,
                        .b        = at,
                        .kind     = EdgeKind::Tether,
                        .relation = relation});
  };
  for (std::uint32_t d = 0; d < documents.size(); ++d) {
    const auto &doc = documents[d];
    if (doc.flew) {
      leave(SubjectId::marker(d, kDocumentGhostSlot), SubjectId::document(d),
            doc.rest, doc.centre, doc.width, doc.height);
    }
    for (std::uint32_t p = 0; p < doc.count; ++p) {
      const auto &page = pages[doc.firstPage + p];
      if (!page.flew) {
        continue;
      }
      const auto subject = subjectOf({.document = d, .page = p});
      leave(SubjectId::marker(subject.value, kPageGhostSlot), subject,
            doc.rest + local(doc, page),
            page.flown.value_or(doc.centre + local(doc, page)),
            page.facts.width, page.facts.height);
    }
  }
}

BaseView::BaseView(BaseViewSettings settings)
    : settings_(settings), work_(std::make_unique<Work>()) {}

BaseView::~BaseView() = default;

BaseView *BaseView::configure(const BaseViewSettings &settings) noexcept {
  settings_ = settings;
  return this;
}

void BaseView::layout(const PageLayoutInput &in,
                      LayoutSink &out) const noexcept {
  try {
    work_->arrange(in, settings_);
    work_->emit(in, settings_, out);
  } catch (const std::exception &error) {
    // Only an allocation can fail here, and a sink that could not grow has
    // already said so.
    GLEDITOR_LOG_WARN(kLayoutCategory, "base view layout stopped short: {}",
                      error.what());
  }
}

void BaseView::transition(const PageLayoutInput &from,
                          const PageLayoutInput &to,
                          LayoutSink &out) const noexcept {
  try {
    LayoutSink before;
    work_->arrange(from, settings_);
    work_->emit(from, settings_, before);
    auto subjects = work_->subjects;

    LayoutSink after;
    work_->arrange(to, settings_);
    work_->emit(to, settings_, after);
    subjects.insert(subjects.end(), work_->subjects.begin(),
                    work_->subjects.end());

    std::unordered_map<SubjectId, glm::vec3> was;
    for (const auto &frame : before.frames()) {
      if (const auto pose = placedPose(before, frame)) {
        was.emplace(frame.id, pose->centre);
      }
    }
    for (const auto &item : before.items()) {
      if (const auto pose = placedPose(before, item)) {
        was.emplace(item.id, pose->centre);
      }
    }
    const auto hint = [&](const SubjectId id, const std::optional<Pose> &pose) {
      const auto found = was.find(id);
      if (!pose || found == was.end() || found->second == pose->centre) {
        return;
      }
      const bool subject = std::ranges::contains(subjects, id);
      out.push(MotionHint{.id      = id,
                          .delayMs = subject ? 0.0F : settings_.base.rowDelayMs,
                          .durationMs = subject ? settings_.base.subjectMs
                                                : settings_.base.rowMs});
    };
    for (const auto &frame : after.frames()) {
      hint(frame.id, placedPose(after, frame));
    }
    for (const auto &item : after.items()) {
      hint(item.id, placedPose(after, item));
    }
  } catch (const std::exception &error) {
    GLEDITOR_LOG_WARN(kLayoutCategory, "base view gave no motion hints: {}",
                      error.what());
  }
}

ViewDescriptor baseViewDescriptor(BaseViewSettings settings) {
  return {
      .kind        = std::string(BaseView::kKind),
      .name        = "Base",
      .description = "Documents side by side, pages down each; the pages of "
                     "an active link come together, level at the passages.",
      .subject     = ViewSubject::Page,
      .subviews    = {{.id = "pages", .name = "Pages"},
                      {.id = "documents", .name = "Documents"}},
      .make = [settings] { return std::make_unique<BaseView>(settings); },
  };
}

} // namespace xanadu::view

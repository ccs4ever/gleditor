#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <utility>
#include <vector>

#include <glm/ext/matrix_transform.hpp>
#include <glm/vec3.hpp>

#include "common/xanadu/document_id.hpp"
#include "common/xanadu/link_occurrences.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/view/page/base_view.hpp"
#include "common/xanadu/view/view.hpp"
#include "common/xanadu/view/view_records.hpp"
#include "hand_catalog.hpp"

namespace {

using xanadu::DocumentId;
using xanadu::LayoutConfig;
using xanadu::LinkSide;
using xanadu::view::ActiveLink;
using xanadu::view::BaseUnit;
using xanadu::view::BaseView;
using xanadu::view::baseViewDescriptor;
using xanadu::view::BaseViewSettings;
using xanadu::view::ContentMode;
using xanadu::view::EdgeKind;
using xanadu::view::itemFocus;
using xanadu::view::itemGhost;
using xanadu::view::itemMarked;
using xanadu::view::kDocumentGhostSlot;
using xanadu::view::kPageGhostSlot;
using xanadu::view::LayoutSink;
using xanadu::view::LinkEnd;
using xanadu::view::PageFacts;
using xanadu::view::PageLayoutInput;
using xanadu::view::PageRef;
using xanadu::view::PaneFrame;
using xanadu::view::PlacedEdge;
using xanadu::view::PlacedItem;
using xanadu::view::placedPose;
using xanadu::view::SubjectId;
using xanadu::view::SubjectKind;
using xanadu::view::subjectOf;
using xanadu::view::ViewRegistry;
using xanadu::view::ViewSubject;
using xuzz_test::HandCatalog;
using xuzz_test::HandDocument;

constexpr float kWidth  = 800.0F;
constexpr float kHeight = 1000.0F;
constexpr float kLine   = 20.0F;
/// Rounding between two ways of adding up a column.
constexpr float kExact = 1e-3F;

/// The settings a fresh system://layout is seeded with.
const LayoutConfig &config() {
  static const LayoutConfig layout;
  return layout;
}
BaseViewSettings settings() { return BaseViewSettings::fromLayout(config()); }
const xanadu::PageBaseConfig &base() { return config().pageBase; }

PageFacts plain(const float width = kWidth, const float height = kHeight) {
  return {.width = width, .height = height, .lineHeight = kLine};
}

HandDocument document(const std::uint32_t pages, const float width = kWidth,
                      const float height = kHeight) {
  HandDocument doc{.facts = {.id = DocumentId{}, .pages = pages}};
  for (std::uint32_t p = 0; p < pages; ++p) {
    doc.pages.push_back(plain(width, height));
  }
  return doc;
}

HandDocument context(HandDocument doc) {
  doc.facts.background = true;
  return doc;
}

LinkEnd end(const std::uint32_t d, const std::uint32_t p, const float top,
            const float bottom, const LinkSide side = LinkSide::Right) {
  return {.page   = {.document = d, .page = p},
          .top    = top,
          .bottom = bottom,
          .side   = side};
}

PageLayoutInput input(const HandCatalog &catalog,
                      const std::vector<LinkEnd> &ends,
                      const std::uint32_t anchor = 0,
                      const BaseUnit unit        = BaseUnit::Pages,
                      const PaneFrame frame      = {}) {
  return {.catalog = catalog,
          .cursor  = {.page = ends.empty() ? PageRef{} : ends[anchor].page},
          .active  = ends.empty() ? std::nullopt
                                  : std::optional{ActiveLink{.ends   = ends,
                                                             .anchor = anchor}},
          .frame   = frame,
          .subview = static_cast<std::uint32_t>(unit)};
}

LayoutSink run(const PageLayoutInput &in) {
  const BaseView view(settings());
  LayoutSink out;
  view.layout(in, out);
  return out;
}

const PlacedItem &item(const LayoutSink &sink, const SubjectId id) {
  const auto found = std::ranges::find(sink.items(), id, &PlacedItem::id);
  EXPECT_NE(found, sink.items().end());
  return *found;
}

glm::vec3 at(const LayoutSink &sink, const SubjectId id) {
  const auto pose = placedPose(sink, item(sink, id));
  EXPECT_TRUE(pose.has_value());
  return pose ? pose->centre : glm::vec3{};
}

glm::vec3 at(const LayoutSink &sink, const PageRef ref) {
  return at(sink, subjectOf(ref));
}

/// Where a passage stands in placement space, y up.
float passageY(const LayoutSink &sink, const LinkEnd &e) {
  const auto &page = item(sink, subjectOf(e.page));
  return at(sink, e.page).y + (0.5F * page.height) -
         (0.5F * (e.top + e.bottom));
}

/// The part of a page that is drawn: all of it, or its window.
struct Drawn {
  float left, right, bottom, top;
};
Drawn drawn(const LayoutSink &sink, const PageRef ref) {
  const auto &page = item(sink, subjectOf(ref));
  const auto c     = at(sink, ref);
  const float top  = c.y + (0.5F * page.height);
  const float from = page.window ? page.window->top : 0.0F;
  const float to   = page.window ? page.window->bottom : page.height;
  return {.left   = c.x - (0.5F * page.width),
          .right  = c.x + (0.5F * page.width),
          .bottom = top - to,
          .top    = top - from};
}
bool overlap(const Drawn &a, const Drawn &b) {
  return a.left < b.right && b.left < a.right && a.bottom < b.top &&
         b.bottom < a.top;
}

/// The row and column §10.3.1 states, written out independently: documents
/// left to right from zero, documentGap apart by their first page's width;
/// pages down from zero, pageGap apart; context documents a row of their
/// own, backgroundDepth behind.
std::vector<std::vector<glm::vec3>>
rowAndColumn(const std::vector<HandDocument> &docs) {
  std::vector<std::vector<glm::vec3>> centres;
  std::optional<float> rowX;
  std::optional<float> backX;
  float rowW  = 0.0F;
  float backW = 0.0F;
  for (const auto &doc : docs) {
    const bool back   = doc.facts.background;
    auto &x           = back ? backX : rowX;
    auto &w           = back ? backW : rowW;
    const float width = doc.pages.empty() ? 0.0F : doc.pages.front().width;
    x = x ? *x + (0.5F * w) + base().documentGap + (0.5F * width) : 0.0F;
    w = width;
    std::vector<glm::vec3> column;
    float top = 0.0F;
    for (std::uint32_t p = 0; p < doc.facts.pages; ++p) {
      const float h = doc.pages[p].height;
      column.emplace_back(*x, -top - (0.5F * h),
                          back ? -config().pages.backgroundDepth : 0.0F);
      top += h + base().pageGap;
    }
    centres.push_back(std::move(column));
  }
  return centres;
}

void expectRest(const LayoutSink &sink, const std::vector<HandDocument> &docs) {
  const auto want   = rowAndColumn(docs);
  std::size_t pages = 0;
  for (std::uint32_t d = 0; d < docs.size(); ++d) {
    for (std::uint32_t p = 0; p < docs[d].facts.pages; ++p, ++pages) {
      const auto got = at(sink, PageRef{.document = d, .page = p});
      EXPECT_NEAR(got.x, want[d][p].x, kExact) << d << ":" << p;
      EXPECT_NEAR(got.y, want[d][p].y, kExact) << d << ":" << p;
      EXPECT_NEAR(got.z, want[d][p].z, kExact) << d << ":" << p;
    }
  }
  EXPECT_EQ(sink.items().size(), pages);
  EXPECT_EQ(sink.frames().size(), docs.size());
  EXPECT_TRUE(sink.edges().empty());
}

std::vector<SubjectId> ghosts(const LayoutSink &sink) {
  std::vector<SubjectId> found;
  for (const auto &i : sink.items()) {
    if ((i.flags & itemGhost) != 0U) {
      found.push_back(i.id);
    }
  }
  return found;
}

// §10.3.4: with no active link, page positions equal the row-and-column
// formula, for any catalog and in either sub-view.
TEST(BaseViewTest, atRestEveryPageIsWhereTheRowAndColumnPutIt) {
  for (std::uint32_t seed = 1; seed <= 40; ++seed) {
    std::mt19937 random(seed);
    const auto pick = [&random](const std::uint32_t below) {
      return std::uniform_int_distribution<std::uint32_t>(0, below - 1)(random);
    };
    std::vector<HandDocument> docs;
    const auto count = 1 + pick(6);
    for (std::uint32_t d = 0; d < count; ++d) {
      const float width = 500.0F + (100.0F * static_cast<float>(pick(6)));
      HandDocument doc{.facts = {.id = DocumentId{}}};
      doc.facts.pages      = pick(5);
      doc.facts.paginating = pick(2) == 0;
      doc.facts.background = pick(4) == 0;
      for (std::uint32_t p = 0; p < doc.facts.pages; ++p) {
        doc.pages.push_back(
            plain(width, 600.0F + (50.0F * static_cast<float>(pick(12)))));
      }
      docs.push_back(std::move(doc));
    }
    const HandCatalog catalog(docs);
    for (const auto unit : {BaseUnit::Pages, BaseUnit::Documents}) {
      SCOPED_TRACE(seed);
      const auto sink = run(input(catalog, {}, 0, unit));
      expectRest(sink, docs);
      for (std::uint32_t d = 0; d < docs.size(); ++d) {
        for (std::uint32_t p = 0; p < docs[d].facts.pages; ++p) {
          const auto &page = item(sink, subjectOf({.document = d, .page = p}));
          EXPECT_FLOAT_EQ(page.opacity, docs[d].facts.background
                                            ? config().pages.backgroundOpacity
                                            : 1.0F);
          EXPECT_EQ(page.content, ContentMode::Full);
          EXPECT_FALSE(page.window.has_value());
        }
      }
    }
  }
}

TEST(BaseViewTest, documentsAreFramesThatCarryTheirPages) {
  const std::vector docs{document(3), document(2, 600.0F)};
  const HandCatalog catalog(docs);
  const auto sink = run(input(catalog, {}));
  ASSERT_EQ(sink.frames().size(), 2U);
  for (std::uint32_t d = 0; d < 2; ++d) {
    const auto &frame = sink.frames()[d];
    EXPECT_EQ(frame.id, SubjectId::document(d));
    EXPECT_FLOAT_EQ(frame.width, docs[d].pages.front().width);
    const float column =
        (static_cast<float>(docs[d].facts.pages) * kHeight) +
        (static_cast<float>(docs[d].facts.pages - 1) * base().pageGap);
    EXPECT_FLOAT_EQ(frame.height, column);
    // The frame is the column's box: its top is the top of the first page.
    EXPECT_NEAR(frame.centre.y + (0.5F * frame.height),
                at(sink, PageRef{.document = d}).y + (0.5F * kHeight), kExact);
    for (const auto &i : sink.items()) {
      if (i.id.value >> 32U == d) {
        EXPECT_EQ(i.frame, d);
      }
    }
  }
}

/// The row LinkBeams lays out today (beams.cpp, docSlots).
std::vector<float> docSlots(const std::vector<float> &widths) {
  std::vector<float> slots(widths.size(), 0.0F);
  for (std::size_t d = 1; d < widths.size(); ++d) {
    slots[d] = slots[d - 1] + (0.5F * widths[d - 1]) + (0.5F * widths[d]) +
               base().documentGap;
  }
  return slots;
}

/// Parity with today: in the documents sub-view every document keeps its
/// slot, every tied passage is level with the anchor's, and every other
/// document stays at its home height.
void expectRowParity(const LayoutSink &sink, const std::vector<float> &widths,
                     const std::vector<LinkEnd> &ends,
                     const std::uint32_t anchor) {
  const auto slots = docSlots(widths);
  for (std::uint32_t d = 0; d < widths.size(); ++d) {
    const auto &frame = sink.frames()[d];
    EXPECT_NEAR(frame.centre.x, slots[d], base().levelTolerance) << d;
    EXPECT_EQ(frame.centre.z, 0.0F) << d;
    const bool tied = std::ranges::any_of(
        ends, [d](const auto &e) { return e.page.document == d; });
    if (!tied) {
      EXPECT_FLOAT_EQ(frame.centre.y, -0.5F * frame.height) << d;
    }
  }
  for (const auto &e : ends) {
    EXPECT_NEAR(passageY(sink, e), passageY(sink, ends[anchor]),
                base().levelTolerance);
  }
}

TEST(BaseViewTest, theDocumentsSubviewIsTheRowUsersSeeToday) {
  // Two documents; the reader at either.
  {
    const std::vector widths{kWidth, kWidth};
    const HandCatalog catalog({document(1), document(1)});
    const std::vector ends{end(0, 0, 280.0F, 320.0F),
                           end(1, 0, 680.0F, 720.0F)};
    expectRowParity(run(input(catalog, ends, 0, BaseUnit::Documents)), widths,
                    ends, 0);
    expectRowParity(run(input(catalog, ends, 1, BaseUnit::Documents)), widths,
                    ends, 1);
  }
  // Three widths, ends either side of the reader.
  {
    const std::vector widths{600.0F, kWidth, 1000.0F};
    const HandCatalog catalog(
        {document(1, 600.0F), document(1), document(1, 1000.0F)});
    const std::vector ends{end(1, 0, 440.0F, 460.0F), end(0, 0, 100.0F, 140.0F),
                           end(2, 0, 860.0F, 900.0F)};
    expectRowParity(run(input(catalog, ends, 0, BaseUnit::Documents)), widths,
                    ends, 0);
  }
  // A document between the two ends keeps its slot, and so does the far end.
  {
    const std::vector widths{kWidth, kWidth, kWidth};
    const HandCatalog catalog({document(1), document(1), document(1)});
    const std::vector ends{end(0, 0, 180.0F, 220.0F),
                           end(2, 0, 580.0F, 620.0F)};
    expectRowParity(run(input(catalog, ends, 0, BaseUnit::Documents)), widths,
                    ends, 0);
  }
  // Many documents of several pages, the reader in the middle of seven.
  {
    const std::vector widths(7, kWidth);
    const HandCatalog catalog({document(2), document(3), document(1),
                               document(2), document(4), document(1),
                               document(2)});
    std::vector<LinkEnd> ends{end(3, 1, 240.0F, 260.0F)};
    for (std::uint32_t d = 0; d < 7; ++d) {
      if (d != 3) {
        ends.push_back(end(d, 0, 100.0F + (100.0F * static_cast<float>(d)),
                           130.0F + (100.0F * static_cast<float>(d))));
      }
    }
    expectRowParity(run(input(catalog, ends, 0, BaseUnit::Documents)), widths,
                    ends, 0);
  }
}

/// Every check of §10.3.4 that holds for any active link.
void expectAcceptance(const LayoutSink &rest, const LayoutSink &sink,
                      const std::vector<LinkEnd> &ends,
                      const std::uint32_t anchor) {
  const auto anchorRef = ends[anchor].page;
  EXPECT_EQ(at(sink, anchorRef), at(rest, anchorRef));

  std::vector<PageRef> participants;
  for (const auto &e : ends) {
    if (!std::ranges::contains(participants, e.page)) {
      participants.push_back(e.page);
    }
  }
  // Every passage visible, none covered by another participant.
  for (const auto &e : ends) {
    const auto &page = item(sink, subjectOf(e.page));
    if (page.window) {
      EXPECT_LE(page.window->top, e.top);
      EXPECT_GE(page.window->bottom, e.bottom);
    }
    const float top = at(sink, e.page).y + (0.5F * page.height);
    const Drawn passage{.left   = at(sink, e.page).x - (0.5F * page.width),
                        .right  = at(sink, e.page).x + (0.5F * page.width),
                        .bottom = top - e.bottom,
                        .top    = top - e.top};
    for (const auto other : participants) {
      if (other != e.page) {
        EXPECT_FALSE(overlap(passage, drawn(sink, other)))
            << "a passage of " << e.page.document << ":" << e.page.page
            << " is under " << other.document << ":" << other.page;
      }
    }
  }
  // Each moved page has one ghost at home and one tether from it.
  for (const auto &i : sink.items()) {
    if (i.id.kind != SubjectKind::Page) {
      continue;
    }
    const auto pose = placedPose(sink, i);
    ASSERT_TRUE(pose.has_value());
    const auto home  = at(rest, i.id);
    const auto ghost = SubjectId::marker(i.id.value, kPageGhostSlot);
    const auto ghostCount =
        std::ranges::count(sink.items(), ghost, &PlacedItem::id);
    const auto tethers =
        std::ranges::count_if(sink.edges(), [&](const auto &e) {
          return e.kind == EdgeKind::Tether && e.from == ghost && e.to == i.id;
        });
    const PageRef ref{.document = static_cast<std::uint32_t>(i.id.value >> 32U),
                      .page     = static_cast<std::uint32_t>(i.id.value)};
    if (pose->centre == home) {
      EXPECT_EQ(ghostCount, 0);
      EXPECT_EQ(tethers, 0);
      if (!std::ranges::contains(participants, ref)) {
        EXPECT_FLOAT_EQ(i.opacity, base().contextOpacity);
      }
      continue;
    }
    EXPECT_TRUE(std::ranges::contains(participants, ref))
        << "a page that holds no end moved";
    EXPECT_FLOAT_EQ(i.opacity, 1.0F);
    EXPECT_EQ(ghostCount, 1);
    EXPECT_EQ(tethers, 1);
    EXPECT_EQ(at(sink, ghost), home);
  }
}

/// Ties are to the anchor, by each page's end nearest it; here each page
/// holds one end, so each tie is the page's only end.
void expectLevel(const LayoutSink &sink, const std::vector<LinkEnd> &ends,
                 const std::uint32_t anchor) {
  for (const auto &e : ends) {
    if (!item(sink, subjectOf(e.page)).window) {
      EXPECT_NEAR(passageY(sink, e), passageY(sink, ends[anchor]),
                  base().levelTolerance)
          << e.page.document << ":" << e.page.page;
    }
  }
}

void expectApart(const LayoutSink &sink, const std::vector<LinkEnd> &ends) {
  for (std::size_t i = 0; i < ends.size(); ++i) {
    for (std::size_t j = i + 1; j < ends.size(); ++j) {
      if (ends[i].page != ends[j].page) {
        EXPECT_FALSE(
            overlap(drawn(sink, ends[i].page), drawn(sink, ends[j].page)))
            << i << " and " << j;
      }
    }
  }
}

TEST(BaseViewTest, theLinkedPagesComeTogetherLevelAndApart) {
  // A page in the middle of each of four documents of three pages, the
  // reader in the second; the pane sets no limit, so all stand whole.
  const std::vector docs{document(3), document(3), document(3), document(3)};
  const HandCatalog catalog(docs);
  const std::vector ends{end(1, 1, 400.0F, 440.0F), end(0, 2, 100.0F, 160.0F),
                         end(2, 0, 820.0F, 860.0F), end(3, 1, 300.0F, 330.0F)};
  const auto rest = run(input(catalog, {}));
  const auto sink = run(input(catalog, ends));

  expectAcceptance(rest, sink, ends, 0);
  expectLevel(sink, ends, 0);
  expectApart(sink, ends);
  for (const auto &e : ends) {
    EXPECT_FALSE(item(sink, subjectOf(e.page)).window.has_value());
  }
  // In front of the row; the anchor is not lifted, it does not move.
  for (std::size_t i = 1; i < ends.size(); ++i) {
    EXPECT_FLOAT_EQ(at(sink, ends[i].page).z, base().liftDepth);
  }
  // A page that flies leaves its document's frame where it was.
  for (std::uint32_t d = 0; d < docs.size(); ++d) {
    EXPECT_EQ(sink.frames()[d].centre, rest.frames()[d].centre);
  }
  EXPECT_EQ(ghosts(sink).size(), 3U);
}

// The finding of P2, as the pages sub-view's intended behaviour: the far end
// is brought beside the anchor, in front of the document between, which
// stays at home and dims. The documents sub-view keeps it in its slot.
TEST(BaseViewTest, aFarPageComesInFrontOfTheDocumentBetween) {
  const HandCatalog catalog({document(1), document(1), document(1)});
  const std::vector ends{end(0, 0, 180.0F, 220.0F), end(2, 0, 580.0F, 620.0F)};
  const auto rest = run(input(catalog, {}));

  const auto pages = run(input(catalog, ends));
  expectAcceptance(rest, pages, ends, 0);
  const auto far = at(pages, PageRef{.document = 2});
  // Beside the anchor, coalesceGap clear of it, to within what the springs
  // settle to: a physics unit.
  EXPECT_GE(far.x, kWidth + base().coalesceGap);
  EXPECT_NEAR(far.x, kWidth + base().coalesceGap, base().physicsUnitPx);
  EXPECT_FLOAT_EQ(far.z, base().liftDepth);
  EXPECT_EQ(at(pages, PageRef{.document = 1}),
            at(rest, PageRef{.document = 1}));
  EXPECT_FLOAT_EQ(item(pages, subjectOf({.document = 1})).opacity,
                  base().contextOpacity);

  const auto docs = run(input(catalog, ends, 0, BaseUnit::Documents));
  EXPECT_NEAR(at(docs, PageRef{.document = 2}).x,
              at(rest, PageRef{.document = 2}).x, base().levelTolerance);
}

TEST(BaseViewTest, onlyThePagesThatHoldEndsFly) {
  const HandCatalog catalog({document(2), document(4)});
  const std::vector ends{end(0, 0, 500.0F, 540.0F), end(1, 2, 120.0F, 160.0F)};
  const auto rest = run(input(catalog, {}));
  const auto sink = run(input(catalog, ends));
  expectAcceptance(rest, sink, ends, 0);
  for (std::uint32_t p = 0; p < 4; ++p) {
    if (p != 2) {
      EXPECT_EQ(at(sink, PageRef{.document = 1, .page = p}),
                at(rest, PageRef{.document = 1, .page = p}));
    }
  }
  EXPECT_NE(at(sink, PageRef{.document = 1, .page = 2}),
            at(rest, PageRef{.document = 1, .page = 2}));
  EXPECT_EQ(ghosts(sink),
            std::vector{SubjectId::marker(
                subjectOf({.document = 1, .page = 2}).value, kPageGhostSlot)});
}

// §10.3.2: when every page of a document takes part, the document moves as
// before, carrying its pages; each of them still leaves a ghost and a
// tether.
TEST(BaseViewTest, aDocumentEveryPageOfWhichTakesPartMovesWhole) {
  const HandCatalog catalog({document(1), document(1), document(2)});
  const std::vector ends{end(0, 0, 500.0F, 540.0F), end(2, 0, 120.0F, 160.0F),
                         end(2, 1, 900.0F, 940.0F)};
  const auto rest = run(input(catalog, {}));
  const auto sink = run(input(catalog, ends));
  expectAcceptance(rest, sink, ends, 0);
  EXPECT_NE(sink.frames()[2].centre, rest.frames()[2].centre);
  for (std::uint32_t p = 0; p < 2; ++p) {
    const auto id = subjectOf({.document = 2, .page = p});
    EXPECT_EQ(item(sink, id).centre, item(rest, id).centre)
        << "a page keeps its place in a document that moves whole";
  }
  EXPECT_NEAR(passageY(sink, ends[1]), passageY(sink, ends[0]),
              base().levelTolerance);
  EXPECT_EQ(ghosts(sink).size(), 2U);
}

// §10.3.4: a many-to-many link with three ends on a side brings all six
// pages.
TEST(BaseViewTest, aManyToManyLinkBringsAllSixPages) {
  std::vector<HandDocument> docs;
  for (int d = 0; d < 6; ++d) {
    docs.push_back(document(3));
  }
  const HandCatalog catalog(docs);
  std::vector<LinkEnd> ends;
  for (std::uint32_t d = 0; d < 6; ++d) {
    auto e   = end(d, d % 3U, 200.0F + (90.0F * static_cast<float>(d)),
                   240.0F + (90.0F * static_cast<float>(d)),
                 d < 3 ? LinkSide::Left : LinkSide::Right);
    e.member = d % 3U;
    ends.push_back(e);
  }
  const auto rest = run(input(catalog, {}));
  for (const std::uint32_t anchor : {0U, 4U}) {
    SCOPED_TRACE(anchor);
    const auto sink = run(input(catalog, ends, anchor));
    expectAcceptance(rest, sink, ends, anchor);
    expectLevel(sink, ends, anchor);
    expectApart(sink, ends);
    for (std::uint32_t i = 0; i < ends.size(); ++i) {
      const auto &page = item(sink, subjectOf(ends[i].page));
      EXPECT_FLOAT_EQ(page.opacity, 1.0F);
      if (i != anchor) {
        EXPECT_FLOAT_EQ(at(sink, ends[i].page).z, base().liftDepth);
      }
    }
    EXPECT_EQ(ghosts(sink).size(), 5U);
  }
}

/// A pane in which a line of kLine is readable down to 14 px: the camera may
/// draw back until it shows 2560·20/14 by 1440·20/14 of placement space,
/// about three pages and two gaps across and one and a half pages down.
PaneFrame pane() {
  return {.widthPx = 2560.0F, .heightPx = 1440.0F, .minReadableLinePx = 14.0F};
}

TEST(BaseViewTest, whatDoesNotFitIsShownAsWindowsRoundThePassages) {
  // The anchor between four ends: two pages fit beside it at a readable
  // size, the rest do not.
  std::vector<HandDocument> docs;
  for (int d = 0; d < 5; ++d) {
    docs.push_back(document(2));
  }
  const HandCatalog catalog(docs);
  const std::vector ends{end(2, 0, 480.0F, 520.0F), end(1, 1, 460.0F, 500.0F),
                         end(3, 0, 500.0F, 540.0F), end(0, 0, 300.0F, 330.0F),
                         end(4, 1, 700.0F, 760.0F)};
  const auto rest = run(input(catalog, {}));
  const auto sink = run(input(catalog, ends, 0, BaseUnit::Pages, pane()));
  expectAcceptance(rest, sink, ends, 0);
  expectApart(sink, ends);

  std::size_t windows = 0;
  for (std::size_t i = 1; i < ends.size(); ++i) {
    const auto &page = item(sink, subjectOf(ends[i].page));
    if (page.window) {
      ++windows;
      // The band is the passage and bandContext lines either side.
      EXPECT_FLOAT_EQ(page.window->top,
                      ends[i].top -
                          (static_cast<float>(base().bandContext) * kLine));
      EXPECT_FLOAT_EQ(page.window->bottom,
                      ends[i].bottom +
                          (static_cast<float>(base().bandContext) * kLine));
      EXPECT_FLOAT_EQ(at(sink, ends[i].page).z, base().liftDepth);
    }
  }
  // The two nearest stand whole, the two farthest are windows.
  EXPECT_FALSE(item(sink, subjectOf(ends[1].page)).window.has_value());
  EXPECT_FALSE(item(sink, subjectOf(ends[2].page)).window.has_value());
  EXPECT_EQ(windows, 2U);
  // The first window on each side is level with the anchor's passage.
  EXPECT_NEAR(passageY(sink, ends[3]), passageY(sink, ends[0]),
              base().levelTolerance);
  EXPECT_NEAR(passageY(sink, ends[4]), passageY(sink, ends[0]),
              base().levelTolerance);

  // With no limit from the pane, every page stands whole.
  const auto open = run(input(catalog, ends));
  for (const auto &e : ends) {
    EXPECT_FALSE(item(open, subjectOf(e.page)).window.has_value());
  }
}

TEST(BaseViewTest, windowsOnOneSideStackInTieOrderWithoutOverlap) {
  // Pages too tall to show two side by side: every other end is a window,
  // stacked to the right of the anchor, nearest first.
  std::vector<HandDocument> docs;
  for (int d = 0; d < 4; ++d) {
    docs.push_back(document(1, kWidth, 1600.0F));
  }
  const HandCatalog catalog(docs);
  const std::vector ends{end(0, 0, 1500.0F, 1540.0F), end(1, 0, 40.0F, 80.0F),
                         end(2, 0, 700.0F, 720.0F), end(3, 0, 10.0F, 30.0F)};
  const auto rest = run(input(catalog, {}));
  const auto sink = run(input(catalog, ends, 0, BaseUnit::Pages, pane()));
  expectAcceptance(rest, sink, ends, 0);
  expectApart(sink, ends);
  float above = std::numeric_limits<float>::max();
  for (std::size_t i = 1; i < ends.size(); ++i) {
    const auto &page = item(sink, subjectOf(ends[i].page));
    ASSERT_TRUE(page.window.has_value()) << i;
    const auto band = drawn(sink, ends[i].page);
    EXPECT_GT(band.left, 0.5F * kWidth);
    EXPECT_LT(band.top, above);
    above = band.bottom;
  }
}

TEST(BaseViewTest, twoLayoutsOfOneInputAreIdentical) {
  const HandCatalog catalog({document(2), document(3), context(document(2))});
  const std::vector ends{end(1, 1, 400.0F, 420.0F), end(0, 1, 100.0F, 120.0F),
                         end(2, 0, 600.0F, 640.0F), end(1, 2, 50.0F, 70.0F)};
  for (const auto unit : {BaseUnit::Pages, BaseUnit::Documents}) {
    const BaseView view(settings());
    LayoutSink first;
    LayoutSink second;
    const auto in = input(catalog, ends, 0, unit, pane());
    view.layout(in, first);
    // Another layout between, so the view's scratch holds something else.
    LayoutSink other;
    view.layout(input(catalog, {}), other);
    view.layout(in, second);

    ASSERT_EQ(first.items().size(), second.items().size());
    ASSERT_EQ(first.frames().size(), second.frames().size());
    ASSERT_EQ(first.edges().size(), second.edges().size());
    for (std::size_t i = 0; i < first.items().size(); ++i) {
      const auto &a = first.items()[i];
      const auto &b = second.items()[i];
      EXPECT_EQ(a.id, b.id);
      EXPECT_EQ(a.centre, b.centre);
      EXPECT_EQ(a.opacity, b.opacity);
      EXPECT_EQ(a.flags, b.flags);
      EXPECT_EQ(a.frame, b.frame);
      EXPECT_EQ(a.window.has_value(), b.window.has_value());
    }
    for (std::size_t i = 0; i < first.frames().size(); ++i) {
      EXPECT_EQ(first.frames()[i].centre, second.frames()[i].centre);
    }
    for (std::size_t i = 0; i < first.edges().size(); ++i) {
      EXPECT_EQ(first.edges()[i].a, second.edges()[i].a);
      EXPECT_EQ(first.edges()[i].b, second.edges()[i].b);
    }
  }
}

TEST(BaseViewTest, aContextDocumentStandsBehindAndIsBroughtForward) {
  const HandCatalog catalog({document(1), context(document(1)), document(1)});
  const auto rest = run(input(catalog, {}));
  EXPECT_FLOAT_EQ(at(rest, PageRef{.document = 1}).z,
                  -config().pages.backgroundDepth);
  EXPECT_FLOAT_EQ(item(rest, subjectOf({.document = 1})).opacity,
                  config().pages.backgroundOpacity);

  const std::vector ends{end(0, 0, 300.0F, 340.0F), end(1, 0, 100.0F, 140.0F)};
  const auto pages = run(input(catalog, ends));
  expectAcceptance(rest, pages, ends, 0);
  EXPECT_FLOAT_EQ(at(pages, PageRef{.document = 1}).z, base().liftDepth);
  EXPECT_FLOAT_EQ(item(pages, subjectOf({.document = 1})).opacity, 1.0F);

  // The documents sub-view brings it into the row in its list place, as
  // LinkBeams does, and the document after it makes room.
  const auto docs = run(input(catalog, ends, 0, BaseUnit::Documents));
  EXPECT_FLOAT_EQ(docs.frames()[1].centre.z, 0.0F);
  EXPECT_NEAR(docs.frames()[1].centre.x, kWidth + base().documentGap,
              base().levelTolerance);
  EXPECT_GT(docs.frames()[2].centre.x, rest.frames()[2].centre.x);
  const auto ghost = SubjectId::marker(1, kDocumentGhostSlot);
  EXPECT_EQ(at(docs, ghost), rest.frames()[1].centre);
  EXPECT_EQ(std::ranges::count_if(docs.edges(),
                                  [&](const PlacedEdge &e) {
                                    return e.kind == EdgeKind::Tether &&
                                           e.from == ghost &&
                                           e.to == SubjectId::document(1);
                                  }),
            1);
}

TEST(BaseViewTest, anAnchorOnNoKnownPageLeavesTheRowAtRest) {
  auto paginating             = document(1);
  paginating.facts.paginating = true;
  const std::vector docs{document(1), paginating};
  const HandCatalog catalog(docs);
  const std::vector ends{end(1, 3, 10.0F, 20.0F), end(0, 0, 10.0F, 20.0F)};
  expectRest(run(input(catalog, ends)), docs);
}

TEST(BaseViewTest, pagesTooSmallToReadAreCoarse) {
  const HandCatalog catalog({document(2)});
  // A line of kLine projects at kLine·scale·360 px in a pane 720 px high:
  // 7.2 px at a thousandth, under the pane's 14; 72 px at a hundredth.
  auto frame        = pane();
  frame.heightPx    = 720.0F;
  frame.localToClip = glm::scale(glm::mat4(1.0F), glm::vec3(0.001F));
  const auto small  = run(input(catalog, {}, 0, BaseUnit::Pages, frame));
  EXPECT_EQ(item(small, subjectOf({})).content, ContentMode::Coarse);
  frame.localToClip = glm::scale(glm::mat4(1.0F), glm::vec3(0.01F));
  const auto large  = run(input(catalog, {}, 0, BaseUnit::Pages, frame));
  EXPECT_EQ(item(large, subjectOf({})).content, ContentMode::Full);
}

TEST(BaseViewTest, theCaretsPageIsFocusedAndMarkedPagesFlagged) {
  auto doc          = document(2);
  doc.pages[1].link = true;
  const HandCatalog catalog({doc});
  auto in             = input(catalog, {});
  in.cursor.page.page = 1;
  const auto sink     = run(in);
  EXPECT_EQ(item(sink, subjectOf({.page = 1})).flags, itemFocus | itemMarked);
  EXPECT_EQ(item(sink, subjectOf({})).flags, 0U);
}

// §10.3.3: the page brought over starts at once and takes longest; what
// makes room waits and is quicker. Subjects that appear get no hint.
TEST(BaseViewTest, theBroughtPageIsTheSubjectOfTheMove) {
  const HandCatalog catalog({document(1), context(document(1)), document(1)});
  const std::vector ends{end(0, 0, 300.0F, 340.0F), end(1, 0, 100.0F, 140.0F)};
  const BaseView view(settings());

  const auto hintFor = [](const LayoutSink &sink, const SubjectId id) {
    return std::ranges::find(sink.hints(), id, &xanadu::view::MotionHint::id);
  };
  {
    LayoutSink out;
    view.transition(input(catalog, {}, 0, BaseUnit::Documents),
                    input(catalog, ends, 0, BaseUnit::Documents), out);
    const auto brought = hintFor(out, SubjectId::document(1));
    ASSERT_NE(brought, out.hints().end());
    EXPECT_FLOAT_EQ(brought->delayMs, 0.0F);
    EXPECT_FLOAT_EQ(brought->durationMs, base().subjectMs);
    const auto room = hintFor(out, SubjectId::document(2));
    ASSERT_NE(room, out.hints().end());
    EXPECT_FLOAT_EQ(room->delayMs, base().rowDelayMs);
    EXPECT_FLOAT_EQ(room->durationMs, base().rowMs);
    EXPECT_EQ(hintFor(out, SubjectId::document(0)), out.hints().end());
    EXPECT_EQ(hintFor(out, SubjectId::marker(1, kDocumentGhostSlot)),
              out.hints().end());
    EXPECT_TRUE(out.items().empty());
  }
  {
    // Released: the one-page document that flew goes home, carrying its
    // page, as the subject; nothing else moves.
    LayoutSink out;
    view.transition(input(catalog, ends), input(catalog, {}), out);
    for (const auto id : {SubjectId::document(1), subjectOf({.document = 1})}) {
      const auto back = hintFor(out, id);
      ASSERT_NE(back, out.hints().end());
      EXPECT_FLOAT_EQ(back->delayMs, 0.0F);
      EXPECT_FLOAT_EQ(back->durationMs, base().subjectMs);
    }
    EXPECT_EQ(out.hints().size(), 2U);
  }
}

TEST(BaseViewTest, theDescriptorInstallsAPageViewWithTwoSubviews) {
  ViewRegistry registry;
  ASSERT_TRUE(registry.add(baseViewDescriptor(settings())).has_value());
  const auto found = registry.find(BaseView::kKind);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->subject, ViewSubject::Page);
  ASSERT_EQ(found->subviews.size(), 2U);
  EXPECT_EQ(found->subviews[static_cast<std::size_t>(BaseUnit::Pages)].id,
            "pages");
  EXPECT_EQ(found->subviews[static_cast<std::size_t>(BaseUnit::Documents)].id,
            "documents");
  const auto made = found->make();
  ASSERT_NE(made, nullptr);
  EXPECT_EQ(made->kind(), BaseView::kKind);
}

TEST(BaseViewTest, newSettingsAreReadByTheNextLayout) {
  const HandCatalog catalog({document(1), document(1)});
  BaseView view(settings());
  auto wider             = settings();
  wider.base.documentGap = 100.0F;
  EXPECT_EQ(view.configure(wider), &view);
  LayoutSink sink;
  view.layout(input(catalog, {}), sink);
  EXPECT_FLOAT_EQ(sink.frames()[1].centre.x, kWidth + 100.0F);
}

} // namespace

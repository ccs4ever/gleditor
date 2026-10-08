#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "common/xanadu/document_id.hpp"
#include "common/xanadu/link_occurrences.hpp"
#include "common/xanadu/view/page_view.hpp"
#include "common/xanadu/view/view_records.hpp"

namespace {

using xanadu::DocumentId;
using xanadu::DocumentSite;
using xanadu::Extent;
using xanadu::LinkSide;
using xanadu::view::ActiveLink;
using xanadu::view::DocumentFacts;
using xanadu::view::itemFocus;
using xanadu::view::itemMarked;
using xanadu::view::LayoutSink;
using xanadu::view::LinkEnd;
using xanadu::view::PageCatalog;
using xanadu::view::PageCursor;
using xanadu::view::PageFacts;
using xanadu::view::PageLayoutInput;
using xanadu::view::PageRef;
using xanadu::view::PageView;
using xanadu::view::PaneFrame;
using xanadu::view::PlacedItem;
using xanadu::view::SubjectId;
using xanadu::view::subjectOf;

/// One document as pagination has it so far: each page's facts, and the
/// byte at which each page's text starts.
struct HandDocument {
  DocumentFacts facts;
  std::vector<PageFacts> pages;
  std::vector<std::uint32_t> pageStarts;
};

/// A catalog written by hand, as §8.6 says a test writes one.
class HandCatalog final : public PageCatalog {
public:
  explicit HandCatalog(std::vector<HandDocument> documents)
      : documents_(std::move(documents)) {}

  [[nodiscard]] std::uint32_t documents() const noexcept override {
    return static_cast<std::uint32_t>(documents_.size());
  }
  [[nodiscard]] DocumentFacts
  document(const std::uint32_t index) const noexcept override {
    return documents_.at(index).facts;
  }
  [[nodiscard]] PageFacts page(const PageRef ref) const noexcept override {
    return documents_.at(ref.document).pages.at(ref.page);
  }
  [[nodiscard]] std::optional<PageRef>
  pageOf(const DocumentSite &site) const noexcept override {
    for (std::uint32_t d = 0; d < documents_.size(); ++d) {
      const auto &doc = documents_[d];
      if (!(doc.facts.id == site.store)) {
        continue;
      }
      const auto after =
          std::ranges::upper_bound(doc.pageStarts, site.range.start);
      if (after == doc.pageStarts.begin()) {
        return std::nullopt;
      }
      const auto page =
          static_cast<std::uint32_t>(after - doc.pageStarts.begin() - 1);
      // Past the last page known so far, while pagination is still running,
      // is not yet on any page.
      if (doc.facts.paginating && after == doc.pageStarts.end()) {
        return std::nullopt;
      }
      return PageRef{.document = d, .page = page};
    }
    return std::nullopt;
  }

private:
  std::vector<HandDocument> documents_;
};

constexpr float kWidth  = 800.0F;
constexpr float kHeight = 1000.0F;
constexpr float kLine   = 20.0F;

PageFacts plainPage() {
  return {.width = kWidth, .height = kHeight, .lineHeight = kLine};
}

/// Two documents: A has three pages, the second holding a link end; B has
/// two known so far and is still paginating, its first page quoting A.
struct TwoDocuments {
  DocumentId a, b;
  HandCatalog catalog;

  TwoDocuments() : catalog(make(a, b)) {}

private:
  static std::vector<HandDocument> make(const DocumentId &a,
                                        const DocumentId &b) {
    auto linked         = plainPage();
    linked.link         = true;
    auto quoted         = plainPage();
    quoted.transclusion = true;
    return {
        {.facts      = {.id = a, .pages = 3},
         .pages      = {plainPage(), linked, plainPage()},
         .pageStarts = {0, 1200, 2400}},
        {.facts      = {.id = b, .pages = 2, .paginating = true},
         .pages      = {quoted, plainPage()},
         .pageStarts = {0, 900}},
    };
  }
};

/// The smallest honest page view: documents side by side, pages down each
/// column, the caret's page focused and marked pages flagged. It reads
/// nothing but its input.
class ColumnView final : public PageView {
public:
  [[nodiscard]] std::string_view kind() const noexcept override {
    return "page.test-columns";
  }
  void layout(const PageLayoutInput &in,
              LayoutSink &out) const noexcept override {
    for (std::uint32_t d = 0; d < in.catalog.documents(); ++d) {
      const auto facts = in.catalog.document(d);
      for (std::uint32_t p = 0; p < facts.pages; ++p) {
        const PageRef ref{.document = d, .page = p};
        const auto page = in.catalog.page(ref);
        std::uint32_t flags{};
        if (page.marked()) {
          flags |= itemMarked;
        }
        if (in.cursor.page == ref) {
          flags |= itemFocus;
        }
        out.push(PlacedItem{
            .id     = subjectOf(ref),
            .centre = {static_cast<float>(d) * 2.0F * page.width,
                       -static_cast<float>(p) * page.height, 0.0F},
            .width  = page.width,
            .height = page.height,
            .flags  = flags,
        });
      }
    }
  }
};

TEST(PageViewTest, aHandWrittenCatalogAnswersWhatPaginationKnows) {
  const TwoDocuments docs;
  const PageCatalog &catalog = docs.catalog;

  ASSERT_EQ(catalog.documents(), 2U);
  EXPECT_EQ(catalog.document(0).id, docs.a);
  EXPECT_EQ(catalog.document(0).pages, 3U);
  EXPECT_FALSE(catalog.document(0).paginating);
  EXPECT_TRUE(catalog.document(1).paginating);

  const auto linked = catalog.page({.document = 0, .page = 1});
  EXPECT_FLOAT_EQ(linked.width, kWidth);
  EXPECT_FLOAT_EQ(linked.height, kHeight);
  EXPECT_FLOAT_EQ(linked.lineHeight, kLine);
  EXPECT_TRUE(linked.link);
  EXPECT_TRUE(linked.marked());
  EXPECT_TRUE(catalog.page({.document = 1, .page = 0}).marked());
  EXPECT_FALSE(catalog.page({.document = 0, .page = 0}).marked());
}

TEST(PageViewTest, aSiteIsFoundOnThePageThatHoldsIt) {
  const TwoDocuments docs;
  const PageCatalog &catalog = docs.catalog;

  const DocumentSite inA{.store = docs.a,
                         .range = {.start = 1300, .end = 1340}};
  EXPECT_EQ(catalog.pageOf(inA), (PageRef{.document = 0, .page = 1}));

  const DocumentSite lastOfA{.store = docs.a, .range = {.start = 5000}};
  EXPECT_EQ(catalog.pageOf(lastOfA), (PageRef{.document = 0, .page = 2}));

  // B is still paginating: a byte past its known pages is on no page yet.
  const DocumentSite aheadInB{.store = docs.b, .range = {.start = 4000}};
  EXPECT_FALSE(catalog.pageOf(aheadInB).has_value());
  const DocumentSite firstOfB{.store = docs.b, .range = {.start = 10}};
  EXPECT_EQ(catalog.pageOf(firstOfB), (PageRef{.document = 1, .page = 0}));

  const DocumentSite elsewhere{.store = DocumentId{}, .range = {}};
  EXPECT_FALSE(catalog.pageOf(elsewhere).has_value());
}

TEST(PageViewTest, pagesOrderByDocumentThenPage) {
  std::vector<PageRef> refs{{.document = 1, .page = 0},
                            {.document = 0, .page = 2},
                            {.document = 0, .page = 0},
                            {.document = 1, .page = 1}};
  std::ranges::sort(refs);
  const std::vector<PageRef> ordered{{.document = 0, .page = 0},
                                     {.document = 0, .page = 2},
                                     {.document = 1, .page = 0},
                                     {.document = 1, .page = 1}};
  EXPECT_EQ(refs, ordered);
}

TEST(PageViewTest, aPageKeepsOneIdentityWhereverItIsPlaced) {
  const PageRef ref{.document = 2, .page = 7};
  EXPECT_EQ(subjectOf(ref), SubjectId::page(2, 7));
  EXPECT_EQ(subjectOf(ref, 3), SubjectId::page(2, 7, 3));
  EXPECT_NE(subjectOf(ref), subjectOf({.document = 7, .page = 2}));
  EXPECT_EQ(subjectOf(ref).epoch, 0U);
}

TEST(PageViewTest, aPageViewIsAFunctionOfItsInput) {
  const TwoDocuments docs;
  ColumnView view;
  const std::array ends{
      LinkEnd{.page   = {.document = 0, .page = 1},
              .top    = 100.0F,
              .bottom = 140.0F,
              .side   = LinkSide::Left},
      LinkEnd{.page   = {.document = 1, .page = 0},
              .top    = 300.0F,
              .bottom = 360.0F,
              .side   = LinkSide::Right},
  };
  const PageLayoutInput in{
      .catalog = docs.catalog,
      .cursor  = {.page = {.document = 0, .page = 1}},
      .active  = ActiveLink{.ends = ends, .anchor = 0},
      .frame   = PaneFrame{.widthPx = 1280.0F, .heightPx = 720.0F},
  };
  EXPECT_EQ(view.prepare(in), &view);

  LayoutSink first;
  LayoutSink second;
  view.layout(in, first);
  view.layout(in, second);

  ASSERT_EQ(first.items().size(), 5U);
  ASSERT_EQ(second.items().size(), first.items().size());
  for (std::size_t i = 0; i < first.items().size(); ++i) {
    EXPECT_EQ(first.items()[i].id, second.items()[i].id);
    EXPECT_EQ(first.items()[i].centre, second.items()[i].centre);
    EXPECT_EQ(first.items()[i].flags, second.items()[i].flags);
  }

  const auto item = [&first](const PageRef ref) {
    return *std::ranges::find(first.items(), subjectOf(ref), &PlacedItem::id);
  };
  EXPECT_EQ(item({.document = 0, .page = 1}).flags, itemMarked | itemFocus);
  EXPECT_EQ(item({.document = 1, .page = 0}).flags, itemMarked);
  EXPECT_EQ(item({.document = 0, .page = 0}).flags, 0U);
  EXPECT_EQ(in.active->ends[in.active->anchor].page,
            (PageRef{.document = 0, .page = 1}));
}

TEST(PageViewTest, aViewWithNoTransitionLeavesTheDefaultEase) {
  const TwoDocuments docs;
  const ColumnView view;
  const PageLayoutInput from{.catalog = docs.catalog,
                             .cursor  = {.page = {.document = 0, .page = 0}}};
  const PageLayoutInput to{.catalog = docs.catalog,
                           .cursor  = {.page = {.document = 1, .page = 0}}};
  EXPECT_NE(from.cursor, to.cursor);

  LayoutSink out;
  view.transition(from, to, out);
  EXPECT_TRUE(out.hints().empty());
  EXPECT_TRUE(out.items().empty());
}

} // namespace

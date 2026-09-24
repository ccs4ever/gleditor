#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <ranges>
#include <string>
#include <variant>
#include <vector>

#include "common/xanadu/link_occurrences.hpp"
#include "common/xanadu/link_views.hpp"
#include "common/xanadu/store.hpp"

namespace {

using xanadu::Coverage;
using xanadu::Extent;
using xanadu::LinkSide;
using xanadu::PrimediaSpan;

// "Alpha beta gamma delta. one two three." -- the offsets below are into it.
constexpr std::uint32_t kBaseLength = 38;
constexpr std::uint32_t kThreeAt    = 32;
constexpr std::uint32_t kWordLength = 5;

/**
 * One 2x3 link over one document and two cells, plus a second link that
 * overlaps its first left member.
 *
 * Left members are "Alpha" and "gamma", disjoint so that a covering extent
 * would swallow " beta ". Right members are "one", "two", "three"; "three" is
 * quoted twice back to back after the original, once whole in a cell and once
 * in part in another, and typed afresh at the end so the same words exist at
 * an address no member names.
 */
struct TwoByThree {
  std::unique_ptr<xanadu::Store> store = std::make_unique<xanadu::Store>();
  xanadu::MicroversionId base;
  xanadu::MicroversionId head;
  zigzag::CellRef wholeCell{zigzag::noCell};
  zigzag::CellRef partCell{zigzag::noCell};
  zigzag::CellRef link{zigzag::noCell};
  zigzag::CellRef overlapping{zigzag::noCell};
  PrimediaSpan three;
};

zigzag::CellRef linkOwnedBy(const xanadu::Store &store,
                            const std::string &owner) {
  const auto found = gleditor::firstOf(
      store.linkView() | std::views::filter(xanadu::links::ownedBy(owner)));
  return found ? found->id : zigzag::noCell;
}

TwoByThree makeTwoByThree() {
  TwoByThree f;
  auto &store = *f.store;
  f.base      = store.insert({}, 0, "Alpha beta gamma delta. one two three.");
  const auto base = store.rebuild(f.base);
  const auto word = [&](std::uint32_t at, std::uint32_t length) {
    const auto spans = base.spansFor(at, length);
    EXPECT_EQ(spans.size(), 1U);
    return spans.front();
  };
  f.three = word(kThreeAt, kWordLength);

  auto at =
      store.transclude(f.base, kBaseLength, f.base, kThreeAt, kWordLength);
  at = store.transclude(at, kBaseLength + kWordLength, f.base, kThreeAt,
                        kWordLength);
  at = store.insert(at, kBaseLength + 2 * kWordLength, " three");

  at          = store.sliceGenesis(at);
  at          = store.makeCell(at, f.three);
  f.wholeCell = store.cellRefOf(at);
  at          = store.makeCell(at, PrimediaSpan{.scroll = f.three.scroll,
                                                .start  = f.three.start,
                                                .length = 3});
  f.partCell  = store.cellRefOf(at);

  xanadu::Link link;
  link.type  = xanadu::LinkType::Comment;
  link.owner = "twobythree";
  link.left  = {word(0, 5), word(11, 5)};
  link.right = {word(24, 3), word(28, 3), f.three};
  at         = store.addLink(at, link);

  xanadu::Link other;
  other.type  = xanadu::LinkType::Comment;
  other.owner = "overlap";
  other.left  = {word(0, 10)};
  other.right = {word(28, 3)};
  at          = store.addLink(at, other);

  f.head        = at;
  f.link        = linkOwnedBy(store, "twobythree");
  f.overlapping = linkOwnedBy(store, "overlap");
  return f;
}

std::vector<Extent> documentRanges(const xanadu::LinkMember &member) {
  std::vector<Extent> out;
  for (const auto &occurrence : member.occurrences) {
    if (const auto *site =
            std::get_if<xanadu::DocumentSite>(&occurrence.site)) {
      out.push_back(site->range);
    }
  }
  return out;
}

std::vector<xanadu::CellSite> cellSites(const xanadu::LinkMember &member) {
  std::vector<xanadu::CellSite> out;
  for (const auto &occurrence : member.occurrences) {
    if (const auto *site = std::get_if<xanadu::CellSite>(&occurrence.site)) {
      out.push_back(*site);
    }
  }
  return out;
}

} // namespace

TEST(LinkOccurrencesTest, BackToBackQuotationsStayTwoMatches) {
  const PrimediaSpan member{.start = 100, .length = 5};
  const std::vector<PrimediaSpan> pieces{
      {.start = 0, .length = 10}, member, member, {.start = 10, .length = 3}};

  const auto matches = xanadu::exactOccurrences(pieces, member);

  ASSERT_EQ(matches.size(), 2U);
  EXPECT_EQ(matches[0].range, (Extent{.start = 10, .end = 15}));
  EXPECT_EQ(matches[1].range, (Extent{.start = 15, .end = 20}));
  EXPECT_EQ(matches[0].coverage, Coverage::Full);
  EXPECT_EQ(matches[1].coverage, Coverage::Full);
}

TEST(LinkOccurrencesTest, ContinuingAddressesAcrossPiecesAreOneMatch) {
  const PrimediaSpan member{.start = 100, .length = 6};
  const std::vector<PrimediaSpan> pieces{{.start = 100, .length = 2},
                                         {.start = 102, .length = 4}};

  const auto matches = xanadu::exactOccurrences(pieces, member);

  ASSERT_EQ(matches.size(), 1U);
  EXPECT_EQ(matches[0].range, (Extent{.start = 0, .end = 6}));
  EXPECT_EQ(matches[0].coverage, Coverage::Full);
}

TEST(LinkOccurrencesTest, RearrangedHalvesAreTwoPartialMatches) {
  const PrimediaSpan member{.start = 100, .length = 6};
  const std::vector<PrimediaSpan> pieces{{.start = 103, .length = 3},
                                         {.start = 100, .length = 3}};

  const auto matches = xanadu::exactOccurrences(pieces, member);

  ASSERT_EQ(matches.size(), 2U);
  EXPECT_EQ(matches[0].coverage, Coverage::Partial);
  EXPECT_EQ(matches[1].coverage, Coverage::Partial);
}

TEST(LinkOccurrencesTest, OtherScrollAndEmptyMemberMatchNothing) {
  const std::vector<PrimediaSpan> pieces{{.start = 100, .length = 5}};
  EXPECT_TRUE(xanadu::exactOccurrences(
                  pieces, PrimediaSpan{.scroll = 7, .start = 100, .length = 5})
                  .empty());
  EXPECT_TRUE(
      xanadu::exactOccurrences(pieces, PrimediaSpan{.start = 100}).empty());
}

TEST(LinkOccurrencesTest, TwoByThreeKeepsMembersSeparateAndOrdered) {
  const auto f      = makeTwoByThree();
  const auto &store = *f.store;
  ASSERT_NE(f.link, zigzag::noCell);
  const auto head = store.rebuild(f.head);
  const std::vector<xanadu::DocumentView> documents{
      {.store = store.documentId(), .version = f.head, .text = head}};

  const auto resolved =
      xanadu::resolveLinkOccurrences(store, f.link, documents, {});

  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->key.authority, store.documentId());
  EXPECT_EQ(resolved->key.id, f.link);
  ASSERT_EQ(resolved->left.size(), 2U);
  ASSERT_EQ(resolved->right.size(), 3U);
  for (std::uint32_t i = 0; i < 3; ++i) {
    EXPECT_EQ(resolved->right[i].index, i);
    EXPECT_EQ(resolved->right[i].side, LinkSide::Right);
  }

  // Disjoint left members: nothing spans the gap between "Alpha" and "gamma",
  // even though a second link covers "Alpha beta".
  EXPECT_EQ(documentRanges(resolved->left[0]),
            (std::vector<Extent>{{.start = 0, .end = 5}}));
  EXPECT_EQ(documentRanges(resolved->left[1]),
            (std::vector<Extent>{{.start = 11, .end = 16}}));
  EXPECT_EQ(documentRanges(resolved->right[0]),
            (std::vector<Extent>{{.start = 24, .end = 27}}));
  EXPECT_EQ(documentRanges(resolved->right[1]),
            (std::vector<Extent>{{.start = 28, .end = 31}}));
}

TEST(LinkOccurrencesTest, RepeatedQuotationsAreSeparateOccurrences) {
  const auto f      = makeTwoByThree();
  const auto &store = *f.store;
  const auto head   = store.rebuild(f.head);
  const std::vector<xanadu::DocumentView> documents{
      {.store = store.documentId(), .version = f.head, .text = head}};

  const auto resolved =
      xanadu::resolveLinkOccurrences(store, f.link, documents, {});

  ASSERT_TRUE(resolved.has_value());
  // The original, then the back-to-back pair; the retyped "three" at the end
  // has the same words at an address no member names.
  const std::vector<Extent> expected{
      {.start = kThreeAt, .end = kThreeAt + kWordLength},
      {.start = kBaseLength, .end = kBaseLength + kWordLength},
      {.start = kBaseLength + kWordLength,
       .end   = kBaseLength + 2 * kWordLength}};
  EXPECT_EQ(documentRanges(resolved->right[2]), expected);
  EXPECT_TRUE(
      std::ranges::all_of(resolved->right[2].occurrences, [](const auto &o) {
        return Coverage::Full == o.coverage;
      }));

  // The position-only join is what this query exists to avoid.
  EXPECT_EQ(head.occurrencesOf(f.three).size(), 2U);
}

TEST(LinkOccurrencesTest, CellContentOccurrencesCarryExactRangeAndCoverage) {
  const auto f        = makeTwoByThree();
  const auto &store   = *f.store;
  const auto manifold = store.rebuildManifold(f.head);
  const std::vector<zigzag::CellRef> visible{f.wholeCell, f.partCell};
  const std::vector<xanadu::CellView> cells{{.store    = store.documentId(),
                                             .version  = f.head,
                                             .manifold = manifold,
                                             .cells    = visible}};

  const auto resolved =
      xanadu::resolveLinkOccurrences(store, f.link, {}, cells);

  ASSERT_TRUE(resolved.has_value());
  const auto sites = cellSites(resolved->right[2]);
  ASSERT_EQ(sites.size(), 2U);
  EXPECT_EQ(sites[0].cell, f.wholeCell);
  EXPECT_EQ(sites[0].range, (Extent{.start = 0, .end = kWordLength}));
  EXPECT_EQ(sites[1].cell, f.partCell);
  EXPECT_EQ(sites[1].range, (Extent{.start = 0, .end = 3}));
  EXPECT_EQ(resolved->right[2].occurrences[0].coverage, Coverage::Full);
  EXPECT_EQ(resolved->right[2].occurrences[1].coverage, Coverage::Partial);
  EXPECT_FALSE(resolved->left[0].inView());
}

TEST(LinkOccurrencesTest, MembersOutOfViewKeepTheirPlace) {
  const auto f      = makeTwoByThree();
  const auto &store = *f.store;

  const auto resolved = xanadu::resolveLinkOccurrences(store, f.link, {}, {});

  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->left.size(), 2U);
  EXPECT_EQ(resolved->right.size(), 3U);
  EXPECT_TRUE(
      std::ranges::none_of(resolved->right, &xanadu::LinkMember::inView));
  EXPECT_EQ(resolved->right[2].span, f.three);
}

TEST(LinkOccurrencesTest, OlderVersionShowsOnlyTheOriginal) {
  const auto f      = makeTwoByThree();
  const auto &store = *f.store;
  const auto base   = store.rebuild(f.base);
  const std::vector<xanadu::DocumentView> documents{
      {.store = store.documentId(), .version = f.base, .text = base}};

  const auto resolved =
      xanadu::resolveLinkOccurrences(store, f.link, documents, {});

  ASSERT_TRUE(resolved.has_value());
  ASSERT_EQ(resolved->right[2].occurrences.size(), 1U);
  EXPECT_EQ(
      std::get<xanadu::DocumentSite>(resolved->right[2].occurrences[0].site)
          .version,
      f.base);
}

TEST(LinkOccurrencesTest, UnknownLinkIsRefusedByName) {
  const auto f = makeTwoByThree();

  const auto resolved =
      xanadu::resolveLinkOccurrences(*f.store, f.wholeCell, {}, {});

  ASSERT_FALSE(resolved.has_value());
  EXPECT_EQ(resolved.error(), xanadu::LinkQueryError::LinkNotFound);
}

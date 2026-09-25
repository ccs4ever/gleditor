#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <ranges>
#include <string>
#include <variant>
#include <vector>

#include "common/xanadu/link_occurrences.hpp"
#include "two_by_three_fixture.hpp"

namespace {

using xanadu::Coverage;
using xanadu::Extent;
using xanadu::LinkSide;
using xanadu::PrimediaSpan;
using xuzz_test::kBaseLength;
using xuzz_test::kThreeAt;
using xuzz_test::kWordLength;
using xuzz_test::makeTwoByThree;

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

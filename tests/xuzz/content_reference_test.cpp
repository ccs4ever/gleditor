#include <gtest/gtest.h>

#include <limits>

#include "common/xanadu/content_reference.hpp"
#include "common/xanadu/store.hpp"

namespace {

xanadu::ContentReference reference() {
  return {.authority  = xanadu::DocumentId{},
          .store      = xanadu::DocumentId{},
          .version    = xanadu::MicroversionId::parse("2a3"),
          .frame      = xanadu::DocumentFrame{.birth = 7},
          .ranges     = {{.source = {.scroll = 3, .start = 100, .length = 4},
                          .range  = {.start = 5, .end = 9}},
                         {.source = {.scroll = 3, .start = 200, .length = 2},
                          .range  = {.start = 15, .end = 17}}},
          .occurrence = std::nullopt};
}

xanadu::LinkKey authorLink(xanadu::Store &store,
                           const xanadu::MicroversionId &parent,
                           const xanadu::LinkMember &member) {
  xanadu::Link link;
  auto &endset = member.side == xanadu::LinkSide::Left ? link.left : link.right;
  endset.resize(member.index + 1, member.span);
  store.addLink(parent, std::move(link));
  return {.authority = store.documentId(), .id = store.links().rbegin()->first};
}

} // namespace

TEST(ContentReference, RetainsDisjointRangesGapsAndPinnedFrame) {
  const auto exact = reference();
  ASSERT_TRUE(xanadu::validateContentReference(exact));
  EXPECT_EQ(exact.ranges.size(), 2U);
  EXPECT_EQ(exact.ranges[0].range.end, 9U);
  EXPECT_EQ(exact.ranges[1].range.start, 15U);
  EXPECT_EQ(exact.version.str(), "2a3");
  EXPECT_EQ(std::get<xanadu::DocumentFrame>(exact.frame).birth, 7U);
}

TEST(ContentReference, RejectsMalformedLengthsOverlapOrderAndAddressOverflow) {
  auto exact                = reference();
  exact.ranges[0].range.end = 4;
  EXPECT_EQ(xanadu::validateContentReference(exact).error(),
            xanadu::ContentReferenceError::InvalidRange);
  exact                         = reference();
  exact.ranges[0].source.length = 5;
  EXPECT_EQ(xanadu::validateContentReference(exact).error(),
            xanadu::ContentReferenceError::InvalidRange);
  exact                 = reference();
  exact.ranges[1].range = {.start = 8, .end = 10};
  EXPECT_EQ(xanadu::validateContentReference(exact).error(),
            xanadu::ContentReferenceError::OverlappingRanges);
  exact = reference();
  std::swap(exact.ranges[0], exact.ranges[1]);
  EXPECT_EQ(xanadu::validateContentReference(exact).error(),
            xanadu::ContentReferenceError::OverlappingRanges);
  exact                        = reference();
  exact.ranges[0].source.start = std::numeric_limits<std::uint64_t>::max();
  EXPECT_EQ(xanadu::validateContentReference(exact).error(),
            xanadu::ContentReferenceError::InvalidSource);
}

TEST(ContentReference, RejectsUnaddressedContentAndMissingState) {
  auto exact                    = reference();
  exact.ranges[0].source.scroll = xanadu::scratchScroll;
  EXPECT_EQ(xanadu::validateContentReference(exact).error(),
            xanadu::ContentReferenceError::InvalidSource);
  exact         = reference();
  exact.version = {};
  EXPECT_EQ(xanadu::validateContentReference(exact).error(),
            xanadu::ContentReferenceError::MissingState);
  exact = reference();
  exact.ranges.clear();
  EXPECT_EQ(xanadu::validateContentReference(exact).error(),
            xanadu::ContentReferenceError::EmptySelection);
}

TEST(ContentReference, LegacyConcatextIsExplicitAndSeparateFromCellBirth) {
  auto exact  = reference();
  exact.frame = xanadu::DocumentFrame{.birth = std::nullopt};
  EXPECT_TRUE(xanadu::validateContentReference(exact));
  exact.frame = xanadu::DocumentFrame{.birth = 0};
  EXPECT_EQ(xanadu::validateContentReference(exact).error(),
            xanadu::ContentReferenceError::WrongFrame);
  exact.frame = xanadu::CellFrame{.cell = zigzag::noCell};
  EXPECT_EQ(xanadu::validateContentReference(exact).error(),
            xanadu::ContentReferenceError::WrongFrame);
}

TEST(ContentReference,
     ConvertsRealRepeatedOccurrencesWithoutChangingAddresses) {
  xanadu::Store store;
  const auto first   = store.insert({}, 0, "same");
  const auto span    = store.rebuild(first).pieces().front();
  const auto twice   = store.transclude(first, 4, first, 0, 4);
  const auto text    = store.rebuild(twice);
  const auto matches = xanadu::exactOccurrences(text.pieces(), span);
  ASSERT_EQ(matches.size(), 2U);
  xanadu::LinkMember member{
      .side = xanadu::LinkSide::Right, .index = 4, .span = span};
  for (const auto &match : matches) {
    member.occurrences.push_back(
        {.site     = xanadu::DocumentSite{.store   = store.documentId(),
                                          .version = twice,
                                          .range   = match.range},
         .coverage = match.coverage});
  }
  const auto link = authorLink(store, twice, member);
  const auto a    = xanadu::contentReferenceFor(
      store, link, member, 0, xanadu::DocumentFrame{.birth = std::nullopt});
  const auto b = xanadu::contentReferenceFor(
      store, link, member, 1, xanadu::DocumentFrame{.birth = std::nullopt});
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  EXPECT_EQ(a->ranges.front().source, b->ranges.front().source);
  EXPECT_NE(a->ranges.front().range, b->ranges.front().range);
  EXPECT_NE(a->occurrence, b->occurrence);
  EXPECT_NE(*a, *b);
  EXPECT_EQ(a->occurrence->side, xanadu::LinkSide::Right);
  EXPECT_EQ(a->occurrence->member, 4U);
}

TEST(ContentReference, EqualBytesAtDifferentAddressesStayDistinct) {
  xanadu::Store store;
  const auto state  = store.insert({}, 0, "sameXsame");
  const auto text   = store.rebuild(state);
  const auto first  = text.spansFor(0, 4);
  const auto second = text.spansFor(5, 4);
  ASSERT_EQ(first.size(), 1U);
  ASSERT_EQ(second.size(), 1U);
  EXPECT_NE(first.front(), second.front());
  auto a   = reference();
  a.ranges = {{.source = first.front(), .range = {.start = 0, .end = 4}}};
  auto b   = a;
  b.ranges.front().source = second.front();
  ASSERT_TRUE(xanadu::validateContentReference(a));
  ASSERT_TRUE(xanadu::validateContentReference(b));
  EXPECT_NE(a, b);
}

TEST(ContentReference, ConversionChecksCellFrameAndPartialMapping) {
  xanadu::Store sourceRegistry;
  const auto base      = sourceRegistry.insert({}, 0, "source registry");
  const auto authority = sourceRegistry.documentId();
  const xanadu::DocumentId destination;
  xanadu::LinkMember member{
      .side        = xanadu::LinkSide::Left,
      .index       = 2,
      .span        = sourceRegistry.rebuild(base).pieces()[0].slice(0, 4),
      .occurrences = {
          {.site =
               xanadu::CellSite{.store   = destination,
                                .version = xanadu::MicroversionId::parse("4b2"),
                                .cell    = 8,
                                .range   = {.start = 2, .end = 6}},
           .coverage = xanadu::Coverage::Full}}};
  const auto link  = authorLink(sourceRegistry, base, member);
  const auto exact = xanadu::contentReferenceFor(
      sourceRegistry, link, member, 0, xanadu::CellFrame{.cell = 8});
  ASSERT_TRUE(exact);
  EXPECT_EQ(exact->authority, authority);
  EXPECT_EQ(exact->store, destination);
  EXPECT_EQ(exact->version.str(), "4b2");
  EXPECT_EQ(xanadu::contentReferenceFor(sourceRegistry, link, member, 0,
                                        xanadu::CellFrame{.cell = 9})
                .error(),
            xanadu::ContentReferenceError::WrongFrame);
  EXPECT_EQ(
      xanadu::contentReferenceFor(sourceRegistry, link, member, 0,
                                  xanadu::DocumentFrame{.birth = std::nullopt})
          .error(),
      xanadu::ContentReferenceError::WrongFrame);
  member.occurrences.front().coverage = xanadu::Coverage::Partial;
  EXPECT_EQ(xanadu::contentReferenceFor(sourceRegistry, link, member, 0,
                                        xanadu::CellFrame{.cell = 8})
                .error(),
            xanadu::ContentReferenceError::PartialOccurrence);
  EXPECT_EQ(xanadu::contentReferenceFor(sourceRegistry, link, member, 1,
                                        xanadu::CellFrame{.cell = 8})
                .error(),
            xanadu::ContentReferenceError::OccurrenceNotFound);
}

TEST(ContentReference, RejectsPackagePlaceholderAndDifferentSourceRegistry) {
  xanadu::Store sourceRegistry;
  const auto base = sourceRegistry.insert({}, 0, "source");
  xanadu::LinkMember member{.span = sourceRegistry.rebuild(base).pieces()[0]};
  member.occurrences.push_back(
      {.site = xanadu::DocumentSite{.store   = sourceRegistry.documentId(),
                                    .version = base,
                                    .range   = {.start = 0, .end = 6}}});
  const auto link = authorLink(sourceRegistry, base, member);
  xanadu::Store otherRegistry;
  EXPECT_EQ(
      xanadu::contentReferenceFor(otherRegistry, link, member, 0,
                                  xanadu::DocumentFrame{.birth = std::nullopt})
          .error(),
      xanadu::ContentReferenceError::SourceRegistryMismatch);
  member.span.start += 10;
  EXPECT_EQ(
      xanadu::contentReferenceFor(sourceRegistry, link, member, 0,
                                  xanadu::DocumentFrame{.birth = std::nullopt})
          .error(),
      xanadu::ContentReferenceError::AuthoredMemberMismatch);
}

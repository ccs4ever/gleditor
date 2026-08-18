/**
 * @file adoption.cpp
 * @brief Reading somebody else's published document, and linking to it.
 *
 * Publishing is one direction and it was the easier one: a document here is
 * rewritten into names that mean something everywhere. This is the other, and
 * it is what makes a published document a thing rather than a file -- a
 * machine that never wrote it can read it, quote it, and link to it, and the
 * link is a real link because both ends are addresses rather than positions.
 *
 * The case worth being careful about is the asymmetric one: a document written
 * here and never published, linked to a passage of a document published
 * elsewhere. Nothing about that document has a global name, and it does not
 * need one -- what a link relates is content, and only the end that has to
 * travel has to be sayable elsewhere. A design that quietly required both ends
 * to be published would make linking a privilege of publishers, which is the
 * arrangement Xanadu exists to argue against.
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <xudu/core/link_layout.hpp>
#include <xudu/core/publication.hpp>
#include <xudu/core/store.hpp>
#include <xudu/core/swarm.hpp>

namespace {

using xudu::GlobalSpan;
using xudu::HalfLink;
using xudu::Link;
using xudu::LinkedPair;
using xudu::LinkType;
using xudu::MicroversionId;
using xudu::Publication;
using xudu::Scroll;
using xudu::ScrollSegment;
using xudu::Store;
using xudu::Version;

/// A published permascroll: named by a key, so it is the same scroll wherever
/// it is mentioned.
Scroll namedScroll(const xudu::PublicKey &key, std::string salt,
                   const std::uint64_t length) {
  Scroll scroll;
  scroll.publisher = key;
  scroll.salt      = std::move(salt);
  ScrollSegment segment;
  segment.at     = 0;
  segment.length = length;
  segment.path   = "permascroll";
  scroll.segments.push_back(segment);
  return scroll;
}

/// A document that quotes a stretch of a published scroll.
MicroversionId quoting(Store &store, const Scroll &scroll,
                       const std::uint64_t from, const std::uint64_t length) {
  return store.transcludeExternal(MicroversionId{}, 0, scroll, from, length);
}

/// Somebody else's document: a quotation of their permascroll, published.
struct Elsewhere {
  xudu::MutableKeys keys;
  Scroll scroll;
  Publication pub;
};
Elsewhere published(const std::uint64_t from   = 100,
                    const std::uint64_t length = 200) {
  Elsewhere out;
  out.keys   = xudu::createMutableKeys();
  out.scroll = namedScroll(out.keys.publicKey, "permascroll", 1000);
  Store theirs;
  const auto version = quoting(theirs, out.scroll, from, length);
  out.pub = xudu::publish(theirs, version, out.keys, "essay", "Their Essay", 1,
                          1700000000);
  return out;
}

/// A stand-in for the OpenPGP-signed authorship record. What sealing insists
/// on is that there be one; whether gpg made it is provenance.cpp's business,
/// and needing a keyring would make every test here need one.
xudu::SignedProvenance signedBy(const std::string &name) {
  xudu::Provenance record;
  record.author.name  = name;
  record.author.email = name + "@example.org";
  return {record.toYaml(), "-----BEGIN PGP SIGNATURE-----\n(for the test)\n"};
}

std::vector<const Version *> viewing(const std::vector<Version> &versions) {
  std::vector<const Version *> out;
  out.reserve(versions.size());
  for (const auto &one : versions) {
    out.push_back(&one);
  }
  return out;
}

} // namespace

// Reading one in: the pieces become a version here, pointing at the same
// content the publisher's own document points at rather than at a copy of it.
TEST(AdoptionTest,
     aPublishedDocumentBecomesAVersionOnAMachineThatNeverWroteIt) {
  const auto theirs = published();

  Store mine;
  const auto taken = xudu::adopt(mine, theirs.pub);
  EXPECT_FALSE(taken.version.isZero());
  EXPECT_EQ(taken.scrolls, 1U);

  const auto here = mine.rebuild(taken.version);
  EXPECT_EQ(here.length(), theirs.pub.length());

  // The addresses came across untouched, which is what makes this a reading of
  // their document and not a document that looks like it.
  ASSERT_EQ(here.pieces().size(), theirs.pub.pieces.size());
  const auto said = xudu::globalise(mine, here.pieces()[0]);
  ASSERT_TRUE(said.has_value());
  EXPECT_EQ(*said, theirs.pub.pieces[0]);
}

// The case this exists for. A document written here, never published and with
// no global name of any kind, carries a link whose far end is a passage of
// somebody else's published document -- and both ends of it are found, which
// is what draws it.
TEST(AdoptionTest, anUnpublishedDocumentCanLinkToAPublishedOne) {
  const auto theirs = published();

  Store mine;
  const auto reading = xudu::adopt(mine, theirs.pub).version;

  // Typed here. Nothing about it is published: its content is at an offset in
  // this machine's spool and nowhere else.
  auto notes = mine.insert(MicroversionId{}, 0, "This passage is the crux.");
  const auto notesText = mine.rebuild(notes);
  EXPECT_FALSE(xudu::globalise(mine, notesText.pieces()[0]).has_value())
      << "content typed here should have no global address until it is sealed";

  Link link;
  link.type  = LinkType::Comment;
  link.owner = "me";
  link.left  = notesText.spansFor(0, 12);
  // The far end is a passage of their document, named by their scroll -- which
  // this store knows about only because their manifest said where it is.
  link.right = mine.rebuild(reading).spansFor(20, 40);
  ASSERT_FALSE(link.right.empty());
  EXPECT_NE(link.right[0].scroll, xudu::localScroll);
  notes = mine.addLink(notes, link);

  const std::vector<Version> open{mine.rebuild(notes), mine.rebuild(reading)};
  std::vector<LinkedPair> between;
  std::vector<HalfLink> leaving;
  xudu::placeLinks(mine.links(), viewing(open), between, leaving);

  ASSERT_EQ(between.size(), 1U);
  EXPECT_EQ(between[0].from.doc, 0U) << "the note is where the link was made";
  EXPECT_EQ(between[0].from.start, 0U);
  EXPECT_EQ(between[0].from.end, 12U);
  EXPECT_EQ(between[0].to.doc, 1U) << "and it points into their document";
  EXPECT_EQ(between[0].to.start, 20U);
  EXPECT_EQ(between[0].to.end, 60U);
  EXPECT_TRUE(leaving.empty());
}

// The same link, on a machine that has read their document and knows nothing
// of mine: the end in my unpublished document is nowhere here, and the end in
// theirs still lands. A link is not all-or-nothing.
TEST(AdoptionTest, theEndInAPublishedDocumentLandsEvenWhenTheOtherIsNotHere) {
  const auto theirs = published();

  Store mine;
  const auto reading = xudu::adopt(mine, theirs.pub).version;
  const auto notes   = mine.insert(MicroversionId{}, 0, "A private note.");
  Link link;
  link.type  = LinkType::Comment;
  link.owner = "me";
  link.left  = mine.rebuild(notes).spansFor(0, 7);
  link.right = mine.rebuild(reading).spansFor(0, 30);
  static_cast<void>(mine.addLink(notes, link));

  // Only their document open.
  const std::vector<Version> open{mine.rebuild(reading)};
  std::vector<LinkedPair> between;
  std::vector<HalfLink> leaving;
  xudu::placeLinks(mine.links(), viewing(open), between, leaving);

  EXPECT_TRUE(between.empty());
  ASSERT_EQ(leaving.size(), 1U);
  EXPECT_EQ(leaving[0].here.doc, 0U);
  EXPECT_EQ(leaving[0].here.start, 0U);
  EXPECT_EQ(leaving[0].here.end, 30U);
}

// And the published-to-published case: once my own content is sealed, the same
// link publishes with both ends global, and a third machine that has never
// seen either store reads both documents and finds the link between them.
TEST(AdoptionTest, aLinkBetweenTwoPublishedDocumentsCrossesToAThirdMachine) {
  const auto theirs = published();

  Store mine;
  const auto reading = xudu::adopt(mine, theirs.pub).version;
  auto notes = mine.insert(MicroversionId{}, 0, "This passage is the crux.");
  Link link;
  link.type  = LinkType::Comment;
  link.owner = "me";
  link.left  = mine.rebuild(notes).spansFor(0, 12);
  link.right = mine.rebuild(reading).spansFor(20, 40);
  notes      = mine.addLink(notes, link);

  const auto myKeys = xudu::createMutableKeys();
  // Sealing needs a signed record of who is doing it, so the test says who --
  // see provenance.cpp for what that record is and why it comes first.
  const auto sealed =
      xudu::sealLocalSpool(mine, myKeys, "notes", "", signedBy("me"));
  const auto myPub = xudu::publish(mine, notes, myKeys, "notes", "My Notes", 1,
                                   1700000200, &sealed.scroll);

  // Both ends said globally, and the far one is their scroll rather than
  // anything of mine -- the link points where it always pointed.
  ASSERT_EQ(myPub.links.size(), 1U);
  const auto theirKey =
      xudu::scrollKeyFor(theirs.keys.publicKey, "permascroll");
  ASSERT_EQ(myPub.links[0].right.size(), 1U);
  EXPECT_EQ(myPub.links[0].right[0].scroll, theirKey);
  EXPECT_EQ(myPub.links[0].right[0].start, 120U)
      << "twenty bytes into a document that starts a hundred into the scroll";
  ASSERT_EQ(myPub.links[0].left.size(), 1U);
  EXPECT_EQ(myPub.links[0].left[0].scroll,
            xudu::scrollKeyFor(myKeys.publicKey, "notes"));

  // A third machine, which has met neither of us.
  Store third;
  const auto theirReading = xudu::adopt(third, theirs.pub).version;
  const auto myReading    = xudu::adopt(third, myPub).version;

  const std::vector<Version> open{third.rebuild(myReading),
                                  third.rebuild(theirReading)};
  std::vector<LinkedPair> between;
  std::vector<HalfLink> leaving;
  xudu::placeLinks(third.links(), viewing(open), between, leaving);

  ASSERT_EQ(between.size(), 1U);
  EXPECT_EQ(between[0].from.doc, 0U);
  EXPECT_EQ(between[0].from.end, 12U);
  EXPECT_EQ(between[0].to.doc, 1U);
  EXPECT_EQ(between[0].to.start, 20U);
  EXPECT_EQ(between[0].to.end, 60U);
}

// A link arriving twice -- because two publications carry it, or because one
// was read twice -- is one link. Otherwise a document read again would show
// every relation it has doubled.
TEST(AdoptionTest, readingTheSamePublicationTwiceDoesNotDoubleItsLinks) {
  const auto keys   = xudu::createMutableKeys();
  const auto scroll = namedScroll(keys.publicKey, "permascroll", 1000);
  Store theirs;
  const auto scrollId = theirs.addScroll(scroll);
  auto version        = quoting(theirs, scroll, 0, 300);
  Link link;
  link.type  = LinkType::Comment;
  link.owner = "them";
  link.left.push_back(xudu::PrimediaSpan{scrollId, 0, 40});
  link.right.push_back(xudu::PrimediaSpan{scrollId, 200, 40});
  version        = theirs.addLink(version, link);
  const auto pub = xudu::publish(theirs, version, keys, "essay", "Their Essay",
                                 1, 1700000000);
  ASSERT_EQ(pub.links.size(), 1U);

  Store mine;
  const auto first = xudu::adopt(mine, pub);
  EXPECT_EQ(first.links, 1U);
  EXPECT_EQ(mine.links().size(), 1U);

  const auto again = xudu::adopt(mine, pub);
  EXPECT_EQ(again.links, 0U) << "the same link arriving again is one link";
  EXPECT_EQ(mine.links().size(), 1U);
  // The second reading is still a document of its own, and the scroll is not
  // recorded twice, or the two readings would not be found to share content.
  EXPECT_NE(again.version, first.version);
  EXPECT_EQ(again.scrolls, 0U);
  EXPECT_EQ(mine.scrolls().size(), 1U);
}

// Reading is where a forgery has to be caught: after this the document is
// indistinguishable from one written here.
TEST(AdoptionTest, aManifestThatDoesNotVerifyIsNotRead) {
  const auto theirs = published();
  auto altered      = theirs.pub;
  altered.title     = "Something Else";

  Store mine;
  EXPECT_THROW(static_cast<void>(xudu::adopt(mine, altered)),
               std::runtime_error);
  EXPECT_EQ(mine.opCount(), 0U) << "and nothing of it was taken in";
}

// A manifest can be honestly signed and still be unreadable: one that points
// at a scroll it does not say the whereabouts of is a document with a hole in
// it, and saying so beats showing text with a silent gap.
TEST(AdoptionTest, aPublicationThatDoesNotSayWhereItsContentIsIsRefused) {
  const auto keys   = xudu::createMutableKeys();
  const auto scroll = namedScroll(keys.publicKey, "permascroll", 1000);
  Store theirs;
  const auto version = quoting(theirs, scroll, 0, 100);
  auto pub = xudu::publish(theirs, version, keys, "essay", "Their Essay", 1,
                           1700000000);

  pub.scrolls.clear();
  // Signed again over what it now says, so this is not the forgery case.
  pub.signature =
      xudu::signMutableItem(xudu::publicationSigningBuffer(pub), keys);
  ASSERT_TRUE(xudu::verifyPublication(pub));

  Store mine;
  EXPECT_THROW(static_cast<void>(xudu::adopt(mine, pub)), std::runtime_error);
}

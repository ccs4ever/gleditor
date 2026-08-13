/**
 * @file publication.cpp
 * @brief What has to be true of a document that has left its machine.
 *
 * Three things, and each has a way of failing quietly that these are here to
 * make loud.
 *
 * It has to be readable elsewhere: every address in it names content by a name
 * that means the same thing on another machine, or the document arrives as a
 * list of numbers nobody can resolve.
 *
 * Its authorship has to survive: a manifest that has been altered, or was
 * never that publisher's, must fail to read rather than read with a warning.
 *
 * And links and transclusions have to reach across it: two documents that
 * quote the same passage must be discoverable as doing so, without either
 * publisher having heard of the other.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <map>
#include <string>

#include <xudu/core/publication.hpp>
#include <xudu/core/store.hpp>
#include <xudu/core/torrent.hpp>
#include <xudu/core/swarm.hpp>

namespace {

using xudu::GlobalSpan;
using xudu::Library;
using xudu::MicroversionId;
using xudu::Publication;
using xudu::Scroll;
using xudu::ScrollSegment;

/// A scroll standing for somebody's published permascroll, named by a key so
/// that it is the same scroll on every machine.
Scroll namedScroll(const xudu::PublicKey &key, std::string salt,
                   const std::uint64_t length) {
  Scroll scroll;
  scroll.publisher = key;
  scroll.salt      = std::move(salt);
  ScrollSegment segment;
  segment.at       = 0;
  segment.length   = length;
  segment.path     = "permascroll";
  segment.torrent  = xudu::InfoHash{};
  scroll.segments.push_back(segment);
  return scroll;
}

/// A document that quotes @p length bytes of a published scroll, starting at
/// @p from. Transclusion rather than typing: what is published has to point at
/// content that already has an address.
MicroversionId quoting(xudu::Store &store, const Scroll &scroll,
                       const std::uint64_t from, const std::uint64_t length) {
  return store.transcludeExternal(MicroversionId{}, 0, scroll, from, length);
}

TEST(PublicationTest, aDocumentIsPublishedAsPointersAndReadsBackTheSame) {
  const auto keys   = xudu::createMutableKeys();
  const auto scroll = namedScroll(keys.publicKey, "permascroll", 1000);
  xudu::Store store;
  const auto version = quoting(store, scroll, 100, 50);

  const auto pub = xudu::publish(store, version, keys, "essay", "An Essay", 1,
                                 1700000000);
  EXPECT_EQ(pub.title, "An Essay");
  EXPECT_EQ(pub.length(), 50U);
  ASSERT_EQ(pub.pieces.size(), 1U);
  // Pointers, not bytes: what went out is where the content is, not a copy.
  EXPECT_EQ(pub.pieces[0].start, 100U);
  EXPECT_EQ(pub.pieces[0].length, 50U);
  EXPECT_EQ(pub.pieces[0].scroll,
            xudu::scrollKeyFor(keys.publicKey, "permascroll"));
  // And where to fetch them, or the addresses would be unresolvable.
  ASSERT_TRUE(pub.scrolls.contains(pub.pieces[0].scroll));

  const auto encoded = xudu::encodePublication(pub);
  const auto read    = xudu::decodePublication(encoded);
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->title, pub.title);
  EXPECT_EQ(read->pieces, pub.pieces);
  EXPECT_EQ(read->publisher, pub.publisher);
  EXPECT_EQ(read->name(), pub.name());
}

TEST(PublicationTest, aManifestThatWasAlteredDoesNotRead) {
  const auto keys   = xudu::createMutableKeys();
  const auto scroll = namedScroll(keys.publicKey, "permascroll", 1000);
  xudu::Store store;
  const auto version = quoting(store, scroll, 0, 40);
  const auto pub = xudu::publish(store, version, keys, "essay", "An Essay", 1,
                                 1700000000);

  // The title is the mildest thing anyone would think to change.
  auto altered  = pub;
  altered.title = "Somebody Else's Essay";
  EXPECT_FALSE(xudu::verifyPublication(altered));
  EXPECT_FALSE(xudu::decodePublication(xudu::encodePublication(altered))
                   .has_value());

  // And the part that would matter: what the document points at.
  auto moved = pub;
  moved.pieces[0].start += 1;
  EXPECT_FALSE(xudu::verifyPublication(moved));
  EXPECT_FALSE(
      xudu::decodePublication(xudu::encodePublication(moved)).has_value());

  // Claiming somebody else's name over one's own content fails the same way,
  // which is the property that makes authorship a fact rather than a field.
  const auto other = xudu::createMutableKeys();
  auto forged      = pub;
  forged.publisher = other.publicKey;
  EXPECT_FALSE(xudu::verifyPublication(forged));
}

TEST(PublicationTest, refusesToPublishContentThisMachineHasNotPublished) {
  // Typed here and never sealed: the pieces point at the local spool, which
  // has no address anyone else could resolve. Publishing that would produce a
  // document that arrives and cannot be read.
  xudu::Store store;
  const auto version = store.insert(MicroversionId{}, 0, "written just now");
  const auto keys    = xudu::createMutableKeys();
  EXPECT_THROW(static_cast<void>(xudu::publish(store, version, keys, "essay",
                                               "An Essay", 1, 1700000000)),
               std::runtime_error);
}

// The path from "written here" to "readable anywhere". Sealing gives the local
// spool the name it was missing; the offsets it already had become global
// addresses, so nothing written before the seal has to be rewritten.
TEST(PublicationTest, whatWasWrittenHereCanBeSealedAndThenPublished) {
  xudu::Store store;
  const auto version = store.insert(MicroversionId{}, 0, "Written here first.");
  const auto keys    = xudu::createMutableKeys();

  // Before sealing there is nothing a reader could resolve.
  EXPECT_THROW(static_cast<void>(xudu::publish(store, version, keys, "essay",
                                               "An Essay", 1, 1700000000)),
               std::runtime_error);

  const auto sealed = xudu::sealLocalSpool(store, keys, "permascroll", "");
  EXPECT_EQ(sealed.scroll.length(), store.primedia().bytes().size());
  EXPECT_TRUE(sealed.scroll.isNamed());
  // A real torrent: it parses, and its info hash is the one sealing reported.
  const auto meta = xudu::Metainfo::parse(sealed.torrentFile);
  EXPECT_EQ(meta.hash(), sealed.hash);
  EXPECT_EQ(meta.totalLength(), store.primedia().bytes().size());

  const auto pub = xudu::publish(store, version, keys, "essay", "An Essay", 1,
                                 1700000000, &sealed.scroll);
  ASSERT_FALSE(pub.pieces.empty());
  EXPECT_EQ(pub.pieces[0].scroll,
            xudu::scrollKeyFor(keys.publicKey, "permascroll"));
  EXPECT_EQ(pub.length(), std::string("Written here first.").size());
  // And the address is the offset it always had, which is why sealing does not
  // rewrite anything.
  EXPECT_EQ(pub.pieces[0].start, 0U);

  ASSERT_TRUE(xudu::decodePublication(xudu::encodePublication(pub)).has_value());
}

TEST(PublicationTest, twoDocumentsQuotingOnePassageAreFoundToShareIt) {
  // Two publishers who have never met, quoting one published permascroll.
  const auto author  = xudu::createMutableKeys();
  const auto scroll  = namedScroll(author.publicKey, "permascroll", 1000);
  const auto quoter  = xudu::createMutableKeys();

  xudu::Store originalStore;
  xudu::Store quotingStore;
  const auto originalVersion = quoting(originalStore, scroll, 100, 200);
  // The middle of it, written by somebody who only has the address.
  const auto quotingVersion = quoting(quotingStore, scroll, 150, 50);

  const auto first  = xudu::publish(originalStore, originalVersion, author,
                                    "original", "The Original", 1, 1700000000);
  const auto second = xudu::publish(quotingStore, quotingVersion, quoter,
                                    "quoting", "A Quotation", 1, 1700000100);

  Library library;
  ASSERT_TRUE(library.add(first));
  ASSERT_TRUE(library.add(second));
  EXPECT_EQ(library.size(), 2U);

  // What the second document shows, asked of the library, finds both -- and
  // says whereabouts in each, which is what lets a reader be taken there.
  const auto sightings = library.showing(second.pieces[0]);
  ASSERT_EQ(sightings.size(), 2U);
  std::map<std::string, Library::Sighting> byTitle;
  for (const auto &sighting : sightings) {
    byTitle.emplace(sighting.document->title, sighting);
  }
  ASSERT_TRUE(byTitle.contains("The Original"));
  ASSERT_TRUE(byTitle.contains("A Quotation"));
  // Fifty bytes starting fifty into the original, and the whole of the
  // quotation.
  EXPECT_EQ(byTitle["The Original"].start, 50U);
  EXPECT_EQ(byTitle["The Original"].end, 100U);
  EXPECT_EQ(byTitle["A Quotation"].start, 0U);
  EXPECT_EQ(byTitle["A Quotation"].end, 50U);
}

TEST(PublicationTest, aLinkMadeHereReachesAPassageThere) {
  const auto author = xudu::createMutableKeys();
  const auto critic = xudu::createMutableKeys();
  const auto scroll = namedScroll(author.publicKey, "permascroll", 1000);

  // The critic writes a comment of their own, and links it to a passage of
  // somebody else's document -- without that publisher's involvement, which is
  // the point of links being separate from what they are about.
  xudu::Store store;
  const auto scrollId = store.addScroll(scroll);
  auto version        = quoting(store, scroll, 500, 30);
  xudu::Link link;
  link.type  = xudu::LinkType::Comment;
  link.owner = "critic";
  link.left.push_back(xudu::PrimediaSpan{scrollId, 500, 30});
  link.right.push_back(xudu::PrimediaSpan{scrollId, 100, 200});
  version = store.addLink(version, link);

  const auto published = xudu::publish(store, version, critic, "comment",
                                       "A Comment", 1, 1700000000);
  ASSERT_EQ(published.links.size(), 1U);
  EXPECT_EQ(published.links[0].owner, "critic");

  Library library;
  ASSERT_TRUE(library.add(published));

  // The far end, addressed globally, is what a reader would follow.
  const auto key = xudu::scrollKeyFor(author.publicKey, "permascroll");
  const auto found = library.linksTouching(GlobalSpan{key, 100, 200});
  ASSERT_EQ(found.size(), 1U);
  EXPECT_FALSE(found[0].onLeft) << "the passage is the link's right end";
  EXPECT_EQ(found[0].link->owner, "critic");
  // And it survived the journey: the link came back off a manifest that was
  // encoded, signed and decoded.
  const auto reread = xudu::decodePublication(xudu::encodePublication(published));
  ASSERT_TRUE(reread.has_value());
  ASSERT_EQ(reread->links.size(), 1U);
  EXPECT_EQ(reread->links[0].right, published.links[0].right);
}

TEST(PublicationTest, aNameMovesForwardAndNotBack) {
  const auto keys   = xudu::createMutableKeys();
  const auto scroll = namedScroll(keys.publicKey, "permascroll", 1000);
  xudu::Store store;
  const auto version = quoting(store, scroll, 0, 10);

  const auto first  = xudu::publish(store, version, keys, "essay",
                                    "First Thoughts", 1, 1700000000);
  const auto second = xudu::publish(store, version, keys, "essay",
                                    "Second Thoughts", 2, 1700000100);

  Library library;
  ASSERT_TRUE(library.add(second));
  EXPECT_FALSE(library.add(first)) << "an older publication is not news";
  ASSERT_NE(library.find(second.name()), nullptr);
  EXPECT_EQ(library.find(second.name())->title, "Second Thoughts");
  EXPECT_EQ(library.size(), 1U) << "both are the same document";
}

TEST(PublicationTest, twoNamesUnderOneKeyAreTwoDocuments) {
  const auto keys   = xudu::createMutableKeys();
  const auto scroll = namedScroll(keys.publicKey, "permascroll", 1000);
  xudu::Store store;
  const auto version = quoting(store, scroll, 0, 10);

  const auto essay = xudu::publish(store, version, keys, "essay", "An Essay", 1,
                                   1700000000);
  const auto notes = xudu::publish(store, version, keys, "notes", "Some Notes",
                                   1, 1700000000);
  EXPECT_NE(essay.name(), notes.name());

  Library library;
  EXPECT_TRUE(library.add(essay));
  EXPECT_TRUE(library.add(notes));
  EXPECT_EQ(library.size(), 2U);
}

TEST(PublicationTest, theSameDocumentEncodesToTheSameBytes) {
  // Which is what makes a signature checkable rather than a coincidence: two
  // machines holding the same manifest must produce the same bytes to sign.
  const auto keys   = xudu::createMutableKeys();
  const auto scroll = namedScroll(keys.publicKey, "permascroll", 1000);
  xudu::Store store;
  const auto version = quoting(store, scroll, 7, 21);
  const auto pub     = xudu::publish(store, version, keys, "essay", "An Essay",
                                     3, 1700000000);

  const auto once  = xudu::encodePublication(pub);
  const auto twice = xudu::encodePublication(*xudu::decodePublication(once));
  EXPECT_EQ(once, twice);
}

} // namespace

// vi: set sw=2 sts=2 ts=2 et:

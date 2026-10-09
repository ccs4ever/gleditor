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
#include <lmdb.h>

#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>

#include "common/xanadu/publication.hpp"
#include "common/xanadu/publication_outbox.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/store_tables.hpp"
#include "common/xanadu/swarm.hpp"
#include "common/xanadu/torrent.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace {

using xanadu::GlobalSpan;
using xanadu::Library;
using xanadu::MicroversionId;
using xanadu::Publication;
using xanadu::Scroll;
using xanadu::ScrollSegment;

/// A scroll standing for somebody's published permascroll, named by a key so
/// that it is the same scroll on every machine.
Scroll namedScroll(const xanadu::PublicKey &key, std::string salt,
                   const std::uint64_t length) {
  Scroll scroll;
  scroll.publisher = key;
  scroll.salt      = std::move(salt);
  ScrollSegment segment;
  segment.at      = 0;
  segment.length  = length;
  segment.path    = "permascroll";
  segment.torrent = xanadu::InfoHash{};
  scroll.segments.push_back(segment);
  return scroll;
}

/// A document that quotes @p length bytes of a published scroll, starting at
/// @p from. Transclusion rather than typing: what is published has to point at
/// content that already has an address.
MicroversionId quoting(xanadu::Store &store, const Scroll &scroll,
                       const std::uint64_t from, const std::uint64_t length) {
  return store.transcludeExternal(MicroversionId{}, 0, scroll, from, length);
}

TEST(PublicationTest, aDocumentIsPublishedAsPointersAndReadsBackTheSame) {
  const auto keys   = xanadu::createMutableKeys();
  const auto scroll = namedScroll(keys.publicKey, "permascroll", 1000);
  xanadu::Store store;
  const auto version = quoting(store, scroll, 100, 50);

  const auto pub =
      xanadu::publish(store, version, keys, "essay", "An Essay", 1, 1700000000);
  EXPECT_EQ(pub.title, "An Essay");
  EXPECT_EQ(pub.length(), 50U);
  ASSERT_EQ(pub.pieces.size(), 1U);
  // Pointers, not bytes: what went out is where the content is, not a copy.
  EXPECT_EQ(pub.pieces[0].start, 100U);
  EXPECT_EQ(pub.pieces[0].length, 50U);
  EXPECT_EQ(pub.pieces[0].scroll,
            xanadu::scrollKeyFor(keys.publicKey, "permascroll"));
  // And where to fetch them, or the addresses would be unresolvable.
  ASSERT_TRUE(pub.scrolls.contains(pub.pieces[0].scroll));

  const auto encoded = xanadu::encodePublication(pub);
  const auto read    = xanadu::decodePublication(encoded);
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->title, pub.title);
  EXPECT_EQ(read->pieces, pub.pieces);
  EXPECT_EQ(read->publisher, pub.publisher);
  EXPECT_EQ(read->name(), pub.name());
}

TEST(PublicationTest, aPageBreakSurvivesBeingPublishedAndRead) {
  // Publishing any document with a break in it used to throw. A break has no
  // scroll -- it names a place in the text rather than any content -- so
  // globalise() read it as "content this machine has not published" and
  // refused. apps/xudu/session.cpp puts one in on every page of a PDF import,
  // so this was the ordinary case rather than an exotic one.
  const auto keys   = xanadu::createMutableKeys();
  const auto scroll = namedScroll(keys.publicKey, "permascroll", 1000);
  xanadu::Store store;
  auto version = quoting(store, scroll, 0, 60);
  version      = store.insertBreak(version, 20);
  ASSERT_THAT(store.rebuild(version).forcedBreaks(), testing::ElementsAre(20U));

  const auto pub =
      xanadu::publish(store, version, keys, "essay", "An Essay", 1, 1700000000);
  // Text before the break, the break, text after: the break is a piece of the
  // document like any other, carrying no length.
  ASSERT_EQ(pub.pieces.size(), 3U);
  EXPECT_EQ(pub.pieces[1].scroll, xanadu::breakMarkerKey);
  EXPECT_EQ(pub.pieces[1].length, 0U);
  EXPECT_EQ(pub.length(), 60U) << "a break must not be counted as text";
  // and contributing nothing to fetch, because there is nothing behind it.
  EXPECT_FALSE(pub.scrolls.contains(std::string{xanadu::breakMarkerKey}));

  const auto read = xanadu::decodePublication(xanadu::encodePublication(pub));
  ASSERT_TRUE(read.has_value()) << "the reserved name broke the signature";
  EXPECT_EQ(read->pieces, pub.pieces);

  // A reader gets the pagination back, at the same place in the same text.
  xanadu::Store reader;
  const auto taken = xanadu::adopt(reader, *read);
  EXPECT_EQ(reader.rebuild(taken.version).length(), 60U);
  EXPECT_THAT(reader.rebuild(taken.version).forcedBreaks(),
              testing::ElementsAre(20U));
}

TEST(PublicationTest, aManifestThatWasAlteredDoesNotRead) {
  const auto keys   = xanadu::createMutableKeys();
  const auto scroll = namedScroll(keys.publicKey, "permascroll", 1000);
  xanadu::Store store;
  const auto version = quoting(store, scroll, 0, 40);
  const auto pub =
      xanadu::publish(store, version, keys, "essay", "An Essay", 1, 1700000000);

  // The title is the mildest thing anyone would think to change.
  auto altered  = pub;
  altered.title = "Somebody Else's Essay";
  EXPECT_FALSE(xanadu::verifyPublication(altered));
  EXPECT_FALSE(xanadu::decodePublication(xanadu::encodePublication(altered))
                   .has_value());

  // And the part that would matter: what the document points at.
  auto moved = pub;
  moved.pieces[0].start += 1;
  EXPECT_FALSE(xanadu::verifyPublication(moved));
  EXPECT_FALSE(
      xanadu::decodePublication(xanadu::encodePublication(moved)).has_value());

  // Claiming somebody else's name over one's own content fails the same way,
  // which is the property that makes authorship a fact rather than a field.
  const auto other = xanadu::createMutableKeys();
  auto forged      = pub;
  forged.publisher = other.publicKey;
  EXPECT_FALSE(xanadu::verifyPublication(forged));
}

TEST(PublicationTest, refusesToPublishContentThisMachineHasNotPublished) {
  // Typed here and never sealed: the pieces point at the local spool, which
  // has no address anyone else could resolve. Publishing that would produce a
  // document that arrives and cannot be read.
  xanadu::Store store;
  const auto version = store.insert(MicroversionId{}, 0, "written just now");
  const auto keys    = xanadu::createMutableKeys();
  EXPECT_THROW(std::ignore = xanadu::publish(store, version, keys, "essay",
                                             "An Essay", 1, 1700000000),
               std::runtime_error);
}

// The path from "written here" to "readable anywhere". Sealing gives the local
// spool the name it was missing; the offsets it already had become global
// addresses, so nothing written before the seal has to be rewritten.
TEST(PublicationTest, whatWasWrittenHereCanBeSealedAndThenPublished) {
  xanadu::Store store;
  const auto version = store.insert(MicroversionId{}, 0, "Written here first.");
  const auto keys    = xanadu::createMutableKeys();

  // Before sealing there is nothing a reader could resolve.
  EXPECT_THROW(std::ignore = xanadu::publish(store, version, keys, "essay",
                                             "An Essay", 1, 1700000000),
               std::runtime_error);

  // Sealing insists on a signed record of who is doing it; whether gpg made
  // that signature is provenance.cpp's business.
  xanadu::Provenance who;
  who.author.name   = "Ada Lovelace";
  who.author.email  = "ada@example.org";
  const auto sealed = xanadu::sealLocalSpool(
      store, keys, "permascroll", "",
      {who.toTsv(), "-----BEGIN PGP SIGNATURE-----\n(for the test)\n"});
  EXPECT_EQ(sealed.scroll.length(), store.primedia().bytes().size());
  EXPECT_TRUE(sealed.scroll.isNamed());
  // A real torrent: it parses, and its info hash is the one sealing reported.
  const auto meta = xanadu::Metainfo::parse(sealed.torrentFile);
  EXPECT_EQ(meta.hash(), sealed.hash);
  // The content is the first file and the whole of the scroll; the authorship
  // record and its signature ride along, which is why the torrent holds more
  // than the spool does.
  ASSERT_FALSE(meta.files().empty());
  EXPECT_EQ(meta.files()[0].length, store.primedia().bytes().size());
  EXPECT_GT(meta.totalLength(), store.primedia().bytes().size());

  const auto pub = xanadu::publish(store, version, keys, "essay", "An Essay", 1,
                                   1700000000, &sealed.scroll);
  ASSERT_FALSE(pub.pieces.empty());
  EXPECT_EQ(pub.pieces[0].scroll,
            xanadu::scrollKeyFor(keys.publicKey, "permascroll"));
  EXPECT_EQ(pub.length(), std::string("Written here first.").size());
  // And the address is the offset it always had, which is why sealing does not
  // rewrite anything.
  EXPECT_EQ(pub.pieces[0].start, 0U);

  ASSERT_TRUE(
      xanadu::decodePublication(xanadu::encodePublication(pub)).has_value());
}

TEST(PublicationTest, twoDocumentsQuotingOnePassageAreFoundToShareIt) {
  // Two publishers who have never met, quoting one published permascroll.
  const auto author = xanadu::createMutableKeys();
  const auto scroll = namedScroll(author.publicKey, "permascroll", 1000);
  const auto quoter = xanadu::createMutableKeys();

  xanadu::Store originalStore;
  xanadu::Store quotingStore;
  const auto originalVersion = quoting(originalStore, scroll, 100, 200);
  // The middle of it, written by somebody who only has the address.
  const auto quotingVersion = quoting(quotingStore, scroll, 150, 50);

  const auto first  = xanadu::publish(originalStore, originalVersion, author,
                                      "original", "The Original", 1, 1700000000);
  const auto second = xanadu::publish(quotingStore, quotingVersion, quoter,
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
  const auto author = xanadu::createMutableKeys();
  const auto critic = xanadu::createMutableKeys();
  const auto scroll = namedScroll(author.publicKey, "permascroll", 1000);

  // The critic writes a comment of their own, and links it to a passage of
  // somebody else's document -- without that publisher's involvement, which is
  // the point of links being separate from what they are about.
  xanadu::Store store;
  const auto scrollId = store.addScroll(scroll);
  auto version        = quoting(store, scroll, 500, 30);
  xanadu::Link link;
  link.type  = xanadu::LinkType::Comment;
  link.owner = "critic";
  link.left.push_back(xanadu::PrimediaSpan{scrollId, 500, 30});
  link.right.push_back(xanadu::PrimediaSpan{scrollId, 100, 200});
  version = store.addLink(version, link);

  const auto published = xanadu::publish(store, version, critic, "comment",
                                         "A Comment", 1, 1700000000);
  ASSERT_EQ(published.links.size(), 1U);
  EXPECT_EQ(published.links[0].owner, "critic");

  Library library;
  ASSERT_TRUE(library.add(published));

  // The far end, addressed globally, is what a reader would follow.
  const auto key   = xanadu::scrollKeyFor(author.publicKey, "permascroll");
  const auto found = library.linksTouching(GlobalSpan{key, 100, 200});
  ASSERT_EQ(found.size(), 1U);
  EXPECT_FALSE(found[0].onLeft) << "the passage is the link's right end";
  EXPECT_EQ(found[0].link->owner, "critic");
  // And it survived the journey: the link came back off a manifest that was
  // encoded, signed and decoded.
  const auto reread =
      xanadu::decodePublication(xanadu::encodePublication(published));
  ASSERT_TRUE(reread.has_value());
  ASSERT_EQ(reread->links.size(), 1U);
  EXPECT_EQ(reread->links[0].right, published.links[0].right);
}

TEST(PublicationTest, aNameMovesForwardAndNotBack) {
  const auto keys   = xanadu::createMutableKeys();
  const auto scroll = namedScroll(keys.publicKey, "permascroll", 1000);
  xanadu::Store store;
  const auto version = quoting(store, scroll, 0, 10);

  const auto first  = xanadu::publish(store, version, keys, "essay",
                                      "First Thoughts", 1, 1700000000);
  const auto second = xanadu::publish(store, version, keys, "essay",
                                      "Second Thoughts", 2, 1700000100);

  Library library;
  ASSERT_TRUE(library.add(second));
  EXPECT_FALSE(library.add(first)) << "an older publication is not news";
  ASSERT_NE(library.find(second.name()), nullptr);
  EXPECT_EQ(library.find(second.name())->title, "Second Thoughts");
  EXPECT_EQ(library.size(), 1U) << "both are the same document";
}

TEST(PublicationTest, twoNamesUnderOneKeyAreTwoDocuments) {
  const auto keys   = xanadu::createMutableKeys();
  const auto scroll = namedScroll(keys.publicKey, "permascroll", 1000);
  xanadu::Store store;
  const auto version = quoting(store, scroll, 0, 10);

  const auto essay =
      xanadu::publish(store, version, keys, "essay", "An Essay", 1, 1700000000);
  const auto notes = xanadu::publish(store, version, keys, "notes",
                                     "Some Notes", 1, 1700000000);
  EXPECT_NE(essay.name(), notes.name());

  Library library;
  EXPECT_TRUE(library.add(essay));
  EXPECT_TRUE(library.add(notes));
  EXPECT_EQ(library.size(), 2U);
}

TEST(PublicationTest, theSameDocumentEncodesToTheSameBytes) {
  // Which is what makes a signature checkable rather than a coincidence: two
  // machines holding the same manifest must produce the same bytes to sign.
  const auto keys   = xanadu::createMutableKeys();
  const auto scroll = namedScroll(keys.publicKey, "permascroll", 1000);
  xanadu::Store store;
  const auto version = quoting(store, scroll, 7, 21);
  const auto pub =
      xanadu::publish(store, version, keys, "essay", "An Essay", 3, 1700000000);

  const auto once = xanadu::encodePublication(pub);
  const auto twice =
      xanadu::encodePublication(*xanadu::decodePublication(once));
  EXPECT_EQ(once, twice);
}

TEST(PublicationTest, publicationWithWithheldHolesRoundTripsAndVerifies) {
  const auto keys   = xanadu::createMutableKeys();
  const auto scroll = namedScroll(keys.publicKey, "permascroll", 5000);
  xanadu::Store store;
  const auto version = quoting(store, scroll, 0, 1000);
  auto pub = xanadu::publish(store, version, keys, "essay", "Withheld Essay", 1,
                             1700000000);

  xanadu::PublishedHoleRecord hole;
  hole.at     = 200;
  hole.length = 300;
  hole.reason = xanadu::HoleReason::Withheld;
  hole.contentCommitment.fill(0xAB);
  pub.holes.push_back(hole);

  // Re-sign with the new hole included
  pub.signature =
      xanadu::signMutableItem(xanadu::publicationSigningBuffer(pub), keys);
  EXPECT_TRUE(xanadu::verifyPublication(pub));

  const auto encoded = xanadu::encodePublication(pub);
  const auto decoded = xanadu::decodePublication(encoded);
  ASSERT_TRUE(decoded.has_value());
  ASSERT_EQ(decoded->holes.size(), 1U);
  EXPECT_EQ(decoded->holes[0].at, 200U);
  EXPECT_EQ(decoded->holes[0].length, 300U);
  EXPECT_EQ(decoded->holes[0].reason, xanadu::HoleReason::Withheld);
  EXPECT_EQ(decoded->holes[0].contentCommitment, hole.contentCommitment);
  EXPECT_FALSE(decoded->holes[0].transcopyright.has_value());

  // Tampering with the hole record invalidates the signature
  auto tampered = pub;
  tampered.holes[0].length += 1;
  EXPECT_FALSE(xanadu::verifyPublication(tampered));
  EXPECT_FALSE(xanadu::decodePublication(xanadu::encodePublication(tampered))
                   .has_value());
}

TEST(PublicationTest,
     PrimediaRedactionKeepsOffsetsAcrossIncrementalBoundaries) {
  const xanadu::PublishedHoleRecord withheld{
      .at = 102, .length = 6, .reason = xanadu::HoleReason::Withheld};
  const std::vector holes{withheld};
  const auto first  = xanadu::publicationPrimedia("abcdef", 100, holes);
  const auto second = xanadu::publicationPrimedia("ghijkl", 106, holes);
  EXPECT_EQ(first + second,
            xanadu::publicationPrimedia("abcdefghijkl", 100, holes));
  EXPECT_EQ(first, std::string("ab\0\0\0\0", 6));
  EXPECT_EQ(second, std::string("\0\0ijkl", 6));
  EXPECT_THROW((void)xanadu::publicationPrimedia(
                   "a", std::numeric_limits<std::uint64_t>::max(), {}),
               std::invalid_argument);
  const xanadu::PublishedHoleRecord overflow{
      .at = std::numeric_limits<std::uint64_t>::max(), .length = 1};
  EXPECT_THROW((void)xanadu::publicationPrimedia("a", 0, {overflow}),
               std::invalid_argument);
}

TEST(PublicationTest, WithheldRangesAreIncludedBeforeTheManifestIsSigned) {
  const auto keys   = xanadu::createMutableKeys();
  const auto scroll = namedScroll(keys.publicKey, "permascroll", 5000);
  xanadu::Store store;
  const auto version = quoting(store, scroll, 0, 1000);
  const xanadu::PublishedHoleRecord hole{
      .at = 1000, .length = 200, .reason = xanadu::HoleReason::Withheld};
  auto pub = xanadu::publish(store, version, keys, "essay", "Ideas", 1,
                             1700000000, nullptr, {}, {hole});
  const auto decoded =
      xanadu::decodePublication(xanadu::encodePublication(pub));
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->holes, std::vector{xanadu::PublishedHoleRecord{hole}});
  pub.holes.front().length++;
  EXPECT_FALSE(xanadu::verifyPublication(pub));
}

TEST(PublicationTest,
     publicationWithTranscopyrightPaywallRoundTripsAndVerifies) {
  const auto keys   = xanadu::createMutableKeys();
  const auto scroll = namedScroll(keys.publicKey, "permascroll", 5000);
  xanadu::Store store;
  const auto version = quoting(store, scroll, 0, 1000);
  auto pub = xanadu::publish(store, version, keys, "essay", "Commercial Essay",
                             1, 1700000000);

  xanadu::TranscopyrightDescriptor tc;
  tc.priceAtomicUnits = 25000000; // 0.025 XU (25 million nano-xu)
  tc.flatFee          = false;
  tc.currencySymbol   = "XU";
  tc.licenseMemo      = "Nelson-Transcopyright-v1";
  tc.authorWallet     = *xanadu::identity::Fingerprint::fromString(
      "4A7F1234567890ABCDEF1234567890ABCDEF1234");
  tc.authorPubKey.bytes.fill(0x33);
  tc.keyId.fill(0x44);
  tc.nonce.fill(0x55);

  xanadu::PublishedHoleRecord hole;
  hole.at     = 500;
  hole.length = 250;
  hole.reason = xanadu::HoleReason::TranscopyrightLock;
  hole.contentCommitment.fill(0x66);
  hole.transcopyright = tc;
  pub.holes.push_back(hole);

  // Re-sign with transcopyright paywall included
  pub.signature =
      xanadu::signMutableItem(xanadu::publicationSigningBuffer(pub), keys);
  EXPECT_TRUE(xanadu::verifyPublication(pub));

  const auto encoded = xanadu::encodePublication(pub);
  const auto decoded = xanadu::decodePublication(encoded);
  ASSERT_TRUE(decoded.has_value());
  ASSERT_EQ(decoded->holes.size(), 1U);
  EXPECT_EQ(decoded->holes[0].at, 500U);
  EXPECT_EQ(decoded->holes[0].length, 250U);
  EXPECT_EQ(decoded->holes[0].reason, xanadu::HoleReason::TranscopyrightLock);
  ASSERT_TRUE(decoded->holes[0].transcopyright.has_value());

  const auto &decodedTc = *decoded->holes[0].transcopyright;
  EXPECT_EQ(decodedTc.priceAtomicUnits, 25000000U);
  EXPECT_FALSE(decodedTc.flatFee);
  EXPECT_EQ(decodedTc.computeCost(250), 25000000ULL * 250ULL);
  EXPECT_EQ(decodedTc.currencySymbol, "XU");
  EXPECT_EQ(decodedTc.licenseMemo, "Nelson-Transcopyright-v1");
  EXPECT_EQ(decodedTc.authorWallet, tc.authorWallet);
  EXPECT_EQ(decodedTc.authorPubKey, tc.authorPubKey);
  EXPECT_EQ(decodedTc.keyId, tc.keyId);
  EXPECT_EQ(decodedTc.nonce, tc.nonce);

  // Tampering with the price invalidates signature
  auto tampered                                      = pub;
  tampered.holes[0].transcopyright->priceAtomicUnits = 1;
  EXPECT_FALSE(xanadu::verifyPublication(tampered));
  EXPECT_FALSE(xanadu::decodePublication(xanadu::encodePublication(tampered))
                   .has_value());
}

TEST(PublicationTest, scrollSegmentWithHoleRecordEncodesAndDecodesInScroll) {
  const auto keys = xanadu::createMutableKeys();
  xanadu::Scroll scroll;
  scroll.publisher = keys.publicKey;
  scroll.salt      = "permascroll";

  xanadu::ScrollSegment seg0;
  seg0.at     = 0;
  seg0.length = 1000;
  seg0.path   = "permascroll";
  scroll.segments.push_back(seg0);

  xanadu::PublishedHoleRecord hole;
  hole.at     = 1000;
  hole.length = 500;
  hole.reason = xanadu::HoleReason::Withheld;

  xanadu::ScrollSegment seg1;
  seg1.at         = 1000;
  seg1.length     = 500;
  seg1.kind       = xanadu::SegmentKind::Withheld;
  seg1.holeRecord = hole;
  scroll.segments.push_back(seg1);

  xanadu::ScrollSegment seg2;
  seg2.at     = 1500;
  seg2.length = 3500;
  seg2.path   = "permascroll";
  scroll.segments.push_back(seg2);

  xanadu::Store store;
  const auto version = quoting(store, scroll, 0, 1000);
  const auto pub     = xanadu::publish(store, version, keys, "essay",
                                       "Segmented Essay", 1, 1700000000);

  const auto encoded = xanadu::encodePublication(pub);
  const auto decoded = xanadu::decodePublication(encoded);
  ASSERT_TRUE(decoded.has_value());

  const auto scrollKey = xanadu::scrollKeyFor(keys.publicKey, "permascroll");
  ASSERT_TRUE(decoded->scrolls.contains(scrollKey));
  const auto &decodedScroll = decoded->scrolls.at(scrollKey);
  ASSERT_EQ(decodedScroll.segments.size(), 3U);

  const auto withheldSeg = decodedScroll.segmentAt(1200);
  ASSERT_TRUE((withheldSeg).has_value());
  EXPECT_TRUE(withheldSeg->isWithheld());
  EXPECT_EQ(withheldSeg->kind, xanadu::SegmentKind::Withheld);
  ASSERT_TRUE(withheldSeg->holeRecord.has_value());
  EXPECT_EQ(withheldSeg->holeRecord->reason, xanadu::HoleReason::Withheld);
}

class PublicationInventoryTest : public testing::Test {
protected:
  xanadu::MutableKeys keys   = xanadu::createMutableKeys();
  std::filesystem::path root = std::filesystem::temp_directory_path() /
                               ("xudu-inventory-" + keys.publicKey.hex());
  xanadu::Store author;
  std::shared_ptr<xanadu::UserPermascroll> reader =
      std::make_shared<xanadu::UserPermascroll>();
  xanadu::DirectoryContentSource source;
  Publication pub;
  MicroversionId docA, docB, sliceA, sliceB, textA, head, fork;
  xanadu::SignedProvenance provenance{.tsv       = "test record",
                                      .signature = "test signature"};

  void SetUp() override {
    docA   = author.makeXanadoc(author.sliceGenesis({}), "Story Ideas");
    textA  = author.insert(docA, 0, "Alice's ideas", docA);
    docB   = author.makeXanadoc(textA, "Research Notes");
    head   = author.insert(docB, 0, "More ideas", docB);
    sliceA = author.makeSlice(head, "Idea board");
    head   = author.makeCell(sliceA, "idea one", sliceA);
    sliceB = author.makeSlice(head, "Second board");
    head   = author.makeCell(sliceB, "idea two", sliceB);
    const std::vector<xanadu::TorrentContent> files{
        {.path = "research", .data = "Reference"}};
    const auto made = xanadu::makeTorrent(files, "research");
    const auto seed = xanadu::writeTorrentSeed(root, made, files);
    (void)source.add(made.file, seed.string());
    const auto research = Scroll::ofTorrentFile(made.hash, 0, "research", 0, 9);
    head                = author.insertSpan(
        head, 13,
        {.scroll = author.addScroll(research), .start = 0, .length = 9}, docA);
    // Recorded late, sorts before later births when restored.
    fork = author.insert(textA, 13, " alternate", docA);
    author.setCurrentVersions({head});
    author.setVersionAnnotation(textA, {.alias       = "draft",
                                        .description = "First ideas",
                                        .tag         = "review",
                                        .timestamp   = "2026-10-04T00:00:00Z"});
    author.sealMetadata();
    head = author.designateEdition(author.structureHead(), "release", docB);
    seal();
  }
  void seal() {
    const auto sealed = xanadu::sealLocalSpool(author, keys, "permascroll",
                                               root.string(), provenance);
    pub = xanadu::publish(author, textA, keys, "doc:ideas", "Story Ideas", 1, 1,
                          &sealed.scroll, {*sealed.opsSegment});
    for (const auto &seed : xanadu::reviewPublicationDependencies(pub, {root}))
      (void)source.add(seed.metainfo, seed.savePath.string());
  }
  void sign() {
    pub.signature =
        xanadu::signMutableItem(xanadu::publicationSigningBuffer(pub), keys);
  }
  ~PublicationInventoryTest() override { std::filesystem::remove_all(root); }
};

TEST_F(PublicationInventoryTest,
       RestoresEveryDocumentSliceBranchEditionAndAnnotation) {
  const auto decoded =
      xanadu::decodePublication(xanadu::encodePublication(pub));
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->inventory, pub.inventory);
  ASSERT_EQ(pub.inventory.size(), 5U);
  const auto restored = xanadu::restorePublication(*decoded, source, reader);
  EXPECT_EQ(restored->documentId(), author.documentId());
  EXPECT_EQ(restored->opCount(), author.opCount());
  EXPECT_EQ(restored->currentVersions(), author.currentVersions());
  const auto annotation = restored->versionAnnotation(textA);
  ASSERT_TRUE(annotation);
  EXPECT_EQ(annotation->alias, "draft");
  EXPECT_EQ(annotation->description, "First ideas");
  EXPECT_EQ(annotation->tag, "review");
  EXPECT_EQ(annotation->timestamp, "2026-10-04T00:00:00.000000000Z");
  EXPECT_EQ(restored->editionNamed(head, "release")->targetVersion, docB);
  EXPECT_EQ(restored->textOf(fork), "Alice's ideas alternate");
  EXPECT_NE(restored->segmentedOps().indexOf(docB),
            author.segmentedOps().indexOf(docB));
  EXPECT_EQ(restored->textOf(head, restored->segmentedOps().indexOf(docA)),
            "Alice's ideasReference");
  EXPECT_EQ(restored->textOf(head, restored->segmentedOps().indexOf(docB)),
            "More ideas");
  const auto folded = restored->rebuildManifold(head);
  EXPECT_EQ(folded.refusedOps(), 0U);
  EXPECT_EQ(restored->resolveStructureName(
                head, restored->segmentedOps().indexOf(sliceA)),
            "Idea board");
  EXPECT_EQ(restored->resolveStructureName(
                head, restored->segmentedOps().indexOf(sliceB)),
            "Second board");
  EXPECT_EQ(reader->bytes().size(), 0U);
}

TEST_F(PublicationInventoryTest,
       ImportedBindingsDoNotInventRegistryOperations) {
  xanadu::Store sparse;
  const auto text    = sparse.insert({}, 0, "Ideas");
  const auto foreign = std::ranges::find_if(pub.scrolls, [](const auto &entry) {
    return entry.first.starts_with("file:");
  });
  ASSERT_NE(foreign, pub.scrolls.end());
  (void)sparse.transcludeExternal(text, 5, foreign->second, 0, 9);
  const auto genesis = sparse.sliceGenesis(text);
  (void)sparse.makeCell(genesis, "Idea cell");
  // The slice branch has no authored registry rank. Complete publication
  // descriptors still make the quotation on the other branch addressable.
  const auto sealed = xanadu::sealLocalSpool(sparse, keys, "sparse-permascroll",
                                             root.string(), provenance);
  const auto publication =
      xanadu::publish(sparse, text, keys, "doc:sparse", "Ideas", 1, 1,
                      &sealed.scroll, {*sealed.opsSegment});
  const auto installed = xanadu::installPublication(publication, {root}, reader,
                                                    root / "sparse-reader");
  EXPECT_EQ(installed->opCount(), sparse.opCount());
  EXPECT_EQ(installed->textOf(text), "Ideas");
  EXPECT_EQ(reader->bytes().size(), 0U);
}

TEST_F(PublicationInventoryTest,
       InstalledReaderReopensOfflineWithoutChangingAuthorship) {
  const auto destination = root / "reader";
  auto installed = xanadu::installPublication(pub, {root}, reader, destination);
  const auto count = installed->opCount();
  EXPECT_EQ(count, author.opCount());
  EXPECT_EQ(reader->bytes().size(), 0U);
  EXPECT_NE(installed->publishedLocalScroll(), xanadu::localScroll);
  const auto table = xanadu::readStoreTables(destination / "store.tables");
  EXPECT_EQ(table.publishedLocalScroll, installed->publishedLocalScroll());
  EXPECT_EQ(table.deployedScrolls.size(), installed->scrolls().size());
  // The installation owns its carriers; removing all author seed roots and
  // discarding every source/store object cannot make the reader dependent on
  // the publishing machine or borrowed ContentSource lifetime.
  installed.reset();
  for (const auto &seed : xanadu::reviewPublicationDependencies(pub, {root}))
    std::filesystem::remove_all(seed.savePath);
  xanadu::Store offline(reader);
  offline.load(destination.string());
  EXPECT_EQ(offline.documentId(), pub.storeId);
  EXPECT_EQ(offline.opCount(), count);
  EXPECT_EQ(offline.textOf(fork), "Alice's ideas alternate");
  EXPECT_EQ(offline.textOf(head, offline.segmentedOps().indexOf(docB)),
            "More ideas");
  EXPECT_EQ(offline.currentVersions(), author.currentVersions());
  EXPECT_EQ(offline.editionNamed(head, "release")->targetVersion, docB);
  EXPECT_EQ(offline.resolveStructureName(
                head, offline.segmentedOps().indexOf(sliceB)),
            "Second board");
  offline.save(destination.string());
  EXPECT_EQ(offline.opCount(), count);
  EXPECT_EQ(reader->bytes().size(), 0U);
}

TEST_F(PublicationInventoryTest, ReaderEditsUseItsOwnScrollAndSurviveReopen) {
  const auto destination = root / "reader-edit";
  auto installed = xanadu::installPublication(pub, {root}, reader, destination);
  const auto edited = installed->insert(textA, 13, " Bob", docA);
  installed->save(destination.string());
  const auto count = installed->opCount();
  EXPECT_EQ(reader->bytes().size(), 4U);
  xanadu::Store offline(reader);
  offline.load(destination.string());
  EXPECT_EQ(offline.textOf(edited), "Alice's ideas Bob");
  EXPECT_EQ(offline.textOf(textA), "Alice's ideas");
  EXPECT_EQ(offline.opCount(), count);
  EXPECT_EQ(offline.editionNamed(head, "release")->targetVersion, docB);
}

TEST_F(PublicationInventoryTest, InstalledReaderRefusesCorruptAndMissingCache) {
  for (const bool corrupt : {false, true}) {
    const auto destination =
        root / (corrupt ? "reader-corrupt" : "reader-missing");
    auto installed =
        xanadu::installPublication(pub, {root}, reader, destination);
    installed.reset();
    const auto &segment = pub.scrolls.at(pub.historyScroll).segments.front();
    const auto seed     = destination / "published" / segment.torrent.hex();
    if (corrupt) {
      const auto meta = xanadu::Metainfo::parse([&] {
        std::ifstream in(seed / "metainfo.torrent", std::ios::binary);
        return std::string{std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>()};
      }());
      std::ofstream bytes(seed / meta.name() / segment.path,
                          std::ios::binary | std::ios::trunc);
      bytes << "corrupt";
    } else {
      std::filesystem::remove_all(seed);
    }
    xanadu::Store offline(reader);
    EXPECT_THROW(offline.load(destination.string()),
                 xanadu::StoreTablesUnreadable);
  }
}

TEST_F(PublicationInventoryTest,
       InstallationRefusesIncompleteHistoryAndLeavesNoStore) {
  const auto destination = root / "refused-reader";
  auto bad               = pub;
  bad.opsSegments.clear();
  bad.signature =
      xanadu::signMutableItem(xanadu::publicationSigningBuffer(bad), keys);
  EXPECT_THROW(
      (void)xanadu::installPublication(bad, {root}, reader, destination),
      xanadu::PublicationUnreadable);
  EXPECT_FALSE(std::filesystem::exists(destination));
  EXPECT_FALSE(std::filesystem::exists(destination.string() + ".partial"));
  EXPECT_EQ(reader->bytes().size(), 0U);
}

TEST_F(PublicationInventoryTest, InstallationNeverOverwritesAnExistingReader) {
  const auto destination = root / "existing-reader";
  auto installed = xanadu::installPublication(pub, {root}, reader, destination);
  const auto edited = installed->insert(textA, 13, " Bob", docA);
  installed->save(destination.string());
  EXPECT_THROW(
      (void)xanadu::installPublication(pub, {root}, reader, destination),
      std::runtime_error);
  xanadu::Store offline(reader);
  offline.load(destination.string());
  EXPECT_EQ(offline.textOf(edited), "Alice's ideas Bob");
}

TEST_F(PublicationInventoryTest,
       RepointingAfterReopenDoesNotInventAnAnnotation) {
  author.save((root / "native").string());
  xanadu::Store reopened(author.userPermascrollPtr());
  reopened.setContentSource(&source);
  reopened.load((root / "native").string());
  const auto release =
      reopened.editionNamed(reopened.structureHead(), "release");
  ASSERT_TRUE(release);
  const auto changed = reopened.repointEdition(
      reopened.structureHead(), reopened.segmentedOps().idOf(release->cell),
      fork);
  reopened.sealMetadata();
  const auto folded = reopened.rebuildManifold(reopened.structureHead());
  EXPECT_FALSE(folded.versionAnnotation(reopened.segmentedOps().indexOf(docB),
                                        reopened));
  EXPECT_EQ(reopened.editionNamed(changed, "release")->targetVersion, fork);
}

TEST_F(PublicationInventoryTest,
       PendingAuthorDecisionsCannotBeOmittedFromAHistoryPublication) {
  author.setCurrentVersions({fork});
  EXPECT_THROW((void)xanadu::publish(author, textA, keys, pub.salt, pub.title,
                                     2, 2, &pub.scrolls.at(pub.historyScroll),
                                     pub.opsSegments),
               xanadu::PublicationUnreadable);
}

TEST_F(PublicationInventoryTest, MetadataPreparationIsIdempotent) {
  author.sealMetadata();
  const auto count   = author.opCount();
  const auto size    = author.primedia().bytes().size();
  const auto current = author.currentVersions();
  author.sealMetadata();
  EXPECT_EQ(author.opCount(), count);
  EXPECT_EQ(author.primedia().bytes().size(), size);
  EXPECT_EQ(author.currentVersions(), current);
}

TEST_F(PublicationInventoryTest,
       ReadingOtherBranchesDoesNotRepointCurrentEditions) {
  const auto designated = author.currentVersions();
  for (const auto &version : pub.heads) (void)author.rebuildManifold(version);
  EXPECT_EQ(author.currentVersions(), designated);
  (void)reader->append("Bob's private notes");
  const auto before   = std::string(reader->bytes());
  const auto restored = xanadu::restorePublication(pub, source, reader);
  EXPECT_EQ(restored->textOf(textA), "Alice's ideas");
  EXPECT_EQ(reader->bytes(), before);
}

TEST_F(PublicationInventoryTest,
       RestoresIncrementalHistoryAndRetainsEarlierAddresses) {
  const auto first   = pub;
  const auto count   = static_cast<std::uint32_t>(author.opCount());
  const auto changed = author.insert(head, 13, " revised", docA);
  const auto sealed  = xanadu::sealLocalSpool(
      author, keys, "permascroll", root.string(), provenance,
      first.scrolls.at(first.historyScroll), count);
  auto ops = first.opsSegments;
  ASSERT_TRUE(sealed.opsSegment);
  ops.push_back(*sealed.opsSegment);
  const auto next = xanadu::publish(author, changed, keys, pub.salt, pub.title,
                                    2, 2, &sealed.scroll, ops);
  for (const auto &seed : xanadu::reviewPublicationDependencies(next, {root}))
    (void)source.add(seed.metainfo, seed.savePath.string());
  const auto restored = xanadu::restorePublication(next, source, reader);
  EXPECT_EQ(restored->textOf(textA), "Alice's ideas");
  EXPECT_EQ(restored->textOf(changed, restored->segmentedOps().indexOf(docA)),
            "Alice's ideas revisedReference");
  EXPECT_EQ(next.historyScroll, first.historyScroll);
  EXPECT_EQ(next.opsSegments.front(), first.opsSegments.front());
  EXPECT_TRUE(reader->bytes().empty());
}

TEST_F(PublicationInventoryTest, RejectsSignedOmittedBirthOrInventedView) {
  pub.inventory.pop_back();
  sign();
  EXPECT_THROW((void)xanadu::restorePublication(pub, source, reader),
               xanadu::PublicationUnreadable);
  seal();
  pub.inventory.front().views.front().head = MicroversionId::parse("99999");
  sign();
  EXPECT_THROW((void)xanadu::restorePublication(pub, source, reader),
               xanadu::PublicationUnreadable);
  EXPECT_TRUE(reader->bytes().empty());
}

TEST_F(PublicationInventoryTest, RejectsMissingPrimediaAndCorruptHistory) {
  xanadu::DirectoryContentSource empty;
  EXPECT_THROW((void)xanadu::restorePublication(pub, empty, reader),
               xanadu::PublicationUnreadable);
  const auto path = root / pub.opsSegments.front().torrent.hex() /
                    "permascroll" / xanadu::sealedOpsName;
  {
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    file.put('!');
  }
  EXPECT_THROW((void)xanadu::restorePublication(pub, source, reader),
               xanadu::PublicationUnreadable);
  EXPECT_TRUE(reader->bytes().empty());
}

TEST_F(PublicationInventoryTest,
       RejectsSignedSegmentGapsWrongCountsAndSelectedEdl) {
  pub.opsSegments.front().at = 1;
  sign();
  EXPECT_THROW((void)xanadu::restorePublication(pub, source, reader),
               xanadu::PublicationUnreadable);
  pub.opsSegments.front().at = 0;
  ++pub.opsSegments.front().length;
  sign();
  EXPECT_THROW((void)xanadu::restorePublication(pub, source, reader),
               xanadu::PublicationUnreadable);
  --pub.opsSegments.front().length;
  ++pub.pieces.front().start;
  sign();
  EXPECT_THROW((void)xanadu::restorePublication(pub, source, reader),
               xanadu::PublicationUnreadable);
}

TEST_F(PublicationInventoryTest,
       AuthorCanRepointOneEditionWithoutChangingTheOthers) {
  const auto oldCurrent = author.currentVersions();
  const auto release    = author.editionNamed(head, "release");
  ASSERT_TRUE(release);
  head = author.repointEdition(author.structureHead(),
                               author.segmentedOps().idOf(release->cell), fork);
  EXPECT_EQ(author.editionNamed(head, "release")->targetVersion, fork);
  EXPECT_EQ(author.currentVersions(), oldCurrent);
  seal();
  const auto restored = xanadu::restorePublication(pub, source, reader);
  EXPECT_EQ(restored->editionNamed(head, "release")->targetVersion, fork);
  EXPECT_EQ(restored->currentVersions(), oldCurrent);
}

} // namespace

TEST(PublicationSequenceTest, reservationsSurviveReopenAndArePerNameAndKey) {
  const auto keys  = xanadu::createMutableKeys();
  const auto other = xanadu::createMutableKeys();
  const auto dir   = std::filesystem::temp_directory_path() /
                   ("xudu-sequences-" + keys.publicKey.hex());
  std::filesystem::remove_all(dir);
  EXPECT_EQ(
      xanadu::reservePublicationSequence(dir, keys.publicKey, "doc:ideas"), 1);
  EXPECT_EQ(
      xanadu::reservePublicationSequence(dir, keys.publicKey, "doc:ideas"), 2);
  EXPECT_EQ(xanadu::reservePublicationSequence(dir, keys.publicKey, "catalog"),
            1);
  EXPECT_EQ(
      xanadu::reservePublicationSequence(dir, other.publicKey, "doc:ideas"), 1);
  EXPECT_EQ(xanadu::reservePublicationSequence(dir, keys.publicKey, "doc:ideas",
                                               1700000000),
            1700000001);
  EXPECT_EQ(
      xanadu::reservePublicationSequence(dir, keys.publicKey, "doc:ideas"),
      1700000002);
  EXPECT_THROW((void)xanadu::reservePublicationSequence(
                   dir, keys.publicKey, "doc:ideas",
                   std::numeric_limits<std::int64_t>::max()),
               std::overflow_error);
  EXPECT_EQ(
      xanadu::reservePublicationSequence(dir, keys.publicKey, "doc:ideas"),
      1700000003);
  std::filesystem::remove_all(dir);
}

TEST(PublicationSequenceTest, simultaneousReservationsNeverReuseASequence) {
  const auto keys = xanadu::createMutableKeys();
  const auto dir  = std::filesystem::temp_directory_path() /
                   ("xudu-sequences-" + keys.publicKey.hex());
  std::vector<std::future<std::int64_t>> writers;
  for (int i = 0; i < 12; ++i) {
    writers.push_back(std::async(std::launch::async, [&] {
      return xanadu::reservePublicationSequence(dir, keys.publicKey,
                                                "doc:ideas");
    }));
  }
  std::set<std::int64_t> reservations;
  for (auto &writer : writers) reservations.insert(writer.get());
  EXPECT_EQ(reservations.size(), 12U);
  EXPECT_EQ(*reservations.begin(), 1);
  EXPECT_EQ(*reservations.rbegin(), 12);
  EXPECT_EQ(
      xanadu::reservePublicationSequence(dir, keys.publicKey, "doc:ideas"), 13);
  std::filesystem::remove_all(dir);
}

TEST(PublicationTest,
     seedWriterRejectsCorruptionWithoutOverwritingAnEarlierSeal) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("xudu-seed-" + xanadu::createMutableKeys().publicKey.hex());
  std::vector<xanadu::TorrentContent> files{{"primedia", "first edition"}};
  const auto torrent = xanadu::makeTorrent(files, "permascroll");
  const auto seed    = xanadu::writeTorrentSeed(dir, torrent, files);
  files.front().data = "wrong edition";
  EXPECT_THROW((void)xanadu::writeTorrentSeed(dir, torrent, files),
               std::invalid_argument);
  std::ifstream saved(seed / "permascroll" / "primedia", std::ios::binary);
  const std::string bytes{std::istreambuf_iterator<char>(saved),
                          std::istreambuf_iterator<char>()};
  EXPECT_EQ(bytes, "first edition");
  const auto traversal = xanadu::makeTorrent(files, "..");
  EXPECT_THROW((void)xanadu::writeTorrentSeed(dir, traversal, files),
               std::invalid_argument);
  std::filesystem::remove_all(dir);
}

TEST(PublicationSequenceTest, anUnknownCounterVersionCannotResetTheSequence) {
  const auto keys = xanadu::createMutableKeys();
  const auto dir  = std::filesystem::temp_directory_path() /
                   ("xudu-sequences-" + keys.publicKey.hex());
  EXPECT_EQ(
      xanadu::reservePublicationSequence(dir, keys.publicKey, "doc:ideas"), 1);
  {
    MDB_env *rawEnv = nullptr;
    ASSERT_EQ(mdb_env_create(&rawEnv), MDB_SUCCESS);
    const std::unique_ptr<MDB_env, decltype(&mdb_env_close)> env(rawEnv,
                                                                 mdb_env_close);
    ASSERT_EQ(mdb_env_open(env.get(), dir.string().c_str(), 0, 0600),
              MDB_SUCCESS);
    MDB_txn *rawTxn = nullptr;
    ASSERT_EQ(mdb_txn_begin(env.get(), nullptr, 0, &rawTxn), MDB_SUCCESS);
    std::unique_ptr<MDB_txn, decltype(&mdb_txn_abort)> txn(rawTxn,
                                                           mdb_txn_abort);
    MDB_dbi db;
    ASSERT_EQ(mdb_dbi_open(txn.get(), nullptr, 0, &db), MDB_SUCCESS);
    auto name = keys.publicKey.hex() + ":doc:ideas";
    MDB_val key{name.size(), name.data()}, value{};
    ASSERT_EQ(mdb_get(txn.get(), db, &key, &value), MDB_SUCCESS);
    std::string record(static_cast<const char *>(value.mv_data), value.mv_size);
    ASSERT_GE(record.size(), 4U);
    record[3] = '2';
    value     = MDB_val{record.size(), record.data()};
    ASSERT_EQ(mdb_put(txn.get(), db, &key, &value, 0), MDB_SUCCESS);
    ASSERT_EQ(mdb_txn_commit(txn.release()), MDB_SUCCESS);
  }
  EXPECT_THROW((void)xanadu::reservePublicationSequence(dir, keys.publicKey,
                                                        "doc:ideas"),
               xanadu::PublicationSequenceUnreadable);
  std::filesystem::remove_all(dir);
}

// Local offsets are into the store's own permascroll, so two stores reading
// different ones can only be compared once one names the other's span.
TEST(PublicationTest, aSpanIsComparedOnlyWhereTheOtherStoreCanNameIt) {
  const auto shared = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Store mine(shared);
  xanadu::Store sameScroll(shared);
  xanadu::Store otherScroll(std::make_shared<xanadu::UserPermascroll>());
  const xanadu::PrimediaSpan local{xanadu::localScroll, 10, 5};

  EXPECT_EQ(xanadu::spanIn(sameScroll, mine, local), local);
  EXPECT_EQ(xanadu::spanIn(mine, mine, local), local);
  EXPECT_FALSE(xanadu::spanIn(otherScroll, mine, local).has_value())
      << "offsets into another permascroll name other text";

  // An external scroll is renamed to the id the reading store knows it by,
  // and not named at all where that store has never heard of it.
  const auto author = xanadu::createMutableKeys();
  const auto scroll = namedScroll(author.publicKey, "permascroll", 1000);
  const auto there  = otherScroll.addScroll(
      namedScroll(xanadu::createMutableKeys().publicKey, "other", 1000));
  ASSERT_EQ(there, 1U);
  const auto fromId = otherScroll.addScroll(scroll);
  const xanadu::PrimediaSpan external{fromId, 100, 20};
  EXPECT_FALSE(xanadu::spanIn(otherScroll, mine, external).has_value());
  const auto hereId = mine.addScroll(scroll);
  ASSERT_NE(hereId, fromId);
  const auto named = xanadu::spanIn(otherScroll, mine, external);
  ASSERT_TRUE(named.has_value());
  EXPECT_EQ(named->scroll, hereId);
  EXPECT_EQ(named->start, 100U);
  EXPECT_EQ(named->length, 20U);
}

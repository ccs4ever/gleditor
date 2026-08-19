/**
 * @file e2e_integration.cpp
 * @brief End-to-end integration tests covering:
 *  1. Loading 2 source media torrents.
 *  2. Authoring and publishing 2 independent XanaDocs.
 *  3. Transcluding content between XanaDocs.
 *  4. Linking between XanaDocs (author and reader links).
 *  5. Creating and adopting 2 standalone LinkPackages (one cross-referencing
 *     the XanaDocs, and one referencing a 3rd XanaDoc/source to test
 *     materializing external sources).
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <xudu/core/link_layout.hpp>
#include <xudu/core/link_package.hpp>
#include <xudu/core/publication.hpp>
#include <xudu/core/store.hpp>
#include <xudu/core/torrent.hpp>
#include <xudu/core/version.hpp>

#include "torrent_data.hpp"

namespace {

using xudu::adopt;
using xudu::adoptLinkPackage;
using xudu::createMutableKeys;
using xudu::GlobalLink;
using xudu::GlobalSpan;
using xudu::HalfLink;
using xudu::InfoHash;
using xudu::Link;
using xudu::LinkedPair;
using xudu::linkPackageRendezvousTarget;
using xudu::LinkType;
using xudu::MicroversionId;
using xudu::MutableKeys;
using xudu::placeLinks;
using xudu::ProminenceTier;
using xudu::Publication;
using xudu::PublicKey;
using xudu::publish;
using xudu::publishLinkPackage;
using xudu::Scroll;
using xudu::ScrollSegment;
using xudu::Store;
using xudu::Version;

Scroll makeNamedScroll(const PublicKey &key, std::string salt,
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

std::vector<const Version *> viewing(const std::vector<Version> &versions) {
  std::vector<const Version *> out;
  out.reserve(versions.size());
  for (const auto &one : versions) {
    out.push_back(&one);
  }
  return out;
}

TEST(E2EIntegrationTest,
     fullLifecycleWithTwoSourcesTwoXanadocsAndTwoLinkPackages) {
  // =========================================================================
  // 1. SOURCE TORRENTS (2 source media torrents)
  // =========================================================================
  const auto src1Hash = InfoHash::fromHex(xudu_test::singleFileHash);
  const auto src2Hash = InfoHash::fromHex(xudu_test::multiFileHash);

  // Source 1: Fox text (62 bytes)
  const auto src1Scroll = Scroll::ofTorrentFile(
      src1Hash, 0, "fox.txt", 0, xudu_test::singleFileText.size());

  // Source 2: Multi-file second file (28 bytes)
  const auto src2Scroll = Scroll::ofTorrentFile(
      src2Hash, 1, "sub/two.txt", xudu_test::multiFileFirst.size(),
      xudu_test::multiFileSecond.size());

  // =========================================================================
  // 2. AUTHOR XANADOC A (Author A)
  // Quotes from Source 1 + original commentary
  // =========================================================================
  const auto authorA = createMutableKeys();
  const auto scrollA = makeNamedScroll(authorA.publicKey, "scrollA", 2000);

  Store storeA;
  // A transcludes "The quick brown fox" (19 bytes) from Source 1
  auto vA = storeA.transcludeExternal(MicroversionId{}, 0, src1Scroll, 0, 19);
  // A transcludes original commentary from own scroll (30 bytes)
  vA = storeA.transcludeExternal(vA, 19, scrollA, 0, 30);

  // Author A adds an internal link on Doc A (e.g. quote to commentary)
  const auto spansA = storeA.rebuild(vA);
  Link authorLinkA;
  authorLinkA.type  = LinkType::Comment;
  authorLinkA.tier  = ProminenceTier::Author;
  authorLinkA.left  = spansA.spansFor(0, 19);
  authorLinkA.right = spansA.spansFor(19, 30);
  vA                = storeA.addLink(vA, authorLinkA);

  // Publish XanaDoc A
  const auto pubA =
      publish(storeA, vA, authorA, "docA", "Document A", 1, 1700000000);

  // =========================================================================
  // 3. AUTHOR XANADOC B (Author B)
  // Quotes from Source 2 + transcludes from XanaDoc A's commentary
  // =========================================================================
  const auto authorB = createMutableKeys();
  const auto scrollB = makeNamedScroll(authorB.publicKey, "scrollB", 2000);

  Store storeB;
  // B transcludes "And the second file follows." (28 bytes) from Source 2
  auto vB = storeB.transcludeExternal(MicroversionId{}, 0, src2Scroll, 0, 28);
  // B transcludes Author A's commentary (30 bytes from scrollA)
  vB = storeB.transcludeExternal(vB, 28, scrollA, 0, 30);
  // B adds original text from own scroll (25 bytes)
  vB = storeB.transcludeExternal(vB, 58, scrollB, 0, 25);

  // B creates an Author Link connecting B's original text to the transcluded commentary
  const auto spansB = storeB.rebuild(vB);
  Link authorLinkB;
  authorLinkB.type  = LinkType::Quotation;
  authorLinkB.tier  = ProminenceTier::Author;
  authorLinkB.left  = spansB.spansFor(58, 25);
  authorLinkB.right = spansB.spansFor(28, 30);
  vB                = storeB.addLink(vB, authorLinkB);

  // Publish XanaDoc B
  const auto pubB =
      publish(storeB, vB, authorB, "docB", "Document B", 1, 1700000000);

  // =========================================================================
  // 4. READER ADOPTION & CROSS-DOCUMENT LINK / TRANSCLUSION VALIDATION
  // =========================================================================
  Store readerStore;
  const auto adoptedA = adopt(readerStore, pubA);
  const auto adoptedB = adopt(readerStore, pubB);

  const auto verA = readerStore.rebuild(adoptedA.version);
  const auto verB = readerStore.rebuild(adoptedB.version);

  // Validate transclusion span lengths
  EXPECT_EQ(verA.length(), 19U + 30U);
  EXPECT_EQ(verB.length(), 28U + 30U + 25U);

  // Validate Author Links placed across open documents
  std::vector<LinkedPair> between;
  std::vector<HalfLink> leaving;
  placeLinks(readerStore.links(), viewing({verA, verB}), between, leaving);

  EXPECT_FALSE(between.empty());
  for (const auto &p : between) {
    EXPECT_EQ(p.tier, ProminenceTier::Author);
  }

  // Cross-document beam from Doc B's note (doc 1, 58..83) to Doc A's commentary (doc 0, 19..49)
  const auto authorBeamIt = std::ranges::find_if(
      between, [](const LinkedPair &p) {
        return p.from.doc == 1U && p.to.doc == 0U;
      });
  ASSERT_NE(authorBeamIt, between.end());
  EXPECT_EQ(authorBeamIt->type, LinkType::Quotation);
  EXPECT_EQ(authorBeamIt->from.start, 58U);
  EXPECT_EQ(authorBeamIt->from.end, 83U);
  EXPECT_EQ(authorBeamIt->to.start, 19U);
  EXPECT_EQ(authorBeamIt->to.end, 49U);

  // =========================================================================
  // 5. LINK PACKAGE 1 (Curated Package connecting Doc A and Doc B)
  // =========================================================================
  const auto curator1 = createMutableKeys();
  GlobalLink glink1;
  glink1.type = LinkType::Disagreement;
  glink1.left.push_back(
      GlobalSpan{"btpk:" + authorA.publicKey.hex() + ":scrollA", 0, 15});
  glink1.right.push_back(
      GlobalSpan{"btpk:" + authorB.publicKey.hex() + ":scrollB", 0, 15});

  std::map<std::string, Scroll> scrolls1;
  scrolls1.emplace("btpk:" + authorA.publicKey.hex() + ":scrollA", scrollA);
  scrolls1.emplace("btpk:" + authorB.publicKey.hex() + ":scrollB", scrollB);

  const auto pkg1 = publishLinkPackage(curator1, "pkg1", "Curator 1 Review", 1,
                                       1700000000, {glink1}, scrolls1);

  // Adopt Link Package 1
  const auto result1 =
      adoptLinkPackage(readerStore, pkg1, ProminenceTier::Curated);
  EXPECT_EQ(result1.linksAdopted, 1U);

  // Verify curated link connects Doc A (on scrollA) and Doc B (on scrollB)
  between.clear();
  leaving.clear();
  placeLinks(readerStore.links(), viewing({verA, verB}), between, leaving);

  const auto curatedIt = std::ranges::find_if(
      between, [](const LinkedPair &p) {
        return p.tier == ProminenceTier::Curated &&
               p.type == LinkType::Disagreement;
      });
  ASSERT_NE(curatedIt, between.end());
  EXPECT_EQ(curatedIt->from.doc, 0U); // Doc A (offset 19 into Doc A)
  EXPECT_EQ(curatedIt->from.start, 19U);
  EXPECT_EQ(curatedIt->from.end, 34U);
  EXPECT_EQ(curatedIt->to.doc, 1U); // Doc B (offset 58 into Doc B)
  EXPECT_EQ(curatedIt->to.start, 58U);
  EXPECT_EQ(curatedIt->to.end, 73U);

  // =========================================================================
  // 6. LINK PACKAGE 2 (Public Package referencing a 3rd XanaDoc / Source C)
  // Tests LinkPackage's ability to materialize external sources
  // =========================================================================
  const auto authorC = createMutableKeys();
  const auto scrollC = makeNamedScroll(authorC.publicKey, "scrollC", 3000);

  // Curator 2 links Doc A's commentary span to 3rd document's scrollC
  const auto curator2 = createMutableKeys();
  GlobalLink glink2;
  glink2.type = LinkType::Illustration;
  // Left: Doc A's span on scrollA
  glink2.left.push_back(
      GlobalSpan{"btpk:" + authorA.publicKey.hex() + ":scrollA", 15, 15});
  // Right: 3rd Document C's span on scrollC
  glink2.right.push_back(
      GlobalSpan{"btpk:" + authorC.publicKey.hex() + ":scrollC", 100, 50});

  // Package carries scrollA and the 3rd source scrollC
  std::map<std::string, Scroll> scrolls2;
  scrolls2.emplace("btpk:" + authorA.publicKey.hex() + ":scrollA", scrollA);
  scrolls2.emplace("btpk:" + authorC.publicKey.hex() + ":scrollC", scrollC);

  const auto pkg2 = publishLinkPackage(curator2, "pkg2", "External Comparison",
                                       1, 1700000000, {glink2}, scrolls2);

  // Adopt Link Package 2 (Reader does not have Doc C opened yet!)
  const auto result2 =
      adoptLinkPackage(readerStore, pkg2, ProminenceTier::Public);
  EXPECT_EQ(result2.linksAdopted, 1U);
  EXPECT_GE(result2.scrollsAdded, 1U); // scrollC is newly materialized in store

  // Viewing only Doc A and Doc B: Link Package 2 manifests as a HalfLink
  // with elsewhere destination pointing to the materialized 3rd source scrollC
  between.clear();
  leaving.clear();
  placeLinks(readerStore.links(), viewing({verA, verB}), between, leaving);

  const auto publicHalfIt = std::ranges::find_if(
      leaving, [](const HalfLink &h) {
        return h.tier == ProminenceTier::Public &&
               h.type == LinkType::Illustration;
      });
  ASSERT_NE(publicHalfIt, leaving.end());
  EXPECT_EQ(publicHalfIt->here.doc, 0U);          // On Doc A
  EXPECT_EQ(publicHalfIt->here.start, 19U + 15U); // Offset into Doc A
  EXPECT_EQ(publicHalfIt->here.end, 19U + 30U);
  ASSERT_EQ(publicHalfIt->elsewhere.size(), 1U); // Points to materialized scrollC
  EXPECT_EQ(publicHalfIt->elsewhere[0].start, 100U);
  EXPECT_EQ(publicHalfIt->elsewhere[0].length, 50U);

  // Now Author C publishes Doc C and Reader adopts Doc C
  Store storeC;
  auto vC = storeC.transcludeExternal(MicroversionId{}, 0, scrollC, 100, 50);
  const auto pubC =
      publish(storeC, vC, authorC, "docC", "Document C", 1, 1700000000);

  const auto adoptedC = adopt(readerStore, pubC);
  const auto verC     = readerStore.rebuild(adoptedC.version);

  // When Reader opens Doc A and Doc C: the Link Package completes into a LinkedPair beam!
  between.clear();
  leaving.clear();
  placeLinks(readerStore.links(), viewing({verA, verC}), between, leaving);

  const auto beamACIt = std::ranges::find_if(
      between, [](const LinkedPair &p) {
        return p.tier == ProminenceTier::Public &&
               p.type == LinkType::Illustration;
      });
  ASSERT_NE(beamACIt, between.end());
  EXPECT_EQ(beamACIt->from.doc, 0U); // Doc A
  EXPECT_EQ(beamACIt->from.start, 19U + 15U);
  EXPECT_EQ(beamACIt->from.end, 19U + 30U);
  EXPECT_EQ(beamACIt->to.doc, 1U); // Doc C
  EXPECT_EQ(beamACIt->to.start, 0U);
  EXPECT_EQ(beamACIt->to.end, 50U);
}

TEST(E2EIntegrationTest, transclusionInheritsPrimalLinksAcrossAdoptions) {
  // Primal scroll shared across documents
  const auto primalAuthor = createMutableKeys();
  const auto primalScroll =
      makeNamedScroll(primalAuthor.publicKey, "primal", 500);

  // Document 1 (Doc 1) transcludes [50..150] from primalScroll
  const auto author1 = createMutableKeys();
  Store store1;
  const auto v1 =
      store1.transcludeExternal(MicroversionId{}, 0, primalScroll, 50, 100);
  const auto pub1 =
      publish(store1, v1, author1, "doc1", "Doc 1", 1, 1700000000);

  // Document 2 (Doc 2) quotes [70..120] from primalScroll
  const auto author2 = createMutableKeys();
  Store store2;
  const auto v2 =
      store2.transcludeExternal(MicroversionId{}, 0, primalScroll, 70, 50);
  const auto pub2 =
      publish(store2, v2, author2, "doc2", "Doc 2", 1, 1700000000);

  // Curator creates a link package targeting primalScroll [80..100]
  const auto curator = createMutableKeys();
  GlobalLink glink;
  glink.type = LinkType::Comment;
  glink.left.push_back(
      GlobalSpan{"btpk:" + primalAuthor.publicKey.hex() + ":primal", 80, 20});
  // Right end connects to an illustration on Doc 1 ([120..140] in primalScroll)
  glink.right.push_back(
      GlobalSpan{"btpk:" + primalAuthor.publicKey.hex() + ":primal", 120, 20});

  std::map<std::string, Scroll> scrolls;
  scrolls.emplace("btpk:" + primalAuthor.publicKey.hex() + ":primal",
                  primalScroll);

  const auto pkg = publishLinkPackage(curator, "pkg", "Primal Notes", 1,
                                      1700000000, {glink}, scrolls);

  // Reader adopts Doc 1, Doc 2, and the LinkPackage
  Store readerStore;
  const auto adopted1 = adopt(readerStore, pub1);
  const auto adopted2 = adopt(readerStore, pub2);
  adoptLinkPackage(readerStore, pkg, ProminenceTier::Curated);

  const auto ver1 = readerStore.rebuild(adopted1.version);
  const auto ver2 = readerStore.rebuild(adopted2.version);

  // When viewing both Doc 1 and Doc 2:
  // Cross-document transclusion beam from Doc 2 (10..30) to Doc 1 (70..90)
  std::vector<LinkedPair> between;
  std::vector<HalfLink> leaving;
  placeLinks(readerStore.links(), viewing({ver1, ver2}), between, leaving);

  ASSERT_EQ(between.size(), 1U);
  EXPECT_EQ(between[0].from.doc, 1U); // Doc 2 (offset 10..30)
  EXPECT_EQ(between[0].from.start, 10U);
  EXPECT_EQ(between[0].from.end, 30U);
  EXPECT_EQ(between[0].to.doc, 0U); // Doc 1 (offset 70..90)
  EXPECT_EQ(between[0].to.start, 70U);
  EXPECT_EQ(between[0].to.end, 90U);

  // And on Doc 2 alone ([80..100] is at offset 10 in Doc 2, [120..140] is not in Doc 2):
  between.clear();
  leaving.clear();
  placeLinks(readerStore.links(), viewing({ver2}), between, leaving);

  const auto halfDoc2It = std::ranges::find_if(leaving, [](const HalfLink &h) {
    return h.tier == ProminenceTier::Curated && h.type == LinkType::Comment;
  });
  ASSERT_NE(halfDoc2It, leaving.end());
  EXPECT_EQ(halfDoc2It->here.doc, 0U);
  EXPECT_EQ(halfDoc2It->here.start, 10U);
  EXPECT_EQ(halfDoc2It->here.end, 30U);
}

} // namespace

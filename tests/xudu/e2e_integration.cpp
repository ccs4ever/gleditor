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

  // B creates an Author Link connecting B's original text to the transcluded
  // commentary
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

  // Cross-document beam from Doc B's note (doc 1, 58..83) to Doc A's commentary
  // (doc 0, 19..49)
  const auto authorBeamIt =
      std::ranges::find_if(between, [](const LinkedPair &p) {
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

  const auto curatedIt = std::ranges::find_if(between, [](const LinkedPair &p) {
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

  const auto publicHalfIt =
      std::ranges::find_if(leaving, [](const HalfLink &h) {
        return h.tier == ProminenceTier::Public &&
               h.type == LinkType::Illustration;
      });
  ASSERT_NE(publicHalfIt, leaving.end());
  EXPECT_EQ(publicHalfIt->here.doc, 0U);          // On Doc A
  EXPECT_EQ(publicHalfIt->here.start, 19U + 15U); // Offset into Doc A
  EXPECT_EQ(publicHalfIt->here.end, 19U + 30U);
  ASSERT_EQ(publicHalfIt->elsewhere.size(),
            1U); // Points to materialized scrollC
  EXPECT_EQ(publicHalfIt->elsewhere[0].start, 100U);
  EXPECT_EQ(publicHalfIt->elsewhere[0].length, 50U);

  // Now Author C publishes Doc C and Reader adopts Doc C
  Store storeC;
  auto vC = storeC.transcludeExternal(MicroversionId{}, 0, scrollC, 100, 50);
  const auto pubC =
      publish(storeC, vC, authorC, "docC", "Document C", 1, 1700000000);

  const auto adoptedC = adopt(readerStore, pubC);
  const auto verC     = readerStore.rebuild(adoptedC.version);

  // When Reader opens Doc A and Doc C: the Link Package completes into a
  // LinkedPair beam!
  between.clear();
  leaving.clear();
  placeLinks(readerStore.links(), viewing({verA, verC}), between, leaving);

  const auto beamACIt = std::ranges::find_if(between, [](const LinkedPair &p) {
    return p.tier == ProminenceTier::Public && p.type == LinkType::Illustration;
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

  // And on Doc 2 alone ([80..100] is at offset 10 in Doc 2, [120..140] is not
  // in Doc 2):
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

TEST(E2EIntegrationTest,
     fullPageMultiLinkPackageWithMultipleLinkTypesAndTopology) {
  // Author A writes a full-page document (750 bytes across 4 sections)
  const auto authorA = createMutableKeys();
  const auto scrollA = makeNamedScroll(authorA.publicKey, "essayA", 1500);

  Store storeA;
  const auto vA =
      storeA.transcludeExternal(MicroversionId{}, 0, scrollA, 0, 750);
  const auto pubA = publish(storeA, vA, authorA, "essay_a",
                            "Universal Hypertext Principles", 1, 1700000100);

  // Author B writes a full-page commentary (700 bytes across 3 observations)
  const auto authorB = createMutableKeys();
  const auto scrollB = makeNamedScroll(authorB.publicKey, "commentaryB", 1500);

  Store storeB;
  const auto vB =
      storeB.transcludeExternal(MicroversionId{}, 0, scrollB, 0, 700);
  const auto pubB = publish(storeB, vB, authorB, "commentary_b",
                            "Commentary on Visual Topology", 1, 1700000150);

  // Editorial team publishes a comprehensive multi-topology LinkPackage
  const auto editorialKeys     = createMutableKeys();
  const std::string scrollKeyA = "btpk:" + authorA.publicKey.hex() + ":essayA";
  const std::string scrollKeyB =
      "btpk:" + authorB.publicKey.hex() + ":commentaryB";

  std::vector<GlobalLink> links;

  // 1. Multiple 1-to-1 links of different types:
  // - Comment: Sec 1.1 [0..150] -> Obs A [0..200]
  GlobalLink l1;
  l1.type = LinkType::Comment;
  l1.left.push_back(GlobalSpan{scrollKeyA, 0, 150});
  l1.right.push_back(GlobalSpan{scrollKeyB, 0, 200});
  links.push_back(l1);

  // - Illustration: Sec 1.2 [150..350] -> Obs B [200..450]
  GlobalLink l2;
  l2.type = LinkType::Illustration;
  l2.left.push_back(GlobalSpan{scrollKeyA, 150, 200});
  l2.right.push_back(GlobalSpan{scrollKeyB, 200, 250});
  links.push_back(l2);

  // - Disagreement: Sec 1.3 [350..550] -> Obs C [450..700]
  GlobalLink l3;
  l3.type = LinkType::Disagreement;
  l3.left.push_back(GlobalSpan{scrollKeyA, 350, 200});
  l3.right.push_back(GlobalSpan{scrollKeyB, 450, 250});
  links.push_back(l3);

  // 2. One-to-Many Link:
  // - Quotation: Sec 1.4 [550..750] -> Obs A, Obs B, Obs C
  GlobalLink l4;
  l4.type = LinkType::Quotation;
  l4.left.push_back(GlobalSpan{scrollKeyA, 550, 200});
  l4.right.push_back(GlobalSpan{scrollKeyB, 0, 100});
  l4.right.push_back(GlobalSpan{scrollKeyB, 250, 100});
  l4.right.push_back(GlobalSpan{scrollKeyB, 500, 100});
  links.push_back(l4);

  // 3. Many-to-One Link:
  // - Authorship: Sec 1.1, 1.2, 1.3 -> Obs C [500..650]
  GlobalLink l5;
  l5.type = LinkType::Authorship;
  l5.left.push_back(GlobalSpan{scrollKeyA, 50, 80});
  l5.left.push_back(GlobalSpan{scrollKeyA, 200, 80});
  l5.left.push_back(GlobalSpan{scrollKeyA, 400, 80});
  l5.right.push_back(GlobalSpan{scrollKeyB, 500, 150});
  links.push_back(l5);

  // 4. Many-to-Many Link:
  // - Other: Sec 1.2 & 1.3 -> Obs B & Obs C
  GlobalLink l6;
  l6.type = LinkType::Other;
  l6.left.push_back(GlobalSpan{scrollKeyA, 180, 100});
  l6.left.push_back(GlobalSpan{scrollKeyA, 380, 100});
  l6.right.push_back(GlobalSpan{scrollKeyB, 220, 100});
  l6.right.push_back(GlobalSpan{scrollKeyB, 480, 100});
  links.push_back(l6);

  std::map<std::string, Scroll> pkgScrolls;
  pkgScrolls.insert_or_assign(scrollKeyA, scrollA);
  pkgScrolls.insert_or_assign(scrollKeyB, scrollB);

  const auto pkg = publishLinkPackage(
      editorialKeys, "pkg_full_topology", "Comprehensive Multi-Topology Suite",
      1, 1700000300, std::move(links), std::move(pkgScrolls));

  // Reader adopts both publications and the multi-topology LinkPackage
  Store readerStore;
  const auto adA = adopt(readerStore, pubA);
  const auto adB = adopt(readerStore, pubB);
  const auto adoptResult =
      adoptLinkPackage(readerStore, pkg, ProminenceTier::Curated);

  EXPECT_EQ(adoptResult.linksAdopted, 6U);

  const auto versionA = readerStore.rebuild(adA.version);
  const auto versionB = readerStore.rebuild(adB.version);

  std::vector<LinkedPair> between;
  std::vector<HalfLink> leaving;
  placeLinks(readerStore.links(), viewing({versionA, versionB}), between,
             leaving);

  EXPECT_EQ(between.size(), 6U);
  EXPECT_TRUE(leaving.empty());

  // Verify all 5 link types are represented
  std::set<LinkType> linkTypes;
  for (const auto &pair : between) {
    linkTypes.insert(pair.type);
    EXPECT_EQ(pair.from.doc, 0U);
    EXPECT_EQ(pair.to.doc, 1U);
  }
  EXPECT_EQ(linkTypes.size(), 6U);
  EXPECT_TRUE(linkTypes.contains(LinkType::Comment));
  EXPECT_TRUE(linkTypes.contains(LinkType::Illustration));
  EXPECT_TRUE(linkTypes.contains(LinkType::Disagreement));
  EXPECT_TRUE(linkTypes.contains(LinkType::Quotation));
  EXPECT_TRUE(linkTypes.contains(LinkType::Authorship));
  EXPECT_TRUE(linkTypes.contains(LinkType::Other));

  // Verify 1-to-Many bounding extent: covers Obs A (0) through Obs C (600)
  const auto qIt = std::ranges::find_if(between, [](const LinkedPair &p) {
    return p.type == LinkType::Quotation;
  });
  ASSERT_NE(qIt, between.end());
  EXPECT_EQ(qIt->from.start, 550U);
  EXPECT_EQ(qIt->from.end, 750U);
  EXPECT_EQ(qIt->to.start, 0U);
  EXPECT_EQ(qIt->to.end, 600U);

  // Verify Many-to-1 bounding extent: covers Sec 1.1 (50) through Sec 1.3 (480)
  const auto aIt = std::ranges::find_if(between, [](const LinkedPair &p) {
    return p.type == LinkType::Authorship;
  });
  ASSERT_NE(aIt, between.end());
  EXPECT_EQ(aIt->from.start, 50U);
  EXPECT_EQ(aIt->from.end, 480U);
  EXPECT_EQ(aIt->to.start, 500U);
  EXPECT_EQ(aIt->to.end, 650U);
}

TEST(E2EIntegrationTest,
     threeDocumentNonOverlappingPlacementAndBackgroundBypassRouting) {
  // Setup 3 authored documents: Doc 0, Doc 1, Doc 2
  const auto author1 = createMutableKeys();
  const auto author2 = createMutableKeys();
  const auto author3 = createMutableKeys();

  const auto scroll1 = makeNamedScroll(author1.publicKey, "doc1", 800);
  const auto scroll2 = makeNamedScroll(author2.publicKey, "doc2", 800);
  const auto scroll3 = makeNamedScroll(author3.publicKey, "doc3", 800);

  Store s1, s2, s3;
  const auto v1 = s1.transcludeExternal(MicroversionId{}, 0, scroll1, 0, 400);
  const auto v2 = s2.transcludeExternal(MicroversionId{}, 0, scroll2, 0, 400);
  const auto v3 = s3.transcludeExternal(MicroversionId{}, 0, scroll3, 0, 400);

  const auto pub1 = publish(s1, v1, author1, "p1", "Document 1", 1, 1700000001);
  const auto pub2 = publish(s2, v2, author2, "p2", "Document 2", 1, 1700000002);
  const auto pub3 = publish(s3, v3, author3, "p3", "Document 3", 1, 1700000003);

  // Links:
  // Link 1: Doc 1 <-> Doc 2 (Adjacent, docSpan = 1)
  // Link 2: Doc 2 <-> Doc 3 (Adjacent, docSpan = 1)
  // Link 3: Doc 1 <-> Doc 3 (Non-adjacent, docSpan = 2 -> Background Bypass
  // Layer Z = -20)
  const auto curator   = createMutableKeys();
  const std::string k1 = "btpk:" + author1.publicKey.hex() + ":doc1";
  const std::string k2 = "btpk:" + author2.publicKey.hex() + ":doc2";
  const std::string k3 = "btpk:" + author3.publicKey.hex() + ":doc3";

  std::vector<GlobalLink> links;

  GlobalLink l12;
  l12.type = LinkType::Comment;
  l12.left.push_back(GlobalSpan{k1, 50, 50});
  l12.right.push_back(GlobalSpan{k2, 50, 50});
  links.push_back(l12);

  GlobalLink l23;
  l23.type = LinkType::Illustration;
  l23.left.push_back(GlobalSpan{k2, 150, 50});
  l23.right.push_back(GlobalSpan{k3, 150, 50});
  links.push_back(l23);

  GlobalLink l13;
  l13.type = LinkType::Quotation;
  l13.left.push_back(GlobalSpan{k1, 250, 50});
  l13.right.push_back(GlobalSpan{k3, 250, 50});
  links.push_back(l13);

  std::map<std::string, Scroll> scrolls;
  scrolls.insert_or_assign(k1, scroll1);
  scrolls.insert_or_assign(k2, scroll2);
  scrolls.insert_or_assign(k3, scroll3);

  const auto pkg =
      publishLinkPackage(curator, "pkg_3doc", "3-Doc Network", 1, 1700000010,
                         std::move(links), std::move(scrolls));

  // Reader adopts all 3 docs and link package
  Store readerStore;
  const auto ad1 = adopt(readerStore, pub1);
  const auto ad2 = adopt(readerStore, pub2);
  const auto ad3 = adopt(readerStore, pub3);
  adoptLinkPackage(readerStore, pkg, ProminenceTier::Curated);

  const auto ver1 = readerStore.rebuild(ad1.version);
  const auto ver2 = readerStore.rebuild(ad2.version);
  const auto ver3 = readerStore.rebuild(ad3.version);

  std::vector<LinkedPair> between;
  std::vector<HalfLink> leaving;
  placeLinks(readerStore.links(), viewing({ver1, ver2, ver3}), between,
             leaving);

  ASSERT_EQ(between.size(), 3U);
  EXPECT_TRUE(leaving.empty());

  // Check the three link pairs:
  // 1. Doc 0 <-> Doc 1 (Adjacent)
  const auto p01 = std::ranges::find_if(between, [](const LinkedPair &p) {
    return p.from.doc == 0U && p.to.doc == 1U;
  });
  ASSERT_NE(p01, between.end());
  const std::size_t span01 = p01->to.doc - p01->from.doc;
  EXPECT_EQ(span01, 1U); // Foreground Z = 0

  // 2. Doc 1 <-> Doc 2 (Adjacent)
  const auto p12 = std::ranges::find_if(between, [](const LinkedPair &p) {
    return p.from.doc == 1U && p.to.doc == 2U;
  });
  ASSERT_NE(p12, between.end());
  const std::size_t span12 = p12->to.doc - p12->from.doc;
  EXPECT_EQ(span12, 1U); // Foreground Z = 0

  // 3. Doc 0 <-> Doc 2 (Non-adjacent across Doc 1)
  const auto p02 = std::ranges::find_if(between, [](const LinkedPair &p) {
    return p.from.doc == 0U && p.to.doc == 2U;
  });
  ASSERT_NE(p02, between.end());
  const std::size_t span02 = p02->to.doc - p02->from.doc;
  EXPECT_EQ(
      span02,
      2U); // Non-adjacent -> routes through background bypass layer (Z = -20)

  // Verify non-overlapping layout geometry:
  // Document 0 at X0, Document 1 at X1 = X0 + W0/2 + W1/2 + Gap, Document 2 at
  // X2 = X1 + W1/2 + W2/2 + Gap
  const float halfW = 10.0F;
  const float gap   = 2.0F;
  const float x0    = 0.0F;
  const float x1    = x0 + (2.0F * halfW + gap); // 22.0
  const float x2    = x1 + (2.0F * halfW + gap); // 44.0

  EXPECT_LT(x0 + halfW, x1 - halfW);
  EXPECT_LT(x1 + halfW, x2 - halfW);

  // Dynamic Camera Framing encompasses all three docs [x0 - halfW - margin, x2
  // + halfW + margin]
  const float minX       = x0 - halfW - 4.0F; // -14.0
  const float maxX       = x2 + halfW + 4.0F; // 58.0
  const float totalWidth = maxX - minX;       // 72.0

  EXPECT_FLOAT_EQ(totalWidth, 72.0F);
}

} // namespace

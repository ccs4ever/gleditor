/**
 * @file link_package.cpp
 * @brief Tests for standalone link packages, author blessings, discovery
 * ranking, and XanaDoc interactions.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <xudu/core/blessing.hpp>
#include <xudu/core/link_discovery.hpp>
#include <xudu/core/link_layout.hpp>
#include <xudu/core/link_package.hpp>
#include <xudu/core/ops.hpp>
#include <xudu/core/publication.hpp>
#include <xudu/core/store.hpp>
#include <xudu/core/swarm.hpp>

namespace {

using xudu::adopt;
using xudu::adoptLinkPackage;
using xudu::Blessing;
using xudu::createBlessing;
using xudu::createMutableKeys;
using xudu::decodeBlessing;
using xudu::decodeLinkPackage;
using xudu::encodeBlessing;
using xudu::encodeLinkPackage;
using xudu::GlobalLink;
using xudu::GlobalSpan;
using xudu::HalfLink;
using xudu::linkColour;
using xudu::LinkDiscoveryEngine;
using xudu::LinkedPair;
using xudu::LinkPackage;
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
using xudu::verifyBlessing;
using xudu::verifyLinkPackage;
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
  for (const auto &v : versions) {
    out.push_back(&v);
  }
  return out;
}

TEST(LinkPackageTest, roundTripEncodingAndVerification) {
  const auto curatorKeys = createMutableKeys();
  const auto scrollKeys  = createMutableKeys();
  const auto scroll = makeNamedScroll(scrollKeys.publicKey, "scroll-1", 5000);

  GlobalLink glink;
  glink.type    = LinkType::Comment;
  glink.tier    = ProminenceTier::Curated;
  glink.owner   = "Critical Annotator";
  glink.curator = curatorKeys.publicKey.hex();
  glink.left.push_back(
      GlobalSpan{"btpk:" + scrollKeys.publicKey.hex() + ":scroll-1", 100, 50});
  glink.right.push_back(
      GlobalSpan{"btpk:" + scrollKeys.publicKey.hex() + ":scroll-1", 500, 30});

  std::map<std::string, Scroll> scrolls;
  scrolls.emplace("btpk:" + scrollKeys.publicKey.hex() + ":scroll-1", scroll);

  const auto pkg =
      publishLinkPackage(curatorKeys, "annotations-v1", "Critical Annotations",
                         1, 1700000000, {glink}, scrolls);

  EXPECT_TRUE(verifyLinkPackage(pkg));

  const std::string encoded = encodeLinkPackage(pkg);
  EXPECT_FALSE(encoded.empty());

  const auto decoded = decodeLinkPackage(encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(verifyLinkPackage(*decoded));
  EXPECT_EQ(decoded->title, "Critical Annotations");
  EXPECT_EQ(decoded->curator, curatorKeys.publicKey);
  EXPECT_EQ(decoded->sequence, 1);
  ASSERT_EQ(decoded->links.size(), 1U);
  EXPECT_EQ(decoded->links[0].type, LinkType::Comment);
  EXPECT_EQ(decoded->links[0].tier, ProminenceTier::Curated);
  EXPECT_EQ(decoded->links[0].owner, "Critical Annotator");
}

TEST(LinkPackageTest, tamperedPackageFailsVerification) {
  const auto curatorKeys = createMutableKeys();
  auto pkg =
      publishLinkPackage(curatorKeys, "notes", "Notes", 1, 1700000000, {}, {});
  EXPECT_TRUE(verifyLinkPackage(pkg));

  // Alter payload
  pkg.title = "Forged Title";
  EXPECT_FALSE(verifyLinkPackage(pkg));
}

TEST(LinkPackageTest, adoptionAddsLinksAndScrollsWithoutModifyingDocuments) {
  const auto authorKeys  = createMutableKeys();
  const auto curatorKeys = createMutableKeys();
  const auto scroll =
      makeNamedScroll(authorKeys.publicKey, "essay-scroll", 2000);

  Store store;
  // Local document typing
  const auto doc = store.insert(MicroversionId{}, 0, "The author typed this.");

  GlobalLink glink;
  glink.type = LinkType::Illustration;
  glink.left.push_back(GlobalSpan{
      "btpk:" + authorKeys.publicKey.hex() + ":essay-scroll", 0, 10});
  glink.right.push_back(GlobalSpan{
      "btpk:" + authorKeys.publicKey.hex() + ":essay-scroll", 100, 20});

  std::map<std::string, Scroll> scrolls;
  scrolls.emplace("btpk:" + authorKeys.publicKey.hex() + ":essay-scroll",
                  scroll);

  const auto pkg =
      publishLinkPackage(curatorKeys, "illustrations", "Illustrations", 1,
                         1700000000, {glink}, scrolls);

  const auto result = adoptLinkPackage(store, pkg, ProminenceTier::Curated);
  EXPECT_EQ(result.scrollsAdded, 1U);
  EXPECT_EQ(result.linksAdopted, 1U);

  ASSERT_EQ(store.links().size(), 1U);
  const auto &link = store.links().begin()->second;
  EXPECT_EQ(link.type, LinkType::Illustration);
  EXPECT_EQ(link.tier, ProminenceTier::Curated);
  EXPECT_EQ(link.curator, curatorKeys.publicKey.hex());
}

TEST(LinkPackageTest, unresolvableSpansAreSafelySkipped) {
  const auto curatorKeys = createMutableKeys();
  const auto knownKeys   = createMutableKeys();
  const auto knownScroll = makeNamedScroll(knownKeys.publicKey, "known", 1000);

  GlobalLink glinkValid;
  glinkValid.type = LinkType::Comment;
  glinkValid.left.push_back(
      GlobalSpan{"btpk:" + knownKeys.publicKey.hex() + ":known", 10, 20});
  glinkValid.right.push_back(
      GlobalSpan{"btpk:" + knownKeys.publicKey.hex() + ":known", 50, 20});

  GlobalLink glinkInvalid; // References an unknown scroll not in pkg.scrolls
  glinkInvalid.type = LinkType::Disagreement;
  glinkInvalid.left.push_back(GlobalSpan{"btpk:unknown_pubkey:unknown", 0, 10});
  glinkInvalid.right.push_back(
      GlobalSpan{"btpk:unknown_pubkey:unknown", 20, 10});

  std::map<std::string, Scroll> scrolls;
  scrolls.emplace("btpk:" + knownKeys.publicKey.hex() + ":known", knownScroll);

  const auto pkg =
      publishLinkPackage(curatorKeys, "mixed", "Mixed", 1, 1700000000,
                         {glinkValid, glinkInvalid}, scrolls);

  Store store;
  const auto result = adoptLinkPackage(store, pkg, ProminenceTier::Curated);
  EXPECT_EQ(result.linksAdopted, 1U);
  ASSERT_EQ(store.links().size(), 1U);
  EXPECT_EQ(store.links().begin()->second.type, LinkType::Comment);
}

// -----------------------------------------------------------------------------
// Cross-Document Interactions with XanaDocs
// -----------------------------------------------------------------------------

TEST(LinkPackageInteractionTest,
     crossDocumentLinkingBetweenTwoAdoptedXanadocs) {
  const auto authorA = createMutableKeys();
  const auto authorB = createMutableKeys();
  const auto curator = createMutableKeys();

  const auto scrollA = makeNamedScroll(authorA.publicKey, "scrollA", 1000);
  const auto scrollB = makeNamedScroll(authorB.publicKey, "scrollB", 1000);

  // Author A writes and publishes Doc A (100 bytes from scrollA at offset 50)
  Store storeA;
  const auto verA =
      storeA.transcludeExternal(MicroversionId{}, 0, scrollA, 50, 100);
  const auto pubA =
      publish(storeA, verA, authorA, "docA", "Document A", 1, 1700000000);

  // Author B writes and publishes Doc B (200 bytes from scrollB at offset 100)
  Store storeB;
  const auto verB =
      storeB.transcludeExternal(MicroversionId{}, 0, scrollB, 100, 200);
  const auto pubB =
      publish(storeB, verB, authorB, "docB", "Document B", 1, 1700000000);

  // Third-party Curator publishes a LinkPackage connecting Doc A's span to Doc
  // B's span
  GlobalLink crossLink;
  crossLink.type  = LinkType::Quotation;
  crossLink.owner = "Literary Comparison";
  crossLink.left.push_back(
      GlobalSpan{"btpk:" + authorA.publicKey.hex() + ":scrollA", 60, 30});
  crossLink.right.push_back(
      GlobalSpan{"btpk:" + authorB.publicKey.hex() + ":scrollB", 120, 40});

  std::map<std::string, Scroll> pkgScrolls;
  pkgScrolls.emplace("btpk:" + authorA.publicKey.hex() + ":scrollA", scrollA);
  pkgScrolls.emplace("btpk:" + authorB.publicKey.hex() + ":scrollB", scrollB);

  const auto pkg =
      publishLinkPackage(curator, "comparison-pkg", "Literary Comparison", 1,
                         1700000000, {crossLink}, pkgScrolls);

  // A reader opens a fresh store, adopts Doc A, Doc B, and the third-party
  // LinkPackage
  Store readerStore;
  const auto adoptedA = adopt(readerStore, pubA);
  const auto adoptedB = adopt(readerStore, pubB);
  const auto adoptedPkg =
      adoptLinkPackage(readerStore, pkg, ProminenceTier::Curated);

  EXPECT_EQ(adoptedPkg.linksAdopted, 1U);

  // View both open documents on screen
  const std::vector<Version> openDocs = {
      readerStore.rebuild(adoptedA.version),
      readerStore.rebuild(adoptedB.version),
  };

  std::vector<LinkedPair> between;
  std::vector<HalfLink> leaving;
  placeLinks(readerStore.links(), viewing(openDocs), between, leaving);

  // Verify the connection beam between Doc A and Doc B was placed
  ASSERT_EQ(between.size(), 1U);
  EXPECT_EQ(between[0].from.doc, 0U) << "Left end in Doc A";
  EXPECT_EQ(between[0].from.start, 10U)
      << "Offset 60 is 10 bytes into Doc A (starts at 50)";
  EXPECT_EQ(between[0].from.end, 40U);
  EXPECT_EQ(between[0].to.doc, 1U) << "Right end in Doc B";
  EXPECT_EQ(between[0].to.start, 20U)
      << "Offset 120 is 20 bytes into Doc B (starts at 100)";
  EXPECT_EQ(between[0].to.end, 60U);
  EXPECT_EQ(between[0].tier, ProminenceTier::Curated);
  EXPECT_EQ(between[0].type, LinkType::Quotation);
  EXPECT_TRUE(leaving.empty());
}

TEST(LinkPackageInteractionTest, halfLinkReportedWhenOnlyOneDocumentIsOpen) {
  const auto authorA = createMutableKeys();
  const auto authorB = createMutableKeys();
  const auto curator = createMutableKeys();

  const auto scrollA = makeNamedScroll(authorA.publicKey, "scrollA", 1000);
  const auto scrollB = makeNamedScroll(authorB.publicKey, "scrollB", 1000);

  Store storeA;
  const auto verA =
      storeA.transcludeExternal(MicroversionId{}, 0, scrollA, 0, 100);
  const auto pubA =
      publish(storeA, verA, authorA, "docA", "Document A", 1, 1700000000);

  GlobalLink crossLink;
  crossLink.type = LinkType::Comment;
  crossLink.left.push_back(
      GlobalSpan{"btpk:" + authorA.publicKey.hex() + ":scrollA", 10, 20});
  crossLink.right.push_back(
      GlobalSpan{"btpk:" + authorB.publicKey.hex() + ":scrollB", 50, 30});

  std::map<std::string, Scroll> pkgScrolls;
  pkgScrolls.emplace("btpk:" + authorA.publicKey.hex() + ":scrollA", scrollA);
  pkgScrolls.emplace("btpk:" + authorB.publicKey.hex() + ":scrollB", scrollB);

  const auto pkg = publishLinkPackage(curator, "pkg", "Package", 1, 1700000000,
                                      {crossLink}, pkgScrolls);

  Store readerStore;
  const auto adoptedA = adopt(readerStore, pubA);
  adoptLinkPackage(readerStore, pkg, ProminenceTier::Curated);

  // Only Doc A is open
  const std::vector<Version> openDocs = {readerStore.rebuild(adoptedA.version)};

  std::vector<LinkedPair> between;
  std::vector<HalfLink> leaving;
  placeLinks(readerStore.links(), viewing(openDocs), between, leaving);

  EXPECT_TRUE(between.empty());
  ASSERT_EQ(leaving.size(), 1U);
  EXPECT_EQ(leaving[0].here.doc, 0U);
  EXPECT_EQ(leaving[0].here.start, 10U);
  EXPECT_EQ(leaving[0].here.end, 30U);
  EXPECT_EQ(leaving[0].tier, ProminenceTier::Curated);
  ASSERT_EQ(leaving[0].elsewhere.size(), 1U);
  EXPECT_EQ(leaving[0].elsewhere[0].start, 50U);
  EXPECT_EQ(leaving[0].elsewhere[0].length, 30U);
}

TEST(LinkPackageInteractionTest, multiTierCoexistenceAcrossDocuments) {
  const auto authorA  = createMutableKeys();
  const auto authorB  = createMutableKeys();
  const auto curator  = createMutableKeys();
  const auto stranger = createMutableKeys();

  const auto scrollA = makeNamedScroll(authorA.publicKey, "scrollA", 1000);
  const auto scrollB = makeNamedScroll(authorB.publicKey, "scrollB", 1000);

  // Author A includes an Author link in Doc A
  Store storeA;
  auto verA = storeA.transcludeExternal(MicroversionId{}, 0, scrollA, 0, 100);
  const auto spansA = storeA.rebuild(verA);
  xudu::Link authorLink;
  authorLink.type  = LinkType::Authorship;
  authorLink.tier  = ProminenceTier::Author;
  authorLink.left  = spansA.spansFor(0, 10);
  authorLink.right = spansA.spansFor(50, 10);
  verA             = storeA.addLink(verA, authorLink);
  const auto pubA =
      publish(storeA, verA, authorA, "docA", "Doc A", 1, 1700000000);

  Store storeB;
  const auto verB =
      storeB.transcludeExternal(MicroversionId{}, 0, scrollB, 0, 100);
  const auto pubB =
      publish(storeB, verB, authorB, "docB", "Doc B", 1, 1700000000);

  // Curated link connecting Doc A to Doc B
  GlobalLink curatedLink;
  curatedLink.type = LinkType::Comment;
  curatedLink.left.push_back(
      GlobalSpan{"btpk:" + authorA.publicKey.hex() + ":scrollA", 20, 10});
  curatedLink.right.push_back(
      GlobalSpan{"btpk:" + authorB.publicKey.hex() + ":scrollB", 20, 10});
  std::map<std::string, Scroll> pkgScrolls = {
      {"btpk:" + authorA.publicKey.hex() + ":scrollA", scrollA},
      {"btpk:" + authorB.publicKey.hex() + ":scrollB", scrollB},
  };
  const auto curatedPkg =
      publishLinkPackage(curator, "c-pkg", "Curated Pkg", 1, 1700000000,
                         {curatedLink}, pkgScrolls);

  // Public link connecting Doc A to Doc B
  GlobalLink publicLink;
  publicLink.type = LinkType::Disagreement;
  publicLink.left.push_back(
      GlobalSpan{"btpk:" + authorA.publicKey.hex() + ":scrollA", 40, 10});
  publicLink.right.push_back(
      GlobalSpan{"btpk:" + authorB.publicKey.hex() + ":scrollB", 40, 10});
  const auto publicPkg = publishLinkPackage(
      stranger, "p-pkg", "Public Pkg", 1, 1700000000, {publicLink}, pkgScrolls);

  // Reader adopts everything
  Store readerStore;
  const auto adoptedA = adopt(readerStore, pubA);
  const auto adoptedB = adopt(readerStore, pubB);
  adoptLinkPackage(readerStore, curatedPkg, ProminenceTier::Curated);
  adoptLinkPackage(readerStore, publicPkg, ProminenceTier::Public);

  const std::vector<Version> openDocs = {
      readerStore.rebuild(adoptedA.version),
      readerStore.rebuild(adoptedB.version),
  };

  std::vector<LinkedPair> between;
  std::vector<HalfLink> leaving;
  placeLinks(readerStore.links(), viewing(openDocs), between, leaving);

  // Two cross-document links (Curated and Public) appear in `between`
  ASSERT_EQ(between.size(), 2U);
  bool foundCurated = false;
  bool foundPublic  = false;

  for (const auto &pair : between) {
    if (pair.tier == ProminenceTier::Curated) {
      foundCurated = true;
      EXPECT_EQ(pair.type, LinkType::Comment);
      EXPECT_EQ(linkColour(pair.type, pair.tier) & 0xFFU, 0xB0U);
    } else if (pair.tier == ProminenceTier::Public) {
      foundPublic = true;
      EXPECT_EQ(pair.type, LinkType::Disagreement);
      EXPECT_EQ(linkColour(pair.type, pair.tier) & 0xFFU, 0x60U);
    }
  }

  EXPECT_TRUE(foundCurated);
  EXPECT_TRUE(foundPublic);

  // The intra-document author link exists in store with Tier 1
  bool foundAuthorLink = false;
  for (const auto &[id, l] : readerStore.links()) {
    if (l.tier == ProminenceTier::Author) {
      foundAuthorLink = true;
      EXPECT_EQ(l.type, LinkType::Authorship);
      EXPECT_EQ(linkColour(l.type, l.tier) & 0xFFU, 0xE0U);
    }
  }
  EXPECT_TRUE(foundAuthorLink);
}

TEST(LinkPackageInteractionTest,
     linkPackageAttachesToTranscludedManifestations) {
  const auto authorA = createMutableKeys();
  const auto authorB = createMutableKeys();
  const auto authorC = createMutableKeys();
  const auto curator = createMutableKeys();

  const auto scrollA = makeNamedScroll(authorA.publicKey, "scrollA", 1000);
  const auto scrollB = makeNamedScroll(authorB.publicKey, "scrollB", 1000);
  const auto scrollC = makeNamedScroll(authorC.publicKey, "scrollC", 1000);

  // Doc B (target)
  Store storeB;
  const auto verB =
      storeB.transcludeExternal(MicroversionId{}, 0, scrollB, 0, 100);
  const auto pubB =
      publish(storeB, verB, authorB, "docB", "Doc B", 1, 1700000000);

  // Doc C quotes scrollC (prefix 8 bytes) and then transcludes scrollA
  // (50..100)
  Store storeC;
  auto verC = storeC.transcludeExternal(MicroversionId{}, 0, scrollC, 0, 8);
  verC      = storeC.transcludeExternal(verC, 8, scrollA, 50, 50);
  const auto pubC =
      publish(storeC, verC, authorC, "docC", "Doc C", 1, 1700000000);

  // LinkPackage attaches to scrollA [60..80]
  GlobalLink glink;
  glink.type = LinkType::Illustration;
  glink.left.push_back(
      GlobalSpan{"btpk:" + authorA.publicKey.hex() + ":scrollA", 60, 20});
  glink.right.push_back(
      GlobalSpan{"btpk:" + authorB.publicKey.hex() + ":scrollB", 10, 20});

  std::map<std::string, Scroll> pkgScrolls = {
      {"btpk:" + authorA.publicKey.hex() + ":scrollA", scrollA},
      {"btpk:" + authorB.publicKey.hex() + ":scrollB", scrollB},
  };
  const auto pkg = publishLinkPackage(curator, "pkg", "Pkg", 1, 1700000000,
                                      {glink}, pkgScrolls);

  // Reader opens Doc C and Doc B (Doc A is never opened!)
  Store readerStore;
  const auto adoptedC = adopt(readerStore, pubC);
  const auto adoptedB = adopt(readerStore, pubB);
  adoptLinkPackage(readerStore, pkg, ProminenceTier::Curated);

  const std::vector<Version> openDocs = {
      readerStore.rebuild(adoptedC.version),
      readerStore.rebuild(adoptedB.version),
  };

  std::vector<LinkedPair> between;
  std::vector<HalfLink> leaving;
  placeLinks(readerStore.links(), viewing(openDocs), between, leaving);

  // The link lands on Doc C because Doc C manifests the transcluded span!
  ASSERT_EQ(between.size(), 1U);
  EXPECT_EQ(between[0].from.doc, 0U) << "Lands on Doc C";
  EXPECT_EQ(between[0].from.start, 18U)
      << "Prefix (8) + offset into transclusion (60-50=10)";
  EXPECT_EQ(between[0].from.end, 38U);
  EXPECT_EQ(between[0].to.doc, 1U) << "Lands on Doc B";
  EXPECT_EQ(between[0].to.start, 10U);
  EXPECT_EQ(between[0].to.end, 30U);
}

// -----------------------------------------------------------------------------
// Blessings & Discovery Engine Tests
// -----------------------------------------------------------------------------

TEST(BlessingTest, roundTripAndVerification) {
  const auto authorKeys  = createMutableKeys();
  const auto curatorKeys = createMutableKeys();

  const std::string docKey = "btpk:" + authorKeys.publicKey.hex() + ":doc";
  const std::string pkgKey = "btpk:" + curatorKeys.publicKey.hex() + ":notes";

  const auto blessing = createBlessing(authorKeys, docKey, pkgKey,
                                       "Endorsed commentary", 1700000000);

  EXPECT_TRUE(verifyBlessing(blessing));

  const std::string encoded = encodeBlessing(blessing);
  EXPECT_FALSE(encoded.empty());

  const auto decoded = decodeBlessing(encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(verifyBlessing(*decoded));
  EXPECT_EQ(decoded->author, authorKeys.publicKey);
  EXPECT_EQ(decoded->targetDocument, docKey);
  EXPECT_EQ(decoded->endorsedPackage, pkgKey);
  EXPECT_EQ(decoded->note, "Endorsed commentary");
}

TEST(BlessingTest, tamperedBlessingFailsVerification) {
  const auto authorKeys = createMutableKeys();
  auto blessing =
      createBlessing(authorKeys, "doc-1", "pkg-1", "Note", 1700000000);
  EXPECT_TRUE(verifyBlessing(blessing));

  blessing.targetDocument = "doc-tampered";
  EXPECT_FALSE(verifyBlessing(blessing));
}

TEST(LinkDiscoveryTest, rankingAppliesTierAndBlessingWeights) {
  LinkDiscoveryEngine engine;

  const auto authorKeys   = createMutableKeys();
  const auto transAuthor  = createMutableKeys();
  const auto curator1Keys = createMutableKeys();
  const auto curator2Keys = createMutableKeys();
  const auto public1Keys  = createMutableKeys();
  const auto public2Keys  = createMutableKeys();

  const std::string docKey = "btpk:" + authorKeys.publicKey.hex() + ":essay";

  // Follow curator 1
  engine.followCurator(curator1Keys.publicKey);

  // Author blesses public1 package (+500)
  const auto blessing1 = createBlessing(
      authorKeys, docKey, "btpk:" + public1Keys.publicKey.hex() + ":public1",
      "Author approved", 1700000000);
  EXPECT_TRUE(engine.addBlessing(blessing1));

  // Transcluded author blesses public2 package (+200)
  const auto blessing2 = createBlessing(
      transAuthor, docKey, "btpk:" + public2Keys.publicKey.hex() + ":public2",
      "Transcluded author approved", 1700000000);
  EXPECT_TRUE(engine.addBlessing(blessing2));

  const auto pkgCurated1 = publishLinkPackage(curator1Keys, "c1", "Curated 1",
                                              1, 1700000000, {}, {});
  const auto pkgCurated2 = publishLinkPackage(curator2Keys, "c2", "Unfollowed",
                                              1, 1700000000, {}, {});
  const auto pkgBlessed1 = publishLinkPackage(
      public1Keys, "public1", "Author Blessed Public", 1, 1700000000, {}, {});
  const auto pkgBlessed2 =
      publishLinkPackage(public2Keys, "public2", "Transcluded Blessed Public",
                         1, 1700000000, {}, {});

  const std::vector<const LinkPackage *> candidates = {
      &pkgCurated2, &pkgBlessed2, &pkgBlessed1, &pkgCurated1};

  const auto ranked = engine.rankPackages(candidates, authorKeys.publicKey,
                                          docKey, {transAuthor.publicKey});

  ASSERT_EQ(ranked.size(), 4U);
  // 1: Curated 1 (score >= 1000)
  EXPECT_EQ(ranked[0].tier, ProminenceTier::Curated);
  EXPECT_EQ(ranked[0].package, &pkgCurated1);

  // 2: Author Blessed Public (score = 501)
  EXPECT_EQ(ranked[1].tier, ProminenceTier::Public);
  EXPECT_EQ(ranked[1].package, &pkgBlessed1);
  EXPECT_TRUE(ranked[1].hasAuthorBlessing);

  // 3: Transcluded Author Blessed Public (score = 201)
  EXPECT_EQ(ranked[2].tier, ProminenceTier::Public);
  EXPECT_EQ(ranked[2].package, &pkgBlessed2);

  // 4: Unfollowed Public (score = 1)
  EXPECT_EQ(ranked[3].tier, ProminenceTier::Public);
  EXPECT_EQ(ranked[3].package, &pkgCurated2);
}

TEST(LinkDiscoveryTest, publicPackagesAreBoundedByConfiguredLimit) {
  LinkDiscoveryEngine engine;
  engine.setMaxPublicPackages(2);

  const auto authorKeys    = createMutableKeys();
  const std::string docKey = "btpk:" + authorKeys.publicKey.hex() + ":doc";

  std::vector<MutableKeys> keys;
  keys.reserve(5);
  std::vector<LinkPackage> packages;
  packages.reserve(5);

  for (int i = 0; i < 5; i++) {
    keys.push_back(createMutableKeys());
    packages.push_back(publishLinkPackage(
        keys.back(), "salt-" + std::to_string(i), "Pkg " + std::to_string(i),
        i + 1, 1700000000, {}, {}));
  }

  std::vector<const LinkPackage *> candidates;
  candidates.reserve(packages.size());
  for (const auto &pkg : packages) {
    candidates.push_back(&pkg);
  }

  const auto ranked =
      engine.rankPackages(candidates, authorKeys.publicKey, docKey);

  // Bounded to 2 public packages
  ASSERT_EQ(ranked.size(), 2U);
  EXPECT_EQ(ranked[0].package->title, "Pkg 4");
  EXPECT_EQ(ranked[1].package->title, "Pkg 3");
}

TEST(LinkDiscoveryTest, rendezvousTargetIsDeterministic) {
  const std::string scrollKey = "btpk:0123456789abcdef:salt";
  const auto t1               = linkPackageRendezvousTarget(scrollKey);
  const auto t2               = linkPackageRendezvousTarget(scrollKey);
  EXPECT_EQ(t1, t2);
  EXPECT_FALSE(t1.hex().empty());
}

TEST(LinkLayoutTest, linkColourAppliesTierAlpha) {
  const auto authorColor =
      linkColour(LinkType::Comment, ProminenceTier::Author);
  const auto curatedColor =
      linkColour(LinkType::Comment, ProminenceTier::Curated);
  const auto publicColor =
      linkColour(LinkType::Comment, ProminenceTier::Public);

  // RGB components should match
  EXPECT_EQ(authorColor & 0xFFFFFF00U, curatedColor & 0xFFFFFF00U);
  EXPECT_EQ(curatedColor & 0xFFFFFF00U, publicColor & 0xFFFFFF00U);

  // Alpha should decrease with prominence tier
  EXPECT_EQ(authorColor & 0xFFU, 0xE0U);
  EXPECT_EQ(curatedColor & 0xFFU, 0xB0U);
  EXPECT_EQ(publicColor & 0xFFU, 0x60U);
}

} // namespace

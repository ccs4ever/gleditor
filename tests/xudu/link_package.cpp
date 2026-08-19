/**
 * @file link_package.cpp
 * @brief Tests for standalone link packages, author blessings, and discovery ranking.
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
#include <xudu/core/store.hpp>
#include <xudu/core/swarm.hpp>

namespace {

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
using xudu::LinkPackage;
using xudu::linkPackageRendezvousTarget;
using xudu::LinkedPair;
using xudu::LinkType;
using xudu::MicroversionId;
using xudu::MutableKeys;
using xudu::placeLinks;
using xudu::ProminenceTier;
using xudu::publishLinkPackage;
using xudu::Scroll;
using xudu::ScrollSegment;
using xudu::Store;
using xudu::verifyBlessing;
using xudu::verifyLinkPackage;
using xudu::Version;

Scroll makeNamedScroll(const MutableKeys &keys, std::string salt,
                       const std::uint64_t length) {
  Scroll scroll;
  scroll.publisher = keys.publicKey;
  scroll.salt      = std::move(salt);
  ScrollSegment segment;
  segment.at     = 0;
  segment.length = length;
  segment.path   = "permascroll";
  scroll.segments.push_back(segment);
  return scroll;
}

TEST(LinkPackageTest, roundTripEncodingAndVerification) {
  const auto curatorKeys = createMutableKeys();
  const auto scrollKeys  = createMutableKeys();
  const auto scroll      = makeNamedScroll(scrollKeys, "scroll-1", 5000);

  GlobalLink glink;
  glink.type    = LinkType::Comment;
  glink.tier    = ProminenceTier::Curated;
  glink.owner   = "Critical Annotator";
  glink.curator = curatorKeys.publicKey.hex();
  glink.left.push_back(GlobalSpan{"btpk:" + scrollKeys.publicKey.hex() + ":scroll-1", 100, 50});
  glink.right.push_back(GlobalSpan{"btpk:" + scrollKeys.publicKey.hex() + ":scroll-1", 500, 30});

  std::map<std::string, Scroll> scrolls;
  scrolls.emplace("btpk:" + scrollKeys.publicKey.hex() + ":scroll-1", scroll);

  const auto pkg = publishLinkPackage(curatorKeys, "annotations-v1",
                                      "Critical Annotations", 1, 1700000000,
                                      {glink}, scrolls);

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
  auto pkg = publishLinkPackage(curatorKeys, "notes", "Notes", 1, 1700000000,
                                {}, {});
  EXPECT_TRUE(verifyLinkPackage(pkg));

  // Alter payload
  pkg.title = "Forged Title";
  EXPECT_FALSE(verifyLinkPackage(pkg));
}

TEST(LinkPackageTest, adoptionAddsLinksAndScrollsWithoutModifyingDocuments) {
  const auto authorKeys  = createMutableKeys();
  const auto curatorKeys = createMutableKeys();
  const auto scroll      = makeNamedScroll(authorKeys, "essay-scroll", 2000);

  Store store;
  // Local document typing
  const auto doc = store.insert(MicroversionId{}, 0, "The author typed this.");

  GlobalLink glink;
  glink.type = LinkType::Illustration;
  glink.left.push_back(GlobalSpan{"btpk:" + authorKeys.publicKey.hex() + ":essay-scroll", 0, 10});
  glink.right.push_back(GlobalSpan{"btpk:" + authorKeys.publicKey.hex() + ":essay-scroll", 100, 20});

  std::map<std::string, Scroll> scrolls;
  scrolls.emplace("btpk:" + authorKeys.publicKey.hex() + ":essay-scroll", scroll);

  const auto pkg = publishLinkPackage(curatorKeys, "illustrations",
                                      "Illustrations", 1, 1700000000,
                                      {glink}, scrolls);

  const auto result = adoptLinkPackage(store, pkg, ProminenceTier::Curated);
  EXPECT_EQ(result.scrollsAdded, 1U);
  EXPECT_EQ(result.linksAdopted, 1U);

  ASSERT_EQ(store.links().size(), 1U);
  const auto &link = store.links().begin()->second;
  EXPECT_EQ(link.type, LinkType::Illustration);
  EXPECT_EQ(link.tier, ProminenceTier::Curated);
  EXPECT_EQ(link.curator, curatorKeys.publicKey.hex());
}

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
  auto blessing = createBlessing(authorKeys, "doc-1", "pkg-1", "Note", 1700000000);
  EXPECT_TRUE(verifyBlessing(blessing));

  blessing.targetDocument = "doc-tampered";
  EXPECT_FALSE(verifyBlessing(blessing));
}

TEST(LinkDiscoveryTest, rankingAppliesTierAndBlessingWeights) {
  LinkDiscoveryEngine engine;

  const auto authorKeys    = createMutableKeys();
  const auto curator1Keys  = createMutableKeys();
  const auto curator2Keys  = createMutableKeys();
  const auto publicKeys    = createMutableKeys();

  const std::string docKey = "btpk:" + authorKeys.publicKey.hex() + ":essay";

  // Follow curator 1
  engine.followCurator(curator1Keys.publicKey);

  // Author blesses public package
  const auto blessing = createBlessing(authorKeys, docKey,
                                       "btpk:" + publicKeys.publicKey.hex() + ":public-pkg",
                                       "Author approved", 1700000000);
  EXPECT_TRUE(engine.addBlessing(blessing));

  const auto pkgCurated1 = publishLinkPackage(curator1Keys, "c1", "Curated 1", 1, 1700000000, {}, {});
  const auto pkgCurated2 = publishLinkPackage(curator2Keys, "c2", "Unfollowed", 1, 1700000000, {}, {});
  const auto pkgBlessedPublic = publishLinkPackage(publicKeys, "public-pkg", "Blessed Public", 1, 1700000000, {}, {});

  const std::vector<const LinkPackage *> candidates = {
      &pkgCurated2, &pkgBlessedPublic, &pkgCurated1};

  const auto ranked = engine.rankPackages(candidates, authorKeys.publicKey, docKey);

  ASSERT_EQ(ranked.size(), 3U);
  // First should be Curated 1 (followed curator: score >= 1000)
  EXPECT_EQ(ranked[0].tier, ProminenceTier::Curated);
  EXPECT_EQ(ranked[0].package, &pkgCurated1);

  // Second should be Blessed Public (public + 500 author blessing = 501)
  EXPECT_EQ(ranked[1].tier, ProminenceTier::Public);
  EXPECT_EQ(ranked[1].package, &pkgBlessedPublic);
  EXPECT_TRUE(ranked[1].hasAuthorBlessing);

  // Third should be Unfollowed (score = 1)
  EXPECT_EQ(ranked[2].tier, ProminenceTier::Public);
  EXPECT_EQ(ranked[2].package, &pkgCurated2);
}

TEST(LinkDiscoveryTest, publicPackagesAreBoundedByConfiguredLimit) {
  LinkDiscoveryEngine engine;
  engine.setMaxPublicPackages(2);

  const auto authorKeys = createMutableKeys();
  const std::string docKey = "btpk:" + authorKeys.publicKey.hex() + ":doc";

  std::vector<MutableKeys> keys;
  keys.reserve(5);
  std::vector<LinkPackage> packages;
  packages.reserve(5);

  for (int i = 0; i < 5; i++) {
    keys.push_back(createMutableKeys());
    packages.push_back(publishLinkPackage(keys.back(), "salt-" + std::to_string(i),
                                          "Pkg " + std::to_string(i), i + 1,
                                          1700000000, {}, {}));
  }

  std::vector<const LinkPackage *> candidates;
  candidates.reserve(packages.size());
  for (const auto &pkg : packages) {
    candidates.push_back(&pkg);
  }

  const auto ranked = engine.rankPackages(candidates, authorKeys.publicKey, docKey);

  // Bounded to 2 public packages
  ASSERT_EQ(ranked.size(), 2U);
  EXPECT_EQ(ranked[0].package->title, "Pkg 4");
  EXPECT_EQ(ranked[1].package->title, "Pkg 3");
}

TEST(LinkDiscoveryTest, rendezvousTargetIsDeterministic) {
  const std::string scrollKey = "btpk:0123456789abcdef:salt";
  const auto t1 = linkPackageRendezvousTarget(scrollKey);
  const auto t2 = linkPackageRendezvousTarget(scrollKey);
  EXPECT_EQ(t1, t2);
  EXPECT_FALSE(t1.hex().empty());
}

TEST(LinkLayoutTest, linkColourAppliesTierAlpha) {
  const auto authorColor  = linkColour(LinkType::Comment, ProminenceTier::Author);
  const auto curatedColor = linkColour(LinkType::Comment, ProminenceTier::Curated);
  const auto publicColor  = linkColour(LinkType::Comment, ProminenceTier::Public);

  // RGB components should match
  EXPECT_EQ(authorColor & 0xFFFFFF00U, curatedColor & 0xFFFFFF00U);
  EXPECT_EQ(curatedColor & 0xFFFFFF00U, publicColor & 0xFFFFFF00U);

  // Alpha should decrease with prominence tier
  EXPECT_EQ(authorColor & 0xFFU, 0xE0U);
  EXPECT_EQ(curatedColor & 0xFFU, 0xB0U);
  EXPECT_EQ(publicColor & 0xFFU, 0x60U);
}

} // namespace

/**
 * @file coverage_boost.cpp
 * @brief Targeted tests to achieve maximum coverage across xudu core.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

#include <xudu/core/bencode.hpp>
#include <xudu/core/binary_ops.hpp>
#include <xudu/core/blessing.hpp>
#include <xudu/core/config.hpp>
#include <xudu/core/link_discovery.hpp>
#include <xudu/core/link_layout.hpp>
#include <xudu/core/link_package.hpp>
#include <xudu/core/microversion.hpp>
#include <xudu/core/mutable_link.hpp>
#include <xudu/core/ops.hpp>
#include <xudu/core/provenance.hpp>
#include <xudu/core/publication.hpp>
#include <xudu/core/resolver.hpp>
#include <xudu/core/scroll.hpp>
#include <xudu/core/store.hpp>
#include <xudu/core/swarm.hpp>
#include <xudu/core/torrent.hpp>
#include <xudu/core/version.hpp>
#include <xudu/core/windows_quoting.hpp>
#include <xudu/core/yaml.hpp>

namespace {

using xudu::Blessing;
using xudu::Config;
using xudu::createBlessing;
using xudu::createMutableKeys;
using xudu::decodeBlessing;
using xudu::decodeLinkPackage;
using xudu::decodeMutablePointer;
using xudu::decodePublication;
using xudu::DhtTarget;
using xudu::encodeBlessing;
using xudu::encodeLinkPackage;
using xudu::GlobalLink;
using xudu::GlobalSpan;
using xudu::InfoHash;
using xudu::Library;
using xudu::linkColour;
using xudu::LinkDiscoveryEngine;
using xudu::LinkPackage;
using xudu::linkTypeFromName;
using xudu::linkTypeName;
using xudu::LinkType;
using xudu::MicroversionId;
using xudu::MutableKeys;
using xudu::MutableLink;
using xudu::Op;
using xudu::opKindName;
using xudu::OpKind;
using xudu::PrimediaSpan;
using xudu::prominenceTierName;
using xudu::ProminenceTier;
using xudu::Publication;
using xudu::PublicKey;
using xudu::publish;
using xudu::publishLinkPackage;
using xudu::readBinaryOpsSpool;
using xudu::readMicroversionId;
using xudu::readOpsSpool;
using xudu::readOsmicTextOpsSpool;
using xudu::readVarint;
using xudu::Scroll;
using xudu::ScrollSegment;
using xudu::SecretKey;
using xudu::Signature;
using xudu::Store;
using xudu::Version;
using xudu::writeBinaryOpsSpool;
using xudu::writeMicroversionId;
using xudu::writeOsmicTextOpsSpool;
using xudu::writeVarint;

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

TEST(CoverageBoostTest, opsEnumsAndNames) {
  // OpKind names
  EXPECT_STREQ(opKindName(OpKind::Insert), "insert");
  EXPECT_STREQ(opKindName(OpKind::Delete), "delete");
  EXPECT_STREQ(opKindName(OpKind::Rearrange), "rearrange");
  EXPECT_STREQ(opKindName(OpKind::Transclude), "transclude");
  EXPECT_STREQ(opKindName(OpKind::Link), "link");

  // LinkType names & reverse lookup
  EXPECT_STREQ(linkTypeName(LinkType::Comment), "comment");
  EXPECT_STREQ(linkTypeName(LinkType::Illustration), "illustration");
  EXPECT_STREQ(linkTypeName(LinkType::Disagreement), "disagreement");
  EXPECT_STREQ(linkTypeName(LinkType::Authorship), "authorship");
  EXPECT_STREQ(linkTypeName(LinkType::Quotation), "quotation");
  EXPECT_STREQ(linkTypeName(LinkType::Other), "other");

  EXPECT_EQ(linkTypeFromName("comment"), LinkType::Comment);
  EXPECT_EQ(linkTypeFromName("illustration"), LinkType::Illustration);
  EXPECT_EQ(linkTypeFromName("disagreement"), LinkType::Disagreement);
  EXPECT_EQ(linkTypeFromName("authorship"), LinkType::Authorship);
  EXPECT_EQ(linkTypeFromName("quotation"), LinkType::Quotation);
  EXPECT_EQ(linkTypeFromName("other"), LinkType::Other);
  EXPECT_EQ(linkTypeFromName("unknown_type"), LinkType::Other);

  // ProminenceTier names
  EXPECT_STREQ(prominenceTierName(ProminenceTier::Author), "author");
  EXPECT_STREQ(prominenceTierName(ProminenceTier::Curated), "curated");
  EXPECT_STREQ(prominenceTierName(ProminenceTier::Public), "public");
}

TEST(CoverageBoostTest, scrollMethodsAndEdgeCases) {
  Scroll scroll;
  EXPECT_EQ(scroll.describe(), "an empty scroll");

  // Add 0-length segment (early return branch)
  scroll.addSegment(ScrollSegment{0, 0, {}, 0, 0, ""});
  EXPECT_TRUE(scroll.segments.empty());

  // Named scroll describe
  const auto keys = createMutableKeys();
  scroll.publisher = keys.publicKey;
  scroll.salt = "salt1";
  EXPECT_TRUE(scroll.isNamed());
  EXPECT_EQ(scroll.describe(), "urn:btpk:" + keys.publicKey.hex() + "/salt1");

  // Unnamed scroll with segments describe
  Scroll unnamed;
  InfoHash ih;
  ih.bytes.fill(0xAB);
  unnamed.segments.push_back(ScrollSegment{0, 100, ih, 0, 0, "path1"});
  unnamed.segments.push_back(ScrollSegment{100, 200, ih, 100, 1, "path2"});
  EXPECT_TRUE(unnamed.describe().find("and 1 more") != std::string::npos);

  // sameContentAs branches
  Scroll empty1;
  Scroll empty2;
  EXPECT_TRUE(empty1.sameContentAs(empty2));

  Scroll single1 = Scroll::ofTorrentFile(ih, 0, "file.txt", 0, 500);
  Scroll single2 = Scroll::ofTorrentFile(ih, 0, "file.txt", 0, 500);
  EXPECT_TRUE(single1.sameContentAs(single2));
  EXPECT_EQ(single1.segmentAt(0)->length, 500U);
  EXPECT_EQ(single1.segmentAt(600), nullptr);
}

TEST(CoverageBoostTest, binaryOpsCodecErrorHandling) {
  // Truncated varint returns false on EOF
  std::istringstream truncatedVarint("\x80\x80");
  std::uint64_t val = 0;
  EXPECT_FALSE(readVarint(truncatedVarint, val));

  // Invalid binary op kind throws
  std::istringstream badBinary("\x07"); // 7 is invalid kindCode
  std::map<MicroversionId, Op> ops1;
  EXPECT_THROW(readBinaryOpsSpool(badBinary, ops1), std::runtime_error);

  // Empty stream to readBinaryOpsSpool returns empty map
  std::istringstream emptyStream("");
  std::map<MicroversionId, Op> ops2;
  readBinaryOpsSpool(emptyStream, ops2);
  EXPECT_TRUE(ops2.empty());

  // Text ops reader error cases
  std::istringstream malformedText("1 invalid_kind 0 0\n");
  std::map<MicroversionId, Op> ops3;
  EXPECT_THROW(readOsmicTextOpsSpool(malformedText, ops3), std::runtime_error);

  std::istringstream shortText("1 insert 0\n");
  std::map<MicroversionId, Op> ops4;
  EXPECT_THROW(readOsmicTextOpsSpool(shortText, ops4), std::runtime_error);

  // Rearrange Op in both binary and text formats
  Op rearrangeOp;
  rearrangeOp.kind   = OpKind::Rearrange;
  rearrangeOp.parent = MicroversionId::parse("1");
  rearrangeOp.at     = 5;
  rearrangeOp.length = 10;
  rearrangeOp.to     = 20;

  std::map<MicroversionId, Op> opsMap = {{MicroversionId::parse("2"), rearrangeOp}};

  // Binary round-trip
  std::stringstream binOut;
  writeBinaryOpsSpool(binOut, opsMap);
  std::map<MicroversionId, Op> binRead;
  readOpsSpool(binOut, binRead);
  ASSERT_EQ(binRead.size(), 1U);
  EXPECT_EQ(binRead.begin()->second.kind, OpKind::Rearrange);
  EXPECT_EQ(binRead.begin()->second.at, 5U);
  EXPECT_EQ(binRead.begin()->second.length, 10U);
  EXPECT_EQ(binRead.begin()->second.to, 20U);

  // Text round-trip
  std::stringstream textOut;
  writeOsmicTextOpsSpool(textOut, opsMap);
  std::map<MicroversionId, Op> textRead;
  readOpsSpool(textOut, textRead);
  ASSERT_EQ(textRead.size(), 1U);
  EXPECT_EQ(textRead.begin()->second.kind, OpKind::Rearrange);
  EXPECT_EQ(textRead.begin()->second.at, 5U);
  EXPECT_EQ(textRead.begin()->second.length, 10U);
  EXPECT_EQ(textRead.begin()->second.to, 20U);
}

TEST(CoverageBoostTest, blessingAndLinkPackageDescribers) {
  const auto keys = createMutableKeys();
  const auto blessing = createBlessing(keys, "doc1", "pkg1", "My Note", 1700000000);
  EXPECT_FALSE(blessing.describe().empty());

  const auto pkg = publishLinkPackage(keys, "salt1", "Pkg 1", 1, 1700000000, {}, {});
  EXPECT_FALSE(pkg.describe().empty());
  EXPECT_FALSE(pkg.packageKey().empty());
  EXPECT_FALSE(pkg.uri().empty());
  EXPECT_FALSE(pkg.name().hex().empty());
}

TEST(CoverageBoostTest, linkDiscoveryEngineExtraMethods) {
  LinkDiscoveryEngine engine;
  const auto curator = createMutableKeys().publicKey;
  EXPECT_FALSE(engine.isFollowed(curator));
  engine.followCurator(curator);
  EXPECT_TRUE(engine.isFollowed(curator));
  EXPECT_EQ(engine.curators().size(), 1U);
  engine.unfollowCurator(curator);
  EXPECT_FALSE(engine.isFollowed(curator));

  EXPECT_EQ(engine.getMaxPublicPackages(), 10U);
  engine.setMaxPublicPackages(25);
  EXPECT_EQ(engine.getMaxPublicPackages(), 25U);
}

TEST(CoverageBoostTest, publicationLibraryIndex) {
  Library library;
  EXPECT_EQ(library.size(), 0U);
  EXPECT_TRUE(library.all().empty());

  const auto authorKeys = createMutableKeys();
  const auto scroll = makeNamedScroll(authorKeys.publicKey, "scroll1", 1000);

  Store store;
  const auto ver = store.transcludeExternal(MicroversionId{}, 0, scroll, 0, 100);
  const auto pub = publish(store, ver, authorKeys, "salt1", "Title 1", 1, 1700000000);

  EXPECT_TRUE(library.add(pub));
  EXPECT_EQ(library.size(), 1U);
  EXPECT_EQ(library.all().size(), 1U);
  EXPECT_NE(library.find(pub.name()), nullptr);

  // Duplicate add is rejected
  EXPECT_FALSE(library.add(pub));

  // Query showing spans
  const auto sightings = library.showing(GlobalSpan{"btpk:" + authorKeys.publicKey.hex() + ":scroll1", 10, 20});
  ASSERT_EQ(sightings.size(), 1U);
  EXPECT_EQ(sightings[0].start, 10U);
  EXPECT_EQ(sightings[0].end, 30U);
}

TEST(CoverageBoostTest, mutableLinkAndHexParsers) {
  // isZero
  PublicKey pkZero{};
  EXPECT_TRUE(pkZero.isZero());

  // fromHex errors
  EXPECT_THROW(static_cast<void>(PublicKey::fromHex("short")), std::runtime_error);
  EXPECT_THROW(static_cast<void>(SecretKey::fromHex("short")), std::runtime_error);
  EXPECT_THROW(static_cast<void>(Signature::fromHex("short")), std::runtime_error);

  // MutableLink::parse with non-magnet
  EXPECT_THROW(static_cast<void>(MutableLink::parse("http://example.com")), std::runtime_error);

  // MutableLink with display name & trackers
  const auto keys = createMutableKeys();
  MutableLink link;
  link.key = keys.publicKey;
  link.salt = "salt1";
  link.displayName = "My Doc";
  link.trackers = {"http://tracker.example.com/announce"};
  const auto uri = link.uri();
  EXPECT_TRUE(uri.find("dn=My+Doc") != std::string::npos || uri.find("dn=My%20Doc") != std::string::npos || uri.find("dn=My Doc") != std::string::npos);
  EXPECT_TRUE(MutableLink::looksLikeMutableLink(uri));

  const auto parsed = MutableLink::parse(uri);
  EXPECT_EQ(parsed.key, keys.publicKey);
  EXPECT_EQ(parsed.salt, "salt1");
}

TEST(CoverageBoostTest, storeHypertimeRearrange) {
  Store store;
  auto v1 = store.insert(MicroversionId{}, 0, "ABCDEFGHIJ");
  // Rearrange: move 3 bytes at 0 ("ABC") to position 7
  auto v2 = store.rearrange(v1, 0, 3, 7);
  const auto text = store.textOf(v2);
  EXPECT_EQ(text, "DEFGABCHIJ");

  // Export OSMIC text
  const auto osmicText = store.exportOsmicText();
  EXPECT_TRUE(osmicText.find("rearrange") != std::string::npos);
}

} // namespace

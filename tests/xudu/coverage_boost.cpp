/**
 * @file coverage_boost.cpp
 * @brief Targeted tests to achieve maximum coverage across xudu core.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

#include "common/xanadu/bencode.hpp"
#include "common/xanadu/binary_ops.hpp"
#include "common/xanadu/blessing.hpp"
#include "common/xanadu/config.hpp"
#include "common/xanadu/link_discovery.hpp"
#include "common/xanadu/link_layout.hpp"
#include "common/xanadu/link_package.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/mutable_link.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/provenance.hpp"
#include "common/xanadu/publication.hpp"
#include "common/xanadu/resolver.hpp"
#include "common/xanadu/scroll.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/swarm.hpp"
#include "common/xanadu/torrent.hpp"
#include "common/xanadu/version.hpp"
#include "common/xanadu/windows_quoting.hpp"
#include "common/xanadu/yaml.hpp"

namespace {

using xanadu::Blessing;
using xanadu::Config;
using xanadu::createBlessing;
using xanadu::createMutableKeys;
using xanadu::decodeBlessing;
using xanadu::decodeLinkPackage;
using xanadu::decodeMutablePointer;
using xanadu::decodePublication;
using xanadu::DhtTarget;
using xanadu::encodeBlessing;
using xanadu::encodeLinkPackage;
using xanadu::GlobalLink;
using xanadu::GlobalSpan;
using xanadu::InfoHash;
using xanadu::Library;
using xanadu::linkColour;
using xanadu::LinkDiscoveryEngine;
using xanadu::LinkPackage;
using xanadu::LinkType;
using xanadu::linkTypeFromName;
using xanadu::linkTypeName;
using xanadu::MicroversionId;
using xanadu::MutableKeys;
using xanadu::MutableLink;
using xanadu::Op;
using xanadu::OpKind;
using xanadu::opKindName;
using xanadu::PrimediaSpan;
using xanadu::ProminenceTier;
using xanadu::prominenceTierName;
using xanadu::Publication;
using xanadu::PublicKey;
using xanadu::publish;
using xanadu::publishLinkPackage;
using xanadu::readBinaryOpsSpool;
using xanadu::readMicroversionId;
using xanadu::readOpsSpool;
using xanadu::readOsmicTextOpsSpool;
using xanadu::readVarint;
using xanadu::Scroll;
using xanadu::ScrollSegment;
using xanadu::SecretKey;
using xanadu::Signature;
using xanadu::Store;
using xanadu::Version;
using xanadu::writeBinaryOpsSpool;
using xanadu::writeMicroversionId;
using xanadu::writeOsmicTextOpsSpool;
using xanadu::writeVarint;

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
  const auto keys  = createMutableKeys();
  scroll.publisher = keys.publicKey;
  scroll.salt      = "salt1";
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
  std::vector<xanadu::OpRecord> ops1;
  EXPECT_THROW(readBinaryOpsSpool(badBinary, ops1), std::runtime_error);

  // Empty stream to readBinaryOpsSpool returns empty map
  std::istringstream emptyStream("");
  std::vector<xanadu::OpRecord> ops2;
  readBinaryOpsSpool(emptyStream, ops2);
  EXPECT_TRUE(ops2.empty());

  // Text ops reader error cases
  std::istringstream malformedText("1 invalid_kind 0 0\n");
  std::vector<xanadu::OpRecord> ops3;
  EXPECT_THROW(readOsmicTextOpsSpool(malformedText, ops3), std::runtime_error);

  std::istringstream shortText("1 insert 0\n");
  std::vector<xanadu::OpRecord> ops4;
  EXPECT_THROW(readOsmicTextOpsSpool(shortText, ops4), std::runtime_error);

  // Rearrange Op in both binary and text formats
  Op rearrangeOp;
  rearrangeOp.kind   = OpKind::Rearrange;
  rearrangeOp.parent = MicroversionId::parse("1");
  rearrangeOp.at     = 5;
  rearrangeOp.length = 10;
  rearrangeOp.to     = 20;

  const std::vector<xanadu::OpRecord> opsMap = {
      xanadu::OpRecord{MicroversionId::parse("2"), rearrangeOp}};

  // Binary round-trip
  std::stringstream binOut;
  writeBinaryOpsSpool(binOut, opsMap);
  std::vector<xanadu::OpRecord> binRead;
  readOpsSpool(binOut, binRead);
  ASSERT_EQ(binRead.size(), 1U);
  EXPECT_EQ(binRead.front().op.kind, OpKind::Rearrange);
  EXPECT_EQ(binRead.front().op.at, 5U);
  EXPECT_EQ(binRead.front().op.length, 10U);
  EXPECT_EQ(binRead.front().op.to, 20U);

  // Text round-trip
  std::stringstream textOut;
  writeOsmicTextOpsSpool(textOut, opsMap);
  std::vector<xanadu::OpRecord> textRead;
  readOpsSpool(textOut, textRead);
  ASSERT_EQ(textRead.size(), 1U);
  EXPECT_EQ(textRead.front().op.kind, OpKind::Rearrange);
  EXPECT_EQ(textRead.front().op.at, 5U);
  EXPECT_EQ(textRead.front().op.length, 10U);
  EXPECT_EQ(textRead.front().op.to, 20U);
}

TEST(CoverageBoostTest, blessingAndLinkPackageDescribers) {
  const auto keys = createMutableKeys();
  const auto blessing =
      createBlessing(keys, "doc1", "pkg1", "My Note", 1700000000);
  EXPECT_FALSE(blessing.describe().empty());

  const auto pkg =
      publishLinkPackage(keys, "salt1", "Pkg 1", 1, 1700000000, {}, {});
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
  const auto ver =
      store.transcludeExternal(MicroversionId{}, 0, scroll, 0, 100);
  const auto pub =
      publish(store, ver, authorKeys, "salt1", "Title 1", 1, 1700000000);

  EXPECT_TRUE(library.add(pub));
  EXPECT_EQ(library.size(), 1U);
  EXPECT_EQ(library.all().size(), 1U);
  EXPECT_NE(library.find(pub.name()), nullptr);

  // Duplicate add is rejected
  EXPECT_FALSE(library.add(pub));

  // Query showing spans
  const auto sightings = library.showing(
      GlobalSpan{"btpk:" + authorKeys.publicKey.hex() + ":scroll1", 10, 20});
  ASSERT_EQ(sightings.size(), 1U);
  EXPECT_EQ(sightings[0].start, 10U);
  EXPECT_EQ(sightings[0].end, 30U);
}

TEST(CoverageBoostTest, mutableLinkAndHexParsers) {
  // isZero
  PublicKey pkZero{};
  EXPECT_TRUE(pkZero.isZero());

  // fromHex errors
  EXPECT_THROW(static_cast<void>(PublicKey::fromHex("short")),
               std::runtime_error);
  EXPECT_THROW(static_cast<void>(SecretKey::fromHex("short")),
               std::runtime_error);
  EXPECT_THROW(static_cast<void>(Signature::fromHex("short")),
               std::runtime_error);

  // MutableLink::parse with non-magnet
  EXPECT_THROW(static_cast<void>(MutableLink::parse("http://example.com")),
               std::runtime_error);

  // MutableLink with display name & trackers
  const auto keys = createMutableKeys();
  MutableLink link;
  link.key         = keys.publicKey;
  link.salt        = "salt1";
  link.displayName = "My Doc";
  link.trackers    = {"http://tracker.example.com/announce"};
  const auto uri   = link.uri();
  EXPECT_TRUE(uri.find("dn=My+Doc") != std::string::npos ||
              uri.find("dn=My%20Doc") != std::string::npos ||
              uri.find("dn=My Doc") != std::string::npos);
  EXPECT_TRUE(MutableLink::looksLikeMutableLink(uri));

  const auto parsed = MutableLink::parse(uri);
  EXPECT_EQ(parsed.key, keys.publicKey);
  EXPECT_EQ(parsed.salt, "salt1");
}

TEST(CoverageBoostTest, storeHypertimeRearrange) {
  Store store;
  auto v1 = store.insert(MicroversionId{}, 0, "ABCDEFGHIJ");
  // Rearrange: move 3 bytes at 0 ("ABC") to position 7
  auto v2         = store.rearrange(v1, 0, 3, 7);
  const auto text = store.textOf(v2);
  EXPECT_EQ(text, "DEFGABCHIJ");

  // Export OSMIC text
  const auto osmicText = store.exportOsmicText();
  EXPECT_TRUE(osmicText.find("rearrange") != std::string::npos);
}

} // namespace

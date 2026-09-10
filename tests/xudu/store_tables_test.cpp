/**
 * @file store_tables_test.cpp
 * @brief The scroll registry and the link table, in the one container that
 *        replaced two plaintext files.
 *
 * The conversion's whole claim is that nothing was lost, so most of what is
 * here is about fields: the ones the plaintext table carried, and the four it
 * silently dropped -- a link's tier and curator, and a segment's withheld kind
 * and hole record. See design R11 and migration step 11.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <xudu/core/store.hpp>
#include <xudu/core/store_tables.hpp>
#include <xudu/core/user_permascroll.hpp>

namespace {

namespace fs = std::filesystem;
using xudu::HoleReason;
using xudu::InfoHash;
using xudu::Link;
using xudu::LinkType;
using xudu::MicroversionId;
using xudu::PrimediaSpan;
using xudu::ProminenceTier;
using xudu::PublishedHoleRecord;
using xudu::Scroll;
using xudu::ScrollSegment;
using xudu::Store;
using xudu::StoreTables;

fs::path scratch(const std::string &name) {
  const auto dir = fs::temp_directory_path() / ("xudu_tables_" + name);
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

/// Everything a table can hold, including the awkward parts.
StoreTables everything() {
  StoreTables tables;

  Scroll published;
  published.publisher.bytes.fill(0x5a);
  published.salt            = "essay";
  published.defaultMimeType = "text/plain;charset=utf-8";
  ScrollSegment plain;
  plain.at           = 0;
  plain.length       = 100;
  plain.torrent      = InfoHash::fromHex(std::string(40, 'b'));
  plain.streamOffset = 27;
  plain.fileIndex    = 2;
  plain.path         = "chapter.txt";
  plain.mimeType     = "text/plain;charset=utf-8";
  published.segments.push_back(plain);

  // A withheld stretch with a price on it: the two fields the plaintext table
  // had no column for, and a nested descriptor inside the second.
  ScrollSegment locked;
  locked.at       = 100;
  locked.length   = 50;
  locked.torrent  = InfoHash::fromHex(std::string(40, 'c'));
  locked.mimeType = "image/png";
  locked.kind     = xudu::SegmentKind::Withheld;
  PublishedHoleRecord hole;
  hole.at     = 100;
  hole.length = 50;
  hole.reason = HoleReason::TranscopyrightLock;
  hole.contentCommitment.fill(0x11);
  xudu::TranscopyrightDescriptor tc;
  tc.priceAtomicUnits = 4200;
  tc.flatFee          = true;
  tc.currencySymbol   = "XU";
  tc.licenseMemo      = "Nelson-Transcopyright-v1";
  tc.keyId.fill(0x22);
  tc.nonce.fill(0x33);
  hole.transcopyright = tc;
  locked.holeRecord   = hole;
  published.segments.push_back(locked);
  tables.scrolls.push_back(published);

  // A scroll with no publisher at all, which a publication's table cannot
  // hold and a registry must: this is what content that exists only as one
  // fixed torrent is named by.
  tables.scrolls.push_back(Scroll::ofTorrentFile(
      InfoHash::fromHex(std::string(40, 'd')), 0, "fox.txt", 0, 61));

  ScrollSegment local;
  local.at       = 4070;
  local.length   = 228;
  local.mimeType = "image/png";
  tables.localSegments.push_back(local);

  Link curated;
  curated.id      = 7;
  curated.type    = LinkType::Quotation;
  curated.tier    = ProminenceTier::Curated;
  curated.owner   = "Theodor_Holm_Nelson";
  curated.curator = "btpk:" + std::string(64, 'e');
  curated.left    = {PrimediaSpan{0, 10, 20}};
  curated.right   = {PrimediaSpan{1, 30, 40}, PrimediaSpan{2, 0, 5}};
  tables.links.emplace(curated.id, curated);

  return tables;
}

TEST(StoreTablesTest, everyFieldSurvivesTheRoundTrip) {
  const auto dir  = scratch("roundtrip");
  const auto path = dir / "store.tables";
  const auto sent = everything();
  xudu::writeStoreTables(path, sent);
  const auto back = xudu::readStoreTables(path);

  ASSERT_EQ(back.scrolls.size(), sent.scrolls.size());
  EXPECT_EQ(back.scrolls[0].publisher.hex(), sent.scrolls[0].publisher.hex());
  EXPECT_EQ(back.scrolls[0].salt, "essay");
  EXPECT_EQ(back.scrolls[0].defaultMimeType, "text/plain;charset=utf-8");
  ASSERT_EQ(back.scrolls[0].segments.size(), 2U);
  EXPECT_EQ(back.scrolls[0].segments, sent.scrolls[0].segments)
      << "a segment came back as something other than what went in";

  // The withheld stretch in particular: kind, hole, and the descriptor inside
  // it. The plaintext table had no column for any of this, so a store that
  // had adopted a document with a price on part of it came back with that
  // part looking like ordinary content.
  const auto &locked = back.scrolls[0].segments[1];
  EXPECT_TRUE(locked.isWithheld());
  EXPECT_TRUE(locked.isLocked());
  ASSERT_TRUE(locked.holeRecord.has_value());
  EXPECT_EQ(locked.holeRecord->reason, HoleReason::TranscopyrightLock);
  ASSERT_TRUE(locked.holeRecord->transcopyright.has_value());
  EXPECT_EQ(locked.holeRecord->transcopyright->priceAtomicUnits, 4200U);
  EXPECT_TRUE(locked.holeRecord->transcopyright->flatFee);

  // The unnamed scroll, which has no publisher key to write down.
  EXPECT_FALSE(back.scrolls[1].isNamed());
  ASSERT_EQ(back.scrolls[1].segments.size(), 1U);
  EXPECT_EQ(back.scrolls[1].segments.front().path, "fox.txt");

  ASSERT_EQ(back.localSegments.size(), 1U);
  EXPECT_EQ(back.localSegments.front().mimeType, "image/png")
      << "a local segment's MIME type is the one thing the shared segment "
         "encoding has no key for, so the registry adds it";

  ASSERT_EQ(back.links.size(), 1U);
  const auto &link = back.links.at(7);
  EXPECT_EQ(link.type, LinkType::Quotation);
  EXPECT_EQ(link.tier, ProminenceTier::Curated) << "tier was dropped before";
  EXPECT_EQ(link.owner, "Theodor_Holm_Nelson");
  EXPECT_EQ(link.curator, "btpk:" + std::string(64, 'e'))
      << "curator was dropped before";
  EXPECT_EQ(link.left, (std::vector<PrimediaSpan>{PrimediaSpan{0, 10, 20}}));
  EXPECT_EQ(link.right, (std::vector<PrimediaSpan>{PrimediaSpan{1, 30, 40},
                                                   PrimediaSpan{2, 0, 5}}));
}

TEST(StoreTablesTest, aFileThatIsNotOneIsRefusedAndSaysWhy) {
  const auto dir = scratch("refused");

  // What the plaintext table looked like, offered where the container goes.
  {
    std::ofstream out(dir / "store.tables");
    out << "scroll 1 - - text/plain\n";
  }
  try {
    static_cast<void>(xudu::readStoreTables(dir / "store.tables"));
    FAIL() << "a file with no signature must not be read as side tables";
  } catch (const xudu::StoreTablesUnreadable &e) {
    EXPECT_THAT(std::string{e.what()}, testing::HasSubstr("signature"));
    EXPECT_THAT(std::string{e.what()}, testing::HasSubstr("XUDUTBL"));
  }

  // A version this build does not know, refused by number the way an ops
  // segment and a binary ops spool both are.
  {
    xudu::writeStoreTables(dir / "future.tables", StoreTables{});
    std::fstream out(dir / "future.tables",
                     std::ios::binary | std::ios::in | std::ios::out);
    out.seekp(static_cast<std::streamoff>(xudu::storeTablesSignature.size()));
    const std::uint32_t later = xudu::storeTablesFormatVersion + 1;
    out.write(reinterpret_cast<const char *>(&later), sizeof(later));
  }
  try {
    static_cast<void>(xudu::readStoreTables(dir / "future.tables"));
    FAIL() << "a version this build does not know must not be guessed at";
  } catch (const xudu::StoreTablesUnreadable &e) {
    EXPECT_THAT(
        std::string{e.what()},
        testing::HasSubstr("version " +
                           std::to_string(xudu::storeTablesFormatVersion + 1)));
    EXPECT_THAT(
        std::string{e.what()},
        testing::HasSubstr("reads version " +
                           std::to_string(xudu::storeTablesFormatVersion)));
  }

  // A header with rubbish behind it, which is a corrupt file rather than a
  // foreign one and still must not be read as an empty store.
  {
    std::ofstream out(dir / "torn.tables", std::ios::binary);
    out.write(reinterpret_cast<const char *>(xudu::storeTablesSignature.data()),
              static_cast<std::streamsize>(xudu::storeTablesSignature.size()));
    const auto version = xudu::storeTablesFormatVersion;
    out.write(reinterpret_cast<const char *>(&version), sizeof(version));
    out << "not bencode";
  }
  EXPECT_THROW(static_cast<void>(xudu::readStoreTables(dir / "torn.tables")),
               xudu::StoreTablesUnreadable);
}

TEST(StoreTablesTest, aStoresLinksAndScrollsSurviveBeingSavedAndReopened) {
  // The same claim from the outside: through Store, which is where the two
  // plaintext parsers used to be.
  const auto dir = scratch("store");
  Link link;
  link.type    = LinkType::Comment;
  link.tier    = ProminenceTier::Public;
  link.owner   = "me";
  link.curator = "somebody else";

  // One permascroll across both, because a store holds no primedia of its own
  // and its local spans are addresses in the author's.
  const auto perma = std::make_shared<xudu::UserPermascroll>();
  MicroversionId at;
  {
    Store store(perma);
    at         = store.insert(MicroversionId{}, 0, "hello world");
    link.left  = store.rebuild(at).spansFor(0, 5);
    link.right = store.rebuild(at).spansFor(6, 5);
    at         = store.addLink(at, link);
    store.save(dir.string());
  }

  Store reopened(perma);
  reopened.load(dir.string());
  EXPECT_EQ(reopened.textOf(at), "hello world");
  const auto &links = reopened.links();
  ASSERT_EQ(links.size(), 1U);
  const auto &back = links.begin()->second;
  EXPECT_EQ(back.owner, "me");
  EXPECT_EQ(back.curator, "somebody else");
  EXPECT_EQ(back.tier, ProminenceTier::Public);
  EXPECT_EQ(back.left, link.left);
  EXPECT_EQ(back.right, link.right);
}

} // namespace

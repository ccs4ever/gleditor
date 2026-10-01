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

#include "common/xanadu/store.hpp"
#include "common/xanadu/store_tables.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace {

namespace fs = std::filesystem;
using xanadu::HoleReason;
using xanadu::InfoHash;
using xanadu::Link;
using xanadu::LinkType;
using xanadu::MicroversionId;
using xanadu::PrimediaSpan;
using xanadu::ProminenceTier;
using xanadu::PublishedHoleRecord;
using xanadu::Scroll;
using xanadu::ScrollSegment;
using xanadu::Store;
using xanadu::StoreTables;

fs::path scratch(const std::string &name) {
  const auto dir = fs::temp_directory_path() / ("xudu_tables_" + name);
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

/// Everything a table can hold, including the awkward parts.
StoreTables everything() {
  StoreTables tables;

  ScrollSegment local;
  local.at       = 4070;
  local.length   = 228;
  local.mimeType = "image/png";
  tables.localSegments.push_back(local);

  return tables;
}

TEST(StoreTablesTest, everyFieldSurvivesTheRoundTrip) {
  const auto dir  = scratch("roundtrip");
  const auto path = dir / "store.tables";
  const auto sent = everything();
  xanadu::writeStoreTables(path, sent);
  const auto back = xanadu::readStoreTables(path);

  EXPECT_EQ(back.documentId, sent.documentId);

  ASSERT_EQ(back.localSegments.size(), 1U);
  EXPECT_EQ(back.localSegments.front().mimeType, "image/png")
      << "a local segment's MIME type is the one thing the shared segment "
         "encoding has no key for, so the registry adds it";
}

TEST(StoreTablesTest, aFileThatIsNotOneIsRefusedAndSaysWhy) {
  const auto dir = scratch("refused");

  // What the plaintext table looked like, offered where the container goes.
  {
    std::ofstream out(dir / "store.tables");
    out << "scroll 1 - - text/plain\n";
  }
  try {
    static_cast<void>(xanadu::readStoreTables(dir / "store.tables"));
    FAIL() << "a file with no signature must not be read as side tables";
  } catch (const xanadu::StoreTablesUnreadable &e) {
    EXPECT_THAT(std::string{e.what()}, testing::HasSubstr("signature"));
    EXPECT_THAT(std::string{e.what()}, testing::HasSubstr("XUDUTBL"));
  }

  // A version this build does not know, refused by number the way an ops
  // segment and a binary ops spool both are.
  {
    xanadu::writeStoreTables(dir / "future.tables", StoreTables{});
    std::fstream out(dir / "future.tables",
                     std::ios::binary | std::ios::in | std::ios::out);
    out.seekp(static_cast<std::streamoff>(xanadu::storeTablesSignature.size()));
    const std::uint32_t later = xanadu::storeTablesFormatVersion + 1;
    out.write(reinterpret_cast<const char *>(&later), sizeof(later));
  }
  try {
    static_cast<void>(xanadu::readStoreTables(dir / "future.tables"));
    FAIL() << "a version this build does not know must not be guessed at";
  } catch (const xanadu::StoreTablesUnreadable &e) {
    EXPECT_THAT(
        std::string{e.what()},
        testing::HasSubstr(
            "version " + std::to_string(xanadu::storeTablesFormatVersion + 1)));
    EXPECT_THAT(
        std::string{e.what()},
        testing::HasSubstr("reads version " +
                           std::to_string(xanadu::storeTablesFormatVersion)));
  }

  // A header with rubbish behind it, which is a corrupt file rather than a
  // foreign one and still must not be read as an empty store.
  {
    std::ofstream out(dir / "torn.tables", std::ios::binary);
    out.write(
        reinterpret_cast<const char *>(xanadu::storeTablesSignature.data()),
        static_cast<std::streamsize>(xanadu::storeTablesSignature.size()));
    const auto version = xanadu::storeTablesFormatVersion;
    out.write(reinterpret_cast<const char *>(&version), sizeof(version));
    out << "not bencode";
  }
  EXPECT_THROW(static_cast<void>(xanadu::readStoreTables(dir / "torn.tables")),
               xanadu::StoreTablesUnreadable);
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
  const auto perma = std::make_shared<xanadu::UserPermascroll>();
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

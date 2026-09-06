#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "xudu/core/resolver.hpp"
#include "xudu/core/scroll.hpp"
#include "xudu/core/store.hpp"
#include "xudu/core/torrent.hpp"
#include "xudu/core/transcopyright_crypto.hpp"
#include "xudu/core/transcopyright_logic.hpp"
#include "xudu/core/user_permascroll.hpp"

namespace {

using namespace xudu;

TEST(TranscopyrightUiTest, mapsHoleReasonsToLabelsAndColors) {
  EXPECT_EQ(TranscopyrightLogic::reasonLabel(HoleReason::Withheld), "WITHHELD");
  EXPECT_EQ(TranscopyrightLogic::reasonLabel(HoleReason::Revoked), "REVOKED");
  EXPECT_EQ(TranscopyrightLogic::reasonLabel(HoleReason::Takedown), "TAKEDOWN");
  EXPECT_EQ(TranscopyrightLogic::reasonLabel(HoleReason::TranscopyrightLock),
            "PAYWALL");
  EXPECT_EQ(TranscopyrightLogic::reasonLabel(HoleReason::Unsealed), "UNSEALED");

  EXPECT_EQ(TranscopyrightLogic::colorForReason(HoleReason::Withheld),
            kWithheldColour);
  EXPECT_EQ(TranscopyrightLogic::colorForReason(HoleReason::Revoked),
            kRevokedColour);
  EXPECT_EQ(TranscopyrightLogic::colorForReason(HoleReason::Takedown),
            kTakedownColour);
  EXPECT_EQ(TranscopyrightLogic::colorForReason(HoleReason::TranscopyrightLock),
            kTranscopyrightColour);
  EXPECT_EQ(TranscopyrightLogic::colorForReason(HoleReason::Unsealed),
            kUnsealedColour);
}

TEST(TranscopyrightUiTest, formatsPricingAndCalculatesCosts) {
  EXPECT_EQ(TranscopyrightLogic::formatCost(250, "nano-XU"), "250 nano-XU");
  EXPECT_EQ(TranscopyrightLogic::formatCost(1000000000ULL, "XU"), "1 XU");
  EXPECT_EQ(TranscopyrightLogic::formatCost(1500000000ULL, "XU"), "1.5 XU");

  TranscopyrightDescriptor flatDesc{
      .priceAtomicUnits = 500,
      .flatFee          = true,
      .currencySymbol   = "SAT",
  };
  EXPECT_EQ(flatDesc.computeCost(1), 500U);
  EXPECT_EQ(flatDesc.computeCost(100), 500U);

  TranscopyrightDescriptor perByteDesc{
      .priceAtomicUnits = 5,
      .flatFee          = false,
      .currencySymbol   = "nano-XU",
  };
  EXPECT_EQ(perByteDesc.computeCost(1), 5U);
  EXPECT_EQ(perByteDesc.computeCost(100), 500U);
}

TEST(TranscopyrightUiTest, computesWireframeProgressMetrics) {
  WireframeProgress wp{
      .docIndex      = 0,
      .title         = "Docuverse Spec",
      .infoHash      = "a1b2c3d4",
      .piecesFetched = 12,
      .totalPieces   = 16,
      .shimmerPhase  = 0.0F,
  };

  EXPECT_FLOAT_EQ(wp.fraction(), 0.75F);
  EXPECT_EQ(wp.percent(), 75U);
  EXPECT_FALSE(wp.isComplete());
  EXPECT_EQ(wp.statusLine(), "[12/16 pieces | 75% materializing]");

  wp.piecesFetched = 16;
  EXPECT_TRUE(wp.isComplete());
  EXPECT_EQ(wp.statusLine(), "[16/16 pieces | 100% materializing]");
}

TEST(TranscopyrightUiTest, derivesDeterministicTestCekAndKeyId) {
  const auto keyId1 = TranscopyrightLogic::testKeyId("sample-seed-a");
  const auto keyId2 = TranscopyrightLogic::testKeyId("sample-seed-a");
  const auto keyId3 = TranscopyrightLogic::testKeyId("sample-seed-b");

  EXPECT_EQ(keyId1, keyId2);
  EXPECT_NE(keyId1, keyId3);

  const auto cek1 = TranscopyrightLogic::deriveDeterministicTestCek(keyId1);
  const auto cek2 = TranscopyrightLogic::deriveDeterministicTestCek(keyId1);
  const auto cek3 = TranscopyrightLogic::deriveDeterministicTestCek(keyId3);

  EXPECT_EQ(cek1, cek2);
  EXPECT_NE(cek1, cek3);

  // AEAD round-trip with derived CEK
  const std::string secret = "Nelsonian sovereign transcopyright content";
  const auto nonce         = crypto::nonceForKeyId(keyId1);
  const auto cipher        = crypto::encryptAead(secret, cek1, nonce, {});

  const auto decrypted = crypto::decryptAead(cipher, cek1, nonce, {});
  ASSERT_TRUE(decrypted.has_value());
  EXPECT_EQ(*decrypted, secret);
}

TEST(TranscopyrightUiTest, inspectsHolesInStoreAndVersion) {
  auto scroll = std::make_shared<UserPermascroll>();
  Store store(scroll);

  Scroll externalScroll;
  // Seg 0: Plain text
  ScrollSegment seg0;
  seg0.at           = 0;
  seg0.length       = 30;
  seg0.streamOffset = 0;
  seg0.kind         = SegmentKind::Plain;
  externalScroll.segments.push_back(seg0);

  // Seg 1: Withheld hole
  ScrollSegment seg1;
  seg1.at           = 30;
  seg1.length       = 20;
  seg1.streamOffset = 30;
  seg1.kind         = SegmentKind::Withheld;
  PublishedHoleRecord hole1;
  hole1.at        = 30;
  hole1.length    = 20;
  hole1.reason    = HoleReason::Withheld;
  seg1.holeRecord = hole1;
  externalScroll.segments.push_back(seg1);

  // Seg 2: TranscopyrightLocked hole
  ScrollSegment seg2;
  seg2.at           = 50;
  seg2.length       = 40;
  seg2.streamOffset = 50;
  seg2.kind         = SegmentKind::Withheld;
  PublishedHoleRecord hole2;
  hole2.at     = 50;
  hole2.length = 40;
  hole2.reason = HoleReason::TranscopyrightLock;
  TranscopyrightDescriptor tc;
  tc.priceAtomicUnits = 250;
  tc.flatFee          = true;
  tc.currencySymbol   = "nano-XU";
  tc.keyId            = TranscopyrightLogic::testKeyId("test-seed");
  hole2.transcopyright = tc;
  seg2.holeRecord     = hole2;
  externalScroll.segments.push_back(seg2);

  const auto v = store.transcludeExternal(MicroversionId{}, 0, externalScroll,
                                          0, externalScroll.length());
  const auto ver = store.rebuild(v);

  const auto holes = TranscopyrightLogic::inspectHoles(store, ver, 0, 0);
  ASSERT_EQ(holes.size(), 2U);

  // First hole is Withheld
  EXPECT_EQ(holes[0].charOffset, 30U);
  EXPECT_EQ(holes[0].length, 20U);
  EXPECT_EQ(holes[0].reasonLabel, "WITHHELD");
  EXPECT_TRUE(holes[0].isWithheld());
  EXPECT_FALSE(holes[0].isLocked());

  // Second hole is TranscopyrightLocked
  EXPECT_EQ(holes[1].charOffset, 50U);
  EXPECT_EQ(holes[1].length, 40U);
  EXPECT_EQ(holes[1].reasonLabel, "PAYWALL");
  EXPECT_TRUE(holes[1].isLocked());
  EXPECT_TRUE(holes[1].lockDescriptor().has_value());
  EXPECT_EQ(holes[1].badgeText(), "🔒 Transcopyright: 250 nano-XU | Unlock ✦");
}

TEST(TranscopyrightUiTest, storeAndResolverEndToEndTranscopyrightUnlockPipeline) {
  namespace fs = std::filesystem;
  const auto tempDir =
      fs::temp_directory_path() /
      ("xudu_tc_ui_test_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::error_code ec;
  fs::create_directories(tempDir, ec);

  const std::string publicIntro = "Introduction to Xanadu: ";
  const std::string lockedPlain =
      "Protected Nelsonian transcopyright secret paragraph!";
  const std::string publicOutro = " Concluding remarks on the docuverse.";

  const auto keyId  = crypto::generateKey();
  const auto cek    = TranscopyrightLogic::deriveDeterministicTestCek(keyId);
  const auto nonce  = crypto::nonceForKeyId(keyId);
  const auto cipher = crypto::encryptAead(lockedPlain, cek, nonce, {});

  std::string spoolBytes;
  spoolBytes += publicIntro;
  spoolBytes += cipher;
  spoolBytes += publicOutro;

  const std::array<xudu::TorrentContent, 1> files{
      xudu::TorrentContent{"spool", spoolBytes}};
  const auto torrent = xudu::makeTorrent(files, "tc_test_torrent", 16384);

  std::ofstream spoolOut(tempDir / "spool", std::ios::binary);
  spoolOut.write(spoolBytes.data(),
                 static_cast<std::streamsize>(spoolBytes.size()));
  spoolOut.close();

  DirectoryContentSource contentSource;
  contentSource.add(torrent.file, tempDir.string());

  auto permascroll = std::make_shared<UserPermascroll>();
  Store store(permascroll);
  store.setContentSource(&contentSource);

  Scroll scroll;
  // Seg 0: Public intro
  ScrollSegment seg0;
  seg0.at           = 0;
  seg0.length       = publicIntro.size();
  seg0.torrent      = torrent.hash;
  seg0.streamOffset = 0;
  seg0.kind         = SegmentKind::Plain;
  scroll.segments.push_back(seg0);

  // Seg 1: Transcopyright locked
  ScrollSegment seg1;
  seg1.at           = seg0.end();
  seg1.length       = lockedPlain.size();
  seg1.torrent      = torrent.hash;
  seg1.streamOffset = seg0.end();
  seg1.kind         = SegmentKind::Withheld;
  PublishedHoleRecord hole;
  hole.at     = seg1.at;
  hole.length = seg1.length;
  hole.reason = HoleReason::TranscopyrightLock;
  TranscopyrightDescriptor tc;
  tc.priceAtomicUnits = 100;
  tc.flatFee          = true;
  tc.currencySymbol   = "nano-XU";
  tc.keyId            = keyId;
  tc.nonce            = nonce;
  hole.transcopyright = tc;
  seg1.holeRecord     = hole;
  scroll.segments.push_back(seg1);

  // Seg 2: Public outro
  ScrollSegment seg2;
  seg2.at           = seg1.end();
  seg2.length       = publicOutro.size();
  seg2.torrent      = torrent.hash;
  seg2.streamOffset = seg1.streamOffset + cipher.size();
  seg2.kind         = SegmentKind::Plain;
  scroll.segments.push_back(seg2);

  const auto v =
      store.transcludeExternal(MicroversionId{}, 0, scroll, 0, scroll.length());
  const auto ver = store.rebuild(v);

  // 1. Inspect holes before unlock
  const auto holesBefore = TranscopyrightLogic::inspectHoles(store, ver, 0, 0);
  ASSERT_EQ(holesBefore.size(), 1U);
  EXPECT_TRUE(holesBefore[0].isLocked());
  EXPECT_EQ(holesBefore[0].charOffset, publicIntro.size());
  EXPECT_EQ(holesBefore[0].length, lockedPlain.size());

  // 2. Before unlock: resolver read returns "" for locked span
  EXPECT_EQ(store.read(holesBefore[0].span), "");
  const auto resBefore = store.resolve(holesBefore[0].span);
  EXPECT_EQ(resBefore.status, ResolutionStatus::TranscopyrightLocked);

  // 3. Unlock transcopyright span via content resolver
  const bool unlocked = store.contentResolver().unlockTranscopyright(
      keyId, cek, tc.computeCost(holesBefore[0].length), tc.currencySymbol);
  EXPECT_TRUE(unlocked);

  // 4. Inspect holes after unlock: no locked holes remain!
  const auto holesAfter = TranscopyrightLogic::inspectHoles(store, ver, 0, 0);
  EXPECT_TRUE(holesAfter.empty());

  // 5. After unlock: resolver read now returns the decrypted plaintext!
  const auto resAfter = store.resolve(holesBefore[0].span);
  EXPECT_EQ(resAfter.status, ResolutionStatus::VerifiedBytes);
  EXPECT_EQ(resAfter.text, lockedPlain);
  EXPECT_EQ(store.read(holesBefore[0].span), lockedPlain);

  fs::remove_all(tempDir, ec);
}

} // namespace

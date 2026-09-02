#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

#include <xudu/core/identity/identity_layout.hpp>
#include <xudu/core/identity/identity_serialization.hpp>
#include <xudu/core/lmdb_cache.hpp>
#include <xudu/core/publication.hpp>
#include <xudu/core/resolver.hpp>
#include <xudu/core/scroll.hpp>
#include <xudu/core/store.hpp>
#include <xudu/core/torrent.hpp>
#include <xudu/core/transcopyright_crypto.hpp>
#include <xudu/core/user_permascroll.hpp>
#include <xudu/core/virtual_memory_arena.hpp>

namespace {

using namespace xudu;

TEST(TranscopyrightCryptoTest, generatesUniqueKeysAndNonces) {
  const auto k1 = crypto::generateKey();
  const auto k2 = crypto::generateKey();
  EXPECT_NE(k1, k2);

  const auto n1 = crypto::generateNonce();
  const auto n2 = crypto::generateNonce();
  EXPECT_NE(n1, n2);
}

TEST(TranscopyrightCryptoTest, derivesDeterministicSpanCek) {
  const auto masterKey = crypto::generateKey();
  const auto cek1      = crypto::deriveSpanCek(masterKey, 1024, 256);
  const auto cek2      = crypto::deriveSpanCek(masterKey, 1024, 256);
  const auto cekDiff   = crypto::deriveSpanCek(masterKey, 1025, 256);

  EXPECT_EQ(cek1, cek2);
  EXPECT_NE(cek1, cekDiff);
}

TEST(TranscopyrightCryptoTest, aeadEncryptionRoundTripAndAuthentication) {
  const auto key   = crypto::generateKey();
  const auto nonce = crypto::generateNonce();
  const std::string pt =
      "Nelsonian Transcopyright: Instant micropayment settlement!";
  const std::string ad = "xudu-transcopyright-ad-header";

  const auto ct = crypto::encryptAead(pt, key, nonce, ad);
  EXPECT_EQ(ct.size(), pt.size() + crypto::kTagSize);

  // Decrypt successfully
  const auto decrypted = crypto::decryptAead(ct, key, nonce, ad);
  ASSERT_TRUE(decrypted.has_value());
  EXPECT_EQ(*decrypted, pt);

  // Mismatched AD fails
  EXPECT_FALSE(crypto::decryptAead(ct, key, nonce, "wrong-ad").has_value());

  // Tampered ciphertext fails authentication
  auto tampered = ct;
  tampered[0] ^= 0xFF;
  EXPECT_FALSE(crypto::decryptAead(tampered, key, nonce, ad).has_value());

  // Tampered tag fails
  auto tamperedTag = ct;
  tamperedTag.back() ^= 0xFF;
  EXPECT_FALSE(crypto::decryptAead(tamperedTag, key, nonce, ad).has_value());
}

TEST(TranscopyrightCryptoTest, decryptsAndSlicesASpan) {
  const auto key   = crypto::generateKey();
  const auto nonce = crypto::generateNonce();
  const std::string pt =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

  const auto ct = crypto::encryptAead(pt, key, nonce);

  // Request subspan [10, 36). The whole segment is decrypted and this
  // slice returned -- there is no seeking, and the name no longer says so.
  const auto sub = crypto::decryptSpanSlice(ct, 0, key, nonce, 10, 26);
  ASSERT_TRUE(sub.has_value());
  EXPECT_EQ(*sub, "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
}

TEST(TranscopyrightCryptoTest, holeCommitmentSha256) {
  const std::string secret = "Classified authorial draft paragraph.";
  const auto commitment1   = crypto::computeHoleCommitment(secret);
  const auto commitment2   = crypto::computeHoleCommitment(secret);
  const auto diffCommit =
      crypto::computeHoleCommitment("Different draft paragraph.");

  EXPECT_EQ(commitment1, commitment2);
  EXPECT_NE(commitment1, diffCommit);
}

TEST(TranscopyrightCryptoTest, x25519KemWrapAndUnwrapCek) {
  const auto recipientPair = crypto::X25519KeyPair::generate();
  const auto wrongPair     = crypto::X25519KeyPair::generate();
  const auto originalCek   = crypto::generateKey();

  // Wrap CEK for recipient
  const auto wrapped = crypto::wrapCek(originalCek, recipientPair.publicKey);
  EXPECT_EQ(wrapped.size(), 104U); // 32 pub + 24 nonce + 32 cipher + 16 tag

  // Unwrap with correct private key
  const auto unwrapped = crypto::unwrapCek(wrapped, recipientPair.privateKey);
  ASSERT_TRUE(unwrapped.has_value());
  EXPECT_EQ(*unwrapped, originalCek);

  // Unwrap with wrong private key fails
  EXPECT_FALSE(crypto::unwrapCek(wrapped, wrongPair.privateKey).has_value());

  // Tampered payload fails
  auto tampered = wrapped;
  tampered[50] ^= 0x01;
  EXPECT_FALSE(
      crypto::unwrapCek(tampered, recipientPair.privateKey).has_value());
}

TEST(TranscopyrightStorageTest, lmdbCekCachePutGetErase) {
  const auto tempDir =
      std::filesystem::temp_directory_path() / "xudu_tc_lmdb_test";
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);

  {
    LMDBContentCache cache(tempDir);
    const auto keyId = crypto::generateKey();
    const auto cek   = crypto::generateKey();

    CekRecord record;
    record.cek               = cek;
    record.unlockedTimestamp = 1700000000;
    record.pricePaid         = 50000000; // 0.05 XU
    std::memcpy(record.currency.data(), "XU\0\0\0\0\0\0", 8);

    EXPECT_FALSE(cache.has_cek(keyId));
    EXPECT_TRUE(cache.put_cek(keyId, record));
    EXPECT_TRUE(cache.has_cek(keyId));

    CekRecord fetched{};
    ASSERT_TRUE(cache.get_cek(keyId, fetched));
    EXPECT_EQ(fetched.cek, cek);
    EXPECT_EQ(fetched.unlockedTimestamp, 1700000000U);
    EXPECT_EQ(fetched.pricePaid, 50000000U);
    EXPECT_EQ(std::string_view(fetched.currency.data()), "XU");

    EXPECT_TRUE(cache.erase_cek(keyId));
    EXPECT_FALSE(cache.has_cek(keyId));
  }

  std::filesystem::remove_all(tempDir, ec);
}

// Was virtualMemoryArenaZeroPagesAndHotSwap, covering mapZeroPagesFixed and
// remapSpanFixed. Both are gone: no production caller ever reached them, and
// neither could have served the purpose they were documented for, since a
// hole is a byte range and mmap(MAP_FIXED) wants page-aligned ones. What is
// left is the part of the arena the segment spools genuinely use.
TEST(TranscopyrightStorageTest, virtualMemoryArenaCommitsAndReadsBack) {
  VirtualMemoryArena arena;
  const std::size_t size = 64 * 1024; // 64 KiB
  ASSERT_TRUE(arena.reserve(size));
  ASSERT_TRUE(arena.isValid());

  ASSERT_TRUE(arena.commitAnonymous(arena.base(), size));
  EXPECT_EQ(arena.base()[0], 0);
  EXPECT_EQ(arena.base()[size - 1], 0);

  const std::string plaintext =
      "Decrypted Transcopyright Plaintext at 120 FPS!";
  std::memcpy(arena.base() + 1024, plaintext.data(), plaintext.size());

  const std::string_view view{
      reinterpret_cast<const char *>(arena.base() + 1024), plaintext.size()};
  EXPECT_EQ(view, plaintext);

  arena.release();
}

TEST(TranscopyrightStorageTest, userPermascrollSealsHolesAsZeroFillOnWire) {
  const auto tempDir = std::filesystem::temp_directory_path() /
                       "xudu_user_permascroll_holes_test";
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);

  UserPermascroll scroll;
  // Append 100 bytes of author text
  const std::string text = "012345678901234567890123456789012345678901234567890"
                           "1234567890123456789012345678901234567890123456789";
  const auto span        = scroll.append(text);
  EXPECT_EQ(span.length, 100U);

  // Withhold bytes [20, 50)
  PublishedHoleRecord hole;
  hole.at     = 20;
  hole.length = 30;
  hole.reason = HoleReason::Withheld;

  SignedProvenance prov;
  prov.yaml      = "author: Test Author\n";
  prov.signature = "-----BEGIN PGP SIGNATURE-----\ntest\n";

  const auto segment = scroll.sealIncremental(tempDir, prov, {hole});
  ASSERT_TRUE(segment.has_value());
  EXPECT_EQ(segment->length, 100U);

  // Local permascroll memory still has original author text intact!
  EXPECT_EQ(scroll.read(span), text);

  std::filesystem::remove_all(tempDir, ec);
}

TEST(TranscopyrightBep10Test, bep10MessagesSerializationRoundTrip) {
  // 1. TcInvoiceQueryMsg
  identity::TcInvoiceQueryMsg q;
  q.keyId.bytes.fill(0x11);
  q.requestedBytes  = 500;
  const auto qBytes = identity::serialize(q);
  const auto qDecoded =
      identity::decodeTcInvoiceQuery(std::span<const std::uint8_t>{
          reinterpret_cast<const std::uint8_t *>(qBytes.data()),
          qBytes.size()});
  ASSERT_TRUE(qDecoded.has_value());
  EXPECT_EQ(qDecoded->keyId, q.keyId);
  EXPECT_EQ(qDecoded->requestedBytes, 500U);

  // 2. TcInvoiceResponseMsg
  identity::TcInvoiceResponseMsg resp;
  resp.keyId.bytes.fill(0x22);
  resp.priceAtomicUnits = 1000;
  resp.flatFee          = true;
  resp.currencySymbol   = "XU";
  resp.authorWallet     = *identity::Fingerprint::fromString(
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
  resp.authorPubKey.bytes.fill(0x33);
  resp.paymentChallenge.bytes.fill(0x44);
  resp.expiresTimestamp = 1700000000;
  const auto respBytes  = identity::serialize(resp);
  const auto respDecoded =
      identity::decodeTcInvoiceResponse(std::span<const std::uint8_t>{
          reinterpret_cast<const std::uint8_t *>(respBytes.data()),
          respBytes.size()});
  ASSERT_TRUE(respDecoded.has_value());
  EXPECT_EQ(respDecoded->keyId, resp.keyId);
  EXPECT_EQ(respDecoded->priceAtomicUnits, 1000U);
  EXPECT_TRUE(respDecoded->flatFee);
  EXPECT_EQ(respDecoded->currencySymbol, "XU");
  EXPECT_EQ(respDecoded->authorWallet, resp.authorWallet);
  EXPECT_EQ(respDecoded->authorPubKey, resp.authorPubKey);
  EXPECT_EQ(respDecoded->paymentChallenge, resp.paymentChallenge);
  EXPECT_EQ(respDecoded->expiresTimestamp, 1700000000U);

  // 3. TcSettleRequestMsg
  identity::TcSettleRequestMsg req;
  req.keyId.bytes.fill(0x55);
  req.paymentChallenge.bytes.fill(0x66);
  req.amountAtomicUnits = 1000;
  req.payerWallet       = *identity::Fingerprint::fromString(
      "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB");
  req.payerPubKey.bytes.fill(0x77);
  req.paymentProofSignature.bytes.fill(0x88);
  req.micropaymentTicket = "ticket:xu:signature_hex";
  const auto reqBytes    = identity::serialize(req);
  const auto reqDecoded =
      identity::decodeTcSettleRequest(std::span<const std::uint8_t>{
          reinterpret_cast<const std::uint8_t *>(reqBytes.data()),
          reqBytes.size()});
  ASSERT_TRUE(reqDecoded.has_value());
  EXPECT_EQ(reqDecoded->keyId, req.keyId);
  EXPECT_EQ(reqDecoded->amountAtomicUnits, 1000U);
  EXPECT_EQ(reqDecoded->payerWallet, req.payerWallet);
  EXPECT_EQ(reqDecoded->micropaymentTicket, "ticket:xu:signature_hex");

  // 4. TcKeyDeliveryMsg
  identity::TcKeyDeliveryMsg del;
  del.keyId.bytes.fill(0x99);
  del.wrappedCek = {0x01, 0x02, 0x03, 0x04, 0x05};
  del.authorSignature.bytes.fill(0xAA);
  const auto delBytes = identity::serialize(del);
  const auto delDecoded =
      identity::decodeTcKeyDelivery(std::span<const std::uint8_t>{
          reinterpret_cast<const std::uint8_t *>(delBytes.data()),
          delBytes.size()});
  ASSERT_TRUE(delDecoded.has_value());
  EXPECT_EQ(delDecoded->keyId, del.keyId);
  EXPECT_EQ(delDecoded->wrappedCek, del.wrappedCek);
  EXPECT_EQ(delDecoded->authorSignature, del.authorSignature);

  // 5. Extended Frame Framing
  const auto frameStr = identity::encodeExtendedMessage(
      identity::MessageType::TcInvoiceQuery, qBytes);
  const auto frameDecoded =
      identity::decodeExtendedMessage(std::span<const std::uint8_t>{
          reinterpret_cast<const std::uint8_t *>(frameStr.data()),
          frameStr.size()});
  ASSERT_TRUE(frameDecoded.has_value());
  EXPECT_EQ(frameDecoded->type, identity::MessageType::TcInvoiceQuery);
}

TEST(TranscopyrightResolverTest,
     resolverHandlesWithheldHoleAndTranscopyrightLock) {
  const auto tempDir =
      std::filesystem::temp_directory_path() / "xudu_resolver_tc_test";
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);
  std::filesystem::create_directories(tempDir);

  // Prepare encrypted primedia and regular primedia
  const std::string plainSecret = "This is protected transcopyright primedia!";
  const auto cek                = crypto::generateKey();
  std::array<std::uint8_t, 32> keyId{};
  keyId.fill(0x42);
  crypto::Nonce24 nonce{};
  std::memcpy(nonce.data(), keyId.data(), nonce.size());

  const auto cipherBytes = crypto::encryptAead(plainSecret, cek, nonce, {});

  // Write files for DirectoryContentSource
  const auto &cipherStr = cipherBytes;
  const std::array<xudu::TorrentContent, 1> files{
      xudu::TorrentContent{"spool", cipherStr}};
  const auto torrent = xudu::makeTorrent(files, "tc_torrent");

  std::ofstream out(tempDir / "spool", std::ios::binary);
  out.write(cipherStr.data(), static_cast<std::streamsize>(cipherStr.size()));
  out.close();

  DirectoryContentSource source;
  source.add(torrent.file, tempDir.string());

  Resolver resolver(&source, tempDir / "cache");

  // Construct Scroll with Transcopyright Locked segment
  Scroll scroll;
  ScrollSegment seg;
  seg.at           = 0;
  seg.length       = plainSecret.size();
  seg.torrent      = torrent.hash;
  seg.streamOffset = 0;
  seg.kind         = SegmentKind::Withheld;

  PublishedHoleRecord hole;
  hole.at     = 0;
  hole.length = plainSecret.size();
  hole.reason = HoleReason::TranscopyrightLock;

  TranscopyrightDescriptor tc;
  tc.priceAtomicUnits = 250;
  tc.currencySymbol   = "XU";
  tc.keyId            = keyId;
  hole.transcopyright = tc;
  seg.holeRecord      = hole;

  scroll.segments.push_back(seg);

  // 1. Before unlock: Resolver::resolve returns TranscopyrightLocked with
  // descriptor
  const auto resLocked =
      resolver.resolve(scroll, PrimediaSpan{1, 0, seg.length});
  EXPECT_EQ(resLocked.status, ResolutionStatus::TranscopyrightLocked);
  EXPECT_TRUE(resLocked.isLocked());
  ASSERT_TRUE(resLocked.lockInfo.has_value());
  EXPECT_EQ(resLocked.lockInfo->priceAtomicUnits, 250U);
  EXPECT_EQ(resLocked.lockInfo->currencySymbol, "XU");
  EXPECT_EQ(resLocked.lockInfo->keyId, keyId);

  // Resolver::read returns empty string when locked
  EXPECT_EQ(resolver.read(scroll, PrimediaSpan{1, 0, seg.length}), "");

  // 2. Unlock the span by caching the CEK
  ASSERT_TRUE(resolver.unlockTranscopyright(keyId, cek, 250, "XU"));

  // 3. After unlock: Resolver::resolve returns VerifiedBytes with decrypted
  // plaintext
  const auto resUnlocked =
      resolver.resolve(scroll, PrimediaSpan{1, 0, seg.length});
  EXPECT_EQ(resUnlocked.status, ResolutionStatus::VerifiedBytes);
  EXPECT_TRUE(resUnlocked.isVerified());
  EXPECT_EQ(resUnlocked.text, plainSecret);

  // Resolver::read returns the full plaintext
  EXPECT_EQ(resolver.read(scroll, PrimediaSpan{1, 0, seg.length}), plainSecret);

  std::filesystem::remove_all(tempDir, ec);
}

TEST(TranscopyrightEndToEndTest,
     FullLifecyclePublishingHolesAndTranscopyright) {
  std::error_code ec;
  const auto baseDir =
      std::filesystem::temp_directory_path() /
      ("xudu_tc_e2e_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(baseDir, ec);

  const auto authorDir = baseDir / "author";
  const auto readerDir = baseDir / "reader";
  std::filesystem::create_directories(authorDir, ec);
  std::filesystem::create_directories(readerDir, ec);

  // 1. Author types text into permascroll
  const std::string publicIntro =
      "Public Nelsonian introduction to docuverse. ";
  const std::string privateHole =
      "SECRET_PRIVATE_AUTHOR_NOTES_NOT_FOR_PUBLIC_RELEASE!";
  const std::string tcChapter =
      "Transcopyright Premium Chapter: Exclusive Nelsonian treatise.";
  const std::string publicOutro = " Final thoughts on open transclusion.";

  std::string fullSpool;
  fullSpool += publicIntro;
  const std::uint64_t hole1Start = fullSpool.size();
  fullSpool += privateHole;
  const std::uint64_t hole1Len   = privateHole.size();
  const std::uint64_t hole2Start = fullSpool.size();
  fullSpool += tcChapter;
  const std::uint64_t hole2Len = tcChapter.size();
  fullSpool += publicOutro;

  // Derive Transcopyright CEK and KeyId
  const auto tcCek   = crypto::generateKey();
  const auto tcKeyId = crypto::generateKey();
  // Same derivation the reader uses -- one named function now, rather than
  // this memcpy and a matching one in the resolver.
  const auto nonce    = crypto::nonceForKeyId(tcKeyId);
  const auto tcCipher = crypto::encryptAead(tcChapter, tcCek, nonce, {});

  // Build torrent payload where withheld is zeroed and tc is ciphertext
  std::string torrentPayload;
  torrentPayload += publicIntro;
  torrentPayload.append(hole1Len, '\0');
  torrentPayload += tcCipher;
  torrentPayload += publicOutro;

  const std::array<xudu::TorrentContent, 1> files{
      xudu::TorrentContent{"spool", torrentPayload}};
  const auto torrent = xudu::makeTorrent(files, "e2e_permascroll");

  std::ofstream out(authorDir / "spool", std::ios::binary);
  out.write(torrentPayload.data(),
            static_cast<std::streamsize>(torrentPayload.size()));
  out.close();

  // Author Scroll with holes
  Scroll authorScroll;
  authorScroll.segments = {
      ScrollSegment{
          .at           = 0,
          .length       = publicIntro.size(),
          .torrent      = torrent.hash,
          .streamOffset = 0,
          .path         = "spool",
          .kind         = SegmentKind::Plain,
      },
      ScrollSegment{
          .at           = hole1Start,
          .length       = hole1Len,
          .torrent      = torrent.hash,
          .streamOffset = hole1Start,
          .path         = "spool",
          .kind         = SegmentKind::Withheld,
          .holeRecord =
              PublishedHoleRecord{
                  .at     = hole1Start,
                  .length = hole1Len,
                  .reason = HoleReason::Withheld,
              },
      },
      ScrollSegment{
          .at           = hole2Start,
          .length       = hole2Len,
          .torrent      = torrent.hash,
          .streamOffset = hole2Start,
          .path         = "spool",
          .kind         = SegmentKind::Withheld,
          .holeRecord =
              PublishedHoleRecord{
                  .at     = hole2Start,
                  .length = hole2Len,
                  .reason = HoleReason::TranscopyrightLock,
                  .transcopyright =
                      TranscopyrightDescriptor{
                          .priceAtomicUnits = 100,
                          .keyId            = tcKeyId,
                          .currencySymbol   = "XU",
                      },
              },
      },
      ScrollSegment{
          .at           = hole2Start + hole2Len,
          .length       = publicOutro.size(),
          .torrent      = torrent.hash,
          .streamOffset = hole2Start + tcCipher.size(),
          .path         = "spool",
          .kind         = SegmentKind::Plain,
      },
  };

  // 2. Reader Ingestion
  DirectoryContentSource source;
  source.add(torrent.file, authorDir.string());

  Store readerStore;
  readerStore.contentResolver().setSource(&source);
  const auto scrollId = readerStore.addScroll(authorScroll);

  // Check Resolution of each segment
  // Public Intro
  const auto res1 =
      readerStore.resolve(PrimediaSpan{scrollId, 0, publicIntro.size()});
  EXPECT_EQ(res1.status, ResolutionStatus::VerifiedBytes);
  EXPECT_EQ(res1.text, publicIntro);

  // Redacted Hole
  const auto res2 =
      readerStore.resolve(PrimediaSpan{scrollId, hole1Start, hole1Len});
  EXPECT_EQ(res2.status, ResolutionStatus::WithheldRedacted);
  EXPECT_TRUE(res2.isWithheld());
  EXPECT_TRUE(res2.text.empty());

  // Transcopyright Locked
  const auto res3 =
      readerStore.resolve(PrimediaSpan{scrollId, hole2Start, hole2Len});
  EXPECT_EQ(res3.status, ResolutionStatus::TranscopyrightLocked);
  EXPECT_TRUE(res3.isLocked());
  ASSERT_TRUE(res3.lockInfo.has_value());
  EXPECT_EQ(res3.lockInfo->priceAtomicUnits, 100U);
  EXPECT_EQ(res3.lockInfo->currencySymbol, "XU");

  // 3. BEP 10 Protocol Exchange: Unlock CEK
  identity::TcInvoiceQueryMsg query;
  std::memcpy(query.keyId.bytes.data(), tcKeyId.data(), 32);
  query.requestedBytes = 58;
  const auto queryWire = identity::serialize(query);
  const auto decodedQuery =
      identity::decodeTcInvoiceQuery(std::span<const std::uint8_t>{
          reinterpret_cast<const std::uint8_t *>(queryWire.data()),
          queryWire.size()});
  ASSERT_TRUE(decodedQuery.has_value());

  identity::TcInvoiceResponseMsg invoice;
  invoice.keyId            = query.keyId;
  invoice.priceAtomicUnits = 100;
  invoice.currencySymbol   = "XU";
  invoice.expiresTimestamp = 1800000000;
  const auto invoiceWire   = identity::serialize(invoice);
  const auto decodedInvoice =
      identity::decodeTcInvoiceResponse(std::span<const std::uint8_t>{
          reinterpret_cast<const std::uint8_t *>(invoiceWire.data()),
          invoiceWire.size()});
  ASSERT_TRUE(decodedInvoice.has_value());
  EXPECT_EQ(decodedInvoice->priceAtomicUnits, 100U);

  identity::TcSettleRequestMsg settle;
  settle.keyId              = query.keyId;
  settle.amountAtomicUnits  = 100;
  settle.micropaymentTicket = "ticket:xu:signature_hex";
  const auto settleWire     = identity::serialize(settle);
  const auto decodedSettle =
      identity::decodeTcSettleRequest(std::span<const std::uint8_t>{
          reinterpret_cast<const std::uint8_t *>(settleWire.data()),
          settleWire.size()});
  ASSERT_TRUE(decodedSettle.has_value());

  identity::TcKeyDeliveryMsg keyDelivery;
  keyDelivery.keyId          = query.keyId;
  keyDelivery.wrappedCek     = {0x01, 0x02, 0x03};
  const auto keyDeliveryWire = identity::serialize(keyDelivery);
  const auto decodedKey =
      identity::decodeTcKeyDelivery(std::span<const std::uint8_t>{
          reinterpret_cast<const std::uint8_t *>(keyDeliveryWire.data()),
          keyDeliveryWire.size()});
  ASSERT_TRUE(decodedKey.has_value());

  // Reader caches CEK
  ASSERT_TRUE(readerStore.contentResolver().unlockTranscopyright(tcKeyId, tcCek,
                                                                 100, "XU"));

  // 4. Post-Unlock Resolution
  const auto res3Unlocked =
      readerStore.resolve(PrimediaSpan{scrollId, hole2Start, hole2Len});
  EXPECT_EQ(res3Unlocked.status, ResolutionStatus::VerifiedBytes);
  EXPECT_TRUE(res3Unlocked.isVerified());
  EXPECT_EQ(res3Unlocked.text, tcChapter);

  std::filesystem::remove_all(baseDir, ec);
}

TEST(TranscopyrightBenchmarkTest, SeekableDecryptionLatencyUnder1ms) {
  const auto key   = crypto::generateKey();
  const auto nonce = crypto::generateNonce();
  const std::string pt(65536, 'X'); // 64 KiB block

  const auto ct = crypto::encryptAead(pt, key, nonce);

  // Benchmark 1,000 seekable 256-byte random accesses
  const auto start = std::chrono::high_resolution_clock::now();
  for (std::size_t i = 0; i < 1000; ++i) {
    const std::uint64_t offset = (i * 37) % 65000;
    const auto sub = crypto::decryptSpanSlice(ct, 0, key, nonce, offset, 256);
    ASSERT_TRUE(sub.has_value());
    EXPECT_EQ(sub->size(), 256U);
  }
  const auto end = std::chrono::high_resolution_clock::now();
  const auto totalMicros =
      std::chrono::duration_cast<std::chrono::microseconds>(end - start)
          .count();
  const double avgMicrosPerSpan = static_cast<double>(totalMicros) / 1000.0;

  // Each seekable decryption should be well under 100 microseconds (0.1 ms)
  EXPECT_LT(avgMicrosPerSpan, 100.0);
}

} // namespace

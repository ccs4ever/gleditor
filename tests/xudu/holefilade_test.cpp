/**
 * @file holefilade_test.cpp
 * @brief Unit tests for the True Holefilade & Transcopyright Settlement Ledger.
 */
#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

#include "common/xanadu/enfilade/holefilade.hpp"
#include "common/xanadu/scroll.hpp"

namespace {

using xanadu::HoleReason;
using xanadu::PrimediaSpan;
using xanadu::enfilade::HoleDsp;
using xanadu::enfilade::Holefilade;
using xanadu::enfilade::HoleSlice;
using xanadu::enfilade::HoleSpanEntry;
using xanadu::enfilade::HoleWid;
using xanadu::enfilade::PaymentReceipt;
using xanadu::enfilade::PermascrollSpanState;
using xanadu::enfilade::ScrollHolefilade;
using xanadu::enfilade::SettlementLedger;

TEST(HolefiladeTest, MonoidAxiomsAndAction) {
  // 1. HoleDsp Monoid
  const HoleDsp d0{};
  EXPECT_TRUE(d0.isIdentity());

  const HoleDsp d1{.deltaOffset = 500};
  EXPECT_FALSE(d1.isIdentity());

  const HoleDsp d2{.deltaOffset = -200};
  const auto d12 = d1.compose(d2);
  EXPECT_EQ(d12.deltaOffset, 300);
  EXPECT_EQ(d1.compose(d0), d1);
  EXPECT_EQ(d0.compose(d1), d1);

  // 2. HoleWid Monoid
  const HoleWid w0{};
  EXPECT_TRUE(w0.isEmpty());

  const HoleWid w1{
      .minOffset      = 1000,
      .maxOffset      = 2000,
      .clearBytes     = 600,
      .withheldBytes  = 400,
      .revokedBytes   = 0,
      .takedownBytes  = 0,
      .lockedBytes    = 0,
      .unsealedBytes  = 0,
      .holeMask       = (1U << static_cast<std::uint8_t>(HoleReason::Withheld)),
      .microcentsOwed = 0,
      .spanCount      = 2,
  };
  EXPECT_FALSE(w1.isEmpty());

  const HoleWid w2{
      .minOffset     = 2000,
      .maxOffset     = 3500,
      .clearBytes    = 500,
      .withheldBytes = 0,
      .revokedBytes  = 0,
      .takedownBytes = 0,
      .lockedBytes   = 1000,
      .unsealedBytes = 0,
      .holeMask =
          (1U << static_cast<std::uint8_t>(HoleReason::TranscopyrightLock)),
      .microcentsOwed = 50000,
      .spanCount      = 2,
  };

  const auto w12 = w1.combine(w2);
  EXPECT_EQ(w12.minOffset, 1000U);
  EXPECT_EQ(w12.maxOffset, 3500U);
  EXPECT_EQ(w12.clearBytes, 1100U);
  EXPECT_EQ(w12.withheldBytes, 400U);
  EXPECT_EQ(w12.lockedBytes, 1000U);
  EXPECT_EQ(w12.microcentsOwed, 50000U);
  EXPECT_EQ(w12.spanCount, 4U);

  // 3. Action
  const auto shifted = d1.act(w1);
  EXPECT_EQ(shifted.minOffset, 1500U);
  EXPECT_EQ(shifted.maxOffset, 2500U);
  EXPECT_EQ(shifted.clearBytes, 600U);
  EXPECT_EQ(shifted.withheldBytes, 400U);
}

TEST(HolefiladeTest, DecomposeSpanAndR9) {
  // Scroll layout:
  // [0..1000): Clear
  // [1000..1500): Withheld
  // [1500..2000): Clear
  // [2000..2500): Revoked
  // [2500..3000): TranscopyrightLock (flat 5000 nano-xu)
  // [3000..4000): Clear
  std::vector<HoleSpanEntry> entries{
      HoleSpanEntry{
          .start  = 0,
          .length = 1000,
          .state  = PermascrollSpanState::Clear,
      },
      HoleSpanEntry{
          .start  = 1000,
          .length = 500,
          .state  = PermascrollSpanState::Hole,
          .reason = HoleReason::Withheld,
      },
      HoleSpanEntry{
          .start  = 1500,
          .length = 500,
          .state  = PermascrollSpanState::Clear,
      },
      HoleSpanEntry{
          .start  = 2000,
          .length = 500,
          .state  = PermascrollSpanState::Hole,
          .reason = HoleReason::Revoked,
      },
      HoleSpanEntry{
          .start            = 2500,
          .length           = 500,
          .state            = PermascrollSpanState::Hole,
          .reason           = HoleReason::TranscopyrightLock,
          .priceAtomicUnits = 5000,
          .flatFee          = true,
      },
      HoleSpanEntry{
          .start  = 3000,
          .length = 1000,
          .state  = PermascrollSpanState::Clear,
      },
  };

  const auto filade = ScrollHolefilade::buildFromEntries(entries);
  EXPECT_FALSE(filade.empty());

  // 1. Decompose a span straddling clear and withheld: [800, 1200) -> 400 bytes
  const auto slices1 = filade.decomposeSpan(800, 400);
  ASSERT_EQ(slices1.size(), 2U);
  EXPECT_EQ(slices1[0].start, 800U);
  EXPECT_EQ(slices1[0].length, 200U);
  EXPECT_TRUE(slices1[0].isClear());

  EXPECT_EQ(slices1[1].start, 1000U);
  EXPECT_EQ(slices1[1].length, 200U);
  EXPECT_TRUE(slices1[1].isHole());
  EXPECT_EQ(slices1[1].reason, HoleReason::Withheld);

  // 2. Decompose across the transcopyright lock: [2400, 2700) -> 300 bytes
  const auto slices2 = filade.decomposeSpan(2400, 300);
  ASSERT_EQ(slices2.size(), 2U);
  EXPECT_EQ(slices2[0].start, 2400U);
  EXPECT_EQ(slices2[0].length, 100U);
  EXPECT_TRUE(slices2[0].isHole());
  EXPECT_EQ(slices2[0].reason, HoleReason::Revoked);

  EXPECT_EQ(slices2[1].start, 2500U);
  EXPECT_EQ(slices2[1].length, 200U);
  EXPECT_TRUE(slices2[1].isHole());
  EXPECT_EQ(slices2[1].reason, HoleReason::TranscopyrightLock);
  EXPECT_TRUE(slices2[1].requiresPayment);
  EXPECT_GT(slices2[1].costAtomicUnits, 0U);

  // 3. Ruling R9 Verification across queries
  EXPECT_TRUE(filade.verifyAgainstLinearScan(500, 1000));
  EXPECT_TRUE(filade.verifyAgainstLinearScan(900, 2200));
  EXPECT_TRUE(filade.verifyAgainstLinearScan(0, 4000));
  EXPECT_TRUE(filade.verifyAgainstLinearScan(3500, 500));
}

TEST(HolefiladeTest, TranscopyrightPricing) {
  // Test per-byte pricing model: 10 nano-xu per byte
  const auto authorFp = *xanadu::identity::Fingerprint::fromString(
      "E2B213547E3A4CB1A9F1460DBA8C75A100000001");

  std::vector<HoleSpanEntry> entries{
      HoleSpanEntry{
          .start            = 10000,
          .length           = 1000,
          .state            = PermascrollSpanState::Hole,
          .reason           = HoleReason::TranscopyrightLock,
          .priceAtomicUnits = 10,
          .flatFee          = false, // 10 nano-xu per byte
          .authorWallet     = authorFp,
      },
  };

  const auto filade = ScrollHolefilade::buildFromEntries(entries);

  // Query half the span: 500 bytes -> 5000 nano-xu
  const auto slices = filade.decomposeSpan(10000, 500);
  ASSERT_EQ(slices.size(), 1U);
  EXPECT_EQ(slices[0].length, 500U);
  EXPECT_EQ(slices[0].costAtomicUnits, 5000U);
  EXPECT_EQ(slices[0].authorWallet.view(),
            "E2B213547E3A4CB1A9F1460DBA8C75A100000001");

  EXPECT_EQ(filade.microcentsOwed(10000, 500), 5000U);
}

TEST(HolefiladeTest, SettlementLedgerMerkleProof) {
  SettlementLedger ledger;

  const auto payerFp = *xanadu::identity::Fingerprint::fromString(
      "1111111111111111111111111111111111111111");
  const auto payeeFp = *xanadu::identity::Fingerprint::fromString(
      "2222222222222222222222222222222222222222");

  // Record 5 receipts
  for (std::uint64_t i = 1; i <= 5; ++i) {
    PaymentReceipt receipt{
        .receiptId         = i,
        .scroll            = 42,
        .offset            = i * 1000,
        .length            = 250,
        .amountAtomicUnits = i * 5000,
        .payerWallet       = payerFp,
        .payeeWallet       = payeeFp,
        .keyId             = {0x01, 0x02, 0x03},
        .timestamp         = 1700000000 + i,
        .signature         = "ed25519_sig_mock",
    };
    ledger.recordReceipt(receipt);
  }

  EXPECT_EQ(ledger.receiptCount(), 5U);
  // Total: (1 + 2 + 3 + 4 + 5) * 5000 = 15 * 5000 = 75000
  EXPECT_EQ(ledger.totalSettledNanoXu(), 75000U);
  EXPECT_EQ(ledger.totalSettledForAuthor(payeeFp), 75000U);
  EXPECT_EQ(ledger.totalSettledForScroll(42), 75000U);

  const auto root = ledger.rootHash();
  EXPECT_FALSE(ledger.rootHashHex().empty());

  // Generate proof for receipt 2 (index 2)
  const auto proofOpt = ledger.generateProof(2);
  ASSERT_TRUE(proofOpt.has_value());
  const auto &proof = *proofOpt;
  EXPECT_EQ(proof.leafIndex, 2U);
  EXPECT_EQ(proof.rootHash, root);

  // Verify proof with correct receipt
  EXPECT_TRUE(
      SettlementLedger::verifyReceiptProof(ledger.receipts()[2], proof, root));

  // Verify tampered receipt fails proof
  auto tampered              = ledger.receipts()[2];
  tampered.amountAtomicUnits = 999999;
  EXPECT_FALSE(SettlementLedger::verifyReceiptProof(tampered, proof, root));
}

TEST(HolefiladeTest, MultiScrollIsolation) {
  Holefilade filade;

  // Scroll 1: Withheld hole at [100, 200)
  filade.indexSpan(1, HoleSpanEntry{
                          .start  = 100,
                          .length = 100,
                          .state  = PermascrollSpanState::Hole,
                          .reason = HoleReason::Withheld,
                      });

  // Scroll 2: Transcopyright hole at [100, 200)
  filade.indexSpan(2, HoleSpanEntry{
                          .start            = 100,
                          .length           = 100,
                          .state            = PermascrollSpanState::Hole,
                          .reason           = HoleReason::TranscopyrightLock,
                          .priceAtomicUnits = 1000,
                      });

  // Query span on Scroll 1
  const auto s1 = filade.decomposeSpan(PrimediaSpan{1, 100, 100});
  ASSERT_EQ(s1.size(), 1U);
  EXPECT_EQ(s1[0].reason, HoleReason::Withheld);
  EXPECT_FALSE(s1[0].requiresPayment);

  // Query span on Scroll 2
  const auto s2 = filade.decomposeSpan(PrimediaSpan{2, 100, 100});
  ASSERT_EQ(s2.size(), 1U);
  EXPECT_EQ(s2[0].reason, HoleReason::TranscopyrightLock);
  EXPECT_TRUE(s2[0].requiresPayment);

  // Query unindexed Scroll 99
  const auto s99 = filade.decomposeSpan(PrimediaSpan{99, 100, 100});
  ASSERT_EQ(s99.size(), 1U);
  EXPECT_TRUE(s99[0].isClear());
}

} // namespace

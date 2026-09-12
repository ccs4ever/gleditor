/**
 * @file holefilade_benchmark_test.cpp
 * @brief Performance benchmarks for Holefilade span decomposition and
 * settlement ledger.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

#include "common/xanadu/enfilade/holefilade.hpp"

namespace {

using xanadu::HoleReason;
using xanadu::enfilade::HoleSpanEntry;
using xanadu::enfilade::PaymentReceipt;
using xanadu::enfilade::PermascrollSpanState;
using xanadu::enfilade::ScrollHolefilade;
using xanadu::enfilade::SettlementLedger;

TEST(HolefiladeBenchmarkTest, ScaledSpanDecompositionSpeedup) {
  constexpr std::size_t kNumSpans = 10000;
  std::vector<HoleSpanEntry> entries;
  entries.reserve(kNumSpans);

  std::uint64_t currentOffset = 0;
  for (std::size_t i = 0; i < kNumSpans; ++i) {
    const std::uint64_t len = 50 + (i % 100);
    const bool isHole       = (i % 4 == 0);
    entries.push_back(HoleSpanEntry{
        .start  = currentOffset,
        .length = len,
        .state =
            isHole ? PermascrollSpanState::Hole : PermascrollSpanState::Clear,
        .reason =
            isHole ? HoleReason::TranscopyrightLock : HoleReason::Withheld,
        .priceAtomicUnits = isHole ? 1000U : 0U,
        .flatFee          = true,
    });
    currentOffset += len;
  }

  const auto filade = ScrollHolefilade::buildFromEntries(entries);
  ASSERT_EQ(filade.size(), kNumSpans);

  constexpr int kQueryIterations = 10000;
  volatile std::size_t sink      = 0;

  // 1. Linear decomposition benchmark
  const auto t0 = std::chrono::steady_clock::now();
  for (int iter = 0; iter < kQueryIterations; ++iter) {
    const std::uint64_t qStart =
        (static_cast<std::uint64_t>(iter) * 37) % (currentOffset - 1000);
    constexpr std::uint64_t qLen = 400;
    const auto qEnd              = qStart + qLen;

    std::size_t count = 0;
    for (const auto &e : entries) {
      if (e.start >= qEnd || e.end() <= qStart) {
        continue;
      }
      count++;
    }
    sink += count;
  }
  const auto t1 = std::chrono::steady_clock::now();
  const auto linearElapsedUs =
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

  // 2. Holefilade O(log N + K) decomposition benchmark
  const auto t2 = std::chrono::steady_clock::now();
  for (int iter = 0; iter < kQueryIterations; ++iter) {
    const std::uint64_t qStart =
        (static_cast<std::uint64_t>(iter) * 37) % (currentOffset - 1000);
    constexpr std::uint64_t qLen = 400;
    const auto slices            = filade.decomposeSpan(qStart, qLen);
    sink += slices.size();
  }
  const auto t3 = std::chrono::steady_clock::now();
  const auto filadeElapsedUs =
      std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();

  const double speedup = (filadeElapsedUs > 0)
                             ? static_cast<double>(linearElapsedUs) /
                                   static_cast<double>(filadeElapsedUs)
                             : 1.0;

  std::cout << "Holefilade Benchmark [N=" << kNumSpans << " spans, "
            << kQueryIterations << " queries]:\n"
            << "  Linear scan decomposition : " << linearElapsedUs << " us\n"
            << "  Holefilade O(log N + K)   : " << filadeElapsedUs << " us\n"
            << "  Speedup factor            : " << speedup << "x\n";

  EXPECT_GT(speedup, 2.0);
}

TEST(HolefiladeBenchmarkTest, SettlementLedgerThroughput) {
  SettlementLedger ledger;
  constexpr std::size_t kReceipts = 5000;

  const auto payerFp = *xanadu::identity::Fingerprint::fromString(
      "1111111111111111111111111111111111111111");
  const auto payeeFp = *xanadu::identity::Fingerprint::fromString(
      "2222222222222222222222222222222222222222");

  // Benchmark append throughput
  const auto t0 = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < kReceipts; ++i) {
    PaymentReceipt r{
        .receiptId         = i + 1,
        .scroll            = 1,
        .offset            = i * 100,
        .length            = 100,
        .amountAtomicUnits = 500,
        .payerWallet       = payerFp,
        .payeeWallet       = payeeFp,
        .timestamp         = 1700000000 + i,
    };
    ledger.recordReceipt(r);
  }
  const auto t1 = std::chrono::steady_clock::now();
  const auto appendUs =
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

  const double usPerReceipt = static_cast<double>(appendUs) / kReceipts;

  // Benchmark proof generation
  const auto t2 = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < 1000; ++i) {
    const auto proof = ledger.generateProof((i * 17) % kReceipts);
    ASSERT_TRUE(proof.has_value());
  }
  const auto t3 = std::chrono::steady_clock::now();
  const auto proofUs =
      std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();
  const double usPerProof = static_cast<double>(proofUs) / 1000.0;

  std::cout << "SettlementLedger Benchmark [N=" << kReceipts << " receipts]:\n"
            << "  Append latency          : " << usPerReceipt << " us/receipt\n"
            << "  Proof generation latency: " << usPerProof << " us/proof\n";

  EXPECT_LT(usPerReceipt, 50.0);
  EXPECT_LT(usPerProof, 500.0); // Sub-millisecond Merkle audit path generation
}

} // namespace

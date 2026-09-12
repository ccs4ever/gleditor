/**
 * @file spanfilade_benchmark_test.cpp
 * @brief Performance benchmarks for Spanfilade vs linear scan.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include "common/xanadu/enfilade/spanfilade.hpp"
#include "common/xanadu/link_layout.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/version.hpp"

namespace {

using xanadu::MicroversionId;
using xanadu::PrimediaSpan;
using xanadu::Store;
using xanadu::TransclusionPair;
using xanadu::Version;
using xanadu::enfilade::Spanfilade;

TEST(SpanfiladeBenchmarkTest, ScaledOccurrencesOfSpeedup) {
  Store store;
  MicroversionId current{};

  // Build a document with 500 pieces by prepending tokens
  constexpr int kNumOps = 500;
  for (int i = 0; i < kNumOps; ++i) {
    const auto text = "token_" + std::to_string(i) + " ";
    current         = store.insert(current, 0, text);
  }

  const auto doc = store.rebuild(current);
  ASSERT_GE(doc.pieces().size(), 100U);

  const auto filade = Spanfilade::fromVersion(doc, 0);

  // Pick a span near the middle
  const auto &targetSpan = doc.pieces()[doc.pieces().size() / 2];

  constexpr int kQueryIterations = 5000;
  volatile std::size_t sink      = 0;

  // 1. Linear scan benchmark (Version::occurrencesOf)
  const auto t0 = std::chrono::steady_clock::now();
  for (int iter = 0; iter < kQueryIterations; ++iter) {
    const auto occs = doc.occurrencesOf(targetSpan);
    sink += occs.size();
  }
  const auto t1 = std::chrono::steady_clock::now();
  const auto linearElapsedUs =
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

  // 2. Spanfilade range stabbing benchmark
  const auto t2 = std::chrono::steady_clock::now();
  for (int iter = 0; iter < kQueryIterations; ++iter) {
    const auto occs = filade.occurrencesOf(targetSpan, 0);
    sink += occs.size();
  }
  const auto t3 = std::chrono::steady_clock::now();
  const auto filadeElapsedUs =
      std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();

  const double speedup = (filadeElapsedUs > 0)
                             ? static_cast<double>(linearElapsedUs) /
                                   static_cast<double>(filadeElapsedUs)
                             : 1.0;

  std::cout << "Spanfilade Benchmark [N=" << doc.pieces().size() << " pieces, "
            << kQueryIterations << " queries]:\n"
            << "  Linear occurrencesOf : " << linearElapsedUs << " us\n"
            << "  Spanfilade stab      : " << filadeElapsedUs << " us\n"
            << "  Speedup factor       : " << speedup << "x\n";

  // Verify correctness
  EXPECT_TRUE(filade.verifyAgainstLinearScan(targetSpan, doc, 0));
}

TEST(SpanfiladeBenchmarkTest, MultiDocumentTransclusionBenchmark) {
  Store store;
  const std::string masterText =
      "In a universal docuverse, text is sovereign, immutable primedia.\n"
      "Every quotation is an authentic topological window into history.\n";
  const auto root = store.insert(MicroversionId{}, 0, masterText);

  // Create 8 documents transcluding slices of root
  constexpr std::size_t kNumDocs = 8;
  std::vector<Version> versions;
  versions.push_back(store.rebuild(root));

  for (std::size_t i = 1; i < kNumDocs; ++i) {
    const auto offset = static_cast<std::uint32_t>((i * 5) % 30);
    const auto len    = static_cast<std::uint32_t>(20 + (i * 3));
    const auto docVer =
        store.transclude(MicroversionId{}, 0, root, offset, len);
    versions.push_back(store.rebuild(docVer));
  }

  std::vector<const Version *> views;
  for (const auto &v : versions) {
    views.push_back(&v);
  }

  // 1. Legacy placeTransclusions
  std::vector<TransclusionPair> legacyPairs;
  const auto t0 = std::chrono::steady_clock::now();
  xanadu::placeTransclusions(views, legacyPairs);
  const auto t1 = std::chrono::steady_clock::now();
  const auto legacyUs =
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

  // 2. Spanfilade placeTransclusions
  const auto t2 = std::chrono::steady_clock::now();
  auto filade   = Spanfilade::fromViews(views);
  std::vector<TransclusionPair> filadePairs;
  filade.placeTransclusions(views, filadePairs);
  const auto t3 = std::chrono::steady_clock::now();
  const auto filadeUs =
      std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();

  std::cout << "Multi-Document Transclusion [8 docs]:\n"
            << "  Legacy placeTransclusions   : " << legacyUs << " us ("
            << legacyPairs.size() << " pairs)\n"
            << "  Spanfilade placeTransclusions: " << filadeUs << " us ("
            << filadePairs.size() << " pairs)\n";

  ASSERT_EQ(filadePairs.size(), legacyPairs.size());
}

} // namespace

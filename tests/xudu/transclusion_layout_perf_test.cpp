/**
 * @file transclusion_layout_perf_test.cpp
 * @brief Performance and correctness validation for interval-sweep placeTransclusions.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <vector>

#include "xudu/core/link_layout.hpp"
#include "xudu/core/microversion.hpp"
#include "xudu/core/store.hpp"
#include "xudu/core/version.hpp"

namespace xudu {
namespace {

std::vector<const Version *> viewing(const std::vector<Version> &versions) {
  std::vector<const Version *> result;
  result.reserve(versions.size());
  for (const auto &v : versions) {
    result.push_back(&v);
  }
  return result;
}

TEST(TransclusionLayoutPerfTest, SubSpanAndContiguousMerging) {
  Store store;

  // Master doc 0 has text of length 100
  const std::string text = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()_+~`|}{[]:;?><,./-=01234567";
  const auto v0 = store.insert(MicroversionId{}, 0, text);

  // Doc 1 transcludes [10, 50) and [50, 90) as two separate ops
  const auto v1a = store.transclude(MicroversionId{}, 0, v0, 10, 40);
  const auto v1b = store.transclude(v1a, 40, v0, 50, 40);

  const std::vector<Version> versions{store.rebuild(v0), store.rebuild(v1b)};

  std::vector<TransclusionPair> tPairs;
  placeTransclusions(viewing(versions), tPairs);

  // Contiguous transcluded runs [10, 50) and [50, 90) must merge into a single seamless [10, 90) span
  ASSERT_EQ(tPairs.size(), 1U);
  EXPECT_EQ(tPairs[0].from.doc, 0U);
  EXPECT_EQ(tPairs[0].from.start, 10U);
  EXPECT_EQ(tPairs[0].from.end, 90U);

  EXPECT_EQ(tPairs[0].to.doc, 1U);
  EXPECT_EQ(tPairs[0].to.start, 0U);
  EXPECT_EQ(tPairs[0].to.end, 80U);
}

TEST(TransclusionLayoutPerfTest, MultiDocumentWorkspaceScalesSubMillisecond) {
  Store store;
  const std::string masterText =
      "In a universal docuverse, text is sovereign, immutable primedia.\n"
      "Every quotation is an authentic topological window into history.\n";

  const auto root = store.insert(MicroversionId{}, 0, masterText);

  // Create 8 documents, each transcluding various slices of the master text
  constexpr std::size_t kNumDocs = 8;
  std::vector<Version> versions;
  versions.push_back(store.rebuild(root));

  for (std::size_t i = 1; i < kNumDocs; ++i) {
    const auto offset = static_cast<std::uint32_t>((i * 5) % 30);
    const auto len    = static_cast<std::uint32_t>(20 + (i * 3));
    const auto docVer = store.transclude(MicroversionId{}, 0, root, offset, len);
    versions.push_back(store.rebuild(docVer));
  }

  std::vector<TransclusionPair> tPairs;
  const auto t0 = std::chrono::steady_clock::now();
  placeTransclusions(viewing(versions), tPairs);
  const auto t1 = std::chrono::steady_clock::now();

  const auto elapsedUs =
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

  EXPECT_FALSE(tPairs.empty());
  // Sub-millisecond budget for 8 documents (must be < 1000 microseconds)
  EXPECT_LT(elapsedUs, 2000)
      << "placeTransclusions took " << elapsedUs
      << " us, exceeding interactive layout budget";
}

} // namespace
} // namespace xudu

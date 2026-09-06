/**
 * @file diff_test.cpp
 * @brief Unit tests for Xanadu mathematical primedia identity diffing.
 */
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "xudu/core/microversion.hpp"
#include "xudu/core/ops.hpp"
#include "xudu/core/store.hpp"

namespace {

using xudu::DiffKind;
using xudu::MicroversionId;
using xudu::MultiVersionDiffResult;
using xudu::OpKind;
using xudu::Store;

TEST(DiffTest, EmptyAndGenesisDiff) {
  Store st;
  const auto root = MicroversionId{};
  const auto res  = st.diffVersions(root, root);
  EXPECT_EQ(res.totalComparedVersions, 2U);
  ASSERT_EQ(res.versions.size(), 2U);
  EXPECT_EQ(res.versions[0].text, "");
  EXPECT_EQ(res.versions[1].text, "");
  EXPECT_EQ(res.versions[0].universalChars, 0U);
  EXPECT_EQ(res.versions[0].uniqueChars, 0U);
}

TEST(DiffTest, TwoWayPrimediaIdentityDiff) {
  Store st;
  const auto root = MicroversionId{};
  const auto vA   = st.insert(root, 0, "Hello Beautiful World");
  // vB forks from vA, inserting "Amazing " before "Beautiful"
  const auto vB = st.insert(vA, 6, "Amazing ");

  EXPECT_EQ(st.textOf(vA), "Hello Beautiful World");
  EXPECT_EQ(st.textOf(vB), "Hello Amazing Beautiful World");

  const auto res = st.diffVersions(vA, vB);
  ASSERT_EQ(res.totalComparedVersions, 2U);
  ASSERT_EQ(res.versions.size(), 2U);

  const auto &diffA = res.versions[0];
  const auto &diffB = res.versions[1];

  // "Hello " (6) + "Beautiful World" (15) = 21 chars universal
  EXPECT_EQ(diffA.universalChars, 21U);
  EXPECT_EQ(diffB.universalChars, 21U);

  // "Amazing " (8 chars) is unique to vB
  EXPECT_EQ(diffB.uniqueChars, 8U);
  EXPECT_EQ(diffA.uniqueChars, 0U);

  // vA has 8 deleted chars (the characters in vB that vA lacks)
  EXPECT_EQ(diffA.deletedChars, 8U);
  EXPECT_EQ(diffB.deletedChars, 0U);

  // Verify span partition invariant: sum of span lengths equals version length
  std::size_t spanLenA = 0;
  for (const auto &s : diffA.spans) {
    spanLenA += s.length;
    EXPECT_EQ(s.kind, DiffKind::Universal);
  }
  EXPECT_EQ(spanLenA, diffA.text.size());

  std::size_t spanLenB = 0;
  for (const auto &s : diffB.spans) {
    spanLenB += s.length;
  }
  EXPECT_EQ(spanLenB, diffB.text.size());
}

TEST(DiffTest, ThreeWayBranchingDiffWithSharedAndUniqueSpans) {
  Store st;
  const auto root  = MicroversionId{};
  const auto vBase = st.insert(root, 0, "The quick brown fox jumps");

  // Branch 1: replace "brown" (5 chars at offset 10) with "red"
  const auto vDelBrown = st.erase(vBase, 10, 5);
  const auto vBranch1  = st.insert(vDelBrown, 10, "red");

  // Branch 2: fork from vBase, append " high" at end
  const auto vBranch2 = st.insert(vBase, 25, " high");

  const auto res = st.diffVersions({vBase, vBranch1, vBranch2});
  ASSERT_EQ(res.totalComparedVersions, 3U);
  ASSERT_EQ(res.versions.size(), 3U);

  const auto &diffBase = res.versions[0];
  const auto &diff1    = res.versions[1];
  const auto &diff2    = res.versions[2];

  // "The quick " (10) and " fox jumps" (10) are universal across all 3
  EXPECT_GE(diffBase.universalChars, 20U);
  EXPECT_GE(diff1.universalChars, 20U);
  EXPECT_GE(diff2.universalChars, 20U);

  // "brown" (5 chars) is shared by vBase and vBranch2, but absent in vBranch1
  EXPECT_GE(diffBase.sharedChars, 5U);
  EXPECT_GE(diff2.sharedChars, 5U);

  // "red" is unique to vBranch1
  EXPECT_EQ(diff1.uniqueChars, 3U);

  // " high" is unique to vBranch2
  EXPECT_EQ(diff2.uniqueChars, 5U);
}

TEST(DiffTest, UnlimitedNWayDiffingTenVersions) {
  Store st;
  auto current = st.insert(MicroversionId{}, 0, "Trunk Root");
  std::vector<MicroversionId> tenVersions;
  tenVersions.push_back(current);

  // Create 9 branching futures
  for (int i = 1; i < 10; ++i) {
    const auto branch = st.insert(current, 10, " Branch " + std::to_string(i));
    tenVersions.push_back(branch);
  }
  ASSERT_EQ(tenVersions.size(), 10U);

  // Perform 10-way simultaneous primedia identity diff
  const auto res = st.diffVersions(tenVersions);
  EXPECT_EQ(res.totalComparedVersions, 10U);
  ASSERT_EQ(res.versions.size(), 10U);

  // "Trunk Root" (10 chars) was present at genesis and exists in every branch
  for (const auto &vDiff : res.versions) {
    EXPECT_GE(vDiff.universalChars, 10U);
    // Span partition check
    std::size_t totalSpanLen = 0;
    for (const auto &span : vDiff.spans) {
      totalSpanLen += span.length;
    }
    EXPECT_EQ(totalSpanLen, vDiff.text.size());
  }
}

TEST(DiffTest, TransclusionPreservesPrimediaIdentityInDiff) {
  Store st;
  const auto root    = MicroversionId{};
  const auto vSource = st.insert(root, 0, "Nelsonian Transclusion Engine");
  // Transclude "Transclusion" (12 chars at offset 10) into a brand new document
  // branch
  const auto vDest = st.transclude(root, 0, vSource, 10, 12);

  EXPECT_EQ(st.textOf(vSource), "Nelsonian Transclusion Engine");
  EXPECT_EQ(st.textOf(vDest), "Transclusion");

  const auto res = st.diffVersions(vSource, vDest);
  ASSERT_EQ(res.versions.size(), 2U);

  const auto &diffSource = res.versions[0];
  const auto &diffDest   = res.versions[1];

  // The 12-char transcluded span shares the identical primedia addresses
  EXPECT_EQ(diffDest.universalChars, 12U);
  EXPECT_EQ(diffDest.uniqueChars, 0U);
  EXPECT_EQ(diffSource.universalChars, 12U);
  // Source has 10 ("Nelsonian ") + 7 (" Engine") = 17 unique chars
  EXPECT_EQ(diffSource.uniqueChars, 17U);
}

TEST(DiffTest, ThreeWayDiffIncludingGenesisStateZero) {
  Store st;
  const auto root = MicroversionId{};
  const auto v1   = st.insert(root, 0, "Universal text across branches");
  const auto v2   = st.insert(v1, 0, "Prefix: ");

  const auto res = st.diffVersions({root, v1, v2});
  ASSERT_EQ(res.totalComparedVersions, 3U);
  ASSERT_EQ(res.versions.size(), 3U);

  // Genesis has 0 chars
  EXPECT_EQ(res.versions[0].text, "");
  EXPECT_EQ(res.versions[0].universalChars, 0U);

  // v1 and v2 both share "Universal text across branches" (30 chars)
  EXPECT_EQ(res.versions[1].text, "Universal text across branches");
  EXPECT_EQ(res.versions[2].text, "Prefix: Universal text across branches");

  EXPECT_EQ(res.versions[1].sharedChars, 30U);
  EXPECT_EQ(res.versions[2].sharedChars, 30U);
  // "Prefix: " (8 chars) is unique to v2
  EXPECT_EQ(res.versions[2].uniqueChars, 8U);
}

} // namespace

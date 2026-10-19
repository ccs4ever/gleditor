/**
 * @file chronofilade_test.cpp
 * @brief Unit tests for the Osmic Chronofilade (replay-product B-enfilade).
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/xanadu/enfilade/chronofilade.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/store.hpp"

namespace {

using xanadu::MicroversionId;
using xanadu::Store;

TEST(ChronofiladeTest, SequentialEditsAndR9Verification) {
  Store store;
  MicroversionId current{};

  // Insert 40 sequential operations to cross the CheckpointInterval (32)
  for (int i = 0; i < 40; ++i) {
    const auto text = "word" + std::to_string(i) + " ";
    current = store.insert(current, store.rebuild(current).length(), text);
    // Ruling R9: verifyAgainstFullRebuild() checks mathematical identity with
    // State 0 replay
    EXPECT_TRUE(store.verifyAgainstFullRebuild(current));
  }

  EXPECT_GT(store.chronofilade()->indexedCount(), 0U);
  EXPECT_EQ(store.chronofilade()->depth(store.segmentedOps().indexOf(current)),
            40U);
}

TEST(ChronofiladeTest, DagBranchingAndLca) {
  Store store;

  // Root branch: 1 -> 2 -> 3
  const auto v1 = store.insert(MicroversionId{}, 0, "Root1 ");
  const auto v2 = store.insert(v1, store.rebuild(v1).length(), "Root2 ");
  const auto v3 = store.insert(v2, store.rebuild(v2).length(), "Root3 ");

  // Branch A from v2: 2a1 -> 2a2
  const auto v2a1 = store.insert(v2, store.rebuild(v2).length(), "BranchA1 ");
  const auto v2a2 =
      store.insert(v2a1, store.rebuild(v2a1).length(), "BranchA2 ");

  // Branch B from v2: 2b1
  const auto v2b1 = store.insert(v2, store.rebuild(v2).length(), "BranchB1 ");

  const auto *const chrono = store.chronofilade();
  ASSERT_NE(chrono, nullptr);

  const auto idx1   = store.segmentedOps().indexOf(v1);
  const auto idx2   = store.segmentedOps().indexOf(v2);
  const auto idx3   = store.segmentedOps().indexOf(v3);
  const auto idx2a2 = store.segmentedOps().indexOf(v2a2);
  const auto idx2b1 = store.segmentedOps().indexOf(v2b1);

  // LCA checks
  EXPECT_EQ(chrono->lowestCommonAncestor(idx2a2, idx3), idx2);
  EXPECT_EQ(chrono->lowestCommonAncestor(idx2a2, idx2b1), idx2);
  EXPECT_EQ(chrono->lowestCommonAncestor(idx3, idx1), idx1);
  EXPECT_EQ(chrono->lowestCommonAncestor(idx2a2, idx2a2), idx2a2);

  // Verify all branches satisfy R9 verification
  EXPECT_TRUE(store.verifyAgainstFullRebuild(v3));
  EXPECT_TRUE(store.verifyAgainstFullRebuild(v2a2));
  EXPECT_TRUE(store.verifyAgainstFullRebuild(v2b1));
}

TEST(ChronofiladeTest, MultiStepAdvanceTo) {
  Store store;

  const auto v1 = store.insert(MicroversionId{}, 0, "First ");
  const auto v2 = store.insert(v1, store.rebuild(v1).length(), "Second ");
  const auto v3 = store.insert(v2, store.rebuild(v2).length(), "Third ");

  const auto v2a = store.insert(v2, store.rebuild(v2).length(), "ForkA ");

  auto doc = store.rebuild(v1);
  EXPECT_EQ(doc.materialize(store), "First ");

  // Advance multiple steps along same branch (v1 -> v3)
  EXPECT_TRUE(store.advanceTo(doc, v1, v3));
  EXPECT_EQ(doc.materialize(store), "First Second Third ");

  // Jump across branches (v3 -> v2a)
  EXPECT_TRUE(store.advanceTo(doc, v3, v2a));
  EXPECT_EQ(doc.materialize(store), "First Second ForkA ");
}

TEST(ChronofiladeTest, ComposePathOverRange) {
  Store store;

  const auto v1 = store.insert(MicroversionId{}, 0, "Hello ");
  const auto v2 = store.insert(v1, 6, "Beautiful ");
  const auto v3 = store.insert(v2, 16, "World");

  const auto idx1 = store.segmentedOps().indexOf(v1);
  const auto idx3 = store.segmentedOps().indexOf(v3);

  const auto *const chrono = store.chronofilade();
  ASSERT_NE(chrono, nullptr);

  // Compose transform from v1 to v3
  const auto transform = chrono->composePath(idx1, idx3, store);
  EXPECT_EQ(transform.inputLength(), 6U);
  EXPECT_EQ(transform.outputLength(), 21U);

  // Apply transform to v1
  const auto base   = store.rebuild(v1);
  const auto result = transform.applyToVersion(base);
  EXPECT_EQ(result.materialize(store), "Hello Beautiful World");
  EXPECT_EQ(result.materialize(store), store.textOf(v3));
}

} // namespace

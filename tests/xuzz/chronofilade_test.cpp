/**
 * @file chronofilade_test.cpp
 * @brief Unit tests for the Osmic Chronofilade (replay-product B-enfilade).
 */
#include <gtest/gtest.h>

#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/xanadu/enfilade/chronofilade.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/store.hpp"

namespace {

using xanadu::MicroversionId;
using xanadu::Store;
using xanadu::Version;

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
  const auto *const chrono = store.chronofilade();
  const auto lastIndex     = store.segmentedOps().indexOf(current).value();
  EXPECT_EQ(chrono->indexedCount(), 40U);
  EXPECT_EQ(chrono->depth(0), std::optional<std::uint32_t>{0});
  EXPECT_EQ(chrono->depth(lastIndex), std::optional<std::uint32_t>{40});
  EXPECT_FALSE(chrono->depth(41));
  EXPECT_FALSE(chrono->jumpAncestor(41, 0));
  EXPECT_FALSE(chrono->lowestCommonAncestor(41, 41));
  EXPECT_EQ(chrono->jumpAncestor(lastIndex, 40),
            std::optional<std::uint32_t>{0});
  EXPECT_THROW((void)chrono->rebuildVersion(41, store), std::invalid_argument);
  EXPECT_FALSE(chrono->verifyAgainstFullRebuild(41, store, Version{}));
  Version unchanged;
  EXPECT_FALSE(chrono->advance(unchanged, 41, 41, store));
  EXPECT_THROW((void)chrono->composePath(41, 41, store), std::invalid_argument);
}

TEST(ChronofiladeTest, ExplicitZeroBirthIsRejected) {
  Store store;
  const auto version = store.insert(MicroversionId{}, 0, "text");
  const auto index   = store.segmentedOps().indexOf(version).value();

  EXPECT_EQ(store.textOf(version), "text");
  EXPECT_THROW((void)store.rebuild(version, 0U), std::invalid_argument);
  EXPECT_THROW((void)store.textOf(version, 0U), std::invalid_argument);
  auto document = store.rebuild(version);
  EXPECT_THROW((void)store.advanceTo(document, version, version, 0U),
               std::invalid_argument);
  EXPECT_THROW((void)store.verifyAgainstFullRebuild(version, 0U),
               std::invalid_argument);
  ASSERT_NE(store.chronofilade(), nullptr);
  EXPECT_THROW((void)store.chronofilade()->rebuildVersion(index, store, 0U),
               std::invalid_argument);
}

TEST(ChronofiladeTest, RootScopeExcludesNamedAndCellContextAcrossCheckpoints) {
  Store store;
  const auto rootText =
      store.insert(MicroversionId{}, 0, "root bytes", MicroversionId{});
  const auto docBirth   = store.makeXanadoc(rootText, "active document");
  const auto activeText = store.insert(docBirth, 0, "active bytes");
  const auto cellBirthVersion =
      store.makeCell(activeText, "cell seed", docBirth);
  const auto cellBirth = store.segmentedOps().indexOf(cellBirthVersion).value();
  auto head = store.insert(cellBirthVersion, 0, "cell bytes", cellBirthVersion);

  std::string activeExpected = "active bytes";
  for (int i = 0; i < 40; ++i) {
    const auto active = store.rebuild(
        head, xanadu::XanadocSelection::birth(
                  store.segmentedOps().indexOf(docBirth).value()));
    head = store.insert(head, active.length(), "x", docBirth);
    activeExpected.push_back('x');
  }
  const auto activeIndex = store.segmentedOps().indexOf(head).value();

  const auto root = store.chronofilade()->rebuildVersion(activeIndex, store);
  EXPECT_EQ(root.materialize(store), "root bytes");
  EXPECT_EQ(store.rebuild(head).materialize(store), activeExpected);
  EXPECT_EQ(
      store.rebuild(head, xanadu::XanadocSelection::root()).materialize(store),
      "root bytes");
  EXPECT_EQ(store.rebuild(head, xanadu::XanadocSelection::birth(cellBirth))
                .materialize(store),
            "cell bytes");
}

TEST(ChronofiladeTest, AutomaticAdvanceRebuildsWhenActiveStructureChanges) {
  Store store;
  const auto rootText =
      store.insert(MicroversionId{}, 0, "root bytes", MicroversionId{});
  const auto docA  = store.makeXanadoc(rootText, "document A");
  const auto textA = store.insert(docA, 0, "A bytes");
  const auto docB  = store.makeXanadoc(textA, "document B");
  const auto textB = store.insert(docB, 0, "B bytes");

  Version incremental = store.rebuild(textA);
  ASSERT_EQ(incremental.materialize(store), "A bytes");
  ASSERT_TRUE(store.advanceTo(incremental, textA, docB));
  EXPECT_EQ(incremental.materialize(store),
            store.rebuild(docB).materialize(store));
  EXPECT_EQ(incremental.materialize(store), "");

  ASSERT_TRUE(store.advanceTo(incremental, docB, textB));
  EXPECT_EQ(incremental.materialize(store), "B bytes");
  ASSERT_TRUE(store.advanceTo(incremental, textB, textA));
  EXPECT_EQ(incremental.materialize(store), "A bytes");

  Version oneStep = store.rebuild(textA);
  ASSERT_TRUE(store.advance(oneStep, textA, docB));
  EXPECT_EQ(oneStep.materialize(store), "");

  Version rootScope = store.rebuild(textA, xanadu::XanadocSelection::root());
  ASSERT_TRUE(store.advanceTo(rootScope, textA, docB,
                              xanadu::XanadocSelection::root()));
  EXPECT_EQ(rootScope.materialize(store), "root bytes");
}

TEST(ChronofiladeTest, BirthSelectionMustNameAGlobalStructureBirth) {
  Store store;
  const auto version = store.insert(MicroversionId{}, 0, "text");
  EXPECT_THROW((void)store.rebuild(version, xanadu::XanadocSelection::birth(1)),
               std::invalid_argument);
  EXPECT_THROW((void)store.rebuild(
                   version, xanadu::XanadocSelection::birth(
                                std::numeric_limits<std::uint32_t>::max())),
               std::invalid_argument);
}

TEST(ChronofiladeTest, RecordOpRejectsSparseOrderAndUnknownParent) {
  Store store;
  xanadu::enfilade::Chronofilade chrono;
  xanadu::CompactOpNode node{};

  EXPECT_THROW(chrono.recordOp(2, node, MicroversionId{}, store),
               std::invalid_argument);
  node.parentIndex = 1;
  EXPECT_THROW(chrono.recordOp(1, node, MicroversionId{}, store),
               std::invalid_argument);
  node.parentIndex = 0;
  EXPECT_THROW(chrono.recordOp(1, node, MicroversionId{}, store),
               std::invalid_argument);
  EXPECT_EQ(chrono.indexedCount(), 0U);
  EXPECT_FALSE(chrono.depth(1));
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

  const auto idx1   = store.segmentedOps().indexOf(v1).value();
  const auto idx2   = store.segmentedOps().indexOf(v2).value();
  const auto idx3   = store.segmentedOps().indexOf(v3).value();
  const auto idx2a2 = store.segmentedOps().indexOf(v2a2).value();
  const auto idx2b1 = store.segmentedOps().indexOf(v2b1).value();

  // LCA checks
  EXPECT_EQ(chrono->lowestCommonAncestor(idx2a2, idx3), idx2);
  EXPECT_EQ(chrono->lowestCommonAncestor(idx2a2, idx2b1), idx2);
  EXPECT_EQ(chrono->lowestCommonAncestor(idx3, idx1), idx1);
  EXPECT_EQ(chrono->lowestCommonAncestor(idx2a2, idx2a2), idx2a2);
  EXPECT_THROW((void)chrono->composePath(idx2a2, idx2b1, store),
               std::invalid_argument);
  const auto separateRootBranch = store.insert(MicroversionId{}, 0, "Other ");
  const auto separateRootIndex =
      store.segmentedOps().indexOf(separateRootBranch).value();
  EXPECT_EQ(chrono->lowestCommonAncestor(idx1, separateRootIndex),
            std::optional<std::uint32_t>{0});

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

  const auto idx1 = store.segmentedOps().indexOf(v1).value();
  const auto idx3 = store.segmentedOps().indexOf(v3).value();

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

TEST(ChronofiladeTest, ComposeTransclusionUsesTheSourceAutoScope) {
  Store store;
  const auto rootText =
      store.insert(MicroversionId{}, 0, "root text", MicroversionId{});
  const auto docBirth         = store.makeXanadoc(rootText, "document");
  const auto docText          = store.insert(docBirth, 0, "doc data");
  const auto cellBirthVersion = store.makeCell(docText, "cell seed", docBirth);
  const auto cellText =
      store.insert(cellBirthVersion, 0, "cell data", cellBirthVersion);
  const auto docBirthIndex = store.segmentedOps().indexOf(docBirth).value();
  const auto sourceIndex   = store.segmentedOps().indexOf(cellText).value();

  const auto transcluded =
      store.transclude(cellText, store.rebuild(cellText).length(), cellText, 0,
                       store.rebuild(cellText).length());
  const auto targetIndex = store.segmentedOps().indexOf(transcluded).value();
  const auto base =
      store.rebuild(cellText, xanadu::XanadocSelection::birth(docBirthIndex));
  const auto transform = store.chronofilade()->composePath(
      sourceIndex, targetIndex, store, docBirthIndex);
  const auto composed = transform.applyToVersion(base);
  const auto rebuilt  = store.rebuild(
      transcluded, xanadu::XanadocSelection::birth(docBirthIndex));

  EXPECT_EQ(composed.materialize(store), "doc datadoc data");
  EXPECT_EQ(composed.materialize(store), rebuilt.materialize(store));
}

} // namespace

/**
 * @file segmented_ops_spool.cpp
 * @brief Tests for contiguous segmented virtual memory operations spool and
 * tree.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <xudu/core/compact_op.hpp>
#include <xudu/core/microversion.hpp>
#include <xudu/core/segmented_ops_spool.hpp>

namespace {

using xudu::CompactOpNode;
using xudu::MicroversionId;
using xudu::OpKind;
using xudu::PrimediaSpan;
using xudu::SegmentedOpsSpool;

TEST(SegmentedOpsSpoolTest, appendAndTraverseLinearChain) {
  SegmentedOpsSpool spool;

  // Op 1: Insert "hello" -> state 1
  CompactOpNode n1;
  n1.kind = OpKind::Insert;
  n1.at   = 0;
  n1.setSpan(PrimediaSpan{xudu::localScroll, 0, 5});
  const auto id1  = MicroversionId::parse("1");
  const auto idx1 = spool.append(n1, id1);
  EXPECT_EQ(idx1, 1U);

  // Op 2: Insert " world" -> state 2
  CompactOpNode n2;
  n2.kind        = OpKind::Insert;
  n2.parentIndex = idx1;
  n2.at          = 5;
  n2.setSpan(PrimediaSpan{xudu::localScroll, 5, 6});
  const auto id2  = MicroversionId::parse("2");
  const auto idx2 = spool.append(n2, id2);
  EXPECT_EQ(idx2, 2U);

  EXPECT_EQ(spool.size(), 2U);
  EXPECT_EQ(spool.indexOf(id1), 1U);
  EXPECT_EQ(spool.indexOf(id2), 2U);
  EXPECT_EQ(spool.idOf(1U).str(), "1");
  EXPECT_EQ(spool.idOf(2U).str(), "2");

  // Verify ancestral path
  const auto path2 = spool.ancestralPath(idx2);
  EXPECT_EQ(path2, (std::vector<std::uint32_t>{1U, 2U}));

  // Verify pointer stability
  const auto *p1 = spool.get(1U);
  const auto *p2 = spool.get(2U);
  ASSERT_NE(p1, nullptr);
  ASSERT_NE(p2, nullptr);
  EXPECT_EQ(p1->firstChildIndex, 2U);
  EXPECT_EQ(p2->parentIndex, 1U);
}

TEST(SegmentedOpsSpoolTest, branchingTreeTopologyAndSiblings) {
  SegmentedOpsSpool spool;

  // 1
  CompactOpNode n1;
  n1.kind         = OpKind::Insert;
  const auto idx1 = spool.append(n1, MicroversionId::parse("1"));

  // 2 (child 1 of 1)
  CompactOpNode n2;
  n2.parentIndex  = idx1;
  const auto idx2 = spool.append(n2, MicroversionId::parse("2"));

  // 1a1 (child 2 of 1 / branch)
  CompactOpNode n1a1;
  n1a1.parentIndex   = idx1;
  n1a1.branchOrdinal = 1;
  const auto idx1a1  = spool.append(n1a1, MicroversionId::parse("1a1"));

  // 1b1 (child 3 of 1 / branch)
  CompactOpNode n1b1;
  n1b1.parentIndex   = idx1;
  n1b1.branchOrdinal = 2;
  const auto idx1b1  = spool.append(n1b1, MicroversionId::parse("1b1"));

  // Children of 1 should be [2, 1a1, 1b1]
  const auto childrenOf1 = spool.childrenOf(idx1);
  EXPECT_EQ(childrenOf1, (std::vector<std::uint32_t>{idx2, idx1a1, idx1b1}));

  // Ancestral path of 1a1 should be [1, 1a1]
  const auto path1a1 = spool.ancestralPath(idx1a1);
  EXPECT_EQ(path1a1, (std::vector<std::uint32_t>{idx1, idx1a1}));

  // Ancestral path of 1b1 should be [1, 1b1]
  const auto path1b1 = spool.ancestralPath(idx1b1);
  EXPECT_EQ(path1b1, (std::vector<std::uint32_t>{idx1, idx1b1}));
}

TEST(SegmentedOpsSpoolTest, pointerStabilityUnderGrowth) {
  SegmentedOpsSpool spool;

  // Record 1000 operations
  std::vector<const CompactOpNode *> recordedPointers;
  for (std::uint32_t i = 1; i <= 1000; i++) {
    CompactOpNode node;
    node.parentIndex = i - 1;
    node.at          = i;
    const auto id    = MicroversionId::parse(std::to_string(i));
    const auto idx   = spool.append(node, id);
    recordedPointers.push_back(spool.get(idx));
  }

  // Verify all pointers remain valid and unchanged
  for (std::uint32_t i = 1; i <= 1000; i++) {
    const auto *actual = spool.get(i);
    EXPECT_EQ(actual, recordedPointers[i - 1]);
    EXPECT_EQ(actual->at, i);
  }
}

// -- segments on disk --------------------------------------------------------
//
// A segment file is a bare run of CompactOpNodes: no header, no state-zero
// slot, and no microversion names anywhere in it. The names come back out of
// the tree, each node saying which index produced it and by which branch
// ordinal, which is what these cover.

namespace {

std::filesystem::path scratchDir(const std::string &name) {
  const auto dir =
      std::filesystem::temp_directory_path() / ("xudu_seg_" + name);
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

/// A chain of @p count operations appended to @p spool, continuing from
/// whatever it already holds.
void appendChain(SegmentedOpsSpool &spool, const std::uint32_t count) {
  for (std::uint32_t i = 0; i < count; i++) {
    const auto index = static_cast<std::uint32_t>(spool.size()) + 1U;
    CompactOpNode node;
    node.kind          = OpKind::Insert;
    node.parentIndex   = index - 1U;
    node.branchOrdinal = 0;
    node.at            = index;
    spool.append(node, MicroversionId::parse(std::to_string(index)));
  }
}

} // namespace

TEST(SegmentedOpsSpoolTest, appendedOpsReachTheActiveSegmentFile) {
  const auto dir    = scratchDir("active");
  const auto active = dir / "active.ops";

  SegmentedOpsSpool spool;
  ASSERT_TRUE(spool.openActiveSegment(active));
  appendChain(spool, 10);
  // Nothing is on disk until it is asked for: appending is a write to memory.
  ASSERT_TRUE(spool.flush());
  EXPECT_EQ(std::filesystem::file_size(active), 10 * sizeof(CompactOpNode));

  // Flushing again writes nothing further -- only the tail is ever written.
  ASSERT_TRUE(spool.flush());
  EXPECT_EQ(std::filesystem::file_size(active), 10 * sizeof(CompactOpNode));

  appendChain(spool, 5);
  ASSERT_TRUE(spool.flush());
  EXPECT_EQ(std::filesystem::file_size(active), 15 * sizeof(CompactOpNode));
}

TEST(SegmentedOpsSpoolTest, anActiveSegmentIsPickedUpWhereItWasLeft) {
  const auto dir    = scratchDir("reopen");
  const auto active = dir / "active.ops";
  {
    SegmentedOpsSpool spool;
    ASSERT_TRUE(spool.openActiveSegment(active));
    appendChain(spool, 6);
    ASSERT_TRUE(spool.flush());
  }

  SegmentedOpsSpool reopened;
  ASSERT_TRUE(reopened.openActiveSegment(active));
  EXPECT_EQ(reopened.size(), 6U);
  // The names were never written down; they come back out of the tree.
  for (std::uint32_t i = 1; i <= 6; i++) {
    const auto id = MicroversionId::parse(std::to_string(i));
    EXPECT_TRUE(reopened.contains(id)) << "state " << i << " went missing";
    EXPECT_EQ(reopened.indexOf(id), i);
    EXPECT_EQ(reopened.idOf(i).str(), id.str());
  }
  // and it keeps growing from there rather than starting over
  appendChain(reopened, 2);
  EXPECT_EQ(reopened.size(), 8U);
  EXPECT_TRUE(reopened.contains(MicroversionId::parse("8")));
}

TEST(SegmentedOpsSpoolTest, sealedSegmentsAreFoundByTheStateTheyProduce) {
  const auto dir = scratchDir("sealed");
  {
    SegmentedOpsSpool writer;
    ASSERT_TRUE(writer.openActiveSegment(dir / "seg0.ops"));
    appendChain(writer, 4);
    ASSERT_TRUE(writer.flush());
  }

  SegmentedOpsSpool spool;
  ASSERT_TRUE(spool.addSealedSegment(dir / "seg0.ops"));
  EXPECT_EQ(spool.size(), 4U);
  ASSERT_EQ(spool.segments().size(), 1U);
  EXPECT_EQ(spool.segments().front().startOpIndex, 1U);
  EXPECT_EQ(spool.segments().front().opCount, 4U);

  for (std::uint32_t i = 1; i <= 4; i++) {
    const auto id = MicroversionId::parse(std::to_string(i));
    EXPECT_TRUE(spool.contains(id)) << "sealed state " << i << " is invisible";
    EXPECT_EQ(spool.indexOf(id), i);
    ASSERT_NE(spool.get(id), nullptr);
    EXPECT_EQ(spool.get(id)->at, i);
  }
  // The ancestral walk has to cross into the sealed range like any other.
  EXPECT_THAT(spool.ancestralPath(4), testing::ElementsAre(1U, 2U, 3U, 4U));
}

TEST(SegmentedOpsSpoolTest, sealingKeepsTheOperationsItAlreadyHas) {
  // Sealing renames a range; it must not read it back in and file every
  // operation a second time.
  const auto dir = scratchDir("seal");
  SegmentedOpsSpool spool;
  ASSERT_TRUE(spool.openActiveSegment(dir / "seg0.ops"));
  appendChain(spool, 5);

  ASSERT_TRUE(spool.sealActive(dir / "seg1.ops"));
  EXPECT_EQ(spool.size(), 5U) << "sealing duplicated the operations";
  ASSERT_EQ(spool.segments().size(), 1U);
  EXPECT_EQ(spool.segments().front().opCount, 5U);

  // Appending continues after the sealed range, into the new active segment.
  appendChain(spool, 3);
  EXPECT_EQ(spool.size(), 8U);
  ASSERT_TRUE(spool.flush());
  EXPECT_EQ(std::filesystem::file_size(dir / "seg0.ops"),
            5 * sizeof(CompactOpNode));
  EXPECT_EQ(std::filesystem::file_size(dir / "seg1.ops"),
            3 * sizeof(CompactOpNode));

  for (std::uint32_t i = 1; i <= 8; i++) {
    EXPECT_TRUE(spool.contains(MicroversionId::parse(std::to_string(i))))
        << "state " << i << " lost across the seal";
  }
}

TEST(SegmentedOpsSpoolTest, branchesSurviveBeingSealedAndReopened) {
  const auto dir = scratchDir("branches");
  {
    SegmentedOpsSpool writer;
    ASSERT_TRUE(writer.openActiveSegment(dir / "seg0.ops"));
    appendChain(writer, 3); // states 1, 2, 3
    // A branch off state 1, then a continuation of that branch: the case a
    // branch ordinal exists for, and the one a name cannot be guessed from
    // position alone.
    CompactOpNode branched;
    branched.parentIndex   = 1;
    branched.branchOrdinal = 1;
    branched.at            = 100;
    writer.append(branched, MicroversionId::parse("1a1"));
    CompactOpNode onward;
    onward.parentIndex   = 4;
    onward.branchOrdinal = 0;
    onward.at            = 101;
    writer.append(onward, MicroversionId::parse("1a2"));
    ASSERT_TRUE(writer.flush());
  }

  SegmentedOpsSpool spool;
  ASSERT_TRUE(spool.addSealedSegment(dir / "seg0.ops"));
  EXPECT_EQ(spool.size(), 5U);
  for (const auto *name : {"1", "2", "3", "1a1", "1a2"}) {
    EXPECT_TRUE(spool.contains(MicroversionId::parse(name)))
        << name << " did not come back";
  }
  // 1a2 continues 1a1 -- ordinal zero, despite its own last segment saying
  // branch a. Deriving that from the segment letter rather than from the step
  // is the way to get this wrong.
  EXPECT_EQ(spool.idOf(5).str(), "1a2");
  EXPECT_THAT(spool.ancestralPath(5), testing::ElementsAre(1U, 4U, 5U));
}

TEST(SegmentedOpsSpoolTest, aSegmentThatDoesNotFitIsRefused) {
  const auto dir = scratchDir("refuse");

  // Not a whole number of nodes.
  {
    std::ofstream out(dir / "ragged.ops", std::ios::binary);
    const std::string junk(sizeof(CompactOpNode) + 7, '\0');
    out.write(junk.data(), static_cast<std::streamsize>(junk.size()));
  }
  SegmentedOpsSpool spool;
  EXPECT_FALSE(spool.addSealedSegment(dir / "ragged.ops"));
  EXPECT_EQ(spool.size(), 0U);

  // Two nodes claiming the same parent and ordinal name one state twice.
  {
    std::vector<CompactOpNode> nodes(2);
    nodes[0].parentIndex = 0;
    nodes[1].parentIndex = 0;
    std::ofstream out(dir / "twins.ops", std::ios::binary);
    out.write(
        reinterpret_cast<const char *>(nodes.data()),
        static_cast<std::streamsize>(nodes.size() * sizeof(CompactOpNode)));
  }
  EXPECT_FALSE(spool.addSealedSegment(dir / "twins.ops"));
  EXPECT_EQ(spool.size(), 0U) << "a refused segment must leave nothing behind";

  EXPECT_FALSE(spool.addSealedSegment(dir / "no-such-file.ops"));
}

} // namespace

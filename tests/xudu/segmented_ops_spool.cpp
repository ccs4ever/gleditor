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

} // namespace

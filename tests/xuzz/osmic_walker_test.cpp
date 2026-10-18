/**
 * @file osmic_walker_test.cpp
 * @brief Unit tests for OsmicWalker, OsmicVisitor, and OsmicFolder concepts.
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/xanadu/compact_op.hpp"
#include "common/xanadu/osmic_walker.hpp"
#include "common/xanadu/segmented_ops_spool.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/version.hpp"

namespace {

using xanadu::CompactOpNode;
using xanadu::MicroversionId;
using xanadu::OpKind;
using xanadu::OsmicFolder;
using xanadu::OsmicVisitor;
using xanadu::OsmicWalker;
using xanadu::SegmentedOpsSpool;
using xanadu::Store;
using xanadu::Version;

// 1. Concept Compile-Time Verification
struct DummyVisitor {
  void operator()(std::uint32_t, const CompactOpNode &) const {}
};
struct DummyBoolVisitor {
  bool operator()(std::uint32_t, const CompactOpNode &) const { return true; }
};
struct DummyFolder {
  void operator()(std::string &, std::uint32_t, const CompactOpNode &) const {}
};

static_assert(OsmicVisitor<DummyVisitor>);
static_assert(OsmicVisitor<DummyBoolVisitor>);
static_assert(
    OsmicVisitor<decltype([](std::uint32_t, const CompactOpNode &) {})>);
static_assert(OsmicVisitor<decltype([](std::uint32_t, const CompactOpNode &) {
  return true;
})>);
static_assert(OsmicFolder<DummyFolder, std::string>);

TEST(OsmicWalkerTest, LinearChainTraversal) {
  Store store;
  MicroversionId current{};

  for (int i = 0; i < 10; ++i) {
    const auto text = "step" + std::to_string(i) + " ";
    current = store.insert(current, store.rebuild(current).length(), text);
  }

  const auto targetIdx = store.segmentedOps().indexOf(current);
  ASSERT_EQ(targetIdx, 10U);

  // Walk full ancestral path from 0 to 10
  std::vector<std::uint32_t> visited;
  const bool ok = OsmicWalker::walkAncestral(
      store.segmentedOps(), targetIdx,
      [&visited](std::uint32_t idx, const CompactOpNode &) {
        visited.push_back(idx);
      });

  EXPECT_TRUE(ok);
  ASSERT_EQ(visited.size(), 10U);
  for (std::uint32_t i = 0; i < 10; ++i) {
    EXPECT_EQ(visited[i], i + 1U);
  }

  // Compare with spool's legacy ancestralPath
  const auto legacyPath = store.segmentedOps().ancestralPath(targetIdx);
  EXPECT_EQ(visited, legacyPath);
}

TEST(OsmicWalkerTest, SubpathTraversal) {
  Store store;
  MicroversionId current{};
  std::vector<std::uint32_t> indices;

  for (int i = 0; i < 10; ++i) {
    const auto text = "word" + std::to_string(i) + " ";
    current = store.insert(current, store.rebuild(current).length(), text);
    indices.push_back(store.segmentedOps().indexOf(current));
  }

  // Walk subpath from index 3 to index 7 (should visit 4, 5, 6, 7)
  std::vector<std::uint32_t> subVisited;
  const bool ok = OsmicWalker::walkPath(
      store.segmentedOps(), indices[2], indices[6],
      [&subVisited](std::uint32_t idx, const CompactOpNode &) {
        subVisited.push_back(idx);
      });

  EXPECT_TRUE(ok);
  ASSERT_EQ(subVisited.size(), 4U);
  EXPECT_EQ(subVisited[0], indices[3]);
  EXPECT_EQ(subVisited[1], indices[4]);
  EXPECT_EQ(subVisited[2], indices[5]);
  EXPECT_EQ(subVisited[3], indices[6]);
}

TEST(OsmicWalkerTest, EarlyTermination) {
  Store store;
  MicroversionId current{};

  for (int i = 0; i < 10; ++i) {
    current = store.insert(current, store.rebuild(current).length(), "x ");
  }

  const auto targetIdx = store.segmentedOps().indexOf(current);
  std::vector<std::uint32_t> visited;

  // Stop after visiting 3 ops
  const bool finished = OsmicWalker::walkAncestral(
      store.segmentedOps(), targetIdx,
      [&visited](std::uint32_t idx, const CompactOpNode &) -> bool {
        visited.push_back(idx);
        return visited.size() < 3;
      });

  EXPECT_FALSE(finished); // indicates early stop
  EXPECT_EQ(visited.size(), 3U);
  EXPECT_EQ(visited[0], 1U);
  EXPECT_EQ(visited[1], 2U);
  EXPECT_EQ(visited[2], 3U);
}

TEST(OsmicWalkerTest, DivergentBranchHandling) {
  Store store;
  const auto root = store.insert(MicroversionId{}, 0, "Root ");
  const auto b1 = store.insert(root, store.rebuild(root).length(), "Branch1 ");
  const auto b2 = store.insert(root, store.rebuild(root).length(), "Branch2 ");

  const auto idxB1 = store.segmentedOps().indexOf(b1);
  const auto idxB2 = store.segmentedOps().indexOf(b2);

  // b1 is not an ancestor of b2
  std::vector<std::uint32_t> visited;
  const bool ok = OsmicWalker::walkPath(
      store.segmentedOps(), idxB1, idxB2,
      [&visited](std::uint32_t idx, const CompactOpNode &) {
        visited.push_back(idx);
      });

  EXPECT_FALSE(ok);
  EXPECT_TRUE(visited.empty());
  EXPECT_FALSE(OsmicWalker::isAncestor(store.segmentedOps(), idxB1, idxB2));
  EXPECT_TRUE(OsmicWalker::isAncestor(
      store.segmentedOps(), store.segmentedOps().indexOf(root), idxB2));
}

TEST(OsmicWalkerTest, StackBufferOverflowToHeapBuffer) {
  Store store;
  MicroversionId current{};

  // Create 80 operations to exceed StackBufferSize (64)
  constexpr std::size_t kNumOps = 80;
  for (std::size_t i = 0; i < kNumOps; ++i) {
    current = store.insert(current, store.rebuild(current).length(), "a");
  }

  const auto targetIdx = store.segmentedOps().indexOf(current);
  EXPECT_EQ(targetIdx, kNumOps);

  std::vector<std::uint32_t> visited;
  const bool ok = OsmicWalker::walkAncestral(
      store.segmentedOps(), targetIdx,
      [&visited](std::uint32_t idx, const CompactOpNode &) {
        visited.push_back(idx);
      });

  EXPECT_TRUE(ok);
  ASSERT_EQ(visited.size(), kNumOps);
  for (std::size_t i = 0; i < kNumOps; ++i) {
    EXPECT_EQ(visited[i], static_cast<std::uint32_t>(i + 1));
  }
}

TEST(OsmicWalkerTest, FoldAccumulation) {
  Store store;
  MicroversionId current{};

  const std::string words[] = {"Alpha ", "Beta ", "Gamma ", "Delta "};
  for (const auto &w : words) {
    current = store.insert(current, store.rebuild(current).length(), w);
  }

  const auto targetIdx = store.segmentedOps().indexOf(current);

  // Fold using OsmicWalker::foldAncestral
  std::uint32_t totalInserts = 0;
  std::uint32_t totalBytes   = 0;
  const bool ok              = OsmicWalker::foldAncestral(
      store.segmentedOps(), targetIdx, totalBytes,
      [&totalInserts](std::uint32_t &bytes, std::uint32_t,
                      const CompactOpNode &node) {
        if (OpKind::Insert == node.kind) {
          ++totalInserts;
          bytes += static_cast<std::uint32_t>(node.spanLength);
        }
      });

  EXPECT_TRUE(ok);
  EXPECT_EQ(totalInserts, 4U);
  EXPECT_EQ(totalBytes, store.rebuild(current).length());
}

} // namespace

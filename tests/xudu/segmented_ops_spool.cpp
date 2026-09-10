/**
 * @file segmented_ops_spool.cpp
 * @brief Tests for contiguous segmented virtual memory operations spool and
 * tree.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <xudu/core/compact_op.hpp>
#include <xudu/core/microversion.hpp>
#include <xudu/core/segmented_ops_spool.hpp>
#include <xudu/core/virtual_memory_arena.hpp>

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
  EXPECT_EQ(p2->parentIndex, 1U);
  // The downward edge is not in the node -- it is derived from parentIndex and
  // held beside it, so that a stored node is never written to again.
  EXPECT_EQ(spool.childrenOf(1U), (std::vector<std::uint32_t>{2U}));
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

// -- the reservation ceiling -------------------------------------------------

TEST(SegmentedOpsSpoolTest, aSpoolCountsDownToItsCeilingAndThenRefuses) {
  // 64 KiB is 1024 nodes, so the ceiling is reachable in a test. It is the
  // same arithmetic the 8 GiB default runs, which is why the numbers below
  // are read off opCapacity() rather than written out.
  SegmentedOpsSpool spool(64U * 1024U);
  const auto capacity = spool.opCapacity();
  ASSERT_GT(capacity, 0U);
  ASSERT_LT(capacity, 4096U) << "a test must not have to fill 8 GiB to finish";
  EXPECT_EQ(spool.opCapacityRemaining(), capacity);

  for (std::uint32_t i = 1; i <= capacity; i++) {
    CompactOpNode node;
    node.parentIndex = i - 1U;
    node.at          = i;
    ASSERT_EQ(spool.append(node, MicroversionId::parse(std::to_string(i))), i);
    ASSERT_EQ(spool.opCapacityRemaining(), capacity - i);
  }

  CompactOpNode refused;
  refused.parentIndex = capacity;
  const auto name     = MicroversionId::parse(std::to_string(capacity + 1U));
  try {
    spool.append(refused, name);
    FAIL() << "an operation past the reservation must not be recorded";
  } catch (const xudu::SpoolExhausted &e) {
    // The numbers a caller needs to say something useful about it, rather
    // than the bare std::bad_alloc the arena would otherwise have raised.
    EXPECT_EQ(e.held(), capacity);
    EXPECT_EQ(e.capacity(), capacity);
  }

  // Nothing was half-recorded: the refused operation has no name, no index and
  // no place in the tree, so the document is still exactly the one it was.
  EXPECT_EQ(spool.size(), capacity);
  EXPECT_FALSE(spool.contains(name));
  EXPECT_EQ(spool.indexOf(name), 0U);
  EXPECT_TRUE(spool.childrenOf(capacity).empty());
  EXPECT_EQ(spool.opCapacityRemaining(), 0U);
}

#if defined(__linux__)
/// Bytes of resident memory this process holds, from the second field of
/// /proc/self/statm. Reserved-but-uncommitted address space is not counted in
/// it, which is the whole of what the test below wants to know.
std::size_t residentBytes() {
  std::ifstream statm("/proc/self/statm");
  std::size_t totalPages    = 0;
  std::size_t residentPages = 0;
  statm >> totalPages >> residentPages;
  return residentPages * xudu::VirtualMemoryArena::pageSize();
}
#endif

TEST(SegmentedOpsSpoolTest, theDefaultReservationIsAddressSpaceNotMemory) {
  // The arena maps its reservation PROT_NONE, so a spool holding no
  // operations costs no pages however large its ceiling is. Four of them is
  // 32 GiB of address space on a 64-bit machine: an implementation that
  // committed any of it would be impossible to miss here.
  constexpr std::size_t spoolCount = 4;
#if defined(__linux__)
  const auto before = residentBytes();
#endif
  std::vector<SegmentedOpsSpool> spools;
  spools.reserve(spoolCount);
  for (std::size_t i = 0; i < spoolCount; i++) {
    spools.emplace_back();
  }
#if defined(__linux__)
  EXPECT_LT(residentBytes(), before + (8U * 1024U * 1024U))
      << "reserving the ops arenas committed memory rather than address space";
#endif

  for (const auto &spool : spools) {
    EXPECT_EQ(spool.opCapacityRemaining(), spool.opCapacity());
    // Whatever the reservation ladder settled on, a spool never opens with
    // less room than the 512 MiB ceiling it used to have.
    EXPECT_GE(spool.opCapacity(),
              xudu::minOpsReservation / sizeof(CompactOpNode) - 1U);
  }
  if constexpr (sizeof(void *) >= 8) {
    // The ruling in design step 5, stated as the thing it is for: a document
    // can be edited a hundred million times before the spool is the limit.
    EXPECT_GE(spools.front().opCapacity(), 100'000'000U);
  }
}

// -- segments on disk --------------------------------------------------------
//
// A segment file is an OpsSegmentHeader and then a run of CompactOpNodes: no
// state-zero slot and no microversion names anywhere in it. The names come
// back out of the tree, each node saying which index produced it and by which
// branch ordinal, which is what these cover.

namespace {

/// The bytes a segment file of @p nodes starting at @p firstOpIndex is made
/// of, written out by hand.
///
/// Duplicating what SegmentedOpsSpool writes is the point: a test that put its
/// nodes there through the spool could not put them at an index the spool did
/// not choose, and could not write a header that is wrong on purpose.
void writeSegmentFileByHand(
    const std::filesystem::path &path, const std::vector<CompactOpNode> &nodes,
    const std::uint32_t firstOpIndex,
    const std::optional<xudu::OpsSegmentHeader> &deliberatelyWrong = {}) {
  xudu::OpsSegmentHeader header;
  if (deliberatelyWrong.has_value()) {
    header = *deliberatelyWrong;
  } else {
    header.signature     = xudu::opsSegmentSignature;
    header.formatVersion = xudu::opsSegmentFormatVersion;
    header.headerBytes   = xudu::opsSegmentHeaderBytes;
    header.nodeSize      = sizeof(CompactOpNode);
    header.firstOpIndex  = firstOpIndex;
    header.nodeCount     = nodes.size();
  }
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(&header), sizeof(header));
    // Never written, so the rest of the header block is a hole.
    out.seekp(xudu::opsSegmentHeaderBytes);
    if (!nodes.empty()) {
      out.write(
          reinterpret_cast<const char *>(nodes.data()),
          static_cast<std::streamsize>(nodes.size() * sizeof(CompactOpNode)));
    }
  }
  std::filesystem::resize_file(path, xudu::opsSegmentHeaderBytes +
                                         nodes.size() * sizeof(CompactOpNode));
}

/// What a segment file holding @p nodeCount operations is on disk.
std::uintmax_t segmentFileSize(const std::uintmax_t nodeCount) {
  return xudu::opsSegmentHeaderBytes + nodeCount * sizeof(CompactOpNode);
}

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
  // Opening one writes its header and nothing else, so the file says what it
  // is from the moment it exists rather than from its first flush.
  EXPECT_EQ(std::filesystem::file_size(active), segmentFileSize(0));

  appendChain(spool, 10);
  // No operation is on disk until it is asked for: appending is a write to
  // memory.
  EXPECT_EQ(std::filesystem::file_size(active), segmentFileSize(0));
  ASSERT_TRUE(spool.flush());
  EXPECT_EQ(std::filesystem::file_size(active), segmentFileSize(10));

  // Flushing again writes nothing further -- only the tail is ever written.
  ASSERT_TRUE(spool.flush());
  EXPECT_EQ(std::filesystem::file_size(active), segmentFileSize(10));

  appendChain(spool, 5);
  ASSERT_TRUE(spool.flush());
  EXPECT_EQ(std::filesystem::file_size(active), segmentFileSize(15));
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
  EXPECT_EQ(std::filesystem::file_size(dir / "seg0.ops"), segmentFileSize(5));
  EXPECT_EQ(std::filesystem::file_size(dir / "seg1.ops"), segmentFileSize(3));

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

#if defined(__linux__)
/// The permission field /proc/self/maps gives for the mapping covering @p addr,
/// or an empty string if no mapping covers it. There is no portable way to ask
/// whether a page is writable without writing to it and finding out.
std::string mappingPermsFor(const void *addr) {
  const auto want = reinterpret_cast<std::uintptr_t>(addr);
  std::ifstream maps("/proc/self/maps");
  std::string line;
  while (std::getline(maps, line)) {
    const auto dash  = line.find('-');
    const auto space = line.find(' ');
    if (dash == std::string::npos || space == std::string::npos) {
      continue;
    }
    const auto lo = std::stoull(line.substr(0, dash), nullptr, 16);
    const auto hi =
        std::stoull(line.substr(dash + 1, space - dash - 1), nullptr, 16);
    if (want >= lo && want < hi) {
      // The three access bits only. The fourth character is the sharing mode,
      // which is 's' here because the segment is mapped from a file, and says
      // nothing about whether a write would land.
      return line.substr(space + 1, 3);
    }
  }
  return {};
}
#endif

TEST(SegmentedOpsSpoolTest, appendingUnderASealedParentDoesNotWriteIntoIt) {
  // A sealed segment that starts and ends on a page boundary is mapped
  // PROT_READ rather than copied, so anything that writes into one of its
  // nodes takes SIGSEGV. Appending a child used to do exactly that: the child
  // and sibling edges lived inside CompactOpNode, so filing a new operation
  // under a parent wrote to the parent. Every other segment test here seals
  // four or five nodes, which never lands on a page boundary and so never gets
  // mapped -- which is why this went unnoticed. See design R10.
  const auto perPage = static_cast<std::uint32_t>(
      xudu::VirtualMemoryArena::pageSize() / sizeof(CompactOpNode));
  ASSERT_GT(perPage, 8U) << "a page must hold enough nodes to branch inside";

  const auto dir = scratchDir("sealed_readonly");
  // One page of nodes continuing the chain the spool will already hold, so
  // they occupy indices [perPage, 2 * perPage).
  {
    std::vector<CompactOpNode> nodes(perPage);
    for (std::uint32_t i = 0; i < perPage; i++) {
      nodes[i].kind        = OpKind::Insert;
      nodes[i].parentIndex = perPage - 1U + i;
      nodes[i].at          = perPage + i;
    }
    // Starting at perPage, which is where the spool below will be up to.
    writeSegmentFileByHand(dir / "sealed.ops", nodes, perPage);
  }

  SegmentedOpsSpool spool;
  // Index 0 is the state-zero slot, so holding perPage - 1 operations puts the
  // next one exactly one page in. This is the whole point of the setup.
  appendChain(spool, perPage - 1U);
  ASSERT_EQ(spool.size(), perPage - 1U);
  ASSERT_TRUE(spool.addSealedSegment(dir / "sealed.ops"));
  ASSERT_EQ(spool.size(), 2U * perPage - 1U);

  const auto *const sealedBase = spool.rawOps() + perPage;
#if defined(__linux__)
  // Without this the test would pass vacuously if the mapping never happened.
  EXPECT_EQ(mappingPermsFor(sealedBase), "r--")
      << "the sealed range was not mapped read-only, so this test proves "
         "nothing about writing into it";
#endif

  // A continuation of the last sealed node, which has no children yet: the
  // case that used to write firstChildIndex into the parent.
  const auto lastSealed = 2U * perPage - 1U;
  CompactOpNode onward;
  onward.parentIndex = lastSealed;
  onward.at          = 9001;
  const auto onwardIdx =
      spool.append(onward, MicroversionId::parse(std::to_string(2U * perPage)));

  // A branch off a sealed node that already has a child, which used to walk
  // into the sealed range and write nextSiblingIndex into the sibling.
  const auto forkAt = perPage + 5U;
  CompactOpNode branched;
  branched.parentIndex   = forkAt;
  branched.branchOrdinal = 1;
  branched.at            = 9002;
  const auto branchIdx   = spool.append(
      branched, MicroversionId::parse(std::to_string(forkAt) + "a1"));

  EXPECT_EQ(spool.childrenOf(lastSealed),
            (std::vector<std::uint32_t>{onwardIdx}));
  EXPECT_EQ(spool.childrenOf(forkAt),
            (std::vector<std::uint32_t>{forkAt + 1U, branchIdx}));
  EXPECT_THAT(spool.ancestralPath(branchIdx),
              testing::Contains(forkAt).Times(1));

  // The sealed nodes still say exactly what the file said.
  for (std::uint32_t i = 0; i < perPage; i++) {
    const auto *const node = spool.get(perPage + i);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->at, perPage + i);
    EXPECT_EQ(node->parentIndex, perPage - 1U + i);
  }
}

TEST(SegmentedOpsSpoolTest, aSegmentThatDoesNotFitIsRefused) {
  // The quiet half of the refusal. These files are perfectly good segments;
  // they just do not belong where they are being offered, which is a fact
  // about the order things were loaded in rather than about the file. So they
  // come back false, where a file that is not a segment at all throws -- see
  // the tests below.
  const auto dir = scratchDir("refuse");

  // Two nodes claiming the same parent and ordinal name one state twice.
  {
    std::vector<CompactOpNode> nodes(2);
    nodes[0].parentIndex = 0;
    nodes[1].parentIndex = 0;
    writeSegmentFileByHand(dir / "twins.ops", nodes, 1);
  }
  SegmentedOpsSpool spool;
  EXPECT_FALSE(spool.addSealedSegment(dir / "twins.ops"));
  EXPECT_EQ(spool.size(), 0U) << "a refused segment must leave nothing behind";

  // A segment whose operations start further along than this spool has got to.
  // Loading it here would put its nodes at indices they were not written for,
  // and every parentIndex in it would then name the wrong operation.
  {
    std::vector<CompactOpNode> nodes(2);
    nodes[0].parentIndex = 40;
    nodes[1].parentIndex = 41;
    writeSegmentFileByHand(dir / "later.ops", nodes, 41);
  }
  EXPECT_FALSE(spool.addSealedSegment(dir / "later.ops"))
      << "a segment that begins at operation 41 does not belong at 1";
  EXPECT_EQ(spool.size(), 0U);

  EXPECT_FALSE(spool.addSealedSegment(dir / "no-such-file.ops"));
}

// -- the header, and what it refuses -----------------------------------------
//
// Migration step 1 changed CompactOpNode's layout and every sample store on
// disk went on loading, meaning something else -- an operation read as a
// Rearrange of zero bytes, fifteen tests quietly asserting on the empty
// string. There was nothing to refuse them with. These are that nothing,
// filled in: see design R14.

TEST(SegmentedOpsSpoolTest, aSegmentWrittenBeforeHeadersExistedIsRefused) {
  // Byte for byte what the previous commit wrote: a bare run of nodes,
  // starting with the root operation whose parentIndex is zero. That leading
  // 00 00 00 00 is why the signature cannot be mistaken for one -- and why
  // this file used to load.
  const auto dir  = scratchDir("headerless");
  const auto path = dir / "ops.nodes";
  {
    std::vector<CompactOpNode> nodes(3);
    for (std::uint32_t i = 0; i < nodes.size(); i++) {
      nodes[i].kind        = OpKind::Insert;
      nodes[i].parentIndex = i;
      nodes[i].length      = 1075; // what step 1's first sample really carried
    }
    std::ofstream out(path, std::ios::binary);
    out.write(
        reinterpret_cast<const char *>(nodes.data()),
        static_cast<std::streamsize>(nodes.size() * sizeof(CompactOpNode)));
  }

  SegmentedOpsSpool spool;
  try {
    static_cast<void>(spool.addSealedSegment(path));
    FAIL() << "a file written before headers existed must not be read as one";
  } catch (const xudu::OpsSegmentUnreadable &e) {
    // Naming the signature is the point: the message has to say what kind of
    // file this is not, or the next person reads it as an I/O error.
    EXPECT_THAT(std::string{e.what()}, testing::HasSubstr("signature"));
    EXPECT_THAT(std::string{e.what()}, testing::HasSubstr("XUDUOPS"));
    EXPECT_THAT(std::string{e.what()}, testing::HasSubstr("ops.nodes"));
  }
  EXPECT_EQ(spool.size(), 0U) << "nothing may be read out of a refused file";

  // And the same file offered as an active segment, which is the path
  // Store::load() actually takes.
  SegmentedOpsSpool opening;
  EXPECT_THROW(static_cast<void>(opening.openActiveSegment(path)),
               xudu::OpsSegmentUnreadable);
}

TEST(SegmentedOpsSpoolTest, aSegmentWhoseNodesAreADifferentSizeIsRefused) {
  // The step-1 failure itself, caught. A file whose operations are not the
  // size this build compiles cannot be read as operations, however well
  // everything else about it checks out.
  const auto dir = scratchDir("nodesize");

  std::vector<CompactOpNode> nodes(2);
  nodes[1].parentIndex = 1;

  xudu::OpsSegmentHeader wrongSize;
  wrongSize.signature     = xudu::opsSegmentSignature;
  wrongSize.formatVersion = xudu::opsSegmentFormatVersion;
  wrongSize.headerBytes   = xudu::opsSegmentHeaderBytes;
  wrongSize.nodeSize      = sizeof(CompactOpNode) + 8;
  wrongSize.firstOpIndex  = 1;
  wrongSize.nodeCount     = nodes.size();
  writeSegmentFileByHand(dir / "wide.ops", nodes, 1, wrongSize);

  SegmentedOpsSpool spool;
  try {
    static_cast<void>(spool.addSealedSegment(dir / "wide.ops"));
    FAIL() << "operations of another size must not be read as these ones";
  } catch (const xudu::OpsSegmentUnreadable &e) {
    // Both numbers, because "this file says 72 and mine are 64" is what makes
    // it obvious what happened, where "cannot read" does not.
    EXPECT_THAT(std::string{e.what()},
                testing::HasSubstr(std::to_string(sizeof(CompactOpNode) + 8)));
    EXPECT_THAT(std::string{e.what()},
                testing::HasSubstr(std::to_string(sizeof(CompactOpNode))));
  }
  EXPECT_EQ(spool.size(), 0U);
}

TEST(SegmentedOpsSpoolTest, aHeaderThisBuildDoesNotUnderstandIsRefused) {
  const auto dir = scratchDir("unknown");
  std::vector<CompactOpNode> nodes(1);

  // A version from the future. Reading it would be guessing.
  xudu::OpsSegmentHeader later;
  later.signature     = xudu::opsSegmentSignature;
  later.formatVersion = xudu::opsSegmentFormatVersion + 1;
  later.headerBytes   = xudu::opsSegmentHeaderBytes;
  later.nodeSize      = sizeof(CompactOpNode);
  later.firstOpIndex  = 1;
  later.nodeCount     = nodes.size();
  writeSegmentFileByHand(dir / "future.ops", nodes, 1, later);

  SegmentedOpsSpool spool;
  EXPECT_THROW(static_cast<void>(spool.addSealedSegment(dir / "future.ops")),
               xudu::OpsSegmentUnreadable);

  // A file cut short mid-operation. The nodes that are left cannot be trusted
  // to be the ones that were written.
  writeSegmentFileByHand(dir / "ragged.ops", nodes, 1);
  std::filesystem::resize_file(
      dir / "ragged.ops", std::filesystem::file_size(dir / "ragged.ops") - 7);
  EXPECT_THROW(static_cast<void>(spool.addSealedSegment(dir / "ragged.ops")),
               xudu::OpsSegmentUnreadable);

  // A file whose header promises more operations than are in it: bytes went
  // missing, rather than a flush that had not caught up.
  std::vector<CompactOpNode> two(2);
  two[1].parentIndex = 1;
  xudu::OpsSegmentHeader overclaiming;
  overclaiming.signature     = xudu::opsSegmentSignature;
  overclaiming.formatVersion = xudu::opsSegmentFormatVersion;
  overclaiming.headerBytes   = xudu::opsSegmentHeaderBytes;
  overclaiming.nodeSize      = sizeof(CompactOpNode);
  overclaiming.firstOpIndex  = 1;
  overclaiming.nodeCount     = 9;
  writeSegmentFileByHand(dir / "short.ops", two, 1, overclaiming);
  EXPECT_THROW(static_cast<void>(spool.addSealedSegment(dir / "short.ops")),
               xudu::OpsSegmentUnreadable);

  // Too short to hold a header at all, which is what an empty file that
  // somebody touched into existence looks like to a sealed-segment reader.
  {
    std::ofstream out(dir / "stub.ops", std::ios::binary);
    out << "no";
  }
  EXPECT_THROW(static_cast<void>(spool.addSealedSegment(dir / "stub.ops")),
               xudu::OpsSegmentUnreadable);

  EXPECT_EQ(spool.size(), 0U) << "nothing was read out of any of them";
}

TEST(SegmentedOpsSpoolTest, aSegmentFileSaysWhatItIsAndComesBackTheSame) {
  const auto dir  = scratchDir("roundtrip");
  const auto path = dir / "written.ops";

  SegmentedOpsSpool spool;
  appendChain(spool, 7);
  CompactOpNode branched;
  branched.parentIndex   = 3;
  branched.branchOrdinal = 1;
  branched.at            = 700;
  spool.append(branched, MicroversionId::parse("3a1"));
  ASSERT_TRUE(spool.writeSegmentFile(path));

  // Exactly a header and its operations, with the header's own 64 KiB left as
  // a hole rather than padded out with anything.
  EXPECT_EQ(std::filesystem::file_size(path), segmentFileSize(8));

  // What a person running `file` or `less` on it sees first.
  {
    std::ifstream in(path, std::ios::binary);
    std::array<char, 12> opening{};
    in.read(opening.data(), opening.size());
    EXPECT_EQ(std::string(opening.data() + 1, 7), "XUDUOPS");
  }

  SegmentedOpsSpool reopened;
  ASSERT_TRUE(reopened.openActiveSegment(path));
  EXPECT_EQ(reopened.size(), spool.size());
  for (std::uint32_t i = 1; i <= spool.size(); i++) {
    ASSERT_NE(reopened.get(i), nullptr);
    EXPECT_EQ(reopened.get(i)->at, spool.get(i)->at);
    EXPECT_EQ(reopened.idOf(i).str(), spool.idOf(i).str());
  }
  // Including the branch, whose name is derived rather than written down.
  EXPECT_TRUE(reopened.contains(MicroversionId::parse("3a1")));

  // And it keeps growing from there, into the same file, past the header.
  CompactOpNode onward;
  onward.parentIndex = 8;
  onward.at          = 701;
  reopened.append(onward, MicroversionId::parse("3a2"));
  CompactOpNode further;
  further.parentIndex = 9;
  further.at          = 702;
  reopened.append(further, MicroversionId::parse("3a3"));
  ASSERT_TRUE(reopened.flush());
  EXPECT_EQ(std::filesystem::file_size(path), segmentFileSize(10));

  // Opened a third time, everything that was ever appended is there -- which
  // is what says the tail landed past the header rather than over it.
  SegmentedOpsSpool again;
  ASSERT_TRUE(again.openActiveSegment(path));
  EXPECT_EQ(again.size(), 10U);
  EXPECT_TRUE(again.contains(MicroversionId::parse("3a3")));
  EXPECT_THAT(again.ancestralPath(10),
              testing::ElementsAre(1U, 2U, 3U, 8U, 9U, 10U));
}

} // namespace

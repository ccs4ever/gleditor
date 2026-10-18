/**
 * @file structure_make_test.cpp
 * @brief Unit tests for generic Structure Make, StructureKind, typed
 * timestamps, and context edge resolution.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <string>
#include <string_view>

#include "common/xanadu/compact_op.hpp"
#include "common/xanadu/focus_target.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/scalar.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace {

using namespace xanadu;

TEST(StructureMakeTest, FlagsAndKinds) {
  // Test makeStructureFlags for Cell, Slice, Xanadoc
  const auto cellFlags =
      makeStructureFlags(StructureKind::Cell, ValueKind::None);
  EXPECT_EQ(structureVerbOf(cellFlags), StructureVerb::Make);
  EXPECT_EQ(structureKindOf(cellFlags), StructureKind::Cell);
  EXPECT_EQ(valueKindOf(cellFlags), ValueKind::None);

  const auto sliceFlags =
      makeStructureFlags(StructureKind::Slice, ValueKind::None);
  EXPECT_EQ(structureVerbOf(sliceFlags), StructureVerb::Make);
  EXPECT_EQ(structureKindOf(sliceFlags), StructureKind::Slice);
  EXPECT_EQ(valueKindOf(sliceFlags), ValueKind::None);

  const auto xanadocFlags =
      makeStructureFlags(StructureKind::Xanadoc, ValueKind::Timestamp);
  EXPECT_EQ(structureVerbOf(xanadocFlags), StructureVerb::Make);
  EXPECT_EQ(structureKindOf(xanadocFlags), StructureKind::Xanadoc);
  EXPECT_EQ(valueKindOf(xanadocFlags), ValueKind::Timestamp);

  // Names
  EXPECT_STREQ(structureVerbName(StructureVerb::Make), "make");
  EXPECT_STREQ(structureKindName(StructureKind::Cell), "cell");
  EXPECT_STREQ(structureKindName(StructureKind::Slice), "slice");
  EXPECT_STREQ(structureKindName(StructureKind::Xanadoc), "xanadoc");
  EXPECT_STREQ(valueKindName(ValueKind::Timestamp), "timestamp");
}

TEST(StructureMakeTest, CompactOpNodeContextEdge) {
  // Structure Make: context lives in sourceOpIndex
  {
    CompactOpNode node{};
    node.kind  = OpKind::Structure;
    node.flags = makeStructureFlags(StructureKind::Slice, ValueKind::None);
    node.sourceOpIndex = 42;
    EXPECT_EQ(contextOf(node), 42U);
    EXPECT_EQ(subjectOf(node), 42U);
  }

  // Structure non-Make (e.g. SetValue): context lives in sourceAt, subject in
  // sourceOpIndex
  {
    CompactOpNode node{};
    node.kind          = OpKind::Structure;
    node.flags         = static_cast<std::uint8_t>(StructureVerb::SetValue);
    node.sourceOpIndex = 10;
    node.sourceAt      = 42;
    EXPECT_EQ(contextOf(node), 42U);
    EXPECT_EQ(subjectOf(node), 10U);
  }

  // Transclude: context lives in to, source version in sourceOpIndex
  {
    CompactOpNode node{};
    node.kind          = OpKind::Transclude;
    node.sourceOpIndex = 10;
    node.to            = 42;
    EXPECT_EQ(contextOf(node), 42U);
    EXPECT_EQ(subjectOf(node), 10U);
  }

  // Other ops (Insert, Delete, Link, PageBreak): context in sourceOpIndex
  {
    CompactOpNode node{};
    node.kind          = OpKind::Insert;
    node.sourceOpIndex = 42;
    EXPECT_EQ(contextOf(node), 42U);
    EXPECT_EQ(subjectOf(node), 42U);
  }

  // fromOp and toOp round-trip with context
  Op op;
  op.kind    = OpKind::Structure;
  op.flags   = makeStructureFlags(StructureKind::Slice, ValueKind::None);
  op.parent  = MicroversionId::parse("1");
  op.source  = MicroversionId::parse("1a1");
  op.context = MicroversionId::parse("1b1");

  CompactOpNode converted = CompactOpNode::fromOp(op, 1, 2, 0, 3);
  EXPECT_EQ(converted.parentIndex, 1U);
  EXPECT_EQ(contextOf(converted), 3U);

  Op reconstructed = converted.toOp(op.parent, op.source, op.context);
  EXPECT_EQ(reconstructed.flags, op.flags);
  EXPECT_EQ(reconstructed.parent, op.parent);
  EXPECT_EQ(reconstructed.source, op.source);
  EXPECT_EQ(reconstructed.context, op.context);
}

TEST(StructureMakeTest, TimestampFormattingAndParsing) {
  // Epoch
  EXPECT_EQ(formatUtcTimestampIso8601(0), "1970-01-01T00:00:00.000000000Z");

  std::int64_t parsedNanos = -1;
  EXPECT_TRUE(
      parseUtcTimestampIso8601("1970-01-01T00:00:00.000000000Z", parsedNanos));
  EXPECT_EQ(parsedNanos, 0);

  // Known timestamp: 2026-09-30T12:34:56.789012345Z
  const std::string iso = "2026-09-30T12:34:56.789012345Z";
  EXPECT_TRUE(parseUtcTimestampIso8601(iso, parsedNanos));
  EXPECT_EQ(formatUtcTimestampIso8601(parsedNanos), iso);

  // Sub-second parsing with fewer digits (e.g. milliseconds)
  const std::string msIso = "2026-09-30T12:34:56.5Z";
  EXPECT_TRUE(parseUtcTimestampIso8601(msIso, parsedNanos));
  EXPECT_EQ(formatUtcTimestampIso8601(parsedNanos),
            "2026-09-30T12:34:56.500000000Z");

  // Negative instant (before 1970): 1969-12-31T23:59:59.000000000Z ->
  // -1,000,000,000 ns
  EXPECT_EQ(formatUtcTimestampIso8601(-1000000000LL),
            "1969-12-31T23:59:59.000000000Z");
  EXPECT_TRUE(
      parseUtcTimestampIso8601("1969-12-31T23:59:59.000000000Z", parsedNanos));
  EXPECT_EQ(parsedNanos, -1000000000LL);

  // Scalar creation
  const auto ts = scalarTimestamp(123456789LL);
  EXPECT_EQ(ts.kind, ValueKind::Timestamp);
  EXPECT_EQ(ts.bits, std::bit_cast<std::uint64_t>(123456789LL));
  EXPECT_EQ(ts.text, formatUtcTimestampIso8601(123456789LL));
}

TEST(StructureMakeTest, TiesToEvenRounding) {
  // Unit = 10
  // 5 rounds to 0 (even), 15 rounds to 20 (even)
  EXPECT_EQ(roundInstantToNearestTiesToEven(5, 10), 0);
  EXPECT_EQ(roundInstantToNearestTiesToEven(15, 10), 20);
  EXPECT_EQ(roundInstantToNearestTiesToEven(25, 10), 20);
  EXPECT_EQ(roundInstantToNearestTiesToEven(35, 10), 40);

  // Negative ties to even:
  // -5 is halfway between -10 and 0 -> rounds to 0 (even quotient 0)
  // -15 is halfway between -20 and -10 -> rounds to -20 (even quotient -2)
  // -25 is halfway between -30 and -20 -> rounds to -20 (even quotient -2)
  EXPECT_EQ(roundInstantToNearestTiesToEven(-5, 10), 0);
  EXPECT_EQ(roundInstantToNearestTiesToEven(-15, 10), -20);
  EXPECT_EQ(roundInstantToNearestTiesToEven(-25, 10), -20);
  EXPECT_EQ(roundInstantToNearestTiesToEven(-35, 10), -40);

  // Non-ties round to nearest
  EXPECT_EQ(roundInstantToNearestTiesToEven(4, 10), 0);
  EXPECT_EQ(roundInstantToNearestTiesToEven(6, 10), 10);
  EXPECT_EQ(roundInstantToNearestTiesToEven(-4, 10), 0);
  EXPECT_EQ(roundInstantToNearestTiesToEven(-6, 10), -10);
}

TEST(StructureMakeTest, StorePutOpWithContext) {
  Store store;
  // Genesis cell birth
  Op genesis;
  genesis.kind     = OpKind::Structure;
  genesis.flags    = makeStructureFlags(StructureKind::Cell, ValueKind::None);
  const auto genId = MicroversionId::parse("1");
  store.putOp(genId, genesis);

  // A Slice birth referencing genesis as context
  Op sliceOp;
  sliceOp.kind    = OpKind::Structure;
  sliceOp.flags   = makeStructureFlags(StructureKind::Slice, ValueKind::None);
  sliceOp.parent  = genId;
  sliceOp.context = genId;
  const auto sliceId = MicroversionId::parse("1a1");
  store.putOp(sliceId, sliceOp);

  // Check retrieved op
  const auto retrieved = store.getOp(sliceId);
  ASSERT_TRUE(retrieved.has_value());
  EXPECT_EQ(structureVerbOf(retrieved->flags), StructureVerb::Make);
  EXPECT_EQ(structureKindOf(retrieved->flags), StructureKind::Slice);
  EXPECT_EQ(retrieved->parent, genId);
  EXPECT_EQ(retrieved->context, genId);

  // Check compact node context
  const auto *node = store.getCompactOp(sliceId);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(contextOf(*node), 1U); // genId is at index 1 in spool
}

TEST(StructureMakeTest, ManifoldTypeAwareFold) {
  zigzag::Manifold manifold;

  // 1. Reserved StructureKind is refused
  {
    CompactOpNode node{};
    node.kind  = OpKind::Structure;
    node.flags = makeStructureFlags(StructureKind::Reserved, ValueKind::None);
    const auto res = manifold.applyStructure(1, node);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), zigzag::FoldRefusal::InvalidMakeKind);
    EXPECT_EQ(manifold.refusedOps(), 1U);
  }

  // 2. Slice birth validations: idle fields non-zero
  {
    CompactOpNode badNode{};
    badNode.kind   = OpKind::Structure;
    badNode.flags  = makeStructureFlags(StructureKind::Slice, ValueKind::None);
    badNode.at     = 10;
    const auto res = manifold.applyStructure(2, badNode);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), zigzag::FoldRefusal::InvalidMakeKind);
  }

  // Slice birth validations: non-zero value
  {
    CompactOpNode badNode{};
    badNode.kind   = OpKind::Structure;
    badNode.flags  = makeStructureFlags(StructureKind::Slice, ValueKind::None);
    badNode.value  = 42;
    const auto res = manifold.applyStructure(2, badNode);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), zigzag::FoldRefusal::InvalidMakeKind);
  }

  // Slice birth validations: unknown container
  {
    CompactOpNode badNode{};
    badNode.kind  = OpKind::Structure;
    badNode.flags = makeStructureFlags(StructureKind::Slice, ValueKind::None);
    badNode.sourceOpIndex = 999;
    const auto res        = manifold.applyStructure(2, badNode);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), zigzag::FoldRefusal::UnknownSubject);
  }

  // 3. Valid Top-Level Slice birth
  PrimediaSpan sliceSpan{.scroll = localScroll, .start = 0, .length = 10};
  CompactOpNode sliceNode{};
  sliceNode.kind  = OpKind::Structure;
  sliceNode.flags = makeStructureFlags(StructureKind::Slice, ValueKind::None);
  sliceNode.setSpan(sliceSpan);
  ASSERT_TRUE(manifold.applyStructure(10, sliceNode).has_value());

  EXPECT_EQ(manifold.cellCount(), 0U);
  EXPECT_FALSE(manifold.isCell(10));
  EXPECT_TRUE(manifold.isStructureBirth(10));
  EXPECT_EQ(manifold.structureKind(10), StructureKind::Slice);
  EXPECT_EQ(manifold.containerOf(10), 0U);
  EXPECT_EQ(manifold.structureNameSpan(10), sliceSpan);

  // 4. Valid Top-Level Xanadoc birth
  PrimediaSpan xanadocSpan{.scroll = localScroll, .start = 10, .length = 15};
  CompactOpNode xanadocNode{};
  xanadocNode.kind = OpKind::Structure;
  xanadocNode.flags =
      makeStructureFlags(StructureKind::Xanadoc, ValueKind::None);
  xanadocNode.setSpan(xanadocSpan);
  ASSERT_TRUE(manifold.applyStructure(20, xanadocNode).has_value());

  EXPECT_EQ(manifold.cellCount(), 0U);
  EXPECT_FALSE(manifold.isCell(20));
  EXPECT_TRUE(manifold.isStructureBirth(20));
  EXPECT_EQ(manifold.structureKind(20), StructureKind::Xanadoc);
  EXPECT_EQ(manifold.containerOf(20), 0U);
  EXPECT_EQ(manifold.structureNameSpan(20), xanadocSpan);

  // 5. Valid Cell birth nested inside Slice (op 10)
  PrimediaSpan cellSpan{.scroll = localScroll, .start = 25, .length = 5};
  CompactOpNode cellNode{};
  cellNode.kind  = OpKind::Structure;
  cellNode.flags = makeStructureFlags(StructureKind::Cell, ValueKind::None);
  cellNode.sourceOpIndex = 10; // container is Slice
  cellNode.setSpan(cellSpan);
  ASSERT_TRUE(manifold.applyStructure(30, cellNode).has_value());

  EXPECT_EQ(manifold.cellCount(), 1U);
  EXPECT_TRUE(manifold.isCell(30));
  EXPECT_TRUE(manifold.isStructureBirth(30));
  EXPECT_EQ(manifold.structureKind(30), StructureKind::Cell);
  EXPECT_EQ(manifold.containerOf(30), 10U);
  EXPECT_EQ(manifold.structureNameSpan(30), cellSpan);

  // 6. Duplicate birth refused
  const auto dupRes = manifold.applyStructure(30, cellNode);
  ASSERT_FALSE(dupRes.has_value());
  EXPECT_EQ(dupRes.error(), zigzag::FoldRefusal::DuplicateCell);

  // 7. Structure query lists
  const auto allBirths = manifold.structureBirths();
  EXPECT_THAT(allBirths, testing::ElementsAre(10U, 20U, 30U));
  EXPECT_THAT(manifold.structureBirths(StructureKind::Slice),
              testing::ElementsAre(10U));
  EXPECT_THAT(manifold.structureBirths(StructureKind::Xanadoc),
              testing::ElementsAre(20U));
  EXPECT_THAT(manifold.structureBirths(StructureKind::Cell),
              testing::ElementsAre(30U));

  // 8. Manifold equivalence
  zigzag::Manifold manifold2;
  ASSERT_TRUE(manifold2.applyStructure(10, sliceNode).has_value());
  ASSERT_TRUE(manifold2.applyStructure(20, xanadocNode).has_value());
  ASSERT_TRUE(manifold2.applyStructure(30, cellNode).has_value());
  EXPECT_TRUE(manifold.equivalentTo(manifold2));

  // Without the xanadoc birth in manifold3
  zigzag::Manifold manifold3;
  ASSERT_TRUE(manifold3.applyStructure(10, sliceNode).has_value());
  ASSERT_TRUE(manifold3.applyStructure(30, cellNode).has_value());
  EXPECT_FALSE(manifold.equivalentTo(manifold3));
}

TEST(StructureMakeTest, SliceGenesisSequence) {
  Store store;
  const auto head = store.sliceGenesis(MicroversionId{}, "test_slice");
  EXPECT_EQ(store.opCount(), 4U);
  EXPECT_EQ(store.sliceBirth(), 1U);
  EXPECT_EQ(store.homeCell(), 2U);
  EXPECT_EQ(store.dimsDimension(), 3U);

  // Op 1: Make(Slice)
  const auto *op1 = store.getCompactOp(1);
  ASSERT_NE(op1, nullptr);
  EXPECT_EQ(op1->kind, OpKind::Structure);
  EXPECT_EQ(structureVerbOf(op1->flags), StructureVerb::Make);
  EXPECT_EQ(structureKindOf(op1->flags), StructureKind::Slice);
  EXPECT_EQ(contextOf(*op1), 0U);
  EXPECT_EQ(store.read(op1->span()), "test_slice");

  // Op 2: Make(Cell: home) with context pointing to Slice (1)
  const auto *op2 = store.getCompactOp(2);
  ASSERT_NE(op2, nullptr);
  EXPECT_EQ(op2->kind, OpKind::Structure);
  EXPECT_EQ(structureVerbOf(op2->flags), StructureVerb::Make);
  EXPECT_EQ(structureKindOf(op2->flags), StructureKind::Cell);
  EXPECT_EQ(contextOf(*op2), 1U);
  EXPECT_EQ(store.read(op2->span()), "home");

  // Op 3: Make(Cell: d.dims) with context pointing to Slice (1)
  const auto *op3 = store.getCompactOp(3);
  ASSERT_NE(op3, nullptr);
  EXPECT_EQ(op3->kind, OpKind::Structure);
  EXPECT_EQ(structureVerbOf(op3->flags), StructureVerb::Make);
  EXPECT_EQ(structureKindOf(op3->flags), StructureKind::Cell);
  EXPECT_EQ(contextOf(*op3), 1U);
  EXPECT_EQ(store.read(op3->span()), "d.dims");

  // Op 4: SetLink home -> d.dims with context pointing to Slice (1)
  const auto *op4 = store.getCompactOp(4);
  ASSERT_NE(op4, nullptr);
  EXPECT_EQ(op4->kind, OpKind::Structure);
  EXPECT_EQ(structureVerbOf(op4->flags), StructureVerb::SetLink);
  EXPECT_EQ(contextOf(*op4), 1U);

  // Manifold inspection
  const auto manifold = store.rebuildManifold(head);
  EXPECT_EQ(manifold.cellCount(), 2U);
  EXPECT_TRUE(manifold.isStructureBirth(1));
  EXPECT_EQ(manifold.structureKind(1), StructureKind::Slice);
  EXPECT_TRUE(manifold.isCell(2));
  EXPECT_EQ(manifold.containerOf(2), 1U);
  EXPECT_TRUE(manifold.isCell(3));
  EXPECT_EQ(manifold.containerOf(3), 1U);
}

TEST(StructureMakeTest, XanadocEditContextChains) {
  Store store;
  const auto doc = store.makeXanadoc(MicroversionId{}, "doc1");
  EXPECT_EQ(doc.str(), "1");
  const auto *docNode = store.getCompactOp(1);
  ASSERT_NE(docNode, nullptr);
  EXPECT_EQ(docNode->kind, OpKind::Structure);
  EXPECT_EQ(structureKindOf(docNode->flags), StructureKind::Xanadoc);
  EXPECT_EQ(contextOf(*docNode), 0U);

  // Consecutive operations on doc
  const auto v1 = store.insert(doc, 0, "hello");
  const auto v2 = store.insert(v1, 5, " world");
  const auto v3 = store.erase(v2, 5, 6);
  const auto v4 = store.insertBreak(v3, 5);
  const auto v5 = store.rearrange(v4, 0, 5, 5);

  // Verify context chain
  const auto *n1 = store.getCompactOp(store.segmentedOps().indexOf(v1));
  ASSERT_NE(n1, nullptr);
  EXPECT_EQ(contextOf(*n1), 1U); // points to doc

  const auto *n2 = store.getCompactOp(store.segmentedOps().indexOf(v2));
  ASSERT_NE(n2, nullptr);
  EXPECT_EQ(contextOf(*n2), store.segmentedOps().indexOf(v1)); // points to v1

  const auto *n3 = store.getCompactOp(store.segmentedOps().indexOf(v3));
  ASSERT_NE(n3, nullptr);
  EXPECT_EQ(contextOf(*n3), store.segmentedOps().indexOf(v2)); // points to v2

  const auto *n4 = store.getCompactOp(store.segmentedOps().indexOf(v4));
  ASSERT_NE(n4, nullptr);
  EXPECT_EQ(contextOf(*n4), store.segmentedOps().indexOf(v3)); // points to v3

  const auto *n5 = store.getCompactOp(store.segmentedOps().indexOf(v5));
  ASSERT_NE(n5, nullptr);
  EXPECT_EQ(contextOf(*n5), store.segmentedOps().indexOf(v4)); // points to v4
}

TEST(StructureMakeTest, InterleavedXanadocsAndBranchLocalContext) {
  Store store;
  const auto docA = store.makeXanadoc(MicroversionId{}, "docA"); // op 1
  const auto docB = store.makeXanadoc(docA, "docB");             // op 2

  const auto vA1 = store.insert(docB, 0, "A1", docA); // op 3
  const auto vB1 = store.insert(vA1, 0, "B1", docB);  // op 4
  const auto vA2 = store.insert(vB1, 2, "A2", docA);  // op 5
  const auto vB2 = store.insert(vA2, 2, "B2", docB);  // op 6

  // Verify each points to its own structure's previous edit:
  const auto *nA1 = store.getCompactOp(store.segmentedOps().indexOf(vA1));
  EXPECT_EQ(contextOf(*nA1), 1U); // docA birth

  const auto *nB1 = store.getCompactOp(store.segmentedOps().indexOf(vB1));
  EXPECT_EQ(contextOf(*nB1), 2U); // docB birth

  const auto *nA2 = store.getCompactOp(store.segmentedOps().indexOf(vA2));
  EXPECT_EQ(contextOf(*nA2), 3U); // vA1

  const auto *nB2 = store.getCompactOp(store.segmentedOps().indexOf(vB2));
  EXPECT_EQ(contextOf(*nB2), 4U); // vB1

  // Branching from vA1:
  const auto vA_fork = store.insert(vA1, 2, "A_fork", docA);
  const auto *nA_fork =
      store.getCompactOp(store.segmentedOps().indexOf(vA_fork));
  EXPECT_EQ(contextOf(*nA_fork), 3U); // vA1
}

TEST(StructureMakeTest, StructureRenameAndResolution) {
  Store store;
  const auto doc = store.makeXanadoc(MicroversionId{}, "original_name");
  EXPECT_EQ(store.resolveStructureName(doc, 1), "original_name");

  const auto v1 = store.insert(doc, 0, "text");

  // Rename structure
  const auto vRenamed = store.renameStructure(v1, 1, "renamed_doc");
  EXPECT_EQ(store.resolveStructureName(vRenamed, 1), "renamed_doc");

  // Branch before rename still sees original name
  EXPECT_EQ(store.resolveStructureName(v1, 1), "original_name");

  // Rename to empty string
  const auto vEmpty = store.renameStructure(vRenamed, 1, "");
  EXPECT_EQ(store.resolveStructureName(vEmpty, 1), "");

  // Verify the rename SetLink carries the named structure's prior edit in
  // sourceAt
  const auto *setLinkNode =
      store.getCompactOp(store.segmentedOps().indexOf(vRenamed));
  ASSERT_NE(setLinkNode, nullptr);
  EXPECT_EQ(setLinkNode->kind, OpKind::Structure);
  EXPECT_EQ(structureVerbOf(setLinkNode->flags), StructureVerb::SetLink);
  EXPECT_EQ(contextOf(*setLinkNode), store.segmentedOps().indexOf(v1));
}

TEST(StructureMakeTest, TimestampAnnotationAndResolution) {
  Store store;
  const auto doc = store.makeXanadoc(MicroversionId{}, "timed_doc");

  // Before annotation: nullopt
  EXPECT_EQ(store.resolveStructureCreated(doc, 1), std::nullopt);

  // Annotate with instant
  const auto instant = TimestampInstant{.epochNanos = 1712345678900000000LL};
  const auto vAnn    = store.annotateTimestamp(doc, 1, instant);

  const auto resolved = store.resolveStructureCreated(vAnn, 1);
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->epochNanos, instant.epochNanos);

  // Annotate with ISO 8601 string
  const auto vIso = store.annotateTimestamp(vAnn, 1, "2026-09-30T12:00:00Z");
  const auto resolvedIso = store.resolveStructureCreated(vIso, 1);
  ASSERT_TRUE(resolvedIso.has_value());
  EXPECT_EQ(formatUtcTimestampIso8601(*resolvedIso),
            "2026-09-30T12:00:00.000000000Z");

  // Sibling branch before annotation still sees no timestamp
  EXPECT_EQ(store.resolveStructureCreated(doc, 1), std::nullopt);
}

TEST(StructureMakeTest, ContextValidationRejections) {
  Store store;
  const auto docA = store.makeXanadoc(MicroversionId{}, "docA");
  const auto vA1  = store.insert(docA, 0, "hello");

  // Fork a sibling branch
  const auto docB = store.makeXanadoc(MicroversionId{}, "docB");

  // 1. Context in the future
  EXPECT_THROW(store.insert(docA, 0, "fail", vA1), std::invalid_argument);

  // 2. Context on sibling branch (unreachable from parent)
  EXPECT_THROW(store.insert(vA1, 0, "fail", docB), std::invalid_argument);
}

TEST(StructureMakeTest, ScopedReplayInterleavedXanadocs) {
  Store store;
  const auto docA   = store.makeXanadoc(MicroversionId{}, "DocA");
  const auto docB   = store.makeXanadoc(docA, "DocB");
  const auto birthA = store.segmentedOps().indexOf(docA);
  const auto birthB = store.segmentedOps().indexOf(docB);

  const auto v1 = store.insert(docB, 0, "Hello DocA! ", docA);
  const auto v2 = store.insert(v1, 0, "Greetings from DocB! ", docB);
  const auto v3 = store.insert(v2, 12, "More DocA text.", docA);

  EXPECT_EQ(store.textOf(v3, birthA), "Hello DocA! More DocA text.");
  EXPECT_EQ(store.textOf(v3, birthB), "Greetings from DocB! ");

  // Single-step advance on scoped version
  Version stepDocA = store.rebuild(docB, birthA);
  EXPECT_TRUE(store.advance(stepDocA, docB, v1, birthA));
  EXPECT_EQ(stepDocA.materialize(store), "Hello DocA! ");

  // Multi-step advanceTo on scoped version
  Version docAVer = store.rebuild(v1, birthA);
  EXPECT_TRUE(store.advanceTo(docAVer, v1, v3, birthA));
  EXPECT_EQ(docAVer.materialize(store), "Hello DocA! More DocA text.");
}

TEST(StructureMakeTest, CellTargetedTextEdits) {
  Store store;
  const auto slice      = store.makeSlice(MicroversionId{}, "Slice1");
  const auto sliceBirth = store.segmentedOps().indexOf(slice);
  const auto cell       = store.makeCell(slice, "", slice);
  const auto cellBirth  = store.segmentedOps().indexOf(cell);

  EXPECT_TRUE(store.isCellOp(cellBirth));
  EXPECT_FALSE(store.isCellOp(sliceBirth));

  // Insert into cell
  const auto vC1  = store.insert(cell, 0, "Cell content here", cell);
  const auto opC1 = store.segmentedOps().indexOf(vC1);
  EXPECT_TRUE(store.isCellOp(opC1));
  EXPECT_EQ(store.editedBirthOf(opC1), cellBirth);

  // Erase from cell
  const auto vC2  = store.erase(vC1, 4, 8, cell);
  const auto opC2 = store.segmentedOps().indexOf(vC2);
  EXPECT_TRUE(store.isCellOp(opC2));
  EXPECT_EQ(store.editedBirthOf(opC2), cellBirth);

  // Document concatext must NOT include cell-targeted text edits
  const auto docVer = store.rebuild(vC2, 0);
  EXPECT_EQ(docVer.length(), 0U);
  EXPECT_EQ(store.textOf(vC2, 0), "");

  // Replay into manifold and verify cell text
  zigzag::Manifold manifold;
  manifold.setStore(&store);
  for (std::uint32_t i = 1; i <= opC2; ++i) {
    const auto *node = store.getCompactOp(i);
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(manifold.applyStructure(i, *node).has_value());
  }
  EXPECT_EQ(manifold.textOf(cellBirth, store), "Cell here");
}

TEST(StructureMakeTest, CellTargetedTransclusion) {
  Store store;
  const auto doc  = store.makeXanadoc(MicroversionId{}, "Doc");
  const auto vDoc = store.insert(doc, 0, "Quoted Source", doc);

  const auto slice     = store.makeSlice(vDoc, "Slice");
  const auto cell      = store.makeCell(slice, "", slice);
  const auto cellBirth = store.segmentedOps().indexOf(cell);

  const auto vTrans  = store.transclude(cell, 0, vDoc, 0, 6, cell);
  const auto opTrans = store.segmentedOps().indexOf(vTrans);
  EXPECT_TRUE(store.isCellOp(opTrans));
  EXPECT_EQ(store.editedBirthOf(opTrans), cellBirth);

  zigzag::Manifold manifold;
  manifold.setStore(&store);
  for (std::uint32_t i = 1; i <= opTrans; ++i) {
    const auto *node = store.getCompactOp(i);
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(manifold.applyStructure(i, *node).has_value());
  }
  EXPECT_EQ(manifold.textOf(cellBirth, store), "Quoted");
}

TEST(StructureMakeTest, CellHistoryAndInterleavedStructureOps) {
  Store store;
  const auto slice     = store.makeSlice(MicroversionId{}, "Slice1");
  const auto cell      = store.makeCell(slice, "", slice);
  const auto cellBirth = store.segmentedOps().indexOf(cell);

  const auto v1 = store.insert(cell, 0, "First ", cell);
  const auto v2 = store.setScalar(v1, cellBirth, 42.0);
  const auto v3 = store.insert(v2, 6, "Second", cell);

  zigzag::Manifold manifold;
  manifold.setStore(&store);
  const auto opV3 = store.segmentedOps().indexOf(v3);
  for (std::uint32_t i = 1; i <= opV3; ++i) {
    const auto *node = store.getCompactOp(i);
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(manifold.applyStructure(i, *node).has_value());
  }

  // historyOf returns operations from birth to head: [cellBirth, opV1, opV2,
  // opV3]
  const auto history = manifold.historyOf(cellBirth);
  ASSERT_EQ(history.size(), 4U);
  EXPECT_EQ(history.front(), cellBirth);
  EXPECT_EQ(history.back(), opV3);

  // contentAsOf at intermediate points
  auto readSpans = [&](const std::vector<PrimediaSpan> &spans) {
    std::string s;
    for (const auto &sp : spans) {
      s += store.read(sp);
    }
    return s;
  };
  const auto opV1 = store.segmentedOps().indexOf(v1);
  const auto opV2 = store.segmentedOps().indexOf(v2);
  EXPECT_EQ(readSpans(manifold.contentAsOf(cellBirth, opV1)), "First ");
  EXPECT_EQ(readSpans(manifold.contentAsOf(cellBirth, opV2)), "42");
  EXPECT_EQ(readSpans(manifold.contentAsOf(cellBirth, opV3)), "42Second");
}

TEST(StructureMakeTest, ContainmentHierarchyAndPath) {
  Store store;
  const auto sliceA = store.makeSlice(MicroversionId{}, "SliceA");
  const auto birthA = store.segmentedOps().indexOf(sliceA);
  const auto cellB  = store.makeCell(sliceA, "", sliceA);
  const auto birthB = store.segmentedOps().indexOf(cellB);
  const auto sliceC = store.makeSlice(cellB, "SliceC", cellB);
  const auto birthC = store.segmentedOps().indexOf(sliceC);
  const auto cellD  = store.makeCell(sliceC, "", sliceC);
  const auto birthD = store.segmentedOps().indexOf(cellD);

  EXPECT_EQ(store.containmentPath(birthA), std::vector<std::uint32_t>{birthA});
  EXPECT_EQ(store.containmentPath(birthB),
            (std::vector<std::uint32_t>{birthA, birthB}));
  EXPECT_EQ(store.containmentPath(birthC),
            (std::vector<std::uint32_t>{birthA, birthB, birthC}));
  EXPECT_EQ(store.containmentPath(birthD),
            (std::vector<std::uint32_t>{birthA, birthB, birthC, birthD}));

  EXPECT_TRUE(store.validateContainment(birthA));
  EXPECT_TRUE(store.validateContainment(birthB));
  EXPECT_TRUE(store.validateContainment(birthC));
  EXPECT_TRUE(store.validateContainment(birthD));

  zigzag::Manifold manifold;
  manifold.setStore(&store);
  for (std::uint32_t i = 1; i <= birthD; ++i) {
    const auto *node = store.getCompactOp(i);
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(manifold.applyStructure(i, *node).has_value());
  }
  EXPECT_EQ(manifold.containmentPath(birthD),
            (std::vector<std::uint32_t>{birthA, birthB, birthC, birthD}));
  EXPECT_TRUE(manifold.validateContainment(birthD));
}

TEST(StructureMakeTest, PageBreakCellContextRejection) {
  Store store;
  const auto slice     = store.makeSlice(MicroversionId{}, "Slice");
  const auto cell      = store.makeCell(slice, "", slice);
  const auto cellBirth = store.segmentedOps().indexOf(cell);

  // Store::insertBreak rejects cell context
  EXPECT_THROW(store.insertBreak(cell, 0, cell), std::invalid_argument);

  // Manifold raw fold rejects PageBreak on cell context
  zigzag::Manifold manifold;
  manifold.setStore(&store);
  for (std::uint32_t i = 1; i <= cellBirth; ++i) {
    EXPECT_TRUE(manifold.applyStructure(i, *store.getCompactOp(i)).has_value());
  }
  CompactOpNode pbNode{};
  pbNode.kind          = OpKind::PageBreak;
  pbNode.sourceOpIndex = cellBirth;
  const auto refusal   = manifold.applyStructure(cellBirth + 1, pbNode);
  ASSERT_FALSE(refusal.has_value());
  EXPECT_EQ(refusal.error(), zigzag::FoldRefusal::WrongContextKind);
}

TEST(StructureMakeTest, HeadOfStructureResolution) {
  Store store;
  const auto doc1   = store.makeXanadoc(MicroversionId{}, "Doc1");
  const auto birth1 = store.segmentedOps().indexOf(doc1);
  const auto doc2   = store.makeXanadoc(doc1, "Doc2");
  const auto birth2 = store.segmentedOps().indexOf(doc2);

  const auto v1 = store.insert(doc2, 0, "A", doc1);
  const auto v2 = store.insert(v1, 0, "B", doc2);
  const auto v3 = store.insert(v2, 1, "C", doc1);

  EXPECT_EQ(store.headOfStructure(birth1, v3), v3);
  EXPECT_EQ(store.headOfStructure(birth2, v3), v2);
  EXPECT_EQ(store.headOfStructure(birth1, v2), v1);
  EXPECT_EQ(store.headOfStructure(birth2, v2), v2);
}

TEST(StructureMakeTest, FocusTargetStruct) {
  FocusTarget target{};
  EXPECT_FALSE(target.isValid());

  target.kind            = StructureKind::Cell;
  target.birthOp         = 42;
  target.containmentPath = {1, 10, 42};
  EXPECT_TRUE(target.isValid());
  EXPECT_EQ(target.kind, StructureKind::Cell);
  EXPECT_EQ(target.birthOp, 42U);
  EXPECT_EQ(target.containmentPath.size(), 3U);
}

} // namespace

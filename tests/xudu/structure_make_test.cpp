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

} // namespace

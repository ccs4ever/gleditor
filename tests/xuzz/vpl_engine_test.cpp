/**
 * @file vpl_engine_test.cpp
 * @brief Unit tests for VPLEngine direct execution and sys:array integration.
 */
#include <array>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "common/xanadu/vpl/vpl_engine.hpp"

namespace {

using namespace xanadu::vpl;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::noCell;

TEST(VPLEngineTest, CatenateKeepsBudgetAndAliasesIntact) {
  VPLEngine engine;
  auto original            = engine.evaluate("data ← 12 10 5");
  const auto originalCells = original.collectCells(engine.arena());
  auto report              = engine.evaluate("report ← data , +/data");
  auto cells               = report.collectCells(engine.arena());
  ASSERT_EQ(cells.size(), 4u);
  const std::array<std::int64_t, 4> expected{12, 10, 5, 27};
  for (std::size_t i = 0; i < cells.size(); ++i) {
    EXPECT_EQ(engine.arena().asInt64(cells[i]), expected[i]);
    EXPECT_NE(cells[i], originalCells[std::min(i, originalCells.size() - 1)]);
  }
  EXPECT_EQ(engine.evaluate("data").collectCells(engine.arena()),
            originalCells);
  auto repeated = engine.evaluate("data , data");
  EXPECT_EQ(repeated.collectCells(engine.arena()).size(), 6u);
  EXPECT_EQ(engine.evaluate("data").collectCells(engine.arena()),
            originalCells);
}

TEST(VPLEngineTest, CatenateScalarReportsAndMixedTypes) {
  VPLEngine engine;
  engine.evaluate("ready ← 2; unready ← 1; total ← 3");
  auto report = engine.evaluate("ready , unready , total");
  auto cells  = report.collectCells(engine.arena());
  ASSERT_EQ(cells.size(), 3u);
  EXPECT_EQ(engine.arena().asInt64(cells[0]), 2);
  EXPECT_EQ(engine.arena().asInt64(cells[1]), 1);
  EXPECT_EQ(engine.arena().asInt64(cells[2]), 3);
  auto mixed =
      engine.evaluate("12 , 3.5 , 'label'").collectCells(engine.arena());
  ASSERT_EQ(mixed.size(), 3u);
  EXPECT_EQ(engine.arena().asInt64(mixed[0]), 12);
  EXPECT_EQ(engine.arena().asDouble(mixed[1]), 3.5);
  EXPECT_EQ(engine.arena().textOf(mixed[2]), "label");
  auto ravel = engine.evaluate(", 2 1 3");
  EXPECT_EQ(ravel.collectCells(engine.arena()).size(), 3u);
  EXPECT_THROW(engine.evaluate("2 ,"), std::runtime_error);
  EXPECT_THROW(engine.evaluate(","), std::runtime_error);
}

TEST(VPLEngineTest, CalculationsKeepOperandProvenanceAcrossBindings) {
  VPLEngine engine;
  auto first             = engine.evaluate("12 10 5");
  const auto firstSource = first.collectCells(engine.arena());
  first.setDependencies(firstSource);
  engine.setVariable("data", first);
  const auto total = engine.evaluate("total ← +/data");
  EXPECT_EQ(total.dependencies(), firstSource);
  auto second             = engine.evaluate("1 1 0");
  const auto secondSource = second.collectCells(engine.arena());
  second.setDependencies(secondSource);
  engine.setVariable("data", second);
  auto report            = engine.evaluate("total , +/data");
  const auto reportCells = report.collectCells(engine.arena());
  ASSERT_EQ(reportCells.size(), 2u);
  EXPECT_THAT(engine.dependenciesOf(reportCells[0]),
              ::testing::ElementsAreArray(firstSource));
  EXPECT_THAT(engine.dependenciesOf(reportCells[1]),
              ::testing::ElementsAreArray(secondSource));
  EXPECT_TRUE(engine.evaluate("literal ← 42").dependencies().empty());
  EXPECT_TRUE(engine.evaluate("+/data; 99").dependencies().empty());
  EXPECT_THROW(engine.evaluate("total ,"), std::runtime_error);
  EXPECT_EQ(engine.evaluate("total").dependencies(), firstSource);
}

TEST(VPLEngineTest, ScalarArithmeticAndScalarExtension) {
  VPLEngine engine;

  // Basic scalar expressions
  auto v1 = engine.evaluate("3 + 4");
  EXPECT_TRUE(v1.isScalar());
  EXPECT_EQ(v1.scalarInt(), 7);

  auto v2 = engine.evaluate("10 - 3");
  EXPECT_EQ(v2.scalarInt(), 7);

  auto v3 = engine.evaluate("6 * 7");
  EXPECT_EQ(v3.scalarInt(), 42);

  auto v4 = engine.evaluate("42 % 6");
  EXPECT_EQ(v4.scalarInt(), 7);

  // Monadic math
  auto v5 = engine.evaluate("- 42");
  EXPECT_EQ(v5.scalarInt(), -42);

  auto v6 = engine.evaluate("| _15");
  EXPECT_EQ(v6.scalarInt(), 15);

  // Scalar extension: scalar + vector
  auto v7 = engine.evaluate("10 + 1 2 3");
  EXPECT_FALSE(v7.isScalar());
  EXPECT_EQ(v7.valence(), 1u);
  auto cells7 = v7.collectCells(engine.arena());
  ASSERT_EQ(cells7.size(), 3u);
  EXPECT_EQ(engine.arena().asInt64(cells7[0]).value_or(0), 11);
  EXPECT_EQ(engine.arena().asInt64(cells7[1]).value_or(0), 12);
  EXPECT_EQ(engine.arena().asInt64(cells7[2]).value_or(0), 13);
}

TEST(VPLEngineTest, IotaShapeAndValenceDualSyntax) {
  VPLEngine engine;

  // APL syntax: A ← 5⍳d.1
  auto vA = engine.evaluate("A ← 5⍳d.1");
  EXPECT_FALSE(vA.isScalar());
  EXPECT_EQ(vA.valence(), 1u);

  auto shA = engine.evaluate("⍴A");
  EXPECT_EQ(shA.scalarInt(), 5);
  EXPECT_EQ(shA.valence(), 1u);

  auto rankA = engine.evaluate("⍴⍴A");
  EXPECT_EQ(rankA.scalarInt(), 1);
  EXPECT_EQ(rankA.valence(), 1u);

  // J-style syntax: B =. 5 i. d.1
  auto vB = engine.evaluate("B =. 5 i. d.1");
  EXPECT_FALSE(vB.isScalar());

  auto shB = engine.evaluate("$ B");
  EXPECT_EQ(shB.scalarInt(), 5);
  EXPECT_EQ(shB.valence(), 1u);

  auto rankB = engine.evaluate("$ $ B");
  EXPECT_EQ(rankB.scalarInt(), 1);
  EXPECT_EQ(rankB.valence(), 1u);
}

TEST(VPLEngineTest, ZeroLengthIotaKeepsItsArrayShapeWithoutAnOrigin) {
  VPLEngine engine;
  const auto empty = engine.evaluate("0 ⍳ d.1");

  EXPECT_FALSE(empty.isScalar());
  ASSERT_EQ(empty.valence(), 1u);
  EXPECT_FALSE(empty.origin());
  EXPECT_EQ(empty.shape(engine.arena()), std::vector<std::size_t>{0});
  EXPECT_TRUE(empty.collectCells(engine.arena()).empty());
}

TEST(VPLEngineTest, TransposeAndReverse) {
  VPLEngine engine;

  engine.evaluate("A ← 5⍳d.1");

  // Reverse: ⌽A
  auto revA = engine.evaluate("⌽A");
  EXPECT_EQ(revA.axes()[0].dir, zigzag::DimVector::NEG);

  // Reverse ASCII: |. A
  auto revAscii = engine.evaluate("|. A");
  EXPECT_EQ(revAscii.axes()[0].dir, zigzag::DimVector::NEG);

  // Transpose: ⍉ and |:
  engine.evaluate("V ← H ⍴ (d.1 d.2 d.3)");
  auto vTrans = engine.evaluate("⍉V");
  EXPECT_EQ(vTrans.axes().size(), 3u);
  EXPECT_EQ(vTrans.axes()[0].dim, engine.resolveDimension("d.3"));
  EXPECT_EQ(vTrans.axes()[2].dim, engine.resolveDimension("d.1"));

  auto vTransAscii = engine.evaluate("|: V");
  EXPECT_EQ(vTransAscii.axes()[0].dim, engine.resolveDimension("d.3"));
}

TEST(VPLEngineTest, TakeAndDrop) {
  VPLEngine engine;

  engine.evaluate("A ← 5⍳d.1");

  // Take 3: 3 ↑ A
  auto take3 = engine.evaluate("3 ↑ A");
  EXPECT_EQ(take3.extents().size(), 1u);
  EXPECT_EQ(take3.extents()[0], 3u);

  // Take ASCII: 3 {. A
  auto takeAscii = engine.evaluate("3 {. A");
  EXPECT_EQ(takeAscii.extents()[0], 3u);

  // Drop 2: 2 ↓ A
  auto drop2 = engine.evaluate("2 ↓ A");
  EXPECT_EQ(drop2.extents().size(), 1u);
  EXPECT_EQ(drop2.extents()[0], 3u);

  // Drop ASCII: 2 }. A
  auto dropAscii = engine.evaluate("2 }. A");
  EXPECT_EQ(dropAscii.extents()[0], 3u);
}

TEST(VPLEngineTest, ReductionsAndScans) {
  VPLEngine engine;

  // Sum reduction: +/ 1 2 3 4 5 -> 15
  auto sumRes = engine.evaluate("+/ 1 2 3 4 5");
  EXPECT_TRUE(sumRes.isScalar());
  EXPECT_EQ(sumRes.scalarInt(), 15);

  // Product reduction: ×/ 1 2 3 4 5 -> 120
  auto prodRes = engine.evaluate("×/ 1 2 3 4 5");
  EXPECT_TRUE(prodRes.isScalar());
  EXPECT_EQ(prodRes.scalarInt(), 120);

  // ASCII product: */ 1 2 3 4 5
  auto prodAscii = engine.evaluate("*/ 1 2 3 4 5");
  EXPECT_EQ(prodAscii.scalarInt(), 120);

  // Scan: +\ 1 2 3 4 5 -> 1 3 6 10 15
  auto scanRes = engine.evaluate("+\\ 1 2 3 4 5");
  EXPECT_FALSE(scanRes.isScalar());
  EXPECT_EQ(scanRes.extents().size(), 1u);
  EXPECT_EQ(scanRes.extents()[0], 5u);

  auto cells = scanRes.collectCells(engine.arena());
  ASSERT_EQ(cells.size(), 5u);
  EXPECT_EQ(engine.arena().asInt64(cells[0]).value_or(0), 1);
  EXPECT_EQ(engine.arena().asInt64(cells[1]).value_or(0), 3);
  EXPECT_EQ(engine.arena().asInt64(cells[2]).value_or(0), 6);
  EXPECT_EQ(engine.arena().asInt64(cells[3]).value_or(0), 10);
  EXPECT_EQ(engine.arena().asInt64(cells[4]).value_or(0), 15);
}

TEST(VPLEngineTest, OuterProduct) {
  VPLEngine engine;

  // Outer product addition: (1 2 3) ∘.+ (10 20)
  auto opRes = engine.evaluate("(1 2 3) ∘.+ (10 20)");
  EXPECT_FALSE(opRes.isScalar());
  EXPECT_EQ(opRes.valence(), 2u);
  auto sh = opRes.shape(engine.arena());
  ASSERT_EQ(sh.size(), 2u);
  EXPECT_EQ(sh[0], 3u);
  EXPECT_EQ(sh[1], 2u);

  // Dual syntax: (1 2 3) o.+ (10 20)
  auto opAscii = engine.evaluate("(1 2 3) o.+ (10 20)");
  EXPECT_EQ(opAscii.valence(), 2u);
  auto shAscii = opAscii.shape(engine.arena());
  EXPECT_EQ(shAscii[0], 3u);
  EXPECT_EQ(shAscii[1], 2u);
}

TEST(VPLEngineTest, EncloseAndDisclose) {
  VPLEngine engine;

  // Enclose view into cell
  auto enc = engine.evaluate("⊂ 1 2 3");
  EXPECT_TRUE(enc.isEnclosed());

  // Disclose unboxes view
  engine.setVariable("B", enc);
  auto disc = engine.evaluate("⊃ B");
  EXPECT_FALSE(disc.isEnclosed());
  auto sh = disc.shape(engine.arena());
  ASSERT_FALSE(sh.empty());
  EXPECT_EQ(sh[0], 3u);
}

TEST(VPLEngineTest, IndexingAndSorting) {
  VPLEngine engine;

  engine.evaluate("A ← 10 20 30 40 50");

  // Single cell indexing
  auto cell2 = engine.evaluate("A[2]");
  EXPECT_TRUE(cell2.isScalar());
  EXPECT_EQ(cell2.scalarInt(), 30);

  // Grade up: ⍋ 30 10 20 -> indices 1 2 0
  auto gradeRes = engine.evaluate("⍋ 30 10 20");
  auto gCells   = gradeRes.collectCells(engine.arena());
  ASSERT_EQ(gCells.size(), 3u);
  EXPECT_EQ(engine.arena().asInt64(gCells[0]).value_or(0), 1);
  EXPECT_EQ(engine.arena().asInt64(gCells[1]).value_or(0), 2);
  EXPECT_EQ(engine.arena().asInt64(gCells[2]).value_or(0), 0);
}

TEST(VPLEngineTest, HypertimeAndTransclusion) {
  VPLEngine engine;

  auto ht = engine.evaluate("⍟ 5 ⍳ d.1");
  EXPECT_TRUE(ht.isScalar());
  EXPECT_GT(ht.scalarInt(), 0);

  // ASCII alias: time.
  auto htAscii = engine.evaluate("time. 5 i. d.1");
  EXPECT_TRUE(htAscii.isScalar());
}

TEST(VPLEngineTest, SysArrayStdLibIntegration) {
  VPLEngine engine;

  EXPECT_TRUE(engine.stdlib().has("sys:array"));
  EXPECT_TRUE(engine.stdlib().has("sys:array/iota"));
  EXPECT_TRUE(engine.stdlib().has("sys:array/tally"));

  DimRef d1    = engine.resolveDimension("d.1");
  CellRef head = engine.stdlib().arrayIota(7, d1).value();
  EXPECT_EQ(engine.stdlib().arrayTally(head, d1), 7u);
}

} // namespace

TEST(VPLEngineCellPresenceTest,
     InvalidOriginCannotBecomeNumericZeroOrEmptyText) {
  VPLEngine engine;
  const auto dim = engine.arena().ensureDimension("d.1");
  const std::vector<zigzag::DirectedDim> axes{{dim, zigzag::DimVector::POS}};
  for (const auto invalid : {zigzag::noCell, zigzag::ephemeralBit - 1}) {
    ASSERT_FALSE(engine.arena().contains(invalid));
    const VplView view(invalid, axes);
    EXPECT_THROW(static_cast<void>(view.shape(engine.arena())),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(view.collectCells(engine.arena())),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(view.cellAt(engine.arena(), {})),
                 std::invalid_argument);
  }
  const VplView empty(std::nullopt, axes);
  EXPECT_TRUE(empty.collectCells(engine.arena()).empty());
  EXPECT_EQ(empty.shape(engine.arena()), std::vector<std::size_t>{0});

  const auto zero = engine.arena().makeScalarCell(std::int64_t{0});
  const VplView zeroView(zero, axes);
  EXPECT_THAT(zeroView.collectCells(engine.arena()),
              testing::ElementsAre(zero));
  engine.setVariable("zero", zeroView);
  EXPECT_EQ(engine.evaluate("+/zero").scalarInt(), 0);
  const auto text = engine.arena().makeCell("");
  EXPECT_THAT(VplView(text, axes).collectCells(engine.arena()),
              testing::ElementsAre(text));
}

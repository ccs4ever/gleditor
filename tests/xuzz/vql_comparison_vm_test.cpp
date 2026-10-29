/**
 * @file vql_comparison_vm_test.cpp
 * @brief Comparison behavior through the compiled VQL and Vortex VM path.
 */
#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/scalar.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::DimVector;

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_compare_inferred_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};
  QueryExecutor executor{coordinator, vm};
  zigzag::DimRef values{coordinator.resolveDimension("d.values")};

  void append(CellRef cell) {
    const auto from = zigzag::rankTail(arena, core.home(), values);
    zigzag::expectWritten(arena.link(from, values, DimVector::POS, cell));
  }

  std::vector<CellRef> run(const std::string &query) {
    const auto result = executor.execute(query);
    EXPECT_TRUE(result) << result.error().message;
    return result ? *result : std::vector<CellRef>{};
  }
};

struct ComparisonOperand {
  bool isDouble;
  std::uint64_t bits;

  [[nodiscard]] CellRef materialize(zigzag::ArenaManifold &arena) const {
    if (isDouble) {
      const auto cell = arena.makeCell();
      zigzag::expectWritten(
          arena.setValueBits(cell, xanadu::ValueKind::Double, bits));
      return cell;
    }
    return arena.makeScalarCell(std::bit_cast<std::int64_t>(bits));
  }
};

enum class ExpectedOrder { Less, Equivalent, Greater, Unordered };

struct MixedComparisonCase {
  const char *name;
  ComparisonOperand left;
  ComparisonOperand right;
  ExpectedOrder expectedOrder;
};

bool runRawComparison(const ComparisonOperand &leftValue,
                      const ComparisonOperand &rightValue,
                      const zigzag::vortex::VqlComparisonKind kind) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  const auto left        = leftValue.materialize(arena);
  const auto right       = rightValue.materialize(arena);
  const auto leftBefore  = *arena.slot(left);
  const auto rightBefore = *arena.slot(right);
  const auto opcode = vm.mintOpcode(zigzag::vortex::OpcodeKind::VqlCompare);
  const auto halt   = vm.mintOpcode(zigzag::vortex::OpcodeKind::Halt);
  const auto output = arena.makeCell();
  zigzag::expectWritten(
      arena.link(opcode, core.dims().spin, DimVector::POS, halt));
  core.bindInput(opcode, arena.makeScalarCell(static_cast<std::int64_t>(kind)));
  core.bindInput(opcode, left);
  core.bindInput(opcode, right);
  core.bindOutput(opcode, output);

  const auto execution = vm.run(vm.spawnCursor(opcode, "mixed-compare"), 8);
  EXPECT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_EQ(static_cast<std::uint8_t>(arena.valueKindOf(left)),
            leftBefore.valueKind);
  EXPECT_EQ(arena.slot(left)->valueBits, leftBefore.valueBits);
  EXPECT_EQ(static_cast<std::uint8_t>(arena.valueKindOf(right)),
            rightBefore.valueKind);
  EXPECT_EQ(arena.slot(right)->valueBits, rightBefore.valueBits);
  const auto result = vm.resultCell(output);
  EXPECT_TRUE(result);
  return result ? arena.asBool(*result).value_or(false) : false;
}

std::array<bool, 6> expectedComparison(const ExpectedOrder order) {
  switch (order) {
  case ExpectedOrder::Less:
    return {false, true, true, true, false, false};
  case ExpectedOrder::Equivalent:
    return {true, false, false, true, false, true};
  case ExpectedOrder::Greater:
    return {false, true, false, false, true, true};
  case ExpectedOrder::Unordered:
    return {false, true, false, false, false, false};
  }
  return {};
}

std::array<bool, 6> reversedExpected(const std::array<bool, 6> &expected) {
  return {expected[0], expected[1], expected[4],
          expected[5], expected[2], expected[3]};
}

TEST(VQLComparisonVMTest, IntegerBoundsAndAdjacentValuesRemainExact) {
  Harness h;
  const auto minimum = h.arena.makeScalarCell(INT64_MIN);
  const auto nearMax = h.arena.makeScalarCell(INT64_MAX - 1);
  const auto maximum = h.arena.makeScalarCell(INT64_MAX);
  const auto two53   = h.arena.makeScalarCell(INT64_C(9007199254740992));
  const auto two53p1 = h.arena.makeScalarCell(INT64_C(9007199254740993));
  for (const auto cell : {minimum, nearMax, maximum, two53, two53p1})
    h.append(cell);

  EXPECT_EQ(h.run("for $x in ##/d.values where $x = 9223372036854775807 "
                  "return $x"),
            (std::vector<CellRef>{maximum}));
  EXPECT_EQ(h.run("for $x in ##/d.values where $x < 9223372036854775807 "
                  "return $x"),
            (std::vector<CellRef>{minimum, nearMax, two53, two53p1}));
  EXPECT_EQ(h.run("for $x in ##/d.values where $x = 9007199254740993 "
                  "return $x"),
            (std::vector<CellRef>{two53p1}));
}

TEST(VQLComparisonVMTest, AllIntegerRelationalOperatorsFilterTheExpectedCells) {
  Harness h;
  const auto one   = h.arena.makeScalarCell(std::int64_t{1});
  const auto two   = h.arena.makeScalarCell(std::int64_t{2});
  const auto three = h.arena.makeScalarCell(std::int64_t{3});
  for (const auto cell : {one, two, three}) h.append(cell);

  const auto check = [&](const std::string_view op,
                         const std::vector<CellRef> &expected) {
    const auto result = h.run("for $x in ##/d.values where $x " +
                              std::string(op) + " 2 return $x");
    EXPECT_EQ(result, expected) << op;
  };
  check("=", {two});
  check("!=", {one, three});
  check("<", {one});
  check("<=", {one, two});
  check(">", {three});
  check(">=", {two, three});
}

TEST(VQLComparisonVMTest, DoubleNaNInfinityAndSignedZeroUseIeeeRules) {
  Harness h;
  const auto negativeInfinity =
      h.arena.makeScalarCell(-std::numeric_limits<double>::infinity());
  const auto positiveInfinity =
      h.arena.makeScalarCell(std::numeric_limits<double>::infinity());
  const auto nan = h.arena.makeScalarCell(
      std::bit_cast<double>(UINT64_C(0x7ff8000000000042)));
  const auto negativeZero = h.arena.makeScalarCell(-0.0);
  const auto positiveZero = h.arena.makeScalarCell(0.0);
  for (const auto cell :
       {negativeInfinity, positiveInfinity, nan, negativeZero, positiveZero})
    h.append(cell);

  EXPECT_EQ(h.run("for $x in ##/d.values where $x = $x return $x"),
            (std::vector<CellRef>{negativeInfinity, positiveInfinity,
                                  negativeZero, positiveZero}));
  EXPECT_EQ(h.run("for $x in ##/d.values where $x != $x return $x"),
            (std::vector<CellRef>{nan}));
  EXPECT_TRUE(h.run("for $x in ##/d.values where $x < $x return $x").empty());
  EXPECT_EQ(h.run("for $x in ##/d.values where $x <= $x return $x"),
            (std::vector<CellRef>{negativeInfinity, positiveInfinity,
                                  negativeZero, positiveZero}));
  EXPECT_TRUE(h.run("for $x in ##/d.values where $x > $x return $x").empty());
  EXPECT_EQ(h.run("for $x in ##/d.values where $x >= $x return $x"),
            (std::vector<CellRef>{negativeInfinity, positiveInfinity,
                                  negativeZero, positiveZero}));
  EXPECT_EQ(h.run("for $x in ##/d.values where $x = 0.0 return $x"),
            (std::vector<CellRef>{negativeZero, positiveZero}));
  EXPECT_EQ(h.run("for $x in ##/d.values where $x < 0.0 return $x"),
            (std::vector<CellRef>{negativeInfinity}));
}

TEST(VQLComparisonVMTest, AdjacentDoublesCompareWithoutAnEpsilon) {
  Harness h;
  const auto one  = h.arena.makeScalarCell(1.0);
  const auto next = h.arena.makeScalarCell(std::nextafter(1.0, 2.0));
  h.append(one);
  h.append(next);
  EXPECT_EQ(h.run("for $x in ##/d.values where $x = 1.0 return $x"),
            (std::vector<CellRef>{one}));
  EXPECT_EQ(h.run("for $x in ##/d.values where $x != 1.0 return $x"),
            (std::vector<CellRef>{next}));
}

TEST(VQLComparisonVMTest, MixedIntegerDoubleComparisonIsLosslessInBothOrders) {
  using Kind         = zigzag::vortex::VqlComparisonKind;
  const auto integer = [](const std::int64_t value) {
    return ComparisonOperand{false, std::bit_cast<std::uint64_t>(value)};
  };
  const auto floating = [](const double value) {
    return ComparisonOperand{true, std::bit_cast<std::uint64_t>(value)};
  };
  const std::vector<MixedComparisonCase> cases{
      {"same integer value", integer(1), floating(1.0),
       ExpectedOrder::Equivalent},
      {"positive fraction", integer(1), floating(1.5), ExpectedOrder::Less},
      {"negative fraction above", integer(-1), floating(-0.5),
       ExpectedOrder::Less},
      {"negative fraction below", integer(-1), floating(-1.5),
       ExpectedOrder::Greater},
      {"positive zero", integer(0), floating(0.0), ExpectedOrder::Equivalent},
      {"negative zero", integer(0), floating(-0.0), ExpectedOrder::Equivalent},
      {"positive subnormal", integer(0),
       floating(std::numeric_limits<double>::denorm_min()),
       ExpectedOrder::Less},
      {"negative subnormal", integer(0),
       floating(-std::numeric_limits<double>::denorm_min()),
       ExpectedOrder::Greater},
      {"quiet NaN", integer(0),
       floating(std::bit_cast<double>(UINT64_C(0x7ff8000000000042))),
       ExpectedOrder::Unordered},
      {"positive infinity", integer(INT64_MAX),
       floating(std::numeric_limits<double>::infinity()), ExpectedOrder::Less},
      {"negative infinity", integer(INT64_MIN),
       floating(-std::numeric_limits<double>::infinity()),
       ExpectedOrder::Greater},
      {"integer above 2^53", integer(INT64_C(9007199254740993)),
       floating(0x1p53), ExpectedOrder::Greater},
      {"integer exactly 2^53", integer(INT64_C(9007199254740992)),
       floating(0x1p53), ExpectedOrder::Equivalent},
      {"maximum below positive 2^63", integer(INT64_MAX), floating(0x1p63),
       ExpectedOrder::Less},
      {"maximum above predecessor of 2^63", integer(INT64_MAX),
       floating(std::nextafter(0x1p63, 0.0)), ExpectedOrder::Greater},
      {"minimum equals negative 2^63", integer(INT64_MIN), floating(-0x1p63),
       ExpectedOrder::Equivalent},
      {"minimum below successor of negative 2^63", integer(INT64_MIN),
       floating(std::nextafter(-0x1p63, 0.0)), ExpectedOrder::Less},
      {"minimum above predecessor of negative 2^63", integer(INT64_MIN),
       floating(
           std::nextafter(-0x1p63, -std::numeric_limits<double>::infinity())),
       ExpectedOrder::Greater},
  };
  constexpr std::array kinds{Kind::Equal,       Kind::NotEqual,
                             Kind::LessThan,    Kind::LessEqual,
                             Kind::GreaterThan, Kind::GreaterEqual};

  for (const auto &test : cases) {
    SCOPED_TRACE(test.name);
    const auto expected = expectedComparison(test.expectedOrder);
    const auto reversed = reversedExpected(expected);
    for (std::size_t index = 0; index < kinds.size(); ++index) {
      EXPECT_EQ(runRawComparison(test.left, test.right, kinds[index]),
                expected[index])
          << "left integer, operator " << index;
      EXPECT_EQ(runRawComparison(test.right, test.left, kinds[index]),
                reversed[index])
          << "left double, operator " << index;
    }
  }
}

TEST(VQLComparisonVMTest, CompiledFrontendMatchesIntegerAndDoubleEquality) {
  Harness h;
  const auto integer  = h.arena.makeScalarCell(std::int64_t{1});
  const auto floating = h.arena.makeScalarCell(1.0);
  h.append(integer);
  h.append(floating);
  EXPECT_EQ(h.run("for $x in ##/d.values where $x = 1.0 return $x"),
            (std::vector<CellRef>{integer, floating}));
}

TEST(VQLComparisonVMTest, CustomDisplayDoesNotOverrideTypedBits) {
  Harness h;
  const auto cell = h.arena.makeCell("display says forty two");
  ASSERT_TRUE(
      h.arena.setValueBits(cell, xanadu::ValueKind::Int64,
                           std::bit_cast<std::uint64_t>(std::int64_t{42})));
  h.append(cell);

  EXPECT_EQ(h.arena.textOf(cell), "display says forty two");
  EXPECT_EQ(h.run("for $x in ##/d.values where $x = 42 return $x"),
            (std::vector<CellRef>{cell}));
  Harness doubleHarness;
  const auto doubleCell =
      doubleHarness.arena.makeCell("display says minus one");
  ASSERT_TRUE(
      doubleHarness.arena.setValueBits(doubleCell, xanadu::ValueKind::Double,
                                       xanadu::canonicalDoubleBits(42.5)));
  doubleHarness.append(doubleCell);
  EXPECT_EQ(
      doubleHarness.run("for $x in ##/d.values where $x = 42.5 return $x"),
      (std::vector<CellRef>{doubleCell}));
}

TEST(VQLComparisonVMTest, MalformedAndNumericLookingTextUseTextComparison) {
  Harness h;
  const auto numericText = h.arena.makeCell("10");
  const auto malformed   = h.arena.makeCell("12x");
  const auto later       = h.arena.makeCell("12y");
  for (const auto cell : {numericText, malformed, later}) h.append(cell);

  EXPECT_EQ(h.run("for $x in ##/d.values where $x = \"12x\" return $x"),
            (std::vector<CellRef>{malformed}));
  EXPECT_EQ(h.run("for $x in ##/d.values where $x != \"12x\" return $x"),
            (std::vector<CellRef>{numericText, later}));
  EXPECT_EQ(h.run("for $x in ##/d.values where $x < \"12y\" return $x"),
            (std::vector<CellRef>{numericText, malformed}));
}

TEST(VQLComparisonVMTest, TextOnlyNumericCellsUseInferredNumericComparison) {
  Harness h;
  const auto ten = h.arena.makeCell("10");
  const auto two = h.arena.makeCell("2");
  h.append(ten);
  h.append(two);

  ASSERT_EQ(h.arena.valueKindOf(ten), xanadu::ValueKind::None);
  EXPECT_EQ(h.run("for $x in ##/d.values where $x = 10 return $x"),
            (std::vector<CellRef>{ten}));
  EXPECT_TRUE(h.run("for $x in ##/d.values where $x < 2 return $x").empty());
}

TEST(VQLComparisonVMTest, MissingOperandsFailEveryComparisonIncludingNotEqual) {
  Harness h;
  const auto one = h.arena.makeScalarCell(std::int64_t{1});
  h.append(one);
  for (const auto op : {"=", "!=", "<", "<=", ">", ">="}) {
    const auto query = "for $x in ##/d.values where $x " + std::string(op) +
                       " $missing return $x";
    const auto result = h.executor.execute(query);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_TRUE(result->empty()) << op;
  }
}

TEST(VQLComparisonVMTest, TextComparisonChargesOperandBytesToWorkBudget) {
  Harness h;
  const std::string longText(256, 'x');
  const auto query =
      "if \"" + longText + "\" = \"" + longText + "\" return ## else return ##";
  VQLCompiler compiler(h.coordinator, h.vm);
  const auto compiled = compiler.compile(query);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  ASSERT_TRUE(compiled.cursor);

  h.arena.setExecutionLimits(10000, 0, 1024 * 1024);
  const auto execution = h.vm.run(*compiled.cursor, 1000);
  EXPECT_FALSE(execution.success);
  EXPECT_NE(execution.errorMessage.find("work budget exhausted"),
            std::string::npos);
}

TEST(VQLComparisonVMTest, InferredTextScalarUsesItsStoredIntegerValue) {
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  xanadu::Store store(scroll);
  auto version            = store.sliceGenesis(store.latest());
  const auto dim          = store.makeDimension(version, "d.values");
  version                 = dim.version;
  const auto valueVersion = store.makeCell(version, "9007199254740993");
  const auto valueRef     = store.cellRefOf(valueVersion);
  ASSERT_TRUE(valueRef);
  const auto value = *valueRef;
  version          = valueVersion;
  ASSERT_TRUE(store.homeCell());
  version =
      store.setLink(version, *store.homeCell(), dim.dim, DimVector::POS, value);
  const auto manifold = store.rebuildManifold(version);
  zigzag::ArenaManifold arena(&manifold, store);
  zigzag::vortex::VortexCore core(arena);
  MultiStoreCoordinator coordinator(core);
  zigzag::vortex::VortexVM vm(core);
  QueryExecutor executor(coordinator, vm);

  ASSERT_EQ(arena.valueKindOf(value), xanadu::ValueKind::Int64);
  ASSERT_EQ(arena.asInt64(value), INT64_C(9007199254740993));
  const auto result = executor.execute(
      "for $x in ##/d.values where $x = 9007199254740993 return $x");
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{value}));
}

TEST(VQLComparisonVMTest, CloneComparisonReadsTheMasterScalarValue) {
  Harness h;
  const auto master = h.arena.makeCell("master display");
  ASSERT_TRUE(
      h.arena.setValueBits(master, xanadu::ValueKind::Int64,
                           std::bit_cast<std::uint64_t>(std::int64_t{73})));
  const auto clone = h.arena.makeCell("clone display");
  zigzag::expectWritten(
      h.arena.link(master, h.core.dims().clone, DimVector::POS, clone));
  zigzag::expectWritten(
      h.arena.link(h.core.home(), h.values, DimVector::POS, clone));

  EXPECT_EQ(h.run("for $x in ##/d.values where $x = 73 return $x"),
            (std::vector<CellRef>{clone}));
}

} // namespace
} // namespace xanadu::vql

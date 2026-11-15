#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using Result = std::expected<std::vector<CellRef>, std::string>;

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};
  CompilationResult compiled;

  CompilationResult
  compile(const std::string_view source,
          std::unordered_map<std::string, std::vector<CellRef>> bindings = {}) {
    VQLCompiler compiler(coordinator, vm);
    compiled =
        compiler.compile(source, {.initialBindings = std::move(bindings)});
    return compiled;
  }

  Result execute() {
    if (!compiled.success) return std::unexpected(compiled.errorMessage);
    if (!compiled.cursor)
      return std::unexpected("compiled query has no cursor");
    const auto execution = vm.run(*compiled.cursor, 100'000);
    if (!execution.success) {
      vm.abortScopedChoices(*compiled.cursor);
      vm.setCursorOpcode(*compiled.cursor, std::nullopt);
      return std::unexpected(execution.errorMessage);
    }
    std::vector<CellRef> result;
    for (const CellRef slot : compiled.resultSlots) {
      const auto values = vm.resultCells(slot);
      result.insert(result.end(), values.begin(), values.end());
    }
    vm.setCursorOpcode(*compiled.cursor, std::nullopt);
    return result;
  }

  Result
  run(const std::string_view source,
      std::unordered_map<std::string, std::vector<CellRef>> bindings = {}) {
    (void)compile(source, std::move(bindings));
    return execute();
  }

  std::size_t count(const zigzag::vortex::OpcodeKind kind) const {
    std::size_t result = 0;
    for (const CellRef opcode : compiled.generatedOpcodes)
      result += vm.getOpcodeKind(opcode) == kind;
    return result;
  }
};

TEST(VQLRuntimeClosureSignaturesTest, NamedActualsMapToTheirFormals) {
  Harness h;
  const CellRef first  = h.arena.makeCell("first");
  const CellRef second = h.arena.makeCell("second");
  const auto result    = h.run(
      R"(let $f := \($left, $right) $left; return $f(right: $b, left: $a))",
      {{"a", {first}}, {"b", {second}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{first}));
}

TEST(VQLRuntimeClosureSignaturesTest, UntypedRestFlattensInGroupOrder) {
  Harness h;
  const CellRef first  = h.arena.makeCell("first");
  const CellRef second = h.arena.makeCell("second");
  const auto result =
      h.run(R"(let $f := \(...$rest) $rest; return $f($a, $b, $a))",
            {{"a", {first}}, {"b", {second}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{first, second, first}));
}

TEST(VQLRuntimeClosureSignaturesTest, TypedRestUsesOneDynamicGroupsPort) {
  Harness h;
  const CellRef first  = h.arena.makeScalarCell(INT64_C(7));
  const CellRef second = h.arena.makeScalarCell(INT64_C(9));
  const auto compiled  = h.compile(
      R"(let $f := \(...$rest: Int) $rest; return $f($a), $f($a, $b))",
      {{"a", {first}}, {"b", {second}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_EQ(h.count(zigzag::vortex::OpcodeKind::GroupBegin), 1U);
  EXPECT_EQ(h.count(zigzag::vortex::OpcodeKind::GroupNext), 1U);
  EXPECT_EQ(h.count(zigzag::vortex::OpcodeKind::CreateClosure), 1U);
  EXPECT_EQ(h.count(zigzag::vortex::OpcodeKind::ApplyClosure), 2U);
  std::optional<CellRef> closureHandle;
  std::optional<CellRef> descriptorCell;
  for (const CellRef opcode : compiled.generatedOpcodes) {
    if (h.vm.getOpcodeKind(opcode) ==
        zigzag::vortex::OpcodeKind::CreateClosure) {
      closureHandle  = h.core.outputsOf(opcode).front();
      descriptorCell = h.core.inputsOf(opcode).front();
    }
  }
  ASSERT_TRUE(closureHandle);
  ASSERT_TRUE(descriptorCell);
  const auto descriptor = h.vm.closureDescriptor(*descriptorCell);
  ASSERT_TRUE(descriptor) << descriptor.error().message;
  ASSERT_TRUE(descriptor->frameDescriptor);
  ASSERT_TRUE(descriptor->variadicFormalIndex);
  EXPECT_EQ(*descriptor->variadicFormalIndex, 0U);
  std::vector<CellRef> checkedHandles;
  for (const CellRef opcode : compiled.generatedOpcodes)
    if (h.vm.getOpcodeKind(opcode) ==
        zigzag::vortex::OpcodeKind::CheckClosureCall)
      checkedHandles.push_back(h.core.outputsOf(opcode).front());
  ASSERT_EQ(checkedHandles.size(), 2U);
  std::size_t uses = 0;
  for (const CellRef opcode : compiled.generatedOpcodes) {
    if (h.vm.getOpcodeKind(opcode) != zigzag::vortex::OpcodeKind::ApplyClosure)
      continue;
    const auto inputs = h.core.inputsOf(opcode);
    ASSERT_FALSE(inputs.empty());
    EXPECT_NE(std::ranges::find(checkedHandles, inputs.front()),
              checkedHandles.end());
    ++uses;
  }
  EXPECT_EQ(uses, 2U);
  const auto result = h.execute();
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{first, first, second}));
}

TEST(VQLRuntimeClosureSignaturesTest,
     TypedRestPatternCapturesDoNotShadowOuterAliases) {
  Harness h;
  const CellRef record     = h.arena.makeCell("record");
  const CellRef fieldValue = h.arena.makeScalarCell(INT64_C(1));
  const CellRef outside    = h.arena.makeScalarCell(INT64_C(99));
  const auto captureDim    = h.coordinator.resolveDimension("d.v");
  zigzag::expectWritten(
      h.arena.link(record, captureDim, zigzag::DimVector::POS, fieldValue));
  const auto result = h.run(
      R"(let $f := \(...$xs: {+d.v: $outside, +d.defaulted := $outside}) $outside; return $f($record))",
      {{"outside", {outside}}, {"record", {record}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{outside}));
}

TEST(VQLRuntimeClosureSignaturesTest,
     RestDestructureCapturesDoNotShadowOuterAliases) {
  Harness h;
  const CellRef record     = h.arena.makeCell("record");
  const CellRef fieldValue = h.arena.makeScalarCell(INT64_C(1));
  const CellRef outside    = h.arena.makeScalarCell(INT64_C(99));
  const auto captureDim    = h.coordinator.resolveDimension("d.v");
  zigzag::expectWritten(
      h.arena.link(record, captureDim, zigzag::DimVector::POS, fieldValue));
  const auto result =
      h.run(R"(let $f := \(...{+d.v: $outside}) $outside; return $f($record))",
            {{"outside", {outside}}, {"record", {record}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{outside}));
}

TEST(VQLRuntimeClosureSignaturesTest, NamedRestIsOneOriginalGroup) {
  Harness h;
  const CellRef first  = h.arena.makeScalarCell(INT64_C(7));
  const CellRef second = h.arena.makeScalarCell(INT64_C(9));
  const auto result =
      h.run(R"(let $f := \(...$rest: Stream<Int>) $rest; return $f(rest: $b))",
            {{"b", {first, second}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{first, second}));
}

TEST(VQLRuntimeClosureSignaturesTest,
     NamedRestDoesNotSplitOneMultiCellActualIntoScalarGroups) {
  Harness h;
  const CellRef first  = h.arena.makeScalarCell(INT64_C(7));
  const CellRef second = h.arena.makeScalarCell(INT64_C(9));
  const auto result =
      h.run(R"(let $f := \(...$rest: Int) $rest; return $f(rest: $b))",
            {{"b", {first, second}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_TRUE(result->empty());
}

TEST(VQLRuntimeClosureSignaturesTest,
     OmittedRestDefaultDiffersFromExplicitEmptyGroup) {
  Harness h;
  const CellRef fallback = h.arena.makeScalarCell(INT64_C(5));
  const auto result      = h.run(
      R"(let $f := \(...$rest: Int := $fallback) $rest; return $f(), $f($empty))",
      {{"fallback", {fallback}}, {"empty", {}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{fallback}));
}

TEST(VQLRuntimeClosureSignaturesTest,
     RequiredRestContractAliasesAreCapturedAtCreationEvenWithoutActuals) {
  Harness h;
  constexpr auto source =
      R"(let $f := \(...$rest: {+d.missing := $outside}) $rest; return $f())";
  const auto before  = h.arena.cellCount();
  const auto refused = h.compile(source);
  ASSERT_FALSE(refused.success);
  EXPECT_NE(refused.errorMessage.find("outside"), std::string::npos);
  EXPECT_EQ(h.arena.cellCount(), before);
  const auto boundEmpty = h.run(source, {{"outside", {}}});
  ASSERT_TRUE(boundEmpty) << boundEmpty.error();
  EXPECT_TRUE(boundEmpty->empty());
}

TEST(VQLRuntimeClosureSignaturesTest, IntegralDoubleDoesNotSatisfyIntRest) {
  Harness h;
  const auto value = h.arena.makeScalarCell(1.0);
  const auto result =
      h.run(R"(let $f := \(...$rest: Int) $rest; return $f($value))",
            {{"value", {value}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_TRUE(result->empty());
  EXPECT_EQ(h.arena.valueKindOf(value), xanadu::ValueKind::Double);
  EXPECT_EQ(h.arena.asDouble(value), 1.0);
}

} // namespace
} // namespace xanadu::vql

/**
 * @file vql_mutation_guards_test.cpp
 * @brief Typed parameter contracts at complete source mutation boundaries.
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::DimVector;

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};

  void link(const CellRef owner, const std::string_view name,
            const CellRef value) {
    const auto dimension = coordinator.resolveDimension(name);
    zigzag::expectWritten(arena.link(owner, dimension, DimVector::POS, value));
  }

  std::expected<std::vector<CellRef>, QueryExecutionError>
  run(const std::string_view source, ContractBindings bindings = {}) {
    QueryExecutor executor(coordinator, vm, {}, std::move(bindings));
    return executor.execute(source);
  }

  CompilationResult compile(const std::string_view source,
                            ContractBindings bindings = {}) {
    CompilationOptions options;
    options.initialBindings = std::move(bindings);
    VQLCompiler compiler(coordinator, vm);
    return compiler.compile(source, options);
  }

  std::optional<CellRef> field(const CellRef owner, const std::string_view name,
                               const DimVector direction = DimVector::POS) {
    return arena.linked(owner, coordinator.resolveDimension(name), direction);
  }
};

constexpr std::string_view kReplaceBadValue =
    "def weave replace($owner: Cell { +d.value: Int }): Cell := { "
    "weave { $owner/+d.value/value(\"bad\") }; return $owner; }; "
    "replace($owner)";

TEST(VQLMutationGuardsTest, InvalidTypedParameterMutationRollsBackBeforeError) {
  Harness h;
  const CellRef owner = h.arena.makeCell("owner");
  const CellRef value = h.arena.makeScalarCell(std::int64_t{12});
  h.link(owner, "d.value", value);

  const auto result = h.run(kReplaceBadValue, {{"owner", {owner}}});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().category,
            QueryExecutionError::Category::TypeContract);
  EXPECT_NE(result.error().message.find("replace"), std::string::npos);
  EXPECT_EQ(h.field(owner, "d.value"), value);
  EXPECT_EQ(h.arena.asInt64(value), 12);
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_FALSE(h.vm.hasActiveGuards());
}

TEST(VQLMutationGuardsTest, TypedParameterMismatchRemainsAFilter) {
  Harness h;
  const CellRef owner = h.arena.makeCell("owner");
  const CellRef value = h.arena.makeCell("not an integer");
  h.link(owner, "d.value", value);

  const auto result = h.run(kReplaceBadValue, {{"owner", {owner}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_TRUE(result->empty());
  EXPECT_EQ(h.field(owner, "d.value"), value);
  EXPECT_EQ(h.arena.textOf(value), "not an integer");
  EXPECT_FALSE(h.vm.hasActiveGuards());
}

TEST(VQLMutationGuardsTest, ValidTypedParameterMutationPreservesOwnerIdentity) {
  Harness h;
  const CellRef owner = h.arena.makeCell("owner");
  const CellRef value = h.arena.makeScalarCell(std::int64_t{12});
  h.link(owner, "d.value", value);
  constexpr std::string_view source =
      "def weave replace($owner: Cell { +d.value: Int }): Cell := { "
      "weave { $owner/+d.value/value(\"24\") }; return $owner; }; "
      "replace($owner)";

  const auto result = h.run(source, {{"owner", {owner}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{owner}));
  const auto replacement = h.field(owner, "d.value");
  ASSERT_TRUE(replacement);
  EXPECT_EQ(h.arena.textOf(*replacement), "24");
  EXPECT_FALSE(h.vm.hasActiveGuards());
}

TEST(VQLMutationGuardsTest,
     PriorMutationGroupRemainsCommittedAfterLaterFailure) {
  Harness h;
  const CellRef owner = h.arena.makeCell("owner");
  const CellRef value = h.arena.makeScalarCell(std::int64_t{12});
  h.link(owner, "d.value", value);
  constexpr std::string_view source =
      "def weave replace($owner: Cell { +d.value: Int }): Cell := { "
      "weave { $owner/+d.value/value(\"24\") }; "
      "weave { $owner/+d.value/value(\"bad\") }; "
      "return $owner; }; replace($owner)";

  const auto result = h.run(source, {{"owner", {owner}}});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().category,
            QueryExecutionError::Category::TypeContract);
  EXPECT_EQ(h.field(owner, "d.value"), value);
  EXPECT_EQ(h.arena.textOf(value), "24");
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
}

TEST(VQLMutationGuardsTest, ValidLinkFanoutPreservesGuardAndReturnedIdentity) {
  Harness h;
  const CellRef owner     = h.arena.makeCell("owner");
  const CellRef oldValue  = h.arena.makeScalarCell(std::int64_t{12});
  const CellRef nextValue = h.arena.makeScalarCell(std::int64_t{24});
  h.link(owner, "d.value", oldValue);
  constexpr std::string_view source =
      "def weave replace($owner: Cell { +d.value: Int }, $value: Int): Cell "
      ":= { weave { $owner/link(+d.value, $value) }; return $owner; }; "
      "replace($owner, $value)";

  const auto result =
      h.run(source, {{"owner", {owner}}, {"value", {nextValue}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{owner}));
  const auto linkedValue = h.field(owner, "d.value");
  ASSERT_TRUE(linkedValue);
  EXPECT_NE(*linkedValue, nextValue);
  EXPECT_NE(*linkedValue, oldValue);
  EXPECT_EQ(h.core.render(*linkedValue), h.core.render(nextValue));
  EXPECT_FALSE(h.field(oldValue, "d.value"));
}

TEST(VQLMutationGuardsTest,
     CreationListInitializesTheWholeChainBeforeCheckingTheContract) {
  Harness h;
  const CellRef owner = h.arena.makeCell("owner");
  constexpr std::string_view source =
      "def weave build($owner: Cell { +d.next: Cell { +d.next: Cell }? }): "
      "Stream<Cell> := $owner/d.next%\"a\"%\"b\"; build($owner)";

  const auto result = h.run(source, {{"owner", {owner}}});
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 2U);
  const auto dimension = h.coordinator.resolveDimension("d.next");
  EXPECT_EQ((*result)[0], h.arena.linked(owner, dimension, DimVector::POS));
  EXPECT_EQ((*result)[1],
            h.arena.linked((*result)[0], dimension, DimVector::POS));
  EXPECT_EQ(h.arena.textOf((*result)[0]), "a");
  EXPECT_EQ(h.arena.textOf((*result)[1]), "b");
}

TEST(VQLMutationGuardsTest,
     TypedContractFollowsMutationThroughAliasedParameter) {
  Harness h;
  const CellRef owner = h.arena.makeCell("owner");
  const CellRef value = h.arena.makeScalarCell(std::int64_t{12});
  h.link(owner, "d.value", value);
  constexpr std::string_view source =
      "def weave replace($typed: Cell { +d.value: Int }, $alias: Cell): Cell "
      ":= { weave { $alias/+d.value/value(\"bad\") }; return $typed; }; "
      "replace($owner, $owner)";

  const auto result = h.run(source, {{"owner", {owner}}});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().category,
            QueryExecutionError::Category::TypeContract);
  EXPECT_NE(result.error().message.find("replace"), std::string::npos);
  EXPECT_EQ(h.field(owner, "d.value"), value);
  EXPECT_EQ(h.arena.asInt64(value), 12);
}

TEST(VQLMutationGuardsTest,
     GuardOnDisplacedEndpointChecksItsRemovedReciprocalLink) {
  Harness h;
  const CellRef owner = h.arena.makeCell("owner");
  const CellRef value = h.arena.makeScalarCell(std::int64_t{12});
  h.link(owner, "d.value", value);
  constexpr std::string_view source =
      "def weave replace($owner: Cell, $endpoint: Cell { -d.value: Cell }): "
      "Cell := { weave { $owner/link(+d.value, \"replacement\") }; "
      "return $owner; }; replace($owner, $endpoint)";

  const auto result =
      h.run(source, {{"owner", {owner}}, {"endpoint", {value}}});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().category,
            QueryExecutionError::Category::TypeContract);
  EXPECT_NE(result.error().message.find("replace"), std::string::npos);
  EXPECT_EQ(h.field(owner, "d.value"), value);
  EXPECT_EQ(h.field(value, "d.value", DimVector::NEG), owner);
  EXPECT_EQ(h.arena.asInt64(value), 12);
}

TEST(VQLMutationGuardsTest, FailedCloneTailRestoresBothOriginalCloneEndpoints) {
  Harness h;
  const CellRef master = h.arena.makeScalarCell(std::int64_t{12});
  const CellRef alias  = h.arena.makeCell("text alias");
  constexpr std::string_view source =
      "def weave join($master: Int, $alias: String): Cell := "
      "$master><$alias; join($master, $alias)";

  const auto result = h.run(source, {{"master", {master}}, {"alias", {alias}}});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().category,
            QueryExecutionError::Category::TypeContract);
  EXPECT_NE(result.error().message.find("join"), std::string::npos);
  const auto clone = h.coordinator.resolveDimension("d.clone");
  EXPECT_FALSE(h.arena.linked(master, clone, DimVector::POS));
  EXPECT_FALSE(h.arena.linked(master, clone, DimVector::NEG));
  EXPECT_FALSE(h.arena.linked(alias, clone, DimVector::POS));
  EXPECT_FALSE(h.arena.linked(alias, clone, DimVector::NEG));
  EXPECT_EQ(h.arena.asInt64(master), 12);
  EXPECT_EQ(h.arena.textOf(alias), "text alias");
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VQLMutationGuardsTest, ExplicitWeaveCanRepairAnIntermediateInvalidValue) {
  Harness h;
  const CellRef owner = h.arena.makeCell("owner");
  const CellRef value = h.arena.makeScalarCell(std::int64_t{12});
  h.link(owner, "d.value", value);
  constexpr std::string_view source =
      "def weave repair($owner: Cell { +d.value: Int }): Cell := { "
      "weave { $owner/+d.value/value(\"temporary\"), "
      "$owner/+d.value/value(\"24\") }; return $owner; }; repair($owner)";

  const auto result = h.run(source, {{"owner", {owner}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{owner}));
  const auto replacement = h.field(owner, "d.value");
  ASSERT_TRUE(replacement);
  EXPECT_EQ(h.arena.textOf(*replacement), "24");
}

TEST(VQLMutationGuardsTest, UnrepairedOuterWeaveRollsBackItsWholeGroup) {
  Harness h;
  const CellRef owner = h.arena.makeCell("owner");
  const CellRef value = h.arena.makeScalarCell(std::int64_t{12});
  h.link(owner, "d.value", value);
  constexpr std::string_view source =
      "def weave replace($owner: Cell { +d.value: Int }): Cell := { "
      "weave { $owner/+d.value/value(\"temporary\") }; "
      "return $owner; }; replace($owner)";

  const auto result = h.run(source, {{"owner", {owner}}});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().category,
            QueryExecutionError::Category::TypeContract);
  EXPECT_EQ(h.field(owner, "d.value"), value);
  EXPECT_EQ(h.arena.asInt64(value), 12);
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
}

TEST(VQLMutationGuardsTest, CalleeGuardExpiresAfterItsScopeInsideOuterWeave) {
  Harness h;
  const CellRef owner = h.arena.makeCell("owner");
  const CellRef value = h.arena.makeScalarCell(std::int64_t{12});
  h.link(owner, "d.value", value);
  constexpr std::string_view source =
      "def weave set_int($owner: Cell { +d.value: Int }): Cell := { "
      "weave { $owner/+d.value/value(\"24\") }; return $owner; }; "
      "def weave caller($owner: Cell): Cell := { weave { $owner/set_int(), "
      "$owner/+d.value/value(\"later string\") }; return $owner; }; "
      "caller($owner)";

  const auto result = h.run(source, {{"owner", {owner}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{owner}));
  const auto replacement = h.field(owner, "d.value");
  ASSERT_TRUE(replacement);
  EXPECT_EQ(h.arena.textOf(*replacement), "later string");
}

TEST(VQLMutationGuardsTest, CallerGuardRemainsActiveAcrossUntypedCallee) {
  Harness h;
  const CellRef owner = h.arena.makeCell("owner");
  const CellRef value = h.arena.makeScalarCell(std::int64_t{12});
  h.link(owner, "d.value", value);
  constexpr std::string_view source =
      "def weave set_string($owner: Cell): Cell := { weave { "
      "$owner/+d.value/value(\"bad\") }; return $owner; }; "
      "def weave caller($owner: Cell { +d.value: Int }): Cell := { "
      "weave { $owner/set_string() }; return $owner; }; caller($owner)";

  const auto result = h.run(source, {{"owner", {owner}}});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().category,
            QueryExecutionError::Category::TypeContract);
  EXPECT_NE(result.error().message.find("caller"), std::string::npos);
  EXPECT_EQ(h.field(owner, "d.value"), value);
  EXPECT_EQ(h.arena.asInt64(value), 12);
}

TEST(VQLMutationGuardsTest, RecursiveTypedMutationRunsInReusableNativeFrames) {
  Harness h;
  const CellRef first  = h.arena.makeScalarCell(std::int64_t{1});
  const CellRef second = h.arena.makeScalarCell(std::int64_t{2});
  const CellRef third  = h.arena.makeScalarCell(std::int64_t{3});
  const auto dimension = h.coordinator.resolveDimension("d.next");
  zigzag::expectWritten(h.arena.link(first, dimension, DimVector::POS, second));
  zigzag::expectWritten(h.arena.link(second, dimension, DimVector::POS, third));
  constexpr std::string_view source =
      "def pure last($node: Int): Cell := { "
      "if $node/+d.next return last($node/value(7)/+d.next[1]) "
      "else return $node/value(7); }; last($first)";

  const auto compiled = h.compile(source, {{"first", {first}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  ASSERT_TRUE(compiled.entryOpcode);
  std::size_t invokeFrames   = 0;
  std::size_t pushes         = 0;
  std::size_t mutationGroups = 0;
  for (const CellRef opcode : compiled.generatedOpcodes) {
    const auto kind = h.vm.getOpcodeKind(opcode);
    invokeFrames += kind == zigzag::vortex::OpcodeKind::InvokeFrame;
    pushes += kind == zigzag::vortex::OpcodeKind::PushGuard;
    mutationGroups += kind == zigzag::vortex::OpcodeKind::BeginMutationGroup;
  }
  EXPECT_GT(invokeFrames, 0U);
  EXPECT_GT(pushes, 0U);
  EXPECT_GT(mutationGroups, 0U);

  const CellRef cursor =
      h.vm.spawnCursor(*compiled.entryOpcode, "recursive-mutation-guard");
  const auto execution = h.vm.run(cursor, 100'000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  std::vector<CellRef> results;
  for (const CellRef slot : compiled.resultSlots) {
    const auto values = h.vm.resultCells(slot);
    results.insert(results.end(), values.begin(), values.end());
  }
  EXPECT_EQ(results, (std::vector<CellRef>{third}));
  EXPECT_EQ(h.arena.asInt64(first), 7);
  EXPECT_EQ(h.arena.asInt64(second), 7);
  EXPECT_EQ(h.arena.asInt64(third), 7);
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VQLMutationGuardsTest, EarlierParameterGuardCoversMutationInLaterDefault) {
  Harness h;
  const CellRef value = h.arena.makeScalarCell(std::int64_t{12});
  constexpr std::string_view source =
      "def pure change($a: Int, $b: Cell := "
      "$a/value(\"bad\")): Cell := $a; change($cell)";

  const auto result = h.run(source, {{"cell", {value}}});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().category,
            QueryExecutionError::Category::TypeContract);
  EXPECT_NE(result.error().message.find("change"), std::string::npos);
  EXPECT_EQ(h.arena.asInt64(value), 12);
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VQLMutationGuardsTest,
     MutationGroupCycleLimitIsBudgetAndRollsBackTypedValueWrite) {
  Harness h;
  const CellRef value = h.arena.makeScalarCell(std::int64_t{12});
  h.vm.setGuardLimits(zigzag::vortex::GuardLimits{.maxMutationGroupCycles = 2});
  constexpr std::string_view source =
      "def weave set($cell: Int): Cell := $cell/value(7); set($cell)";

  const auto result = h.run(source, {{"cell", {value}}});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().category, QueryExecutionError::Category::Budget);
  EXPECT_EQ(h.arena.asInt64(value), 12);
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

} // namespace
} // namespace xanadu::vql

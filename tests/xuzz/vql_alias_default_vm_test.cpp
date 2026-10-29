/**
 * @file vql_alias_default_vm_test.cpp
 * @brief VM tests for lazy, identity-preserving VQL contract defaults.
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/contracts.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::VortexCore;
using zigzag::vortex::VortexVM;

struct Harness {
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  VortexVM vm{core};
};

std::expected<std::vector<CellRef>, QueryExecutionError>
run(Harness &h, const std::string_view query, ContractBindings bindings = {}) {
  QueryExecutor executor(h.coordinator, h.vm, {}, std::move(bindings));
  return executor.execute(query);
}

CellRef addRank(Harness &h, const CellRef from, const std::string_view name,
                const CellRef value) {
  const auto dim  = h.coordinator.resolveDimension(name);
  const auto tail = zigzag::rankTail(h.arena, from, dim);
  zigzag::expectWritten(h.arena.link(tail, dim, DimVector::POS, value));
  return value;
}

TEST(VQLAliasDefaultVMTest, DefaultAliasesEarlierParameterCellIdentity) {
  Harness h;
  const auto outer    = h.arena.makeCell("outer");
  const auto argument = h.arena.makeCell("parameter");
  const auto item     = h.arena.makeCell("item without field");
  ContractBindings bindings{
      {"fallback", {outer}}, {"argument", {argument}}, {"item", {item}}};

  const auto result =
      run(h,
          "def choose($fallback: Cell, $item: Cell { +d.value: $value: Cell := "
          "$fallback }): Cell := $value; choose($argument, $item)",
          std::move(bindings));

  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ(result->front(), argument);
  EXPECT_NE(result->front(), outer);
}

TEST(VQLAliasDefaultVMTest, OuterAliasWinsOverSameNamedPatternCapture) {
  Harness h;
  const auto fallback = h.arena.makeCell("outer fallback");
  const auto item     = h.arena.makeCell("item without field");
  ContractBindings bindings{{"fallback", {fallback}}, {"item", {item}}};

  const auto result =
      run(h,
          "def choose({ .: $fallback, +d.value: $value: Cell? := $fallback }): "
          "Cell? := $value; choose($item)",
          std::move(bindings));

  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ(result->front(), fallback);
}

TEST(VQLAliasDefaultVMTest, PresentFieldSkipsEmptyAliasAndKeepsFieldIdentity) {
  Harness h;
  const auto item   = h.arena.makeCell("item with field");
  const auto actual = h.arena.makeCell("actual field");
  addRank(h, item, "d.value", actual);
  ContractBindings bindings{{"fallback", {}}, {"item", {item}}};

  const auto result =
      run(h,
          "def choose($item: Cell { +d.value: $value: Cell? := $fallback }): "
          "Cell? := $value; choose($item)",
          std::move(bindings));

  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ(result->front(), actual);
}

TEST(VQLAliasDefaultVMTest, LoopBindingAliasChangesWithEachRuntimeIteration) {
  Harness h;
  const auto firstFallback  = h.arena.makeCell("first fallback");
  const auto secondFallback = h.arena.makeCell("second fallback");
  const auto missingField   = h.arena.makeCell("item without field");
  addRank(h, h.core.home(), "d.fallbacks", firstFallback);
  addRank(h, h.core.home(), "d.fallbacks", secondFallback);
  addRank(h, h.core.home(), "d.items", missingField);

  const auto result = run(
      h, "def choose($fallback: Cell, $item: Cell { +d.value: $value: Cell := "
         "$fallback }): Cell := $value; for $fallback in ##/d.fallbacks for "
         "$item in ##/d.items return choose($fallback, $item)");

  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{firstFallback, secondFallback}));
}

TEST(VQLAliasDefaultVMTest, EmptyAliasStaysEmptyWhenFieldIsMissing) {
  Harness h;
  const auto item = h.arena.makeCell("item without field");
  ContractBindings bindings{{"fallback", {}}, {"item", {item}}};
  const auto result =
      run(h,
          "def choose($item: Cell { +d.value: $value: Cell? := $fallback }): "
          "Cell? := $value; choose($item)",
          std::move(bindings));

  ASSERT_TRUE(result) << result.error().message;
  EXPECT_TRUE(result->empty());
}

TEST(VQLAliasDefaultVMTest, UnsupportedDefaultExpressionsRefuseCompilation) {
  const std::vector<std::string> queries{
      "def choose($item: Cell { +d.value: $value: Cell := ##/d.value }): "
      "Cell := $value; choose(##/d.item)",
      "def fallback(): Cell := ##/d.value; def choose($item: Cell { "
      "+d.value: $value: Cell := fallback() }): Cell := $value; "
      "choose(##/d.item)",
      "def choose($item: Cell { +d.value: $value: Cell := $missing }): Cell "
      ":= $value; choose(##/d.item)",
      "def choose($item: Cell { +d.value: $value: Cell := $fallback }): "
      "Cell := $value; let $fallback := /+d.step; choose(##/d.item)",
  };
  for (const auto &query : queries) {
    SCOPED_TRACE(query);
    Harness h;
    const auto result = run(h, query);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().category, QueryExecutionError::Category::Compile);
    EXPECT_FALSE(result.error().message.empty());
  }
}

TEST(VQLAliasDefaultVMTest, KnownMultiCellAliasRefusesCompilation) {
  Harness h;
  const auto first  = h.arena.makeCell("first");
  const auto second = h.arena.makeCell("second");
  ContractBindings bindings{{"fallback", {first, second}},
                            {"item", {h.arena.makeCell("item")}}};

  const auto result =
      run(h,
          "def choose($item: Cell { +d.value: $value: Cell := $fallback }): "
          "Cell := $value; choose($item)",
          std::move(bindings));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().category, QueryExecutionError::Category::Compile);
}

TEST(VQLAliasDefaultVMTest, RuntimeMultiCellAliasRefusesContractMatch) {
  Harness h;
  const auto first  = h.arena.makeCell("first");
  const auto second = h.arena.makeCell("second");
  addRank(h, h.core.home(), "d.fallbacks", first);
  addRank(h, h.core.home(), "d.fallbacks", second);

  const auto result =
      run(h, "def choose($item: Cell { +d.value: $value: Cell := $fallback }): "
             "Cell := $value; let $fallback := ##/d.fallbacks; "
             "let $item := %\"item\"; choose($item)");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().category, QueryExecutionError::Category::Runtime);
  EXPECT_NE(result.error().message.find("one cell"), std::string::npos);
}

TEST(VQLAliasDefaultVMTest, ExistingFieldDoesNotResolveMultiCellAlias) {
  Harness h;
  const auto first  = h.arena.makeCell("first");
  const auto second = h.arena.makeCell("second");
  const auto item   = h.arena.makeCell("item with field");
  const auto actual = h.arena.makeCell("actual field");
  addRank(h, h.core.home(), "d.fallbacks", first);
  addRank(h, h.core.home(), "d.fallbacks", second);
  addRank(h, item, "d.value", actual);

  ContractBindings bindings{{"item", {item}}};
  const auto result =
      run(h,
          "def choose($item: Cell { +d.value: $value: Cell := $fallback }): "
          "Cell := $value; let $fallback := ##/d.fallbacks; choose($item)",
          std::move(bindings));

  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ(result->front(), actual);
}

} // namespace
} // namespace xanadu::vql

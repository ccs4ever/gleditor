/**
 * @file vql_closed_functor_frames_test.cpp
 * @brief Closed point-free traversal functors in reusable VQL frames.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/mesh_evaluator.hpp"
#include "common/xanadu/vortex/vortex_host.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/frame_eligibility.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};

  DimRef dim(const std::string_view name) {
    return coordinator.resolveDimension(name);
  }

  CellRef cell(const std::string_view value) { return arena.makeCell(value); }

  void link(const CellRef from, const std::string_view dimension,
            const CellRef to) {
    zigzag::expectWritten(arena.link(from, dim(dimension), DimVector::POS, to));
  }

  CompilationResult compile(const std::string_view source,
                            CompilationOptions options = {}) {
    VQLCompiler compiler(coordinator, vm);
    return compiler.compile(source, options);
  }

  std::expected<std::vector<CellRef>, QueryExecutionError>
  execute(const std::string_view source, ContractBindings bindings = {}) {
    QueryExecutor executor(coordinator, vm, {}, std::move(bindings));
    return executor.execute(source);
  }
};

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_closed_functor_frames_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

std::vector<CellRef> invokeDescriptors(const CompilationResult &compiled,
                                       const zigzag::vortex::VortexVM &vm,
                                       const zigzag::vortex::VortexCore &core,
                                       const std::string_view function) {
  std::vector<CellRef> result;
  const std::string marker = "#INVOKE_FRAME " + std::string(function);
  for (const CellRef opcode : compiled.generatedOpcodes) {
    if (vm.getOpcodeKind(opcode) != OpcodeKind::InvokeFrame ||
        core.arena().textOf(opcode).find(marker) == std::string::npos)
      continue;
    const auto inputs = core.inputsOf(opcode);
    if (!inputs.empty()) result.push_back(inputs.front());
  }
  return result;
}

std::size_t countInvokes(const CompilationResult &compiled,
                         const zigzag::vortex::VortexVM &vm) {
  std::size_t count = 0;
  for (const CellRef opcode : compiled.generatedOpcodes)
    count += vm.getOpcodeKind(opcode) == OpcodeKind::InvokeFrame;
  return count;
}

TEST(VQLClosedFunctorFrameTest, ClosedFormalIsEligibleForNativeRecursiveGraph) {
  constexpr std::string_view source =
      "def pure walk($nodes: Stream<Cell>, $depth: Int, $route): "
      "Stream<Cell> := { if $depth > 0 return walk($nodes/$route, "
      "$depth + -1, $route) else return $nodes; }; "
      "walk(##/+d.items, 1, /+d.next/+d.other)";
  const auto query    = Parser(source).parseQuery();
  const auto analysis = analyzeFrameEligibility(query);
  ASSERT_TRUE(analysis) << analysis.error().message;
  EXPECT_TRUE(analysis->reusableDefinitions.contains("walk"));
  EXPECT_TRUE(analysis->freeCaptureNames.at("walk").empty());

  Harness h;
  const CellRef first  = h.cell("first");
  const CellRef second = h.cell("second");
  const CellRef third  = h.cell("third");
  h.link(h.core.home(), "d.items", first);
  h.link(first, "d.items", second);
  h.link(second, "d.items", third);
  const CellRef firstNext  = h.cell("first next");
  const CellRef secondNext = h.cell("second next");
  const CellRef thirdNext  = h.cell("third next");
  h.link(first, "d.next", firstNext);
  h.link(second, "d.next", secondNext);
  h.link(third, "d.next", thirdNext);
  const CellRef firstOther  = h.cell("first other");
  const CellRef secondOther = h.cell("second other");
  const CellRef thirdOther  = h.cell("third other");
  h.link(firstNext, "d.other", firstOther);
  h.link(secondNext, "d.other", secondOther);
  h.link(thirdNext, "d.other", thirdOther);

  const auto compiled = h.compile(source);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_GT(countInvokes(compiled, h.vm), 0U);
  const auto descriptors = invokeDescriptors(compiled, h.vm, h.core, "walk");
  ASSERT_FALSE(descriptors.empty());
  const auto descriptor = h.vm.frameDescriptor(descriptors.front());
  ASSERT_TRUE(descriptor) << descriptor.error().message;
  ASSERT_EQ(descriptor->argumentPorts.size(), 4U);
  for (const auto &port : descriptor->argumentPorts)
    EXPECT_EQ(port.kind, zigzag::vortex::FramePortKind::Stream);
  EXPECT_NE(
      std::ranges::find(descriptor->locals, descriptor->argumentPorts[3].slot),
      descriptor->locals.end());
  const auto result = h.execute(source);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result,
            (std::vector<CellRef>{firstOther, secondOther, thirdOther}));
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VQLClosedFunctorFrameTest, SelfAndMutualRecursionForwardTemplateInVM) {
  Harness h;
  const CellRef a = h.cell("a");
  const CellRef b = h.cell("b");
  const CellRef c = h.cell("c");
  h.link(h.core.home(), "d.items", a);
  h.link(a, "d.items", b);
  h.link(b, "d.items", c);
  for (const CellRef node : {a, b, c}) {
    const CellRef first       = h.cell("first");
    const CellRef second      = h.cell("second");
    const CellRef secondNext  = h.cell("first");
    const CellRef secondOther = h.cell("second");
    h.link(node, "d.next", first);
    h.link(first, "d.other", second);
    h.link(second, "d.next", secondNext);
    h.link(secondNext, "d.other", secondOther);
  }

  constexpr std::string_view self =
      "def pure walk($nodes: Stream<Cell>, $depth: Int, $route): "
      "Stream<Cell> := { if $depth > 0 return walk($nodes/$route, "
      "$depth + -1, $route) else return $nodes; }; "
      "walk(##/+d.items, 2, /+d.next/+d.other)";
  const auto selfResult = h.execute(self);
  ASSERT_TRUE(selfResult) << selfResult.error().message;
  ASSERT_EQ(selfResult->size(), 3U);
  for (const CellRef value : *selfResult)
    EXPECT_EQ(h.arena.textOf(value), "second");

  constexpr std::string_view mutual =
      "def pure even($nodes: Stream<Cell>, $depth: Int, $route): "
      "Stream<Cell> := { if $depth > 0 return odd($nodes/$route, "
      "$depth + -1, $route) else return $nodes; }; "
      "def pure odd($nodes: Stream<Cell>, $depth: Int, $route): "
      "Stream<Cell> := { if $depth > 0 return even($nodes/$route, "
      "$depth + -1, $route) else return $nodes; }; "
      "even(##/+d.items, 2, /+d.next/+d.other)";
  const auto mutualResult = h.execute(mutual);
  ASSERT_TRUE(mutualResult) << mutualResult.error().message;
  ASSERT_EQ(mutualResult->size(), 3U);
  for (const CellRef value : *mutualResult)
    EXPECT_EQ(h.arena.textOf(value), "second");
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VQLClosedFunctorFrameTest, DescriptorKeyDeduplicatesOnlyTheSameTemplate) {
  Harness h;
  const CellRef node  = h.cell("node");
  const CellRef next  = h.cell("next");
  const CellRef other = h.cell("other");
  h.link(h.core.home(), "d.items", node);
  h.link(node, "d.next", next);
  h.link(node, "d.other", other);
  constexpr std::string_view source = R"(
    def pure walk($nodes: Stream<Cell>, $depth: Int, $route): Stream<Cell> := {
      if $depth > 0 return walk($nodes/$route, $depth + -1, $route)
      else return $nodes;
    };
    let $same := walk(##/+d.items, 1, /+d.next);
    let $different := walk(##/+d.items, 1, /+d.other);
    let $again := walk(##/+d.items, 1, /+d.next);
    return $same, $different, $again
  )";
  const auto compiled               = h.compile(source);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  const auto descriptors = invokeDescriptors(compiled, h.vm, h.core, "walk");
  ASSERT_GE(descriptors.size(), 3U);
  std::map<CellRef, std::size_t> descriptorUseCounts;
  for (const CellRef descriptor : descriptors)
    ++descriptorUseCounts[descriptor];
  ASSERT_EQ(descriptorUseCounts.size(), 2U);
  EXPECT_TRUE(std::ranges::any_of(descriptorUseCounts, [](const auto &entry) {
    return entry.second > 1U;
  }));

  const auto result = h.execute(source);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{next, other, next}));
}

TEST(VQLClosedFunctorFrameTest, TypedRestArityRemainsPartOfTemplateFrameKey) {
  Harness h;
  const CellRef node = h.cell("node");
  const CellRef next = h.cell("next");
  h.link(h.core.home(), "d.items", node);
  h.link(node, "d.next", next);
  constexpr std::string_view source = R"(
    def pure walk($nodes: Stream<Cell>, $depth: Int, $route,
                  ...$values: Stream<Int>): Stream<Cell> := {
      if $depth > 0 return walk($nodes/$route, $depth + -1, $route, $values)
      else return $nodes;
    };
    let $withRest := walk(##/+d.items, 1, /+d.next, 7);
    let $withoutRest := walk(##/+d.items, 1, /+d.next);
    return $withRest, $withoutRest
  )";
  const auto compiled               = h.compile(source);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  const auto descriptors = invokeDescriptors(compiled, h.vm, h.core, "walk");
  std::map<CellRef, std::size_t> descriptorUseCounts;
  for (const CellRef descriptor : descriptors)
    ++descriptorUseCounts[descriptor];
  EXPECT_GE(descriptorUseCounts.size(), 2U);

  const auto result = h.execute(source);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{next, next}));
}

TEST(VQLClosedFunctorFrameTest, EqualTemplatesBindToTheirOwnFormalPositions) {
  Harness h;
  const CellRef node   = h.cell("node");
  const CellRef middle = h.cell("middle");
  const CellRef end    = h.cell("end");
  h.link(h.core.home(), "d.items", node);
  h.link(node, "d.next", middle);
  h.link(middle, "d.next", end);
  constexpr std::string_view source = R"(
    def pure walk($nodes: Stream<Cell>, $depth: Int, $first, $second):
      Stream<Cell> := {
        if $depth > 0
          return walk($nodes/$first/$second, $depth + -1, $first, $second)
        else return $nodes;
      };
    walk(##/+d.items, 1, /+d.next, /+d.next)
  )";
  const auto compiled               = h.compile(source);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_GT(countInvokes(compiled, h.vm), 0U);
  const auto result = h.execute(source);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{end}));
}

TEST(VQLClosedFunctorFrameTest,
     PreparedArgumentsStayInCallerScopeAndFormalOrderIsStable) {
  Harness h;
  const CellRef start  = h.cell("start");
  const CellRef first  = h.cell("first");
  const CellRef second = h.cell("second");
  h.link(h.core.home(), "d.items", start);
  h.link(start, "d.first", first);
  h.link(start, "d.second", second);

  constexpr std::string_view source = R"(
    def pure apply($nodes: Stream<Cell>, $route, $offset: Int):
      Stream<Cell> := $nodes/$route;
    let $route := /+d.first;
    let $before := apply(##/+d.items, $route, 7);
    let $route := /+d.second;
    let $after := apply(##/+d.items, $route, 9);
    return $before, $after
  )";
  const auto result                 = h.execute(source);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{first, second}));

  const auto notAppliedAsAnArgument =
      h.execute("def pure lazy($nodes: Stream<Cell>, $depth: Int, $route): "
                "Stream<Cell> := { if $depth > 0 return lazy($nodes/$route, "
                "$depth + -1, $route) else return $nodes; }; "
                "lazy(##/+d.items, 0, /+d.first)");
  ASSERT_TRUE(notAppliedAsAnArgument) << notAppliedAsAnArgument.error().message;
  EXPECT_EQ(*notAppliedAsAnArgument, (std::vector<CellRef>{start}));

  const auto reordered =
      h.execute("def pure apply($nodes: Stream<Cell>, $route, $offset: Int): "
                "Stream<Cell> := $nodes/$route; "
                "apply(##/+d.items, /+d.second, 7)");
  ASSERT_TRUE(reordered) << reordered.error().message;
  EXPECT_EQ(*reordered, (std::vector<CellRef>{second}));

  const CellRef callerValue = h.cell("caller value");
  const CellRef replacement = h.cell("callee formal value");
  const auto prepared       = h.execute(
      "def pure capture(): Cell := $first; "
            "def pure apply($first: Cell, $route, $result: Cell): Cell := $result; "
            "apply($replacement, /+d.first, capture())",
      {{"first", {callerValue}}, {"replacement", {replacement}}});
  ASSERT_TRUE(prepared) << prepared.error().message;
  EXPECT_EQ(*prepared, (std::vector<CellRef>{callerValue}));
}

TEST(VQLClosedFunctorFrameTest,
     OneFormalUFCSFunctorCompilesWithContextAndEmptyFallback) {
  Harness h;
  const CellRef node = h.cell("node");
  const CellRef next = h.cell("next");
  h.link(node, "d.next", next);
  constexpr std::string_view source =
      "def pure via($step): Stream<Cell> := ./$step; "
      "$items/via(/+d.next)";
  const auto query       = Parser(source).parseQuery();
  const auto eligibility = analyzeFrameEligibility(query);
  ASSERT_TRUE(eligibility) << eligibility.error().message;
  EXPECT_TRUE(eligibility->reusableDefinitions.contains("via"));
  EXPECT_EQ(eligibility->functorFormalIndices.at("via"),
            (std::vector<std::size_t>{0}));

  CompilationOptions options;
  options.initialBindings.emplace("items", std::vector<CellRef>{});
  const auto compiled = h.compile(source, options);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  const auto result = h.execute(source, {{"items", {node}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{next}));

  const auto empty = h.execute(source, {{"items", {}}});
  ASSERT_TRUE(empty) << empty.error().message;
  EXPECT_TRUE(empty->empty());
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VQLClosedFunctorFrameTest,
     FunctorApplicationPreservesIdentityOrderAndDuplicates) {
  Harness h;
  const CellRef first      = h.cell("first");
  const CellRef second     = h.cell("second");
  const CellRef firstNext  = h.cell("first next");
  const CellRef secondNext = h.cell("second next");
  h.link(first, "d.next", firstNext);
  h.link(second, "d.next", secondNext);

  const auto result =
      h.execute("def pure apply($nodes: Stream<Cell>, $route): Stream<Cell> := "
                "$nodes/$route; "
                "apply($inputs, /+d.next)",
                {{"inputs", {first, second, first}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{firstNext, secondNext, firstNext}));
}

TEST(VQLClosedFunctorFrameTest,
     OrdinaryFunctorCallsStayFiniteAndUnsupportedShapesRefuseRecursion) {
  Harness h;
  const CellRef node = h.cell("node");
  const CellRef next = h.cell("next");
  h.link(h.core.home(), "d.items", node);
  h.link(node, "d.next", next);

  const auto ordinary =
      h.compile("def pure apply($nodes: Stream<Cell>, $route): Stream<Cell> := "
                "$nodes/$route; apply(##/+d.items, /+d.next)");
  ASSERT_TRUE(ordinary.success) << ordinary.errorMessage;
  EXPECT_FALSE(invokeDescriptors(ordinary, h.vm, h.core, "apply").empty());
  const auto decorated =
      h.execute("def pure apply($nodes: Stream<Cell>, $route): Stream<Cell> := "
                "$nodes/$route; apply(##/+d.items, /+d.next[1])");
  ASSERT_TRUE(decorated) << decorated.error().message;
  EXPECT_EQ(*decorated, (std::vector<CellRef>{next}));

  const auto clampedRecursive =
      h.compile("def pure walk($nodes: Stream<Cell>, $route): Stream<Cell> := "
                "walk($nodes/$route, $route); walk(##/+d.items, /+d.next[1])");
  ASSERT_TRUE(clampedRecursive.success) << clampedRecursive.errorMessage;
  EXPECT_FALSE(
      invokeDescriptors(clampedRecursive, h.vm, h.core, "walk").empty());

  for (const std::string_view actual : {"/+d.next[. = \"next\"]"}) {
    const std::string source =
        "def pure walk($nodes: Stream<Cell>, $route): Stream<Cell> := "
        "walk($nodes/$route, $route); walk(##/+d.items, " +
        std::string(actual) + ")";
    const auto refused = h.compile(source);
    EXPECT_FALSE(refused.success) << source;
    EXPECT_FALSE(refused.cursor) << source;
  }

  const auto projectedArgument =
      h.compile("def pure hold($route): Stream<Cell> := $route; "
                "hold(/+d.next{+d.copy: 7}/+d.copy)");
  EXPECT_FALSE(projectedArgument.success);
  EXPECT_NE(projectedArgument.errorMessage.find("projections"),
            std::string::npos);

  const auto freeCapture =
      h.compile("def pure walk($nodes: Stream<Cell>): Stream<Cell> := "
                "walk($nodes/$outside); let $outside := /+d.next; "
                "walk(##/+d.items)");
  EXPECT_FALSE(freeCapture.success);
  EXPECT_FALSE(freeCapture.cursor);
  EXPECT_NE(freeCapture.errorMessage.find("recursive"), std::string::npos);
}

TEST(VQLClosedFunctorFrameTest,
     TemplateWorkLimitRejectsAtomicallyAllowsOrdinaryCallsAndRetries) {
  Harness h;
  constexpr std::string_view closed =
      "def pure walk($nodes: Stream<Cell>, $depth: Int, $route): "
      "Stream<Cell> := { if $depth > 0 return walk($nodes/$route, "
      "$depth + -1, $route) else return $nodes; }; "
      "walk(##, 1, /+d.next)";
  const std::size_t before = h.arena.cells().size();
  CompilationOptions limited;
  limited.limits.maxClosedFunctorTemplateWork = 0;
  const auto refused                          = h.compile(closed, limited);
  EXPECT_FALSE(refused.success);
  EXPECT_FALSE(refused.cursor);
  EXPECT_NE(refused.errorMessage.find(
                "compiled VQL closed functor template work budget exhausted"),
            std::string::npos);
  EXPECT_EQ(h.arena.cells().size(), before);

  CompilationOptions ordinaryLimited;
  ordinaryLimited.limits.maxClosedFunctorTemplateWork = 0;
  const auto ordinary =
      h.compile("def pure identity($value: Cell): Cell := $value; identity(##)",
                ordinaryLimited);
  ASSERT_TRUE(ordinary.success) << ordinary.errorMessage;

  CompilationOptions retry;
  retry.limits.maxClosedFunctorTemplateWork = 100'000;
  const auto accepted                       = h.compile(closed, retry);
  ASSERT_TRUE(accepted.success) << accepted.errorMessage;
  EXPECT_GT(countInvokes(accepted, h.vm), 0U);
}

TEST(VQLClosedFunctorFrameTest, TemplateWorkWeightsDimensionNameBytes) {
  const auto compileAtLimit = [](const std::string_view dimension,
                                 const std::size_t limit) {
    Harness h;
    const std::string source =
        "def pure walk($nodes: Stream<Cell>, $depth: Int, $route): "
        "Stream<Cell> := { if $depth > 0 return walk($nodes/$route, "
        "$depth + -1, $route) else return $nodes; }; "
        "walk(##, 1, /+" +
        std::string(dimension) + ")";
    CompilationOptions options;
    options.limits.maxClosedFunctorTemplateWork = limit;
    return h.compile(source, options);
  };

  const auto shortName = compileAtLimit("d.x", 200);
  ASSERT_TRUE(shortName.success) << shortName.errorMessage;
  const std::string longDimension = "d." + std::string(1000, 'x');
  const auto longName             = compileAtLimit(longDimension, 200);
  EXPECT_FALSE(longName.success);
  EXPECT_NE(longName.errorMessage.find(
                "compiled VQL closed functor template work budget exhausted"),
            std::string::npos);
}

TEST(VQLClosedFunctorFrameTest,
     RecursiveExportRunsAfterSourceVmAndAstAreDestroyed) {
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<UserPermascroll>(
      UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Harness h;
    const CellRef first  = h.cell("first");
    const CellRef second = h.cell("second");
    h.link(h.core.home(), "d.items", first);
    h.link(first, "d.items", second);
    const CellRef firstNext  = h.cell("first next");
    const CellRef secondNext = h.cell("second next");
    h.link(first, "d.next", firstNext);
    h.link(second, "d.next", secondNext);
    constexpr std::string_view source =
        "def pure walk($nodes: Stream<Cell>, $depth: Int, $route): "
        "Stream<Cell> := { if $depth > 0 return walk($nodes/$route, "
        "$depth + -1, $route) else return $nodes; }; "
        "walk(##/+d.items, 1, /+d.next)";
    VQLCompiler compiler(h.coordinator, h.vm);
    const auto compiled = compiler.compile(source);
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    ASSERT_GT(countInvokes(compiled, h.vm), 0U);
    Store store(scroll);
    compiler.exportToStore(store);
    store.save((temporary.path / "program").string());
  }

  Store store(scroll);
  ASSERT_NO_THROW(store.load((temporary.path / "program").string()));
  ASSERT_TRUE(store.structureHead());
  const auto manifold = store.rebuildManifold(*store.structureHead());
  zigzag::ArenaManifold arena(&manifold, store);
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  const auto entry =
      arena.linked(core.home(), core.dims().spin, DimVector::POS);
  ASSERT_TRUE(entry);
  const auto execution =
      vm.run(vm.spawnCursor(*entry, "closed-functor-reload"), 100'000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  auto halt = *entry;
  auto next = arena.linked(halt, core.dims().spin, DimVector::POS);
  while (next && vm.getOpcodeKind(*next) != OpcodeKind::Halt) {
    halt = *next;
    next = arena.linked(halt, core.dims().spin, DimVector::POS);
  }
  ASSERT_TRUE(next);
  const auto ports = core.inputsOf(*next);
  ASSERT_EQ(ports.size(), 1U);
  const auto values = vm.resultCells(ports.front());
  ASSERT_EQ(values.size(), 2U);
  EXPECT_EQ(arena.textOf(values.front()), "first next");
  EXPECT_EQ(arena.textOf(values.back()), "second next");
  EXPECT_FALSE(vm.hasActiveFrames());
}

TEST(VQLClosedFunctorFrameTest,
     HostAndMeshSourcesAcceptClosedRecursiveFunctors) {
  constexpr std::string_view source =
      "def pure visit($nodes: Stream<Cell>, $depth: Int, $route): "
      "Stream<Cell> := { if $depth > 0 return visit($nodes/$route, "
      "$depth + -1, $route) else return $nodes; }; "
      "let $owner := ##/d.seed%\"owner\"; "
      "let $leaf := $owner/d.next%\"leaf\"; "
      "visit($owner, 1, /+d.next)";
  zigzag::vortex::VortexHost host;
  const auto hostResult = host.executeVQLChecked(source);
  ASSERT_TRUE(hostResult) << hostResult.error().message;
  ASSERT_EQ(hostResult->size(), 1U);
  EXPECT_EQ(host.core().arena().textOf(hostResult->front()), "leaf");

  namespace mesh        = zigzag::vortex::mesh;
  const auto meshResult = mesh::evaluate(mesh::Frontend::Vquery, source,
                                         {.maxCycles      = 100'000,
                                          .maxCells       = 100'000,
                                          .maxSolutions   = 16,
                                          .maxOutputBytes = 1024 * 1024});
  ASSERT_TRUE(meshResult) << meshResult.error();
  auto &arena = meshResult->context->arena;
  std::optional<CellRef> items;
  std::optional<CellRef> value;
  bool sawVisitInvoke = false;
  for (const auto &cell : arena.cells()) {
    if (arena.textOf(cell.birthOp) == "d.mesh.items") items = cell.birthOp;
    if (arena.textOf(cell.birthOp) == "d.mesh.value") value = cell.birthOp;
    if (meshResult->context->vm.getOpcodeKind(cell.birthOp) ==
            OpcodeKind::InvokeFrame &&
        arena.textOf(cell.birthOp).starts_with("#INVOKE_FRAME visit"))
      sawVisitInvoke = true;
  }
  EXPECT_TRUE(sawVisitInvoke);
  ASSERT_TRUE(items);
  ASSERT_TRUE(value);
  const auto record = arena.linked(meshResult->root, *items, DimVector::POS);
  ASSERT_TRUE(record);
  const auto output = arena.linked(*record, *value, DimVector::POS);
  ASSERT_TRUE(output);
  EXPECT_EQ(arena.textOf(*output), "leaf");
  EXPECT_FALSE(arena.linked(*record, *items, DimVector::POS));
}

} // namespace
} // namespace xanadu::vql

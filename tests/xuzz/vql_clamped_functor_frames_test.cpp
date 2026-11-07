/**
 * @file vql_clamped_functor_frames_test.cpp
 * @brief Static range clamps in reusable closed-functor VQL frames.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <set>
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
  execute(const std::string_view source) {
    QueryExecutor executor(coordinator, vm);
    return executor.execute(source);
  }
};

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_clamped_functor_frames_" +
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

std::set<CellRef> uniqueFrameDescriptors(const CompilationResult &compiled,
                                         const zigzag::vortex::VortexVM &vm,
                                         const zigzag::vortex::VortexCore &core,
                                         const std::string_view function) {
  const auto descriptors = invokeDescriptors(compiled, vm, core, function);
  return {descriptors.begin(), descriptors.end()};
}

constexpr std::string_view recursiveSource =
    "def pure walk($nodes: Stream<Cell>, $depth: Int, $route): "
    "Stream<Cell> := { if $depth > 0 return walk($nodes/$route, "
    "$depth + -1, $route) else return $nodes; }; ";

TEST(VQLClampedFunctorFrameTest, NamedSelfAndMutualFramesWalkClampedRankOnce) {
  Harness h;
  std::vector<CellRef> nodes;
  std::vector<CellRef> firstTargets;
  for (int index = 0; index < 3; ++index) {
    const CellRef node = h.cell("node");
    if (nodes.empty())
      h.link(h.core.home(), "d.items", node);
    else
      h.link(nodes.back(), "d.items", node);
    nodes.push_back(node);
    const CellRef first  = h.cell("first target");
    const CellRef second = h.cell("second target");
    h.link(node, "d.next", first);
    h.link(first, "d.next", second);
    firstTargets.push_back(first);
  }

  const std::string self =
      std::string(recursiveSource) + "walk(##/+d.items, 1, /+d.next[1])";
  const auto compiled = h.compile(self);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_FALSE(invokeDescriptors(compiled, h.vm, h.core, "walk").empty());
  const auto clamped = h.execute(self);
  ASSERT_TRUE(clamped) << clamped.error().message;
  EXPECT_EQ(*clamped, (std::vector<CellRef>{firstTargets.front()}));

  const std::string mapped =
      std::string(recursiveSource) +
      "##/+d.items / \\($node) walk($node, 1, /+d.next[1])";
  const auto mappedResult = h.execute(mapped);
  ASSERT_TRUE(mappedResult) << mappedResult.error().message;
  EXPECT_EQ(*mappedResult, firstTargets);

  const std::string plain =
      std::string(recursiveSource) + "walk(##/+d.items, 1, /+d.next)";
  const auto all = h.execute(plain);
  ASSERT_TRUE(all) << all.error().message;
  EXPECT_EQ(all->size(), 2U * nodes.size());

  constexpr std::string_view mutual =
      "def pure even($nodes: Stream<Cell>, $depth: Int, $route): "
      "Stream<Cell> := { if $depth > 0 return odd($nodes/$route, "
      "$depth + -1, $route) else return $nodes; }; "
      "def pure odd($nodes: Stream<Cell>, $depth: Int, $route): "
      "Stream<Cell> := { if $depth > 0 return even($nodes/$route, "
      "$depth + -1, $route) else return $nodes; }; "
      "even(##/+d.items, 1, /+d.next[1])";
  const auto mutualCompiled = h.compile(mutual);
  ASSERT_TRUE(mutualCompiled.success) << mutualCompiled.errorMessage;
  EXPECT_FALSE(invokeDescriptors(mutualCompiled, h.vm, h.core, "even").empty());
  const auto mutualResult = h.execute(mutual);
  ASSERT_TRUE(mutualResult) << mutualResult.error().message;
  EXPECT_EQ(*mutualResult, (std::vector<CellRef>{firstTargets.front()}));
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VQLClampedFunctorFrameTest,
     ClampAndStepIdentityRemainDistinctAndIdenticalKeysDeduplicate) {
  Harness h;
  constexpr std::string_view source = R"(
    def pure walk($nodes: Stream<Cell>, $depth: Int, $route):
      Stream<Cell> := {
        if $depth > 0 return walk($nodes/$route, $depth + -1, $route)
        else return $nodes;
      };
    let $plain := walk(##, 0, /+d.next);
    let $single := walk(##, 0, /+d.next[1]);
    let $singleAgain := walk(##, 0, /+d.next[1]);
    let $explicitRange := walk(##, 0, /+d.next[1, 1]);
    let $range := walk(##, 0, /+d.next[2, 3]);
    let $negative := walk(##, 0, /+d.next[-1]);
    let $wideIndex := walk(##, 0, /+d.next[4294967297]);
    let $wideDimension := walk(##, 0, /+d.width.4294967297);
    let $maximum := walk(##, 0, /+d.next[9223372036854775807]);
    return $plain, $single, $singleAgain, $explicitRange, $range,
           $negative, $wideIndex, $wideDimension, $maximum
  )";
  const auto compiled               = h.compile(source);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  const auto descriptors =
      uniqueFrameDescriptors(compiled, h.vm, h.core, "walk");
  // `[n]` normalizes to the same AST clamp as `[n, n]`; the missing clamp,
  // other bounds, wider index, wider dimension and int64 upper edge do not.
  EXPECT_EQ(descriptors.size(), 7U);
  const auto raw = invokeDescriptors(compiled, h.vm, h.core, "walk");
  EXPECT_GT(raw.size(), descriptors.size());
}

TEST(VQLClampedFunctorFrameTest, ClampAndDimensionKeepTheirFullWidthValues) {
  constexpr std::string_view source = R"(
    def pure walk($nodes: Stream<Cell>, $depth: Int, $route):
      Stream<Cell> := {
        if $depth > 0 return walk($nodes/$route, $depth + -1, $route)
        else return $nodes;
      };
    walk(##, 0, /+d.width.4294967297[9223372036854775807])
  )";
  Harness h;
  const auto compiled = h.compile(source);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  const auto descriptors = invokeDescriptors(compiled, h.vm, h.core, "walk");
  ASSERT_FALSE(descriptors.empty());
  const auto descriptor = h.vm.frameDescriptor(descriptors.front());
  ASSERT_TRUE(descriptor) << descriptor.error().message;
  EXPECT_EQ(descriptor->argumentPorts.size(), 4U);
  for (const auto &port : descriptor->argumentPorts)
    EXPECT_EQ(port.kind, zigzag::vortex::FramePortKind::Stream);

  const CellRef owner = h.cell("owner");
  const CellRef first = h.cell("first");
  const CellRef last  = h.cell("last");
  const auto axis     = h.dim("d.width.4294967297");
  EXPECT_GT(axis, std::numeric_limits<std::uint16_t>::max());
  h.link(h.core.home(), "d.items", owner);
  h.link(owner, "d.width.4294967297", first);
  h.link(first, "d.width.4294967297", last);
  for (const std::string_view index :
       {"1", "-1", "4294967297", "-4294967297"}) {
    const std::string program = std::string(recursiveSource) +
                                "walk(##/+d.items, 1, /+d.width.4294967297[" +
                                std::string(index) + "])";
    const auto result = h.execute(program);
    ASSERT_TRUE(result) << result.error().message;
    if (index == "1" || index == "-4294967297")
      EXPECT_EQ(*result, (std::vector<CellRef>{first}));
    else
      EXPECT_EQ(*result, (std::vector<CellRef>{last}));
  }
}

TEST(VQLClampedFunctorFrameTest,
     ClampWorkBudgetRefusesAtomicallyAndAllowsOrdinaryRetry) {
  Harness h;
  const auto before = h.arena.cells().size();
  const std::string clamped =
      std::string(recursiveSource) + "walk(##, 1, /+d.next[1, 2])";
  CompilationOptions zero;
  zero.limits.maxClosedFunctorTemplateWork = 0;
  const auto refused                       = h.compile(clamped, zero);
  EXPECT_FALSE(refused.success);
  EXPECT_FALSE(refused.cursor);
  EXPECT_NE(refused.errorMessage.find(
                "compiled VQL closed functor template work budget exhausted"),
            std::string::npos);
  EXPECT_EQ(h.arena.cells().size(), before);

  CompilationOptions ordinaryZero;
  ordinaryZero.limits.maxClosedFunctorTemplateWork = 0;
  const auto ordinary =
      h.compile("def pure identity($value: Cell): Cell := $value; identity(##)",
                ordinaryZero);
  ASSERT_TRUE(ordinary.success) << ordinary.errorMessage;

  CompilationOptions retry;
  retry.limits.maxClosedFunctorTemplateWork = 100'000;
  const auto accepted                       = h.compile(clamped, retry);
  ASSERT_TRUE(accepted.success) << accepted.errorMessage;
  EXPECT_FALSE(invokeDescriptors(accepted, h.vm, h.core, "walk").empty());
}

TEST(VQLClampedFunctorFrameTest,
     RecursiveExportRunsAfterSourceVmAndAstAreDestroyed) {
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<UserPermascroll>(
      UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Harness h;
    const CellRef node   = h.cell("node");
    const CellRef first  = h.cell("first");
    const CellRef second = h.cell("second");
    h.link(h.core.home(), "d.items", node);
    h.link(node, "d.next", first);
    h.link(first, "d.next", second);
    const std::string source =
        std::string(recursiveSource) + "walk(##/+d.items, 1, /+d.next[1])";
    VQLCompiler compiler(h.coordinator, h.vm);
    const auto compiled = compiler.compile(source);
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    ASSERT_FALSE(invokeDescriptors(compiled, h.vm, h.core, "walk").empty());
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
      vm.run(vm.spawnCursor(*entry, "clamped-functor-reload"), 100'000);
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
  ASSERT_EQ(values.size(), 1U);
  EXPECT_EQ(arena.textOf(values.front()), "first");
  EXPECT_FALSE(vm.hasActiveFrames());
}

TEST(VQLClampedFunctorFrameTest, HostAndMeshExecuteRecursiveClampedSource) {
  constexpr std::string_view source =
      "def pure visit($nodes: Stream<Cell>, $depth: Int, $route): "
      "Stream<Cell> := { if $depth > 0 return visit($nodes/$route, "
      "$depth + -1, $route) else return $nodes; }; "
      "let $owner := ##/d.seed%\"owner\"; "
      "let $first := $owner/d.next%\"first\"; "
      "let $second := $first/d.next%\"second\"; "
      "visit($owner, 1, /+d.next[1])";
  zigzag::vortex::VortexHost host;
  const auto hostResult = host.executeVQLChecked(source);
  ASSERT_TRUE(hostResult) << hostResult.error().message;
  ASSERT_EQ(hostResult->size(), 1U);
  EXPECT_EQ(host.core().arena().textOf(hostResult->front()), "first");

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
  EXPECT_EQ(arena.textOf(*output), "first");
  EXPECT_FALSE(arena.linked(*record, *items, DimVector::POS));
}

} // namespace
} // namespace xanadu::vql

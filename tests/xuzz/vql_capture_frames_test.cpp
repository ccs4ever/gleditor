#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
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
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_capture_frames_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

struct RunResult {
  bool compiled{false};
  bool executed{false};
  std::string error;
  std::vector<CellRef> values;
  std::size_t invokeOps{0};
};

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};
  DimRef next{coordinator.resolveDimension("d.next")};
  CellRef first{arena.makeCell("first")};
  CellRef second{arena.makeCell("second")};
  CellRef third{arena.makeCell("third")};

  Harness() {
    zigzag::expectWritten(arena.link(first, next, DimVector::POS, second));
    zigzag::expectWritten(arena.link(second, next, DimVector::POS, third));
  }

  RunResult run(const std::string_view source, ContractBindings bindings = {}) {
    VQLCompiler compiler(coordinator, vm);
    CompilationOptions options;
    options.initialBindings = std::move(bindings);
    const auto compiled     = compiler.compile(source, options);
    RunResult result{.compiled = compiled.success};
    if (!compiled.success) {
      result.error = compiled.errorMessage;
      return result;
    }
    for (const auto opcode : compiled.generatedOpcodes)
      result.invokeOps += vm.getOpcodeKind(opcode) == OpcodeKind::InvokeFrame;
    if (!compiled.cursor) {
      result.error = "compiled query has no cursor";
      return result;
    }
    const auto execution = vm.run(*compiled.cursor, 100'000);
    result.executed      = execution.success;
    result.error         = execution.errorMessage;
    vm.setCursorOpcode(*compiled.cursor, std::nullopt);
    if (!execution.success) return result;
    for (const auto slot : compiled.resultSlots) {
      const auto values = vm.resultCells(slot);
      result.values.insert(result.values.end(), values.begin(), values.end());
    }
    return result;
  }

  RunResult compileOnly(const std::string_view source,
                        ContractBindings bindings = {}) {
    VQLCompiler compiler(coordinator, vm);
    CompilationOptions options;
    options.initialBindings = std::move(bindings);
    const auto compiled     = compiler.compile(source, options);
    RunResult result{.compiled = compiled.success};
    result.error = compiled.errorMessage;
    for (const auto opcode : compiled.generatedOpcodes)
      result.invokeOps += vm.getOpcodeKind(opcode) == OpcodeKind::InvokeFrame;
    return result;
  }
};

TEST(VQLCaptureFrameTest,
     DirectScalarCapturePreservesIdentityAndMissingStreamIsEmpty) {
  Harness h;
  const auto direct = h.run("def pure read(): Cell := $outside; read()",
                            {{"outside", {h.second}}});
  ASSERT_TRUE(direct.compiled) << direct.error;
  ASSERT_TRUE(direct.executed) << direct.error;
  EXPECT_GT(direct.invokeOps, 0u);
  EXPECT_EQ(direct.values, (std::vector<CellRef>{h.second}));

  const auto missing =
      h.run("def pure read(): Stream<Cell> := { for $item in $outside "
            "return $item; }; read()");
  ASSERT_TRUE(missing.compiled) << missing.error;
  ASSERT_TRUE(missing.executed) << missing.error;
  EXPECT_TRUE(missing.values.empty());
}

TEST(VQLCaptureFrameTest,
     FreeStreamCaptureKeepsEmptyOrderAndDuplicatesPerCall) {
  Harness h;
  constexpr std::string_view source =
      "def pure echo(): Stream<Cell> := { for $item in $outside "
      "return $item; }; echo()";

  const auto empty = h.run(source, {{"outside", {}}});
  ASSERT_TRUE(empty.compiled) << empty.error;
  ASSERT_TRUE(empty.executed) << empty.error;
  EXPECT_TRUE(empty.values.empty());

  const auto values =
      h.run(source, {{"outside", {h.first, h.second, h.first}}});
  ASSERT_TRUE(values.compiled) << values.error;
  ASSERT_TRUE(values.executed) << values.error;
  EXPECT_GT(values.invokeOps, 0u);
  EXPECT_EQ(values.values, (std::vector<CellRef>{h.first, h.second, h.first}));
}

TEST(VQLCaptureFrameTest,
     RecursiveAndMutuallyRecursiveFramesReceiveFreeCellCapture) {
  Harness h;
  const CellRef root = h.arena.makeCell("root");
  zigzag::expectWritten(h.arena.link(root, h.next, DimVector::POS, h.first));
  const CellRef outside = h.arena.makeCell("outside");

  const auto recursive =
      h.run("def pure walk($node: Cell): Cell := { if $node/+d.next[1] "
            "return walk($node/+d.next[1]) else return $outside; }; "
            "walk($root)",
            {{"outside", {outside}}, {"root", {root}}});
  ASSERT_TRUE(recursive.compiled) << recursive.error;
  ASSERT_TRUE(recursive.executed) << recursive.error;
  EXPECT_GT(recursive.invokeOps, 0u);
  EXPECT_EQ(recursive.values, (std::vector<CellRef>{outside}));

  const auto mutual =
      h.run("def pure even($node: Cell): Cell := { if $node/+d.next[1] "
            "return odd($node/+d.next[1]) else return $outside; }; "
            "def pure odd($node: Cell): Cell := { if $node/+d.next[1] "
            "return even($node/+d.next[1]) else return $outside; }; "
            "even($root)",
            {{"outside", {outside}}, {"root", {root}}});
  ASSERT_TRUE(mutual.compiled) << mutual.error;
  ASSERT_TRUE(mutual.executed) << mutual.error;
  EXPECT_GT(mutual.invokeOps, 0u);
  EXPECT_EQ(mutual.values, (std::vector<CellRef>{outside}));

  h.vm.setFrameLimits({.maxDepth = 1});
  const auto limited =
      h.run("def pure walk($node: Cell): Cell := { if $node/+d.next[1] "
            "return walk($node/+d.next[1]) else return $outside; }; "
            "walk($root)",
            {{"outside", {outside}}, {"root", {root}}});
  ASSERT_TRUE(limited.compiled) << limited.error;
  EXPECT_FALSE(limited.executed);
  EXPECT_NE(limited.error.find("frame depth"), std::string::npos);
}

TEST(VQLCaptureFrameTest,
     WrapperFormalsAndTransitiveHelpersSupplyCallSiteCapture) {
  Harness h;
  const auto direct = h.run("def pure leaf(): Cell := $outside; "
                            "def pure wrapper($outside: Cell): Cell := leaf(); "
                            "wrapper($candidate)",
                            {{"candidate", {h.second}}});
  ASSERT_TRUE(direct.compiled) << direct.error;
  ASSERT_TRUE(direct.executed) << direct.error;
  EXPECT_GT(direct.invokeOps, 0u);
  EXPECT_EQ(direct.values, (std::vector<CellRef>{h.second}));

  const auto transitive =
      h.run("def pure leaf(): Cell := $outside; "
            "def pure middle(): Cell := leaf(); "
            "def pure wrapper($outside: Cell): Cell := middle(); "
            "wrapper($candidate)",
            {{"candidate", {h.third}}});
  ASSERT_TRUE(transitive.compiled) << transitive.error;
  ASSERT_TRUE(transitive.executed) << transitive.error;
  EXPECT_GT(transitive.invokeOps, 0u);
  EXPECT_EQ(transitive.values, (std::vector<CellRef>{h.third}));
}

TEST(VQLCaptureFrameTest,
     CallerBindingsAreRecomputedAcrossCallsAndNestedFrames) {
  Harness h;
  VQLEngine session(h.coordinator);
  session.setVariable("outside", h.first);
  QueryExecutor executor(session, h.vm);
  constexpr std::string_view source =
      "def pure leaf(): Cell := $outside; "
      "def pure wrapper(): Cell := leaf(); wrapper()";

  const auto first = executor.execute(source);
  ASSERT_TRUE(first) << first.error().message;
  EXPECT_EQ(*first, (std::vector<CellRef>{h.first}));

  session.setVariable("outside", h.third);
  const auto second = executor.execute(source);
  ASSERT_TRUE(second) << second.error().message;
  EXPECT_EQ(*second, (std::vector<CellRef>{h.third}));

  const auto rebound =
      h.run("def pure show(): Cell := $outside; "
            "let $outside := %\"first\"; let $saved := show(); "
            "let $outside := %\"second\"; return $saved, show()");
  ASSERT_TRUE(rebound.compiled) << rebound.error;
  ASSERT_TRUE(rebound.executed) << rebound.error;
  EXPECT_GT(rebound.invokeOps, 0u);
  ASSERT_EQ(rebound.values.size(), 2u);
  EXPECT_EQ(h.arena.textOf(rebound.values[0]), "first");
  EXPECT_EQ(h.arena.textOf(rebound.values[1]), "second");
  EXPECT_NE(rebound.values[0], rebound.values[1]);
}

TEST(VQLCaptureFrameTest, FreeReferenceBeforeLocalShadowKeepsCallerValue) {
  Harness h;
  const auto result = h.run(
      "def pure scope(): Stream<Cell> := { let $before := $outside; "
      "let $outside := $replacement; return $before, $outside; }; scope()",
      {{"outside", {h.first}}, {"replacement", {h.second}}});
  ASSERT_TRUE(result.compiled) << result.error;
  ASSERT_TRUE(result.executed) << result.error;
  EXPECT_GT(result.invokeOps, 0u);
  EXPECT_EQ(result.values, (std::vector<CellRef>{h.first, h.second}));
}

TEST(VQLCaptureFrameTest,
     PatternShadowingDoesNotReplacePrematchDefaultCapture) {
  Harness h;
  const auto item     = h.arena.makeCell("item");
  const auto captured = h.arena.makeCell("pattern value");
  const auto outside  = h.arena.makeCell("caller value");
  const DimRef first  = h.coordinator.resolveDimension("d.first");
  zigzag::expectWritten(h.arena.link(item, first, DimVector::POS, captured));

  const auto result =
      h.run("def pure choose($item: Cell { +d.first: $outside: Cell, "
            "+d.second: $later: Cell := $outside }): Cell := $later; "
            "choose($item)",
            {{"item", {item}}, {"outside", {outside}}});
  ASSERT_TRUE(result.compiled) << result.error;
  ASSERT_TRUE(result.executed) << result.error;
  EXPECT_GT(result.invokeOps, 0u);
  EXPECT_EQ(result.values, (std::vector<CellRef>{outside}));

  const auto completeItem = h.arena.makeCell("complete item");
  zigzag::expectWritten(
      h.arena.link(completeItem, first, DimVector::POS, captured));
  zigzag::expectWritten(h.arena.link(completeItem,
                                     h.coordinator.resolveDimension("d.second"),
                                     DimVector::POS, h.third));
  const auto missingDefaultCapture =
      h.compileOnly("def pure choose($item: Cell { +d.first: $outside: Cell, "
                    "+d.second: $later: Cell := $outside }): Cell := $later; "
                    "choose($item)",
                    {{"item", {completeItem}}});
  EXPECT_FALSE(missingDefaultCapture.compiled);
  EXPECT_NE(missingDefaultCapture.error.find("default"), std::string::npos);
}

TEST(VQLCaptureFrameTest, LazyParameterDefaultDoesNotHoistMissingFieldAlias) {
  Harness h;
  constexpr std::string_view definitions =
      "def pure g($node: Cell { +d.field: $value: Cell? := $outside }): "
      "Cell? := $value; "
      "def pure f($value: Cell? := g(##)): Cell? := $value; ";

  const auto skipped =
      h.run(std::string(definitions) + "f($actual)", {{"actual", {h.first}}});
  ASSERT_TRUE(skipped.compiled) << skipped.error;
  ASSERT_TRUE(skipped.executed) << skipped.error;
  EXPECT_EQ(skipped.values, (std::vector<CellRef>{h.first}));

  const auto required = h.compileOnly(std::string(definitions) + "f()");
  EXPECT_FALSE(required.compiled);
  EXPECT_NE(required.error.find("default"), std::string::npos);
}

TEST(VQLCaptureFrameTest, ReturnContractDefaultsCanUseBodyPrefixLetBindings) {
  Harness h;
  const CellRef fallback   = h.arena.makeCell("return contract fallback");
  const DimRef fallbackDim = h.coordinator.resolveDimension("d.fallback");
  zigzag::expectWritten(
      h.arena.link(h.core.home(), fallbackDim, DimVector::POS, fallback));
  const auto result =
      h.run("def pure make(): Cell { +d.missing: Cell? := $fallback } := { "
            "let $fallback := ##/d.fallback; return ##; }; make()");
  ASSERT_TRUE(result.compiled) << result.error;
  ASSERT_TRUE(result.executed) << result.error;
  EXPECT_GT(result.invokeOps, 0u);
  EXPECT_EQ(result.values, (std::vector<CellRef>{h.core.home()}));
}

TEST(VQLCaptureFrameTest, FunctorFreeBindingsStayOnInlineCompilationPath) {
  Harness h;
  const CellRef child    = h.arena.makeCell("child");
  const CellRef parent   = h.arena.makeCell("parent");
  const DimRef childDim  = h.coordinator.resolveDimension("d.child");
  const DimRef parentDim = h.coordinator.resolveDimension("d.parent");
  zigzag::expectWritten(
      h.arena.link(h.core.home(), childDim, DimVector::POS, child));
  zigzag::expectWritten(h.arena.link(child, parentDim, DimVector::POS, parent));

  const auto direct = h.run("def pure apply($node: Cell): Cell := $node/$up; "
                            "let $up := /+d.parent; apply(##/d.child)");
  ASSERT_TRUE(direct.compiled) << direct.error;
  ASSERT_TRUE(direct.executed) << direct.error;
  EXPECT_EQ(direct.invokeOps, 0u);
  EXPECT_EQ(direct.values, (std::vector<CellRef>{parent}));

  const auto indirect =
      h.run("def pure apply($node: Cell): Cell := $node/$up; "
            "def pure wrapper($node: Cell): Cell := apply($node); "
            "let $up := /+d.parent; wrapper(##/d.child)");
  ASSERT_TRUE(indirect.compiled) << indirect.error;
  ASSERT_TRUE(indirect.executed) << indirect.error;
  EXPECT_EQ(indirect.invokeOps, 0u);
  EXPECT_EQ(indirect.values, (std::vector<CellRef>{parent}));

  const auto recursive = h.compileOnly(
      "def pure left($node: Cell): Cell := right($node); "
      "def pure right($node: Cell): Cell := { if $node/+d.next[1] "
      "return left($node/+d.next[1]) else return $node/$up; }; "
      "let $up := /+d.parent; left(##/d.child)");
  EXPECT_FALSE(recursive.compiled);
  EXPECT_NE(recursive.error.find("recursive"), std::string::npos);
}

TEST(VQLCaptureFrameTest,
     PipelinedFreeCaptureStaysInlineAndDefaultCaptureRefuses) {
  Harness h;
  const CellRef outside = h.arena.makeCell("pipelined caller");
  const CellRef stop    = h.vm.mintOpcode(OpcodeKind::Halt);
  h.core.attachPipeline(outside, stop);

  const auto direct =
      h.run("def pure read() := $outside; read()", {{"outside", {outside}}});
  ASSERT_TRUE(direct.compiled) << direct.error;
  ASSERT_TRUE(direct.executed) << direct.error;
  EXPECT_EQ(direct.invokeOps, 0u);
  EXPECT_EQ(direct.values, (std::vector<CellRef>{outside}));

  const CellRef item        = h.arena.makeCell("item");
  const auto defaultCapture = h.compileOnly(
      "def pure choose($item: Cell { +d.absent: Cell? := $outside }): "
      "Cell := $item; choose($item)",
      {{"item", {item}}, {"outside", {outside}}});
  EXPECT_FALSE(defaultCapture.compiled);
  EXPECT_NE(defaultCapture.error.find("pipeline"), std::string::npos);
}

TEST(VQLCaptureFrameTest, HostAndMeshRoutesUseCallerFreeCapturePorts) {
  constexpr std::string_view source =
      "def pure read(): Cell := $outside; read()";
  zigzag::vortex::VortexHost host;
  const CellRef outside = host.arena().makeCell("host caller");
  host.vqlEngine().setVariable("outside", outside);
  const auto hostResult = host.executeVQLChecked(source);
  ASSERT_TRUE(hostResult) << hostResult.error().message;
  EXPECT_EQ(*hostResult, (std::vector<CellRef>{outside}));

  namespace mesh = zigzag::vortex::mesh;
  const auto meshResult =
      mesh::evaluate(mesh::Frontend::Vquery,
                     "def pure read(): Cell := $outside; "
                     "let $outside := %\"meshcaller\"; read()",
                     {.maxCycles      = 10000,
                      .maxCells       = 10000,
                      .maxSolutions   = 16,
                      .maxOutputBytes = 1024 * 1024});
  ASSERT_TRUE(meshResult) << meshResult.error();
  auto &arena = meshResult->context->arena;
  std::optional<CellRef> items;
  std::optional<CellRef> value;
  for (const auto &cell : arena.cells()) {
    if (arena.textOf(cell.birthOp) == "d.mesh.items") items = cell.birthOp;
    if (arena.textOf(cell.birthOp) == "d.mesh.value") value = cell.birthOp;
  }
  ASSERT_TRUE(items);
  ASSERT_TRUE(value);
  const auto record = arena.linked(meshResult->root, *items, DimVector::POS);
  ASSERT_TRUE(record);
  const auto output = arena.linked(*record, *value, DimVector::POS);
  ASSERT_TRUE(output);
  EXPECT_EQ(arena.textOf(*output), "meshcaller");
}

TEST(VQLCaptureFrameTest, RecursiveCapturedGraphSurvivesFreshVMReload) {
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<UserPermascroll>(
      UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  const auto storePath = temporary.path / "captured-program";
  {
    zigzag::ArenaManifold arena;
    zigzag::vortex::VortexCore core(arena);
    MultiStoreCoordinator coordinator(core);
    zigzag::vortex::VortexVM vm(core);
    const CellRef outside = arena.makeCell("outside");
    const CellRef root    = arena.makeCell("root");
    const CellRef leaf    = arena.makeCell("leaf");
    zigzag::expectWritten(arena.link(
        root, coordinator.resolveDimension("d.next"), DimVector::POS, leaf));
    zigzag::expectWritten(arena.link(core.home(),
                                     coordinator.resolveDimension("d.expected"),
                                     DimVector::POS, outside));
    VQLCompiler compiler(coordinator, vm);
    CompilationOptions options;
    options.initialBindings = {{"outside", {outside}}, {"root", {root}}};
    const auto compiled     = compiler.compile(
        "def pure walk($node: Cell): Cell := { if $node/+d.next[1] "
            "return walk($node/+d.next[1]) else return $outside; }; "
            "walk($root)",
        options);
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    ASSERT_TRUE(compiled.entryOpcode);
    Store store(scroll);
    compiler.exportToStore(store);
    store.save(storePath.string());
  }

  Store reopenedStore(scroll);
  ASSERT_NO_THROW(reopenedStore.load(storePath.string()));
  ASSERT_TRUE(reopenedStore.structureHead());
  const auto manifold =
      reopenedStore.rebuildManifold(*reopenedStore.structureHead());
  zigzag::ArenaManifold arena(&manifold, reopenedStore);
  zigzag::vortex::VortexCore core(arena);
  MultiStoreCoordinator coordinator(core);
  zigzag::vortex::VortexVM vm(core);
  vm.setContractCoordinator(coordinator);
  const auto entry =
      arena.linked(core.home(), core.dims().spin, DimVector::POS);
  ASSERT_TRUE(entry);
  const auto cursor    = vm.spawnCursor(*entry, "capture-frame-reload");
  const auto execution = vm.run(cursor, 10000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;

  auto halt = *entry;
  auto next = arena.linked(halt, core.dims().spin, DimVector::POS);
  while (next && vm.getOpcodeKind(*next) != OpcodeKind::Halt) {
    halt = *next;
    next = arena.linked(halt, core.dims().spin, DimVector::POS);
  }
  ASSERT_TRUE(next);
  ASSERT_EQ(vm.getOpcodeKind(*next), OpcodeKind::Halt);
  const auto resultPorts = core.inputsOf(*next);
  ASSERT_EQ(resultPorts.size(), 1u);
  const auto values = vm.resultCells(resultPorts.front());
  ASSERT_EQ(values.size(), 1u);
  const auto expectedDim = core.findDimension("d.expected");
  ASSERT_TRUE(expectedDim);
  const auto expectedOutside =
      arena.linked(core.home(), *expectedDim, DimVector::POS);
  ASSERT_TRUE(expectedOutside);
  EXPECT_EQ(values.front(), *expectedOutside);
  EXPECT_EQ(arena.textOf(values.front()), "outside");
  EXPECT_FALSE(vm.hasActiveFrames());
}

} // namespace
} // namespace xanadu::vql

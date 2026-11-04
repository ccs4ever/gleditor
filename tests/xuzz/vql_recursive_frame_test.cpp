#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/mesh_evaluator.hpp"
#include "common/xanadu/vortex/vortex_host.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;

struct TempDir {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_recursive_frames_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDir() { std::filesystem::create_directories(path); }
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};
  DimRef next{coordinator.resolveDimension("d.next")};
  CellRef first{core.home()};
  CellRef second{arena.makeCell("second")};
  CellRef third{arena.makeCell("third")};

  Harness() {
    zigzag::expectWritten(arena.link(first, next, DimVector::POS, second));
    zigzag::expectWritten(arena.link(second, next, DimVector::POS, third));
  }
};

TEST(VQLRecursiveFrameTest, SelfAndMutualRecursionTraverseFiniteGraph) {
  Harness h;
  QueryExecutor executor(h.coordinator, h.vm);
  const auto self =
      executor.execute("def pure last($node: Cell): Cell := { if $node/+d.next "
                       "return last($node/+d.next[1]) else return $node; }; "
                       "last(##/d.next[1])");
  ASSERT_TRUE(self) << self.error().message;
  ASSERT_EQ(self->size(), 1u);
  EXPECT_EQ(self->front(), h.third);

  const auto mutual =
      executor.execute("def pure even($node: Cell): Cell := { if $node/+d.next "
                       "return odd($node/+d.next[1]) else return $node; }; "
                       "def pure odd($node: Cell): Cell := { if $node/+d.next "
                       "return even($node/+d.next[1]) else return $node; }; "
                       "even(##/d.next[1])");
  ASSERT_TRUE(mutual) << mutual.error().message;
  ASSERT_EQ(mutual->size(), 1u);
  EXPECT_EQ(mutual->front(), h.third);
}

TEST(VQLRecursiveFrameTest, ClosedCallsPreserveIdentityAndCallerBindings) {
  Harness h;
  const auto external = h.arena.makeCell("external");
  ContractBindings bindings{{"external", {external}}, {"argument", {h.second}}};
  QueryExecutor executor(h.coordinator, h.vm, {}, std::move(bindings));
  const auto direct = executor.execute(
      "def pure id($value: Cell): Cell := $value; id($external)");
  ASSERT_TRUE(direct) << direct.error().message;
  EXPECT_EQ(*direct, (std::vector<CellRef>{external}));

  const auto result = executor.execute(
      "def pure id($value: Cell): Cell := $value; "
      "def pure wrap(): Cell := { return id($argument); }; wrap()");
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{h.second}));

  const auto repeated = executor.execute(
      "def pure id($value: Cell): Cell := $value; id($argument)");
  ASSERT_TRUE(repeated) << repeated.error().message;
  EXPECT_EQ(*repeated, (std::vector<CellRef>{h.second}));
}

TEST(VQLRecursiveFrameTest, StreamArgumentsAndEmptyDefaultPresenceAreDistinct) {
  Harness h;
  const auto fallback = h.arena.makeCell("fallback");
  zigzag::expectWritten(
      h.arena.link(h.core.home(), h.coordinator.resolveDimension("d.fallback"),
                   DimVector::POS, fallback));
  QueryExecutor executor(h.coordinator, h.vm);
  const auto omitted = executor.execute(
      "def pure choose($value: Cell := ##/d.fallback): Cell := $value; "
      "choose()");
  ASSERT_TRUE(omitted) << omitted.error().message;
  EXPECT_EQ(*omitted, (std::vector<CellRef>{fallback}));

  QueryExecutor emptyExecutor(h.coordinator, h.vm, {}, {{"missing", {}}});
  const auto explicitEmpty = emptyExecutor.execute(
      "def pure choose($value: Cell := ##/d.fallback): Cell := $value; "
      "choose($missing)");
  ASSERT_TRUE(explicitEmpty) << explicitEmpty.error().message;
  EXPECT_TRUE(explicitEmpty->empty());

  const auto stream = executor.execute(
      "def pure pass($values: Stream<Cell>): Stream<Cell> := $values; "
      "pass(##/d.next)");
  ASSERT_TRUE(stream) << stream.error().message;
  EXPECT_EQ(*stream, (std::vector<CellRef>{h.second, h.third}));
}

TEST(VQLRecursiveFrameTest, RecursiveCallSitesAreFiniteAndDepthIsBudgeted) {
  Harness h;
  VQLCompiler compiler(h.coordinator, h.vm);
  const auto compiled = compiler.compile(
      "def pure loop($node: Cell): Cell := loop($node); loop(##)");
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  std::size_t invocations = 0;
  for (const auto opcode : compiled.generatedOpcodes) {
    invocations += h.vm.getOpcodeKind(opcode) == OpcodeKind::InvokeFrame;
  }
  EXPECT_LE(invocations, 3u);

  h.vm.setFrameLimits({.maxDepth = 6});
  QueryExecutor executor(h.coordinator, h.vm, {.maxCycles = 10'000});
  const auto result = executor.execute(
      "def pure loop($node: Cell): Cell := loop($node); loop(##)");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().category, QueryExecutionError::Category::Budget);
  EXPECT_NE(result.error().message.find("frame depth"), std::string::npos);
}

TEST(VQLRecursiveFrameTest, HostRouteExecutesNestedFrameCallsWithoutContext) {
  zigzag::vortex::VortexHost host;
  const auto result = host.executeVQLChecked(
      "def pure id($value: Int): Int := $value; "
      "def pure wrapper(): Int := { return id(7); }; wrapper()");
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ(host.core().arena().asInt64(result->front()),
            std::optional<std::int64_t>{7});
}

TEST(VQLRecursiveFrameTest, ExportedFramesRunInFreshReloadedVM) {
  TempDir temporary;
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Harness h;
    VQLCompiler compiler(h.coordinator, h.vm);
    const auto compiled = compiler.compile(
        "def pure last($node: Cell): Cell := { if $node/+d.next[1] "
        "return last($node/+d.next[1]) else return $node; }; last(##)");
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    xanadu::Store store(scroll);
    compiler.exportToStore(store);
    store.save((temporary.path / "program").string());
  }

  xanadu::Store store(scroll);
  store.load((temporary.path / "program").string());
  ASSERT_TRUE(store.structureHead());
  const auto manifold = store.rebuildManifold(*store.structureHead());
  zigzag::ArenaManifold arena(&manifold, store);
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  const auto entry =
      arena.linked(core.home(), core.dims().spin, DimVector::POS);
  ASSERT_TRUE(entry);
  const auto cursor    = vm.spawnCursor(*entry, "reloaded-vql-frame");
  const auto execution = vm.run(cursor, 1000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  auto halt = *entry;
  auto next = arena.linked(halt, core.dims().spin, DimVector::POS);
  while (next && vm.getOpcodeKind(*next) != OpcodeKind::Halt) {
    halt = *next;
    next = arena.linked(halt, core.dims().spin, DimVector::POS);
  }
  ASSERT_TRUE(next);
  halt = *next;
  ASSERT_EQ(vm.getOpcodeKind(halt), OpcodeKind::Halt);
  const auto resultPorts = core.inputsOf(halt);
  ASSERT_EQ(resultPorts.size(), 1u);
  const auto results = vm.resultCells(resultPorts.front());
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(arena.textOf(results.front()), "third");
  EXPECT_FALSE(vm.hasActiveFrames());
}

TEST(VQLRecursiveFrameTest, MeshQueryRouteExecutesClosedFrames) {
  namespace mesh    = zigzag::vortex::mesh;
  const auto result = mesh::evaluate(
      mesh::Frontend::Vquery, "def pure id($value: Int): Int := $value; id(7)",
      {.maxCycles      = 10000,
       .maxCells       = 10000,
       .maxSolutions   = 16,
       .maxOutputBytes = 1024 * 1024});
  ASSERT_TRUE(result) << result.error();
  auto &arena = result->context->arena;
  std::optional<zigzag::CellRef> items;
  std::optional<zigzag::CellRef> value;
  for (const auto &cell : arena.cells()) {
    if (arena.textOf(cell.birthOp) == "d.mesh.items") items = cell.birthOp;
    if (arena.textOf(cell.birthOp) == "d.mesh.value") value = cell.birthOp;
  }
  ASSERT_TRUE(items);
  ASSERT_TRUE(value);
  const auto record = arena.linked(result->root, *items, DimVector::POS);
  ASSERT_TRUE(record);
  const auto output = arena.linked(*record, *value, DimVector::POS);
  ASSERT_TRUE(output);
  EXPECT_EQ(arena.asInt64(*output), std::optional<std::int64_t>{7});
}

} // namespace
} // namespace xanadu::vql

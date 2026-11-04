#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/mesh_evaluator.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_host.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;
using zigzag::vortex::VortexCore;
using zigzag::vortex::VortexVM;

struct TempDir {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_context_frames_" +
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
  VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  VortexVM vm{core};
  DimRef items{coordinator.resolveDimension("d.items")};
  DimRef next{coordinator.resolveDimension("d.next")};
  DimRef fallback{coordinator.resolveDimension("d.fallback")};
  CellRef first{arena.makeCell("first")};
  CellRef second{arena.makeCell("second")};
  CellRef third{arena.makeCell("third")};

  Harness() {
    zigzag::expectWritten(
        arena.link(core.home(), items, DimVector::POS, first));
    zigzag::expectWritten(arena.link(first, items, DimVector::POS, second));
    zigzag::expectWritten(arena.link(first, next, DimVector::POS, second));
    zigzag::expectWritten(arena.link(second, next, DimVector::POS, third));
    zigzag::expectWritten(
        arena.link(core.home(), fallback, DimVector::POS, third));
  }

  [[nodiscard]] std::expected<std::vector<CellRef>, QueryExecutionError>
  run(std::string_view source, std::vector<CellRef> context = {}) {
    QueryExecutor executor(coordinator, vm);
    return executor.execute(source, context);
  }
};

TEST(VQLContextFrameTest, ScalarAndStreamDotFormalsPreserveIdentityAndOrder) {
  Harness h;
  const auto scalar =
      h.run("def pure identity(. : Cell): Cell := .; identity(##/d.items[1])");
  ASSERT_TRUE(scalar) << scalar.error().message;
  EXPECT_EQ(*scalar, (std::vector<CellRef>{h.first}));

  const auto stream =
      h.run("def pure pass(. : Stream<Cell>): Stream<Cell> := .; "
            "pass(##/d.items)");
  ASSERT_TRUE(stream) << stream.error().message;
  EXPECT_EQ(*stream, (std::vector<CellRef>{h.first, h.second}));

  const auto empty =
      h.run("def pure pass(. : Stream<Cell>): Stream<Cell> := .; pass()");
  ASSERT_TRUE(empty) << empty.error().message;
  EXPECT_TRUE(empty->empty());

  const auto explicitEmpty =
      h.run("def pure pass(. : Stream<Cell>): Stream<Cell> := .; "
            "pass(##/d.absent)");
  ASSERT_TRUE(explicitEmpty) << explicitEmpty.error().message;
  EXPECT_TRUE(explicitEmpty->empty());
}

TEST(VQLContextFrameTest, DotTypeMismatchFiltersTheCallResult) {
  Harness h;
  const auto valid = h.run("def pure typed(. : Int): Int := .; typed(7)");
  ASSERT_TRUE(valid) << valid.error().message;
  ASSERT_EQ(valid->size(), 1u);
  EXPECT_EQ(h.arena.asInt64(valid->front()), std::optional<std::int64_t>{7});

  const auto invalid =
      h.run("def pure typed(. : Int): Int := .; typed(\"wrong\")");
  ASSERT_TRUE(invalid) << invalid.error().message;
  EXPECT_TRUE(invalid->empty());
}

TEST(VQLContextFrameTest,
     MissingDotActualIsEmptyAndDoesNotBorrowAmbientContext) {
  Harness h;
  const auto ambient = h.arena.makeCell("ambient");
  const auto result =
      h.run("def pure select(. : Stream<Cell>): Stream<Cell> := .; "
            "def pure wrapper(): Stream<Cell> := select(); wrapper()",
            {ambient});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_TRUE(result->empty());
}

TEST(VQLContextFrameTest, UfcsContextSuppliesDotFormalArgumentZero) {
  Harness h;
  const auto result =
      h.run("def pure identity(. : Stream<Cell>): Stream<Cell> := .; "
            "##/d.items/identity()");
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{h.first, h.second}));
}

TEST(VQLContextFrameTest, DefaultsSeePriorParametersAndHiddenContext) {
  Harness h;
  const auto priorParameter =
      h.run("def pure choose($value: Cell, . : Cell := $value): Cell := .; "
            "choose(##/d.items[1])");
  ASSERT_TRUE(priorParameter) << priorParameter.error().message;
  EXPECT_EQ(*priorParameter, (std::vector<CellRef>{h.first}));

  const auto hiddenContext = h.run("def pure choose(. : Cell := .): Cell := .; "
                                   "def pure wrapper(): Cell := choose(); "
                                   "##/d.items[1]/wrapper()");
  ASSERT_TRUE(hiddenContext) << hiddenContext.error().message;
  EXPECT_EQ(*hiddenContext, (std::vector<CellRef>{h.first}));

  const auto explicitEmpty =
      h.run("def pure choose(. : Stream<Cell> := ##/d.fallback): "
            "Stream<Cell> := .; choose(##/d.absent)");
  ASSERT_TRUE(explicitEmpty) << explicitEmpty.error().message;
  EXPECT_TRUE(explicitEmpty->empty());
}

TEST(VQLContextFrameTest,
     RecursiveDotCallsForwardValuesAndRestoreCallerContext) {
  Harness h;
  const auto result =
      h.run("def pure last(. : Cell): Cell := { if ./d.next "
            "return last(./d.next[1]) else return .; }; last(##/d.items[1])");
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{h.third}));

  const auto nested =
      h.run("def pure child(. : Cell): Cell := .; "
            "def pure outer(. : Cell): Stream<Cell> := { "
            "return child(##/d.fallback), .; }; outer(##/d.items[1])");
  ASSERT_TRUE(nested) << nested.error().message;
  EXPECT_EQ(*nested, (std::vector<CellRef>{h.third, h.first}));
}

TEST(VQLContextFrameTest, HostAndMeshVqlRoutesRunDotFrames) {
  zigzag::vortex::VortexHost host;
  const auto hostResult = host.executeVQLChecked(
      "def pure identity(. : Int): Int := .; identity(7)");
  ASSERT_TRUE(hostResult) << hostResult.error().message;
  ASSERT_EQ(hostResult->size(), 1u);
  EXPECT_EQ(host.core().arena().asInt64(hostResult->front()),
            std::optional<std::int64_t>{7});

  namespace mesh = zigzag::vortex::mesh;
  const auto meshResult =
      mesh::evaluate(mesh::Frontend::Vquery,
                     "def pure identity(. : Int): Int := .; identity(7)",
                     {.maxCycles      = 10000,
                      .maxCells       = 10000,
                      .maxSolutions   = 16,
                      .maxOutputBytes = 1024 * 1024});
  ASSERT_TRUE(meshResult) << meshResult.error();
  const auto &arena = meshResult->context->arena;
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
  EXPECT_EQ(arena.asInt64(*output), std::optional<std::int64_t>{7});
}

TEST(VQLContextFrameTest, ExportedDotFrameRunsAfterReloadInFreshVM) {
  TempDir temporary;
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Harness h;
    VQLCompiler compiler(h.coordinator, h.vm);
    const auto compiled =
        compiler.compile("def pure identity(. : Int): Int := .; identity(7)");
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
  VortexCore core(arena);
  VortexVM vm(core);
  const auto entry =
      arena.linked(core.home(), core.dims().spin, DimVector::POS);
  ASSERT_TRUE(entry);
  const auto cursor    = vm.spawnCursor(*entry, "reloaded-dot-frame");
  const auto execution = vm.run(cursor, 1000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;

  auto halt = *entry;
  auto next = arena.linked(halt, core.dims().spin, DimVector::POS);
  while (next && vm.getOpcodeKind(*next) != OpcodeKind::Halt) {
    halt = *next;
    next = arena.linked(halt, core.dims().spin, DimVector::POS);
  }
  ASSERT_TRUE(next);
  halt               = *next;
  const auto outputs = core.inputsOf(halt);
  ASSERT_EQ(outputs.size(), 1u);
  const auto values = vm.resultCells(outputs.front());
  ASSERT_EQ(values.size(), 1u);
  EXPECT_EQ(arena.asInt64(values.front()), std::optional<std::int64_t>{7});
  EXPECT_FALSE(vm.hasActiveFrames());
}

} // namespace
} // namespace xanadu::vql

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
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
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_path_morphism_" +
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

  [[nodiscard]] DimRef dimension(const std::string_view name) {
    return coordinator.resolveDimension(name);
  }

  [[nodiscard]] CellRef cell(const std::string_view text) {
    return arena.makeCell(text);
  }

  void link(const CellRef from, const DimRef dim, const DimVector direction,
            const CellRef to) {
    zigzag::expectWritten(arena.link(from, dim, direction, to));
  }

  [[nodiscard]] CompilationResult compile(const std::string_view source,
                                          ContractBindings bindings) {
    CompilationOptions options;
    options.initialBindings = std::move(bindings);
    VQLCompiler compiler(coordinator, vm);
    return compiler.compile(source, options);
  }

  [[nodiscard]] std::vector<CellRef> run(const CompilationResult &compiled) {
    EXPECT_TRUE(compiled.success) << compiled.errorMessage;
    EXPECT_TRUE(compiled.cursor);
    if (!compiled.success || !compiled.cursor) return {};

    const auto execution = vm.run(*compiled.cursor, 100'000);
    vm.setCursorOpcode(*compiled.cursor, std::nullopt);
    EXPECT_TRUE(execution.success) << execution.errorMessage;
    if (!execution.success) return {};

    std::vector<CellRef> result;
    for (const CellRef slot : compiled.resultSlots) {
      const auto cells = vm.resultCells(slot);
      result.insert(result.end(), cells.begin(), cells.end());
    }
    return result;
  }
};

TEST(VQLPathMorphismVMRegressionTest,
     CompilationDoesNotMutateTopologyAndRuntimeAppendsFreshTargetChains) {
  Harness h;
  const DimRef tag             = h.dimension("d.tag");
  const CellRef owner          = h.cell("owner");
  const CellRef existing       = h.cell("existing");
  const CellRef secondOwner    = h.cell("second owner");
  const CellRef secondExisting = h.cell("second existing");
  h.link(owner, tag, DimVector::POS, existing);
  h.link(secondOwner, tag, DimVector::POS, secondExisting);

  const auto compiled = h.compile(R"($owner/+d.tag => [ "first", "second" ])",
                                  {{"owner", {owner, secondOwner}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_EQ(h.arena.linked(owner, tag, DimVector::POS), existing);
  EXPECT_FALSE(h.arena.linked(existing, tag, DimVector::POS));
  EXPECT_EQ(h.arena.linked(secondOwner, tag, DimVector::POS), secondExisting);
  EXPECT_FALSE(h.arena.linked(secondExisting, tag, DimVector::POS));

  const auto firstRun = h.run(compiled);
  ASSERT_EQ(firstRun.size(), 4U);
  EXPECT_EQ(h.arena.textOf(firstRun[0]), "first");
  EXPECT_EQ(h.arena.textOf(firstRun[1]), "second");
  EXPECT_EQ(h.arena.textOf(firstRun[2]), "first");
  EXPECT_EQ(h.arena.textOf(firstRun[3]), "second");
  EXPECT_EQ(h.arena.linked(existing, tag, DimVector::POS), firstRun[0]);
  EXPECT_EQ(h.arena.linked(firstRun[0], tag, DimVector::POS), firstRun[1]);
  EXPECT_EQ(h.arena.linked(secondExisting, tag, DimVector::POS), firstRun[2]);
  EXPECT_EQ(h.arena.linked(firstRun[2], tag, DimVector::POS), firstRun[3]);

  ASSERT_TRUE(compiled.entryOpcode);
  const CellRef rerunCursor =
      h.vm.spawnCursor(*compiled.entryOpcode, "morphism-second-run");
  const auto secondExecution = h.vm.run(rerunCursor, 100'000);
  h.vm.setCursorOpcode(rerunCursor, std::nullopt);
  ASSERT_TRUE(secondExecution.success) << secondExecution.errorMessage;

  std::vector<CellRef> secondRun;
  for (const CellRef slot : compiled.resultSlots) {
    const auto cells = h.vm.resultCells(slot);
    secondRun.insert(secondRun.end(), cells.begin(), cells.end());
  }
  ASSERT_EQ(secondRun.size(), 4U);
  for (std::size_t i = 0; i < secondRun.size(); ++i) {
    EXPECT_NE(secondRun[i], firstRun[i]);
    EXPECT_EQ(h.arena.textOf(secondRun[i]), i % 2 == 0 ? "first" : "second");
  }
  EXPECT_EQ(h.arena.linked(firstRun[1], tag, DimVector::POS), secondRun[0]);
  EXPECT_EQ(h.arena.linked(secondRun[0], tag, DimVector::POS), secondRun[1]);
  EXPECT_EQ(h.arena.linked(firstRun[3], tag, DimVector::POS), secondRun[2]);
  EXPECT_EQ(h.arena.linked(secondRun[2], tag, DimVector::POS), secondRun[3]);
}

TEST(VQLPathMorphismVMRegressionTest,
     EmptyRuntimeTargetSkipsOnlyThatTargetAndClearsOnRerun) {
  Harness h;
  const DimRef tag    = h.dimension("d.tag");
  const CellRef owner = h.cell("owner");

  const auto compiled = h.compile(
      R"($owner/+d.tag => [ ./+d.missing, "later" ])", {{"owner", {owner}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_FALSE(h.arena.linked(owner, tag, DimVector::POS));

  const auto firstRun = h.run(compiled);
  ASSERT_EQ(firstRun.size(), 1U);
  EXPECT_EQ(h.arena.textOf(firstRun.front()), "later");
  EXPECT_EQ(h.arena.linked(owner, tag, DimVector::POS), firstRun.front());

  ASSERT_TRUE(compiled.entryOpcode);
  const CellRef rerunCursor =
      h.vm.spawnCursor(*compiled.entryOpcode, "morphism-empty-target-rerun");
  const auto rerun = h.vm.run(rerunCursor, 100'000);
  h.vm.setCursorOpcode(rerunCursor, std::nullopt);
  ASSERT_TRUE(rerun.success) << rerun.errorMessage;

  std::vector<CellRef> secondRun;
  for (const CellRef slot : compiled.resultSlots) {
    const auto cells = h.vm.resultCells(slot);
    secondRun.insert(secondRun.end(), cells.begin(), cells.end());
  }
  ASSERT_EQ(secondRun.size(), 1U);
  EXPECT_NE(secondRun.front(), firstRun.front());
  EXPECT_EQ(h.arena.textOf(secondRun.front()), "later");
  EXPECT_EQ(h.arena.linked(firstRun.front(), tag, DimVector::POS),
            secondRun.front());
}

TEST(VQLPathMorphismVMRegressionTest,
     NegativeDimensionMorphismPreservesDirectionAndTargetOrder) {
  Harness h;
  const DimRef tag       = h.dimension("d.tag");
  const CellRef owner    = h.cell("owner");
  const CellRef existing = h.cell("existing");
  h.link(owner, tag, DimVector::NEG, existing);

  const auto compiled = h.compile(R"($owner/-d.tag => [ "first", "second" ])",
                                  {{"owner", {owner}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_FALSE(h.arena.linked(existing, tag, DimVector::NEG));

  const auto result = h.run(compiled);
  ASSERT_EQ(result.size(), 2U);
  EXPECT_EQ(h.arena.textOf(result[0]), "first");
  EXPECT_EQ(h.arena.textOf(result[1]), "second");
  EXPECT_EQ(h.arena.linked(existing, tag, DimVector::NEG), result[0]);
  EXPECT_EQ(h.arena.linked(result[0], tag, DimVector::NEG), result[1]);
}

TEST(VQLPathMorphismVMRegressionTest,
     GeneratedGraphSurvivesAstAndSourceVMDestructionAndReload) {
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<UserPermascroll>(
      UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    zigzag::ArenaManifold sourceArena;
    zigzag::vortex::VortexCore sourceCore(sourceArena);
    MultiStoreCoordinator sourceCoordinator(sourceCore);
    zigzag::vortex::VortexVM sourceVm(sourceCore);
    VQLCompiler compiler(sourceCoordinator, sourceVm);
    CompilationResult compiled;
    {
      Parser parser(
          R"(##/+d.persisted => [ "persisted first", "persisted second" ])");
      const auto ast = parser.parseQuery();
      compiled       = compiler.compile(ast);
    }
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    EXPECT_FALSE(sourceArena.linked(
        sourceCore.home(), sourceCore.findOrMintDimension("d.persisted"),
        DimVector::POS));

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
      vm.run(vm.spawnCursor(*entry, "morphism-export-reload"), 100'000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;

  const DimRef dimension = core.findOrMintDimension("d.persisted");
  const auto first       = arena.linked(core.home(), dimension, DimVector::POS);
  ASSERT_TRUE(first);
  const auto second = arena.linked(*first, dimension, DimVector::POS);
  ASSERT_TRUE(second);
  EXPECT_EQ(arena.textOf(*first), "persisted first");
  EXPECT_EQ(arena.textOf(*second), "persisted second");
  EXPECT_FALSE(arena.linked(*second, dimension, DimVector::POS));
}

TEST(VQLPathMorphismVMRegressionTest,
     HostAndMeshExecuteDimensionMorphismSource) {
  constexpr std::string_view hostSource =
      R"(##/+d.host-morphism => [ "host first", "host second" ])";
  zigzag::vortex::VortexHost host;
  const auto hosted = host.executeVQLChecked(hostSource);
  ASSERT_TRUE(hosted) << hosted.error().message;
  ASSERT_EQ(hosted->size(), 2U);
  const DimRef hostDimension =
      host.core().findOrMintDimension("d.host-morphism");
  const auto hostFirst = host.core().arena().linked(
      host.core().home(), hostDimension, DimVector::POS);
  ASSERT_TRUE(hostFirst);
  EXPECT_EQ(*hostFirst, hosted->front());
  EXPECT_EQ(host.core().arena().textOf(*hostFirst), "host first");
  const auto hostSecond =
      host.core().arena().linked(*hostFirst, hostDimension, DimVector::POS);
  ASSERT_TRUE(hostSecond);
  EXPECT_EQ(*hostSecond, hosted->back());
  EXPECT_EQ(host.core().arena().textOf(*hostSecond), "host second");

  namespace mesh = zigzag::vortex::mesh;
  constexpr std::string_view meshSource =
      R"(##/+d.mesh-morphism => [ "mesh first", "mesh second" ])";
  const auto meshed = mesh::evaluate(mesh::Frontend::Vquery, meshSource,
                                     {.maxCycles      = 100'000,
                                      .maxCells       = 100'000,
                                      .maxSolutions   = 16,
                                      .maxOutputBytes = 1024 * 1024});
  ASSERT_TRUE(meshed) << meshed.error();
  auto &meshArena            = meshed->context->arena;
  auto &meshCore             = meshed->context->core;
  const DimRef meshDimension = meshCore.findOrMintDimension("d.mesh-morphism");
  const auto meshFirst =
      meshArena.linked(meshCore.home(), meshDimension, DimVector::POS);
  ASSERT_TRUE(meshFirst);
  EXPECT_EQ(meshArena.textOf(*meshFirst), "mesh first");
  const auto meshSecond =
      meshArena.linked(*meshFirst, meshDimension, DimVector::POS);
  ASSERT_TRUE(meshSecond);
  EXPECT_EQ(meshArena.textOf(*meshSecond), "mesh second");
}

TEST(VQLPathMorphismVMRegressionTest,
     PerspectiveMorphismUsesPrivateDimensionAndExistingTargetIdentity) {
  Harness h;
  const CellRef owner    = h.cell("owner");
  const CellRef target   = h.cell("existing target");
  const DimRef canonical = h.dimension("d.note");
  const DimRef alice     = h.dimension("d.note@alice");
  const DimRef bob       = h.dimension("d.note@bob");

  const auto perspective =
      h.compile(R"(weave { @alice: $owner/+d.note => [ "Alice note" ] })",
                {{"owner", {owner}}});
  ASSERT_TRUE(perspective.success) << perspective.errorMessage;
  EXPECT_FALSE(h.arena.linked(owner, alice, DimVector::POS));
  EXPECT_FALSE(h.arena.linked(owner, canonical, DimVector::POS));

  const auto aliceResult = h.run(perspective);
  ASSERT_EQ(aliceResult.size(), 1U);
  EXPECT_EQ(h.arena.textOf(aliceResult.front()), "Alice note");
  EXPECT_EQ(h.arena.linked(owner, alice, DimVector::POS), aliceResult.front());
  EXPECT_FALSE(h.arena.linked(owner, canonical, DimVector::POS));
  EXPECT_FALSE(h.arena.linked(owner, bob, DimVector::POS));

  const auto existingTarget =
      h.compile(R"($owner/+d.parent => $target)",
                {{"owner", {owner}}, {"target", {target}}});
  ASSERT_TRUE(existingTarget.success) << existingTarget.errorMessage;
  EXPECT_FALSE(h.arena.linked(owner, h.dimension("d.parent"), DimVector::POS));
  const auto targetResult = h.run(existingTarget);
  ASSERT_EQ(targetResult.size(), 1U);
  EXPECT_EQ(targetResult.front(), target);
  EXPECT_EQ(h.arena.linked(owner, h.dimension("d.parent"), DimVector::POS),
            target);
}

} // namespace
} // namespace xanadu::vql

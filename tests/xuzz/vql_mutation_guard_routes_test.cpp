/**
 * @file vql_mutation_guard_routes_test.cpp
 * @brief Mutation groups across exported and source-driven VQL routes.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/mesh_evaluator.hpp"
#include "common/xanadu/vortex/mesh_result.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_host.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_mutation_guard_routes_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

constexpr std::string_view kUpdateSource =
    "def pure update($n: Int): Cell := $n/value(7); update(12)";
constexpr std::string_view kInvalidUpdateSource =
    "def pure update($n: Int): Cell := $n/value(\"bad\"); update(12)";

TEST(VQLMutationGuardRoutesTest,
     ExportedMutationGroupRunsInFreshVMWithRelocatedResultSlot) {
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<UserPermascroll>(
      UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    zigzag::ArenaManifold sourceArena;
    zigzag::vortex::VortexCore sourceCore(sourceArena);
    MultiStoreCoordinator sourceCoordinator(sourceCore);
    zigzag::vortex::VortexVM sourceVm(sourceCore);
    {
      VQLCompiler compiler(sourceCoordinator, sourceVm);
      CompilationResult compiled;
      {
        Parser parser(kUpdateSource);
        const auto ast = parser.parseQuery();
        compiled       = compiler.compile(ast);
      }
      ASSERT_TRUE(compiled.success) << compiled.errorMessage;
      ASSERT_TRUE(compiled.terminalOpcode);
      std::size_t mutationGroups = 0;
      for (const CellRef opcode : compiled.generatedOpcodes) {
        const auto kind = sourceVm.getOpcodeKind(opcode);
        mutationGroups += kind == OpcodeKind::BeginMutationGroup ||
                          kind == OpcodeKind::EndMutationGroup;
      }
      EXPECT_GE(mutationGroups, 2U);

      Store store(scroll);
      compiler.exportToStore(store);
      store.save((temporary.path / "program").string());
    }
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
  const CellRef cursor = vm.spawnCursor(*entry, "mutation-group-reload");
  const auto execution = vm.run(cursor, 100'000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;

  auto terminal = entry;
  while (terminal && vm.getOpcodeKind(*terminal) != OpcodeKind::Halt)
    terminal = arena.linked(*terminal, core.dims().spin, DimVector::POS);
  ASSERT_TRUE(terminal);
  const auto resultPorts = core.inputsOf(*terminal);
  ASSERT_EQ(resultPorts.size(), 1U);
  const auto results = vm.resultCells(resultPorts.front());
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(arena.asInt64(results.front()), 7);

  vm.setCursorOpcode(cursor, std::nullopt);
  EXPECT_FALSE(vm.hasActiveGuards());
  EXPECT_FALSE(vm.hasActiveFrames());
  EXPECT_EQ(vm.choiceDepth(), 0U);
  EXPECT_EQ(arena.outstandingMarks(), 0U);
}

TEST(VQLMutationGuardRoutesTest,
     HostAndMeshVqueryRoutesRejectNamedMutationAndRecover) {
  zigzag::vortex::VortexHost host;
  const auto hostResult = host.executeVQLChecked(kUpdateSource);
  ASSERT_TRUE(hostResult) << hostResult.error().message;
  ASSERT_EQ(hostResult->size(), 1U);
  EXPECT_EQ(host.arena().asInt64(hostResult->front()), 7);

  const auto hostRejected = host.executeVQLChecked(kInvalidUpdateSource);
  ASSERT_FALSE(hostRejected);
  EXPECT_EQ(hostRejected.error().category,
            QueryExecutionError::Category::TypeContract);
  EXPECT_NE(hostRejected.error().message.find("update"), std::string::npos);
  EXPECT_EQ(host.vm().choiceDepth(), 0U);
  EXPECT_FALSE(host.vm().hasActiveGuards());
  EXPECT_FALSE(host.vm().hasActiveFrames());
  EXPECT_EQ(host.arena().outstandingMarks(), 0U);

  const auto hostRecovered = host.executeVQLChecked(kUpdateSource);
  ASSERT_TRUE(hostRecovered) << hostRecovered.error().message;
  ASSERT_EQ(hostRecovered->size(), 1U);
  EXPECT_EQ(host.arena().asInt64(hostRecovered->front()), 7);

  namespace mesh = zigzag::vortex::mesh;
  constexpr mesh::EvaluationBudget budget{
      .maxCycles      = 100'000,
      .maxCells       = 100'000,
      .maxSolutions   = 16,
      .maxOutputBytes = 1024 * 1024,
  };
  const auto meshResult =
      mesh::evaluate(mesh::Frontend::Vquery, kUpdateSource, budget);
  ASSERT_TRUE(meshResult) << meshResult.error();
  auto &meshArena    = meshResult->context->arena;
  const auto decoded = zigzag::vortex::mesh::readResultCells(*meshResult);

  ASSERT_TRUE(decoded) << decoded.error();

  ASSERT_EQ(decoded->size(), 1U);

  const auto output = std::optional<zigzag::CellRef>{decoded->front()};
  EXPECT_EQ(meshArena.asInt64(*output), 7);

  const auto meshRejected =
      mesh::evaluate(mesh::Frontend::Vquery, kInvalidUpdateSource, budget);
  ASSERT_FALSE(meshRejected);
  EXPECT_NE(meshRejected.error().find("TypeContractError"), std::string::npos);
  EXPECT_NE(meshRejected.error().find("update"), std::string::npos);

  const auto meshRecovered =
      mesh::evaluate(mesh::Frontend::Vquery, kUpdateSource, budget);
  ASSERT_TRUE(meshRecovered) << meshRecovered.error();
  auto &recoveredArena        = meshRecovered->context->arena;
  const auto recoveredOutputs = mesh::readResultCells(*meshRecovered);
  ASSERT_TRUE(recoveredOutputs) << recoveredOutputs.error();
  ASSERT_EQ(recoveredOutputs->size(), 1U);
  EXPECT_EQ(recoveredArena.asInt64(recoveredOutputs->front()), 7);
}

} // namespace
} // namespace xanadu::vql

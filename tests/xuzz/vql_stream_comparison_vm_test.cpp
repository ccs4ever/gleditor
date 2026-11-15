#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/mesh_evaluator.hpp"
#include "common/xanadu/vortex/mesh_result.hpp"
#include "common/xanadu/vortex/vortex_host.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using Bindings = ContractBindings;
using Result   = std::expected<std::vector<CellRef>, QueryExecutionError>;

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};

  CellRef integer(const std::int64_t value) {
    return arena.makeScalarCell(value);
  }

  Result execute(const std::string_view source, Bindings bindings = {},
                 const std::size_t maxCycles = 100'000) {
    QueryExecutor executor(coordinator, vm, {.maxCycles = maxCycles},
                           std::move(bindings));
    return executor.execute(source);
  }

  CompilationResult compile(const std::string_view source,
                            Bindings bindings = {}) {
    VQLCompiler compiler(coordinator, vm);
    return compiler.compile(source, {.initialBindings = std::move(bindings)});
  }

  Result predicate(const std::string_view expression, Bindings bindings = {},
                   const std::size_t maxCycles = 100'000) {
    return execute("##[" + std::string(expression) + "]", std::move(bindings),
                   maxCycles);
  }

  bool matches(Result result) {
    EXPECT_TRUE(result) << (result ? "" : result.error().message);
    return result && !result->empty();
  }

  std::size_t count(const CompilationResult &program,
                    const zigzag::vortex::OpcodeKind kind) const {
    return std::ranges::count_if(
        program.generatedOpcodes,
        [&](const CellRef opcode) { return vm.getOpcodeKind(opcode) == kind; });
  }
};

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_stream_comparison_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

TEST(VQLStreamComparisonVMTest, CartesianExistentialCoversAllSixOperators) {
  Harness h;
  const auto one = h.integer(1);
  const auto two = h.integer(2);
  const Bindings bindings{{"left", {one, two}}, {"right", {two}}};

  EXPECT_TRUE(h.matches(h.predicate("$left = $right", bindings)));
  EXPECT_TRUE(h.matches(h.predicate("$left != $right", bindings)));
  EXPECT_TRUE(h.matches(h.predicate("$left < $right", bindings)));
  EXPECT_TRUE(h.matches(h.predicate("$left <= $right", bindings)));
  EXPECT_TRUE(h.matches(h.predicate("$left >= $right", bindings)));
  EXPECT_FALSE(h.matches(h.predicate("$left > $right", bindings)));

  EXPECT_TRUE(h.matches(h.predicate("$right > $left", bindings)));
  EXPECT_TRUE(h.matches(h.predicate("$right != $left", bindings)));
  EXPECT_TRUE(h.matches(h.predicate("$left = $left", bindings)));
}

TEST(VQLStreamComparisonVMTest, EmptyOperandMakesEveryOperatorFalse) {
  Harness h;
  const auto one = h.integer(1);
  const Bindings bindings{{"empty", {}}, {"one", {one}}};
  for (const std::string_view op : {"=", "!=", "<", "<=", ">", ">="}) {
    SCOPED_TRACE(op);
    EXPECT_FALSE(h.matches(
        h.predicate("$empty " + std::string(op) + " $one", bindings)));
    EXPECT_FALSE(h.matches(
        h.predicate("$one " + std::string(op) + " $empty", bindings)));
    EXPECT_FALSE(h.matches(
        h.predicate("$empty " + std::string(op) + " $empty", bindings)));
  }
}

TEST(VQLStreamComparisonVMTest, ScalarLeavesKeepExactTypedComparison) {
  Harness h;
  EXPECT_FALSE(h.matches(h.predicate("9007199254740992 = 9007199254740993")));
  EXPECT_TRUE(h.matches(h.predicate("9007199254740992 != 9007199254740993")));
  EXPECT_TRUE(h.matches(h.predicate(R"("01" = 1)")));
  EXPECT_FALSE(h.matches(h.predicate(R"("x" = 1)")));
}

TEST(VQLStreamComparisonVMTest,
     OperandsCompileOnceAndProgramShapeIgnoresProductCardinality) {
  Harness h;
  const auto closureProgram = h.compile(
      R"(let $left := \() @1;
         let $right := \() @2;
         return ##[$left() < $right()])");
  ASSERT_TRUE(closureProgram.success) << closureProgram.errorMessage;
  EXPECT_EQ(
      h.count(closureProgram, zigzag::vortex::OpcodeKind::CheckClosureCall),
      2u);
  EXPECT_EQ(h.count(closureProgram, zigzag::vortex::OpcodeKind::ApplyClosure),
            2u);
  EXPECT_EQ(h.count(closureProgram, zigzag::vortex::OpcodeKind::VqlCompare),
            1u);

  std::vector<CellRef> small(2, h.integer(1));
  std::vector<CellRef> large(64, h.integer(1));
  const auto smallProgram = h.compile("return ##[$left = $right]",
                                      {{"left", small}, {"right", small}});
  const auto largeProgram = h.compile("return ##[$left = $right]",
                                      {{"left", large}, {"right", large}});
  ASSERT_TRUE(smallProgram.success) << smallProgram.errorMessage;
  ASSERT_TRUE(largeProgram.success) << largeProgram.errorMessage;
  EXPECT_EQ(h.count(smallProgram, zigzag::vortex::OpcodeKind::VqlCompare), 1u);
  EXPECT_EQ(h.count(largeProgram, zigzag::vortex::OpcodeKind::VqlCompare), 1u);
  EXPECT_EQ(smallProgram.generatedOpcodes.size(),
            largeProgram.generatedOpcodes.size());
}

TEST(VQLStreamComparisonVMTest,
     OperandEffectsRunOnceInOrderEvenWhenLeftStreamIsEmpty) {
  constexpr std::string_view source = R"(
      def left($ignored := $mark/value("L", 0, 0)) := $lhs;
      def right($ignored := $mark/value("R", 0, 0)) := $rhs;
      return ##[left() = right()]
    )";
  Harness h;
  const auto lhs0 = h.integer(1);
  const auto lhs1 = h.integer(2);
  const auto mark = h.arena.makeCell("");
  const Bindings nonempty{
      {"lhs", {lhs0, lhs1}}, {"rhs", {lhs0, lhs1}}, {"mark", {mark}}};
  const auto matched = h.execute(source, nonempty);
  ASSERT_TRUE(matched) << matched.error().message;
  ASSERT_EQ(matched->size(), 1u);
  EXPECT_EQ(h.arena.textOf(mark), "RL");

  const auto emptyMark = h.arena.makeCell("");
  const Bindings emptyLeft{
      {"lhs", {}}, {"rhs", {lhs0, lhs1}}, {"mark", {emptyMark}}};
  const auto empty = h.execute(source, emptyLeft);
  ASSERT_TRUE(empty) << empty.error().message;
  EXPECT_TRUE(empty->empty());
  EXPECT_EQ(h.arena.textOf(emptyMark), "RL");
}

TEST(VQLStreamComparisonVMTest, KnownVersionsRemainCausalAndRuntimeRefuses) {
  Harness h;
  EXPECT_TRUE(h.matches(h.predicate("@9 < @10")));
  EXPECT_FALSE(h.matches(h.predicate("@10 < @9")));
  EXPECT_TRUE(h.matches(h.predicate("@2a1 != @2b1")));
  EXPECT_FALSE(h.matches(h.predicate("@2a1 < @2b1")));

  const CellRef item = h.integer(3);
  const auto items   = h.coordinator.resolveDimension("d.items");
  ASSERT_TRUE(h.arena.link(h.core.home(), items, zigzag::DimVector::POS, item));
  const auto refused =
      h.execute("for $cell in ##/d.items/count() return ##[$cell < @5]");
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().category, QueryErrorCategory::Unsupported);
  EXPECT_NE(refused.error().message.find("version comparison"),
            std::string::npos);
}

TEST(VQLStreamComparisonVMTest, EmptyCallDoesNotInventARequiredArgument) {
  Harness h;
  const auto result = h.execute("def need($x: Cell): Cell := $x; need()");
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_TRUE(result->empty());
}

TEST(VQLStreamComparisonVMTest,
     ComparisonRunsInReloadedHostAndSourceMeshPrograms) {
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  constexpr std::string_view source = R"(
      let $a := ##/d.items%"a";
      let $b := $a/d.next%"b";
      let $c := $b/d.next%"c";
      let $lhs := $a/+d.next;
      let $root := ## where $lhs != $b return string("different");
    )";
  {
    zigzag::ArenaManifold sourceArena;
    zigzag::vortex::VortexCore sourceCore(sourceArena);
    MultiStoreCoordinator sourceCoordinator(sourceCore);
    zigzag::vortex::VortexVM sourceVm(sourceCore);
    VQLCompiler compiler(sourceCoordinator, sourceVm);
    Parser parser(source);
    auto ast            = parser.parseQuery();
    const auto compiled = compiler.compile(ast);
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    ast = QueryExpression{};
    xanadu::Store artifact(scroll);
    compiler.exportToStore(artifact);
    artifact.save((temporary.path / "program").string());
  }

  xanadu::Store artifact(scroll);
  artifact.load((temporary.path / "program").string());
  ASSERT_TRUE(artifact.structureHead());
  const auto manifold = artifact.rebuildManifold(*artifact.structureHead());
  zigzag::ArenaManifold loadedArena(&manifold, artifact);
  zigzag::vortex::VortexCore loadedCore(loadedArena);
  zigzag::vortex::VortexVM loadedVm(loadedCore);
  const auto entry = loadedArena.linked(
      loadedCore.home(), loadedCore.dims().spin, zigzag::DimVector::POS);
  ASSERT_TRUE(entry);
  const auto execution = loadedVm.run(
      loadedVm.spawnCursor(*entry, "stream-comparison-reload"), 10'000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  std::optional<CellRef> halt;
  auto opcode = entry;
  for (std::size_t count = 0; opcode && count < 10'000; ++count) {
    if (loadedVm.getOpcodeKind(*opcode) == zigzag::vortex::OpcodeKind::Halt) {
      halt = opcode;
      break;
    }
    opcode = loadedArena.linked(*opcode, loadedCore.dims().spin,
                                zigzag::DimVector::POS);
  }
  ASSERT_TRUE(halt);
  const auto outputs = loadedCore.inputsOf(*halt);
  ASSERT_EQ(outputs.size(), 1u);
  const auto loadedValues = loadedVm.resultCells(outputs.front());
  ASSERT_EQ(loadedValues.size(), 1u);
  EXPECT_EQ(loadedArena.textOf(loadedValues.front()), "different");

  zigzag::vortex::VortexHost host;
  const auto hosted = host.executeVQLChecked(source);
  ASSERT_TRUE(hosted) << hosted.error().message;
  ASSERT_EQ(hosted->size(), 1u);
  EXPECT_EQ(host.core().arena().textOf(hosted->front()), "different");

  namespace mesh    = zigzag::vortex::mesh;
  const auto meshed = mesh::evaluate(mesh::Frontend::Vquery, source,
                                     {.maxCycles      = 10'000,
                                      .maxCells       = 10'000,
                                      .maxSolutions   = 16,
                                      .maxOutputBytes = 1024 * 1024});
  ASSERT_TRUE(meshed) << meshed.error();
  auto &meshArena    = meshed->context->arena;
  const auto decoded = zigzag::vortex::mesh::readResultCells(*meshed);

  ASSERT_TRUE(decoded) << decoded.error();

  ASSERT_EQ(decoded->size(), 1U);

  const auto output = std::optional<zigzag::CellRef>{decoded->front()};
  EXPECT_EQ(meshArena.textOf(*output), "different");
}

TEST(VQLStreamComparisonVMTest, EarlyMatchAndBudgetFailureCanRetrySameVM) {
  Harness h;
  std::vector<CellRef> left;
  std::vector<CellRef> right;
  left.reserve(100);
  right.reserve(100);
  for (std::int64_t i = 0; i < 100; ++i) {
    left.push_back(h.integer(i));
    right.push_back(h.integer(i + 1000));
  }
  right.front() = left.front();

  const auto early =
      h.predicate("$left = $right", {{"left", left}, {"right", right}}, 10'000);
  ASSERT_TRUE(early) << early.error().message;
  ASSERT_EQ(early->size(), 1u);

  std::fill(right.begin(), right.end(), h.integer(1000));
  const auto exhausted =
      h.predicate("$left = $right", {{"left", left}, {"right", right}}, 10'000);
  ASSERT_FALSE(exhausted);
  EXPECT_EQ(exhausted.error().category, QueryExecutionError::Category::Budget);

  const auto retry = h.predicate(
      "$left = $right", {{"left", {left.front()}}, {"right", {left.front()}}},
      10'000);
  ASSERT_TRUE(retry) << retry.error().message;
  EXPECT_EQ(retry->size(), 1u);
}

TEST(VQLStreamComparisonVMTest, ReusingOneCompiledLoopResetsItsIterators) {
  Harness h;
  const auto left    = h.integer(1);
  const auto right   = h.integer(1);
  const auto program = h.compile("return ##[$left = $right]",
                                 {{"left", {left}}, {"right", {right}}});
  ASSERT_TRUE(program.success) << program.errorMessage;
  ASSERT_TRUE(program.cursor);
  ASSERT_TRUE(program.entryOpcode);

  const auto runAgain = [&]() -> Result {
    h.vm.setCursorOpcode(*program.cursor, program.entryOpcode);
    const auto execution = h.vm.run(*program.cursor, 10'000);
    h.vm.abortScopedChoices(*program.cursor);
    h.vm.setCursorOpcode(*program.cursor, std::nullopt);
    if (!execution.success)
      return std::unexpected(QueryExecutionError{
          .category = QueryExecutionError::Category::Runtime,
          .message  = execution.errorMessage});
    std::vector<CellRef> values;
    for (const CellRef slot : program.resultSlots) {
      const auto results = h.vm.resultCells(slot);
      values.insert(values.end(), results.begin(), results.end());
    }
    return values;
  };

  const auto first = runAgain();
  ASSERT_TRUE(first) << first.error().message;
  ASSERT_EQ(first->size(), 1u);
  ASSERT_TRUE(h.core.writeValue(right, 0, std::nullopt,
                                zigzag::vortex::CellValue(std::int64_t{2})));
  const auto second = runAgain();
  ASSERT_TRUE(second) << second.error().message;
  EXPECT_TRUE(second->empty());
  ASSERT_TRUE(h.core.writeValue(right, 0, std::nullopt,
                                zigzag::vortex::CellValue(std::int64_t{1})));
  const auto third = runAgain();
  ASSERT_TRUE(third) << third.error().message;
  EXPECT_EQ(third->size(), 1u);
}

} // namespace
} // namespace xanadu::vql

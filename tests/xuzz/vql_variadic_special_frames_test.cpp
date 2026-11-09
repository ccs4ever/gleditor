#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;
using Bindings = std::unordered_map<std::string, std::vector<CellRef>>;

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};

  CellRef cell(const std::string_view value) { return arena.makeCell(value); }

  CellRef integer(const std::int64_t value) {
    return arena.makeScalarCell(value);
  }

  void link(const CellRef from, const std::string_view dimension,
            const CellRef to) {
    zigzag::expectWritten(arena.link(
        from, coordinator.resolveDimension(dimension), DimVector::POS, to));
  }

  CompilationResult compile(const std::string_view source) {
    VQLCompiler compiler(coordinator, vm);
    return compiler.compile(source);
  }

  std::expected<std::vector<CellRef>, QueryExecutionError>
  run(const std::string_view source, Bindings bindings = {}) {
    QueryExecutor executor(coordinator, vm, {}, std::move(bindings));
    return executor.execute(source);
  }

  std::size_t invokes(const CompilationResult &compiled) const {
    std::size_t count = 0;
    for (const CellRef opcode : compiled.generatedOpcodes)
      count += vm.getOpcodeKind(opcode) == OpcodeKind::InvokeFrame;
    return count;
  }
};

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_variadic_special_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

TEST(VQLVariadicSpecialFramesTest,
     OmittedNamedRestDefaultRunsInAValueCallAndReturnsItsValue) {
  Harness h;
  constexpr std::string_view source =
      "def pure collect(...$rest: Int := 7): Stream<Int> := $rest; "
      "return (collect())";
  const auto compiled = h.compile(source);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_GT(h.invokes(compiled), 0u);
  const auto result = h.run(source);
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ(h.arena.asInt64(result->front()), 7);
}

TEST(VQLVariadicSpecialFramesTest,
     ExplicitEmptyRestActualSkipsTheWholeRestDefault) {
  Harness h;
  constexpr std::string_view source =
      "def pure collect(...$rest: Int := 7): Stream<Int> := $rest; "
      "let $empty := ##/d.missing; "
      "return (collect($empty))";
  const auto compiled = h.compile(source);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_GT(h.invokes(compiled), 0u);
  const auto result = h.run(source);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_TRUE(result->empty());
}

TEST(VQLVariadicSpecialFramesTest, WholeRestDefaultCanUseAnEarlierFixedFormal) {
  Harness h;
  constexpr std::string_view source =
      "def pure collect($seed: Int, ...$rest: Int := $seed): "
      "Stream<Int> := $rest; return (collect(9))";
  const auto result = h.run(source);
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ(h.arena.asInt64(result->front()), 9);
}

TEST(VQLVariadicSpecialFramesTest,
     WholeRestDefaultUsesOneOrderedGroupAndKeepsDuplicateValues) {
  Harness h;
  const CellRef first  = h.integer(1);
  const CellRef second = h.integer(2);
  constexpr std::string_view source =
      "def pure collect(...$rest: Stream<Int> := $values): "
      "Stream<Int> := $rest; return (collect())";
  const auto result = h.run(source, {{"values", {first, second, first}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{first, second, first}));
}

TEST(VQLVariadicSpecialFramesTest,
     DotRestDefaultSuppliesTheContextValueAndEmptyActualSkipsIt) {
  Harness h;
  constexpr std::string_view source =
      "def pure context(... .: Int := 7): Stream<Int> := .; "
      "return (context())";
  const auto omitted = h.run(source);
  ASSERT_TRUE(omitted) << omitted.error().message;
  ASSERT_EQ(omitted->size(), 1u);
  EXPECT_EQ(h.arena.asInt64(omitted->front()), 7);

  const auto empty =
      h.run("def pure context(... .: Int := 7): Stream<Int> := .; "
            "let $empty := ##/d.missing; return (context($empty))");
  ASSERT_TRUE(empty) << empty.error().message;
  EXPECT_TRUE(empty->empty());
}

TEST(VQLVariadicSpecialFramesTest,
     DestructureDefaultAliasDoesNotShadowAnOuterBinding) {
  Harness h;
  const CellRef outer = h.integer(99);
  constexpr std::string_view wrapped =
      "def pure reveal(...{ .: $outside: Int } := 7): Int := $outside; "
      "return (reveal())";
  const auto result = h.run(wrapped, {{"outside", {outer}}});
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ(result->front(), outer);
}

TEST(VQLVariadicSpecialFramesTest,
     RestCallDefaultFlowsThroughWrapperAndUfcsContext) {
  Harness h;
  const CellRef first  = h.integer(1);
  const CellRef second = h.integer(2);
  constexpr std::string_view wrapped =
      "def pure fallback($values: Stream<Int>): Stream<Int> := $values; "
      "def pure select($head: Stream<Int>, ...$rest: Stream<Int> := "
      "fallback($head)): Stream<Int> := $rest; "
      "def pure wrapper($items: Stream<Int>): Stream<Int> := select($items); "
      "wrapper($items)";
  const auto wrappedResult = h.run(wrapped, {{"items", {first, second}}});
  ASSERT_TRUE(wrappedResult) << wrappedResult.error().message;
  EXPECT_EQ(*wrappedResult, (std::vector<CellRef>{first, second}));

  const CellRef owner = h.cell("owner");
  const CellRef score = h.integer(5);
  h.link(owner, "d.score", score);
  h.link(h.core.home(), "d.items", owner);
  constexpr std::string_view ufcs =
      "def pure select($head: Stream<Cell>, ...$rest: Stream<Int> := "
      "./+d.score): Stream<Int> := $rest; ##/d.items/select()";
  const auto ufcsResult = h.run(ufcs);
  ASSERT_TRUE(ufcsResult) << ufcsResult.error().message;
  EXPECT_EQ(*ufcsResult, (std::vector<CellRef>{score}));
}

TEST(VQLVariadicSpecialFramesTest,
     FixedParameterMismatchFiltersBeforeWholeRestDefaultAndBody) {
  Harness h;
  const CellRef sentinel = h.cell("body ran");
  constexpr std::string_view source =
      "def pure guarded($fixed: Int, ...$rest: Int := 7): Cell := "
      "$sentinel; guarded(\"bad\")";
  const auto result = h.run(source, {
                                        {"sentinel", {sentinel}},
                                    });
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_TRUE(result->empty());
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VQLVariadicSpecialFramesTest,
     FilteredParameterAndBrokenReturnPromiseRemainDifferentErrors) {
  Harness h;
  const auto filtered =
      h.run("def pure guarded($value: Int, ...$rest: Int): String := \"bad\"; "
            "guarded(\"wrong\")");
  ASSERT_TRUE(filtered) << filtered.error().message;
  EXPECT_TRUE(filtered->empty());

  const auto broken =
      h.run("def pure broken(...$rest: Int): String := $rest; broken(7)");
  ASSERT_FALSE(broken);
  EXPECT_EQ(broken.error().category,
            QueryExecutionError::Category::TypeContract);
  EXPECT_NE(broken.error().message.find("broken"), std::string::npos);
}

TEST(VQLVariadicSpecialFramesTest,
     ZeroAndSingleRestGroupsRecurseAndReloadWithoutCompilerState) {
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Harness h;
    constexpr std::string_view source =
        "def pure count($n: Int, ...$rest: Int := 7): Stream<Int> := { "
        "if $n > 0 return count($n + -1, $rest) else return $rest; }; "
        "count(2)";
    VQLCompiler compiler(h.coordinator, h.vm);
    const auto compiled = compiler.compile(source);
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    EXPECT_GT(h.invokes(compiled), 0u);
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
  const auto execution =
      vm.run(vm.spawnCursor(*entry, "variadic-special-reload"), 100'000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  auto opcode = entry;
  std::optional<CellRef> halt;
  for (std::size_t count = 0; opcode && count < 100'000; ++count) {
    if (vm.getOpcodeKind(*opcode) == OpcodeKind::Halt) {
      halt = opcode;
      break;
    }
    opcode = arena.linked(*opcode, core.dims().spin, DimVector::POS);
  }
  ASSERT_TRUE(halt);
  const auto resultPorts = core.inputsOf(*halt);
  ASSERT_EQ(resultPorts.size(), 1u);
  const auto result = vm.resultCells(resultPorts.front());
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(arena.asInt64(result.front()), 7);
  EXPECT_FALSE(vm.hasActiveFrames());
}

TEST(VQLVariadicSpecialFramesTest,
     NonFinalRestRefusesWithoutWritesAndLeavesTheVmAvailable) {
  Harness h;
  const auto before = h.arena.cellCount();
  const auto unsupported =
      h.compile("def pure invalid(...$rest: Int, $last: Int): Stream<Int> := "
                "$rest; invalid(1, 2)");
  EXPECT_FALSE(unsupported.success);
  EXPECT_EQ(h.arena.cellCount(), before);

  const auto retry = h.run(
      "def pure collect(...$rest: Int): Stream<Int> := $rest; collect(1, 2)");
  ASSERT_TRUE(retry) << retry.error().message;
  ASSERT_EQ(retry->size(), 2u);
  EXPECT_EQ(h.arena.asInt64((*retry)[0]), 1);
  EXPECT_EQ(h.arena.asInt64((*retry)[1]), 2);
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

} // namespace
} // namespace xanadu::vql

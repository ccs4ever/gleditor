#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/mesh_evaluator.hpp"
#include "common/xanadu/vortex/vortex_host.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/zigzag/query_scalar.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;
using Bindings = std::unordered_map<std::string, std::vector<CellRef>>;
using Result   = std::expected<std::vector<CellRef>, std::string>;

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};
  CompilationResult compiled;

  CompilationResult compile(std::string_view source, Bindings bindings = {}) {
    VQLCompiler compiler(coordinator, vm);
    compiled =
        compiler.compile(source, {.initialBindings = std::move(bindings)});
    return compiled;
  }

  Result execute() {
    if (!compiled.success) return std::unexpected(compiled.errorMessage);
    if (!compiled.cursor) return std::unexpected("missing compiled cursor");
    const auto execution = vm.run(*compiled.cursor, 100'000);
    if (!execution.success) {
      vm.abortScopedChoices(*compiled.cursor);
      vm.setCursorOpcode(*compiled.cursor, std::nullopt);
      return std::unexpected(execution.errorMessage);
    }
    std::vector<CellRef> values;
    for (const auto slot : compiled.resultSlots) {
      const auto cells = vm.resultCells(slot);
      values.insert(values.end(), cells.begin(), cells.end());
    }
    vm.setCursorOpcode(*compiled.cursor, std::nullopt);
    return values;
  }

  Result run(std::string_view source, Bindings bindings = {}) {
    compile(source, std::move(bindings));
    return execute();
  }

  std::size_t count(OpcodeKind kind) const {
    std::size_t count = 0;
    for (const auto opcode : compiled.generatedOpcodes)
      if (vm.getOpcodeKind(opcode) == kind) ++count;
    return count;
  }
};

TEST(VQLClosureValueFramesTest,
     StoredIdentityUsesCreateApplyAndDetachedFrames) {
  Harness h;
  const auto a      = h.arena.makeCell("a");
  const auto b      = h.arena.makeCell("b");
  const auto result = h.run(R"(let $f := \($x) $x; return $input / $f)",
                            {{"input", {a, b, a}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{a, b, a}));
  EXPECT_GT(h.count(OpcodeKind::CreateClosure), 0u);
  EXPECT_GT(h.count(OpcodeKind::ApplyClosure), 0u);
  EXPECT_GT(h.count(OpcodeKind::ReturnFrame), 0u);
  EXPECT_NE(h.compiled.disassembly.find("CREATE_CLOSURE"), std::string::npos);
  EXPECT_EQ(h.vm.activationDepth(*h.compiled.cursor), 0u);
}

TEST(VQLClosureValueFramesTest,
     MembershipSnapshotsPreserveDuplicatesAndEmptyAliases) {
  Harness h;
  const auto a           = h.arena.makeCell("a");
  const auto b           = h.arena.makeCell("b");
  const auto replacement = h.arena.makeCell("replacement");
  const auto one         = h.arena.makeCell("one");
  const auto two         = h.arena.makeCell("two");
  const auto result      = h.run(
      R"(let $alias := $before; let $f := \($x) $alias;
         let $alias := $after; return $input / $f)",
      {{"before", {a, b, a}}, {"after", {replacement}}, {"input", {one, two}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{a, b, a, a, b, a}));
  const auto empty = h.run(
      R"(let $f := \($x) $missing; let $missing := $later;
         return $input / $f)",
      {{"later", {replacement}}, {"input", {one, two}}});
  ASSERT_TRUE(empty) << empty.error();
  EXPECT_TRUE(empty->empty());
}

TEST(VQLClosureValueFramesTest, CapturedCellsRetainLiveContentAndIdentity) {
  Harness h;
  const auto captured = h.arena.makeCell("before");
  const auto item     = h.arena.makeCell("candidate");
  const auto compiled =
      h.compile(R"(let $f := \($x) $captured; return $input / $f)",
                {{"captured", {captured}}, {"input", {item}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  ASSERT_TRUE(compiled.cursor);
  std::optional<CellRef> create;
  for (const auto opcode : compiled.generatedOpcodes)
    if (h.vm.getOpcodeKind(opcode) == OpcodeKind::CreateClosure)
      create = opcode;
  ASSERT_TRUE(create);
  std::size_t steps = 0;
  while (h.vm.getCursorOpcode(*compiled.cursor) != create && steps++ < 1000) {
    const auto step = h.vm.step(*compiled.cursor);
    ASSERT_TRUE(step.success) << step.errorMessage;
  }
  ASSERT_EQ(h.vm.getCursorOpcode(*compiled.cursor), create);
  const auto creation = h.vm.step(*compiled.cursor);
  ASSERT_TRUE(creation.success) << creation.errorMessage;
  const auto text = h.arena.intern("after");
  ASSERT_TRUE(h.arena.setContent(captured, std::span{&text, std::size_t{1}}));
  const auto result = h.execute();
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{captured}));
  EXPECT_EQ(h.arena.textOf(result->front()), "after");
}

TEST(VQLClosureValueFramesTest,
     PredicateContextAndFreeAliasesHaveSeparateScopes) {
  Harness h;
  const auto owner  = h.arena.makeCell("owner");
  const auto keep   = h.arena.makeCell("keep");
  const auto drop   = h.arena.makeCell("drop");
  const auto wanted = h.arena.makeCell("keep");
  const auto tag    = h.coordinator.resolveDimension("d.tag");
  ASSERT_TRUE(h.arena.link(owner, tag, DimVector::POS, keep));
  ASSERT_TRUE(h.arena.link(keep, tag, DimVector::POS, drop));
  const auto result = h.run(
      R"(let $f := \($x) $x/d.tag[. = $wanted];
         let $wanted := "drop"; return $input / $f)",
      {{"wanted", {wanted}}, {"input", {owner, owner}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{keep, keep}));
}

constexpr std::string_view recursiveMaker = R"(
  def pure make($depth: Int) := {
    let $owned := string(9007199254740993);
    if $depth > 0 return (make($depth + -1))
    else return (\($item) $owned);
  };
  let $f := make(2); return ## / $f
)";

TEST(VQLClosureValueFramesTest, RecursiveNativeMakerReturnsAnEscapingClosure) {
  Harness h;
  const auto before = h.arena.cellCount();
  const auto result = h.run(recursiveMaker);
  ASSERT_TRUE(result) << result.error();
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ(h.arena.textOf(result->front()), "9007199254740993");
  EXPECT_GE(h.arena.cellCount(), before);
  EXPECT_GT(h.count(OpcodeKind::InvokeFrame), 0u);
  EXPECT_GT(h.count(OpcodeKind::CreateClosure), 0u);
  EXPECT_GT(h.count(OpcodeKind::ApplyClosure), 0u);
  EXPECT_EQ(h.vm.activationDepth(*h.compiled.cursor), 0u);
}

TEST(VQLClosureValueFramesTest,
     LoopClosuresOwnEachIterationBindingAfterFrameReturn) {
  Harness h;
  const auto a         = h.arena.makeCell("a");
  const auto b         = h.arena.makeCell("b");
  const auto candidate = h.arena.makeCell("application candidate");
  const auto result    = h.run(R"(
      def pure build($xs: Stream<Cell>) := {
        for $item in $xs return (\($x) $item);
      };
      for $f in build($input) return $candidate / $f
    )",
                               {{"input", {a, b, a}}, {"candidate", {candidate}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{a, b, a}));
  EXPECT_GT(h.count(OpcodeKind::InvokeFrame), 0u);
  EXPECT_EQ(h.count(OpcodeKind::CreateClosure), 1u);
  EXPECT_GT(h.count(OpcodeKind::ApplyClosure), 0u);
}

TEST(VQLClosureValueFramesTest,
     ClosureValuesStayOutsideScalarAndNumericCoercions) {
  Harness h;
  const auto value = h.run(R"(let $f := \($x) $x; return $f)");
  ASSERT_TRUE(value) << value.error();
  ASSERT_EQ(value->size(), 1u);
  EXPECT_NE(value->front(), zigzag::noCell);
  ASSERT_TRUE(h.vm.closureValue(value->front()));
  EXPECT_FALSE(h.run(R"(let $f := \($x) $x; return (string($f)))"));
  EXPECT_FALSE(h.run(R"(let $f := \($x) $x; return ($f = $f))"));
  const auto pointerAsNumber =
      h.arena.makeScalarCell(static_cast<std::int64_t>(value->front()));
  const auto input = h.arena.makeCell("input");
  EXPECT_FALSE(h.run("return $input/$bad",
                     {{"input", {input}}, {"bad", {pointerAsNumber}}}));
}

TEST(VQLClosureValueFramesTest,
     HandlesRequireOneValueButEmptyInputSkipsApplication) {
  Harness h;
  const auto value = h.run(R"(let $f := \($x) $x; return $f)");
  ASSERT_TRUE(value) << value.error();
  ASSERT_EQ(value->size(), 1u);
  const auto input = h.arena.makeCell("input");
  EXPECT_FALSE(h.run("return $input/$f", {{"input", {input}}, {"f", {}}}));
  EXPECT_FALSE(
      h.run("return $input/$f",
            {{"input", {input}}, {"f", {value->front(), value->front()}}}));
  const auto empty = h.run("return $input/$f", {{"input", {}}, {"f", {}}});
  ASSERT_TRUE(empty) << empty.error();
  EXPECT_TRUE(empty->empty());
}

TEST(VQLClosureValueFramesTest, CapturedNumericKindsAndExactBitsStayDistinct) {
  Harness h;
  const auto integer  = h.arena.makeScalarCell(INT64_C(9007199254740993));
  const auto floating = h.arena.makeScalarCell(9007199254740992.0);
  const auto item     = h.arena.makeCell("item");
  const auto result =
      h.run(R"(let $f := \($x) $numbers; return $input/$f)",
            {{"numbers", {integer, floating, integer}}, {"input", {item}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{integer, floating, integer}));
  EXPECT_EQ(h.arena.valueKindOf(integer), xanadu::ValueKind::Int64);
  EXPECT_EQ(h.arena.valueKindOf(floating), xanadu::ValueKind::Double);
  EXPECT_EQ(h.arena.asInt64(integer), std::optional{INT64_C(9007199254740993)});
}

TEST(VQLClosureValueFramesTest,
     UnsupportedSignaturesAndLimitsRollbackAndRetry) {
  for (const auto source :
       {R"(let $f := \() $outside; return $f)",
        R"(let $f := \(.) .; return $f)",
        R"(let $f := \($x: Int) $x; return $f)",
        R"(let $f := \($x := 0) $x; return $f)",
        R"(let $f := \($x) ##; return $f)", R"(let $f := \($x) .; return $f)",
        R"(let $f := \($x) string($x); return $f)",
        R"(let $f := \($x) \($y) $y; return $f)"}) {
    Harness h;
    const auto before = h.arena.cellCount();
    EXPECT_FALSE(h.compile(source).success) << source;
    EXPECT_EQ(h.arena.cellCount(), before) << source;
    const auto retry = h.run(R"(let $f := \($x) $x; return ##/$f)");
    ASSERT_TRUE(retry) << retry.error();
    EXPECT_EQ(*retry, (std::vector<CellRef>{h.core.home()}));
  }
  Harness h;
  VQLCompiler compiler(h.coordinator, h.vm);
  CompilationOptions options;
  options.limits.maxNamedFrameVariants = 0;
  const auto before                    = h.arena.cellCount();
  EXPECT_FALSE(compiler.compile(R"(return (\($x) $x))", options).success);
  EXPECT_EQ(h.arena.cellCount(), before);
  EXPECT_TRUE(compiler.compile(R"(return (\($x) $x))").success);
}

TEST(VQLClosureValueFramesTest,
     CyclicLambdaBodyAndDirectRouteRefuseWithoutEffects) {
  Harness h;
  Parser parser(R"(return (\($x) $x))");
  auto query     = parser.parseQuery();
  auto &block    = std::get<ExecutionBlock>(query.body.statements.front());
  auto &clause   = std::get<ReturnClause>(block.action.clause);
  auto &returned = std::get<ReturnValue>(clause.items.front().item);
  auto lambda = std::get<std::shared_ptr<PathLambda>>(returned.expression.kind);
  ASSERT_TRUE(lambda);
  auto cyclicBody  = std::make_shared<ValueExpr>();
  cyclicBody->kind = lambda;
  lambda->body     = cyclicBody;
  VQLCompiler compiler(h.coordinator, h.vm);
  const auto before  = h.arena.cellCount();
  const auto refused = compiler.compile(query);
  EXPECT_FALSE(refused.success);
  EXPECT_NE(refused.errorMessage.find("cycl"), std::string::npos);
  EXPECT_EQ(h.arena.cellCount(), before);
  cyclicBody->kind = std::string{"x"};
  EXPECT_TRUE(compiler.compile(query).success);
  QueryExecutor direct(h.coordinator, h.vm,
                       {.mode = QueryExecutionMode::Direct});
  const auto directResult = direct.execute(R"(return (\($x) $x))");
  EXPECT_FALSE(directResult);
}

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_closure_value_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

TEST(VQLClosureValueFramesTest,
     RecursiveGraphsReloadWithoutAstAndHostMeshUseSource) {
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Harness h;
    VQLCompiler compiler(h.coordinator, h.vm);
    Parser parser(recursiveMaker);
    auto ast            = parser.parseQuery();
    const auto compiled = compiler.compile(ast);
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    ast = QueryExpression{};
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
      vm.run(vm.spawnCursor(*entry, "lambda-value-reload"), 10000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  auto opcode = entry;
  std::optional<CellRef> halt;
  for (std::size_t count = 0; opcode && count < 10000; ++count) {
    if (vm.getOpcodeKind(*opcode) == OpcodeKind::Halt) {
      halt = opcode;
      break;
    }
    opcode = arena.linked(*opcode, core.dims().spin, DimVector::POS);
  }
  ASSERT_TRUE(halt);
  const auto outputs = core.inputsOf(*halt);
  ASSERT_EQ(outputs.size(), 1u);
  const auto values = vm.resultCells(outputs.front());
  ASSERT_EQ(values.size(), 1u);
  EXPECT_EQ(arena.textOf(values.front()), "9007199254740993");
  zigzag::vortex::VortexHost host;
  const auto hosted = host.executeVQLChecked(recursiveMaker);
  ASSERT_TRUE(hosted) << hosted.error().message;
  ASSERT_EQ(hosted->size(), 1u);
  EXPECT_EQ(host.core().arena().textOf(hosted->front()), "9007199254740993");
  namespace mesh    = zigzag::vortex::mesh;
  const auto meshed = mesh::evaluate(mesh::Frontend::Vquery, recursiveMaker,
                                     {.maxCycles      = 10000,
                                      .maxCells       = 10000,
                                      .maxSolutions   = 16,
                                      .maxOutputBytes = 1024 * 1024});
  ASSERT_TRUE(meshed) << meshed.error();
  auto &meshArena = meshed->context->arena;
  std::optional<CellRef> items;
  std::optional<CellRef> value;
  for (const auto &cell : meshArena.cells()) {
    if (meshArena.textOf(cell.birthOp) == "d.mesh.items") items = cell.birthOp;
    if (meshArena.textOf(cell.birthOp) == "d.mesh.value") value = cell.birthOp;
  }
  ASSERT_TRUE(items);
  ASSERT_TRUE(value);
  const auto record = meshArena.linked(meshed->root, *items, DimVector::POS);
  ASSERT_TRUE(record);
  const auto output = meshArena.linked(*record, *value, DimVector::POS);
  ASSERT_TRUE(output);
  EXPECT_EQ(meshArena.textOf(*output), "9007199254740993");
}

} // namespace
} // namespace xanadu::vql

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
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
using Bindings = std::unordered_map<std::string, std::vector<CellRef>>;
using Result   = std::expected<std::vector<CellRef>, QueryExecutionError>;

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};

  CellRef cell(const std::string_view text) { return arena.makeCell(text); }

  CellRef integer(const std::int64_t value) {
    return arena.makeScalarCell(value);
  }

  Result run(const std::string_view source, Bindings bindings = {}) {
    QueryExecutor executor(coordinator, vm, {.maxCycles = 500'000},
                           std::move(bindings));
    return executor.execute(source);
  }
};

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_closure_signature_routes_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

constexpr std::string_view namedRestSource = R"(
  let $big := 9007199254740993;
  let $f := \($head: Int, ...$rest: Int) $rest;
  return $f(1, $big, $big, rest: $big)
)";

std::vector<std::int64_t> integerValues(const zigzag::ArenaManifold &arena,
                                        const std::vector<CellRef> &cells) {
  std::vector<std::int64_t> values;
  values.reserve(cells.size());
  for (const CellRef cell : cells) {
    const auto value = arena.asInt64(cell);
    EXPECT_TRUE(value);
    if (value) values.push_back(*value);
  }
  return values;
}

std::vector<std::int64_t>
meshIntegerValues(const zigzag::vortex::mesh::EvaluatedValue &result) {
  const auto &arena = result.context->arena;
  std::optional<zigzag::DimRef> items;
  std::optional<zigzag::DimRef> value;
  for (const auto &cell : arena.cells()) {
    if (arena.textOf(cell.birthOp) == "d.mesh.items") items = cell.birthOp;
    if (arena.textOf(cell.birthOp) == "d.mesh.value") value = cell.birthOp;
  }
  EXPECT_TRUE(items);
  EXPECT_TRUE(value);
  if (!items || !value) return {};

  std::vector<std::int64_t> values;
  auto record = arena.linked(result.root, *items, DimVector::POS);
  while (record) {
    const auto cell = arena.linked(*record, *value, DimVector::POS);
    EXPECT_TRUE(cell);
    if (!cell) break;
    const auto integer = arena.asInt64(*cell);
    EXPECT_TRUE(integer);
    if (integer) values.push_back(*integer);
    record = arena.linked(*record, *items, DimVector::POS);
  }
  return values;
}

TEST(VQLClosureSignatureRoutesTest,
     InvalidNamedCallsRefuseBeforeActualOrBodyEffects) {
  for (const std::string call :
       {"$f(unknown: $mark/value(\"actual\"))",
        "$f(left: $mark/value(\"first\"), left: $mark/value(\"second\"))",
        "$f($mark/value(\"pos\"), left: $mark/value(\"named\"))",
        "$f(left: $mark/value(\"named\"), $mark/value(\"pos\"))",
        "$f($mark/value(\"one\"), $mark/value(\"two\"), "
        "$mark/value(\"extra\"))"}) {
    SCOPED_TRACE(call);
    Harness h;
    const auto mark   = h.cell("");
    const auto body   = h.cell("");
    const auto result = h.run(
        "let $f := \\($left, $right) $body/value(\"entered\"); return " + call,
        {{"mark", {mark}}, {"body", {body}}});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().category, QueryExecutionError::Category::Runtime);
    EXPECT_EQ(h.arena.textOf(mark), "");
    EXPECT_EQ(h.arena.textOf(body), "");
    EXPECT_FALSE(h.vm.hasActiveFrames());
    EXPECT_EQ(h.vm.retainedGroupsReferences(), 0U);
  }
}

TEST(VQLClosureSignatureRoutesTest,
     ActualsRunInSourceOrderBeforeDefaultsInDeclarationOrder) {
  Harness h;
  const auto mark   = h.cell("");
  const auto result = h.run(R"(
    let $f := \($a, $b,
      $c := $mark/value("C", 0, 0), $d := $mark/value("D", 0, 0)) $a;
    return $f(b: $mark/value("A", 0, 0), a: $mark/value("B", 0, 0))
  )",
                            {{"mark", {mark}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(h.arena.textOf(mark), "DCBA");
  EXPECT_EQ(*result, (std::vector<CellRef>{mark}));
}

TEST(VQLClosureSignatureRoutesTest,
     AllActualsPrepareBeforeAParameterFiltersTheInvocation) {
  Harness h;
  const auto mark   = h.cell("");
  const auto body   = h.cell("");
  const auto result = h.run(R"(
    let $f := \($left: Int, $right: Int) $body/value("entered");
    return $f(right: $mark/value("R", 0, 0), left: $mark/value("L", 0, 0))
  )",
                            {{"mark", {mark}}, {"body", {body}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_TRUE(result->empty());
  EXPECT_EQ(h.arena.textOf(mark), "LR");
  EXPECT_EQ(h.arena.textOf(body), "");
}

TEST(VQLClosureSignatureRoutesTest,
     TypedRestFailureCleansUpAndTheSameVmCanRetry) {
  Harness h;
  const auto accepted = h.integer(1000);
  const auto filtered =
      h.run(R"(let $f := \(...$rest: Int) $rest; return $f($bad, $good))",
            {{"bad", {h.cell("not an Int")}}, {"good", {accepted}}});
  ASSERT_TRUE(filtered) << filtered.error().message;
  EXPECT_TRUE(filtered->empty());
  const auto failed = h.run(R"(
    def broken(): Int := "bad";
    let $f := \(...$rest: Int) broken();
    return $f($good)
  )",
                            {{"good", {accepted}}});
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().category,
            QueryExecutionError::Category::TypeContract);
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_EQ(h.vm.retainedGroupsReferences(), 0U);
  const auto retry =
      h.run(R"(let $f := \(...$rest: Int) $rest; return $f($good))",
            {{"good", {accepted}}});
  ASSERT_TRUE(retry) << retry.error().message;
  EXPECT_EQ(*retry, (std::vector<CellRef>{accepted}));
}

TEST(VQLClosureSignatureRoutesTest, GuardsKeepEarlierGroupSubjects) {
  Harness h;
  const auto victim   = h.integer(1000);
  const auto survivor = h.integer(2000);
  const auto result   = h.run(R"(
    let $f := \(...$rest: Int) {
      let $ignored := $victim/value("bad");
      return $rest;
    };
    return $f($victim, $survivor)
  )",
                              {{"victim", {victim}}, {"survivor", {survivor}}});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().category,
            QueryExecutionError::Category::TypeContract);
  EXPECT_EQ(h.arena.asInt64(victim), 1000);
  EXPECT_EQ(h.arena.asInt64(survivor), 2000);
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_EQ(h.vm.retainedGroupsReferences(), 0U);
}

TEST(VQLClosureSignatureRoutesTest,
     PresenceAndContextStaySeparateFromRestDefaults) {
  Harness h;
  const auto owner    = h.integer(1000);
  const auto fallback = h.integer(111);
  const auto result =
      h.run(R"(
    let $only := \(...$rest: Int := $fallback) $rest;
    let $prefix := \($head: Int, ...$rest: Int := $fallback) $rest;
    let $fixed := \($head: Stream<Int> := $fallback) $head;
    return $owner/$only, $owner/$prefix, $only(rest: $empty),
           $fixed(head: $empty), $fixed()
  )",
            {{"owner", {owner}}, {"fallback", {fallback}}, {"empty", {}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{owner, fallback, fallback}));
}

TEST(VQLClosureSignatureRoutesTest, StreamUnionMatchesEachOriginalGroup) {
  Harness h;
  const auto one    = h.integer(111);
  const auto two    = h.integer(222);
  const auto text   = h.cell("text");
  const auto result = h.run(R"(
    let $f := \(...$rest: Stream<Int> | Stream<String>) $rest;
    return $f($ints, $strings), $f(rest: $mixed)
  )",
                            {{"ints", {one, two}},
                             {"strings", {text, text}},
                             {"mixed", {one, text}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{one, two, text, text}));
}

TEST(VQLClosureSignatureRoutesTest,
     NamedAndVariadicCallsiteSurvivesAstFreeExportHostAndVqueryMesh) {
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Harness h;
    VQLCompiler compiler(h.coordinator, h.vm);
    Parser parser(namedRestSource);
    auto ast            = parser.parseQuery();
    const auto compiled = compiler.compile(ast);
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    EXPECT_TRUE(
        std::ranges::any_of(compiled.generatedOpcodes, [&](const auto op) {
          return h.vm.getOpcodeKind(op) == OpcodeKind::CheckClosureCall;
        }));
    ast = QueryExpression{};
    xanadu::Store store(scroll);
    compiler.exportToStore(store);
    store.save((temporary.path / "program").string());
  }

  xanadu::Store store(scroll);
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
      vm.run(vm.spawnCursor(*entry, "named-rest-signature-reload"), 100'000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  std::optional<CellRef> halt;
  auto opcode = entry;
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
  const auto loadedValues = vm.resultCells(resultPorts.front());
  ASSERT_EQ(loadedValues.size(), 3U);
  EXPECT_EQ(loadedValues[0], loadedValues[1]);
  EXPECT_EQ(loadedValues[1], loadedValues[2]);
  EXPECT_EQ(integerValues(arena, loadedValues),
            (std::vector<std::int64_t>{INT64_C(9007199254740993),
                                       INT64_C(9007199254740993),
                                       INT64_C(9007199254740993)}));

  zigzag::vortex::VortexHost host;
  const auto hosted = host.executeVQLChecked(namedRestSource);
  ASSERT_TRUE(hosted) << hosted.error().message;
  EXPECT_EQ(integerValues(host.arena(), *hosted),
            (std::vector<std::int64_t>{INT64_C(9007199254740993),
                                       INT64_C(9007199254740993),
                                       INT64_C(9007199254740993)}));

  namespace mesh = zigzag::vortex::mesh;
  const auto meshed =
      mesh::evaluate(mesh::Frontend::Vquery,
                     R"(let $f := \($head: Int, ...$rest: Int) $rest;
                                        return $f(1, 9007199254740993, 8, rest: 9))",
                     {.maxCycles      = 100'000,
                      .maxCells       = 100'000,
                      .maxSolutions   = 16,
                      .maxOutputBytes = 1024 * 1024});
  ASSERT_TRUE(meshed) << meshed.error();
  EXPECT_EQ(meshIntegerValues(*meshed),
            (std::vector<std::int64_t>{INT64_C(9007199254740993), 8, 9}));
}

} // namespace
} // namespace xanadu::vql

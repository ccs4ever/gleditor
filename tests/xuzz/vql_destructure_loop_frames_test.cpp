/**
 * @file vql_destructure_loop_frames_test.cpp
 * @brief Destructured FOR bindings in reusable VQL frames.
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
#include <utility>
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

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_destructure_loop_frames_" +
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
  DimRef people{coordinator.resolveDimension("d.people")};
  DimRef colors{coordinator.resolveDimension("d.colors")};
  DimRef tag{coordinator.resolveDimension("d.tag")};
  DimRef next{coordinator.resolveDimension("d.next")};
  CellRef peopleTail{core.home()};
  CellRef colorsTail{core.home()};
  std::vector<CellRef> peopleCells;
  std::vector<CellRef> colorCells;

  CellRef person(const std::string &text) {
    const CellRef cell = arena.makeCell(text);
    zigzag::expectWritten(arena.link(peopleTail, people, DimVector::POS, cell));
    peopleTail = cell;
    peopleCells.push_back(cell);
    return cell;
  }

  CellRef color(const std::string &text) {
    const CellRef cell = arena.makeCell(text);
    zigzag::expectWritten(arena.link(colorsTail, colors, DimVector::POS, cell));
    colorsTail = cell;
    colorCells.push_back(cell);
    return cell;
  }

  CellRef attach(const CellRef from, const DimRef dim,
                 const std::string &text) {
    const CellRef cell = arena.makeCell(text);
    zigzag::expectWritten(arena.link(from, dim, DimVector::POS, cell));
    return cell;
  }

  std::expected<std::vector<CellRef>, QueryExecutionError>
  run(const std::string_view source, ContractBindings bindings = {}) {
    QueryExecutor executor(coordinator, vm, {}, std::move(bindings));
    return executor.execute(source);
  }

  std::size_t invokeCount(const std::string_view source,
                          ContractBindings bindings = {}) {
    VQLCompiler compiler(coordinator, vm);
    CompilationOptions options;
    options.initialBindings = std::move(bindings);
    const auto compiled     = compiler.compile(source, options);
    EXPECT_TRUE(compiled.success) << compiled.errorMessage;
    std::size_t count = 0;
    for (const CellRef opcode : compiled.generatedOpcodes)
      count += vm.getOpcodeKind(opcode) == OpcodeKind::InvokeFrame;
    return count;
  }

  std::vector<std::string> texts(const std::vector<CellRef> &cells) const {
    std::vector<std::string> result;
    for (const CellRef cell : cells) result.push_back(arena.textOf(cell));
    return result;
  }
};

TEST(VQLDestructureLoopFrameTest,
     TypedDefaultsAndMissingUntypedFieldsKeepEachIteration) {
  Harness h;
  const CellRef alice = h.person("Alice");
  const CellRef bob   = h.person("Bob");
  const CellRef carol = h.person("Carol");
  const CellRef red   = h.attach(alice, h.tag, "red");
  const CellRef blue  = h.attach(carol, h.tag, "blue");

  const std::string typed =
      "def pure labels($people: Stream<Cell>): Stream<Cell> := { "
      "for { .: $person: Cell, +d.tag: $label: String := \"untagged\" } "
      "in $people return $person, $label; }; labels(##/d.people)";
  EXPECT_GT(h.invokeCount(typed), 0u);
  const auto labeled = h.run(typed);
  ASSERT_TRUE(labeled) << labeled.error().message;
  EXPECT_EQ(h.texts(*labeled),
            (std::vector<std::string>{"Alice", "red", "Bob", "untagged",
                                      "Carol", "blue"}));
  EXPECT_EQ((*labeled)[1], red);

  const auto untyped =
      h.run("def pure pairs($people: Stream<Cell>): Stream<Cell> := { "
            "for { .: $person: Cell, +d.tag: $label } in $people "
            "return $person, $label; }; pairs(##/d.people)");
  ASSERT_TRUE(untyped) << untyped.error().message;
  EXPECT_EQ(*untyped, (std::vector<CellRef>{alice, red, bob, carol, blue}));
}

TEST(VQLDestructureLoopFrameTest, NestedLoopsPreserveOrderAndDuplicates) {
  Harness h;
  const CellRef first  = h.person("first");
  const CellRef second = h.person("second");
  h.color("red");
  h.color("blue");
  const std::string source =
      "def pure repeat($people: Stream<Cell>, $colors: Stream<Cell>): "
      "Stream<Cell> := { for { .: $person: Cell } in $people "
      "for { .: $color: Cell } in $colors return $person; }; "
      "repeat(##/d.people, ##/d.colors)";

  EXPECT_GT(h.invokeCount(source), 0u);
  const auto result = h.run(source);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{first, first, second, second}));
}

TEST(VQLDestructureLoopFrameTest, NoForLetAndPrefixLetRemainInFrameScope) {
  Harness h;
  h.person("Alice");
  h.person("Bob");
  h.person("Carol");

  const std::string noLoop =
      "def pure constant(): Int := { let $value := 7; return $value; }; "
      "constant()";
  EXPECT_GT(h.invokeCount(noLoop), 0u);
  const auto constant = h.run(noLoop);
  ASSERT_TRUE(constant) << constant.error().message;
  ASSERT_EQ(constant->size(), 1u);
  EXPECT_EQ(h.arena.asInt64(constant->front()), 7);

  const std::string prefix =
      "def pure prefix($people: Stream<Cell>): Int := { "
      "let $value := 7 "
      "for { .: $person: Cell } in $people return $value; "
      "return $value; }; prefix(##/d.people)";
  EXPECT_GT(h.invokeCount(prefix), 0u);
  const auto result = h.run(prefix);
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ(h.arena.asInt64(result->front()), 7);
}

TEST(VQLDestructureLoopFrameTest,
     DestructureAndNamedLoopBindingsDoNotCaptureCallerAliases) {
  Harness h;
  const CellRef alice = h.person("Alice");
  h.person("Bob");
  const CellRef localTag  = h.attach(alice, h.tag, "local tag");
  const CellRef callerTag = h.arena.makeCell("caller tag");
  ContractBindings destructureBindings{{"people", {alice}},
                                       {"tag", {callerTag}}};
  const std::string destructured =
      "def pure after($people: Stream<Cell>): Cell := { "
      "for { +d.tag: $tag } in $people return $tag; return $tag; }; "
      "after($people)";
  EXPECT_EQ(h.invokeCount(destructured, destructureBindings), 0u);
  const auto destructuredResult = h.run(destructured, destructureBindings);
  ASSERT_TRUE(destructuredResult) << destructuredResult.error().message;
  EXPECT_EQ(*destructuredResult, (std::vector<CellRef>{callerTag}));
  EXPECT_NE(localTag, callerTag);

  const CellRef callerItem = h.arena.makeCell("caller item");
  ContractBindings namedBindings{{"people", {alice}}, {"item", {callerItem}}};
  const std::string named =
      "def pure after($people: Stream<Cell>): Cell := { "
      "for $item in $people return $item; return $item; }; after($people)";
  EXPECT_EQ(h.invokeCount(named, namedBindings), 0u);
  const auto namedResult = h.run(named, namedBindings);
  ASSERT_TRUE(namedResult) << namedResult.error().message;
  EXPECT_EQ(*namedResult, (std::vector<CellRef>{callerItem}));

  const CellRef callerLater = h.arena.makeCell("caller later");
  ContractBindings letBindings{{"people", {alice}}, {"later", {callerLater}}};
  const std::string afterLoopLet =
      "def pure after($people: Stream<Cell>): Cell := { "
      "for $item in $people let $later := $item return $later; "
      "return $later; }; after($people)";
  EXPECT_EQ(h.invokeCount(afterLoopLet, letBindings), 0u);
  const auto letResult = h.run(afterLoopLet, letBindings);
  ASSERT_TRUE(letResult) << letResult.error().message;
  EXPECT_EQ(*letResult, (std::vector<CellRef>{callerLater}));
}

TEST(VQLDestructureLoopFrameTest, DestructureDefaultsCanReadEarlierFormal) {
  Harness h;
  h.person("person");
  const CellRef fallback = h.arena.makeCell("outer fallback");
  const std::string source =
      "def pure choose($fallback: Cell, $people: Stream<Cell>): "
      "Stream<Cell> := { for { .: $person: Cell, +d.missing: $value: "
      "Cell := $fallback } in $people return $value; }; "
      "choose($source, ##/d.people)";
  EXPECT_GT(h.invokeCount(source, ContractBindings{{"source", {fallback}}}),
            0u);
  const auto result = h.run(source, ContractBindings{{"source", {fallback}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{fallback}));
}

TEST(VQLDestructureLoopFrameTest, RecursiveLoopGraphSurvivesFreshVMReload) {
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<UserPermascroll>(
      UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Harness h;
    const CellRef first  = h.person("first");
    const CellRef second = h.person("second");
    zigzag::expectWritten(h.arena.link(first, h.next, DimVector::POS, second));
    const std::string source =
        "def pure last($people: Stream<Cell>): Cell := { "
        "for { .: $person: Cell, +d.next: $next: Cell? } in $people "
        "if $person/+d.next return last($next) else return $person; }; "
        "last(##/d.people[1])";
    VQLCompiler compiler(h.coordinator, h.vm);
    const auto compiled = compiler.compile(source);
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    std::size_t invocations = 0;
    for (const CellRef opcode : compiled.generatedOpcodes)
      invocations += h.vm.getOpcodeKind(opcode) == OpcodeKind::InvokeFrame;
    EXPECT_GT(invocations, 0u);
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
  const auto cursor    = vm.spawnCursor(*entry, "destructure-loop-reload");
  const auto execution = vm.run(cursor, 1000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  auto halt = *entry;
  auto next = arena.linked(halt, core.dims().spin, DimVector::POS);
  while (next && vm.getOpcodeKind(*next) != OpcodeKind::Halt) {
    halt = *next;
    next = arena.linked(halt, core.dims().spin, DimVector::POS);
  }
  ASSERT_TRUE(next);
  const auto resultPorts = core.inputsOf(*next);
  ASSERT_EQ(resultPorts.size(), 1u);
  const auto results = vm.resultCells(resultPorts.front());
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(arena.textOf(results.front()), "second");
  EXPECT_FALSE(vm.hasActiveFrames());
}

TEST(VQLDestructureLoopFrameTest, HostAndMeshVquerySourcesAcceptLoopFrames) {
  zigzag::vortex::VortexHost host;
  const std::string source =
      "def pure collect($values: Stream<Int>): Stream<Int> := { "
      "for { .: $value: Int } in $values return $value; }; collect(7)";
  const auto hostResult = host.executeVQLChecked(source);
  ASSERT_TRUE(hostResult) << hostResult.error().message;
  ASSERT_EQ(hostResult->size(), 1u);
  EXPECT_EQ(host.core().arena().asInt64(hostResult->front()), 7);

  const auto meshResult = zigzag::vortex::mesh::evaluate(
      zigzag::vortex::mesh::Frontend::Vquery, source,
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
  EXPECT_EQ(arena.asInt64(*output), std::optional<std::int64_t>{7});
}

} // namespace
} // namespace xanadu::vql

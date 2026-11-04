/**
 * @file vql_string_vm_test.cpp
 * @brief VQL string conversion through the native Vortex VM opcode.
 */
#include <gtest/gtest.h>

#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/scalar.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/mesh_evaluator.hpp"
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

struct RunResult {
  bool compiled{false};
  bool executed{false};
  std::string error;
  std::vector<CellRef> values;
  std::size_t stringOps{0};
};

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_string_vm_" +
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
    for (const CellRef opcode : compiled.generatedOpcodes)
      result.stringOps += vm.getOpcodeKind(opcode) == OpcodeKind::VqlString;
    if (!compiled.cursor) {
      result.error = "compiled VQL has no cursor";
      return result;
    }
    const auto execution = vm.run(*compiled.cursor, 10000);
    result.executed      = execution.success;
    result.error         = execution.errorMessage;
    vm.setCursorOpcode(*compiled.cursor, std::nullopt);
    if (!execution.success) return result;
    for (const CellRef slot : compiled.resultSlots) {
      const auto values = vm.resultCells(slot);
      result.values.insert(result.values.end(), values.begin(), values.end());
    }
    return result;
  }

  CellRef typedText(const std::string_view display, const ValueKind kind,
                    const std::uint64_t bits) {
    const CellRef cell = arena.makeCell(display);
    zigzag::expectWritten(arena.setValueBits(cell, kind, bits));
    return cell;
  }
};

struct ForeignFixture {
  std::shared_ptr<UserPermascroll> scroll = std::make_shared<UserPermascroll>();
  std::shared_ptr<Store> store            = std::make_shared<Store>(scroll);
  MicroversionId version                  = store->sliceGenesis({});
  CellRef cloneDimension{};

  ForeignFixture() {
    const auto clone = store->makeDimension(version, "d.clone");
    version          = clone.version;
    cloneDimension   = clone.dim;
  }

  CellRef add(const std::string_view display, const ValueKind kind,
              const std::uint64_t bits) {
    version             = store->makeCell(version, display);
    const CellRef value = store->cellRefOf(version).value();
    version =
        store->setValue(version, value, scroll->append(display), kind, bits);
    return value;
  }

  CellRef addClone(const CellRef source, const std::string_view display,
                   const ValueKind kind, const std::uint64_t bits) {
    const CellRef clone = add(display, kind, bits);
    version =
        store->setLink(version, source, cloneDimension, DimVector::POS, clone);
    return clone;
  }
};

std::size_t countOpcode(const zigzag::vortex::VortexVM &vm,
                        const CompilationResult &compiled,
                        const OpcodeKind kind) {
  std::size_t result = 0;
  for (const CellRef opcode : compiled.generatedOpcodes)
    result += vm.getOpcodeKind(opcode) == kind;
  return result;
}

TEST(VQLStringVMTest, ConvertsIntegerDoubleBoolAndTextWithoutMutatingInputs) {
  Harness h;
  const auto assertString = [&](const CellRef input,
                                const std::string_view expected) {
    const auto originalKind = h.arena.valueKindOf(input);
    const auto originalBits = h.arena.slot(input)->valueBits;
    const auto originalText = h.arena.textOf(input);
    const auto result       = h.run("string($value)", {{"value", {input}}});
    ASSERT_TRUE(result.compiled) << result.error;
    ASSERT_TRUE(result.executed) << result.error;
    EXPECT_GT(result.stringOps, 0u);
    ASSERT_EQ(result.values.size(), 1u);
    EXPECT_EQ(h.arena.textOf(result.values.front()), expected);
    EXPECT_EQ(h.arena.valueKindOf(result.values.front()), ValueKind::None);
    EXPECT_NE(result.values.front(), input);
    EXPECT_EQ(h.arena.valueKindOf(input), originalKind);
    EXPECT_EQ(h.arena.slot(input)->valueBits, originalBits);
    EXPECT_EQ(h.arena.textOf(input), originalText);
  };

  for (const std::int64_t value : {std::numeric_limits<std::int64_t>::min(),
                                   std::numeric_limits<std::int64_t>::max()}) {
    const auto input =
        h.typedText("misleading integer display", ValueKind::Int64,
                    std::bit_cast<std::uint64_t>(value));
    assertString(input, std::to_string(value));
  }

  for (const double value :
       {1.0, -0.0, 42.25, std::numeric_limits<double>::denorm_min(),
        std::numeric_limits<double>::max()}) {
    const auto scalar = xanadu::scalarValue(value);
    const auto input  = h.typedText("misleading floating display",
                                    ValueKind::Double, scalar.bits);
    assertString(input,
                 xanadu::scalarValue(std::bit_cast<double>(scalar.bits)).text);
  }

  for (const bool value : {false, true}) {
    const auto input =
        h.typedText("not-a-boolean", ValueKind::Bool, value ? 1U : 0U);
    assertString(input, value ? "true" : "false");
  }

  const auto text = h.arena.makeCell("0010 remains text");
  assertString(text, "0010 remains text");
}

TEST(VQLStringVMTest, CloneAndForeignTypedValuesUseTheirOwnBits) {
  Harness h;
  const auto local = h.typedText("local display is not 91", ValueKind::Int64,
                                 std::bit_cast<std::uint64_t>(INT64_C(91)));
  const auto clone = h.typedText("clone display is not 91", ValueKind::Int64,
                                 std::bit_cast<std::uint64_t>(INT64_C(91)));
  const auto cloneDimension = h.coordinator.resolveDimension("d.clone");
  zigzag::expectWritten(
      h.arena.link(local, cloneDimension, DimVector::POS, clone));
  for (const CellRef input : {local, clone}) {
    const auto result = h.run("string($value)", {{"value", {input}}});
    ASSERT_TRUE(result.compiled) << result.error;
    ASSERT_TRUE(result.executed) << result.error;
    ASSERT_EQ(result.values.size(), 1u);
    EXPECT_EQ(h.arena.textOf(result.values.front()), "91");
    EXPECT_NE(result.values.front(), input);
  }

  ForeignFixture foreign;
  const CellRef foreignValue =
      foreign.add("foreign display is not 1", ValueKind::Double,
                  xanadu::scalarValue(1.0).bits);
  const CellRef foreignClone =
      foreign.addClone(foreignValue, "foreign clone display is not 1",
                       ValueKind::Double, xanadu::scalarValue(1.0).bits);
  h.coordinator.addStore("foreign", "primary", foreign.store, foreign.version);
  const CellRef foreignHome =
      h.coordinator.resolveNamedStore("foreign").value();
  const auto space = h.arena.foreignOf(foreignHome)->space;
  for (const CellRef input : {foreignValue, foreignClone}) {
    const CellRef proxy = h.arena.proxyFor(space, input);
    const auto result   = h.run("string($value)", {{"value", {proxy}}});
    ASSERT_TRUE(result.compiled) << result.error;
    ASSERT_TRUE(result.executed) << result.error;
    ASSERT_EQ(result.values.size(), 1u);
    EXPECT_EQ(h.arena.textOf(result.values.front()), "1");
    EXPECT_NE(result.values.front(), proxy);
  }
}

TEST(VQLStringVMTest, NestedFramesDefaultsAndCallerContextUseNativeOpcode) {
  Harness h;
  const std::string nested =
      "def pure inner($value: Int): String := string($value); "
      "def pure outer($value: Int := 7): String := inner($value); outer()";
  const auto result = h.run(nested);
  ASSERT_TRUE(result.compiled) << result.error;
  ASSERT_TRUE(result.executed) << result.error;
  EXPECT_GT(result.stringOps, 0u);
  ASSERT_EQ(result.values.size(), 1u);
  EXPECT_EQ(h.arena.textOf(result.values.front()), "7");

  const auto context =
      h.run("def pure as_text(. : Int): String := string(.); as_text(9)");
  ASSERT_TRUE(context.compiled) << context.error;
  ASSERT_TRUE(context.executed) << context.error;
  ASSERT_EQ(context.values.size(), 1u);
  EXPECT_EQ(h.arena.textOf(context.values.front()), "9");

  const auto nestedBuiltin = h.run("string(string(42))");
  ASSERT_TRUE(nestedBuiltin.compiled) << nestedBuiltin.error;
  ASSERT_TRUE(nestedBuiltin.executed) << nestedBuiltin.error;
  EXPECT_GT(nestedBuiltin.stringOps, 0u);
  ASSERT_EQ(nestedBuiltin.values.size(), 1u);
  EXPECT_EQ(h.arena.textOf(nestedBuiltin.values.front()), "42");
}

TEST(VQLStringVMTest, UserDefinitionNamedStringTakesPrecedence) {
  Harness h;
  const auto result =
      h.run("def pure string($value: Int): String := \"user definition\"; "
            "string(42)");
  ASSERT_TRUE(result.compiled) << result.error;
  ASSERT_TRUE(result.executed) << result.error;
  EXPECT_EQ(result.stringOps, 0u);
  ASSERT_EQ(result.values.size(), 1u);
  EXPECT_EQ(h.arena.textOf(result.values.front()), "user definition");
}

TEST(VQLStringVMTest, ParameterDefaultCanCallStringButFieldDefaultRefuses) {
  Harness h;
  const auto parameterDefault =
      h.run("def pure choose($value: String := string(7)): String := $value; "
            "choose()");
  ASSERT_TRUE(parameterDefault.compiled) << parameterDefault.error;
  ASSERT_TRUE(parameterDefault.executed) << parameterDefault.error;
  EXPECT_GT(parameterDefault.stringOps, 0u);
  ASSERT_EQ(parameterDefault.values.size(), 1u);
  EXPECT_EQ(h.arena.textOf(parameterDefault.values.front()), "7");

  const auto fieldDefault =
      h.run("def pure choose($fallback: Int, $item: Cell { +d.name: "
            "$name: String := string($fallback) }): String := $name; "
            "choose(7, ##)");
  EXPECT_FALSE(fieldDefault.compiled);
  EXPECT_NE(fieldDefault.error.find("default expression"), std::string::npos);
}

TEST(VQLStringVMTest, EmptyMultiAndStructuralInputsRefuseWithoutTruncation) {
  Harness h;
  const auto empty = h.run("string($values)", {{"values", {}}});
  ASSERT_TRUE(empty.compiled) << empty.error;
  EXPECT_FALSE(empty.executed);

  const CellRef first  = h.arena.makeScalarCell(std::int64_t{1});
  const CellRef second = h.arena.makeScalarCell(std::int64_t{2});
  const auto multiple = h.run("string($values)", {{"values", {first, second}}});
  ASSERT_TRUE(multiple.compiled) << multiple.error;
  EXPECT_FALSE(multiple.executed);
  EXPECT_TRUE(multiple.values.empty());

  const CellRef operation  = h.vm.mintOpcode(OpcodeKind::Nop);
  const CellRef structural = h.arena.makeCell();
  zigzag::expectWritten(
      h.arena.setValueBits(structural, ValueKind::OpHandle, operation));
  EXPECT_EQ(h.arena.handleTarget(structural), operation);
  const auto handle = h.run("string($value)", {{"value", {structural}}});
  ASSERT_TRUE(handle.compiled) << handle.error;
  EXPECT_FALSE(handle.executed);
  EXPECT_NE(handle.error.find("structural handles"), std::string::npos);
  EXPECT_TRUE(handle.values.empty());

  const auto noArgument = h.run("string()");
  EXPECT_FALSE(noArgument.compiled);
  const auto twoArguments = h.run("string(1, 2)");
  EXPECT_FALSE(twoArguments.compiled);
}

TEST(VQLStringVMTest, MalformedNativeOpcodeDoesNotWriteAnyOutput) {
  Harness h;
  const CellRef opcode = h.vm.mintOpcode(OpcodeKind::VqlString);
  h.core.bindInput(opcode, h.arena.makeCell("unchanged input"));
  const CellRef firstOutput  = h.arena.makeCell("first sentinel");
  const CellRef secondOutput = h.arena.makeCell("second sentinel");
  h.core.bindOutput(opcode, firstOutput);
  h.core.bindOutput(opcode, secondOutput);
  const auto cursor = h.vm.spawnCursor(opcode, "malformed-vql-string");
  const auto result = h.vm.step(cursor);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(h.arena.textOf(firstOutput), "first sentinel");
  EXPECT_EQ(h.arena.textOf(secondOutput), "second sentinel");
}

TEST(VQLStringVMTest, NativeOpcodeRecoversAfterWorkBudgetExhaustion) {
  Harness h;
  const CellRef opcode = h.vm.mintOpcode(OpcodeKind::VqlString);
  h.core.bindInput(opcode, h.arena.makeCell("42"));
  const CellRef output = h.arena.makeCell("sentinel");
  h.core.bindOutput(opcode, output);
  const CellRef cursor = h.vm.spawnCursor(opcode, "vql-string-budget");

  h.arena.setExecutionLimits(1000, 0, 1000);
  const auto refused = h.vm.step(cursor);
  EXPECT_FALSE(refused.success);
  EXPECT_EQ(h.arena.textOf(output), "sentinel");

  h.arena.setExecutionLimits(1000, 1000, 1000);
  const auto retried = h.vm.step(cursor);
  ASSERT_TRUE(retried.success) << retried.errorMessage;
  EXPECT_EQ(h.arena.textOf(output), "42");
}

TEST(VQLStringVMTest, NativeStringOpcodeSurvivesAstDestructionAndReload) {
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
      Parser parser("def pure render($value: Float): String := "
                    "string($value); render(1.0)");
      const auto ast = parser.parseQuery();
      compiled       = compiler.compile(ast);
    }
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    EXPECT_GT(countOpcode(sourceVm, compiled, OpcodeKind::VqlString), 0u);
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
  MultiStoreCoordinator coordinator(core);
  zigzag::vortex::VortexVM vm(core);
  const auto entry =
      arena.linked(core.home(), core.dims().spin, DimVector::POS);
  ASSERT_TRUE(entry);
  const auto cursor    = vm.spawnCursor(*entry, "vql-string-reload");
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
  const auto values = vm.resultCells(resultPorts.front());
  ASSERT_EQ(values.size(), 1u);
  EXPECT_EQ(arena.textOf(values.front()), "1");
}

TEST(VQLStringVMTest, SealedScalarOpcodeExecutesWithoutStreamMetadata) {
  namespace mesh = zigzag::vortex::mesh;
  mesh::VortexProgram program{
      .cells        = {{},
                       {},
                       {zigzag::vortex::CellValue{
                     std::numeric_limits<std::int64_t>::max()},
                        {}},
                       {}},
      .instructions = {{0, OpcodeKind::VqlString, {2}, {3}, 1},
                       {1, OpcodeKind::Halt, {}, {}, {}}},
      .entry        = 0,
      .outputs      = {{3}}};
  const auto sealed = mesh::sealVortexProgram(program);
  ASSERT_TRUE(sealed) << sealed.error();
  const auto result = mesh::evaluate(mesh::Frontend::Vortex, *sealed,
                                     {.maxCycles      = 10000,
                                      .maxCells       = 10000,
                                      .maxSolutions   = 16,
                                      .maxOutputBytes = 1024 * 1024});
  ASSERT_TRUE(result) << result.error();
  auto &arena = result->context->arena;
  std::optional<CellRef> items;
  std::optional<CellRef> value;
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
  EXPECT_EQ(arena.textOf(*output), "9223372036854775807");
}

TEST(VQLStringVMTest, HostAndMeshVqueryRoutesExecuteStringGraph) {
  constexpr std::string_view source =
      "def pure render($value: Int): String := string($value); render(42)";
  zigzag::vortex::VortexHost host;
  const auto hostResult = host.executeVQLChecked(source);
  ASSERT_TRUE(hostResult) << hostResult.error().message;
  ASSERT_EQ(hostResult->size(), 1u);
  EXPECT_EQ(host.core().arena().textOf(hostResult->front()), "42");

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
  EXPECT_EQ(arena.textOf(*output), "42");
}

} // namespace
} // namespace xanadu::vql

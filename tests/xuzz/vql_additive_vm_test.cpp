/**
 * @file vql_additive_vm_test.cpp
 * @brief Additive VQL values through the Vortex VM.
 */
#include <gtest/gtest.h>

#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
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
#include "common/xanadu/vql/frame_eligibility.hpp"
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
  std::size_t addOps{0};
  std::size_t boxOps{0};
  std::size_t frameOps{0};
};

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_additive_vm_" +
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
    for (const CellRef opcode : compiled.generatedOpcodes) {
      const auto kind = vm.getOpcodeKind(opcode);
      result.addOps += kind == OpcodeKind::VqlAdd;
      result.boxOps += kind == OpcodeKind::VqlBoxScalar;
      result.frameOps += kind == OpcodeKind::InvokeFrame;
    }
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
    version            = store->makeCell(version, display);
    const CellRef cell = store->cellRefOf(version).value();
    version =
        store->setValue(version, cell, scroll->append(display), kind, bits);
    return cell;
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

TEST(VQLAdditiveVMTest, StringOperandsConcatenateWithoutParsingNumbers) {
  Harness h;
  const auto result =
      h.run("def pure join(): String := (\"007\" + \" \" ) + \"items\"; "
            "join()");
  ASSERT_TRUE(result.compiled) << result.error;
  ASSERT_TRUE(result.executed) << result.error;
  EXPECT_GT(result.addOps, 0u);
  ASSERT_EQ(result.values.size(), 1u);
  EXPECT_EQ(h.arena.textOf(result.values.front()), "007 items");
  EXPECT_EQ(h.arena.valueKindOf(result.values.front()), ValueKind::None);

  const auto numbers = h.run(
      "def pure join_numbers(): String := \"12\" + \"3\"; join_numbers()");
  ASSERT_TRUE(numbers.compiled) << numbers.error;
  ASSERT_TRUE(numbers.executed) << numbers.error;
  ASSERT_EQ(numbers.values.size(), 1u);
  EXPECT_EQ(h.arena.textOf(numbers.values.front()), "123");
  EXPECT_EQ(h.arena.valueKindOf(numbers.values.front()), ValueKind::None);
}

TEST(VQLAdditiveVMTest, CheckedIntegerAndFloatingArithmetic) {
  Harness h;
  const auto maximum =
      h.run("def pure max_sum(): Int := 9223372036854775806 + 1; max_sum()");
  ASSERT_TRUE(maximum.compiled) << maximum.error;
  ASSERT_TRUE(maximum.executed) << maximum.error;
  ASSERT_EQ(maximum.values.size(), 1u);
  EXPECT_EQ(h.arena.valueKindOf(maximum.values.front()), ValueKind::Int64);
  EXPECT_EQ(h.arena.asInt64(maximum.values.front()), INT64_MAX);

  const auto minimum =
      h.run("def pure min_sum(): Int := -9223372036854775807 + -1; min_sum()");
  ASSERT_TRUE(minimum.compiled) << minimum.error;
  ASSERT_TRUE(minimum.executed) << minimum.error;
  ASSERT_EQ(minimum.values.size(), 1u);
  EXPECT_EQ(h.arena.asInt64(minimum.values.front()), INT64_MIN);

  const auto floating =
      h.run("def pure float_sum(): Float := 1.25 + 2.5; float_sum()");
  ASSERT_TRUE(floating.compiled) << floating.error;
  ASSERT_TRUE(floating.executed) << floating.error;
  ASSERT_EQ(floating.values.size(), 1u);
  EXPECT_EQ(h.arena.valueKindOf(floating.values.front()), ValueKind::Double);
  EXPECT_EQ(h.arena.asDouble(floating.values.front()), 3.75);

  const auto exactMixed =
      h.run("def pure mixed_sum(): Float := 4 + 0.5; mixed_sum()");
  ASSERT_TRUE(exactMixed.compiled) << exactMixed.error;
  ASSERT_TRUE(exactMixed.executed) << exactMixed.error;
  ASSERT_EQ(exactMixed.values.size(), 1u);
  EXPECT_EQ(h.arena.asDouble(exactMixed.values.front()), 4.5);

  const auto overflow =
      h.run("def pure overflow(): Int := 9223372036854775807 + 1; overflow()");
  ASSERT_TRUE(overflow.compiled) << overflow.error;
  EXPECT_FALSE(overflow.executed);

  const auto inexact =
      h.run("def pure inexact(): Float := 9007199254740993 + 1.0; inexact()");
  ASSERT_TRUE(inexact.compiled) << inexact.error;
  EXPECT_FALSE(inexact.executed);
  EXPECT_NE(inexact.error.find("exact"), std::string::npos);

  const auto reversedInexact =
      h.run("def pure inexact(): Float := 1.0 + 9007199254740993; inexact()");
  ASSERT_TRUE(reversedInexact.compiled) << reversedInexact.error;
  EXPECT_FALSE(reversedInexact.executed);

  const auto exactLarge =
      h.run("def pure mixed($left: Any, $right: Any): Any := $left + $right; "
            "mixed($left, $right)",
            {{"left", {h.arena.makeScalarCell(INT64_C(9007199254740992))}},
             {"right",
              {h.typedText("display 1.0", ValueKind::Double,
                           xanadu::scalarValue(1.0).bits)}}});
  ASSERT_TRUE(exactLarge.compiled) << exactLarge.error;
  ASSERT_TRUE(exactLarge.executed) << exactLarge.error;
  ASSERT_EQ(exactLarge.values.size(), 1u);
  EXPECT_EQ(h.arena.valueKindOf(exactLarge.values.front()), ValueKind::Double);

  const auto minimumMixed =
      h.run("def pure mixed($left: Any, $right: Any): Any := $left + $right; "
            "mixed($left, $right)",
            {{"left", {h.arena.makeScalarCell(INT64_MIN)}},
             {"right",
              {h.typedText("display zero", ValueKind::Double,
                           xanadu::scalarValue(0.0).bits)}}});
  ASSERT_TRUE(minimumMixed.compiled) << minimumMixed.error;
  ASSERT_TRUE(minimumMixed.executed) << minimumMixed.error;
  ASSERT_EQ(minimumMixed.values.size(), 1u);
  EXPECT_EQ(h.arena.valueKindOf(minimumMixed.values.front()),
            ValueKind::Double);
  EXPECT_EQ(h.arena.asDouble(minimumMixed.values.front()),
            static_cast<double>(INT64_MIN));
}

TEST(VQLAdditiveVMTest, RefusesUnsupportedKindsAndMixedNumericText) {
  Harness h;
  for (const std::string_view source :
       {"def pure invalid(): Any := true + false; invalid()",
        "def pure invalid(): Any := \"x\" + 1; invalid()",
        "def pure invalid(): Any := 1 + true; invalid()"}) {
    const auto result = h.run(source);
    ASSERT_TRUE(result.compiled) << source << ": " << result.error;
    EXPECT_FALSE(result.executed) << source;
  }

  const CellRef operation  = h.vm.mintOpcode(OpcodeKind::Nop);
  const CellRef structural = h.arena.makeCell();
  zigzag::expectWritten(
      h.arena.setValueBits(structural, ValueKind::OpHandle, operation));
  const auto handle =
      h.run("def pure add_left($left: Any): Any := $left + 1; add_left($left)",
            {{"left", {structural}}});
  ASSERT_TRUE(handle.compiled) << handle.error;
  EXPECT_FALSE(handle.executed);
}

TEST(VQLAdditiveVMTest, TypedLocalCloneAndForeignInputsUseTheirBits) {
  Harness h;
  const CellRef local = h.typedText("display is not 41", ValueKind::Int64,
                                    std::bit_cast<std::uint64_t>(INT64_C(41)));
  const CellRef clone = h.typedText("clone display is not 41", ValueKind::Int64,
                                    std::bit_cast<std::uint64_t>(INT64_C(41)));
  const auto cloneDimension = h.coordinator.resolveDimension("d.clone");
  zigzag::expectWritten(
      h.arena.link(local, cloneDimension, DimVector::POS, clone));
  for (const CellRef input : {local, clone}) {
    const auto result = h.run(
        "def pure add_left($left: Int): Int := $left + 1; add_left($left)",
        {{"left", {input}}});
    ASSERT_TRUE(result.compiled) << result.error;
    ASSERT_TRUE(result.executed) << result.error;
    ASSERT_EQ(result.values.size(), 1u);
    EXPECT_EQ(h.arena.asInt64(result.values.front()), 42);
    EXPECT_EQ(h.arena.textOf(input),
              input == local ? "display is not 41" : "clone display is not 41");
  }

  ForeignFixture foreign;
  const CellRef foreignValue =
      foreign.add("foreign display is not 41", ValueKind::Int64,
                  std::bit_cast<std::uint64_t>(INT64_C(41)));
  const CellRef foreignClone = foreign.addClone(
      foreignValue, "foreign clone display is not 41", ValueKind::Int64,
      std::bit_cast<std::uint64_t>(INT64_C(41)));
  h.coordinator.addStore("foreign", "primary", foreign.store, foreign.version);
  const CellRef foreignHome =
      h.coordinator.resolveNamedStore("foreign").value();
  const auto space = h.arena.foreignOf(foreignHome)->space;
  for (const CellRef cell : {foreignValue, foreignClone}) {
    const CellRef proxy = h.arena.proxyFor(space, cell);
    const auto result   = h.run(
        "def pure add_left($left: Int): Int := $left + 1; add_left($left)",
        {{"left", {proxy}}});
    ASSERT_TRUE(result.compiled) << result.error;
    ASSERT_TRUE(result.executed) << result.error;
    ASSERT_EQ(result.values.size(), 1u);
    EXPECT_EQ(h.arena.asInt64(result.values.front()), 42);
  }
}

TEST(VQLAdditiveVMTest, FullStreamCardinalityIsRequiredForBothOperands) {
  Harness h;
  const CellRef one   = h.arena.makeScalarCell(std::int64_t{1});
  const CellRef two   = h.arena.makeScalarCell(std::int64_t{2});
  const CellRef three = h.arena.makeScalarCell(std::int64_t{3});
  constexpr std::string_view addSource =
      "def pure add($left: Stream<Any>, $right: Stream<Any>): Any := $left + "
      "$right; "
      "add($left, $right)";
  const auto empty = h.run(addSource, {{"left", {}}, {"right", {one}}});
  ASSERT_TRUE(empty.compiled) << empty.error;
  EXPECT_FALSE(empty.executed);

  const auto multiple =
      h.run(addSource, {{"left", {one, two}}, {"right", {three}}});
  ASSERT_TRUE(multiple.compiled) << multiple.error;
  EXPECT_FALSE(multiple.executed);
  EXPECT_TRUE(multiple.values.empty());

  const auto secondEmpty = h.run(addSource, {{"left", {one}}, {"right", {}}});
  ASSERT_TRUE(secondEmpty.compiled) << secondEmpty.error;
  EXPECT_FALSE(secondEmpty.executed);
}

TEST(VQLAdditiveVMTest, FramesCapturesDefaultsAndRealQueryBodiesUseNativeAdd) {
  Harness h;
  const CellRef document   = h.arena.makeCell("document");
  const CellRef titleValue = h.arena.makeCell("A text adventure");
  zigzag::expectWritten(h.arena.link(document,
                                     h.coordinator.resolveDimension("d.title"),
                                     DimVector::POS, titleValue));
  const auto title = h.run(
      "def pure format_title({., +d.title: $t, +d.tag: $tag := \"note\"}): "
      "String := \"[\" + $tag + \"] \" + $t; format_title($document)",
      {{"document", {document}}});
  ASSERT_TRUE(title.compiled) << title.error;
  ASSERT_TRUE(title.executed) << title.error;
  EXPECT_GT(title.addOps, 0u);
  EXPECT_GT(title.frameOps, 0u);
  ASSERT_EQ(title.values.size(), 1u);
  EXPECT_EQ(h.arena.textOf(title.values.front()), "[note] A text adventure");

  const CellRef videoCell  = h.arena.makeCell("video");
  const CellRef codecValue = h.arena.makeCell("h264");
  zigzag::expectWritten(h.arena.link(videoCell,
                                     h.coordinator.resolveDimension("d.codec"),
                                     DimVector::POS, codecValue));
  const auto video = h.run(
      "def pure process_video({., +d.codec: $c: String, +d.fps: $fps: Int "
      ":= 60}): String := $c + \" @ \" + string($fps) + \"fps\"; "
      "process_video($video)",
      {{"video", {videoCell}}});
  ASSERT_TRUE(video.compiled) << video.error;
  ASSERT_TRUE(video.executed) << video.error;
  EXPECT_GT(video.addOps, 0u);
  EXPECT_GT(video.frameOps, 0u);
  ASSERT_EQ(video.values.size(), 1u);
  EXPECT_EQ(h.arena.textOf(video.values.front()), "h264 @ 60fps");

  const CellRef outside = h.arena.makeScalarCell(std::int64_t{9});
  const auto capture =
      h.run("def pure add_capture($value: Int): Int := $value + $outside; "
            "add_capture(3)",
            {{"outside", {outside}}});
  ASSERT_TRUE(capture.compiled) << capture.error;
  ASSERT_TRUE(capture.executed) << capture.error;
  EXPECT_GT(capture.frameOps, 0u);
  ASSERT_EQ(capture.values.size(), 1u);
  EXPECT_EQ(h.arena.asInt64(capture.values.front()), 12);

  const auto parameterDefault = h.run(
      "def pure choose($value: String := \"left\" + \"right\"): String := "
      "$value; choose()");
  ASSERT_TRUE(parameterDefault.compiled) << parameterDefault.error;
  ASSERT_TRUE(parameterDefault.executed) << parameterDefault.error;
  ASSERT_EQ(parameterDefault.values.size(), 1u);
  EXPECT_EQ(h.arena.textOf(parameterDefault.values.front()), "leftright");

  const auto fieldDefault = h.run(
      "def pure inspect($item: Cell { +d.sum: $sum: Int := 1 + 2 }): Int := "
      "$sum; inspect(##)");
  EXPECT_FALSE(fieldDefault.compiled);
  EXPECT_NE(fieldDefault.error.find("default expression"), std::string::npos);
}

TEST(VQLAdditiveVMTest, LoopAddResultsAreBoxedPerIterationAndRemainStable) {
  Harness h;
  const auto items     = h.coordinator.resolveDimension("d.items");
  const CellRef first  = h.arena.makeCell("A");
  const CellRef second = h.arena.makeCell("B");
  zigzag::expectWritten(
      h.arena.link(h.core.home(), items, DimVector::POS, first));
  zigzag::expectWritten(h.arena.link(first, items, DimVector::POS, second));

  const auto direct = h.run(
      "for $item in ##/d.items let $label := $item + \"!\" return $label");
  ASSERT_TRUE(direct.compiled) << direct.error;
  ASSERT_TRUE(direct.executed) << direct.error;
  EXPECT_GT(direct.addOps, 0u);
  EXPECT_GT(direct.boxOps, 0u);
  ASSERT_EQ(direct.values.size(), 2u);
  EXPECT_EQ(h.arena.textOf(direct.values[0]), "A!");
  EXPECT_EQ(h.arena.textOf(direct.values[1]), "B!");
  EXPECT_NE(direct.values[0], direct.values[1]);

  const auto framed =
      h.run("def pure decorate($value: String := \"fallback\"): String := "
            "$value + \"?\"; for $item in ##/d.items return decorate($item)");
  ASSERT_TRUE(framed.compiled) << framed.error;
  ASSERT_TRUE(framed.executed) << framed.error;
  EXPECT_GT(framed.frameOps, 0u);
  ASSERT_EQ(framed.values.size(), 2u);
  EXPECT_EQ(h.arena.textOf(framed.values[0]), "A?");
  EXPECT_EQ(h.arena.textOf(framed.values[1]), "B?");
  EXPECT_NE(framed.values[0], framed.values[1]);

  const auto later = h.run(
      "for $item in ##/d.items let $label := $item + \"!\" return $label");
  ASSERT_TRUE(later.compiled) << later.error;
  ASSERT_TRUE(later.executed) << later.error;
  ASSERT_EQ(later.values.size(), 2u);
  EXPECT_NE(direct.values[0], later.values[0]);
  EXPECT_NE(direct.values[1], later.values[1]);
  EXPECT_EQ(h.arena.textOf(direct.values[0]), "A!");
  EXPECT_EQ(h.arena.textOf(direct.values[1]), "B!");
  EXPECT_EQ(h.arena.textOf(framed.values[0]), "A?");
  EXPECT_EQ(h.arena.textOf(framed.values[1]), "B?");
}

TEST(VQLAdditiveVMTest,
     ParenthesesComparisonAndFunctionArgumentsKeepPrecedence) {
  Harness h;
  const auto arithmetic =
      h.run("def pure show($value: Int): String := string($value); "
            "show((1 + 2) + 3)");
  ASSERT_TRUE(arithmetic.compiled) << arithmetic.error;
  ASSERT_TRUE(arithmetic.executed) << arithmetic.error;
  ASSERT_EQ(arithmetic.values.size(), 1u);
  EXPECT_EQ(h.arena.textOf(arithmetic.values.front()), "6");

  const auto comparison =
      h.run("def pure equals_three(): Bool := 1 + 2 = 3; equals_three()");
  ASSERT_TRUE(comparison.compiled) << comparison.error;
  ASSERT_TRUE(comparison.executed) << comparison.error;
  ASSERT_EQ(comparison.values.size(), 1u);
  EXPECT_EQ(h.arena.asBool(comparison.values.front()), true);

  const CellRef value = h.arena.makeCell("path value");
  const auto foo      = h.coordinator.resolveDimension("d.foo");
  zigzag::expectWritten(
      h.arena.link(h.core.home(), foo, DimVector::POS, value));
  const auto path = h.run("##/+d.foo");
  ASSERT_TRUE(path.compiled) << path.error;
  ASSERT_TRUE(path.executed) << path.error;
  EXPECT_EQ(path.values, (std::vector<CellRef>{value}));
  EXPECT_EQ(path.addOps, 0u);
}

TEST(VQLAdditiveVMTest, BinaryChildCyclesAreRejectedByEligibilityAnalysis) {
  Parser parser("def pure cycle($value: Int): Int := $value + 1; cycle(1)");
  auto query = parser.parseQuery();
  ASSERT_EQ(query.defs.size(), 1u);
  auto *body =
      std::get_if<std::shared_ptr<ValueExpr>>(&query.defs.front().body);
  ASSERT_NE(body, nullptr);
  auto *binary = std::get_if<std::shared_ptr<BinaryValueExpr>>(&(*body)->kind);
  ASSERT_NE(binary, nullptr);
  ASSERT_TRUE(*binary);
  (*binary)->left = *body;

  const auto analysis = analyzeFrameEligibility(query);
  ASSERT_FALSE(analysis);
  EXPECT_NE(analysis.error().message.find("cyclic"), std::string::npos);

  (*binary)->left.reset();
}

TEST(VQLAdditiveVMTest, BinaryExpressionGraphSurvivesAstDestructionAndReload) {
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
      Parser parser("def pure add($left: Int, $right: Int): Int := "
                    "$left + $right; add(40, 2)");
      const auto ast = parser.parseQuery();
      compiled       = compiler.compile(ast);
    }
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    EXPECT_GT(countOpcode(sourceVm, compiled, OpcodeKind::VqlAdd), 0u);
    EXPECT_GT(countOpcode(sourceVm, compiled, OpcodeKind::VqlBoxScalar), 0u);
    EXPECT_GT(countOpcode(sourceVm, compiled, OpcodeKind::InvokeFrame), 0u);
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
  const auto execution = vm.run(vm.spawnCursor(*entry, "vql-add-reload"), 1000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  auto current = *entry;
  auto next    = arena.linked(current, core.dims().spin, DimVector::POS);
  while (next && vm.getOpcodeKind(*next) != OpcodeKind::Halt) {
    current = *next;
    next    = arena.linked(current, core.dims().spin, DimVector::POS);
  }
  ASSERT_TRUE(next);
  const auto resultPorts = core.inputsOf(*next);
  ASSERT_EQ(resultPorts.size(), 1u);
  const auto values = vm.resultCells(resultPorts.front());
  ASSERT_EQ(values.size(), 1u);
  EXPECT_EQ(arena.asInt64(values.front()), 42);
  EXPECT_EQ(arena.valueKindOf(values.front()), ValueKind::Int64);
}

TEST(VQLAdditiveVMTest, HostMeshAndSealedVortexRoutesExecuteNativeAdd) {
  constexpr std::string_view source =
      "def pure add($left: Int, $right: Int): Int := $left + $right; "
      "add(40, 2)";
  zigzag::vortex::VortexHost host;
  const auto hostResult = host.executeVQLChecked(source);
  ASSERT_TRUE(hostResult) << hostResult.error().message;
  ASSERT_EQ(hostResult->size(), 1u);
  EXPECT_EQ(host.core().arena().asInt64(hostResult->front()), 42);

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
  EXPECT_EQ(arena.asInt64(*output), 42);

  namespace mesh = zigzag::vortex::mesh;
  mesh::VortexProgram program{
      .cells        = {{},
                       {},
                       {zigzag::vortex::CellValue{INT64_MAX - 1}, {}},
                       {zigzag::vortex::CellValue{std::int64_t{1}}, {}},
                       {}},
      .instructions = {{0, OpcodeKind::VqlAdd, {2, 3}, {4}, 1},
                       {1, OpcodeKind::Halt, {}, {}, {}}},
      .entry        = 0,
      .outputs      = {{4}}};
  const auto sealed = mesh::sealVortexProgram(program);
  ASSERT_TRUE(sealed) << sealed.error();
  const auto artifact = mesh::evaluate(mesh::Frontend::Vortex, *sealed,
                                       {.maxCycles      = 10000,
                                        .maxCells       = 10000,
                                        .maxSolutions   = 16,
                                        .maxOutputBytes = 1024 * 1024});
  ASSERT_TRUE(artifact) << artifact.error();
  auto &artifactArena = artifact->context->arena;
  std::optional<CellRef> artifactItems;
  std::optional<CellRef> artifactValue;
  for (const auto &cell : artifactArena.cells()) {
    if (artifactArena.textOf(cell.birthOp) == "d.mesh.items")
      artifactItems = cell.birthOp;
    if (artifactArena.textOf(cell.birthOp) == "d.mesh.value")
      artifactValue = cell.birthOp;
  }
  ASSERT_TRUE(artifactItems);
  ASSERT_TRUE(artifactValue);
  const auto artifactRecord =
      artifactArena.linked(artifact->root, *artifactItems, DimVector::POS);
  ASSERT_TRUE(artifactRecord);
  const auto artifactOutput =
      artifactArena.linked(*artifactRecord, *artifactValue, DimVector::POS);
  ASSERT_TRUE(artifactOutput);
  EXPECT_EQ(artifactArena.asInt64(*artifactOutput), INT64_MAX);
}

} // namespace
} // namespace xanadu::vql

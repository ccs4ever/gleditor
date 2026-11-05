/**
 * @file vql_contract_validator_frames_test.cpp
 * @brief Computed return contracts through capture-free Vortex frames.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <expected>
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
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::FramePortKind;
using zigzag::vortex::OpcodeKind;

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_contract_validator_frames_" +
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

  void link(const CellRef owner, const std::string_view name,
            const CellRef value, const DimVector direction = DimVector::POS) {
    const auto dimension = coordinator.resolveDimension(name);
    zigzag::expectWritten(arena.link(owner, dimension, direction, value));
  }

  CompilationResult compile(const std::string_view source,
                            ContractBindings bindings  = {},
                            CompilationOptions options = {}) {
    options.initialBindings = std::move(bindings);
    VQLCompiler compiler(coordinator, vm);
    return compiler.compile(source, options);
  }

  std::expected<std::vector<CellRef>, QueryExecutionError>
  run(const std::string_view source, ContractBindings bindings = {},
      const std::vector<CellRef> &context = {}) {
    QueryExecutor executor(coordinator, vm, {}, std::move(bindings));
    return executor.execute(source, context);
  }
};

std::optional<CellRef> validatorInvoke(const Harness &h,
                                       const CompilationResult &program) {
  for (const CellRef opcode : program.generatedOpcodes) {
    if (h.arena.textOf(opcode).starts_with("#INVOKE_CONTRACT_VALIDATOR "))
      return opcode;
  }
  return std::nullopt;
}

std::vector<CellRef> resultsFor(Harness &h, const CompilationResult &program) {
  std::vector<CellRef> results;
  for (const CellRef slot : program.resultSlots) {
    const auto values = h.vm.resultCells(slot);
    results.insert(results.end(), values.begin(), values.end());
  }
  return results;
}

TEST(VQLContractValidatorFrameTest,
     ComputedReturnUsesBooleanValidatorFrameAndPreservesReturnedIdentity) {
  Harness h;
  const CellRef item = h.arena.makeCell("return identity");
  constexpr std::string_view source =
      "def pure fallback(): Cell := \"temporary default\"; "
      "def pure keep($item: Cell): Cell { +d.optional: Cell := "
      "fallback() } := $item; keep($item)";
  const auto compiled = h.compile(source, {{"item", {item}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  const auto invoke = validatorInvoke(h, compiled);
  ASSERT_TRUE(invoke);
  ASSERT_EQ(h.vm.getOpcodeKind(*invoke), OpcodeKind::InvokeFrame);
  const auto inputs = h.core.inputsOf(*invoke);
  ASSERT_FALSE(inputs.empty());
  const auto descriptor = h.vm.frameDescriptor(inputs.front());
  ASSERT_TRUE(descriptor) << descriptor.error().message;
  ASSERT_EQ(descriptor->resultPorts.size(), 1U);
  EXPECT_EQ(descriptor->resultPorts.front().kind, FramePortKind::Cell);

  ASSERT_TRUE(compiled.entryOpcode);
  const auto cursor =
      h.vm.spawnCursor(*compiled.entryOpcode, "contract-validator-identity");
  const auto execution = h.vm.run(cursor, 100'000);
  h.vm.setCursorOpcode(cursor, std::nullopt);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_EQ(resultsFor(h, compiled), (std::vector<CellRef>{item}));
  EXPECT_FALSE(h.core.findDimension("d.optional"));
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VQLContractValidatorFrameTest,
     PresentReturnFieldSkipsAnInvalidComputedDefault) {
  Harness h;
  const CellRef missing = h.arena.makeCell("missing return field");
  const CellRef present = h.arena.makeCell("present return field");
  const CellRef number  = h.arena.makeScalarCell(std::int64_t{12});
  h.link(present, "d.value", number);
  constexpr std::string_view source =
      "def pure bad(): Cell := \"not an integer\"; "
      "def pure keep($item: Cell): Cell { +d.value: Int := bad() } := "
      "$item; keep($item)";
  const auto compiled = h.compile(source, {{"item", {present}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  ASSERT_TRUE(validatorInvoke(h, compiled));

  const auto accepted = h.run(source, {{"item", {present}}});
  ASSERT_TRUE(accepted) << accepted.error().message;
  EXPECT_EQ(*accepted, (std::vector<CellRef>{present}));

  const auto rejected = h.run(source, {{"item", {missing}}});
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().category,
            QueryExecutionError::Category::TypeContract);
  EXPECT_NE(rejected.error().message.find("keep"), std::string::npos);
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VQLContractValidatorFrameTest,
     EmptyOptionalReturnPassesButRequiredEmptyReturnIsNamedFailure) {
  Harness h;
  constexpr std::string_view source = "def pure empty($items: Stream<Cell>): "
                                      "Cell { +d.value: Int := 7 + 0 }? := "
                                      "$items; empty($items)";
  const auto optional               = h.run(source, {{"items", {}}});
  ASSERT_TRUE(optional) << optional.error().message;
  EXPECT_TRUE(optional->empty());

  constexpr std::string_view requiredSource =
      "def pure empty($items: Stream<Cell>): Cell { +d.value: Int := 7 + 0 } "
      ":= "
      "$items; empty($items)";
  const auto required = h.run(requiredSource, {{"items", {}}});
  ASSERT_FALSE(required);
  EXPECT_EQ(required.error().category,
            QueryExecutionError::Category::TypeContract);
  EXPECT_NE(required.error().message.find("empty"), std::string::npos);

  const auto falseResult =
      h.run("def pure false_value(): Bool { +d.aux: Int := 7 + 0 } := false; "
            "false_value()");
  ASSERT_TRUE(falseResult) << falseResult.error().message;
  ASSERT_EQ(falseResult->size(), 1U);
  EXPECT_EQ(h.arena.asBool(falseResult->front()), false);
}

TEST(VQLContractValidatorFrameTest,
     FrozenLexicalStreamsKeepOrderDuplicatesAndCallerBindings) {
  Harness h;
  const CellRef item   = h.arena.makeCell("return item");
  const CellRef first  = h.arena.makeCell("first text");
  const CellRef middle = h.arena.makeScalarCell(std::int64_t{2});
  const CellRef caller = h.arena.makeCell("caller binding");
  constexpr std::string_view source =
      "def pure identity(): Stream<Cell> := .; "
      "def pure keep($item: Cell): Cell { +d.first: String := "
      "$xs/identity()[1], "
      "+d.middle: Int := $xs/identity()[2], +d.last: String := "
      "$xs/identity()[3], "
      "+d.shadow: $value: String := $value/identity()[1], "
      "+d.optional: String? := $empty/identity()[1] } := $item; "
      "return keep($item), $value";
  const auto result = h.run(source, {{"item", {item}},
                                     {"xs", {first, middle, first}},
                                     {"value", {caller}},
                                     {"empty", {}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{item, caller}));

  const auto wrongOrder = h.run(source, {{"item", {item}},
                                         {"xs", {middle, first, first}},
                                         {"value", {caller}},
                                         {"empty", {}}});
  ASSERT_FALSE(wrongOrder);
  EXPECT_EQ(wrongOrder.error().category,
            QueryExecutionError::Category::TypeContract);

  const auto missing = h.compile(
      "def pure keep($item: Cell): Cell { +d.missing: Cell := $unknown, "
      "+d.force: Int := 1 + 0 } "
      ":= $item; keep($item)",
      {{"item", {item}}});
  EXPECT_FALSE(missing.success);
  EXPECT_NE(missing.errorMessage.find("unknown"), std::string::npos);
}

TEST(VQLContractValidatorFrameTest,
     SameCompiledGraphRechecksCurrentOwnerOnFreshCursor) {
  Harness h;
  const CellRef item     = h.arena.makeCell("mutable owner");
  const CellRef original = h.arena.makeScalarCell(std::int64_t{7});
  h.link(item, "d.fallback", original);
  constexpr std::string_view source =
      "def pure keep($item: Cell): Cell { +d.value: Int := ./+d.fallback } "
      ":= $item; keep($item)";
  const auto compiled = h.compile(source, {{"item", {item}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  ASSERT_TRUE(compiled.entryOpcode);

  auto cursor    = h.vm.spawnCursor(*compiled.entryOpcode, "validator-before");
  auto execution = h.vm.run(cursor, 100'000);
  h.vm.setCursorOpcode(cursor, std::nullopt);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_EQ(resultsFor(h, compiled), (std::vector<CellRef>{item}));

  const CellRef wrong = h.arena.makeCell("not an integer");
  h.link(item, "d.fallback", wrong);
  cursor    = h.vm.spawnCursor(*compiled.entryOpcode, "validator-after");
  execution = h.vm.run(cursor, 100'000);
  h.vm.setCursorOpcode(cursor, std::nullopt);
  EXPECT_FALSE(execution.success);
  EXPECT_NE(execution.errorMessage.find("keep"), std::string::npos);
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VQLContractValidatorFrameTest,
     RecursiveComputedReturnFailureRecoversAndParameterMismatchStillFilters) {
  Harness h;
  const CellRef item = h.arena.makeCell("recursive return");
  h.vm.setFrameLimits({.maxDepth = 4});
  constexpr std::string_view recursive =
      "def pure loop($item: Cell): Cell { +d.value: Cell := loop($item) } := "
      "$item; loop($item)";
  const auto failed = h.run(recursive, {{"item", {item}}});
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().category, QueryExecutionError::Category::Budget);
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);

  h.vm.setFrameLimits({});
  const auto recovered = h.run("def pure okay(): Int := 7; okay()");
  ASSERT_TRUE(recovered) << recovered.error().message;
  ASSERT_EQ(recovered->size(), 1U);
  EXPECT_EQ(h.arena.asInt64(recovered->front()),
            std::optional<std::int64_t>{7});

  constexpr std::string_view mismatch =
      "def pure typed($value: Int): Cell { +d.value: Int := 1 + 0 } := $value; "
      "typed($bad)";
  const auto filtered = h.run(mismatch, {{"bad", {item}}});
  ASSERT_TRUE(filtered) << filtered.error().message;
  EXPECT_TRUE(filtered->empty());
}

TEST(VQLContractValidatorFrameTest,
     ExportedValidatorFrameRunsAfterCompilerAndSourceArenaDestruction) {
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<UserPermascroll>(
      UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Harness h;
    constexpr std::string_view source =
        "def pure keep($item: Cell): Cell { +d.value: Int := 9 + 0 } := $item; "
        "keep(##/d.expected[1])";
    const auto expected = h.arena.makeCell("exported expected");
    h.link(h.core.home(), "d.expected", expected);
    VQLCompiler compiler(h.coordinator, h.vm);
    const auto compiled = compiler.compile(source);
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    ASSERT_TRUE(validatorInvoke(h, compiled));
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
  MultiStoreCoordinator coordinator(core);
  zigzag::vortex::VortexVM vm(core);
  vm.setContractCoordinator(coordinator);
  const auto entry =
      arena.linked(core.home(), core.dims().spin, DimVector::POS);
  ASSERT_TRUE(entry);
  const auto cursor    = vm.spawnCursor(*entry, "reloaded-return-validator");
  const auto execution = vm.run(cursor, 100'000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;

  auto terminal = *entry;
  auto next     = arena.linked(terminal, core.dims().spin, DimVector::POS);
  while (next && vm.getOpcodeKind(*next) != OpcodeKind::Halt) {
    terminal = *next;
    next     = arena.linked(terminal, core.dims().spin, DimVector::POS);
  }
  ASSERT_TRUE(next);
  const auto resultSlots = core.inputsOf(*next);
  ASSERT_EQ(resultSlots.size(), 1U);
  const auto results = vm.resultCells(resultSlots.front());
  ASSERT_EQ(results.size(), 1U);
  const auto expectedDim = coordinator.resolveDimension("d.expected");
  const auto expected = arena.linked(core.home(), expectedDim, DimVector::POS);
  ASSERT_TRUE(expected);
  EXPECT_EQ(results.front(), *expected);
  EXPECT_FALSE(vm.hasActiveFrames());
}

TEST(VQLContractValidatorFrameTest,
     UnusedBindingsDoNotConsumeTheValidatorFrameLimit) {
  Harness h;
  const CellRef item = h.arena.makeCell("only used subject");
  h.vm.setFrameLimits({.maxLocalsPerFrame = 128});
  constexpr std::string_view source =
      "def weave keep($item: Cell): Cell { +d.value: Int := 7 + 0 } := "
      "$item; keep($item)";
  const auto baseline = h.run(source, {{"item", {item}}});
  ASSERT_TRUE(baseline) << baseline.error().message;
  EXPECT_EQ(*baseline, (std::vector<CellRef>{item}));

  ContractBindings bindings{{"item", {item}}};
  for (std::size_t index = 0; index < 200; ++index)
    bindings.emplace("unused_" + std::to_string(index),
                     std::vector<CellRef>{item});
  const auto withUnused = h.run(source, std::move(bindings));
  ASSERT_TRUE(withUnused) << withUnused.error().message;
  EXPECT_EQ(*withUnused, (std::vector<CellRef>{item}));
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VQLContractValidatorFrameTest,
     HostMeshAndInlinePipelineKeepTheirExistingContractRoutes) {
  constexpr std::string_view source =
      "def pure keep($item: Cell): Cell { +d.value: Int := 9 + 0 } := $item; "
      "keep(##)";
  zigzag::vortex::VortexHost host;
  const auto hostResult = host.executeVQLChecked(source);
  ASSERT_TRUE(hostResult) << hostResult.error().message;
  ASSERT_EQ(hostResult->size(), 1U);

  namespace mesh        = zigzag::vortex::mesh;
  const auto meshResult = mesh::evaluate(mesh::Frontend::Vquery, source,
                                         {.maxCycles      = 100'000,
                                          .maxCells       = 100'000,
                                          .maxSolutions   = 16,
                                          .maxOutputBytes = 1024 * 1024});
  ASSERT_TRUE(meshResult) << meshResult.error();

  Harness h;
  const CellRef outside   = h.arena.makeCell("unrelated pipeline input");
  const CellRef keepValue = h.arena.makeCell("inline return");
  const CellRef pipeline  = h.vm.mintOpcode(OpcodeKind::Assert);
  h.core.bindInput(pipeline, h.arena.makeScalarCell(false));
  h.core.attachPipeline(outside, pipeline);
  constexpr std::string_view inlineSource =
      "def weave keep($item: Cell): Cell { +d.value: Int := 9 + 0 } := { "
      "return $item; }; keep($item)";
  const auto compiled =
      h.compile(inlineSource, {{"item", {keepValue}}, {"outside", {outside}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_FALSE(validatorInvoke(h, compiled));
  const auto representable = h.compile(inlineSource, {{"item", {keepValue}}});
  ASSERT_TRUE(representable.success) << representable.errorMessage;
  EXPECT_TRUE(validatorInvoke(h, representable));
  EXPECT_TRUE(h.core.hasPipeline(outside));
  const auto result =
      h.run(inlineSource, {{"item", {keepValue}}, {"outside", {outside}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{keepValue}));
}

} // namespace
} // namespace xanadu::vql

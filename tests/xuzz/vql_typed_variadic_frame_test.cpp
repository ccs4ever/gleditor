/**
 * @file vql_typed_variadic_frame_test.cpp
 * @brief Typed variadic actual groups in reusable native VQL frames.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/mesh_evaluator.hpp"
#include "common/xanadu/vortex/vortex_host.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::FrameDescriptor;
using zigzag::vortex::OpcodeKind;

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_typed_variadic_frames_" +
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

  CellRef cell(const std::string_view text) { return arena.makeCell(text); }

  CellRef integer(const std::int64_t value) {
    return arena.makeScalarCell(value);
  }

  void link(const CellRef owner, const std::string_view dimension,
            const CellRef target) {
    zigzag::expectWritten(arena.link(owner,
                                     coordinator.resolveDimension(dimension),
                                     DimVector::POS, target));
  }

  CompilationResult compile(const std::string_view source,
                            const CompilationOptions &options = {}) {
    VQLCompiler compiler(coordinator, vm);
    return compiler.compile(source, options);
  }

  std::expected<std::vector<CellRef>, QueryExecutionError>
  execute(const std::string_view source, ContractBindings bindings = {}) {
    QueryExecutor executor(coordinator, vm, {}, std::move(bindings));
    return executor.execute(source);
  }
};

std::vector<CellRef> invokes(const Harness &h,
                             const CompilationResult &compiled,
                             const std::string_view name) {
  std::vector<CellRef> found;
  std::unordered_set<CellRef> seen;
  const std::string prefix = "#INVOKE_FRAME " + std::string(name);
  for (const CellRef opcode : compiled.generatedOpcodes) {
    if (h.vm.getOpcodeKind(opcode) != OpcodeKind::InvokeFrame ||
        !seen.insert(opcode).second)
      continue;
    if (h.arena.textOf(opcode).starts_with(prefix)) found.push_back(opcode);
  }
  return found;
}

std::optional<CellRef> descriptorOf(const Harness &h, const CellRef invoke) {
  const auto ports = h.core.inputsOf(invoke);
  if (ports.empty()) return std::nullopt;
  return ports.front();
}

std::unordered_set<CellRef>
descriptorsForLabel(const Harness &h, const CompilationResult &compiled,
                    const std::string_view name, const std::string_view label) {
  std::unordered_set<CellRef> descriptors;
  for (const CellRef invoke : invokes(h, compiled, name)) {
    if (h.arena.textOf(invoke) != label) continue;
    if (const auto descriptor = descriptorOf(h, invoke))
      descriptors.insert(*descriptor);
  }
  return descriptors;
}

std::unordered_set<std::string> invokeLabels(const Harness &h,
                                             const CompilationResult &compiled,
                                             const std::string_view name) {
  std::unordered_set<std::string> labels;
  for (const CellRef invoke : invokes(h, compiled, name))
    labels.emplace(h.arena.textOf(invoke));
  return labels;
}

std::size_t descriptorRecords(const FrameDescriptor &descriptor) {
  return 1 + descriptor.locals.size() + descriptor.argumentPorts.size() +
         descriptor.resultPorts.size();
}

std::vector<CellRef> resultsOf(Harness &h, const CompilationResult &compiled) {
  std::vector<CellRef> results;
  for (const CellRef slot : compiled.resultSlots) {
    const auto cells = h.vm.resultCells(slot);
    results.insert(results.end(), cells.begin(), cells.end());
  }
  return results;
}

TEST(VQLTypedVariadicFrameTest,
     NamedInvokePreservesActualGroupsAndExactValuesAcrossSourceRoutes) {
  Harness h;
  const CellRef first  = h.integer(1);
  const CellRef second = h.integer(2);
  const CellRef third  = h.integer(3);
  h.link(h.core.home(), "d.items", first);
  h.link(first, "d.items", second);
  h.link(second, "d.items", third);

  constexpr std::string_view source =
      "def pure collect(...$rest: Stream<Int>): Stream<Int> := $rest; "
      "collect(##/d.items, 9007199254740993, ##/d.items[-1])";
  const auto compiled = h.compile(source);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  const auto calls = invokes(h, compiled, "collect");
  ASSERT_FALSE(calls.empty());
  EXPECT_EQ(invokeLabels(h, compiled, "collect"),
            (std::unordered_set<std::string>{"#INVOKE_FRAME collect<rest=3>"}));
  const auto threeGroupDescriptors = descriptorsForLabel(
      h, compiled, "collect", "#INVOKE_FRAME collect<rest=3>");
  ASSERT_EQ(threeGroupDescriptors.size(), 1U);
  const auto descriptor = h.vm.frameDescriptor(*threeGroupDescriptors.begin());
  ASSERT_TRUE(descriptor) << descriptor.error().message;
  EXPECT_EQ(descriptor->argumentPorts.size(), 4U);

  ASSERT_TRUE(compiled.cursor);
  const auto execution = h.vm.run(*compiled.cursor, 100'000);
  h.vm.setCursorOpcode(*compiled.cursor, std::nullopt);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  const auto values = resultsOf(h, compiled);
  ASSERT_EQ(values.size(), 5U);
  EXPECT_EQ(values[0], first);
  EXPECT_EQ(values[1], second);
  EXPECT_EQ(values[2], third);
  EXPECT_EQ(values[4], third);
  EXPECT_EQ(h.arena.asInt64(values[0]), 1);
  EXPECT_EQ(h.arena.asInt64(values[1]), 2);
  EXPECT_EQ(h.arena.asInt64(values[2]), 3);
  EXPECT_EQ(h.arena.asInt64(values[3]), INT64_C(9007199254740993));
  EXPECT_EQ(h.arena.asInt64(values[4]), 3);

  constexpr std::string_view ufcsSource =
      "def pure collect($head: Stream<Int>, ...$rest: Int): Stream<Int> := "
      "$rest; ##/d.items/collect(7)";
  const auto ufcs = h.compile(ufcsSource);
  ASSERT_TRUE(ufcs.success) << ufcs.errorMessage;
  const auto ufcsCalls = invokes(h, ufcs, "collect");
  ASSERT_FALSE(ufcsCalls.empty());
  EXPECT_EQ(invokeLabels(h, ufcs, "collect"),
            (std::unordered_set<std::string>{"#INVOKE_FRAME collect<rest=0>",
                                             "#INVOKE_FRAME collect<rest=1>"}));
  const auto ufcsDescriptors =
      descriptorsForLabel(h, ufcs, "collect", "#INVOKE_FRAME collect<rest=1>");
  ASSERT_EQ(ufcsDescriptors.size(), 1U);
  const auto ufcsDescriptor = h.vm.frameDescriptor(*ufcsDescriptors.begin());
  ASSERT_TRUE(ufcsDescriptor) << ufcsDescriptor.error().message;
  EXPECT_EQ(ufcsDescriptor->argumentPorts.size(), 3U);
  const auto ufcsResult = h.execute(ufcsSource);
  ASSERT_TRUE(ufcsResult) << ufcsResult.error().message;
  ASSERT_EQ(ufcsResult->size(), 3U);
  for (const auto value : *ufcsResult) EXPECT_EQ(h.arena.asInt64(value), 7);

  const auto invalid =
      h.execute("def pure collect(...$rest: Int): Stream<Int> := $rest; "
                "collect(##/d.items)");
  ASSERT_TRUE(invalid) << invalid.error().message;
  EXPECT_TRUE(invalid->empty());
  const auto valid =
      h.execute("def pure collect(...$rest: Int): Stream<Int> := $rest; "
                "collect(7, 9007199254740993, 7)");
  ASSERT_TRUE(valid) << valid.error().message;
  ASSERT_EQ(valid->size(), 3U);
  EXPECT_EQ(h.arena.asInt64((*valid)[0]), 7);
  EXPECT_EQ(h.arena.asInt64((*valid)[1]), INT64_C(9007199254740993));
  EXPECT_EQ(h.arena.asInt64((*valid)[2]), 7);

  constexpr std::string_view routedSource =
      "def pure collect(...$rest: Stream<Int>): Stream<Int> := $rest; "
      "collect(7, 9007199254740993, 7)";
  zigzag::vortex::VortexHost host;
  const auto hostResult = host.executeVQLChecked(routedSource);
  ASSERT_TRUE(hostResult) << hostResult.error().message;
  ASSERT_EQ(hostResult->size(), 3U);
  EXPECT_EQ(host.core().arena().asInt64((*hostResult)[1]),
            INT64_C(9007199254740993));

  namespace mesh        = zigzag::vortex::mesh;
  const auto meshResult = mesh::evaluate(mesh::Frontend::Vquery, routedSource,
                                         {.maxCycles      = 10000,
                                          .maxCells       = 10000,
                                          .maxSolutions   = 16,
                                          .maxOutputBytes = 1024 * 1024});
  ASSERT_TRUE(meshResult) << meshResult.error();
  auto &meshArena = meshResult->context->arena;
  std::optional<CellRef> items;
  std::optional<CellRef> value;
  for (const auto &cell : meshArena.cells()) {
    if (meshArena.textOf(cell.birthOp) == "d.mesh.items") items = cell.birthOp;
    if (meshArena.textOf(cell.birthOp) == "d.mesh.value") value = cell.birthOp;
  }
  ASSERT_TRUE(items);
  ASSERT_TRUE(value);
  std::vector<std::int64_t> meshValues;
  auto record = meshArena.linked(meshResult->root, *items, DimVector::POS);
  while (record) {
    const auto output = meshArena.linked(*record, *value, DimVector::POS);
    ASSERT_TRUE(output);
    const auto integer = meshArena.asInt64(*output);
    ASSERT_TRUE(integer);
    meshValues.push_back(*integer);
    record = meshArena.linked(*record, *items, DimVector::POS);
  }
  EXPECT_EQ(meshValues,
            (std::vector<std::int64_t>{7, INT64_C(9007199254740993), 7}));
}

TEST(VQLTypedVariadicFrameTest,
     EmptyCallAndOneExplicitEmptyGroupHaveDistinctDescriptors) {
  Harness h;
  constexpr std::string_view source =
      "def pure collect(...$rest: Int): Stream<Int> := $rest; "
      "let $empty := ##/d.missing; collect(); collect($empty)";
  const auto compiled = h.compile(source);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  const auto calls = invokes(h, compiled, "collect");
  ASSERT_FALSE(calls.empty());
  EXPECT_EQ(invokeLabels(h, compiled, "collect"),
            (std::unordered_set<std::string>{"#INVOKE_FRAME collect<rest=0>",
                                             "#INVOKE_FRAME collect<rest=1>"}));
  const auto zeroDescriptors = descriptorsForLabel(
      h, compiled, "collect", "#INVOKE_FRAME collect<rest=0>");
  const auto oneDescriptors = descriptorsForLabel(
      h, compiled, "collect", "#INVOKE_FRAME collect<rest=1>");
  ASSERT_EQ(zeroDescriptors.size(), 1U);
  ASSERT_EQ(oneDescriptors.size(), 1U);
  EXPECT_NE(*zeroDescriptors.begin(), *oneDescriptors.begin());
  const auto execution = h.execute(source);
  ASSERT_TRUE(execution) << execution.error().message;
  EXPECT_TRUE(execution->empty());

  const auto emptyBeforeValid =
      h.execute("def pure collect(...$rest: Int): Stream<Int> := $rest; "
                "let $empty := ##/d.missing; collect($empty, 7)");
  ASSERT_TRUE(emptyBeforeValid) << emptyBeforeValid.error().message;
  EXPECT_TRUE(emptyBeforeValid->empty());
}

TEST(VQLTypedVariadicFrameTest,
     ReusesSameArityDescriptorAndFiltersAfterEveryGroupMatch) {
  Harness h;
  constexpr std::string_view repeated =
      "def pure collect(...$rest: Int): Stream<Int> := $rest; "
      "collect(1); collect(2); collect(3, 4)";
  const auto compiled = h.compile(repeated);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  const auto calls = invokes(h, compiled, "collect");
  ASSERT_FALSE(calls.empty());
  EXPECT_EQ(invokeLabels(h, compiled, "collect"),
            (std::unordered_set<std::string>{"#INVOKE_FRAME collect<rest=1>",
                                             "#INVOKE_FRAME collect<rest=2>"}));
  const auto oneGroupDescriptors = descriptorsForLabel(
      h, compiled, "collect", "#INVOKE_FRAME collect<rest=1>");
  const auto twoGroupDescriptors = descriptorsForLabel(
      h, compiled, "collect", "#INVOKE_FRAME collect<rest=2>");
  ASSERT_EQ(oneGroupDescriptors.size(), 1U);
  ASSERT_EQ(twoGroupDescriptors.size(), 1U);
  EXPECT_NE(*oneGroupDescriptors.begin(), *twoGroupDescriptors.begin());

  const auto sentinel = h.cell("body ran");
  const auto lateMismatch =
      h.execute("def pure guarded(...$rest: Int): Cell := $sentinel; "
                "guarded(1, 2, \"bad\")",
                {{"sentinel", {sentinel}}});
  ASSERT_TRUE(lateMismatch) << lateMismatch.error().message;
  EXPECT_TRUE(lateMismatch->empty());

  const auto skippedPromise =
      h.execute("def pure guarded(...$rest: Int): String := $rest; "
                "guarded(1, 2, \"bad\")");
  ASSERT_TRUE(skippedPromise) << skippedPromise.error().message;
  EXPECT_TRUE(skippedPromise->empty());
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VQLTypedVariadicFrameTest,
     SelfAndMutualRecursionReloadWithChangedRestGroupArity) {
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Harness h;
    constexpr std::string_view source =
        "def pure even($n: Int, ...$rest: Stream<Int>): Stream<Int> := { "
        "if $n > 0 return odd($n + -1, $rest) else return $rest; }; "
        "def pure odd($n: Int, ...$rest: Stream<Int>): Stream<Int> := { "
        "if $n > 0 return even($n + -1, $rest) else return $rest; }; "
        "even(2, 7, 8)";
    VQLCompiler compiler(h.coordinator, h.vm);
    const auto compiled = compiler.compile(source);
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    const auto evenCalls = invokes(h, compiled, "even");
    ASSERT_FALSE(evenCalls.empty());
    EXPECT_EQ(invokeLabels(h, compiled, "even"),
              (std::unordered_set<std::string>{"#INVOKE_FRAME even<rest=1>",
                                               "#INVOKE_FRAME even<rest=2>"}));
    bool sawOddOneGroup  = false;
    bool sawEvenOneGroup = false;
    for (const CellRef invoke : compiled.generatedOpcodes) {
      const std::string label = h.arena.textOf(invoke);
      sawOddOneGroup |= label == "#INVOKE_FRAME odd<rest=1>";
      sawEvenOneGroup |= label == "#INVOKE_FRAME even<rest=1>";
    }
    EXPECT_TRUE(sawOddOneGroup);
    EXPECT_TRUE(sawEvenOneGroup);

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
  const auto cursor = vm.spawnCursor(*entry, "typed-variadic-recursive-reload");
  const auto execution = vm.run(cursor, 100'000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;

  auto opcode = *entry;
  auto next   = arena.linked(opcode, core.dims().spin, DimVector::POS);
  while (next && vm.getOpcodeKind(*next) != OpcodeKind::Halt) {
    opcode = *next;
    next   = arena.linked(opcode, core.dims().spin, DimVector::POS);
  }
  ASSERT_TRUE(next);
  const auto resultPorts = core.inputsOf(*next);
  ASSERT_EQ(resultPorts.size(), 1U);
  const auto result = vm.resultCells(resultPorts.front());
  ASSERT_EQ(result.size(), 2U);
  EXPECT_EQ(arena.asInt64(result[0]), 7);
  EXPECT_EQ(arena.asInt64(result[1]), 8);
  EXPECT_FALSE(vm.hasActiveFrames());
}

TEST(VQLTypedVariadicFrameTest,
     RestContractDefaultsUsePriorFixedFormalsAndCallerCaptures) {
  Harness h;
  const CellRef owner          = h.cell("owner");
  const CellRef capturedString = h.cell("captured string");
  h.link(owner, "d.first", capturedString);
  h.link(owner, "d.defaultScore", h.integer(55));
  const CellRef outside = h.integer(55);
  const auto defaulted =
      h.execute("def pure defaults($first: Int := 7, $second: Int := $first, "
                "...$rest: Int): Int := $second; defaults()");
  ASSERT_TRUE(defaulted) << defaulted.error().message;
  ASSERT_EQ(defaulted->size(), 1U);
  EXPECT_EQ(h.arena.asInt64(defaulted->front()), 7);

  constexpr std::string_view source =
      "def pure fallback($value: Int): Int := $value; "
      "def pure check($first: Int := 7, $second: Int := $first, "
      "...$rest: Cell { +d.score: Int := fallback($second) }): "
      "Stream<Cell> := $rest; check(7, 55, $owner)";
  ContractBindings bindings{{"owner", {owner}},
                            {"second", {h.cell("wrong caller binding")}}};
  CompilationOptions options;
  options.initialBindings = bindings;
  const auto compiled     = h.compile(source, options);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  ASSERT_FALSE(invokes(h, compiled, "check").empty());
  const auto result = h.execute(source, bindings);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{owner}));

  const auto freeAlias = h.execute(
      "def pure check(...$rest: Cell { +d.first: $outside: String, "
      "+d.second: Int := $outside }): Stream<Cell> := $rest; check($owner)",
      {{"owner", {owner}}, {"outside", {outside}}});
  ASSERT_TRUE(freeAlias) << freeAlias.error().message;
  EXPECT_EQ(*freeAlias, (std::vector<CellRef>{owner}));

  const auto owningContext =
      h.execute("def pure check(...$rest: Cell { +d.score: Int := "
                "./+d.defaultScore }): Stream<Cell> := $rest; check($owner)",
                {{"owner", {owner}}});
  ASSERT_TRUE(owningContext) << owningContext.error().message;
  EXPECT_EQ(*owningContext, (std::vector<CellRef>{owner}));

  const auto noLeak = h.execute(
      "def pure reveal(...$rest: Cell { +d.first: $outside: String }): Int "
      ":= $outside; reveal($owner)",
      {{"owner", {owner}}, {"outside", {outside}}});
  ASSERT_TRUE(noLeak) << noLeak.error().message;
  ASSERT_EQ(noLeak->size(), 1U);
  EXPECT_EQ(h.arena.asInt64(noLeak->front()), 55);
}

TEST(VQLTypedVariadicFrameTest,
     RestOnlyDefaultCaptureIsRequiredOnlyForPositiveRestGroups) {
  Harness h;
  constexpr std::string_view emptyDefinition =
      "def pure empty(...$rest: Cell { +d.score: Int := $outside }): "
      "Stream<Cell> := $rest; ";

  const std::string directSource = std::string(emptyDefinition) + "empty()";
  const auto direct              = h.compile(directSource);
  ASSERT_TRUE(direct.success) << direct.errorMessage;
  EXPECT_FALSE(invokes(h, direct, "empty").empty());
  EXPECT_EQ(invokeLabels(h, direct, "empty"),
            (std::unordered_set<std::string>{"#INVOKE_FRAME empty<rest=0>"}));
  const auto directResult = h.execute(directSource);
  ASSERT_TRUE(directResult) << directResult.error().message;
  EXPECT_TRUE(directResult->empty());

  const std::string wrappedSource =
      std::string(emptyDefinition) +
      "def pure wrapper(): Stream<Cell> := empty(); wrapper()";
  const auto wrapped = h.compile(wrappedSource);
  ASSERT_TRUE(wrapped.success) << wrapped.errorMessage;
  EXPECT_EQ(invokeLabels(h, wrapped, "empty"),
            (std::unordered_set<std::string>{"#INVOKE_FRAME empty<rest=0>"}));
  EXPECT_FALSE(invokes(h, wrapped, "wrapper").empty());
  const auto wrappedResult = h.execute(wrappedSource);
  ASSERT_TRUE(wrappedResult) << wrappedResult.error().message;
  EXPECT_TRUE(wrappedResult->empty());

  const std::string lazySource =
      std::string(emptyDefinition) +
      "def pure lazy($value: Stream<Cell> := empty()): Stream<Cell> := "
      "$value; lazy()";
  const auto lazy = h.compile(lazySource);
  ASSERT_TRUE(lazy.success) << lazy.errorMessage;
  EXPECT_FALSE(invokes(h, lazy, "lazy").empty());
  const auto lazyResult = h.execute(lazySource);
  ASSERT_TRUE(lazyResult) << lazyResult.error().message;
  EXPECT_TRUE(lazyResult->empty());

  constexpr std::string_view nestedSource =
      "def pure fallback($owner: Cell { +d.score: Int := $outside }): "
      "Int := 7; def pure nested(...$rest: Cell { +d.score: Int := "
      "fallback(.) }): Stream<Cell> := $rest; nested()";
  const auto nested = h.compile(nestedSource);
  ASSERT_TRUE(nested.success) << nested.errorMessage;
  EXPECT_FALSE(invokes(h, nested, "nested").empty());
  const auto nestedResult = h.execute(nestedSource);
  ASSERT_TRUE(nestedResult) << nestedResult.error().message;
  EXPECT_TRUE(nestedResult->empty());

  const auto positiveOwner = h.cell("positive rest group");
  const std::string positiveSource =
      std::string(emptyDefinition) + "empty($owner)";
  const auto positive = h.compile(
      positiveSource,
      CompilationOptions{.initialBindings = {{"owner", {positiveOwner}}}});
  EXPECT_FALSE(positive.success);
  EXPECT_NE(positive.errorMessage.find(
                "contract default $outside has no prior lexical binding"),
            std::string::npos);

  const std::string positiveWrapperSource =
      std::string(emptyDefinition) +
      "def pure wrapper($owner: Cell): Stream<Cell> := empty($owner); "
      "wrapper($owner)";
  const auto positiveWrapper = h.compile(
      positiveWrapperSource,
      CompilationOptions{.initialBindings = {{"owner", {positiveOwner}}}});
  EXPECT_FALSE(positiveWrapper.success);
  EXPECT_NE(positiveWrapper.errorMessage.find(
                "contract default $outside has no prior lexical binding"),
            std::string::npos);

  const std::string ufcsSource =
      "def pure select($head: Stream<Cell>, ...$rest: Cell { "
      "+d.score: Int := $outside }): Stream<Cell> := $rest; "
      "##/d.items/select(##/d.items[-1])";
  const auto ufcsOwner = h.cell("UFCS rest group");
  h.link(h.core.home(), "d.items", ufcsOwner);
  const auto ufcsPositive = h.compile(ufcsSource);
  EXPECT_FALSE(ufcsPositive.success);
  EXPECT_NE(ufcsPositive.errorMessage.find(
                "contract default $outside has no prior lexical binding"),
            std::string::npos);

  constexpr std::string_view ufcsWrapperSource =
      "def pure select($head: Stream<Cell>, ...$rest: Cell { "
      "+d.score: Int := $outside }): Stream<Cell> := $rest; "
      "def pure wrapper(): Stream<Cell> := "
      "##/d.items/select(##/d.items[-1]); wrapper()";
  const auto missingUfcsCapture = h.compile(ufcsWrapperSource);
  EXPECT_FALSE(missingUfcsCapture.success);
  EXPECT_NE(missingUfcsCapture.errorMessage.find(
                "contract default $outside has no prior lexical binding"),
            std::string::npos);
  const ContractBindings ufcsBindings{{"outside", {h.integer(55)}}};
  const auto boundUfcs = h.compile(
      ufcsWrapperSource, CompilationOptions{.initialBindings = ufcsBindings});
  ASSERT_TRUE(boundUfcs.success) << boundUfcs.errorMessage;
  EXPECT_FALSE(invokes(h, boundUfcs, "wrapper").empty());
  EXPECT_FALSE(invokes(h, boundUfcs, "select").empty());
  const auto boundUfcsResult = h.execute(ufcsWrapperSource, ufcsBindings);
  ASSERT_TRUE(boundUfcsResult) << boundUfcsResult.error().message;
  EXPECT_EQ(*boundUfcsResult, (std::vector<CellRef>{ufcsOwner}));
}

TEST(VQLTypedVariadicFrameTest,
     RestDefaultCaptureDoesNotHideOtherRequiredUsesAtZeroGroups) {
  Harness h;
  const auto item         = h.cell("item");
  const auto missingFixed = h.compile(
      "def pure fixed($value: Cell { +d.score: Int := $outside }, "
      "...$rest: Cell { +d.score: Int := $outside }): Cell := $value; "
      "fixed($item)",
      CompilationOptions{.initialBindings = {{"item", {item}}}});
  EXPECT_FALSE(missingFixed.success);
  EXPECT_NE(missingFixed.errorMessage.find(
                "contract default $outside has no prior lexical binding"),
            std::string::npos);

  const auto missingReturn = h.compile(
      "def pure result($item: Cell, ...$rest: Cell { +d.score: Int := "
      "$outside }): Cell { +d.score: Int := $outside } := $item; "
      "result($item)",
      CompilationOptions{.initialBindings = {{"item", {item}}}});
  EXPECT_FALSE(missingReturn.success);
  EXPECT_NE(missingReturn.errorMessage.find(
                "contract default $outside has no prior lexical binding"),
            std::string::npos);

  const auto outside = h.integer(55);
  CompilationOptions options;
  options.initialBindings = {{"outside", {outside}}};
  const auto body         = h.compile(
      "def pure body(...$rest: Cell { +d.score: Int := $outside }): Int := "
              "$outside; body()",
      options);
  ASSERT_TRUE(body.success) << body.errorMessage;
  ASSERT_FALSE(invokes(h, body, "body").empty());
  const auto result = h.execute(
      "def pure body(...$rest: Cell { +d.score: Int := $outside }): Int := "
      "$outside; body()",
      {{"outside", {outside}}});
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 1U);
  EXPECT_EQ(result->front(), outside);
}

TEST(VQLTypedVariadicFrameTest,
     EveryRestGroupKeepsItsMutationGuardAndRollsBackEarlierOwner) {
  Harness h;
  const CellRef target      = h.cell("target");
  const CellRef other       = h.cell("other");
  const CellRef targetScore = h.integer(1);
  const CellRef otherScore  = h.integer(2);
  h.link(target, "d.score", targetScore);
  h.link(other, "d.score", otherScore);

  constexpr std::string_view source =
      "def pure guarded($target: Cell, ...$xs: Cell { +d.score: Int }): "
      "Cell := $target{ *, +d.score: \"bad\" }; "
      "guarded($target, $target, $other)";
  CompilationOptions options;
  options.initialBindings = {{"target", {target}}, {"other", {other}}};
  const auto compiled     = h.compile(source, options);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  ASSERT_FALSE(invokes(h, compiled, "guarded").empty());

  const auto result =
      h.execute(source, {{"target", {target}}, {"other", {other}}});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().category,
            QueryExecutionError::Category::TypeContract);
  EXPECT_EQ(h.arena.linked(target, h.coordinator.resolveDimension("d.score"),
                           DimVector::POS),
            targetScore);
  EXPECT_EQ(h.arena.asInt64(targetScore), 1);
  EXPECT_EQ(h.arena.linked(other, h.coordinator.resolveDimension("d.score"),
                           DimVector::POS),
            otherScore);
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VQLTypedVariadicFrameTest,
     DescriptorAndOpcodeCapsRollbackAndPermitSameVmRetry) {
  Harness h;
  const std::string query =
      "def pure collect(...$rest: Int): Stream<Int> := $rest; "
      "collect(1); collect(2, 3); collect(4, 5, 6)";
  const std::size_t cellsBefore = h.arena.cellCount();
  CompilationOptions limited;
  limited.limits.maxNamedFrameVariants = 2;
  const auto refused                   = h.compile(query, limited);
  EXPECT_FALSE(refused.success);
  EXPECT_NE(refused.errorMessage.find("variant budget"), std::string::npos);
  EXPECT_EQ(h.arena.cellCount(), cellsBefore);

  CompilationOptions retryOptions;
  retryOptions.limits.maxNamedFrameVariants = 3;
  const auto retried                        = h.compile(query, retryOptions);
  ASSERT_TRUE(retried.success) << retried.errorMessage;
  std::unordered_set<CellRef> retryDescriptors;
  for (const std::string_view label :
       {"#INVOKE_FRAME collect<rest=1>", "#INVOKE_FRAME collect<rest=2>",
        "#INVOKE_FRAME collect<rest=3>"}) {
    const auto descriptors = descriptorsForLabel(h, retried, "collect", label);
    ASSERT_EQ(descriptors.size(), 1U);
    retryDescriptors.insert(*descriptors.begin());
  }
  EXPECT_EQ(retryDescriptors.size(), 3U);

  Harness opcodeHarness;
  const auto baseline = opcodeHarness.compile("7");
  ASSERT_TRUE(baseline.success) << baseline.errorMessage;
  ASSERT_FALSE(baseline.generatedOpcodes.empty());
  Harness terminalHarness;
  CompilationOptions terminalCap;
  terminalCap.limits.maxGeneratedOpcodes = baseline.generatedOpcodes.size() - 1;
  const std::size_t terminalCellsBefore  = terminalHarness.arena.cellCount();
  const auto terminalRefused = terminalHarness.compile("7", terminalCap);
  EXPECT_FALSE(terminalRefused.success);
  EXPECT_EQ(terminalHarness.arena.cellCount(), terminalCellsBefore);
  terminalCap.targetLibrary       = true;
  const auto libraryReturnRefused = terminalHarness.compile("7", terminalCap);
  EXPECT_FALSE(libraryReturnRefused.success);
  EXPECT_EQ(terminalHarness.arena.cellCount(), terminalCellsBefore);
  const auto terminalRetry = terminalHarness.compile("7");
  ASSERT_TRUE(terminalRetry.success) << terminalRetry.errorMessage;
}

TEST(VQLTypedVariadicFrameTest,
     DescriptorRecordCapAggregatesAcrossDifferentArityVariants) {
  constexpr std::string_view source =
      "def pure collect(...$rest: Int): Stream<Int> := $rest; "
      "collect(1); collect(2, 3)";
  Harness baselineHarness;
  const auto baseline = baselineHarness.compile(source);
  ASSERT_TRUE(baseline.success) << baseline.errorMessage;
  std::unordered_set<CellRef> namedDescriptors;
  for (const CellRef call : invokes(baselineHarness, baseline, "collect")) {
    const auto descriptorCell = descriptorOf(baselineHarness, call);
    ASSERT_TRUE(descriptorCell);
    namedDescriptors.insert(*descriptorCell);
  }
  ASSERT_EQ(namedDescriptors.size(), 2U);

  std::unordered_set<CellRef> allReturnFrameDescriptors;
  std::size_t totalRecords = 0;
  std::size_t largest      = 0;
  for (const CellRef opcode : baseline.generatedOpcodes) {
    if (baselineHarness.vm.getOpcodeKind(opcode) != OpcodeKind::ReturnFrame)
      continue;
    const auto inputs = baselineHarness.core.inputsOf(opcode);
    ASSERT_FALSE(inputs.empty());
    const CellRef descriptorCell = inputs.front();
    if (!allReturnFrameDescriptors.insert(descriptorCell).second) continue;
    const auto descriptor = baselineHarness.vm.frameDescriptor(descriptorCell);
    ASSERT_TRUE(descriptor) << descriptor.error().message;
    const std::size_t records = descriptorRecords(*descriptor);
    totalRecords += records;
    largest = std::max(largest, records);
  }
  ASSERT_GE(allReturnFrameDescriptors.size(), namedDescriptors.size());
  for (const CellRef descriptor : namedDescriptors)
    EXPECT_TRUE(allReturnFrameDescriptors.contains(descriptor));
  ASSERT_GT(totalRecords, largest);

  Harness limitedHarness;
  CompilationOptions options;
  options.limits.maxFrameDescriptorRecords = largest;
  const std::size_t cellsBefore            = limitedHarness.arena.cellCount();
  const auto refused = limitedHarness.compile(source, options);
  EXPECT_FALSE(refused.success);
  EXPECT_NE(refused.errorMessage.find("descriptor record budget"),
            std::string::npos);
  EXPECT_EQ(limitedHarness.arena.cellCount(), cellsBefore);
  const auto retried = limitedHarness.compile(source);
  ASSERT_TRUE(retried.success) << retried.errorMessage;
}

} // namespace
} // namespace xanadu::vql

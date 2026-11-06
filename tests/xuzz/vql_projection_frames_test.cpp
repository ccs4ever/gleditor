/**
 * @file vql_projection_frames_test.cpp
 * @brief Bounded shape projections inside reusable VQL activation frames.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
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
#include "common/xanadu/vql/frame_eligibility.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::FrameDescriptor;
using zigzag::vortex::FramePortKind;
using zigzag::vortex::OpcodeKind;

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};

  DimRef dim(const std::string_view name) {
    return coordinator.resolveDimension(name);
  }

  CellRef cell(const std::string_view text) { return arena.makeCell(text); }

  CellRef integer(const std::int64_t value) {
    return arena.makeScalarCell(value);
  }

  CellRef real(const double value) { return arena.makeScalarCell(value); }

  void link(const CellRef owner, const std::string_view name,
            const CellRef target, const DimVector direction = DimVector::POS) {
    zigzag::expectWritten(arena.link(owner, dim(name), direction, target));
  }

  CompilationResult compile(const std::string_view source,
                            ContractBindings bindings = {}) {
    CompilationOptions options;
    options.initialBindings = std::move(bindings);
    VQLCompiler compiler(coordinator, vm);
    return compiler.compile(source, options);
  }

  std::expected<std::vector<CellRef>, QueryExecutionError>
  execute(const std::string_view source, ContractBindings bindings = {}) {
    QueryExecutor executor(coordinator, vm, {}, std::move(bindings));
    return executor.execute(source);
  }

  std::optional<CellRef> field(const CellRef owner, const std::string_view name,
                               const DimVector direction = DimVector::POS) {
    return arena.linked(owner, dim(name), direction);
  }
};

struct CompiledRun {
  CompilationResult compilation;
  bool executed{false};
  std::string error;
  std::vector<CellRef> values;
};

CompiledRun runCompiled(Harness &h, const std::string_view source,
                        ContractBindings bindings = {}) {
  CompiledRun result;
  result.compilation = h.compile(source, std::move(bindings));
  if (!result.compilation.success) {
    result.error = result.compilation.errorMessage;
    return result;
  }
  if (!result.compilation.cursor) {
    result.error = "compiled projection frame has no cursor";
    return result;
  }
  const auto execution = h.vm.run(*result.compilation.cursor, 1'000'000);
  result.executed      = execution.success;
  result.error         = execution.errorMessage;
  h.vm.setCursorOpcode(*result.compilation.cursor, std::nullopt);
  if (!execution.success) return result;
  for (const CellRef slot : result.compilation.resultSlots) {
    const auto cells = h.vm.resultCells(slot);
    result.values.insert(result.values.end(), cells.begin(), cells.end());
  }
  return result;
}

struct FunctionFrame {
  CellRef invocation;
  FrameDescriptor descriptor;
};

std::optional<FunctionFrame>
functionFrame(const Harness &h, const CompilationResult &program,
              const std::string_view functionName) {
  const std::string label = "#INVOKE_FRAME " + std::string(functionName);
  for (const CellRef opcode : program.generatedOpcodes) {
    if (h.vm.getOpcodeKind(opcode) != OpcodeKind::InvokeFrame ||
        h.arena.textOf(opcode) != label)
      continue;
    const auto inputs = h.core.inputsOf(opcode);
    if (inputs.empty()) return std::nullopt;
    const auto descriptor = h.vm.frameDescriptor(inputs.front());
    if (!descriptor || !descriptor->entry) return std::nullopt;
    return FunctionFrame{.invocation = opcode, .descriptor = *descriptor};
  }
  return std::nullopt;
}

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_projection_frames_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

std::shared_ptr<ValueExpr> scalarValue(const std::int64_t value) {
  return std::make_shared<ValueExpr>(ValueExpr{
      .kind = ScalarLiteral{.value = value},
  });
}

std::shared_ptr<ShapeLiteral> nestedShape(const std::size_t depth) {
  auto result = std::make_shared<ShapeLiteral>();
  auto cursor = result;
  for (std::size_t index = 0; index < depth; ++index) {
    auto nested = std::make_shared<ShapeLiteral>();
    cursor->rules.push_back(ShapeRule{.rule = nested});
    cursor = std::move(nested);
  }
  return result;
}

TEST(VQLProjectionFramesTest,
     RecursiveProjectionUsesNamedFunctionFrameAndFreshLiteralCells) {
  Harness h;
  const CellRef first  = h.cell("first");
  const CellRef second = h.cell("second");
  const CellRef third  = h.cell("third");
  h.link(first, "d.next", second);
  h.link(second, "d.next", third);

  constexpr std::string_view source =
      "def pure walk($node: Cell): Cell := { "
      "$node{*, +d.score: 9007199254740993}; "
      "if $node/+d.next[1] return walk($node/+d.next[1]) "
      "else return $node; }; walk($root)";
  const auto compiled = h.compile(source, {{"root", {first}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  const auto frame = functionFrame(h, compiled, "walk");
  ASSERT_TRUE(frame) << "the projection body must be an invoked function frame";
  ASSERT_EQ(frame->descriptor.argumentPorts.size(), 2U);
  ASSERT_EQ(frame->descriptor.resultPorts.size(), 1U);
  EXPECT_EQ(frame->descriptor.resultPorts.front().kind, FramePortKind::Stream);
  EXPECT_FALSE(frame->descriptor.locals.empty());
  EXPECT_GT(std::ranges::count_if(compiled.generatedOpcodes,
                                  [&](const CellRef opcode) {
                                    return h.vm.getOpcodeKind(opcode) ==
                                           OpcodeKind::VqlMaskLinks;
                                  }),
            0);

  ASSERT_TRUE(compiled.cursor);
  const CellRef cursor = *compiled.cursor;
  const auto execution = h.vm.run(cursor, 1'000'000);
  h.vm.setCursorOpcode(cursor, std::nullopt);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  for (const CellRef node : {first, second, third}) {
    const auto score = h.field(node, "d.score");
    ASSERT_TRUE(score);
    EXPECT_EQ(h.arena.asInt64(*score),
              std::optional<std::int64_t>{INT64_C(9007199254740993)});
  }
  EXPECT_NE(h.field(first, "d.score"), h.field(second, "d.score"));
  EXPECT_NE(h.field(second, "d.score"), h.field(third, "d.score"));
  const auto results = h.vm.resultCells(compiled.resultSlots.front());
  EXPECT_EQ(results, (std::vector<CellRef>{third}));
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VQLProjectionFramesTest,
     TypedStructuralGuardFiltersMismatchesAndRollsBackInvalidProjection) {
  Harness h;
  const CellRef owner   = h.cell("owner");
  const CellRef initial = h.integer(12);
  const CellRef invalid = h.cell("not an integer");
  const CellRef valid   = h.integer(24);
  h.link(owner, "d.keep", initial);
  h.link(owner, "d.bad", invalid);
  h.link(owner, "d.good", valid);

  constexpr std::string_view invalidSource =
      "def pure replace($owner: Cell { +d.keep: Int }): Cell := { "
      "$owner{*, +d.keep: ./d.bad}; return $owner; }; replace($owner)";
  const auto compiled = h.compile(invalidSource, {{"owner", {owner}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  ASSERT_TRUE(functionFrame(h, compiled, "replace"));

  const auto rejected = h.execute(invalidSource, {{"owner", {owner}}});
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().category,
            QueryExecutionError::Category::TypeContract);
  EXPECT_NE(rejected.error().message.find("replace"), std::string::npos);
  EXPECT_EQ(h.field(owner, "d.keep"), initial);
  EXPECT_EQ(h.field(initial, "d.keep", DimVector::NEG), owner);
  EXPECT_EQ(h.field(owner, "d.bad"), invalid);
  EXPECT_EQ(h.field(invalid, "d.bad", DimVector::NEG), owner);
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);

  constexpr std::string_view validSource =
      "def pure replace($owner: Cell { +d.keep: Int }): Cell := { "
      "$owner{*, +d.keep: ./d.good}; return $owner; }; replace($owner)";
  const auto accepted = runCompiled(h, validSource, {{"owner", {owner}}});
  ASSERT_TRUE(accepted.compilation.success)
      << accepted.compilation.errorMessage;
  ASSERT_TRUE(functionFrame(h, accepted.compilation, "replace"));
  ASSERT_TRUE(accepted.executed) << accepted.error;
  EXPECT_EQ(accepted.values, (std::vector<CellRef>{owner}));
  const auto replacement = h.field(owner, "d.keep");
  ASSERT_TRUE(replacement);
  EXPECT_NE(*replacement, valid);
  EXPECT_EQ(h.arena.cloneMaster(*replacement, h.dim("d.clone")), valid);
  EXPECT_EQ(h.core.render(*replacement), h.core.render(valid));
  EXPECT_EQ(h.field(owner, "d.good"), valid);
  EXPECT_EQ(h.field(valid, "d.good", DimVector::NEG), owner);
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);

  const CellRef wrongOwner = h.cell("wrong owner");
  const CellRef wrongType  = h.cell("wrong type");
  h.link(wrongOwner, "d.keep", wrongType);
  const auto filtered = h.execute(invalidSource, {{"owner", {wrongOwner}}});
  ASSERT_TRUE(filtered) << filtered.error().message;
  EXPECT_TRUE(filtered->empty());
  EXPECT_EQ(h.field(wrongOwner, "d.keep"), wrongType);
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_FALSE(h.vm.hasActiveGuards());
}

TEST(VQLProjectionFramesTest,
     FreeAliasTargetsReachNestedFramesBeforeMaskAndKeepTypedMasterIdentity) {
  Harness h;
  const CellRef first          = h.cell("first");
  const CellRef second         = h.cell("second");
  const CellRef integerTarget  = h.integer(INT64_C(9007199254740993));
  const CellRef doubleTarget   = h.real(0x1.0000000000001p53);
  const CellRef intBacklink    = h.cell("integer backlink");
  const CellRef doubleBacklink = h.cell("double backlink");
  const CellRef oldA           = h.cell("old A");
  const CellRef oldB           = h.cell("old B");
  h.link(first, "d.old_tag", oldA);
  h.link(second, "d.old_tag", oldB);
  h.link(integerTarget, "d.annotation", intBacklink);
  h.link(doubleTarget, "d.annotation", doubleBacklink);

  constexpr std::string_view source =
      "def pure copy_int($owner: Cell): Cell := { "
      "$owner{+d.copy_int: $intTarget}; return $owner; }; "
      "def pure wrap_int($owner: Cell): Cell := { "
      "return copy_int($owner); }; "
      "def pure copy_double($owner: Cell): Cell := { "
      "$owner{*, +d.copy_double: $doubleTarget}; return $owner; }; "
      "return wrap_int($first), wrap_int($second), "
      "copy_double($first), copy_double($second)";
  ContractBindings bindings{{"first", {first}},
                            {"second", {second}},
                            {"intTarget", {integerTarget}},
                            {"doubleTarget", {doubleTarget}}};
  const auto compiled = h.compile(source, bindings);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  const auto intFrame    = functionFrame(h, compiled, "copy_int");
  const auto wrapFrame   = functionFrame(h, compiled, "wrap_int");
  const auto doubleFrame = functionFrame(h, compiled, "copy_double");
  ASSERT_TRUE(intFrame);
  ASSERT_TRUE(wrapFrame);
  ASSERT_TRUE(doubleFrame);
  EXPECT_GE(intFrame->descriptor.argumentPorts.size(), 2U);
  EXPECT_GE(wrapFrame->descriptor.argumentPorts.size(), 2U);
  EXPECT_GE(doubleFrame->descriptor.argumentPorts.size(), 2U);
  EXPECT_EQ(intFrame->descriptor.resultPorts.front().kind,
            FramePortKind::Stream);
  EXPECT_EQ(wrapFrame->descriptor.resultPorts.front().kind,
            FramePortKind::Stream);

  ASSERT_TRUE(compiled.cursor);
  const CellRef cursor = *compiled.cursor;
  const auto execution = h.vm.run(cursor, 1'000'000);
  h.vm.setCursorOpcode(cursor, std::nullopt);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  const auto results = h.vm.resultCells(compiled.resultSlots.front());
  EXPECT_EQ(results, (std::vector<CellRef>{first, second, first, second}));

  std::vector<CellRef> intCopies;
  std::vector<CellRef> doubleCopies;
  for (const CellRef owner : {first, second}) {
    const auto intCopy    = h.field(owner, "d.copy_int");
    const auto doubleCopy = h.field(owner, "d.copy_double");
    ASSERT_TRUE(intCopy);
    ASSERT_TRUE(doubleCopy);
    EXPECT_NE(*intCopy, integerTarget);
    EXPECT_NE(*doubleCopy, doubleTarget);
    EXPECT_EQ(h.arena.cloneMaster(*intCopy, h.dim("d.clone")), integerTarget);
    EXPECT_EQ(h.arena.cloneMaster(*doubleCopy, h.dim("d.clone")), doubleTarget);
    EXPECT_EQ(h.core.render(*intCopy), h.core.render(integerTarget));
    EXPECT_EQ(h.core.render(*doubleCopy), h.core.render(doubleTarget));
    EXPECT_FALSE(h.field(owner, "d.old_tag"));
    intCopies.push_back(*intCopy);
    doubleCopies.push_back(*doubleCopy);
  }
  EXPECT_NE(intCopies[0], intCopies[1]);
  EXPECT_NE(doubleCopies[0], doubleCopies[1]);
  ASSERT_EQ(std::get<std::int64_t>(*h.core.render(intCopies[0])),
            INT64_C(9007199254740993));
  const auto real = std::get<double>(*h.core.render(doubleCopies[0]));
  EXPECT_EQ(std::bit_cast<std::uint64_t>(real),
            std::bit_cast<std::uint64_t>(0x1.0000000000001p53));
  EXPECT_EQ(h.field(integerTarget, "d.annotation"), intBacklink);
  EXPECT_EQ(h.field(doubleTarget, "d.annotation"), doubleBacklink);
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VQLProjectionFramesTest,
     CallerContextAndTypedDefaultAliasAreVisibleBeforeProjectionInFrame) {
  Harness h;
  const CellRef owner    = h.cell("owner");
  const CellRef fallback = h.cell("fallback string");
  h.link(fallback, "d.annotation", h.cell("retained annotation"));

  constexpr std::string_view source =
      "def pure use_default(. : Cell { "
      "+d.old_tag: $target: String := $fallback }): Cell := { "
      ".{*, +d.copy: $target}; return .; }; use_default($owner)";
  const auto compiled =
      h.compile(source, {{"owner", {owner}}, {"fallback", {fallback}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  const auto frame = functionFrame(h, compiled, "use_default");
  ASSERT_TRUE(frame);
  EXPECT_GE(frame->descriptor.argumentPorts.size(), 2U);

  ASSERT_TRUE(compiled.cursor);
  const CellRef cursor = *compiled.cursor;
  const auto execution = h.vm.run(cursor, 1'000'000);
  h.vm.setCursorOpcode(cursor, std::nullopt);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  const auto copy = h.field(owner, "d.copy");
  ASSERT_TRUE(copy);
  EXPECT_EQ(h.arena.cloneMaster(*copy, h.dim("d.clone")), fallback);
  EXPECT_EQ(h.core.render(*copy), h.core.render(fallback));
  EXPECT_FALSE(h.field(owner, "d.old_tag"));
  EXPECT_TRUE(h.field(fallback, "d.annotation"));
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VQLProjectionFramesTest,
     RecursiveProjectedProgramExportsAndRunsAfterSourceVMIsDestroyed) {
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<UserPermascroll>(
      UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Harness source;
    const CellRef first  = source.cell("first");
    const CellRef second = source.cell("second");
    const CellRef third  = source.cell("third");
    source.link(first, "d.next", second);
    source.link(second, "d.next", third);
    source.link(source.core.home(), "d.next", first);

    VQLCompiler compiler(source.coordinator, source.vm);
    const auto compiled =
        compiler.compile("def pure walk($node: Cell): Cell := { "
                         "$node{*, +d.score: 9007199254740993}; "
                         "if $node/+d.next[1] return walk($node/+d.next[1]) "
                         "else return $node; }; walk(##/d.next[1])");
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    ASSERT_TRUE(compiled.entryOpcode);
    ASSERT_TRUE(functionFrame(source, compiled, "walk"));
    Store artifact(scroll);
    compiler.exportToStore(artifact);
    artifact.save((temporary.path / "program").string());
  }

  Store reloaded(scroll);
  ASSERT_NO_THROW(reloaded.load((temporary.path / "program").string()));
  ASSERT_TRUE(reloaded.structureHead());
  const auto manifold = reloaded.rebuildManifold(*reloaded.structureHead());
  zigzag::ArenaManifold arena(&manifold, reloaded);
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  const auto entry =
      arena.linked(core.home(), core.dims().spin, DimVector::POS);
  ASSERT_TRUE(entry);
  const CellRef cursor = vm.spawnCursor(*entry, "projection-frame-reload");
  const auto execution = vm.run(cursor, 1'000'000);
  vm.setCursorOpcode(cursor, std::nullopt);
  ASSERT_TRUE(execution.success) << execution.errorMessage;

  auto halt = *entry;
  auto next = arena.linked(halt, core.dims().spin, DimVector::POS);
  while (next && vm.getOpcodeKind(*next) != OpcodeKind::Halt) {
    halt = *next;
    next = arena.linked(halt, core.dims().spin, DimVector::POS);
  }
  ASSERT_TRUE(next);
  const auto ports = core.inputsOf(*next);
  ASSERT_EQ(ports.size(), 1U);
  const auto results = vm.resultCells(ports.front());
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(arena.textOf(results.front()), "third");
  MultiStoreCoordinator coordinator(core);
  const DimRef nextDimension  = coordinator.resolveDimension("d.next");
  const DimRef scoreDimension = coordinator.resolveDimension("d.score");
  auto node = arena.linked(core.home(), nextDimension, DimVector::POS);
  for (std::size_t index = 0; index < 3; ++index) {
    ASSERT_TRUE(node);
    const auto score = arena.linked(*node, scoreDimension, DimVector::POS);
    ASSERT_TRUE(score);
    EXPECT_EQ(arena.asInt64(*score),
              std::optional<std::int64_t>{INT64_C(9007199254740993)});
    node = arena.linked(*node, nextDimension, DimVector::POS);
  }
  vm.setCursorOpcode(cursor, std::nullopt);
  EXPECT_FALSE(vm.hasActiveFrames());
  EXPECT_FALSE(vm.hasActiveGuards());
  EXPECT_EQ(vm.choiceDepth(), 0U);
  EXPECT_EQ(arena.outstandingMarks(), 0U);
}

TEST(VQLProjectionFramesTest,
     HostAndMeshSourceRoutesProjectAndRecoverAfterTypedFailure) {
  constexpr std::string_view source =
      "def pure project($owner: Cell): Cell := { "
      "$owner{*, +d.tag: ./d.old_tag}; return $owner; }; "
      "let $owner := ##{+d.old_tag: 41} "
      "let $projected := project($owner) "
      "return $projected/+d.tag[1]";
  zigzag::vortex::VortexHost host;
  const auto hostResult = host.executeVQLChecked(source);
  ASSERT_TRUE(hostResult) << hostResult.error().message;
  ASSERT_EQ(hostResult->size(), 1U);
  EXPECT_EQ(host.core().render(hostResult->front()),
            std::optional<zigzag::vortex::CellValue>{INT64_C(41)});

  namespace mesh = zigzag::vortex::mesh;
  constexpr mesh::EvaluationBudget budget{
      .maxCycles      = 1'000'000,
      .maxCells       = 100'000,
      .maxSolutions   = 16,
      .maxOutputBytes = 1024 * 1024,
  };
  const auto meshResult =
      mesh::evaluate(mesh::Frontend::Vquery, source, budget);
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
  const auto record =
      meshArena.linked(meshResult->root, *items, DimVector::POS);
  ASSERT_TRUE(record);
  const auto output = meshArena.linked(*record, *value, DimVector::POS);
  ASSERT_TRUE(output);
  EXPECT_EQ(meshResult->context->core.render(*output),
            std::optional<zigzag::vortex::CellValue>{INT64_C(41)});

  constexpr std::string_view invalid =
      "def pure replace($owner: Cell { +d.keep: Int }): Cell := { "
      "$owner{*, +d.keep: \"bad\"}; return $owner; }; "
      "let $owner := ##{+d.keep: 12} return replace($owner)";
  const auto rejected = host.executeVQLChecked(invalid);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().category,
            QueryExecutionError::Category::TypeContract);
  EXPECT_NE(rejected.error().message.find("replace"), std::string::npos);

  constexpr std::string_view recovered =
      "def pure replace($owner: Cell { +d.keep: Int }): Cell := { "
      "$owner{*, +d.keep: 24}; return $owner; }; "
      "let $owner := ##{+d.keep: 12} return replace($owner)/+d.keep[1]";
  const auto recoveredResult = host.executeVQLChecked(recovered);
  ASSERT_TRUE(recoveredResult) << recoveredResult.error().message;
  ASSERT_EQ(recoveredResult->size(), 1U);
  EXPECT_EQ(host.core().render(recoveredResult->front()),
            std::optional<zigzag::vortex::CellValue>{INT64_C(24)});
  EXPECT_FALSE(host.vm().hasActiveFrames());
  EXPECT_FALSE(host.vm().hasActiveGuards());
}

TEST(VQLProjectionFramesTest,
     UnsupportedProjectionFormsStayInlineOnlyAndPropagateToCallers) {
  constexpr std::string_view source =
      "def pure dynamic($owner: Cell): Cell := "
      "$owner{+d.copy: string(\"unsupported\")}; "
      "def pure predicate($owner: Cell): Cell := "
      "$owner{+d.copy: ./d.source[d.flag]}; "
      "def pure wrapper($owner: Cell): Cell := dynamic($owner); "
      "wrapper(##)";
  auto query          = Parser(source).parseQuery();
  const auto analysis = analyzeFrameEligibility(query);
  ASSERT_TRUE(analysis) << analysis.error().message;
  EXPECT_FALSE(analysis->reusableDefinitions.contains("dynamic"));
  EXPECT_FALSE(analysis->reusableDefinitions.contains("predicate"));
  EXPECT_FALSE(analysis->reusableDefinitions.contains("wrapper"));
  EXPECT_NE(analysis->refusalReasons.at("wrapper").find("inline-only"),
            std::string::npos);

  for (const ShapeLiteral &shape :
       {ShapeLiteral{},
        ShapeLiteral{
            .rules = {ShapeRule{.rule = ShapeWildcard::Everything}}}}) {
    auto standalone =
        Parser("def pure value(): Cell := ##; value()").parseQuery();
    standalone.defs.front().body = std::make_shared<ValueExpr>(ValueExpr{
        .kind = std::make_shared<ShapeLiteral>(shape),
    });
    const auto shapeAnalysis     = analyzeFrameEligibility(standalone);
    ASSERT_TRUE(shapeAnalysis) << shapeAnalysis.error().message;
    EXPECT_FALSE(shapeAnalysis->reusableDefinitions.contains("value"));
  }

  auto shapeReference = Parser("def pure use_shape($owner: Cell): Cell := "
                               "$owner{$shape}; use_shape(##)")
                            .parseQuery();
  const auto shapeRefAnalysis = analyzeFrameEligibility(shapeReference);
  ASSERT_TRUE(shapeRefAnalysis) << shapeRefAnalysis.error().message;
  EXPECT_FALSE(shapeRefAnalysis->reusableDefinitions.contains("use_shape"));
}

TEST(VQLProjectionFramesTest,
     ProjectionEligibilityKeepsMalformedAstDepthAndNodeBudgetsHardErrors) {
  auto cycleQuery =
      Parser("def pure value(): Cell := ##; value()").parseQuery();
  auto cyclicShape = std::make_shared<ShapeLiteral>();
  cyclicShape->rules.push_back(ShapeRule{.rule = cyclicShape});
  cycleQuery.defs.front().body = std::make_shared<ValueExpr>(ValueExpr{
      .kind = cyclicShape,
  });
  const auto cyclic            = analyzeFrameEligibility(cycleQuery);
  cyclicShape->rules.clear();
  ASSERT_FALSE(cyclic);
  EXPECT_NE(cyclic.error().message.find("cyclic"), std::string::npos);

  auto projectionCycleQuery =
      Parser("def pure project($owner: Cell): Cell := $owner{*}; project(##)")
          .parseQuery();
  auto &projectionBody = *std::get<std::shared_ptr<ValueExpr>>(
      projectionCycleQuery.defs.front().body);
  auto projectionPath =
      std::get<std::shared_ptr<PathExpression>>(projectionBody.kind);
  projectionPath->shapeProjection->rules.push_back(ShapeRule{
      .rule = DimensionShapeRule{.dimName     = "d.cycle",
                                 .targetValue = std::make_shared<ValueExpr>(
                                     ValueExpr{.kind = projectionPath})}});
  const auto projectionCycle = analyzeFrameEligibility(projectionCycleQuery);
  projectionPath->shapeProjection->rules.pop_back();
  ASSERT_FALSE(projectionCycle);
  EXPECT_NE(projectionCycle.error().message.find("cyclic"), std::string::npos);

  auto depthQuery =
      Parser("def pure value(): Cell := ##; value()").parseQuery();
  depthQuery.defs.front().body = std::make_shared<ValueExpr>(ValueExpr{
      .kind = nestedShape(32),
  });
  const auto deep =
      analyzeFrameEligibility(depthQuery, {.maxNodes = 100'000, .maxDepth = 8});
  ASSERT_FALSE(deep);
  EXPECT_NE(deep.error().message.find("depth budget"), std::string::npos);

  auto largeQuery = Parser("def pure project($owner: Cell): Cell := $owner{*}; "
                           "project(##)")
                        .parseQuery();
  auto *body =
      std::get_if<std::shared_ptr<ValueExpr>>(&largeQuery.defs.front().body);
  ASSERT_TRUE(body);
  ASSERT_TRUE(*body);
  auto *path = std::get_if<std::shared_ptr<PathExpression>>(&(*body)->kind);
  ASSERT_TRUE(path);
  ASSERT_TRUE(*path);
  ASSERT_TRUE((*path)->shapeProjection);
  for (std::size_t index = 0; index < 128; ++index) {
    DimensionShapeRule rule;
    rule.dimName     = "d.projection." + std::to_string(index);
    rule.targetValue = scalarValue(static_cast<std::int64_t>(index));
    (*path)->shapeProjection->rules.push_back(
        ShapeRule{.rule = std::move(rule)});
  }
  const auto overBudget =
      analyzeFrameEligibility(largeQuery, {.maxNodes = 80, .maxDepth = 256});
  ASSERT_FALSE(overBudget);
  EXPECT_NE(overBudget.error().message.find("node budget"), std::string::npos);
}

} // namespace
} // namespace xanadu::vql

#include <gtest/gtest.h>

#include <bit>
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
#include "common/xanadu/scalar.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/mesh_evaluator.hpp"
#include "common/xanadu/vortex/vortex_host.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/query_clone_master.hpp"
#include "common/xanadu/zigzag/query_scalar.hpp"

namespace xanadu::vql {
namespace {
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;
using Result = std::expected<std::vector<CellRef>, std::string>;
struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};
  CompilationResult compiled;

  CellRef cell(const std::string_view text) { return arena.makeCell(text); }

  CellRef integer(const std::int64_t value) {
    return arena.makeScalarCell(value);
  }

  void link(const CellRef from, const std::string_view dimension,
            const CellRef to) {
    zigzag::expectWritten(arena.link(
        from, coordinator.resolveDimension(dimension), DimVector::POS, to));
  }

  Result
  run(const std::string_view source,
      std::unordered_map<std::string, std::vector<CellRef>> bindings = {}) {
    VQLCompiler compiler(coordinator, vm);
    compiled =
        compiler.compile(source, {.initialBindings = std::move(bindings)});
    if (!compiled.success) return std::unexpected(compiled.errorMessage);
    if (!compiled.cursor)
      return std::unexpected("compiled query has no cursor");
    const auto execution = vm.run(*compiled.cursor, 100'000);
    if (!execution.success) {
      vm.abortScopedChoices(*compiled.cursor);
      vm.setCursorOpcode(*compiled.cursor, std::nullopt);
      return std::unexpected(execution.errorMessage);
    }
    std::vector<CellRef> result;
    for (const CellRef slot : compiled.resultSlots) {
      const auto values = vm.resultCells(slot);
      result.insert(result.end(), values.begin(), values.end());
    }
    vm.setCursorOpcode(*compiled.cursor, std::nullopt);
    return result;
  }

  std::size_t count(const OpcodeKind kind) const {
    std::size_t result = 0;
    for (const CellRef opcode : compiled.generatedOpcodes)
      result += vm.getOpcodeKind(opcode) == kind;
    return result;
  }
};

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_computed_clock_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

constexpr std::string_view scoreBody   = R"(
  def pure score_paper($p: Cell { +d.title: String, +d.score: Int }): Int :=
    $p/+d.score;
)";
constexpr std::string_view titleBody   = R"(
  def pure format_title({ ., +d.title: $t, +d.tag: $tag := "note" }): String :=
    "[" + $tag + "] " + $t;
)";
constexpr std::string_view videoBody   = R"(
  def pure process_video({ ., +d.codec: $c: String, +d.fps: $fps: Int := 60 }): String :=
    $c + " @ " + string($fps) + "fps";
)";
constexpr std::string_view archiveBody = R"(
  def weave archive_task($task: Cell { +d.priority: $p: String, +d.owner: $o: Cell }): Cell := {
    weave $task {
      *,
      +d.status => "archived",
      +d.archived_at => @now
    };
    return $task;
  };
)";

std::string archiveDefinition(const bool keep) {
  std::string result(archiveBody);
  if (!keep) result.erase(result.find("*,"), 2);
  return result;
}

std::optional<CellRef> field(Harness &h, const CellRef owner,
                             const std::string_view name) {
  return h.arena.linked(owner, h.coordinator.resolveDimension(name),
                        DimVector::POS);
}

std::size_t timestamps(const zigzag::ArenaManifold &arena) {
  std::size_t result = 0;
  for (const auto &cell : arena.cells())
    result += arena.valueKindOf(cell.birthOp) == ValueKind::Timestamp;
  return result;
}

std::optional<std::int64_t> checkTimestamp(zigzag::ArenaManifold &arena,
                                           zigzag::vortex::VortexCore &core,
                                           const CellRef cell) {
  const auto master = zigzag::queryCloneMaster(arena, cell, core.dims().clone);
  if (!master) {
    ADD_FAILURE() << "missing timestamp master";
    return std::nullopt;
  }
  EXPECT_EQ(arena.valueKindOf(*master), ValueKind::Timestamp);
  const auto slot = arena.slot(*master);
  if (!slot) {
    ADD_FAILURE() << "missing timestamp slot";
    return std::nullopt;
  }
  const auto nanos = std::bit_cast<std::int64_t>(slot->valueBits);
  const auto text  = formatUtcTimestampIso8601(nanos);
  EXPECT_EQ(arena.textOf(*master), text);
  TimestampInstant parsed;
  EXPECT_TRUE(parseUtcTimestampIso8601(text, parsed));
  EXPECT_EQ(parsed.epochNanos, nanos);
  const auto scalar = zigzag::readQueryScalar(
      arena, cell, core.dims().clone, core.dims().runtimeObjectClaim.value());
  if (!scalar) {
    ADD_FAILURE() << "unreadable timestamp scalar";
    return std::nullopt;
  }
  EXPECT_TRUE(std::holds_alternative<std::string>(*scalar));
  if (const auto *value = std::get_if<std::string>(&*scalar))
    EXPECT_EQ(*value, text);
  EXPECT_FALSE(arena.asInt64(*master));
  return nanos;
}

struct Task {
  CellRef cell;
  CellRef priority;
  CellRef owner;
};
Task task(Harness &h) {
  const Task result{h.cell("task"), h.cell("high"), h.cell("owner")};
  h.link(result.cell, "d.priority", result.priority);
  h.link(result.cell, "d.owner", result.owner);
  return result;
}

TEST(VQLComputedTargetsClockTest,
     DocumentedPureTypeBodyReturnsItsOriginalScore) {
  Harness h;
  const auto paper = h.cell("paper");
  const auto score = h.integer(95);
  h.link(paper, "d.title", h.cell("Example"));
  h.link(paper, "d.score", score);
  const auto result =
      h.run(std::string(scoreBody) + "return (score_paper($paper))",
            {{"paper", {paper}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{score}));
  EXPECT_GT(h.count(OpcodeKind::InvokeFrame), 0u);
}

TEST(VQLComputedTargetsClockTest,
     DocumentedDestructureBodyUsesItsLiteralDefault) {
  Harness h;
  const auto paper = h.cell("paper");
  h.link(paper, "d.title", h.cell("Example"));
  const auto result =
      h.run(std::string(titleBody) + "return (format_title($paper))",
            {{"paper", {paper}}});
  ASSERT_TRUE(result) << result.error();
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ(h.arena.textOf(result->front()), "[note] Example");
  EXPECT_GT(h.count(OpcodeKind::InvokeFrame), 0u);
}

TEST(VQLComputedTargetsClockTest,
     DocumentedCombinedBodyFormatsDefaultAndActualFps) {
  Harness h;
  const auto video = h.cell("video");
  h.link(video, "d.codec", h.cell("av1"));
  const auto first =
      h.run(std::string(videoBody) + "return (process_video($video))",
            {{"video", {video}}});
  ASSERT_TRUE(first) << first.error();
  ASSERT_EQ(first->size(), 1u);
  EXPECT_EQ(h.arena.textOf(first->front()), "av1 @ 60fps");
  h.link(video, "d.fps", h.integer(24));
  const auto second =
      h.run(std::string(videoBody) + "return (process_video($video))",
            {{"video", {video}}});
  ASSERT_TRUE(second) << second.error();
  ASSERT_EQ(second->size(), 1u);
  EXPECT_EQ(h.arena.textOf(second->front()), "av1 @ 24fps");
}

TEST(VQLComputedTargetsClockTest,
     ArchiveBodyPreservesContractAndCreatesUtcTimestamp) {
  Harness h;
  const auto data = task(h);
  const auto result =
      h.run(archiveDefinition(true) + "return (archive_task($task))",
            {{"task", {data.cell}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{data.cell}));
  EXPECT_EQ(field(h, data.cell, "d.priority"), data.priority);
  EXPECT_EQ(field(h, data.cell, "d.owner"), data.owner);
  const auto status = field(h, data.cell, "d.status");
  ASSERT_TRUE(status);
  EXPECT_EQ(h.arena.textOf(*status), "archived");
  const auto stamp = field(h, data.cell, "d.archived_at");
  ASSERT_TRUE(stamp);
  EXPECT_TRUE(checkTimestamp(h.arena, h.core, *stamp));
  EXPECT_GT(h.count(OpcodeKind::CurrentTimestamp), 0u);
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_EQ(h.arena.outstandingMarks(), 0u);
}

TEST(VQLComputedTargetsClockTest,
     OriginalExclusiveArchiveRefusesAtomicallyAndCanRetry) {
  Harness h;
  const auto data     = task(h);
  const auto pending  = h.cell("pending");
  const auto prior    = scalarTimestamp(42);
  const auto oldStamp = h.cell(prior.text);
  zigzag::expectWritten(h.arena.setValueBits(oldStamp, prior.kind, prior.bits));
  h.link(data.cell, "d.status", pending);
  h.link(data.cell, "d.archived_at", oldStamp);
  const auto before = timestamps(h.arena);
  const auto failed =
      h.run(archiveDefinition(false) + "return (archive_task($task))",
            {{"task", {data.cell}}});
  ASSERT_FALSE(failed);
  EXPECT_NE(failed.error().find("archive_task"), std::string::npos);
  EXPECT_EQ(field(h, data.cell, "d.priority"), data.priority);
  EXPECT_EQ(field(h, data.cell, "d.owner"), data.owner);
  EXPECT_EQ(field(h, data.cell, "d.status"), pending);
  EXPECT_EQ(field(h, data.cell, "d.archived_at"), oldStamp);
  EXPECT_EQ(timestamps(h.arena), before);
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_EQ(h.arena.outstandingMarks(), 0u);
  const auto retried =
      h.run(archiveDefinition(true) + "return (archive_task($task))",
            {{"task", {data.cell}}});
  ASSERT_TRUE(retried) << retried.error();
  EXPECT_EQ(*retried, (std::vector<CellRef>{data.cell}));
}

TEST(VQLComputedTargetsClockTest, ParameterMismatchSkipsClockAndWeaveBody) {
  Harness h;
  const auto data = task(h);
  const auto bad  = h.integer(3);
  h.link(data.cell, "d.priority", bad);
  const auto before = timestamps(h.arena);
  const auto result =
      h.run(archiveDefinition(true) + "return (archive_task($task))",
            {{"task", {data.cell}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_TRUE(result->empty());
  EXPECT_EQ(field(h, data.cell, "d.priority"), bad);
  EXPECT_FALSE(field(h, data.cell, "d.status"));
  EXPECT_EQ(timestamps(h.arena), before);
}

TEST(VQLComputedTargetsClockTest,
     ReservedLiteralIsRuntimeDataAndCannotBeShadowed) {
  Harness h;
  constexpr std::string_view source = R"(
    def pure now(): Int := 7;
    def pure __vql_current_timestamp(): Int := 9;
    return (@now), (now()), (__vql_current_timestamp())
  )";
  VQLCompiler compiler(h.coordinator, h.vm);
  const auto compiled = compiler.compile(source);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_EQ(timestamps(h.arena), 0u);
  ASSERT_TRUE(compiled.cursor);
  const auto execution = h.vm.run(*compiled.cursor, 100'000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  std::vector<CellRef> values;
  for (const auto output : compiled.resultSlots) {
    const auto members = h.vm.resultCells(output);
    values.insert(values.end(), members.begin(), members.end());
  }
  ASSERT_EQ(values.size(), 3u);
  EXPECT_TRUE(checkTimestamp(h.arena, h.core, values[0]));
  EXPECT_EQ(h.arena.asInt64(values[1]), 7);
  EXPECT_EQ(h.arena.asInt64(values[2]), 9);
  h.vm.setCursorOpcode(*compiled.cursor, std::nullopt);
}

TEST(VQLComputedTargetsClockTest,
     ForgedReservedDefinitionRefusesBeforeEffects) {
  Harness h;
  const std::string source = "def pure user(): Int := 7; return (user())";
  Parser parser(source);
  auto query              = parser.parseQuery();
  query.defs.front().name = "#vql-current-timestamp";
  const auto before       = h.arena.cellCount();
  VQLCompiler compiler(h.coordinator, h.vm);
  const auto compiled = compiler.compile(query);
  EXPECT_FALSE(compiled.success);
  EXPECT_NE(compiled.errorMessage.find("reserved"), std::string::npos);
  EXPECT_EQ(h.arena.cellCount(), before);
}

TEST(VQLComputedTargetsClockTest,
     RepeatedOwnersReceiveFreshRuntimeTimestampMasters) {
  Harness h;
  const auto first  = h.cell("first");
  const auto second = h.cell("second");
  const auto result =
      h.run("$owners{*, +d.stamp: @now}", {{"owners", {first, second, first}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{first, second, first}));
  const auto a = field(h, first, "d.stamp");
  const auto b = field(h, second, "d.stamp");
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  const auto aMaster =
      zigzag::queryCloneMaster(h.arena, *a, h.core.dims().clone);
  const auto bMaster =
      zigzag::queryCloneMaster(h.arena, *b, h.core.dims().clone);
  EXPECT_NE(aMaster, bMaster);
  EXPECT_GE(timestamps(h.arena), 3u);
  EXPECT_TRUE(checkTimestamp(h.arena, h.core, *a));
  EXPECT_TRUE(checkTimestamp(h.arena, h.core, *b));
}

TEST(VQLComputedTargetsClockTest,
     PureRecursiveHelperDefaultsUseEachOwnersContext) {
  Harness h;
  const auto first  = h.cell("first");
  const auto second = h.cell("second");
  h.link(first, "d.seed", h.integer(1));
  h.link(second, "d.seed", h.integer(7));
  const auto result = h.run(R"(
    def pure descend($n: Int, $value: Int): Int := {
      if $n > 0 return descend($n + -1, $value + 1) else return $value;
    };
    def pure compute($seed: Int := ./+d.seed[1]): Int := descend(2, $seed);
    $owners{*, +d.result: compute()}
  )",
                            {{"owners", {first, second, first}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{first, second, first}));
  const auto a = field(h, first, "d.result");
  const auto b = field(h, second, "d.result");
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  const auto aMaster =
      zigzag::queryCloneMaster(h.arena, *a, h.core.dims().clone);
  const auto bMaster =
      zigzag::queryCloneMaster(h.arena, *b, h.core.dims().clone);
  ASSERT_TRUE(aMaster);
  ASSERT_TRUE(bMaster);
  EXPECT_EQ(h.arena.asInt64(*aMaster), 3);
  EXPECT_EQ(h.arena.asInt64(*bMaster), 9);
  EXPECT_GT(h.count(OpcodeKind::InvokeFrame), 0u);
}

TEST(VQLComputedTargetsClockTest,
     TargetCardinalityRefusesBeforeMaskAndRollsBackClock) {
  for (const bool multiple : {false, true}) {
    Harness h;
    const auto owner  = h.cell("owner");
    const auto keep   = h.cell("keep");
    const auto old    = h.cell("old");
    const auto target = h.cell("target");
    h.link(owner, "d.keep", keep);
    h.link(owner, "d.result", old);
    const auto before        = timestamps(h.arena);
    const std::string source = R"(
      def pure pick(): Stream<Cell> := $targets;
      $owner{+d.stamp: @now, +d.result: pick()}
    )";
    std::vector<CellRef> targets;
    if (multiple) targets = {target, target};
    const auto failed =
        h.run(source, {{"owner", {owner}}, {"targets", targets}});
    ASSERT_FALSE(failed);
    EXPECT_EQ(field(h, owner, "d.keep"), keep);
    EXPECT_EQ(field(h, owner, "d.result"), old);
    EXPECT_FALSE(field(h, owner, "d.stamp"));
    EXPECT_EQ(timestamps(h.arena), before);
    EXPECT_FALSE(h.vm.hasActiveFrames());
    EXPECT_FALSE(h.vm.hasActiveGuards());
    EXPECT_EQ(h.arena.outstandingMarks(), 0u);
    const auto retry =
        h.run(source, {{"owner", {owner}}, {"targets", {target}}});
    ASSERT_TRUE(retry) << retry.error();
    EXPECT_EQ(*retry, (std::vector<CellRef>{owner}));
  }
}

TEST(VQLComputedTargetsClockTest, ImpureAndIndirectHelpersRefuseBeforeEffects) {
  for (const std::string_view call :
       {"def pure bad($c: Cell): Cell := $c/value(9); $owner{*, +d.stamp: "
        "@now, +d.result: bad($victim)}",
        "def weave bad($c: Cell): Cell := $c/value(9); $owner{*, +d.result: "
        "bad($victim)}",
        "def pure invoke($f, $c: Cell): Cell := $c/$f; let $f := \\($x) $x; "
        "$owner{*, +d.result: invoke($f, $victim)}"}) {
    Harness h;
    const auto owner  = h.cell("owner");
    const auto victim = h.integer(3);
    const auto keep   = h.cell("keep");
    h.link(owner, "d.keep", keep);
    const auto before = h.arena.cellCount();
    const auto failed = h.run(call, {{"owner", {owner}}, {"victim", {victim}}});
    EXPECT_FALSE(failed) << call;
    EXPECT_EQ(field(h, owner, "d.keep"), keep);
    EXPECT_EQ(h.arena.asInt64(victim), 3);
    EXPECT_EQ(timestamps(h.arena), 0u);
    EXPECT_EQ(h.arena.cellCount(), before);
  }
}

TEST(VQLComputedTargetsClockTest,
     PureHelperCannotHideAnAttachedCapturePipeline) {
  Harness h;
  const auto owner   = h.cell("owner");
  const auto outside = h.cell("outside");
  const auto keep    = h.cell("keep");
  h.link(owner, "d.keep", keep);
  h.core.attachPipeline(outside, h.vm.mintOpcode(OpcodeKind::Halt));
  const auto failed = h.run(R"(
    def pure helper(): Cell := $outside;
    $owner{+d.stamp: @now, +d.result: helper()}
  )",
                            {{"owner", {owner}}, {"outside", {outside}}});
  ASSERT_FALSE(failed);
  EXPECT_NE(failed.error().find("pipeline"), std::string::npos);
  EXPECT_EQ(field(h, owner, "d.keep"), keep);
  EXPECT_EQ(timestamps(h.arena), 0u);
}

TEST(VQLComputedTargetsClockTest,
     HelperReturnPromiseFailureRollsBackPreparedTimestamp) {
  Harness h;
  const auto owner = h.cell("owner");
  const auto keep  = h.cell("keep");
  h.link(owner, "d.keep", keep);
  const auto failed = h.run(R"(
    def pure wrong(): Int := "bad";
    $owner{+d.stamp: @now, +d.result: wrong()}
  )",
                            {{"owner", {owner}}});
  ASSERT_FALSE(failed);
  EXPECT_NE(failed.error().find("wrong"), std::string::npos);
  EXPECT_EQ(field(h, owner, "d.keep"), keep);
  EXPECT_FALSE(field(h, owner, "d.stamp"));
  EXPECT_EQ(timestamps(h.arena), 0u);
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_EQ(h.arena.outstandingMarks(), 0u);
}

TEST(VQLComputedTargetsClockTest,
     ArchiveGraphSurvivesAstFreeExportHostAndMesh) {
  const std::string source = archiveDefinition(true) + R"(
    let $task := %"task";
    $task/d.priority%"high";
    $task/d.owner%"owner";
    return archive_task($task)/+d.archived_at
  )";
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<UserPermascroll>(
      UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Harness h;
    VQLCompiler compiler(h.coordinator, h.vm);
    Parser parser(source);
    auto query          = parser.parseQuery();
    const auto compiled = compiler.compile(query);
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    EXPECT_EQ(timestamps(h.arena), 0u);
    query = QueryExpression{};
    Store store(scroll);
    compiler.exportToStore(store);
    store.save((temporary.path / "program").string());
  }
  Store store(scroll);
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
      vm.run(vm.spawnCursor(*entry, "archive-clock-reload"), 100'000);
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
  const auto outputs = core.inputsOf(*halt);
  ASSERT_EQ(outputs.size(), 1u);
  const auto values = vm.resultCells(outputs.front());
  ASSERT_EQ(values.size(), 1u);
  EXPECT_TRUE(checkTimestamp(arena, core, values.front()));

  zigzag::vortex::VortexHost host;
  const auto hosted = host.executeVQLChecked(source);
  ASSERT_TRUE(hosted) << hosted.error().message;
  ASSERT_EQ(hosted->size(), 1u);
  EXPECT_TRUE(
      checkTimestamp(host.core().arena(), host.core(), hosted->front()));

  namespace mesh    = zigzag::vortex::mesh;
  const auto meshed = mesh::evaluate(mesh::Frontend::Vquery, source,
                                     {.maxCycles      = 100'000,
                                      .maxCells       = 100'000,
                                      .maxSolutions   = 16,
                                      .maxOutputBytes = 1024 * 1024});
  ASSERT_TRUE(meshed) << meshed.error();
  auto &meshArena = meshed->context->arena;
  std::optional<CellRef> itemsDimension;
  std::optional<CellRef> valueDimension;
  for (const auto &cell : meshArena.cells()) {
    if (meshArena.textOf(cell.birthOp) == "d.mesh.items")
      itemsDimension = cell.birthOp;
    if (meshArena.textOf(cell.birthOp) == "d.mesh.value")
      valueDimension = cell.birthOp;
  }
  ASSERT_TRUE(itemsDimension);
  ASSERT_TRUE(valueDimension);
  const auto record =
      meshArena.linked(meshed->root, *itemsDimension, DimVector::POS);
  ASSERT_TRUE(record);
  const auto result =
      meshArena.linked(*record, *valueDimension, DimVector::POS);
  ASSERT_TRUE(result);
  EXPECT_TRUE(checkTimestamp(meshArena, meshed->context->core, *result));
}

TEST(VQLComputedTargetsClockTest, FailedArchiveAppendsNoSourceStoreOperations) {
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<UserPermascroll>(
      UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  Store store(scroll);
  auto version           = store.sliceGenesis(MicroversionId{});
  const auto priorityDim = store.makeDimension(version, "d.priority");
  version                = priorityDim.version;
  const auto ownerDim    = store.makeDimension(version, "d.owner");
  version                = ownerDim.version;
  version                = store.makeCell(version, "task");
  const auto taskCell    = store.cellRefOf(version).value();
  version                = store.makeCell(version, "high");
  const auto priority    = store.cellRefOf(version).value();
  version                = store.makeCell(version, "owner");
  const auto owner       = store.cellRefOf(version).value();
  version = store.setLink(version, taskCell, priorityDim.dim, false, priority);
  version = store.setLink(version, taskCell, ownerDim.dim, false, owner);
  const auto manifold = store.rebuildManifold(version);
  zigzag::ArenaManifold arena(&manifold, store);
  zigzag::vortex::VortexCore core(arena);
  MultiStoreCoordinator coordinator(core);
  zigzag::vortex::VortexVM vm(core);
  const auto before = store.opCount();
  QueryExecutor executor(coordinator, vm, {}, {{"task", {taskCell}}});
  const auto failed = executor.execute(archiveDefinition(false) +
                                       "return (archive_task($task))");
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().category,
            QueryExecutionError::Category::TypeContract);
  EXPECT_EQ(store.opCount(), before);
  EXPECT_EQ(arena.linked(taskCell, priorityDim.dim, DimVector::POS), priority);
  EXPECT_EQ(arena.linked(taskCell, ownerDim.dim, DimVector::POS), owner);
  EXPECT_EQ(timestamps(arena), 0u);
  EXPECT_FALSE(vm.hasActiveGuards());
  EXPECT_FALSE(vm.hasActiveFrames());
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

} // namespace
} // namespace xanadu::vql

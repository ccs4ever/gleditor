/**
 * @file vortex_cell_int_test.cpp
 * @brief CellOfInt / IntOfCell: the two pure opcodes that convert between an
 *        Int64 cell number and the cell it names, and every place a new
 *        opcode must be known (decode ceiling, purity, name tables, stdlib,
 *        export, vagent).
 *
 * A cell number held as an Int64 is not protected against reuse after
 * release(); ReleasedNumberNamesALaterCell records that hazard as behaviour
 * so the release-epoch card has a test to turn.
 */
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vagent/meta.hpp"
#include "common/xanadu/vortex/mesh_evaluator.hpp"
#include "common/xanadu/vortex/mesh_result.hpp"
#include "common/xanadu/vortex/program_export.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vpl/compiler.hpp"
#include "common/xanadu/vql/compiler.hpp"

namespace zigzag::vortex {
namespace {

struct Harness {
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};

  Harness() = default;
  explicit Harness(const Manifold *base) : arena{base}, core{arena}, vm{core} {}

  CellRef convert(const OpcodeKind kind, const CellRef in, const CellRef out) {
    const auto opcode = vm.mintOpcode(kind);
    core.bindInput(opcode, in);
    core.bindOutput(opcode, out);
    return opcode;
  }

  ExecutionResult run(const OpcodeKind kind, const CellRef in,
                      const CellRef out) {
    return vm.step(vm.spawnCursor(convert(kind, in, out)));
  }

  // IntOfCell then CellOfInt over @p cell; the final reference slot.
  std::optional<CellRef> roundTrip(const CellRef cell) {
    const auto number = arena.makeCell();
    const auto back   = arena.makeCell();
    EXPECT_TRUE(run(OpcodeKind::IntOfCell, cell, number).success);
    EXPECT_EQ(arena.asInt64(number),
              std::optional{static_cast<std::int64_t>(cell)});
    EXPECT_TRUE(run(OpcodeKind::CellOfInt, number, back).success);
    return vm.cellReferenceOf(back);
  }
};

TEST(VortexCellIntTest, RoundTripReturnsTheSameCellForCellsAndDimensions) {
  Harness h;
  const auto plain = h.arena.makeCell("plain");
  EXPECT_TRUE(isEphemeral(plain));
  EXPECT_EQ(h.roundTrip(plain), plain);
  const auto dimension = h.core.findOrMintDimension("d.cell-int");
  EXPECT_EQ(h.roundTrip(dimension), dimension);
}

TEST(VortexCellIntTest, RoundTripReturnsADurableBaseCell) {
  xanadu::UserPermascroll::Config config;
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("vortex_cell_int_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(directory);
  config.storageDir = directory;
  {
    xanadu::Store store{std::make_shared<xanadu::UserPermascroll>(config)};
    auto version       = store.sliceGenesis(xanadu::MicroversionId{});
    const auto base    = store.rebuildManifold(store.latest());
    const auto durable = store.homeCell().value();
    EXPECT_FALSE(isEphemeral(durable));
    Harness h{&base};
    EXPECT_EQ(h.roundTrip(durable), durable);
    static_cast<void>(version);
  }
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

TEST(VortexCellIntTest, ComputedNumberNamesItsCell) {
  Harness h;
  const auto target = h.arena.makeCell("target");
  const auto offset = h.arena.makeScalarCell(std::int64_t{3});
  const auto base =
      h.arena.makeScalarCell(static_cast<std::int64_t>(target) - 3);
  const auto sum = h.arena.makeCell();
  const auto add = h.vm.mintOpcode(OpcodeKind::Add);
  h.core.bindInput(add, base);
  h.core.bindInput(add, offset);
  h.core.bindOutput(add, sum);
  ASSERT_TRUE(h.vm.step(h.vm.spawnCursor(add)).success);
  const auto out = h.arena.makeCell();
  ASSERT_TRUE(h.run(OpcodeKind::CellOfInt, sum, out).success);
  EXPECT_EQ(h.vm.cellReferenceOf(out), target);
}

// The V0 use case: an Int64 box names a source, the box is unwrapped to the
// cell, and RelationNext is walked over it.
TEST(VortexCellIntTest, AnInt64BoxNamingASourceDrivesARelationWalk) {
  Harness h;
  h.vm.setRelationProvider(
      "src", [](const RelationQuery &query) -> RelationResult {
        if (query.position != 0 || query.arguments.size() != 1)
          return std::optional<RelationAnswer>{};
        return std::optional<RelationAnswer>{
            RelationAnswer{.nextPosition = 1,
                           .limit        = 1,
                           .answers      = {query.arguments.front()}}};
      });
  const auto source = h.arena.makeCell("source");
  const auto box    = h.arena.makeScalarCell(static_cast<std::int64_t>(source));
  const auto unwrapped = h.arena.makeCell();
  ASSERT_TRUE(h.run(OpcodeKind::CellOfInt, box, unwrapped).success);
  ASSERT_EQ(h.vm.cellReferenceOf(unwrapped), source);

  const auto next = h.vm.mintOpcode(OpcodeKind::RelationNext);
  h.core.bindInput(next, h.arena.makeCell("src"));
  h.core.bindInput(next, h.arena.makeScalarCell(std::int64_t{0}));
  h.core.bindInput(next, h.arena.makeScalarCell(std::int64_t{0}));
  h.core.bindInput(next, unwrapped);
  const auto found = h.arena.makeCell();
  h.core.bindOutput(next, found);
  h.core.bindOutput(next, h.arena.makeCell());
  h.core.bindOutput(next, h.arena.makeCell());
  const auto answer = h.arena.makeCell();
  h.core.bindOutput(next, answer);
  ASSERT_TRUE(h.vm.step(h.vm.spawnCursor(next)).success);
  EXPECT_EQ(h.vm.cellReferenceOf(answer), source);
}

TEST(VortexCellIntTest, AnAbsentNumberIsNoCellWithoutAFault) {
  Harness h;
  h.arena.makeCell("only");
  for (const auto absent :
       {static_cast<std::int64_t>(noCell),
        static_cast<std::int64_t>(ArenaManifold::refOf(4'000'000)),
        std::int64_t{0}}) {
    const auto out    = h.arena.makeCell();
    const auto in     = h.arena.makeScalarCell(absent);
    const auto result = h.run(OpcodeKind::CellOfInt, in, out);
    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_FALSE(h.vm.cellReferenceOf(out));
    EXPECT_EQ(h.arena.asInt64(out),
              std::optional{static_cast<std::int64_t>(noCell)});
  }
}

TEST(VortexCellIntTest, CellOfIntRefusesByNameAndLeavesTheOutputAlone) {
  const auto refused = [](const std::function<CellRef(Harness &)> &operand,
                          const std::string &needle) {
    Harness h;
    const auto in     = operand(h);
    const auto out    = h.arena.makeScalarCell(true);
    const auto result = h.run(OpcodeKind::CellOfInt, in, out);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find(needle), std::string::npos)
        << result.errorMessage;
    EXPECT_EQ(h.arena.asBool(out), std::optional{true});
    EXPECT_FALSE(h.vm.cellReferenceOf(out));
  };
  refused([](Harness &h) { return h.arena.makeCell("text"); },
          "CellOfInt operand must be an Int64");
  refused([](Harness &h) { return h.arena.makeScalarCell(2.5); },
          "CellOfInt operand must be an Int64");
  refused([](Harness &h) { return h.arena.makeScalarCell(true); },
          "CellOfInt operand must be an Int64");
  refused([](Harness &h) { return h.arena.makeScalarCell(std::int64_t{-1}); },
          "CellOfInt operand is negative");
  refused(
      [](Harness &h) { return h.arena.makeScalarCell(std::int64_t{1} << 40); },
      "CellOfInt operand is outside the cell number range");
}

TEST(VortexCellIntTest, IntOfCellRefusesAnAbsentOrTypedOperandByName) {
  {
    Harness h;
    const auto absent = h.arena.makeCell();
    const auto source = h.arena.makeScalarCell(
        static_cast<std::int64_t>(ArenaManifold::refOf(4'000'000)));
    ASSERT_TRUE(h.run(OpcodeKind::CellOfInt, source, absent).success);
    const auto out    = h.arena.makeScalarCell(true);
    const auto result = h.run(OpcodeKind::IntOfCell, absent, out);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find("IntOfCell operand is not a cell"),
              std::string::npos)
        << result.errorMessage;
    EXPECT_EQ(h.arena.asBool(out), std::optional{true});
  }
  {
    Harness h;
    const auto opcode = h.vm.mintOpcode(OpcodeKind::IntOfCell);
    h.core.bindInput(opcode, h.arena.makeCell());
    h.core.bindInput(opcode, h.arena.makeCell());
    h.core.bindOutput(opcode, h.arena.makeCell());
    const auto result = h.vm.step(h.vm.spawnCursor(opcode));
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find("IntOfCell expects one input"),
              std::string::npos)
        << result.errorMessage;
  }
}

TEST(VortexCellIntTest, SpendsOneUnitOfWorkAndRecoversAfterABudgetReset) {
  Harness h;
  const auto cell   = h.arena.makeCell("c");
  const auto out    = h.arena.makeCell();
  const auto opcode = h.convert(OpcodeKind::IntOfCell, cell, out);
  const auto cursor = h.vm.spawnCursor(opcode);
  h.arena.setExecutionLimits(100'000, 0, 100'000);
  EXPECT_FALSE(h.vm.step(cursor).success);
  h.arena.setExecutionLimits(100'000, 100'000, 100'000);
  h.vm.setCursorOpcode(cursor, opcode);
  ASSERT_TRUE(h.vm.step(cursor).success);
}

TEST(VortexCellIntTest, UnsupportedCompositionIsRefused) {
  for (const auto kind : {OpcodeKind::CellOfInt, OpcodeKind::IntOfCell}) {
    {
      Harness h;
      const auto in     = h.arena.makeScalarCell(std::int64_t{1});
      const auto opcode = h.convert(kind, in, h.arena.makeCell());
      h.core.attachPipeline(in, h.vm.mintOpcode(OpcodeKind::Nop));
      EXPECT_FALSE(h.vm.step(h.vm.spawnCursor(opcode)).success);
    }
    {
      Harness h;
      const auto in     = h.arena.makeScalarCell(std::int64_t{1});
      const auto out    = h.arena.makeCell();
      const auto opcode = h.convert(kind, in, out);
      h.vm.enableMemoization(opcode, "k");
      const auto result = h.vm.step(h.vm.spawnCursor(opcode));
      EXPECT_FALSE(result.success);
      EXPECT_NE(result.errorMessage.find("cannot be memoized"),
                std::string::npos)
          << result.errorMessage;
    }
    {
      Harness h;
      const auto in     = h.arena.makeScalarCell(std::int64_t{1});
      const auto opcode = h.convert(kind, in, in);
      const auto result = h.vm.step(h.vm.spawnCursor(opcode));
      EXPECT_FALSE(result.success);
      EXPECT_NE(result.errorMessage.find("must not alias"), std::string::npos)
          << result.errorMessage;
    }
    {
      Harness h;
      const auto in     = h.arena.makeScalarCell(std::int64_t{1});
      const auto opcode = h.convert(kind, in, h.arena.makeCell());
      const auto param  = h.arena.makeScalarCell(std::int64_t{1});
      h.core.attachPipeline(param, opcode);
      const auto add = h.vm.mintOpcode(OpcodeKind::Add);
      h.core.bindInput(add, param);
      h.core.bindInput(add, h.arena.makeScalarCell(std::int64_t{2}));
      h.core.bindOutput(add, h.arena.makeScalarCell(std::int64_t{0}));
      const auto result = h.vm.step(h.vm.spawnCursor(add));
      EXPECT_FALSE(result.success);
      EXPECT_NE(result.errorMessage.find("cannot run in a parameter"),
                std::string::npos)
          << result.errorMessage;
    }
    {
      Harness h;
      const auto condition = h.convert(
          kind, h.arena.makeScalarCell(std::int64_t{1}), h.arena.makeCell());
      const auto guarded = h.vm.mintOpcode(OpcodeKind::Nop);
      h.core.attachPrecondition(guarded, condition);
      const auto result = h.vm.step(h.vm.spawnCursor(guarded));
      EXPECT_FALSE(result.success);
      EXPECT_NE(result.errorMessage.find("cannot run as"), std::string::npos)
          << result.errorMessage;
    }
  }
}

// The hazard the opcode comment states: nothing ties the number to the cell
// it was taken from, so a number outliving release() names whatever is
// minted into the same slot next.
TEST(VortexCellIntTest, ReleasedNumberNamesALaterCell) {
  Harness h;
  h.arena.makeCell("anchor");
  auto mark         = h.arena.mark();
  const auto doomed = h.arena.makeCell("doomed");
  const auto number = h.arena.makeScalarCell(static_cast<std::int64_t>(doomed));
  static_cast<void>(number);
  const auto kept = static_cast<std::int64_t>(doomed);
  ASSERT_TRUE(h.arena.release(mark));
  const auto later = h.arena.makeCell("later");
  const auto in    = h.arena.makeScalarCell(kept);
  const auto out   = h.arena.makeCell();
  ASSERT_TRUE(h.run(OpcodeKind::CellOfInt, in, out).success);
  EXPECT_EQ(h.vm.cellReferenceOf(out), later);
}

TEST(VortexCellIntTest, TheDecodeCeilingReachesIntOfCellAndStopsThere) {
  Harness h;
  EXPECT_EQ(h.vm.getOpcodeKind(h.arena.makeScalarCell(
                static_cast<std::int64_t>(OpcodeKind::CellOfInt))),
            OpcodeKind::CellOfInt);
  EXPECT_EQ(h.vm.getOpcodeKind(h.arena.makeScalarCell(
                static_cast<std::int64_t>(OpcodeKind::IntOfCell))),
            OpcodeKind::IntOfCell);
  EXPECT_FALSE(h.vm.getOpcodeKind(h.arena.makeScalarCell(
      static_cast<std::int64_t>(OpcodeKind::IntOfCell) + 1)));
  const auto stale = h.arena.makeScalarCell(
      static_cast<std::int64_t>(OpcodeKind::IntOfCell) + 1);
  EXPECT_FALSE(h.vm.registerProgramOps(std::vector<CellRef>{stale}));
  const auto good =
      h.arena.makeScalarCell(static_cast<std::int64_t>(OpcodeKind::IntOfCell));
  EXPECT_TRUE(h.vm.registerProgramOps(std::vector<CellRef>{good}));
}

TEST(VortexCellIntTest, RawStdlibCallsRefuseBoth) {
  Harness h;
  VortexStdLib stdlib{h.core, h.vm};
  for (const auto kind : {OpcodeKind::CellOfInt, OpcodeKind::IntOfCell}) {
    const auto opcode = h.vm.mintOpcode(kind);
    try {
      static_cast<void>(stdlib.call(opcode, {}));
      FAIL() << "the raw call was accepted";
    } catch (const std::invalid_argument &error) {
      EXPECT_NE(std::string{error.what()}.find("cell-number conversions"),
                std::string::npos)
          << error.what();
    }
  }
}

TEST(VortexCellIntTest, TheNameTablesKnowBoth) {
  EXPECT_EQ(xanadu::vpl::VPLCompiler::opcodeMnemonic(OpcodeKind::CellOfInt),
            "CELL_OF_INT");
  EXPECT_EQ(xanadu::vpl::VPLCompiler::opcodeMnemonic(OpcodeKind::IntOfCell),
            "INT_OF_CELL");
  EXPECT_EQ(xanadu::vql::VQLCompiler::opcodeMnemonic(OpcodeKind::CellOfInt),
            "CELLOFINT");
  EXPECT_EQ(xanadu::vql::VQLCompiler::opcodeMnemonic(OpcodeKind::IntOfCell),
            "INTOFCELL");
}

TEST(VortexCellIntTest, MeshSealsAndEvaluatesBothAsPureOpcodes) {
  namespace mesh = zigzag::vortex::mesh;
  const mesh::VortexProgram program{
      .cells = {{}, {}, {}, {CellValue{std::string("target")}, {}}, {}, {}},
      .instructions = {{0, OpcodeKind::IntOfCell, {3}, {4}, 1},
                       {1, OpcodeKind::CellOfInt, {4}, {5}, 2},
                       {2, OpcodeKind::Halt, {}, {}, {}}},
      .entry        = 0,
      .outputs      = {{5, true}}};
  const auto bytes = mesh::sealVortexProgram(program);
  ASSERT_TRUE(bytes) << bytes.error();
  const auto result = mesh::evaluate(mesh::Frontend::Vortex, *bytes,
                                     {100'000, 100'000, 16, 1'000'000});
  ASSERT_TRUE(result) << result.error();
  const auto values = mesh::readResultCells(*result);
  ASSERT_TRUE(values) << values.error();
  ASSERT_EQ(values->size(), 1U);
  EXPECT_EQ(result->context->arena.textOf(values->front()), "target");
}

struct TempDir {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vortex_cell_int_export_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDir() { std::filesystem::create_directories(path); }
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

// The conversion pair survives export: the operands relocate as ordinary
// cells and the number is computed in the loading arena, so it names the
// loaded target, not the cell number the author's arena used.
TEST(VortexCellIntTest, ExportedProgramRunsAgainstTheLoadedCells) {
  TempDir temporary;
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Harness h;
    // Offset so the author's cell numbers differ from the loaded ones.
    for (int i = 0; i < 7; ++i) h.arena.makeCell("padding");
    const auto target = h.arena.makeCell("target");
    const auto number = h.arena.makeCell("number slot");
    const auto back   = h.arena.makeCell();
    const auto first  = h.convert(OpcodeKind::IntOfCell, target, number);
    const auto second = h.convert(OpcodeKind::CellOfInt, number, back);
    const auto halt   = h.vm.mintOpcode(OpcodeKind::Halt);
    ASSERT_TRUE(
        h.arena.link(h.core.home(), h.core.dims().spin, DimVector::POS, first));
    ASSERT_TRUE(
        h.arena.link(first, h.core.dims().spin, DimVector::POS, second));
    ASSERT_TRUE(h.arena.link(second, h.core.dims().spin, DimVector::POS, halt));
    xanadu::Store store(scroll);
    const auto exported =
        exportProgram(h.core, h.vm, std::array{first, second, halt}, store);
    EXPECT_FALSE(exported.isZero());
    store.save((temporary.path / "program").string());
  }
  xanadu::Store loaded(scroll);
  loaded.load((temporary.path / "program").string());
  ASSERT_TRUE(loaded.structureHead());
  const auto manifold = loaded.rebuildManifold(*loaded.structureHead());
  ArenaManifold arena(&manifold, loaded);
  VortexCore core(arena);
  VortexVM vm(core);
  const auto first =
      arena.linked(core.home(), core.dims().spin, DimVector::POS);
  ASSERT_TRUE(first);
  const auto execution = vm.run(vm.spawnCursor(*first, "cell-int"), 100);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  const auto findNamed = [&](const std::string_view name) {
    for (const auto &cell : manifold.cells())
      if (arena.textOf(cell.birthOp) == name)
        return std::optional{cell.birthOp};
    return std::optional<CellRef>{};
  };
  const auto target = findNamed("target");
  // The output slot's text became the number, so reach it through the op.
  const auto second = arena.linked(*first, core.dims().spin, DimVector::POS);
  ASSERT_TRUE(target);
  ASSERT_TRUE(second);
  ASSERT_EQ(core.outputsOf(*second).size(), 1U);
  EXPECT_EQ(vm.cellReferenceOf(core.outputsOf(*second).front()), target);
}

TEST(VortexCellIntTest, VagentValidatesBothAtArityOneInOneOutAndStops) {
  Harness h;
  const auto root  = h.arena.makeCell();
  const auto agent = h.arena.makeCell();
  auto binds = xanadu::vagent::mintMetaBindSet(h.arena, h.core, root, agent);
  ASSERT_TRUE(binds.has_value());
  xanadu::vagent::MetaRuntime meta(h.core, h.vm, *binds);
  for (const auto kind : {OpcodeKind::CellOfInt, OpcodeKind::IntOfCell}) {
    const auto shaped = [&](std::size_t inputs, std::size_t outputs) {
      const auto op = h.vm.mintOpcode(kind);
      for (std::size_t i = 0; i < inputs; ++i)
        h.core.bindInput(op, h.arena.makeScalarCell(std::int64_t{1}));
      for (std::size_t i = 0; i < outputs; ++i)
        h.core.bindOutput(op, h.arena.makeCell());
      return meta.validate(op, {.runReachable = false, .expectPure = true});
    };
    const auto good = shaped(1, 1);
    EXPECT_TRUE(good.ok) << good.detail;
    for (const auto [in, out] :
         {std::pair{0U, 1U}, {2U, 1U}, {1U, 0U}, {1U, 2U}}) {
      const auto bad = shaped(in, out);
      EXPECT_FALSE(bad.ok);
      EXPECT_NE(bad.detail.find("wing arity"), std::string::npos) << bad.detail;
    }
  }
  const auto beyond = h.arena.makeScalarCell(
      static_cast<std::int64_t>(OpcodeKind::IntOfCell) + 1);
  const auto unknown = meta.validate(beyond, {.runReachable = false});
  EXPECT_FALSE(unknown.ok);
  EXPECT_NE(unknown.detail.find("not an op cell"), std::string::npos)
      << unknown.detail;
}

} // namespace
} // namespace zigzag::vortex

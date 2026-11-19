/**
 * @file vortex_epoch_of_cell_test.cpp
 * @brief EpochOfCell: the pure opcode that reads ArenaManifold::epochOfCell in
 *        the graph, and every place a new opcode must be known (decode
 *        ceiling, purity, name tables, stdlib, export, vagent).
 *
 * The operand is a cell reference slot, or an Int64 naming a cell as
 * CellOfInt reads it; the answer is a raw Int64. An operand that names no
 * cell is refused by name rather than answered with a number, so no value of
 * the result means "absent".
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

#include "common/xanadu/bencode.hpp"
#include "common/xanadu/provenance.hpp"
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

  /// A reference slot naming @p cell, made the way a program makes one.
  CellRef referenceTo(const CellRef cell) {
    const auto number = arena.makeScalarCell(static_cast<std::int64_t>(cell));
    const auto slot   = arena.makeCell();
    EXPECT_TRUE(run(OpcodeKind::CellOfInt, number, slot).success);
    EXPECT_EQ(vm.cellReferenceOf(slot), cell);
    return slot;
  }

  /// EpochOfCell over @p operand; the answer, or nullopt on a refusal.
  std::optional<std::int64_t> epochOf(const CellRef operand) {
    const auto out    = arena.makeCell();
    const auto result = run(OpcodeKind::EpochOfCell, operand, out);
    if (!result.success) return std::nullopt;
    EXPECT_FALSE(vm.cellReferenceOf(out)) << "the answer must be a raw number";
    return arena.asInt64(out);
  }

  std::int64_t numberOperandEpoch(const CellRef cell) {
    return epochOf(arena.makeScalarCell(static_cast<std::int64_t>(cell)))
        .value();
  }
};

std::int64_t asNumber(const std::optional<ArenaManifold::ReleaseEpoch> epoch) {
  return static_cast<std::int64_t>(epoch.value());
}

TEST(VortexEpochOfCellTest, AgreesWithTheArenaForNumberAndReferenceOperands) {
  Harness h;
  const auto plain     = h.arena.makeCell("plain");
  const auto dimension = h.core.findOrMintDimension("d.epoch-of-cell");
  for (const auto cell : {plain, dimension}) {
    EXPECT_EQ(h.numberOperandEpoch(cell), asNumber(h.arena.epochOfCell(cell)));
    EXPECT_EQ(h.epochOf(h.referenceTo(cell)),
              std::optional{asNumber(h.arena.epochOfCell(cell))});
  }
  // A cell minted after a release is born in a later epoch, and the opcode
  // follows the arena into it.
  auto mark = h.arena.mark();
  h.arena.makeCell("doomed");
  ASSERT_TRUE(h.arena.release(mark));
  const auto later = h.arena.makeCell("later");
  EXPECT_GT(h.arena.epochOfCell(later), h.arena.epochOfCell(plain));
  EXPECT_EQ(h.numberOperandEpoch(later), asNumber(h.arena.epochOfCell(later)));
  EXPECT_EQ(h.epochOf(h.referenceTo(later)),
            std::optional{asNumber(h.arena.epochOfCell(later))});
  // An older cell keeps its epoch across the release.
  EXPECT_EQ(h.numberOperandEpoch(plain), asNumber(h.arena.epochOfCell(plain)));
}

// The witness the opcode exists for: a number kept as an Int64 answers a
// different epoch once its cell was released and the number minted again.
TEST(VortexEpochOfCellTest, ARecordedEpochTellsAReusedNumberFromTheOriginal) {
  Harness h;
  h.arena.makeCell("anchor");
  auto mark           = h.arena.mark();
  const auto doomed   = h.arena.makeCell("doomed");
  const auto kept     = static_cast<std::int64_t>(doomed);
  const auto recorded = h.numberOperandEpoch(doomed);
  ASSERT_TRUE(h.arena.release(mark));
  const auto later = h.arena.makeCell("later");
  ASSERT_EQ(static_cast<std::int64_t>(later), kept);
  const auto now = h.epochOf(h.arena.makeScalarCell(kept));
  ASSERT_TRUE(now);
  EXPECT_NE(*now, recorded);
}

TEST(VortexEpochOfCellTest, ADurableBaseCellIsBornInEpochZero) {
  xanadu::UserPermascroll::Config config;
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("vortex_epoch_of_cell_" +
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
    EXPECT_EQ(h.numberOperandEpoch(durable), 0);
    auto mark = h.arena.mark();
    h.arena.makeCell("scratch");
    ASSERT_TRUE(h.arena.release(mark));
    EXPECT_EQ(h.numberOperandEpoch(durable), 0);
    static_cast<void>(version);
  }
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

// Absence is a refusal by name, not a number: the one representation of "no
// cell" the VM has is noCell, and an epoch of it would be a sentinel.
TEST(VortexEpochOfCellTest, AnOperandNamingNoCellRefusesByName) {
  Harness h;
  h.arena.makeCell("only");
  for (const auto absent :
       {static_cast<std::int64_t>(noCell),
        static_cast<std::int64_t>(ArenaManifold::refOf(4'000'000)),
        std::int64_t{0}, std::int64_t{-1}, std::int64_t{1} << 40}) {
    const auto out = h.arena.makeScalarCell(true);
    const auto result =
        h.run(OpcodeKind::EpochOfCell, h.arena.makeScalarCell(absent), out);
    EXPECT_FALSE(result.success) << absent;
    EXPECT_NE(result.errorMessage.find("EpochOfCell operand is not a cell"),
              std::string::npos)
        << result.errorMessage;
    EXPECT_EQ(h.arena.asBool(out), std::optional{true});
  }
  // A reference slot whose cell is absent says the same.
  const auto absentReference = h.arena.makeCell();
  ASSERT_TRUE(h.run(OpcodeKind::CellOfInt,
                    h.arena.makeScalarCell(static_cast<std::int64_t>(
                        ArenaManifold::refOf(4'000'000))),
                    absentReference)
                  .success);
  const auto out    = h.arena.makeScalarCell(true);
  const auto result = h.run(OpcodeKind::EpochOfCell, absentReference, out);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.errorMessage.find("EpochOfCell operand is not a cell"),
            std::string::npos)
      << result.errorMessage;
  EXPECT_EQ(h.arena.asBool(out), std::optional{true});
}

TEST(VortexEpochOfCellTest,
     AnOperandThatIsNeitherACellNorANumberRefusesByName) {
  const auto refused = [](const std::function<CellRef(Harness &)> &operand) {
    Harness h;
    const auto in     = operand(h);
    const auto out    = h.arena.makeScalarCell(true);
    const auto result = h.run(OpcodeKind::EpochOfCell, in, out);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find(
                  "EpochOfCell operand must be a cell reference or an Int64"),
              std::string::npos)
        << result.errorMessage;
    EXPECT_EQ(h.arena.asBool(out), std::optional{true});
  };
  refused([](Harness &h) { return h.arena.makeCell("text"); });
  refused([](Harness &h) { return h.arena.makeScalarCell(2.5); });
  refused([](Harness &h) { return h.arena.makeScalarCell(true); });
}

TEST(VortexEpochOfCellTest, ASlotMustNotBeAPrivateGroupsPort) {
  Harness h;
  const auto cell   = h.arena.makeCell("c");
  const auto number = h.arena.makeScalarCell(static_cast<std::int64_t>(cell));
  const auto opcode = h.vm.mintOpcode(OpcodeKind::EpochOfCell);
  h.core.bindInput(opcode, number);
  h.core.bindOutput(opcode, h.arena.makeCell());
  EXPECT_TRUE(h.vm.step(h.vm.spawnCursor(opcode)).success);
  const auto malformed = h.vm.mintOpcode(OpcodeKind::EpochOfCell);
  h.core.bindInput(malformed, number);
  h.core.bindInput(malformed, number);
  h.core.bindOutput(malformed, h.arena.makeCell());
  const auto result = h.vm.step(h.vm.spawnCursor(malformed));
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.errorMessage.find("EpochOfCell expects one input"),
            std::string::npos)
      << result.errorMessage;
}

TEST(VortexEpochOfCellTest, SpendsOneUnitOfWorkAndRecoversAfterABudgetReset) {
  Harness h;
  const auto cell   = h.arena.makeCell("c");
  const auto in     = h.arena.makeScalarCell(static_cast<std::int64_t>(cell));
  const auto out    = h.arena.makeCell();
  const auto opcode = h.convert(OpcodeKind::EpochOfCell, in, out);
  const auto cursor = h.vm.spawnCursor(opcode);
  h.arena.setExecutionLimits(100'000, 0, 100'000);
  EXPECT_FALSE(h.vm.step(cursor).success);
  h.arena.setExecutionLimits(100'000, 100'000, 100'000);
  h.vm.setCursorOpcode(cursor, opcode);
  ASSERT_TRUE(h.vm.step(cursor).success);
  // Exactly one unit: a budget of one runs it, so the opcode is O(1) and
  // spends no more than a conversion does.
  Harness one;
  const auto target = one.arena.makeCell("c");
  const auto number =
      one.arena.makeScalarCell(static_cast<std::int64_t>(target));
  const auto step =
      one.convert(OpcodeKind::EpochOfCell, number, one.arena.makeCell());
  const auto spawned = one.vm.spawnCursor(step);
  one.arena.setExecutionLimits(100'000, 1, 100'000);
  EXPECT_TRUE(one.vm.step(spawned).success);
}

TEST(VortexEpochOfCellTest, StreamAndIteratorSlotsAreRefusedAsInputOrOutput) {
  for (const bool asInput : {true, false}) {
    for (const bool stream : {true, false}) {
      Harness h;
      const auto typedSlot = h.arena.makeCell("typed");
      RuntimeBindingSnapshot bindings;
      if (stream)
        bindings.streams.emplace(typedSlot, std::vector<CellRef>{});
      else
        bindings.iterators.emplace(typedSlot, ClosedIteratorState::Exhausted);
      h.vm.restoreBindings(bindings);
      const auto plain = h.arena.makeScalarCell(std::int64_t{1});
      const auto result =
          asInput
              ? h.run(OpcodeKind::EpochOfCell, typedSlot, h.arena.makeCell())
              : h.run(OpcodeKind::EpochOfCell, plain, typedSlot);
      EXPECT_FALSE(result.success);
      EXPECT_NE(result.errorMessage.find("requires scalar reference slots"),
                std::string::npos)
          << result.errorMessage;
      EXPECT_NE(result.errorMessage.find("EpochOfCell"), std::string::npos)
          << result.errorMessage;
    }
  }
}

TEST(VortexEpochOfCellTest, UnsupportedCompositionIsRefused) {
  const auto kind = OpcodeKind::EpochOfCell;
  {
    Harness h;
    const auto in     = h.arena.makeScalarCell(std::int64_t{1});
    const auto opcode = h.convert(kind, in, h.arena.makeCell());
    h.core.attachPipeline(in, h.vm.mintOpcode(OpcodeKind::Nop));
    const auto result = h.vm.step(h.vm.spawnCursor(opcode));
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find("EpochOfCell does not support"),
              std::string::npos)
        << result.errorMessage;
  }
  {
    Harness h;
    const auto in     = h.arena.makeScalarCell(std::int64_t{1});
    const auto opcode = h.convert(kind, in, h.arena.makeCell());
    h.vm.enableMemoization(opcode, "k");
    const auto result = h.vm.step(h.vm.spawnCursor(opcode));
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find("EpochOfCell cannot be memoized"),
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
    // As a parameter pipeline, and as a pre/postcondition: both would run it
    // where the arena's position is not the caller's.
    Harness h;
    const auto opcode = h.convert(kind, h.arena.makeScalarCell(std::int64_t{1}),
                                  h.arena.makeCell());
    const auto param  = h.arena.makeScalarCell(std::int64_t{1});
    h.core.attachPipeline(param, opcode);
    const auto add = h.vm.mintOpcode(OpcodeKind::Add);
    h.core.bindInput(add, param);
    h.core.bindInput(add, h.arena.makeScalarCell(std::int64_t{2}));
    h.core.bindOutput(add, h.arena.makeScalarCell(std::int64_t{0}));
    const auto result = h.vm.step(h.vm.spawnCursor(add));
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find("EpochOfCell cannot run in a parameter"),
              std::string::npos)
        << result.errorMessage;
  }
  {
    Harness h;
    const auto condition = h.convert(
        kind, h.arena.makeScalarCell(std::int64_t{1}), h.arena.makeCell());
    for (const bool pre : {true, false}) {
      const auto guarded = h.vm.mintOpcode(OpcodeKind::Nop);
      if (pre)
        h.core.attachPrecondition(guarded, condition);
      else
        h.core.attachPostcondition(guarded, condition);
      const auto result = h.vm.step(h.vm.spawnCursor(guarded));
      EXPECT_FALSE(result.success);
      EXPECT_NE(result.errorMessage.find("EpochOfCell cannot run as"),
                std::string::npos)
          << result.errorMessage;
    }
  }
}

TEST(VortexEpochOfCellTest, TheDecodeCeilingReachesEpochOfCellAndStopsThere) {
  Harness h;
  EXPECT_EQ(h.vm.getOpcodeKind(h.arena.makeScalarCell(
                static_cast<std::int64_t>(OpcodeKind::IntOfCell))),
            OpcodeKind::IntOfCell);
  EXPECT_EQ(h.vm.getOpcodeKind(h.arena.makeScalarCell(
                static_cast<std::int64_t>(OpcodeKind::EpochOfCell))),
            OpcodeKind::EpochOfCell);
  EXPECT_EQ(static_cast<std::int64_t>(OpcodeKind::EpochOfCell),
            static_cast<std::int64_t>(OpcodeKind::IntOfCell) + 1);
  const auto beyond = static_cast<std::int64_t>(OpcodeKind::EpochOfCell) + 1;
  EXPECT_FALSE(h.vm.getOpcodeKind(h.arena.makeScalarCell(beyond)));
  EXPECT_FALSE(h.vm.registerProgramOps(
      std::vector<CellRef>{h.arena.makeScalarCell(beyond)}));
  EXPECT_TRUE(
      h.vm.registerProgramOps(std::vector<CellRef>{h.arena.makeScalarCell(
          static_cast<std::int64_t>(OpcodeKind::EpochOfCell))}));
}

TEST(VortexEpochOfCellTest, RawStdlibCallsRefuseItByName) {
  Harness h;
  VortexStdLib stdlib{h.core, h.vm};
  const auto opcode = h.vm.mintOpcode(OpcodeKind::EpochOfCell);
  try {
    static_cast<void>(stdlib.call(opcode, {}));
    FAIL() << "the raw call was accepted";
  } catch (const std::invalid_argument &error) {
    EXPECT_NE(std::string{error.what()}.find("cell-number conversions"),
              std::string::npos)
        << error.what();
  }
}

TEST(VortexEpochOfCellTest, TheNameTablesKnowIt) {
  EXPECT_EQ(xanadu::vpl::VPLCompiler::opcodeMnemonic(OpcodeKind::EpochOfCell),
            "EPOCH_OF_CELL");
  EXPECT_EQ(xanadu::vql::VQLCompiler::opcodeMnemonic(OpcodeKind::EpochOfCell),
            "EPOCHOFCELL");
}

// The evaluating arena's epochs depend on how it was built and released, so
// the same sealed bytes would answer differently per arena: not pure.
TEST(VortexEpochOfCellTest, MeshRefusesItAsImpureNamingTheOpcode) {
  namespace mesh = zigzag::vortex::mesh;
  const mesh::VortexProgram program{
      .cells        = {{}, {}, {CellValue{std::int64_t{5}}, {}}, {}},
      .instructions = {{0, OpcodeKind::EpochOfCell, {2}, {3}, 1},
                       {1, OpcodeKind::Halt, {}, {}, {}}},
      .entry        = 0,
      .outputs      = {{3, true}}};
  const auto sealed = mesh::sealVortexProgram(program);
  ASSERT_FALSE(sealed) << "an artifact holding EpochOfCell was sealed";
  EXPECT_NE(sealed.error().find("EpochOfCell"), std::string::npos)
      << sealed.error();
}

namespace mesh = zigzag::vortex::mesh;
using Wire     = xanadu::bencode::Value;

// An artifact is bytes, so one naming EpochOfCell can arrive that was never
// sealed here. Decoding must reach the opcode (the decode ceiling) and refuse
// it by name (the purity allowlist), not call it unknown or run it.
std::string artifactNaming(const OpcodeKind kind) {
  const mesh::VortexProgram program{
      .cells        = {{}, {}, {CellValue{std::int64_t{5}}, {}}, {}},
      .instructions = {{0, OpcodeKind::Add, {2, 2}, {3}, 1},
                       {1, OpcodeKind::Halt, {}, {}, {}}},
      .entry        = 0,
      .outputs      = {{3, false}}};
  const auto sealed = mesh::sealVortexProgram(program);
  EXPECT_TRUE(sealed) << (sealed ? "" : sealed.error());
  auto envelope         = xanadu::bencode::decode(*sealed);
  auto fields           = envelope.asDict();
  auto body             = xanadu::bencode::decode(fields.at("body").asString());
  auto parts            = body.asDict();
  auto list             = parts.at("instructions").asList();
  auto first            = list.front().asDict();
  first["opcode"]       = Wire::integer(static_cast<std::int64_t>(kind));
  list.front()          = Wire::dict(std::move(first));
  parts["instructions"] = Wire::list(std::move(list));
  const auto encoded    = Wire::dict(std::move(parts)).encode();
  fields["body"]        = Wire::string(encoded);
  fields["sha256"]      = Wire::string(xanadu::sha256Hex(encoded));
  return Wire::dict(std::move(fields)).encode();
}

constexpr mesh::EvaluationBudget meshBudget{1'000'000, 100'000, 100, 1'000'000};

TEST(VortexEpochOfCellTest, ADecodedArtifactNamingItIsRefusedByName) {
  const auto result =
      mesh::evaluate(mesh::Frontend::Vortex,
                     artifactNaming(OpcodeKind::EpochOfCell), meshBudget);
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().find("does not support EpochOfCell"),
            std::string::npos)
      << result.error();
}

TEST(VortexEpochOfCellTest, TheMeshDecodeCeilingStopsAfterIt) {
  const auto beyond = static_cast<OpcodeKind>(
      static_cast<std::int64_t>(OpcodeKind::EpochOfCell) + 1);
  const auto result = mesh::evaluate(mesh::Frontend::Vortex,
                                     artifactNaming(beyond), meshBudget);
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().find("Unknown Vortex artifact opcode"),
            std::string::npos)
      << result.error();
}

struct TempDir {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vortex_epoch_of_cell_export_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDir() { std::filesystem::create_directories(path); }
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

void chainOps(Harness &h, const std::vector<CellRef> &ops) {
  const auto halt  = h.vm.mintOpcode(OpcodeKind::Halt);
  CellRef previous = h.core.home();
  for (const auto op : ops) {
    ASSERT_TRUE(h.arena.link(previous, h.core.dims().spin, DimVector::POS, op));
    previous = op;
  }
  ASSERT_TRUE(h.arena.link(previous, h.core.dims().spin, DimVector::POS, halt));
}

std::string
exportMessage(const std::function<void(Harness &, CellRef)> &shape) {
  TempDir temporary;
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  Harness h;
  const auto target = h.arena.makeCell("target");
  shape(h, target);
  const auto first =
      h.arena.linked(h.core.home(), h.core.dims().spin, DimVector::POS);
  if (!first) return "no program";
  std::vector<CellRef> chain{*first};
  while (const auto next =
             h.arena.linked(chain.back(), h.core.dims().spin, DimVector::POS))
    chain.push_back(*next);
  xanadu::Store store(scroll);
  try {
    static_cast<void>(exportProgram(h.core, h.vm, chain, store));
  } catch (const std::exception &error) {
    return error.what();
  }
  return {};
}

TEST(VortexEpochOfCellTest, ExportRefusesAConstantCellNumberByName) {
  const auto message = exportMessage([](Harness &h, const CellRef target) {
    const auto literal =
        h.arena.makeScalarCell(static_cast<std::int64_t>(target));
    chainOps(h,
             {h.convert(OpcodeKind::EpochOfCell, literal, h.arena.makeCell())});
  });
  EXPECT_NE(message.find("EpochOfCell operand is a constant"),
            std::string::npos)
      << message;
}

TEST(VortexEpochOfCellTest, ExportRefusesAMalformedShape) {
  const auto message = exportMessage([](Harness &h, const CellRef target) {
    const auto op = h.vm.mintOpcode(OpcodeKind::EpochOfCell);
    h.core.bindInput(op, target);
    h.core.bindInput(op, target);
    h.core.bindOutput(op, h.arena.makeCell());
    chainOps(h, {op});
  });
  EXPECT_NE(message.find("one input and one output"), std::string::npos)
      << message;
}

// A computed operand exports, and the loaded program reads the epoch of the
// loaded cell in the loading arena, not the author's.
TEST(VortexEpochOfCellTest, ExportedProgramReadsTheLoadedCellsEpoch) {
  TempDir temporary;
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Harness h;
    for (int i = 0; i < 7; ++i) h.arena.makeCell("padding");
    const auto target = h.arena.makeCell("target");
    const auto number = h.arena.makeCell("number slot");
    const auto epoch  = h.arena.makeCell("epoch slot");
    const auto first  = h.convert(OpcodeKind::IntOfCell, target, number);
    const auto second = h.convert(OpcodeKind::EpochOfCell, number, epoch);
    const auto halt   = h.vm.mintOpcode(OpcodeKind::Halt);
    ASSERT_TRUE(
        h.arena.link(h.core.home(), h.core.dims().spin, DimVector::POS, first));
    ASSERT_TRUE(
        h.arena.link(first, h.core.dims().spin, DimVector::POS, second));
    ASSERT_TRUE(h.arena.link(second, h.core.dims().spin, DimVector::POS, halt));
    xanadu::Store store(scroll);
    static_cast<void>(
        exportProgram(h.core, h.vm, std::array{first, second, halt}, store));
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
  const auto execution = vm.run(vm.spawnCursor(*first, "epoch"), 100);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  const auto second = arena.linked(*first, core.dims().spin, DimVector::POS);
  ASSERT_TRUE(second);
  ASSERT_EQ(core.outputsOf(*second).size(), 1U);
  // The loaded target is a durable base cell: epoch 0.
  EXPECT_EQ(arena.asInt64(core.outputsOf(*second).front()),
            std::optional<std::int64_t>{0});
}

TEST(VortexEpochOfCellTest, VagentRefusesItByOrdinalAndStops) {
  Harness h;
  const auto root  = h.arena.makeCell();
  const auto agent = h.arena.makeCell();
  auto binds = xanadu::vagent::mintMetaBindSet(h.arena, h.core, root, agent);
  ASSERT_TRUE(binds.has_value());
  xanadu::vagent::MetaRuntime meta(h.core, h.vm, *binds);
  EXPECT_EQ(h.arena.asInt64(binds->validate.linkedCeiling),
            static_cast<std::int64_t>(OpcodeKind::EpochOfCell));
  const auto op = h.vm.mintOpcode(OpcodeKind::EpochOfCell);
  h.core.bindInput(op, h.arena.makeScalarCell(std::int64_t{1}));
  h.core.bindOutput(op, h.arena.makeCell());
  const auto report = meta.validate(op, {.runReachable = false});
  EXPECT_FALSE(report.ok);
  EXPECT_NE(report.detail.find("absent from the arity table"),
            std::string::npos)
      << report.detail;
  EXPECT_NE(report.detail.find(
                std::to_string(static_cast<int>(OpcodeKind::EpochOfCell))),
            std::string::npos)
      << report.detail;
  const auto beyond = h.arena.makeScalarCell(
      static_cast<std::int64_t>(OpcodeKind::EpochOfCell) + 1);
  const auto unknown = meta.validate(beyond, {.runReachable = false});
  EXPECT_FALSE(unknown.ok);
  EXPECT_NE(unknown.detail.find("not an op cell"), std::string::npos)
      << unknown.detail;
}

TEST(VortexEpochOfCellTest, VagentCeilingFallbacksAlsoReachEpochOfCell) {
  Harness h;
  const auto root  = h.arena.makeCell();
  const auto agent = h.arena.makeCell();
  auto binds = xanadu::vagent::mintMetaBindSet(h.arena, h.core, root, agent);
  ASSERT_TRUE(binds.has_value());
  binds->validate.linkedCeiling = h.arena.makeCell("not a number");
  xanadu::vagent::MetaRuntime meta(h.core, h.vm, *binds);
  const auto op = h.vm.mintOpcode(OpcodeKind::EpochOfCell);
  h.core.bindInput(op, h.arena.makeScalarCell(std::int64_t{1}));
  h.core.bindOutput(op, h.arena.makeCell());
  const auto report = meta.validate(op, {.runReachable = false});
  EXPECT_FALSE(report.ok);
  EXPECT_NE(report.detail.find("absent from the arity table"),
            std::string::npos)
      << report.detail;
}

} // namespace
} // namespace zigzag::vortex

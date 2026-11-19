#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vagent/meta.hpp"
#include "common/xanadu/vortex/mesh_evaluator.hpp"
#include "common/xanadu/vortex/program_export.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vpl/compiler.hpp"
#include "common/xanadu/vql/compiler.hpp"

// The places a new opcode must be known to besides the VM: sealing, raw
// stdlib calls, program export, name tables, the vagent ceiling, and the
// contexts where probes and masks are refused.
namespace zigzag::vortex {
namespace {

namespace fs = std::filesystem;

constexpr std::int64_t keepNone = 0;

struct Bare {
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
};

TEST(VortexMaskProbeWiringTest, ThePortableArtifactRefusesTheProbeByName) {
  mesh::VortexProgram program;
  program.cells        = {{.value = std::monostate{}, .links = {}},
                          {.value = std::monostate{}, .links = {}}};
  program.instructions = {{.cell    = 0,
                           .opcode  = OpcodeKind::VqlMaskProbe,
                           .inputs  = {1},
                           .outputs = {1},
                           .next    = std::nullopt}};
  program.entry        = 0;
  const auto sealed    = mesh::sealVortexProgram(program);
  ASSERT_FALSE(sealed);
  EXPECT_NE(sealed.error().find("VqlMaskProbe"), std::string::npos)
      << sealed.error();
}

TEST(VortexMaskProbeWiringTest, RawStdlibCallsRefuseTheProbe) {
  Bare h;
  VortexStdLib stdlib{h.core, h.vm};
  const auto probe = h.vm.mintOpcode(OpcodeKind::VqlMaskProbe);
  try {
    static_cast<void>(stdlib.call(probe, {}));
    FAIL() << "the raw call was accepted";
  } catch (const std::invalid_argument &error) {
    EXPECT_NE(std::string{error.what()}.find("shape link probes"),
              std::string::npos)
        << error.what();
  }
}

TEST(VortexMaskProbeWiringTest, TheNameTablesKnowTheProbe) {
  EXPECT_EQ(xanadu::vpl::VPLCompiler::opcodeMnemonic(OpcodeKind::VqlMaskProbe),
            "VQL_MASK_PROBE");
  EXPECT_EQ(xanadu::vql::VQLCompiler::opcodeMnemonic(OpcodeKind::VqlMaskProbe),
            "VQLMASKPROBE");
}

TEST(VortexMaskProbeWiringTest, VagentCeilingReachesTheProbeAndStopsThere) {
  Bare h;
  const auto root  = h.arena.makeCell();
  const auto agent = h.arena.makeCell();
  auto binds = xanadu::vagent::mintMetaBindSet(h.arena, h.core, root, agent);
  ASSERT_TRUE(binds.has_value());
  xanadu::vagent::MetaRuntime meta(h.core, h.vm, *binds);

  // The probe is a decodable op the arity table lists (owner, default, then
  // mask pairs, one output), so a bare one is refused on its wing arity and
  // the refusal names the ordinal; a well-formed one passes.
  const auto probe = h.vm.mintOpcode(OpcodeKind::VqlMaskProbe);
  const auto known = meta.validate(probe, {.runReachable = false});
  EXPECT_FALSE(known.ok);
  EXPECT_NE(known.detail.find("wing arity"), std::string::npos) << known.detail;
  EXPECT_NE(known.detail.find(
                std::to_string(static_cast<int>(OpcodeKind::VqlMaskProbe))),
            std::string::npos)
      << known.detail;

  // One ordinal further is not an op at all.
  const auto beyond = h.arena.makeScalarCell(
      static_cast<std::int64_t>(OpcodeKind::VqlMaskProbe) + 1);
  const auto unknown = meta.validate(beyond, {.runReachable = false});
  EXPECT_FALSE(unknown.ok);
  EXPECT_NE(unknown.detail.find("not an op cell"), std::string::npos)
      << unknown.detail;
}

TEST(VortexMaskProbeWiringTest, ProbeCannotRunInAParameterPipeline) {
  Bare h;
  const auto probe = h.vm.mintOpcode(OpcodeKind::VqlMaskProbe);
  const auto param = h.arena.makeScalarCell(std::int64_t{1});
  h.core.attachPipeline(param, probe);
  const auto add = h.vm.mintOpcode(OpcodeKind::Add);
  h.core.bindInput(add, param);
  h.core.bindInput(add, h.arena.makeScalarCell(std::int64_t{2}));
  h.core.bindOutput(add, h.arena.makeScalarCell(std::int64_t{0}));
  const auto result = h.vm.step(h.vm.spawnCursor(add));
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.errorMessage.find("VqlMaskProbe cannot run in a parameter"),
            std::string::npos)
      << result.errorMessage;
}

TEST(VortexMaskProbeWiringTest, ProbeCannotRunAsAPreconditionOrPostcondition) {
  for (const bool pre : {true, false}) {
    Bare h;
    const auto condition = h.vm.mintOpcode(OpcodeKind::VqlMaskProbe);
    h.core.bindInput(condition, h.arena.makeCell("owner"));
    h.core.bindInput(condition, h.arena.makeScalarCell(false));
    const auto guarded = h.vm.mintOpcode(OpcodeKind::Nop);
    if (pre)
      h.core.attachPrecondition(guarded, condition);
    else
      h.core.attachPostcondition(guarded, condition);
    const auto result = h.vm.step(h.vm.spawnCursor(guarded));
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find("VqlMaskProbe cannot run as"),
              std::string::npos)
        << result.errorMessage;
  }
}

TEST(VortexMaskProbeWiringTest, TrailGrowthIsTheSameWhateverTheProbeFinds) {
  // The trail only records under an outstanding mark. A probe that wrote or
  // removed a link would add entries beyond the single output write that a
  // probe finding nothing makes.
  const auto trailDelta = [](const bool linked) {
    Bare h;
    const auto owner = h.arena.makeCell("owner");
    if (linked)
      zigzag::expectWritten(h.arena.link(owner, h.core.dims().role,
                                         DimVector::POS,
                                         h.arena.makeCell("neighbor")));
    const auto opcode = h.vm.mintOpcode(OpcodeKind::VqlMaskProbe);
    h.core.bindInput(opcode, owner);
    h.core.bindInput(opcode, h.arena.makeScalarCell(false));
    const auto output = h.arena.makeScalarCell(false);
    h.core.bindOutput(opcode, output);
    const auto cursor = h.vm.spawnCursor(opcode);
    const auto mark   = h.arena.mark();
    const auto before = h.arena.trailSize();
    const auto result = h.vm.step(cursor);
    EXPECT_TRUE(result.success) << result.errorMessage;
    EXPECT_EQ(h.arena.asBool(output).value_or(false), linked);
    const auto delta = h.arena.trailSize() - before;
    zigzag::expectMarkOrder(h.arena.release(mark));
    return delta;
  };
  const auto none = trailDelta(false);
  EXPECT_GT(none, 0U) << "the output write should be trailed under a mark";
  EXPECT_EQ(trailDelta(true), none);
}

// -- Program export ----------------------------------------------------------

struct TempDir {
  fs::path path =
      fs::temp_directory_path() /
      ("vortex_mask_probe_export_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDir() { fs::create_directories(path); }
  ~TempDir() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
};

TEST(VortexMaskProbeWiringTest, ExportedProbeRelocatesItsDimensionOperand) {
  TempDir temporary;
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Bare h;
    const DimRef axis       = h.core.mintDimension("d.probe-axis");
    const CellRef ownerSlot = h.arena.makeCell();
    const CellRef copy      = h.vm.mintOpcode(OpcodeKind::CopyCellRefs);
    h.core.bindOutput(copy, ownerSlot);
    const CellRef probe = h.vm.mintOpcode(OpcodeKind::VqlMaskProbe);
    h.core.bindInput(probe, ownerSlot);
    h.core.bindInput(probe, h.arena.makeScalarCell(true));
    h.core.bindInput(probe,
                     h.arena.makeScalarCell(static_cast<std::int64_t>(axis)));
    h.core.bindInput(probe, h.arena.makeScalarCell(keepNone));
    h.core.bindOutput(probe, h.arena.makeScalarCell(false));
    zigzag::expectWritten(
        h.arena.link(copy, h.core.dims().spin, DimVector::POS, probe));
    zigzag::expectWritten(
        h.arena.link(h.core.home(), h.core.dims().spin, DimVector::POS, copy));
    const CellRef opcodes[]{copy, probe};
    xanadu::Store program(scroll);
    static_cast<void>(exportProgram(h.core, h.vm, opcodes, program));
    program.save((temporary.path / "program").string());
  }

  xanadu::Store reloaded(scroll);
  reloaded.load((temporary.path / "program").string());
  ASSERT_TRUE(reloaded.structureHead());
  const auto manifold = reloaded.rebuildManifold(*reloaded.structureHead());
  ArenaManifold arena(&manifold, reloaded);
  VortexCore core(arena);
  VortexVM vm(core);
  const auto copy = arena.linked(core.home(), core.dims().spin, DimVector::POS);
  ASSERT_TRUE(copy);
  const auto probe = arena.linked(*copy, core.dims().spin, DimVector::POS);
  ASSERT_TRUE(probe);
  ASSERT_EQ(vm.getOpcodeKind(*probe), OpcodeKind::VqlMaskProbe);
  const auto inputs  = core.inputsOf(*probe);
  const auto outputs = core.outputsOf(*probe);
  ASSERT_EQ(inputs.size(), 4U);
  ASSERT_EQ(outputs.size(), 1U);
  const auto axis = arena.dimensionNamed("d.probe-axis");
  ASSERT_TRUE(axis);
  EXPECT_EQ(arena.asInt64(inputs[2]), static_cast<std::int64_t>(*axis));

  // Keeping nothing on a cell that links d.probe-axis would remove that link;
  // the answer proves the relocated operand names the right dimension.
  const auto linked = arena.makeCell("linked");
  zigzag::expectWritten(
      arena.link(linked, *axis, DimVector::POS, arena.makeCell("leaf")));
  core.bindInput(*copy, linked);
  const auto cursor = vm.spawnCursor(*copy);
  ASSERT_TRUE(vm.step(cursor).success);
  const auto result = vm.step(cursor);
  ASSERT_TRUE(result.success) << result.errorMessage;
  EXPECT_EQ(arena.asBool(outputs.front()), std::optional{true});
}

TEST(VortexMaskProbeWiringTest,
     ExportNamesTheProbeWhenItsOperandsAreMalformed) {
  Bare h;
  const CellRef probe = h.vm.mintOpcode(OpcodeKind::VqlMaskProbe);
  h.core.bindInput(probe, h.arena.makeCell("owner"));
  h.core.bindInput(probe, h.arena.makeScalarCell(false));
  // No output: a mask layout, not a probe layout.
  const CellRef opcodes[]{probe};
  TempDir temporary;
  xanadu::Store program(
      std::make_shared<xanadu::UserPermascroll>(xanadu::UserPermascroll::Config{
          .storageDir = temporary.path / "scroll"}));
  try {
    static_cast<void>(exportProgram(h.core, h.vm, opcodes, program));
    FAIL() << "export accepted a probe without an output";
  } catch (const std::runtime_error &error) {
    EXPECT_NE(std::string{error.what()}.find("VqlMaskProbe"), std::string::npos)
        << error.what();
  }
}

} // namespace
} // namespace zigzag::vortex

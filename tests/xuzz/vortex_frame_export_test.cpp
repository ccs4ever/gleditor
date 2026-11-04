#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/program_export.hpp"

namespace {
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::FrameDescriptorSpec;
using zigzag::vortex::OpcodeKind;

struct TempDir {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vortex_frames_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDir() { std::filesystem::create_directories(path); }
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
};

struct Program {
  CellRef invoke;
  CellRef halt;
  std::vector<CellRef> opcodes;
};

Program adder(Harness &h) {
  const auto a   = h.arena.makeCell();
  const auto b   = h.arena.makeCell();
  const auto sum = h.arena.makeCell();
  const auto add = h.vm.mintOpcode(OpcodeKind::Add);
  const auto ret = h.vm.mintOpcode(OpcodeKind::ReturnFrame);
  h.core.bindInput(add, a);
  h.core.bindInput(add, b);
  h.core.bindOutput(add, sum);
  zigzag::expectWritten(
      h.arena.link(add, h.core.dims().spin, DimVector::POS, ret));
  const auto descriptor =
      h.vm.makeFrameDescriptor(FrameDescriptorSpec{.entry         = add,
                                                   .locals        = {a, b, sum},
                                                   .argumentPorts = {a, b},
                                                   .resultPorts   = {sum}});
  EXPECT_TRUE(descriptor);
  if (!descriptor) throw std::runtime_error("invalid adder descriptor");
  h.core.bindInput(ret, *descriptor);
  const auto invoke = h.vm.mintOpcode(OpcodeKind::InvokeFrame);
  const auto halt   = h.vm.mintOpcode(OpcodeKind::Halt);
  const auto answer = h.arena.makeCell();
  h.core.bindInput(invoke, *descriptor);
  h.core.bindInput(invoke, h.arena.makeScalarCell(std::int64_t{20}));
  h.core.bindInput(invoke, h.arena.makeScalarCell(std::int64_t{22}));
  h.core.bindOutput(invoke, answer);
  h.core.bindInput(halt, answer);
  zigzag::expectWritten(
      h.arena.link(invoke, h.core.dims().spin, DimVector::POS, halt));
  zigzag::expectWritten(
      h.arena.link(h.core.home(), h.core.dims().spin, DimVector::POS, invoke));
  return {invoke, halt, {invoke, halt, add, ret}};
}

TEST(VortexFrameExportTest,
     DescriptorAndDisconnectedBodySurviveReloadIntoFreshVM) {
  TempDir temporary;
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Harness h;
    const auto program = adder(h);
    xanadu::Store store(scroll);
    const auto exported =
        zigzag::vortex::exportProgram(h.core, h.vm, program.opcodes, store);
    EXPECT_FALSE(exported.isZero());
    store.save((temporary.path / "program").string());
  }
  xanadu::Store reloaded(scroll);
  reloaded.load((temporary.path / "program").string());
  ASSERT_TRUE(reloaded.structureHead());
  const auto manifold = reloaded.rebuildManifold(*reloaded.structureHead());
  zigzag::ArenaManifold arena(&manifold, reloaded);
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  const auto entry =
      arena.linked(core.home(), core.dims().spin, DimVector::POS);
  ASSERT_TRUE(entry);
  const auto halt = arena.linked(*entry, core.dims().spin, DimVector::POS);
  ASSERT_TRUE(halt);
  const auto cursor = vm.spawnCursor(*entry, "reopened-frame");
  const auto result = vm.run(cursor, 1000);
  ASSERT_TRUE(result.success) << result.errorMessage;
  const auto outputs = core.inputsOf(*halt);
  ASSERT_EQ(outputs.size(), 1u);
  const auto answer = vm.resultCell(outputs.front());
  ASSERT_TRUE(answer);
  EXPECT_EQ(arena.asInt64(*answer), std::optional{std::int64_t{42}});
  EXPECT_EQ(vm.activationDepth(cursor), 0u);
}

TEST(VortexFrameExportTest, ExportOfLiveActivationRefusesBeforeWritingStore) {
  Harness h;
  const auto program = adder(h);
  const auto cursor  = h.vm.spawnCursor(program.invoke, "pending-frame");
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_EQ(h.vm.activationDepth(cursor), 1u);
  auto scroll = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Store store(scroll);
  const auto before = store.latest();
  EXPECT_THROW(
      (void)zigzag::vortex::exportProgram(h.core, h.vm, program.opcodes, store),
      std::exception);
  EXPECT_EQ(store.latest(), before);
  h.vm.setCursorOpcode(cursor, std::nullopt);
  EXPECT_EQ(h.vm.activationDepth(cursor), 0u);
}

TEST(VortexFrameExportTest, InvalidDescriptorRefusesWithoutMintingMetadata) {
  Harness h;
  const auto entry    = h.vm.mintOpcode(OpcodeKind::Nop);
  const auto local    = h.arena.makeCell();
  const auto external = h.arena.makeCell();
  const auto before   = h.arena.cellCount();
  EXPECT_FALSE(
      h.vm.makeFrameDescriptor(FrameDescriptorSpec{.entry  = entry,
                                                   .locals = {local, local},
                                                   .argumentPorts = {local},
                                                   .resultPorts   = {local}}));
  EXPECT_EQ(h.arena.cellCount(), before);
  EXPECT_FALSE(
      h.vm.makeFrameDescriptor(FrameDescriptorSpec{.entry         = entry,
                                                   .locals        = {local},
                                                   .argumentPorts = {external},
                                                   .resultPorts   = {local}}));
  EXPECT_EQ(h.arena.cellCount(), before);
  EXPECT_FALSE(
      h.vm.makeFrameDescriptor(FrameDescriptorSpec{.entry         = entry,
                                                   .locals        = {entry},
                                                   .argumentPorts = {},
                                                   .resultPorts   = {entry}}));
  EXPECT_EQ(h.arena.cellCount(), before);
}
} // namespace

/**
 * @file vql_compiler_test.cpp
 * @brief Unit tests for the VQL Compiler (vqueryc) translating AST to Vortex.
 */
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::noCell;
using zigzag::vortex::OpcodeKind;
using zigzag::vortex::VortexCore;
using zigzag::vortex::VortexVM;

class VQLCompilerTest : public ::testing::Test {
protected:
  void SetUp() override {
    core = std::make_unique<VortexCore>(arena);
    vm   = std::make_unique<VortexVM>(*core);
  }

  zigzag::ArenaManifold arena;
  std::unique_ptr<VortexCore> core;
  std::unique_ptr<VortexVM> vm;
};

TEST_F(VQLCompilerTest, CompileExecutableStore) {
  VQLCompiler compiler(*core, *vm);

  CompilationOptions opts;
  opts.targetLibrary = false;

  auto result = compiler.compile("##/d.step", opts);
  ASSERT_TRUE(result.success) << result.errorMessage;
  ASSERT_NE(result.entryOpcode, noCell);

  // Home should be linked along +d.spin to entryOpcode
  CellRef spinTarget =
      core->arena().linked(core->home(), core->dims().spin, false);
  EXPECT_EQ(spinTarget, result.entryOpcode);

  // Active cursor should be spawned
  auto cursors = vm->activeCursors();
  ASSERT_FALSE(cursors.empty());
  EXPECT_EQ(vm->getCursorOpcode(cursors.front()), result.entryOpcode);

  // Last instruction in stream should be Halt
  CellRef lastOp = noCell;
  CellRef cur    = result.entryOpcode;
  while (cur != noCell) {
    lastOp = cur;
    cur    = core->arena().linked(cur, core->dims().spin, false);
  }
  ASSERT_NE(lastOp, noCell);
  auto kind = vm->getOpcodeKind(lastOp);
  ASSERT_TRUE(kind.has_value());
  EXPECT_EQ(*kind, OpcodeKind::Halt);
}

TEST_F(VQLCompilerTest, CompileLibraryStore) {
  VQLCompiler compiler(*core, *vm);

  CompilationOptions opts;
  opts.targetLibrary = true;
  opts.moduleName    = "std:geometry";
  opts.symbolName    = "calculate_area";

  auto result = compiler.compile("##/d.step/d.width", opts);
  ASSERT_TRUE(result.success) << result.errorMessage;
  ASSERT_NE(result.entryOpcode, noCell);

  // Module should be linked along +d.stdlib off home
  CellRef modCell =
      core->arena().linked(core->home(), core->dims().stdlib, false);
  ASSERT_NE(modCell, noCell);
  EXPECT_EQ(core->arena().textOf(modCell), "std:geometry");

  // Symbol should be linked along +d.vars off module cell
  CellRef symCell = core->arena().linked(modCell, core->dims().vars, false);
  ASSERT_NE(symCell, noCell);
  EXPECT_EQ(core->arena().textOf(symCell), "calculate_area");

  // Symbol's +d.values should point to entryOpcode
  CellRef fnEntry = core->arena().linked(symCell, core->dims().values, false);
  EXPECT_EQ(fnEntry, result.entryOpcode);

  // Last instruction should be Return
  CellRef lastOp = noCell;
  CellRef cur    = result.entryOpcode;
  while (cur != noCell) {
    lastOp = cur;
    cur    = core->arena().linked(cur, core->dims().spin, false);
  }
  ASSERT_NE(lastOp, noCell);
  auto kind = vm->getOpcodeKind(lastOp);
  ASSERT_TRUE(kind.has_value());
  EXPECT_EQ(*kind, OpcodeKind::Return);
}

TEST_F(VQLCompilerTest, CreationSugarAndClones) {
  VQLCompiler compiler(*core, *vm);

  CompilationOptions opts;
  auto result = compiler.compile("##/d.step%\"Alpha\"%\"Beta\"><%", opts);
  ASSERT_TRUE(result.success) << result.errorMessage;

  // Disassembly should contain NEW, VALUE, and LINK opcodes
  EXPECT_NE(result.disassembly.find("#NEW"), std::string::npos);
  EXPECT_NE(result.disassembly.find("#VALUE"), std::string::npos);
  EXPECT_NE(result.disassembly.find("#LINK"), std::string::npos);
  EXPECT_NE(result.disassembly.find("Alpha"), std::string::npos);
  EXPECT_NE(result.disassembly.find("Beta"), std::string::npos);
}

TEST_F(VQLCompilerTest, FLWORAndConditionals) {
  VQLCompiler compiler(*core, *vm);

  auto result = compiler.compile(
      "for $x in ##/d.items let $factor := 2 where $factor > 1 return $x");
  ASSERT_TRUE(result.success) << result.errorMessage;

  // Check generated opcodes
  EXPECT_NE(result.disassembly.find("#FOR_BIND"), std::string::npos);
  EXPECT_NE(result.disassembly.find("#BIND"), std::string::npos);
  EXPECT_NE(result.disassembly.find("#COMP GT"), std::string::npos);
  EXPECT_NE(result.disassembly.find("#BRANCH"), std::string::npos);
}

TEST_F(VQLCompilerTest, DisassemblyFormatting) {
  VQLCompiler compiler(*core, *vm);

  auto result = compiler.compile("##/d.step");
  ASSERT_TRUE(result.success);

  std::string dis = compiler.disassemble(result.entryOpcode);
  EXPECT_FALSE(dis.empty());
  EXPECT_NE(dis.find("#LINK"), std::string::npos);
  EXPECT_NE(dis.find("#HALT"), std::string::npos);
  EXPECT_NE(dis.find("in:"), std::string::npos);
}

TEST_F(VQLCompilerTest, ExportToStorePersistence) {
  VQLCompiler compiler(*core, *vm);

  CompilationOptions opts;
  opts.targetLibrary = true;
  opts.moduleName    = "std:test_mod";
  opts.symbolName    = "test_fn";

  auto result = compiler.compile("##/d.step%\"persisted_cell\"", opts);
  ASSERT_TRUE(result.success);

  auto permascroll = std::make_shared<UserPermascroll>();
  Store store(permascroll);

  MicroversionId ver = compiler.exportToStore(store);
  EXPECT_FALSE(ver.isZero());
  EXPECT_NE(store.homeCell(), noCell);

  // Save to temporary directory and verify reloading
  std::string tempDir =
      "/tmp/test_vql_compiler_store_" +
      std::to_string(
          std::chrono::system_clock::now().time_since_epoch().count());
  std::filesystem::create_directories(tempDir);

  store.save(tempDir);
  EXPECT_TRUE(std::filesystem::exists(tempDir + "/ops.nodes"));
  EXPECT_TRUE(std::filesystem::exists(tempDir + "/store.tables"));

  Store reloaded(permascroll);
  EXPECT_NO_THROW(reloaded.load(tempDir));
  EXPECT_EQ(reloaded.homeCell(), store.homeCell());

  std::filesystem::remove_all(tempDir);
}

TEST_F(VQLCompilerTest, VortexVMExecutionEquivalence) {
  // Pre-seed arena with items on +d.step
  CellRef c1 = core->arena().makeCell("item1");
  core->arena().link(core->home(), core->dims().step, false, c1);

  VQLCompiler compiler(*core, *vm);
  CompilationOptions opts;
  opts.targetLibrary = false;

  auto result = compiler.compile("##/d.step", opts);
  ASSERT_TRUE(result.success);

  auto cursors = vm->activeCursors();
  ASSERT_FALSE(cursors.empty());
  CellRef cursor = cursors.front();

  // Execute bytecode using VortexVM run
  auto execRes = vm->run(cursor, 100);
  EXPECT_TRUE(execRes.success);
  EXPECT_EQ(execRes.errorMessage, "Halted");
}

} // namespace
} // namespace xanadu::vql

/**
 * @file vpl_compiler_test.cpp
 * @brief Unit tests for the VPL Bytecode Compiler (vplc) translating AST to
 * Vortex.
 */
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vpl/compiler.hpp"
#include "common/xanadu/vpl/parser.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vpl {
namespace {

using zigzag::CellRef;
using zigzag::noCell;
using zigzag::vortex::OpcodeKind;
using zigzag::vortex::VortexCore;
using zigzag::vortex::VortexVM;

class VPLCompilerTest : public ::testing::Test {
protected:
  void SetUp() override {
    core = std::make_unique<VortexCore>(arena);
    vm   = std::make_unique<VortexVM>(*core);
  }

  zigzag::ArenaManifold arena;
  std::unique_ptr<VortexCore> core;
  std::unique_ptr<VortexVM> vm;
};

TEST_F(VPLCompilerTest, CompileExecutableStore) {
  VPLCompiler compiler(*core, *vm);

  CompilationOptions opts;
  opts.targetLibrary = false;

  auto result = compiler.compile("3 + 4", opts);
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

TEST_F(VPLCompilerTest, CompileLibraryStore) {
  VPLCompiler compiler(*core, *vm);

  CompilationOptions opts;
  opts.targetLibrary = true;
  opts.moduleName    = "vpl:math";
  opts.symbolName    = "add_nums";

  auto result = compiler.compile("10 + 20", opts);
  ASSERT_TRUE(result.success) << result.errorMessage;
  ASSERT_NE(result.entryOpcode, noCell);

  // Module should be linked along +d.stdlib off home
  CellRef modCell =
      core->arena().linked(core->home(), core->dims().stdlib, false);
  ASSERT_NE(modCell, noCell);
  EXPECT_EQ(core->arena().textOf(modCell), "vpl:math");

  // Symbol should be linked along +d.vars off module cell
  CellRef symCell = core->arena().linked(modCell, core->dims().vars, false);
  ASSERT_NE(symCell, noCell);
  EXPECT_EQ(core->arena().textOf(symCell), "add_nums");

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

TEST_F(VPLCompilerTest, CompileDyadicIotaAndAssignment) {
  VPLCompiler compiler(*core, *vm);

  // Both APL and J-style syntax
  auto resultApl = compiler.compile("A ← 5⍳d.1");
  ASSERT_TRUE(resultApl.success) << resultApl.errorMessage;
  EXPECT_NE(resultApl.disassembly.find("#NEW"), std::string::npos);
  EXPECT_NE(resultApl.disassembly.find("#VALUE"), std::string::npos);
  EXPECT_NE(resultApl.disassembly.find("#BIND A"), std::string::npos);
  EXPECT_NE(resultApl.disassembly.find("#HALT"), std::string::npos);

  // Re-instantiate for clean manifold
  zigzag::ArenaManifold arena2;
  VortexCore core2(arena2);
  VortexVM vm2(core2);
  VPLCompiler compiler2(core2, vm2);

  auto resultJ = compiler2.compile("A =. 5 i. d.1");
  ASSERT_TRUE(resultJ.success) << resultJ.errorMessage;
  EXPECT_NE(resultJ.disassembly.find("#NEW"), std::string::npos);
  EXPECT_NE(resultJ.disassembly.find("#VALUE"), std::string::npos);
  EXPECT_NE(resultJ.disassembly.find("#BIND A"), std::string::npos);
}

TEST_F(VPLCompilerTest, CompileMonadicOperations) {
  VPLCompiler compiler(*core, *vm);

  auto resNeg = compiler.compile("- 42");
  ASSERT_TRUE(resNeg.success);
  EXPECT_NE(resNeg.disassembly.find("#NEG"), std::string::npos);

  auto resAbs = compiler.compile("| -10");
  ASSERT_TRUE(resAbs.success);
  EXPECT_NE(resAbs.disassembly.find("#ABS"), std::string::npos);

  auto resNot = compiler.compile("∼ 0");
  ASSERT_TRUE(resNot.success);
  EXPECT_NE(resNot.disassembly.find("#NOT"), std::string::npos);

  auto resRecip = compiler.compile("÷ 2");
  ASSERT_TRUE(resRecip.success);
  EXPECT_NE(resRecip.disassembly.find("#RECIPROCAL"), std::string::npos);
}

TEST_F(VPLCompilerTest, CompileReduceAndScan) {
  VPLCompiler compiler(*core, *vm);

  auto resRed = compiler.compile("+/ 1 2 3 4 5");
  ASSERT_TRUE(resRed.success);
  EXPECT_NE(resRed.disassembly.find("#REDUCE_ADD"), std::string::npos);

  auto resScan = compiler.compile("+\\ 1 2 3 4 5");
  ASSERT_TRUE(resScan.success);
  EXPECT_NE(resScan.disassembly.find("#SCAN_ADD"), std::string::npos);
}

TEST_F(VPLCompilerTest, CompileOptimizationConstantFolding) {
  VPLCompiler compiler(*core, *vm);

  CompilationOptions unopt;
  unopt.optimize = false;
  auto resUnopt  = compiler.compile("3 + 4", unopt);
  ASSERT_TRUE(resUnopt.success);
  EXPECT_NE(resUnopt.disassembly.find("#ADD"), std::string::npos);
  EXPECT_EQ(resUnopt.generatedOpcodes.size(), 2u); // Add + Halt

  zigzag::ArenaManifold arenaOpt;
  VortexCore coreOpt(arenaOpt);
  VortexVM vmOpt(coreOpt);
  VPLCompiler compilerOpt(coreOpt, vmOpt);

  CompilationOptions opt;
  opt.optimize = true;
  auto resOpt  = compilerOpt.compile("3 + 4", opt);
  ASSERT_TRUE(resOpt.success);
  // Add should be folded away!
  EXPECT_EQ(resOpt.disassembly.find("#ADD"), std::string::npos);
  EXPECT_EQ(resOpt.generatedOpcodes.size(), 1u); // Only Halt
}

TEST_F(VPLCompilerTest, CompileIndexingAndEnclose) {
  VPLCompiler compiler(*core, *vm);

  auto resEnc = compiler.compile("⊂ 42");
  ASSERT_TRUE(resEnc.success);
  EXPECT_NE(resEnc.disassembly.find("#ENCLOSE"), std::string::npos);

  auto resDisc = compiler.compile("⊃ C");
  ASSERT_TRUE(resDisc.success);
  EXPECT_NE(resDisc.disassembly.find("#DISCLOSE"), std::string::npos);

  auto resIdx = compiler.compile("A[3]");
  ASSERT_TRUE(resIdx.success);
  EXPECT_NE(resIdx.disassembly.find("#INDEX_WALK"), std::string::npos);
}

TEST_F(VPLCompilerTest, RenderASTFormatting) {
  Parser parser("A ← 5⍳d.1");
  auto ast = parser.parseProgram();
  ASSERT_NE(ast, nullptr);

  std::string rendered = VPLCompiler::renderAST(*ast);
  EXPECT_FALSE(rendered.empty());
  EXPECT_NE(rendered.find("Program"), std::string::npos);
  EXPECT_NE(rendered.find("AssignExpr: A"), std::string::npos);
  EXPECT_NE(rendered.find("DyadicExpr: ⍳"), std::string::npos);
  EXPECT_NE(rendered.find("ScalarExpr: 5"), std::string::npos);
  EXPECT_NE(rendered.find("DimensionExpr: d.1"), std::string::npos);
}

TEST_F(VPLCompilerTest, ExportToStorePersistence) {
  VPLCompiler compiler(*core, *vm);

  CompilationOptions opts;
  opts.targetLibrary = true;
  opts.moduleName    = "vpl:persisted";
  opts.symbolName    = "calculate";

  auto result = compiler.compile("10 + 20", opts);
  ASSERT_TRUE(result.success);

  auto permascroll = std::make_shared<UserPermascroll>();
  Store store(permascroll);

  MicroversionId ver = compiler.exportToStore(store);
  EXPECT_FALSE(ver.isZero());
  EXPECT_NE(store.homeCell(), noCell);

  std::string tempDir =
      "/tmp/test_vpl_compiler_store_" +
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

TEST_F(VPLCompilerTest, VortexVMExecutionRun) {
  VPLCompiler compiler(*core, *vm);

  CompilationOptions opts;
  opts.targetLibrary = false;
  opts.optimize      = false;

  auto result = compiler.compile("3 + 4", opts);
  ASSERT_TRUE(result.success);

  auto cursors = vm->activeCursors();
  ASSERT_FALSE(cursors.empty());
  CellRef cursor = cursors.front();

  // Execute bytecode using VortexVM run
  auto execRes = vm->run(cursor, 100);
  EXPECT_TRUE(execRes.success);
  EXPECT_EQ(execRes.errorMessage, "Halted");

  // Output cell of Add opcode should contain 7
  CellRef addOp = result.entryOpcode;
  auto outCells = core->outputsOf(addOp);
  ASSERT_FALSE(outCells.empty());
  EXPECT_EQ(core->arena().asInt64(outCells.front()), 7);
}

} // namespace
} // namespace xanadu::vpl

/**
 * @file vpl_compiler_test.cpp
 * @brief Unit tests for the VPL Bytecode Compiler (vplc) translating AST to
 * Vortex.
 */
#include <chrono>
#include <filesystem>
#include <functional>
#include <gtest/gtest.h>
#include <memory>

#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vpl/compiler.hpp"
#include "common/xanadu/vpl/parser.hpp"
#include "common/xanadu/vpl/vpl_engine.hpp"
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
  ASSERT_TRUE(result.entryOpcode);

  // Home should be linked along +d.spin to entryOpcode
  const auto spinTarget =
      core->arena().linked(core->home(), core->dims().spin, false);
  ASSERT_TRUE(spinTarget);
  EXPECT_EQ(*spinTarget, *result.entryOpcode);

  // Active cursor should be spawned
  auto cursors = vm->activeCursors();
  ASSERT_FALSE(cursors.empty());
  EXPECT_EQ(vm->getCursorOpcode(cursors.front()), *result.entryOpcode);

  // Last instruction in stream should be Halt
  CellRef lastOp = *result.entryOpcode;
  auto cur       = core->arena().linked(lastOp, core->dims().spin, false);
  while (cur) {
    lastOp = *cur;
    cur    = core->arena().linked(*cur, core->dims().spin, false);
  }
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
  ASSERT_TRUE(result.entryOpcode);

  // Module should be linked along +d.stdlib off home
  const auto modCell =
      core->arena().linked(core->home(), core->dims().stdlib, false);
  ASSERT_TRUE(modCell);
  EXPECT_EQ(core->arena().textOf(*modCell), "vpl:math");

  // Symbol should be linked along +d.vars off module cell
  const auto symCell = core->arena().linked(*modCell, core->dims().vars, false);
  ASSERT_TRUE(symCell);
  EXPECT_EQ(core->arena().textOf(*symCell), "add_nums");

  // Symbol's +d.values should point to entryOpcode
  const auto fnEntry =
      core->arena().linked(*symCell, core->dims().values, false);
  EXPECT_EQ(fnEntry, result.entryOpcode);

  // Last instruction should be Return
  CellRef lastOp = *result.entryOpcode;
  auto cur       = core->arena().linked(lastOp, core->dims().spin, false);
  while (cur) {
    lastOp = *cur;
    cur    = core->arena().linked(*cur, core->dims().spin, false);
  }
  auto kind = vm->getOpcodeKind(lastOp);
  ASSERT_TRUE(kind.has_value());
  EXPECT_EQ(*kind, OpcodeKind::Return);
}

TEST_F(VPLCompilerTest, RejectsDyadicIotaInsteadOfCompilingItAtCompileTime) {
  VPLCompiler compiler(*core, *vm);

  const auto resultApl = compiler.compile("A ← 5⍳d.1");
  EXPECT_FALSE(resultApl.success);
  EXPECT_NE(resultApl.errorMessage.find("unsupported VPL dyadic verb"),
            std::string::npos);
  EXPECT_FALSE(resultApl.entryOpcode);
  EXPECT_TRUE(resultApl.generatedOpcodes.empty());

  const auto resultJ = compiler.compile("A =. 5 i. d.1");
  EXPECT_FALSE(resultJ.success);
  EXPECT_NE(resultJ.errorMessage.find("unsupported VPL dyadic verb"),
            std::string::npos);
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
  ASSERT_EQ(resRed.resultSlots.size(), 1u);
  EXPECT_TRUE(resRed.resultAxes.empty());
  ASSERT_EQ(resScan.resultSlots.size(), 1u);
  ASSERT_EQ(resScan.resultAxes.size(), 1u);
  EXPECT_EQ(resScan.resultAxes.front(),
            (zigzag::DirectedDim{core->dims().step, zigzag::DimVector::POS}));
}

TEST_F(VPLCompilerTest, ProgramExposesOnlyFinalResultAndRankAxes) {
  VPLCompiler compiler(*core, *vm);

  const auto result = compiler.compile("A ← 1 2 3; B ← A; B");
  ASSERT_TRUE(result.success) << result.errorMessage;
  ASSERT_EQ(result.resultSlots.size(), 1u);
  ASSERT_EQ(result.resultAxes.size(), 1u);
  EXPECT_EQ(result.resultAxes.front(),
            (zigzag::DirectedDim{core->dims().step, zigzag::DimVector::POS}));
  ASSERT_TRUE(result.assignedBindings.contains("A"));
  ASSERT_TRUE(result.assignedBindings.contains("B"));
  ASSERT_EQ(result.assignedBindings.at("A").axes.size(), 1u);
  EXPECT_EQ(result.assignedBindings.at("A").axes.front(),
            result.resultAxes.front());
  EXPECT_EQ(result.assignedBindings.at("B").axes,
            result.assignedBindings.at("A").axes);

  const auto multiple = compiler.compile("1; 2; 3");
  ASSERT_TRUE(multiple.success) << multiple.errorMessage;
  ASSERT_EQ(multiple.resultSlots.size(), 1u);
  EXPECT_TRUE(multiple.resultAxes.empty());
}

TEST_F(VPLCompilerTest, ScalarArithmeticExplicitlyRefusesRankOperands) {
  VPLCompiler compiler(*core, *vm);

  const auto dyadic = compiler.compile("1 2 3 + 4 5 6");
  EXPECT_FALSE(dyadic.success);
  EXPECT_NE(dyadic.errorMessage.find("scalar dyadic arithmetic on ranks"),
            std::string::npos);

  const auto monadic = compiler.compile("- 1 2 3");
  EXPECT_FALSE(monadic.success);
  EXPECT_NE(monadic.errorMessage.find("scalar monadic arithmetic on a rank"),
            std::string::npos);
}

TEST_F(VPLCompilerTest, UnboundIdentifierIsRefused) {
  VPLCompiler compiler(*core, *vm);

  const auto unbound = compiler.compile("missing");
  EXPECT_FALSE(unbound.success);
  EXPECT_NE(unbound.errorMessage.find("unbound VPL identifier"),
            std::string::npos);
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

TEST_F(VPLCompilerTest, RejectsUnsupportedEncloseAndDisclose) {
  VPLCompiler compiler(*core, *vm);

  const auto enclose = compiler.compile("⊂ 42");
  EXPECT_FALSE(enclose.success);
  EXPECT_NE(enclose.errorMessage.find("unsupported VPL monadic verb"),
            std::string::npos);

  EXPECT_FALSE(compiler.compile("⊃ C").success);
}

TEST_F(VPLCompilerTest, RejectsUnsupportedIndexing) {
  VPLCompiler compiler(*core, *vm);

  const auto indexing = compiler.compile("A[3]");
  EXPECT_FALSE(indexing.success);
  EXPECT_NE(indexing.errorMessage.find("VPL indexing is unsupported"),
            std::string::npos);
}

TEST_F(VPLCompilerTest, RejectsUnsupportedDimensionValue) {
  VPLCompiler compiler(*core, *vm);

  const auto dimension = compiler.compile("d.1");
  EXPECT_FALSE(dimension.success);
  EXPECT_NE(dimension.errorMessage.find("typed-handle VM lowering"),
            std::string::npos);
}

// Intent (rewritten from "both entry points throw"): the facade's AST and
// source entry points must be the VM and nothing else. Evaluation advances the
// VM's cycle counter, a tiny cycle budget stops it (a host interpreter has no
// such budget), and forms the compiler does not lower are refused through the
// same path at compile time.
TEST_F(VPLCompilerTest, DirectEvaluationEntryPointsRunOnTheVmOnly) {
  VPLEngine engine(*core);
  Parser parser("1 + 2");
  const auto ast = parser.parseProgram();
  ASSERT_NE(ast, nullptr);

  const auto before = engine.totalCycles();
  EXPECT_EQ(engine.evaluate(*ast).scalarInt(), 3);
  const auto afterAst = engine.totalCycles();
  EXPECT_GT(afterAst, before);
  EXPECT_EQ(engine.evaluate("1 + 2").scalarInt(), 3);
  EXPECT_GT(engine.totalCycles(), afterAst);

  engine.setCycleBudget(1);
  for (const auto &attempt :
       {std::function<VplView()>{[&] { return engine.evaluate(*ast); }},
        std::function<VplView()>{[&] { return engine.evaluate("1 + 2"); }}}) {
    try {
      static_cast<void>(attempt());
      FAIL() << "a one-cycle budget must stop the VM";
    } catch (const VPLExecutionError &error) {
      EXPECT_EQ(error.stage(), ExecutionError::Stage::Budget) << error.what();
    }
  }
  engine.setCycleBudget(kDefaultCycleBudget);

  try {
    static_cast<void>(engine.evaluate("1 2 ⍤ 1 3"));
    FAIL() << "an unlowered verb must be refused";
  } catch (const VPLExecutionError &error) {
    EXPECT_EQ(error.stage(), ExecutionError::Stage::Compile);
    EXPECT_NE(std::string(error.what()).find("conjunction"), std::string::npos);
    EXPECT_EQ(std::string(error.what()).find("unbound"), std::string::npos);
  }
  try {
    static_cast<void>(engine.evaluate("missing"));
    FAIL() << "an unbound name must be refused";
  } catch (const VPLExecutionError &error) {
    EXPECT_EQ(error.stage(), ExecutionError::Stage::Compile);
    EXPECT_NE(
        std::string(error.what()).find("unbound VPL identifier 'missing'"),
        std::string::npos);
  }
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
  EXPECT_TRUE(store.homeCell().has_value());

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
  EXPECT_EQ(reloaded.homeCell().value(), store.homeCell().value());

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
  ASSERT_TRUE(result.entryOpcode);
  CellRef addOp = *result.entryOpcode;
  auto outCells = core->outputsOf(addOp);
  ASSERT_FALSE(outCells.empty());
  EXPECT_EQ(core->arena().asInt64(outCells.front()), 7);
}

} // namespace
} // namespace xanadu::vpl

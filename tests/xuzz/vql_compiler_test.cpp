/**
 * @file vql_compiler_test.cpp
 * @brief Unit tests for the VQL Compiler (vqueryc) translating AST to Vortex.
 */
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/parser.hpp"
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

  auto result = compiler.compile("##/d.step%\"compiled\"", opts);
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

TEST_F(VQLCompilerTest, MissingLiteralAnchorProducesNoFallbackResult) {
  VQLCompiler compiler(*core, *vm);
  PathExpression path;
  path.anchor.kind   = AnchorKind::LiteralCellId;
  path.anchor.cellId = core->arena().cellCount() + 100;
  QueryExpression query;
  query.expr = path;

  const auto result = compiler.compile(query);
  ASSERT_TRUE(result.success) << result.errorMessage;
  ASSERT_TRUE(result.entryOpcode);
  ASSERT_EQ(result.generatedOpcodes.size(), 1u);
  const auto kind = vm->getOpcodeKind(*result.entryOpcode);
  ASSERT_TRUE(kind);
  EXPECT_EQ(*kind, OpcodeKind::Halt);
  EXPECT_FALSE(core->arena().linked(*result.entryOpcode, core->dims().grab,
                                    zigzag::DimVector::POS));
}

TEST_F(VQLCompilerTest, CompilesFunctionDefinitionsWithRuntimeContracts) {
  VQLCompiler compiler(*core, *vm);
  const auto result =
      compiler.compile("def pure identity($x: Int): Int := $x; identity(7)");
  ASSERT_TRUE(result.success) << result.errorMessage;
  ASSERT_FALSE(vm->activeCursors().empty());
  EXPECT_TRUE(vm->run(vm->activeCursors().back(), 1000).success);
  const auto slots = core->inputsOf(result.generatedOpcodes.back());
  ASSERT_EQ(slots.size(), 1u);
  const auto cells = vm->resultCells(slots.front());
  ASSERT_EQ(cells.size(), 1u);
  EXPECT_EQ(core->arena().asInt64(cells.front()), 7);
}

TEST_F(VQLCompilerTest, CompilesAndRunsPointFreeFunctorBindingAndApplication) {
  const CellRef child  = core->arena().makeCell("child");
  const CellRef parent = core->arena().makeCell("parent");
  ASSERT_TRUE(core->arena().link(child, core->dims().step, false, parent));
  ASSERT_TRUE(
      core->arena().link(core->home(), core->dims().step, false, child));

  VQLCompiler compiler(*core, *vm);
  const auto result = compiler.compile("let $up := /+d.step return ##/$up");

  ASSERT_TRUE(result.success) << result.errorMessage;
  ASSERT_TRUE(result.entryOpcode);
  const auto cursors = vm->activeCursors();
  ASSERT_FALSE(cursors.empty());
  const auto execution = vm->run(cursors.front(), 1000);
  EXPECT_TRUE(execution.success) << execution.errorMessage;
  const auto slots = core->inputsOf(result.generatedOpcodes.back());
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(vm->resultCells(slots.front()),
            (std::vector<CellRef>{child, parent}));
}

TEST_F(VQLCompilerTest, ComposesBoundPointFreeFunctors) {
  const CellRef child  = core->arena().makeCell("child");
  const CellRef parent = core->arena().makeCell("parent");
  ASSERT_TRUE(
      core->arena().link(core->home(), core->dims().step, false, child));
  ASSERT_TRUE(core->arena().link(child, core->dims().step, false, parent));

  VQLCompiler compiler(*core, *vm);
  const auto result =
      compiler.compile("let $up := /+d.step; let $twice := $up / /$up "
                       "return ##/$twice");

  ASSERT_TRUE(result.success) << result.errorMessage;
  ASSERT_TRUE(result.entryOpcode);
  const auto cursors = vm->activeCursors();
  ASSERT_FALSE(cursors.empty());
  const auto execution = vm->run(cursors.front(), 1000);
  EXPECT_TRUE(execution.success) << execution.errorMessage;
  const auto slots = core->inputsOf(result.generatedOpcodes.back());
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(vm->resultCells(slots.front()), (std::vector<CellRef>{parent}));
}

TEST_F(VQLCompilerTest, RefusesSelfReferentialFunctorBinding) {
  VQLCompiler compiler(*core, *vm);
  const auto result = compiler.compile("let $self := /$self return ##/$self");

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.errorMessage.find("unbound point-free functor"),
            std::string::npos);
}

TEST_F(VQLCompilerTest,
       RefusesQualifiedFunctorCaptureWithoutDroppingModifiers) {
  for (const auto suffix : {">", "@(88)"}) {
    VQLCompiler compiler(*core, *vm);
    auto query = Parser("def apply($f): Cell := ##/$f; "
                        "let $f := /+d.step; apply($f)")
                     .parseQuery();
    const auto qualified = Parser("$f" + std::string(suffix)).parseQuery();
    auto &path = std::get<PathExpression>(query.body.statements.back());
    auto &call = std::get<FunctionInvocation>(path.steps.front().selector);
    call.args.front().kind = std::make_shared<PathExpression>(
        std::get<PathExpression>(qualified.expr));
    const auto result = compiler.compile(query);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find("qualified functor anchors"),
              std::string::npos)
        << result.errorMessage;
  }
}

TEST_F(VQLCompilerTest, RefusesNamedStoreWithoutCoordinator) {
  VQLCompiler compiler(*core, *vm);
  const auto result = compiler.compile("##missing/d.items");
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.errorMessage.find("require a coordinator"),
            std::string::npos);
}

TEST_F(VQLCompilerTest, CompileLibraryStore) {
  VQLCompiler compiler(*core, *vm);

  CompilationOptions opts;
  opts.targetLibrary = true;
  opts.moduleName    = "std:geometry";
  opts.symbolName    = "calculate_area";

  auto result =
      compiler.compile("##/d.step%\"compiled\"/d.width%\"field\"", opts);
  ASSERT_TRUE(result.success) << result.errorMessage;
  ASSERT_TRUE(result.entryOpcode);

  // Module should be linked along +d.stdlib off home
  const auto modCell =
      core->arena().linked(core->home(), core->dims().stdlib, false);
  ASSERT_TRUE(modCell);
  EXPECT_EQ(core->arena().textOf(*modCell), "std:geometry");

  // Symbol should be linked along +d.vars off module cell
  const auto symCell = core->arena().linked(*modCell, core->dims().vars, false);
  ASSERT_TRUE(symCell);
  EXPECT_EQ(core->arena().textOf(*symCell), "calculate_area");

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

TEST_F(VQLCompilerTest, LiteralCreationSugar) {
  VQLCompiler compiler(*core, *vm);

  CompilationOptions opts;
  auto result = compiler.compile("##/d.step%\"Alpha\"%\"Beta\"", opts);
  ASSERT_TRUE(result.success) << result.errorMessage;

  // Literal creation lowers to structural NEW and VALUE opcodes
  EXPECT_NE(result.disassembly.find("#NEW"), std::string::npos);
  EXPECT_NE(result.disassembly.find("#VALUE"), std::string::npos);
  EXPECT_NE(result.disassembly.find("Alpha"), std::string::npos);
  EXPECT_NE(result.disassembly.find("Beta"), std::string::npos);
}

TEST_F(VQLCompilerTest, BareCreationRefusesBeforeEmission) {
  VQLCompiler compiler(*core, *vm);
  const auto cellCount  = arena.cellCount();
  const auto cursors    = vm->activeCursors();
  const auto spinTarget = arena.linked(core->home(), core->dims().spin, false);

  for (const auto *query : {"%\"Alpha\"", "%\"Alpha\"/d.child%\"Beta\""}) {
    SCOPED_TRACE(query);
    const auto result = compiler.compile(query);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find("bare creation anchors"),
              std::string::npos);
    EXPECT_NE(result.errorMessage.find("--engine direct"), std::string::npos);
    EXPECT_FALSE(result.entryOpcode);
    EXPECT_TRUE(result.generatedOpcodes.empty());
    EXPECT_EQ(arena.cellCount(), cellCount);
    EXPECT_EQ(vm->activeCursors(), cursors);
    EXPECT_EQ(arena.linked(core->home(), core->dims().spin, false), spinTarget);
  }
}

TEST_F(VQLCompilerTest, LowersForAndWhereControlFlow) {
  VQLCompiler compiler(*core, *vm);
  auto result = compiler.compile(
      "for $x in ##/d.items let $factor := 2 where $factor > 1 return $x");
  ASSERT_TRUE(result.success) << result.errorMessage;
  EXPECT_NE(result.disassembly.find("#FOR_BEGIN"), std::string::npos);
  EXPECT_NE(result.disassembly.find("#WHERE_FALSE"), std::string::npos);
  EXPECT_NE(result.disassembly.find("#STREAM_YIELD"), std::string::npos);
}

TEST_F(VQLCompilerTest, FLWORAndConditionals) {
  VQLCompiler compiler(*core, *vm);
  auto result = compiler.compile(
      "for $x in ##/d.items let $factor := 2 where $factor > 1 return $x");
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.errorMessage.find("FLWOR"), std::string::npos);
  EXPECT_TRUE(result.generatedOpcodes.empty());
}

TEST_F(VQLCompilerTest, DisassemblyFormatting) {
  VQLCompiler compiler(*core, *vm);

  auto result = compiler.compile("##/d.step%\"compiled\"");
  ASSERT_TRUE(result.success);

  ASSERT_TRUE(result.entryOpcode);
  std::string dis = compiler.disassemble(*result.entryOpcode);
  EXPECT_FALSE(dis.empty());
  EXPECT_NE(dis.find("#RANK_BEGIN"), std::string::npos);
  EXPECT_NE(dis.find("#NEW"), std::string::npos);
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
  EXPECT_TRUE(store.homeCell().has_value());

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
  EXPECT_EQ(reloaded.homeCell().value(), store.homeCell().value());

  std::filesystem::remove_all(tempDir);
}

TEST_F(VQLCompilerTest, VortexVMExecutionEquivalence) {
  VQLCompiler compiler(*core, *vm);
  CompilationOptions opts;
  opts.targetLibrary = false;

  auto result = compiler.compile("##/d.step%\"compiled\"", opts);
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

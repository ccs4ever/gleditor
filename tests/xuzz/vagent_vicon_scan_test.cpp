#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "common/xanadu/vagent/compiler.hpp"
#include "common/xanadu/vagent/parser.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

namespace {

using namespace xanadu::vagent;
using zigzag::CellRef;
using zigzag::vortex::CellValue;
using zigzag::vortex::OpcodeKind;

struct ScanHarness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  zigzag::vortex::VortexStdLib stdlib{core, vm};
  Compiler compiler{core, vm};

  std::expected<CompiledStatement, Diagnostic>
  compile(std::string_view source, const CompilerBindings &bindings = {},
          ResultUse use = ResultUse::Value) {
    auto expression = Parser(source).parseExpression();
    if (!expression) return std::unexpected(expression.error());
    return compiler.compileStatement(**expression, bindings, use);
  }

  CellRef run(const CompiledStatement &statement) {
    const auto cursor = vm.spawnCursor(statement.entry);
    vm.setCursorOpcode(cursor, statement.entry);
    const auto result = vm.run(cursor);
    EXPECT_TRUE(result.success) << result.errorMessage;
    const auto pc = vm.getCursorOpcode(cursor);
    EXPECT_TRUE(pc);
    if (pc) EXPECT_EQ(vm.getOpcodeKind(*pc), OpcodeKind::Halt);
    return cursor;
  }

  bool produced(const CompiledStatement &statement) {
    const auto value = arena.asBool(statement.produced);
    EXPECT_TRUE(value.has_value());
    return value.value_or(false);
  }
};

TEST(VAgentVIconScanTest, StdLibDirectScanningRoutines) {
  ScanHarness h;
  const CellRef subj = h.core.arena().makeCell("ID:12345;REST");
  const auto cursor  = h.stdlib.scanPush(subj);
  ASSERT_TRUE(cursor.has_value());
  EXPECT_EQ(h.stdlib.scanPos(), 1);
  EXPECT_EQ(h.stdlib.goalSubjectText(), "ID:12345;REST");

  // match("ID:") -> returns 4 (1-based position after match)
  const auto m = h.stdlib.goalMatch("ID:");
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(*m, 4);
  EXPECT_EQ(h.stdlib.scanPos(), 1); // match does not move &pos

  // tab(4) -> moves pos to 4, yields "ID:"
  const auto t1 = h.stdlib.goalTab(4);
  ASSERT_TRUE(t1.has_value());
  EXPECT_EQ(*t1, "ID:");
  EXPECT_EQ(h.stdlib.scanPos(), 4);

  // upto(";") -> returns 10
  const auto u = h.stdlib.goalUpto(";");
  ASSERT_TRUE(u.has_value());
  EXPECT_EQ(*u,
            9); // 'I'=1, 'D'=2, ':'=3, '1'=4, '2'=5, '3'=6, '4'=7, '5'=8, ';'=9

  // tab(9) -> moves pos to 9, yields "12345"
  const auto t2 = h.stdlib.goalTab(9);
  ASSERT_TRUE(t2.has_value());
  EXPECT_EQ(*t2, "12345");
  EXPECT_EQ(h.stdlib.scanPos(), 9);

  // move(1) -> moves pos past ';', yields ";"
  const auto m1 = h.stdlib.goalMove(1);
  ASSERT_TRUE(m1.has_value());
  EXPECT_EQ(*m1, ";");
  EXPECT_EQ(h.stdlib.scanPos(), 10);

  // tab(0) -> moves to end (len + 1 = 14), yields "REST"
  const auto t3 = h.stdlib.goalTab(0);
  ASSERT_TRUE(t3.has_value());
  EXPECT_EQ(*t3, "REST");
  EXPECT_EQ(h.stdlib.scanPos(), 14);

  h.stdlib.scanPop(*cursor);
  EXPECT_FALSE(h.stdlib.scanCursor().has_value());
}

TEST(VAgentVIconScanTest, CompiledScanBasicTabAndMove) {
  ScanHarness h;
  auto statement = h.compile("\"hello world\" ? tab(6)");
  ASSERT_TRUE(statement) << (statement ? ""
                                       : formatDiagnostic(statement.error()));
  EXPECT_EQ(statement->resultType, (Type{"Str", {}}));
  h.run(*statement);
  EXPECT_TRUE(h.produced(*statement));
  EXPECT_EQ(h.arena.textOf(statement->result), "hello");
}

TEST(VAgentVIconScanTest, CompiledScanMoveSequence) {
  ScanHarness h;
  // tab(7) consumes "hello ", landing pos at 7 ('w'); move(5) moves from 7 to
  // 12 ("world")
  auto statement = h.compile("\"hello world\" ? (tab(7) & move(5))");
  ASSERT_TRUE(statement) << (statement ? ""
                                       : formatDiagnostic(statement.error()));
  EXPECT_EQ(statement->resultType, (Type{"Str", {}}));
  h.run(*statement);
  EXPECT_TRUE(h.produced(*statement));
  EXPECT_EQ(h.arena.textOf(statement->result), "world");
}

TEST(VAgentVIconScanTest, CompiledScanMatchAndUpto) {
  ScanHarness h;
  auto statement =
      h.compile("\"ID:42;\" ? (tab(match(\"ID:\")) & tab(upto(\";\")))");
  ASSERT_TRUE(statement) << (statement ? ""
                                       : formatDiagnostic(statement.error()));
  EXPECT_EQ(statement->resultType, (Type{"Str", {}}));
  h.run(*statement);
  EXPECT_TRUE(h.produced(*statement));
  EXPECT_EQ(h.arena.textOf(statement->result), "42");
}

TEST(VAgentVIconScanTest, CompiledScanManyDigits) {
  ScanHarness h;
  auto statement = h.compile("\"12345abc\" ? tab(many(\"0123456789\"))");
  ASSERT_TRUE(statement) << (statement ? ""
                                       : formatDiagnostic(statement.error()));
  EXPECT_EQ(statement->resultType, (Type{"Str", {}}));
  h.run(*statement);
  EXPECT_TRUE(h.produced(*statement));
  EXPECT_EQ(h.arena.textOf(statement->result), "12345");
}

TEST(VAgentVIconScanTest, CompiledScanFindNeedle) {
  ScanHarness h;
  auto statement = h.compile("\"the quick brown fox\" ? tab(find(\"brown\"))");
  ASSERT_TRUE(statement) << (statement ? ""
                                       : formatDiagnostic(statement.error()));
  EXPECT_EQ(statement->resultType, (Type{"Str", {}}));
  h.run(*statement);
  EXPECT_TRUE(h.produced(*statement));
  EXPECT_EQ(h.arena.textOf(statement->result), "the quick ");
}

TEST(VAgentVIconScanTest, CompiledScanPosAndSubject) {
  ScanHarness h;
  auto statement1 = h.compile("\"abcdef\" ? (tab(4) & pos())");
  ASSERT_TRUE(statement1) << (statement1
                                  ? ""
                                  : formatDiagnostic(statement1.error()));
  EXPECT_EQ(statement1->resultType, (Type{"Int", {}}));
  h.run(*statement1);
  EXPECT_TRUE(h.produced(*statement1));
  EXPECT_EQ(h.arena.asInt64(statement1->result), 4);

  auto statement2 = h.compile("\"target\" ? subject()");
  ASSERT_TRUE(statement2) << (statement2
                                  ? ""
                                  : formatDiagnostic(statement2.error()));
  EXPECT_EQ(statement2->resultType, (Type{"Str", {}}));
  h.run(*statement2);
  EXPECT_TRUE(h.produced(*statement2));
  EXPECT_EQ(h.arena.textOf(statement2->result), "target");
}

TEST(VAgentVIconScanTest, CompiledScanBacktrackingRestoresPos) {
  ScanHarness h;
  // If first alternative fails, backtracking restores &pos to 1 so second
  // alternative succeeds!
  auto statement = h.compile(
      "\"target\" ? (tab(match(\"nomatch\")) | tab(match(\"target\")))");
  ASSERT_TRUE(statement) << (statement ? ""
                                       : formatDiagnostic(statement.error()));
  EXPECT_EQ(statement->resultType, (Type{"Str", {}}));
  h.run(*statement);
  EXPECT_TRUE(h.produced(*statement));
  EXPECT_EQ(h.arena.textOf(statement->result), "target");
}

TEST(VAgentVIconScanTest, CompiledNestedScanning) {
  ScanHarness h;
  // Nested scan restores the enclosing scan environment on exit
  auto statement = h.compile(
      "\"outer_string\" ? (tab(7) & (\"inner_string\" ? tab(6)) & tab(13))");
  ASSERT_TRUE(statement) << (statement ? ""
                                       : formatDiagnostic(statement.error()));
  EXPECT_EQ(statement->resultType, (Type{"Str", {}}));
  h.run(*statement);
  EXPECT_TRUE(h.produced(*statement));
  // tab(13) in outer_string from pos 7 produces "string"
  EXPECT_EQ(h.arena.textOf(statement->result), "string");
}

TEST(VAgentVIconScanTest, CompiledScanFailureUnwindsCleanly) {
  ScanHarness h;
  auto statement = h.compile("\"foo\" ? tab(match(\"bar\"))");
  ASSERT_TRUE(statement) << (statement ? ""
                                       : formatDiagnostic(statement.error()));
  h.run(*statement);
  // Failure inside bounded scan yields unproduced
  EXPECT_FALSE(h.produced(*statement));
  // Scan cursor was cleanly unlinked from d.scan
  EXPECT_FALSE(h.stdlib.scanCursor().has_value());
}

} // namespace

#include <gtest/gtest.h>

#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/vql_engine.hpp"

namespace {
using zigzag::vortex::OpcodeKind;

struct Result {
  std::vector<std::string> values;
  std::string error;
  bool emittedMatch{false};
};

Result evaluate(std::string_view query, bool compiled) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  xanadu::MultiStoreCoordinator coordinator(core);
  const auto title = coordinator.resolveDimension("d.title");
  const auto score = coordinator.resolveDimension("d.score");
  zigzag::expectWritten(
      arena.link(core.home(), title, false, arena.makeCell("paper")));
  zigzag::expectWritten(arena.link(core.home(), score, false,
                                   arena.makeScalarCell(std::int64_t{95})));
  const auto child = arena.makeCell("child");
  zigzag::expectWritten(arena.link(
      core.home(), coordinator.resolveDimension("d.child"), false, child));
  zigzag::expectWritten(
      arena.link(child, title, false, arena.makeCell("child title")));
  Result result;
  std::vector<zigzag::CellRef> cells;
  if (compiled) {
    zigzag::vortex::VortexVM vm(core);
    xanadu::vql::VQLCompiler compiler(coordinator, vm);
    const auto program = compiler.compile(query);
    if (!program.success) return {.error = program.errorMessage};
    const auto cursors = vm.activeCursors();
    if (cursors.empty()) return {.error = "compiled query has no cursor"};
    const auto execution = vm.run(cursors.back(), 10000);
    if (!execution.success) return {.error = execution.errorMessage};
    for (const auto opcode : program.generatedOpcodes) {
      if (vm.getOpcodeKind(opcode) == OpcodeKind::MatchContract) {
        result.emittedMatch = true;
      }
      if (vm.getOpcodeKind(opcode) != OpcodeKind::Halt) continue;
      for (const auto slot : core.inputsOf(opcode)) {
        const auto values = vm.resultCells(slot);
        cells.insert(cells.end(), values.begin(), values.end());
      }
    }
  } else {
    xanadu::vql::VQLEngine engine(coordinator);
    try {
      cells = engine.execute(query);
    } catch (const std::exception &error) {
      return {.error = error.what()};
    }
  }
  for (const auto cell : cells) {
    const auto rendered = core.render(cell);
    if (!rendered) continue;
    result.values.push_back(std::visit(
        [](const auto &value) -> std::string {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::same_as<T, std::string>)
            return value;
          else if constexpr (std::same_as<T, bool>)
            return value ? "true" : "false";
          else
            return std::to_string(value);
        },
        *rendered));
  }
  return result;
}

TEST(VQLTypeParityTest, PrimitiveCallsMatchAndFilterInBothBackends) {
  const std::vector<std::pair<std::string, std::vector<std::string>>> cases{
      {"def f($x: Int): Int := $x; f(7)", {"7"}},
      {"def f($x: Int): Int := $x; f(\"7\")", {"7"}},
      {"def f($x: Int): Int := $x; f(\"no\")", {}},
      {"def f($x: Int?): Int? := $x; f()", {}},
      {"def f($x: Stream<Int>): Stream<Int> := $x; f()", {}},
      {"def f($x: Bool): Bool := $x; f(true)", {"true"}},
      {"def f($x: String | Int): String | Int := $x; f(\"text\")", {"text"}},
      {"def inner($x: Int): Int := $x; def outer($x: Int): Int := inner($x); "
       "outer(9)",
       {"9"}},
      {"def f($p: Cell { +d.title: String, +d.score: $s: Int }): Int := $s; "
       "##/f()",
       {"95"}},
      {"def f({ +d.absent: $s: Int := 60 }): Int := $s; ##/f()", {"60"}},
      {"def f($x: Int): Int := $x; f(1); f(\"bad\")", {}},
      {"def read(): Cell := ./d.title; def outer($v: Cell): Cell := $v; "
       "##/d.child/outer(read())",
       {"child title"}},
  };
  for (const auto &[query, expected] : cases) {
    SCOPED_TRACE(query);
    const auto direct = evaluate(query, false);
    const auto vortex = evaluate(query, true);
    EXPECT_TRUE(direct.error.empty()) << direct.error;
    EXPECT_TRUE(vortex.error.empty()) << vortex.error;
    EXPECT_EQ(direct.values, expected);
    EXPECT_EQ(vortex.values, expected);
    EXPECT_TRUE(vortex.emittedMatch);
  }
}

TEST(VQLTypeParityTest, ReturnPromisesFailWithTheFunctionNameInBothBackends) {
  const auto query  = "def broken($x: Int): String := $x; broken(7)";
  const auto direct = evaluate(query, false);
  const auto vortex = evaluate(query, true);
  EXPECT_EQ(direct.error,
            "broken() returned a value outside its declared type");
  EXPECT_EQ(vortex.error, direct.error);
}

TEST(VQLTypeParityTest, UserDefinedCallsRejectNamedArgumentsInBothBackends) {
  const auto query  = "def f($x: Int): Int := $x; f(x: 7)";
  const auto direct = evaluate(query, false);
  const auto vortex = evaluate(query, true);
  const auto expected =
      "named arguments are not supported for user-defined function 'f'";
  EXPECT_EQ(direct.error, expected);
  EXPECT_EQ(vortex.error, expected);
}

TEST(VQLTypeParityTest, PredicateFunctionCallsUseTheTypedCallPath) {
  const auto query  = "def truth(): Bool := true; ##/d.child[truth()]";
  const auto direct = evaluate(query, false);
  const auto vortex = evaluate(query, true);
  EXPECT_TRUE(direct.error.empty()) << direct.error;
  EXPECT_TRUE(vortex.error.empty()) << vortex.error;
  EXPECT_EQ(direct.values, std::vector<std::string>{"child"});
  EXPECT_EQ(vortex.values, direct.values);
}
TEST(VQLTypeParityTest, TypedPredicatesFilterAndChainAtRuntime) {
  const std::vector<std::pair<std::string, std::vector<std::string>>> cases{
      {"def accept($x: Cell): Bool := true; ##/d.child[accept(.)]", {"child"}},
      {"def accept($x: Cell): Bool := false; ##/d.child[accept(.)]", {}},
      {"def accept($x: Cell): Bool := false; ##/d.child[not accept(.)]",
       {"child"}},
      {"def accept($x: Cell): Bool := false; ##/d.child[accept(.)]/d.title",
       {}},
      {"def accept($x: Cell): Bool := true; "
       "##/d.child[accept(.)][false]",
       {}},
      {"def accept($x: Cell): Bool := true; "
       "##/d.child[false][accept(.)]",
       {}},
  };
  for (const auto &[query, expected] : cases) {
    SCOPED_TRACE(query);
    const auto direct = evaluate(query, false);
    const auto vortex = evaluate(query, true);
    EXPECT_TRUE(direct.error.empty()) << direct.error;
    EXPECT_TRUE(vortex.error.empty()) << vortex.error;
    EXPECT_EQ(direct.values, expected);
    EXPECT_EQ(vortex.values, expected);
  }
}

TEST(VQLTypeParityTest, PredicateControlFlowShortCircuitsCalls) {
  const std::vector<std::pair<std::string, std::vector<std::string>>> cases{
      {"def broken(): Bool := 7; ##/d.child[false and broken()]", {}},
      {"def broken(): Bool := 7; ##/d.child[true or broken()]", {"child"}},
      {"def broken(): Bool := 7; ##/d.child[false][broken()]", {}},
      {"def broken(): Bool := 7; ##/d.missing[broken()]", {}},
      {"def accept($x: Cell): Bool := true; ##/d.child[accept(.) and true]",
       {"child"}},
  };
  for (const auto &[query, expected] : cases) {
    SCOPED_TRACE(query);
    const auto direct = evaluate(query, false);
    const auto vortex = evaluate(query, true);
    EXPECT_TRUE(direct.error.empty()) << direct.error;
    EXPECT_TRUE(vortex.error.empty()) << vortex.error;
    EXPECT_EQ(direct.values, expected);
    EXPECT_EQ(vortex.values, expected);
  }
  const auto query  = "def broken(): Bool := 7; ##/d.child[true and broken()]";
  const auto direct = evaluate(query, false);
  const auto vortex = evaluate(query, true);
  EXPECT_EQ(direct.error,
            "broken() returned a value outside its declared type");
  EXPECT_EQ(vortex.error, direct.error);
}

TEST(VQLTypeParityTest, OmittedParameterDefaultsRunLazilyAsVMInstructions) {
  const std::vector<std::pair<std::string, std::vector<std::string>>> cases{
      {"def f($x: Int, $y: Int := $x): Int := $y; f(12)", {"12"}},
      {"def inner($x: Int): Int := $x; "
       "def outer($x: Int := inner(9)): Int := $x; outer()",
       {"9"}},
      {"def f($x: Cell := ##/d.title): Cell := $x; f()", {"paper"}},
      {"def broken(): Int := \"bad\"; "
       "def f($x: Int := broken()): Int := $x; f(7)",
       {"7"}},
      {"def broken(): Int := \"bad\"; "
       "def f($x: Int, $y: Int := broken()): Int := $y; f(\"bad\")",
       {}},
      {"def f($x: Cell, $y: Cell := $x/d.title): Cell := $y; f(##)", {"paper"}},
  };
  for (const auto &[query, expected] : cases) {
    SCOPED_TRACE(query);
    const auto direct = evaluate(query, false);
    const auto vortex = evaluate(query, true);
    EXPECT_TRUE(direct.error.empty()) << direct.error;
    EXPECT_TRUE(vortex.error.empty()) << vortex.error;
    EXPECT_EQ(direct.values, expected);
    EXPECT_EQ(vortex.values, expected);
  }
  const auto query  = "def broken(): Int := \"bad\"; "
                      "def f($x: Int := broken()): Int := $x; f()";
  const auto direct = evaluate(query, false);
  const auto vortex = evaluate(query, true);
  EXPECT_EQ(direct.error,
            "broken() returned a value outside its declared type");
  EXPECT_EQ(vortex.error, direct.error);
}

TEST(VQLTypeParityTest, FunctorParametersCaptureCallerStepsAndCompose) {
  const std::vector<std::pair<std::string, std::vector<std::string>>> cases{
      {"def apply($c: Cell, $f): Cell := $c/$f; "
       "apply(##, /+d.child)",
       {"child"}},
      {"def apply($c: Cell, $f): Cell := $c/$f; "
       "let $down := /+d.child; apply(##, $down)",
       {"child"}},
      {"def apply($c: Cell, $f): Cell := $c/$f; "
       "let $down := /+d.child; let $title := /+d.title; "
       "apply(##, $down/$title)",
       {"child title"}},
      {"def apply($c: Cell, $f): Cell := $c/$f; "
       "apply(##, /+d.child[true])",
       {"child"}},
      {"def apply($c: Cell, $f): Cell? := $c/$f; "
       "apply(##, /+d.child[false])",
       {}},
      {"def inner($c: Cell, $g): Cell := $c/$g; "
       "def outer($c: Cell, $f): Cell := inner($c, $f); "
       "outer(##, /+d.child)",
       {"child"}},
      {"def apply($down, $f): Cell := ##/$f; "
       "let $down := /+d.title; apply(/+d.child, $down)",
       {"paper"}},
  };
  for (const auto &[query, expected] : cases) {
    SCOPED_TRACE(query);
    const auto direct = evaluate(query, false);
    const auto vortex = evaluate(query, true);
    EXPECT_TRUE(direct.error.empty()) << direct.error;
    EXPECT_TRUE(vortex.error.empty()) << vortex.error;
    EXPECT_EQ(direct.values, expected);
    EXPECT_EQ(vortex.values, expected);
  }
}

} // namespace

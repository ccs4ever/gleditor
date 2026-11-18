#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/vql/vql_engine.hpp"

namespace xanadu::vql {
namespace {

std::string example(const std::string &name) {
  const auto path =
      std::filesystem::path("tests/samples/vql/conformance") / name;
  std::ifstream input(path);
  if (!input)
    throw std::runtime_error("Cannot read VQL corpus: " + path.string());
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::vector<std::string> texts(const zigzag::ArenaManifold &arena,
                               const std::vector<zigzag::CellRef> &cells) {
  std::vector<std::string> result;
  for (const auto cell : cells) result.push_back(arena.textOf(cell));
  return result;
}

TEST(VQLConformanceTest, CommonEngineCorpusExecutesWithEqualResults) {
  struct Case {
    std::string file;
    std::vector<std::string> expected;
  };
  const std::vector<Case> cases{
      {"create_rank.vql", {"Alpha", "Beta"}},
      {"create_last.vql", {"Beta"}},
      {"create_fields.vql", {"Title"}},
      {"create_fields_last.vql", {"Title"}},
      {"create_fields_both.vql", {"Alpha", "Beta", "Title", "Title"}},
  };
  for (const auto &test : cases) {
    SCOPED_TRACE(test.file);
    const auto source = example(test.file);
    zigzag::ArenaManifold directArena;
    zigzag::vortex::VortexCore directCore(directArena);
    VQLEngine engine(directCore);
    EXPECT_EQ(texts(directArena, engine.execute(source)), test.expected);

    zigzag::ArenaManifold compiledArena;
    zigzag::vortex::VortexCore core(compiledArena);
    zigzag::vortex::VortexVM vm(core);
    VQLCompiler compiler(core, vm);
    const auto compiled = compiler.compile(source);
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    ASSERT_TRUE(compiled.cursor);
    const auto executed = vm.run(*compiled.cursor, 100000);
    ASSERT_TRUE(executed.success) << executed.errorMessage;
    std::vector<zigzag::CellRef> results;
    for (const auto slot : compiled.resultSlots) {
      const auto cells = vm.resultCells(slot);
      results.insert(results.end(), cells.begin(), cells.end());
    }
    EXPECT_EQ(texts(compiledArena, results), test.expected);
  }
}

TEST(VQLConformanceTest, HomeAnchorPreservesIdentityInBothEngines) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);
  EXPECT_EQ(engine.execute(example("home.vql")),
            std::vector<zigzag::CellRef>{core.home()});
  zigzag::vortex::VortexVM vm(core);
  VQLCompiler compiler(core, vm);
  const auto compiled = compiler.compile(example("home.vql"));
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_EQ(core.inputsOf(compiled.generatedOpcodes.back()),
            std::vector<zigzag::CellRef>{core.home()});
}

TEST(VQLConformanceTest, CompiledCreationPreservesExistingRankAtExecution) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);
  zigzag::vortex::VortexVM vm(core);
  VQLCompiler compiler(core, vm);
  (void)engine.execute("##/+d.items%\"Old\"");
  const auto compiled = compiler.compile(example("create_rank.vql"));
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  // A change after compilation must also be respected by the runtime append.
  (void)engine.execute("##/+d.items%\"Later\"");
  ASSERT_TRUE(compiled.cursor);
  ASSERT_TRUE(vm.run(*compiled.cursor, 100).success);
  EXPECT_EQ(texts(arena, engine.execute(example("rank_read.vql"))),
            (std::vector<std::string>{"Old", "Later", "Alpha", "Beta"}));
  ASSERT_TRUE(compiled.entryOpcode);
  const auto again    = vm.spawnCursor(*compiled.entryOpcode);
  const auto repeated = vm.run(again, 100);
  ASSERT_TRUE(repeated.success) << repeated.errorMessage;
  EXPECT_EQ(texts(arena, engine.execute(example("rank_read.vql"))),
            (std::vector<std::string>{"Old", "Later", "Alpha", "Beta", "Alpha",
                                      "Beta"}));
}

TEST(VQLConformanceTest, CompiledAppendRefusesCyclesWithoutBreakingLinks) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  VQLCompiler compiler(core, vm);
  const auto dim    = core.findOrMintDimension("d.items");
  const auto member = arena.makeCell("cycle");
  ASSERT_TRUE(arena.link(core.home(), dim, false, member));
  ASSERT_TRUE(arena.link(member, dim, false, core.home()));
  const auto compiled = compiler.compile(example("create_rank.vql"));
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  const auto result = vm.run(vm.activeCursors().front(), 100);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.errorMessage.find("cycle"), std::string::npos);
  EXPECT_EQ(arena.linked(core.home(), dim, false), member);
  EXPECT_EQ(arena.linked(member, dim, false), core.home());
}

TEST(VQLConformanceTest, AcceptedSyntaxDoesNotImplyCompiledSemantics) {
  const std::vector<std::string> compiledFiles{
      "rank_read.vql",        "filter_rank.vql", "multiple_statements.vql",
      "shape_projection.vql", "conditional.vql", "definition.vql",
  };
  for (const auto &file : compiledFiles) {
    SCOPED_TRACE(file);
    const auto source = example(file);
    ASSERT_NO_THROW((void)Parser(source).parseQuery());
    zigzag::ArenaManifold arena;
    zigzag::vortex::VortexCore core(arena);
    zigzag::vortex::VortexVM vm(core);
    VQLCompiler compiler(core, vm);
    const auto result = compiler.compile(source);
    EXPECT_TRUE(result.success) << result.errorMessage;
  }

  const std::vector<std::pair<std::string, std::string>> refusedFiles{
      {"generator.vql", "store generator"},
      {"watch.vql", "baseline"},
      {"version.vql", "microversion anchor"},
      {"functor.vql", "functor"},
  };
  for (const auto &[file, expectedSubstr] : refusedFiles) {
    SCOPED_TRACE(file);
    const auto source = example(file);
    ASSERT_NO_THROW((void)Parser(source).parseQuery());
    zigzag::ArenaManifold arena;
    zigzag::vortex::VortexCore core(arena);
    zigzag::vortex::VortexVM vm(core);
    VQLCompiler compiler(core, vm);
    const auto count  = arena.cellCount();
    const auto result = compiler.compile(source);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find(expectedSubstr), std::string::npos);
    EXPECT_TRUE(result.generatedOpcodes.empty());
    EXPECT_EQ(arena.cellCount(), count);
    EXPECT_TRUE(vm.activeCursors().empty());
  }
}

TEST(VQLConformanceTest, DirectCorpusReadsRanksAndFilters) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);
  (void)engine.execute(example("create_rank.vql"));
  EXPECT_EQ(texts(arena, engine.execute(example("rank_read.vql"))),
            (std::vector<std::string>{"Alpha", "Beta"}));
  EXPECT_EQ(texts(arena, engine.execute(example("filter_rank.vql"))),
            (std::vector<std::string>{"Alpha"}));
  EXPECT_EQ(engine.execute(example("empty_shape.vql")),
            std::vector<zigzag::CellRef>{core.home()});
}

TEST(VQLConformanceTest, RefusesUnboundContextAndInvalidProgrammaticCellIds) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  VQLCompiler compiler(core, vm);
  const auto bare = compiler.compile(".");
  EXPECT_FALSE(bare.success);
  EXPECT_NE(bare.errorMessage.find("no bound context"), std::string::npos)
      << bare.errorMessage;
  const auto stepped = compiler.compile("/d.1/d.2");
  EXPECT_FALSE(stepped.success);
  EXPECT_NE(stepped.errorMessage.find("no bound context"), std::string::npos)
      << stepped.errorMessage;
  for (const auto id : {std::uint64_t{0}, std::uint64_t{core.home()} +
                                              (std::uint64_t{1} << 32)}) {
    QueryExpression query;
    query.expr = PathExpression{
        .anchor = AnchorNode{.kind = AnchorKind::LiteralCellId, .cellId = id}};
    const auto count = arena.cellCount();
    EXPECT_FALSE(compiler.compile(query).success);
    EXPECT_EQ(arena.cellCount(), count);
  }
}

TEST(VQLConformanceTest, RejectsTrailingClosingBraceAndUnsupportedValueSyntax) {
  EXPECT_THROW((void)Parser("## } ignored").parseQuery(), ParseError);
  EXPECT_NO_THROW((void)Parser("let $x := f(name: 1) return ##").parseQuery());
  EXPECT_NO_THROW((void)Parser("let $x := 1 + 2 return ##").parseQuery());

  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  VQLCompiler compiler(core, vm);
  const auto resNamed = compiler.compile(
      "def f($x: Cell): Cell := $x; let $y := f(name: 1) return ##");
  EXPECT_FALSE(resNamed.success);
  EXPECT_NE(resNamed.errorMessage.find("named arguments"), std::string::npos);
  const auto resAdd = compiler.compile("let $x := 1 + 2 return ##");
  EXPECT_TRUE(resAdd.success) << resAdd.errorMessage;
}

} // namespace
} // namespace xanadu::vql

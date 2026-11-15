#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/contracts.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using xanadu::vql::CompilationOptions;
using xanadu::vql::CompilationResult;
using xanadu::vql::VQLCompiler;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::ExecutionResult;

struct CursorCleanup {
  zigzag::vortex::VortexVM &vm;
  CellRef cursor;

  ~CursorCleanup() {
    vm.abortScopedChoices(cursor);
    vm.setCursorOpcode(cursor, std::nullopt);
  }
};

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  xanadu::MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};

  CompilationResult
  compile(const std::string_view source,
          std::unordered_map<std::string, std::vector<CellRef>> bindings = {}) {
    VQLCompiler compiler(coordinator, vm);
    CompilationOptions options;
    options.initialBindings = std::move(bindings);
    return compiler.compile(source, options);
  }

  ExecutionResult execute(const CompilationResult &program,
                          const std::size_t maxCycles = 1'000'000) {
    if (!program.success || !program.cursor || !program.entryOpcode) {
      return {.success = false, .errorMessage = "no executable VQL graph"};
    }
    const CellRef cursor = *program.cursor;
    vm.setCursorOpcode(cursor, *program.entryOpcode);
    const CursorCleanup cleanup{vm, cursor};
    return vm.run(cursor, maxCycles);
  }

  std::vector<CellRef> results(const CompilationResult &program) const {
    std::vector<CellRef> cells;
    for (const CellRef slot : program.resultSlots) {
      const auto values = vm.resultCells(slot);
      cells.insert(cells.end(), values.begin(), values.end());
    }
    return cells;
  }

  DimRef dimension(const std::string_view name) {
    return core.findOrMintDimension(name);
  }

  std::vector<CellRef> rank(const std::string_view name,
                            const std::vector<std::string> &values) {
    const DimRef axis = dimension(name);
    CellRef previous  = core.home();
    std::vector<CellRef> cells;
    cells.reserve(values.size());
    for (const auto &value : values) {
      const CellRef cell = arena.makeCell(value);
      zigzag::expectWritten(arena.link(previous, axis, DimVector::POS, cell));
      cells.push_back(cell);
      previous = cell;
    }
    return cells;
  }

  bool boolResult(const CompilationResult &program) const {
    const auto values = results(program);
    return values.size() == 1 && core.isTruthy(values.front()).value_or(false);
  }
};

TEST(VQLQuantifiersVmRegression, RuntimeDomainStreamsUseEveryOccurrence) {
  Harness h;
  h.rank("d.items", {"miss", "hit", "tail"});

  auto any = h.compile(R"(any(##/d.items, . = "hit"))");
  ASSERT_TRUE(any.success) << any.errorMessage;
  ASSERT_TRUE(h.execute(any).success);
  EXPECT_TRUE(h.boolResult(any));

  auto all = h.compile(R"(all(##/d.items, . != "miss"))");
  ASSERT_TRUE(all.success) << all.errorMessage;
  ASSERT_TRUE(h.execute(all).success);
  EXPECT_FALSE(h.boolResult(all));

  auto none = h.compile(R"(none(##/d.items, . = "absent"))");
  ASSERT_TRUE(none.success) << none.errorMessage;
  ASSERT_TRUE(h.execute(none).success);
  EXPECT_TRUE(h.boolResult(none));
}

TEST(VQLQuantifiersVmRegression, EmptyDomainsUseQuantifierIdentities) {
  Harness h;
  const std::unordered_map<std::string, std::vector<CellRef>> bindings{
      {"empty", {}}};
  for (const auto &[source, expected] :
       std::vector<std::pair<std::string, bool>>{
           {"any($empty, true)", false},
           {"all($empty, false)", true},
           {"none($empty, true)", true}}) {
    auto program = h.compile(source, bindings);
    ASSERT_TRUE(program.success) << source << ": " << program.errorMessage;
    ASSERT_TRUE(h.execute(program).success) << source;
    EXPECT_EQ(h.boolResult(program), expected) << source;
  }
}

TEST(VQLQuantifiersVmRegression, LambdasFunctorsAndNestedRuntimeContextWork) {
  Harness h;
  const CellRef one = h.arena.makeScalarCell(std::int64_t{1});
  const CellRef two = h.arena.makeScalarCell(std::int64_t{2});
  const std::unordered_map<std::string, std::vector<CellRef>> bindings{
      {"items", {one, two}}};

  auto lambda = h.compile(R"(any($items, \($x) ($x = 2)))", bindings);
  ASSERT_TRUE(lambda.success) << lambda.errorMessage;
  ASSERT_TRUE(h.execute(lambda).success);
  EXPECT_TRUE(h.boolResult(lambda));

  auto functor = h.compile(
      R"(let $has_flag := /+d.flag return any($items, $has_flag))", bindings);
  ASSERT_TRUE(functor.success) << functor.errorMessage;
  ASSERT_TRUE(h.execute(functor).success);
  EXPECT_FALSE(h.boolResult(functor));

  const DimRef parents       = h.dimension("d.parents");
  const DimRef children      = h.dimension("d.children");
  const CellRef firstParent  = h.arena.makeCell("first parent");
  const CellRef secondParent = h.arena.makeCell("second parent");
  const CellRef firstChild   = h.arena.makeCell("miss");
  const CellRef secondChild  = h.arena.makeCell("needle");
  zigzag::expectWritten(
      h.arena.link(h.core.home(), parents, DimVector::POS, firstParent));
  zigzag::expectWritten(
      h.arena.link(firstParent, parents, DimVector::POS, secondParent));
  zigzag::expectWritten(
      h.arena.link(firstParent, children, DimVector::POS, firstChild));
  zigzag::expectWritten(
      h.arena.link(secondParent, children, DimVector::POS, secondChild));

  auto nested = h.compile(R"(any(##/d.parents, ./+d.children[. = "needle"]))");
  ASSERT_TRUE(nested.success) << nested.errorMessage;
  ASSERT_TRUE(h.execute(nested).success) << nested.errorMessage;
  EXPECT_TRUE(h.boolResult(nested));
}

TEST(VQLQuantifiersVmRegression, LambdaParametersUseSharedContractBinding) {
  Harness h;
  const auto items  = h.rank("d.items", {"hit", "miss"});
  const DimRef flag = h.dimension("d.flag");
  const CellRef yes = h.arena.makeCell("yes");
  zigzag::expectWritten(h.arena.link(items.front(), flag, DimVector::POS, yes));
  const std::unordered_map<std::string, std::vector<CellRef>> bindings{
      {"items", items}};

  auto typed = h.compile(R"(any($items, \($x: Cell) ($x = "hit")))", bindings);
  ASSERT_TRUE(typed.success) << typed.errorMessage;
  ASSERT_TRUE(h.execute(typed).success) << typed.errorMessage;
  EXPECT_TRUE(h.boolResult(typed));

  auto filtered = h.compile(R"(any($items, \($x: Int) true))", bindings);
  ASSERT_TRUE(filtered.success) << filtered.errorMessage;
  ASSERT_TRUE(h.execute(filtered).success) << filtered.errorMessage;
  EXPECT_FALSE(h.boolResult(filtered));

  auto destructured = h.compile(
      R"(any($items, \({ ., +d.flag: $value }) ($value = "yes")))", bindings);
  ASSERT_TRUE(destructured.success) << destructured.errorMessage;
  ASSERT_TRUE(h.execute(destructured).success) << destructured.errorMessage;
  EXPECT_TRUE(h.boolResult(destructured));
}

TEST(VQLQuantifiersVmRegression, ScalarLambdaPreservesFalseResultTruthiness) {
  Harness h;
  const auto items = h.rank("d.items", {"candidate"});
  const std::unordered_map<std::string, std::vector<CellRef>> bindings{
      {"items", items}};

  auto program = h.compile(R"(any($items, \($x) false))", bindings);
  ASSERT_TRUE(program.success) << program.errorMessage;
  ASSERT_TRUE(h.execute(program).success) << program.errorMessage;
  EXPECT_FALSE(h.boolResult(program));
}

TEST(VQLQuantifiersVmRegression, UnsupportedLambdaShapesRefuseExplicitly) {
  Harness h;
  const CellRef item = h.arena.makeCell("item");
  const std::unordered_map<std::string, std::vector<CellRef>> bindings{
      {"empty", {}}, {"items", {item}}};

  const auto before = h.arena.cellCount();
  try {
    static_cast<void>(h.compile(
        R"(any($empty, \($x: QuantifierUnknownType) true))", bindings));
    FAIL() << "an empty domain hid an invalid formal type";
  } catch (const xanadu::vql::TypeContractError &error) {
    EXPECT_NE(std::string_view(error.what()).find("QuantifierUnknownType"),
              std::string_view::npos);
  }
  EXPECT_EQ(h.arena.cellCount(), before);

  for (const auto source :
       {R"(any($empty, \($x, $y) true))", R"(any($empty, \($xs...) true))"}) {
    const auto malformed = h.compile(source, bindings);
    EXPECT_FALSE(malformed.success) << source;
    EXPECT_NE(malformed.errorMessage.find("nonvariadic formal"),
              std::string::npos)
        << source;
  }

  const auto block =
      h.compile(R"(any($items, \($x) { return $x; }))", bindings);
  EXPECT_FALSE(block.success);
  EXPECT_NE(block.errorMessage.find("block bodies are unsupported"),
            std::string::npos);
}

TEST(VQLQuantifiersVmRegression, QuantifierShortCircuitSkipsLaterCreation) {
  Harness h;
  const auto items   = h.rank("d.items", {"hit", "tail"});
  const DimRef marks = h.dimension("d.marks");
  auto program =
      h.compile(R"(any(##/d.items, \($x) ($x = "hit" and $x/d.marks%"seen")))");
  ASSERT_TRUE(program.success) << program.errorMessage;
  ASSERT_TRUE(h.execute(program).success) << program.errorMessage;
  EXPECT_TRUE(h.boolResult(program));
  EXPECT_TRUE(h.arena.linked(items[0], marks, DimVector::POS));
  EXPECT_FALSE(h.arena.linked(items[1], marks, DimVector::POS));

  Harness allHarness;
  const auto allItems   = allHarness.rank("d.items", {"miss", "hit"});
  const DimRef allMarks = allHarness.dimension("d.marks");
  auto all              = allHarness.compile(
      R"(all(##/d.items, \($x) ($x != "miss" and $x/d.marks%"seen")))");
  ASSERT_TRUE(all.success) << all.errorMessage;
  ASSERT_TRUE(allHarness.execute(all).success) << all.errorMessage;
  EXPECT_FALSE(allHarness.boolResult(all));
  EXPECT_FALSE(allHarness.arena.linked(allItems[0], allMarks, DimVector::POS));
  EXPECT_FALSE(allHarness.arena.linked(allItems[1], allMarks, DimVector::POS));

  Harness noneHarness;
  const auto noneItems   = noneHarness.rank("d.items", {"hit", "tail"});
  const DimRef noneMarks = noneHarness.dimension("d.marks");
  auto none              = noneHarness.compile(
      R"(none(##/d.items, \($x) ($x = "hit" or $x/d.marks%"seen")))");
  ASSERT_TRUE(none.success) << none.errorMessage;
  ASSERT_TRUE(noneHarness.execute(none).success) << none.errorMessage;
  EXPECT_FALSE(noneHarness.boolResult(none));
  EXPECT_FALSE(
      noneHarness.arena.linked(noneItems[1], noneMarks, DimVector::POS));
}

TEST(VQLQuantifiersVmRegression, CardinalityAndFirstExtractAtRuntime) {
  Harness h;
  const auto values = h.rank("d.items", {"first", "second"});
  const std::unordered_map<std::string, std::vector<CellRef>> bindings{
      {"empty", {}},
      {"only", {values.front()}},
      {"ordered", values},
      {"duplicates", {values[1], values[0], values[1]}}};

  auto first = h.compile("first(##/d.items)");
  ASSERT_TRUE(first.success) << first.errorMessage;
  ASSERT_TRUE(h.execute(first).success);
  EXPECT_EQ(h.results(first), (std::vector<CellRef>{values.front()}));

  auto singleton = h.compile("one($only)", bindings);
  ASSERT_TRUE(singleton.success) << singleton.errorMessage;
  ASSERT_TRUE(h.execute(singleton).success);
  EXPECT_EQ(h.results(singleton), (std::vector<CellRef>{values.front()}));

  auto optionalEmpty = h.compile("optional_one($empty)", bindings);
  ASSERT_TRUE(optionalEmpty.success) << optionalEmpty.errorMessage;
  ASSERT_TRUE(h.execute(optionalEmpty).success);
  EXPECT_TRUE(h.results(optionalEmpty).empty());

  auto requireOneEmpty = h.compile("one($empty)", bindings);
  ASSERT_TRUE(requireOneEmpty.success) << requireOneEmpty.errorMessage;
  const auto missing = h.execute(requireOneEmpty);
  EXPECT_FALSE(missing.success);
  EXPECT_NE(missing.errorMessage.find("one cardinality"), std::string::npos);

  auto optionalMany = h.compile("optional_one($ordered)", bindings);
  ASSERT_TRUE(optionalMany.success) << optionalMany.errorMessage;
  const auto multiple = h.execute(optionalMany);
  EXPECT_FALSE(multiple.success);
  EXPECT_NE(multiple.errorMessage.find("optional_one cardinality"),
            std::string::npos);

  auto firstDuplicate = h.compile("first($duplicates)", bindings);
  ASSERT_TRUE(firstDuplicate.success) << firstDuplicate.errorMessage;
  ASSERT_TRUE(h.execute(firstDuplicate).success);
  EXPECT_EQ(h.results(firstDuplicate), (std::vector<CellRef>{values[1]}));

  auto oneDuplicate = h.compile("one($duplicates)", bindings);
  ASSERT_TRUE(oneDuplicate.success) << oneDuplicate.errorMessage;
  EXPECT_FALSE(h.execute(oneDuplicate).success);

  // A failed cardinality assertion releases the cursor and its choice state;
  // the same VM can execute a new graph immediately afterwards.
  auto retry = h.compile(R"(any(##/d.items, . = "second"))");
  ASSERT_TRUE(retry.success) << retry.errorMessage;
  ASSERT_TRUE(h.execute(retry).success) << retry.errorMessage;
  EXPECT_TRUE(h.boolResult(retry));
}

TEST(VQLQuantifiersVmRegression, SameCellChecksRuntimeSingletonIdentity) {
  Harness h;
  const CellRef first  = h.arena.makeCell("same");
  const CellRef second = h.arena.makeCell("same");
  const std::unordered_map<std::string, std::vector<CellRef>> bindings{
      {"a", {first}},
      {"b", {second}},
      {"many", {first, second}},
      {"empty", {}}};

  auto equalContent = h.compile("same_cell($a, $b)", bindings);
  ASSERT_TRUE(equalContent.success) << equalContent.errorMessage;
  ASSERT_TRUE(h.execute(equalContent).success);
  EXPECT_FALSE(h.boolResult(equalContent));

  auto sameIdentity = h.compile("same_cell($a, $a)", bindings);
  ASSERT_TRUE(sameIdentity.success) << sameIdentity.errorMessage;
  ASSERT_TRUE(h.execute(sameIdentity).success);
  EXPECT_TRUE(h.boolResult(sameIdentity));

  for (const std::string source :
       {"same_cell($many, $a)", "same_cell($empty, $a)"}) {
    auto invalid = h.compile(source, bindings);
    ASSERT_TRUE(invalid.success) << invalid.errorMessage;
    EXPECT_FALSE(h.execute(invalid).success) << source;
  }
}

TEST(VQLQuantifiersVmRegression, GraphSizeDoesNotScaleWithRuntimeRankLength) {
  const auto graphSize = [](const std::size_t memberCount) {
    Harness h;
    std::vector<std::string> values(memberCount, "member");
    h.rank("d.items", values);
    const auto program = h.compile(R"(any(##/d.items, . = "target"))");
    EXPECT_TRUE(program.success) << program.errorMessage;
    return program.generatedOpcodes.size();
  };

  EXPECT_EQ(graphSize(1), graphSize(1'000));
}

TEST(VQLQuantifiersVmRegression, ReexecutingOneGraphReadsChangedRankContents) {
  Harness h;
  const auto values = h.rank("d.items", {"miss", "tail"});
  auto program      = h.compile(R"(any(##/d.items, . = "hit"))");
  ASSERT_TRUE(program.success) << program.errorMessage;
  ASSERT_TRUE(h.execute(program).success);
  EXPECT_FALSE(h.boolResult(program));

  zigzag::expectWritten(
      h.arena.unlink(h.core.home(), h.dimension("d.items"), DimVector::POS));
  zigzag::expectWritten(
      h.arena.unlink(values[0], h.dimension("d.items"), DimVector::POS));
  const CellRef replacement = h.arena.makeCell("hit");
  zigzag::expectWritten(h.arena.link(h.core.home(), h.dimension("d.items"),
                                     DimVector::POS, replacement));

  ASSERT_TRUE(h.execute(program).success);
  EXPECT_TRUE(h.boolResult(program));
}

} // namespace

#include <cstdint>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/xanadu/vortex/mesh_evaluator.hpp"
#include "common/xanadu/vortex/vortex_host.hpp"

namespace {
using xanadu::vql::QueryExecutionMode;
using zigzag::vortex::VortexHost;

TEST(VQLHostParityTest, QueryNavigationScriptAndBatchUseTheRequestedBackend) {
  for (const auto mode : {QueryExecutionMode::Vortex}) {
    SCOPED_TRACE(mode == QueryExecutionMode::Direct ? "direct" : "vortex");
    VortexHost host;
    host.setVQLExecutionOptions({.mode = mode});
    const auto query =
        host.executeVQLChecked("def f($x: Int): Int := $x; f(7)");
    ASSERT_TRUE(query) << query.error().message;
    ASSERT_EQ(query->size(), 1u);
    EXPECT_EQ(host.core().arena().asInt64(query->front()), 7);
    const auto target = host.core().arena().makeCell("focus");
    const auto destination =
        host.navigatePath("def f($x: Cell): Cell := $x; f(.)", target);
    EXPECT_EQ(destination, target);
    const auto script =
        host.executeScript("def broken($x: Int): String := $x; broken(7)");
    EXPECT_FALSE(script.success);
    EXPECT_EQ(script.message,
              "broken() returned a value outside its declared type");
    const auto rows = host.executeVQLBatchParallel(
        {"def f($x: Int): Int := $x; f(7)"},
        [](zigzag::vortex::WorkerContext &, std::size_t) {});
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows.front(), std::vector<std::string>{"7"});
  }
}
TEST(VQLHostParityTest, RuntimeRanksFlowThroughQueryNavigationScriptAndBatch) {
  const std::string query =
      "def identity($x: Int): Int := $x; ##/d.items/identity()";
  for (const auto mode : {QueryExecutionMode::Vortex}) {
    VortexHost host;
    host.setVQLExecutionOptions({.mode = mode});
    const auto seed = [](auto &core) {
      auto &arena      = core.arena();
      const auto axis  = core.mintDimension("d.items");
      const auto first = arena.makeScalarCell(std::int64_t{7});
      const auto bad   = arena.makeCell("bad");
      const auto last  = arena.makeScalarCell(std::int64_t{9});
      zigzag::expectWritten(arena.link(core.home(), axis, false, first));
      zigzag::expectWritten(arena.link(first, axis, false, bad));
      zigzag::expectWritten(arena.link(bad, axis, false, last));
      return std::vector<zigzag::CellRef>{first, last};
    };
    const auto expected = seed(host.core());
    const auto results  = host.executeVQLChecked(query);
    ASSERT_TRUE(results) << results.error().message;
    EXPECT_EQ(*results, expected);
    EXPECT_EQ(host.navigatePath(query + "[-1]", host.core().home()),
              expected.back());
    EXPECT_EQ(host.navigatePath("##/d.items::tail", host.core().home()),
              expected.back());
    const auto script = host.executeScript(query);
    EXPECT_TRUE(script.success) << script.message;
    const std::string decorated =
        "let $f := /d.items; let $g := /$f[. != \"bad\"][-1]; "
        "let $f := /d.absent; ##/$g";
    const auto functorResult = host.executeVQLChecked(decorated);
    ASSERT_TRUE(functorResult) << functorResult.error().message;
    EXPECT_EQ(*functorResult, (std::vector<zigzag::CellRef>{expected.back()}));
    EXPECT_EQ(host.navigatePath(decorated, host.core().home()),
              expected.back());
    const auto functorScript = host.executeScript(decorated);
    ASSERT_TRUE(functorScript.success) << functorScript.message;
    EXPECT_EQ(functorScript.affectedCells,
              (std::vector<zigzag::CellRef>{expected.back()}));
    const auto rows = host.executeVQLBatchParallel(
        {query, decorated}, [&](zigzag::vortex::WorkerContext &worker,
                                std::size_t) { seed(worker.core); });
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows.front(), (std::vector<std::string>{"7", "9"}));
    EXPECT_EQ(rows.back(), (std::vector<std::string>{"9"}));
  }
}

TEST(VQLHostParityTest,
     TypedVariadicCallsFlowThroughQueryNavigationScriptAndBatch) {
  const std::string success =
      "def collect(...$rest: Int): Stream<Int> := $rest; collect(7, 9, 7)";
  const std::string mismatch =
      "def collect(...$rest: Int): Stream<Int> := $rest; "
      "collect(7, 9, \"bad\")";
  const std::string brokenReturn =
      "def broken(...$rest: Int): String := $rest; broken(7, 9)";

  for (const auto mode : {QueryExecutionMode::Vortex}) {
    SCOPED_TRACE(mode == QueryExecutionMode::Direct ? "direct" : "vortex");
    VortexHost host;
    host.setVQLExecutionOptions({.mode = mode});

    const auto query = host.executeVQLChecked(success);
    ASSERT_TRUE(query) << query.error().message;
    std::vector<std::int64_t> actual;
    for (const auto cell : *query) {
      const auto value = host.core().arena().asInt64(cell);
      ASSERT_TRUE(value);
      actual.push_back(*value);
    }
    EXPECT_EQ(actual, (std::vector<std::int64_t>{7, 9, 7}));

    const auto rejected = host.executeVQLChecked(mismatch);
    ASSERT_TRUE(rejected) << rejected.error().message;
    EXPECT_TRUE(rejected->empty());

    const auto target = host.core().arena().makeCell("context");
    EXPECT_EQ(
        host.navigatePath("def from_context(...$rest: Cell): Cell := $rest; "
                          "from_context(.)",
                          target),
        target);

    const auto script = host.executeScript(success);
    ASSERT_TRUE(script.success) << script.message;
    ASSERT_EQ(script.affectedCells.size(), 3u);
    std::vector<std::int64_t> scriptValues;
    for (const auto cell : script.affectedCells) {
      const auto value = host.core().arena().asInt64(cell);
      ASSERT_TRUE(value);
      scriptValues.push_back(*value);
    }
    EXPECT_EQ(scriptValues, (std::vector<std::int64_t>{7, 9, 7}));
    const auto failedScript = host.executeScript(brokenReturn);
    EXPECT_FALSE(failedScript.success);
    EXPECT_EQ(failedScript.message,
              "broken() returned a value outside its declared type");

    const auto rows = host.executeVQLBatchParallel(
        {success, mismatch},
        [](zigzag::vortex::WorkerContext &, std::size_t) {});
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0], (std::vector<std::string>{"7", "9", "7"}));
    EXPECT_TRUE(rows[1].empty());
  }
}

} // namespace

TEST(VQLHostParityTest, ForContractsRunThroughEveryHostAndMeshRoute) {
  const std::string source =
      "let $p := %\"paper\"; $p/d.items%\"7\"%\"bad\"%\"9\"; "
      "for { .: $item: Int } in $p/d.items where $item != \"9\" return $item";
  VortexHost host;
  const auto query = host.executeVQLChecked(source);
  ASSERT_TRUE(query) << query.error().message;
  ASSERT_EQ(query->size(), 1u);
  EXPECT_EQ(host.core().arena().textOf(query->front()), "7");
  const auto destination = host.navigatePath(source, host.core().home());
  ASSERT_TRUE(destination);
  EXPECT_EQ(host.core().arena().textOf(*destination), "7");
  const auto script = host.executeScript(source);
  ASSERT_TRUE(script.success) << script.message;
  ASSERT_EQ(script.affectedCells.size(), 1u);
  EXPECT_EQ(host.core().arena().textOf(script.affectedCells.front()), "7");
  const auto rows = host.executeVQLBatchParallel(
      {source}, [](zigzag::vortex::WorkerContext &, std::size_t) {});
  EXPECT_EQ(rows, (std::vector<std::vector<std::string>>{{"7"}}));

  namespace mesh = zigzag::vortex::mesh;
  auto remote    = mesh::evaluate(mesh::Frontend::Vquery, source,
                                  {.maxCycles      = 100000,
                                   .maxCells       = 100000,
                                   .maxSolutions   = 16,
                                   .maxOutputBytes = 1024 * 1024});
  ASSERT_TRUE(remote) << remote.error();
  auto &arena = remote->context->arena;
  std::optional<zigzag::DimRef> items, value;
  for (const auto &cell : arena.cells()) {
    if (arena.textOf(cell.birthOp) == "d.mesh.items") items = cell.birthOp;
    if (arena.textOf(cell.birthOp) == "d.mesh.value") value = cell.birthOp;
  }
  ASSERT_TRUE(items);
  ASSERT_TRUE(value);
  const auto record =
      arena.linked(remote->root, *items, zigzag::DimVector::POS);
  ASSERT_TRUE(record);
  const auto result = arena.linked(*record, *value, zigzag::DimVector::POS);
  ASSERT_TRUE(result);
  EXPECT_EQ(arena.textOf(*result), "7");
  EXPECT_FALSE(arena.linked(*record, *items, zigzag::DimVector::POS));
}

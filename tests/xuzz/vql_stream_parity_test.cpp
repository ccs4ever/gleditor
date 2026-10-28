#include <gtest/gtest.h>

#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/vql_engine.hpp"

namespace xanadu::vql {
namespace {
using zigzag::CellRef;
using zigzag::DimVector;

struct StreamHarness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  VQLEngine engine{coordinator};
  zigzag::vortex::VortexVM vm{core};
  const zigzag::DimRef items = coordinator.resolveDimension("d.items");
  const CellRef first        = arena.makeScalarCell(std::int64_t{7});
  const CellRef bad          = arena.makeCell("bad");
  const CellRef last         = arena.makeScalarCell(std::int64_t{9});

  StreamHarness() {
    zigzag::expectWritten(
        arena.link(core.home(), items, DimVector::POS, first));
    zigzag::expectWritten(arena.link(first, items, DimVector::POS, bad));
    zigzag::expectWritten(arena.link(bad, items, DimVector::POS, last));
  }
};

TEST(VQLStreamParityTest, RanksAndTypedMemberCallsPreserveOrderAndIdentity) {
  for (const auto mode :
       {QueryExecutionMode::Direct, QueryExecutionMode::Vortex}) {
    StreamHarness h;
    QueryExecutor executor(h.engine, h.vm, {.mode = mode});
    for (
        const auto &[query, expected] :
        std::vector<std::pair<std::string, std::vector<CellRef>>>{
            {"##/d.items", {h.first, h.bad, h.last}},
            {"##/d.items[. != \"bad\"]", {h.first, h.last}},
            {"def identity($x: Int): Int := $x; ##/d.items/identity()",
             {h.first, h.last}},
            {"def identity($x: Cell): Cell := $x; ##/d.items/identity()",
             {h.first, h.bad, h.last}},
            {"def inner($x: Int): Int := $x; "
             "def outer($x: Int): Int := inner($x); ##/d.items/outer()",
             {h.first, h.last}},
            {"def one($xs: Cell): Cell := $xs; one(##/d.items)", {}},
            {"def all($xs: Stream<Cell>): Stream<Cell> := $xs; all(##/d.items)",
             {h.first, h.bad, h.last}},
            {"def ints($xs: Stream<Int>): Stream<Int> := $xs; ints(##/d.items)",
             {}},
            {"##/d.absent", {}},
        }) {
      SCOPED_TRACE(query);
      const auto result = executor.execute(query);
      ASSERT_TRUE(result) << result.error().message;
      EXPECT_EQ(*result, expected);
    }
  }
}

TEST(VQLStreamParityTest, ContextStreamsPreserveDuplicateMembers) {
  for (const auto mode :
       {QueryExecutionMode::Direct, QueryExecutionMode::Vortex}) {
    StreamHarness h;
    QueryExecutor executor(h.engine, h.vm, {.mode = mode});
    const auto result = executor.execute(
        "def identity($x: Int): Int := $x; ./d.items/identity()",
        {h.core.home(), h.first});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(*result, (std::vector<CellRef>{h.first, h.last, h.last}));
  }
}

TEST(VQLStreamParityTest, EmptyRankCallsAndReturnFailuresMatch) {
  for (const auto mode :
       {QueryExecutionMode::Direct, QueryExecutionMode::Vortex}) {
    StreamHarness h;
    QueryExecutor executor(h.engine, h.vm, {.mode = mode});
    const auto empty =
        executor.execute("def f($x: Cell): Cell := $x; ##/d.absent/f()");
    ASSERT_TRUE(empty) << empty.error().message;
    EXPECT_TRUE(empty->empty());
    const auto contextFree =
        executor.execute("def f(): Int := 42; ##/d.absent/f()");
    ASSERT_TRUE(contextFree) << contextFree.error().message;
    ASSERT_EQ(contextFree->size(), 1u);
    EXPECT_EQ(h.arena.asInt64(contextFree->front()), 42);
    const auto fallbackStream = executor.execute(
        "def all(): Stream<Cell> := ##/d.items; ##/d.absent/all()");
    ASSERT_TRUE(fallbackStream) << fallbackStream.error().message;
    EXPECT_EQ(*fallbackStream, (std::vector<CellRef>{h.first, h.bad, h.last}));
    const auto broken = executor.execute(
        "def broken($x: Int): String := $x; ##/d.items/broken()");
    ASSERT_FALSE(broken);
    EXPECT_EQ(broken.error().category,
              QueryExecutionError::Category::TypeContract);
    EXPECT_EQ(broken.error().message,
              "broken() returned a value outside its declared type");
    const auto recovered = executor.execute("##/d.items");
    ASSERT_TRUE(recovered) << recovered.error().message;
    EXPECT_EQ(*recovered, (std::vector<CellRef>{h.first, h.bad, h.last}));
  }
}

TEST(VQLStreamParityTest, StreamPresenceDiffersFromScalarFirstValueTruthiness) {
  for (const auto mode :
       {QueryExecutionMode::Direct, QueryExecutionMode::Vortex}) {
    StreamHarness h;
    const auto axis = h.coordinator.resolveDimension("d.flags");
    const auto no   = h.arena.makeScalarCell(false);
    const auto yes  = h.arena.makeScalarCell(true);
    zigzag::expectWritten(h.arena.link(h.first, axis, DimVector::POS, no));
    zigzag::expectWritten(h.arena.link(no, axis, DimVector::POS, yes));
    QueryExecutor executor(h.engine, h.vm, {.mode = mode});
    const auto present = executor.execute("##/d.items[./d.flags]");
    ASSERT_TRUE(present) << present.error().message;
    EXPECT_EQ(*present, (std::vector<CellRef>{h.first}));
    const auto scalar = executor.execute("def flags($x: Cell): Stream<Bool> := "
                                         "$x/d.flags; ##/d.items[flags(.)]");
    ASSERT_TRUE(scalar) << scalar.error().message;
    EXPECT_TRUE(scalar->empty());
  }
}

TEST(VQLStreamParityTest, RuntimeClampsCountFilteredMembersAndResetPerCall) {
  for (const auto mode :
       {QueryExecutionMode::Direct, QueryExecutionMode::Vortex}) {
    StreamHarness h;
    QueryExecutor executor(h.engine, h.vm, {.mode = mode});
    for (const auto &[query, expected] :
         std::vector<std::pair<std::string, std::vector<CellRef>>>{
             {"##/d.items[1]", {h.first}},
             {"##/d.items[-1]", {h.last}},
             {"##/d.items[2, 3]", {h.bad, h.last}},
             {"##/d.items[1, -2]", {h.first, h.bad}},
             {"##/d.items[3, 1]", {}},
             {"##/d.items[0]", {h.first}},
             {"##/d.items[99]", {h.last}},
             {"##/d.items[-99]", {h.first}},
             {"##/d.items[false][-1]", {}},
             {"##/d.items[. != \"bad\"][-1]", {h.last}},
             {"def identity($x: Int): Int := $x; ##/d.items/identity()[-1]",
              {h.last}},
             {"let $pick := /+d.items[. != \"bad\"][-1]; ##/$pick", {h.last}},
         }) {
      SCOPED_TRACE(query);
      const auto result = executor.execute(query);
      ASSERT_TRUE(result) << result.error().message;
      EXPECT_EQ(*result, expected);
    }
    const auto repeated = executor.execute(
        "def pick($x: Cell): Cell? := $x/d.items[-1]; ./pick()",
        {h.core.home(), h.first});
    ASSERT_TRUE(repeated) << repeated.error().message;
    EXPECT_EQ(*repeated, (std::vector<CellRef>{h.last, h.last}));
  }
}

TEST(VQLStreamParityTest, PlacementAndBidirectionalTraversalMatch) {
  for (const auto mode :
       {QueryExecutionMode::Direct, QueryExecutionMode::Vortex}) {
    StreamHarness h;
    QueryExecutor executor(h.engine, h.vm, {.mode = mode});
    for (const auto &[query, expected] :
         std::vector<std::pair<std::string, std::vector<CellRef>>>{
             {"./d.items::from", {h.last}},
             {"./d.items::rank", {h.core.home(), h.first, h.bad, h.last}},
             {"./-d.items::rank", {h.last, h.bad, h.first, h.core.home()}},
             {"./d.absent::rank", {h.bad}},
             {"./~d.items", {h.last, h.first, h.core.home()}},
             {"./-d.items::head", {h.core.home()}},
             {"./-d.items::tail", {h.last}},
             {"./d.absent::head", {h.bad}},
             {"./d.items[-1]!keep", {h.bad}},
             {"./d.items[false]!keep", {h.bad}},
             {"./d.items::tail[-1]", {h.last}},
         }) {
      SCOPED_TRACE(query);
      const auto result = executor.execute(query, {h.bad});
      ASSERT_TRUE(result) << result.error().message;
      EXPECT_EQ(*result, expected);
    }
    zigzag::expectWritten(
        h.arena.link(h.last, h.items, DimVector::POS, h.core.home()));
    const auto ring = executor.execute("./~d.items", {h.bad});
    ASSERT_TRUE(ring) << ring.error().message;
    EXPECT_EQ(*ring, (std::vector<CellRef>{h.last, h.core.home(), h.first,
                                           h.first, h.core.home(), h.last}));
    const auto whole = executor.execute("./-d.items::rank", {h.bad});
    ASSERT_TRUE(whole) << whole.error().message;
    EXPECT_EQ(*whole,
              (std::vector<CellRef>{h.bad, h.first, h.core.home(), h.last}));
  }
}

TEST(VQLStreamParityTest, DefaultCreatesAppendAndMintPerRuntimeInvocation) {
  for (const auto mode :
       {QueryExecutionMode::Direct, QueryExecutionMode::Vortex}) {
    StreamHarness h;
    QueryExecutor executor(h.engine, h.vm, {.mode = mode});
    const auto appended = executor.execute("##/-d.items%\"new\"");
    ASSERT_TRUE(appended) << appended.error().message;
    ASSERT_EQ(appended->size(), 1u);
    const auto rank = executor.execute("##/d.items");
    ASSERT_TRUE(rank) << rank.error().message;
    EXPECT_EQ(*rank, (std::vector<CellRef>{h.first, h.bad, h.last,
                                           appended->front()}));
    const auto perMember =
        executor.execute("./d.children%\"child\"", {h.first, h.last});
    ASSERT_TRUE(perMember) << perMember.error().message;
    ASSERT_EQ(perMember->size(), 2u);
    EXPECT_NE(perMember->front(), perMember->back());
    EXPECT_EQ(h.arena.linked(h.first,
                             h.coordinator.resolveDimension("d.children"),
                             DimVector::POS),
              perMember->front());
    EXPECT_EQ(h.arena.linked(h.last,
                             h.coordinator.resolveDimension("d.children"),
                             DimVector::POS),
              perMember->back());
  }
}

TEST(VQLStreamParityTest, UnsupportedVersionPinsRefuseEverySelectorKind) {
  StreamHarness h;
  QueryExecutor executor(h.engine, h.vm, {.mode = QueryExecutionMode::Vortex});
  for (const auto query :
       {"##@(88)", "##/d.items@(88)", "def f(): Int := 1; ##/f()@(88)",
        "let $f := /d.items@(88); ##/$f"}) {
    SCOPED_TRACE(query);
    const auto result = executor.execute(query);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().category, QueryExecutionError::Category::Compile);
    EXPECT_NE(result.error().message.find("version pins"), std::string::npos);
  }
}

TEST(VQLStreamParityTest, DecoratedFunctorsApplyModifiersAfterCapturedPath) {
  for (const auto mode :
       {QueryExecutionMode::Direct, QueryExecutionMode::Vortex}) {
    StreamHarness h;
    const auto out    = h.coordinator.resolveDimension("d.out");
    const auto target = h.arena.makeScalarCell(std::int64_t{11});
    zigzag::expectWritten(h.arena.link(h.bad, out, DimVector::POS, target));
    QueryExecutor executor(h.engine, h.vm, {.mode = mode});
    for (const auto &[query, expected] :
         std::vector<std::pair<std::string, std::vector<CellRef>>>{
             {"let $f := /d.items; ##/$f[. != \"bad\"][-1]", {h.last}},
             {"let $f := /d.items/d.out; ##/$f[. = 11]", {target}},
             {"let $f := /d.items; ##/$f[false]!keep", {h.core.home()}},
             {"let $f := /d.absent; ##/$f[-1]", {}},
             {"let $f := /d.items; ##/$f>[. != \"bad\"][-1]", {h.last}},
             {"let $f := /d.items; let $g := /$f[1]; let $f := /d.absent; "
              "##/$g",
              {h.first}},
             {"let $f := /d.items; let $g := /$f[. != \"bad\"]; ##/$g[-1]",
              {h.last}},
             {"def apply($x: Cell, $f): Stream<Cell> := $x/$f[. != "
              "\"bad\"][-1]; apply(##, /d.items)",
              {h.last}},
             {"let $f := /d.items; ##/$f!last", {h.first, h.bad, h.last}},
             {"let $f := /d.items; ##/$f!both", {h.first, h.bad, h.last}},
         }) {
      SCOPED_TRACE(query);
      const auto result = executor.execute(query);
      ASSERT_TRUE(result) << result.error().message;
      EXPECT_EQ(*result, expected);
    }
    const auto emptyConstant =
        executor.execute("def answer(): Int := 42; let $f := /answer(); let $g "
                         ":= /$f[. = 42]; ##/d.absent/$g");
    ASSERT_TRUE(emptyConstant) << emptyConstant.error().message;
    ASSERT_EQ(emptyConstant->size(), 1u);
    EXPECT_EQ(h.arena.asInt64(emptyConstant->front()), 42);
    const auto aggregate = executor.execute("let $f := /d.items; ./$f[3, 4]",
                                            {h.core.home(), h.first});
    ASSERT_TRUE(aggregate) << aggregate.error().message;
    EXPECT_EQ(*aggregate, (std::vector<CellRef>{h.last, h.bad}));
  }
}

TEST(VQLStreamParityTest, QueryDereferenceKeepsOrdinaryCellsAndResolvesChains) {
  for (const auto mode :
       {QueryExecutionMode::Direct, QueryExecutionMode::Vortex}) {
    StreamHarness h;
    const auto master = h.arena.makeCell("master");
    const auto middle = h.arena.makeCell("middle");
    zigzag::expectWritten(
        h.arena.link(master, h.core.dims().clone, DimVector::POS, middle));
    zigzag::expectWritten(
        h.arena.link(middle, h.core.dims().clone, DimVector::POS, h.first));
    QueryExecutor executor(h.engine, h.vm, {.mode = mode});
    const auto result =
        executor.execute("let $f := /d.items; ##/$f>[. = \"master\"][-1]");
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(*result, (std::vector<CellRef>{master}));
    // Anchor bindings are explicit and retain the original cell identity.
    h.engine.setVariable("x", h.first);
    const auto bound = executor.execute("$x>");
    ASSERT_TRUE(bound) << bound.error().message;
    EXPECT_EQ(*bound, (std::vector<CellRef>{master}));
    zigzag::expectWritten(
        h.arena.link(h.first, h.core.dims().clone, DimVector::POS, master));
    const auto ring = executor.execute("let $f := /d.items; ##/$f>");
    ASSERT_TRUE(ring) << ring.error().message;
    EXPECT_EQ(*ring, (std::vector<CellRef>{h.first, h.bad, h.last}));
  }
}

TEST(VQLStreamParityTest, RuntimeRankCycleBudgetStopsBeforeFullTraversal) {
  StreamHarness h;
  auto tail = h.last;
  for (int i = 0; i < 256; ++i) {
    const auto next = h.arena.makeCell("item");
    zigzag::expectWritten(h.arena.link(tail, h.items, DimVector::POS, next));
    tail = next;
  }
  QueryExecutor executor(h.engine, h.vm,
                         {.mode = QueryExecutionMode::Vortex, .maxCycles = 32});
  const auto result = executor.execute("##/d.items");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().category, QueryExecutionError::Category::Budget);
  const auto recovered = executor.execute("##/d.absent");
  ASSERT_TRUE(recovered) << recovered.error().message;
  EXPECT_TRUE(recovered->empty());
}
} // namespace
} // namespace xanadu::vql

/**
 * @file vagent_compiler_test.cpp
 * @brief Tests for the vagent compiler (design/vagent-agent-language.md §5,
 * §9): a tiny harness stands in for the executor adapter, setting parameters
 * and the `$turn` record by hand and running the two routines to their halt op.
 */
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <variant>
#include <vector>

#include "common/xanadu/vagent/runtime/compiler.hpp"
#include "common/xanadu/vagent/runtime/parser.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace {

using namespace xanadu::vagent::runtime;
using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::noCell;
using zigzag::vortex::CellValue;
using zigzag::vortex::VortexCore;
using zigzag::vortex::VortexVM;

constexpr std::size_t kMaxCycles = 2'000'000;

struct Harness {
  ArenaManifold arena;
  VortexCore core{arena};
  Compiler compiler{core};
  VortexVM vm{core};
  Unit unit;
  CompiledAgent ca;
  AgentDims dims;
  std::string lastError;
  bool ok{false};

  bool compile(const std::string &src, const std::string &agent) {
    auto parsed = parseUnit(src);
    if (!parsed) {
      lastError = formatParseError(parsed.error());
      return false;
    }
    unit   = std::move(*parsed);
    auto r = compiler.compileAgent(unit, agent);
    if (!r) {
      lastError = std::string(compileErrorName(r.error().code)) + ": " +
                  r.error().detail;
      return false;
    }
    ca   = *r;
    dims = registerDimensions(arena);
    vm.setDeweyTracking(false);
    return true;
  }

  // -- structure helpers ----------------------------------------------------
  CellRef link(CellRef c, zigzag::DimRef d) const {
    return arena.linked(c, d, DimVector::POS).value_or(noCell);
  }
  void chain(CellRef from, zigzag::DimRef dim, CellRef to) {
    zigzag::expectWritten(arena.link(from, dim, DimVector::POS, to));
  }
  CellRef nameCellOf(CellRef owner, const std::string &name) const {
    for (CellRef n = link(owner, core.dims().vars); n != noCell;
         n         = link(n, core.dims().vars)) {
      if (arena.textOf(n) == name) return n;
    }
    return noCell;
  }
  CellRef fieldOf(CellRef owner, const std::string &name) const {
    const CellRef n = nameCellOf(owner, name);
    return n == noCell ? noCell : link(n, core.dims().values);
  }
  CellRef fieldCell(const std::string &name) const {
    return fieldOf(ca.agentCell, name);
  }
  CellValue field(const std::string &name) const {
    return core.render(fieldCell(name)).value();
  }
  void setField(const std::string &name, const CellValue &v) {
    (void)core.writeValue(fieldCell(name), 0, std::nullopt, v);
  }
  std::int64_t intField(const std::string &name) const {
    return std::get<std::int64_t>(field(name));
  }
  std::string strField(const std::string &name) const {
    return std::get<std::string>(field(name));
  }
  CellRef scalarCell(const CellValue &v) {
    if (std::holds_alternative<std::int64_t>(v))
      return arena.makeScalarCell(std::get<std::int64_t>(v));
    if (std::holds_alternative<double>(v))
      return arena.makeScalarCell(std::get<double>(v));
    if (std::holds_alternative<bool>(v))
      return arena.makeScalarCell(std::get<bool>(v));
    return arena.makeCell(std::get<std::string>(v));
  }
  CellRef addFields(CellRef owner,
                    const std::vector<std::pair<std::string, CellRef>> &fs) {
    CellRef prev = owner;
    for (const auto &[n, v] : fs) {
      const CellRef nc = arena.makeCell(n);
      chain(prev, core.dims().vars, nc);
      chain(nc, core.dims().values, v);
      prev = nc;
    }
    return owner;
  }
  /// An `Opt<Agent>` or `Opt<Space>` holding @p value: a list cell of one.
  CellRef optCell(const std::string &value) {
    const CellRef l = arena.makeCell(std::string(kListContent));
    chain(l, core.dims().grab, arena.makeCell(value));
    return l;
  }
  CellRef
  makeEvent(const std::string &type,
            const std::vector<std::pair<std::string, CellValue>> &fs = {}) {
    const CellRef e = arena.makeCell(type);
    std::vector<std::pair<std::string, CellRef>> cells;
    for (const auto &[n, v] : fs) cells.emplace_back(n, scalarCell(v));
    cells.emplace_back("source", optCell("peer"));
    cells.emplace_back("space", optCell("inner"));
    cells.emplace_back("time", arena.makeScalarCell(std::int64_t{1000}));
    cells.emplace_back("ordinal", arena.makeScalarCell(std::int64_t{1}));
    return addFields(e, cells);
  }
  void setTurn(CellRef event) {
    const CellRef rec = arena.makeCell();
    std::vector<std::pair<std::string, CellRef>> fs;
    fs.emplace_back(std::string(kTurnTime),
                    arena.makeScalarCell(std::int64_t{1000}));
    if (event != noCell) fs.emplace_back(std::string(kTurnEvent), event);
    addFields(rec, fs);
    chain(nameCellOf(ca.agentCell, std::string(kFieldTurn)), core.dims().values,
          rec);
  }

  // -- running --------------------------------------------------------------
  bool run(CellRef entry) {
    const CellRef cursor = vm.spawnCursor(entry);
    vm.pushChoicePoint(cursor, ca.failOp);
    const auto r = vm.run(cursor, kMaxCycles);
    lastError    = r.errorMessage;
    ok           = r.success && vm.getCursorOpcode(cursor) == ca.haltOp;
    vm.clearChoicePoints();
    return ok;
  }
  bool init() { return run(ca.initEntry); }
  bool dispatch(const std::string &type,
                const std::vector<std::pair<std::string, CellValue>> &fs = {}) {
    setTurn(makeEvent(type, fs));
    return run(ca.dispatchEntry);
  }

  // -- outbox ---------------------------------------------------------------
  std::vector<CellRef> outbox() const {
    std::vector<CellRef> out;
    for (CellRef e = link(ca.agentCell, dims.outbox); e != noCell;
         e         = link(e, dims.outbox)) {
      out.push_back(e);
    }
    return out;
  }
  std::vector<CellValue> listValues(CellRef list) const {
    std::vector<CellValue> out;
    for (CellRef e = link(list, core.dims().grab); e != noCell;
         e         = link(e, core.dims().step)) {
      out.push_back(core.render(e).value());
    }
    return out;
  }
};

constexpr const char *kPrelude = R"vagent(
event Tick { x: Int }
event Ping { n: Int }
record Point { x: Int, y: Int }
)vagent";

std::string program(const std::string &agentBody) {
  return std::string(kPrelude) + "agent A {\n" + agentBody + "\n}\n";
}

CellValue I(std::int64_t v) { return v; }
CellValue Str(const char *v) { return std::string(v); }

TEST(VagentCompiler, FieldInitialisersInDeclarationOrderAfterParameters) {
  Harness h;
  ASSERT_TRUE(h.compile(std::string(kPrelude) + R"vagent(
agent A(n: Int) {
  var a: Int := 5
  var s: Str := "hi"
  var b: Int := n + a
  var f: Float := 1.5
}
)vagent",
                        "A"))
      << h.lastError;
  h.setField("n", I(3));
  ASSERT_TRUE(h.init()) << h.lastError;
  EXPECT_EQ(h.intField("a"), 5);
  EXPECT_EQ(h.strField("s"), "hi");
  EXPECT_EQ(h.intField("b"), 8);
  EXPECT_EQ(h.field("f"), CellValue(1.5));
}

TEST(VagentCompiler, AgentCellCarriesTypeNameAndReservedFields) {
  Harness h;
  ASSERT_TRUE(h.compile(program("var a: Int := 1"), "A")) << h.lastError;
  EXPECT_EQ(h.arena.textOf(h.ca.agentCell), "A");
  for (const auto n :
       {kFieldName, kFieldParent, kFieldOuter, kFieldInitialized, kFieldPeers,
        kFieldNextId, kFieldSpawnCount, kFieldTurnCount, kFieldTimers,
        kFieldTurn, kFieldTerminating, kFieldCancelled, kFieldStats,
        kFieldOrdinal, kFieldConsumed}) {
    EXPECT_NE(h.fieldCell(std::string(n)), noCell) << n;
  }
  EXPECT_NE(h.ca.initEntry, noCell);
  EXPECT_NE(h.ca.dispatchEntry, noCell);
  EXPECT_NE(h.ca.haltOp, h.ca.failOp);
}

TEST(VagentCompiler, InitializeHandlersRunAfterInitialisers) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"vagent(
  var total: Int := 1
  on Initialize { total := total + 10 }
  on Initialize { total := total * 2 }
)vagent"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.init()) << h.lastError;
  EXPECT_EQ(h.intField("total"), 22);
}

TEST(VagentCompiler, GuardSelectsHandler) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"vagent(
  var hits: Int := 0
  on Tick as t [t.x > 5] { hits := hits + 1 }
)vagent"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.init());
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(9)}})) << h.lastError;
  EXPECT_EQ(h.intField("hits"), 1);
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(2)}})) << h.lastError;
  EXPECT_EQ(h.intField("hits"), 1);
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(2)}})) << h.lastError;
  EXPECT_EQ(h.intField("hits"), 1);
}

TEST(VagentCompiler, HandlersRunByDescendingPriorityThenDeclarationOrder) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"vagent(
  var log: Str := ""
  on Tick { log := log ++ "a" }
  on Tick priority 5 { log := log ++ "b" }
  on Tick priority 5 { log := log ++ "c" }
  on Tick priority 0 { log := log ++ "d" }
)vagent"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.init());
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(0)}})) << h.lastError;
  EXPECT_EQ(h.strField("log"), "bcad");
}

TEST(VagentCompiler, SupertypeHandlersMergeWithTheEventsOwn) {
  Harness h;
  ASSERT_TRUE(h.compile(R"vagent(
event Base { v: Int }
event Sub extends Base { w: Int }
agent A {
  var log: Str := ""
  on Base { log := log ++ "B" }
  on Sub priority 1 { log := log ++ "S" }
}
)vagent",
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.init());
  ASSERT_TRUE(h.dispatch("Sub", {{"v", I(1)}, {"w", I(2)}})) << h.lastError;
  EXPECT_EQ(h.strField("log"), "SB");
  ASSERT_TRUE(h.dispatch("Base", {{"v", I(1)}})) << h.lastError;
  EXPECT_EQ(h.strField("log"), "SBB");
}

TEST(VagentCompiler, ConsumeStopsLowerPriorityHandlers) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"vagent(
  var log: Str := ""
  on Tick priority 3 {
    log := log ++ "a"
    consume
  }
  on Tick priority 2 { log := log ++ "b" }
)vagent"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.init());
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(0)}})) << h.lastError;
  EXPECT_EQ(h.strField("log"), "a");
  // The dispatcher clears the flag per event.
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(0)}})) << h.lastError;
  EXPECT_EQ(h.strField("log"), "aa");
}

TEST(VagentCompiler, FailingHandlerUndoesItsOwnWritesOnly) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"vagent(
  var a: Int := 0
  var b: Int := 0
  var c: Int := 0
  on Tick priority 5 { a := 1 }
  on Tick priority 3 {
    b := 2
    fail
  }
  on Tick priority 1 { c := 3 }
)vagent"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.init());
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(0)}})) << h.lastError;
  EXPECT_EQ(h.intField("a"), 1);
  EXPECT_EQ(h.intField("b"), 0);
  EXPECT_EQ(h.intField("c"), 3);
}

TEST(VagentCompiler, ConsumeInAFailedHandlerIsUndone) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"vagent(
  var log: Str := ""
  on Tick priority 3 {
    consume
    fail
  }
  on Tick priority 2 { log := log ++ "b" }
)vagent"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.init());
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(0)}})) << h.lastError;
  EXPECT_EQ(h.strField("log"), "b");
}

TEST(VagentCompiler, LoopsAndConditionals) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"vagent(
  var sum: Int := 0
  var evens: Int := 0
  var w: Int := 0
  var neg: Int := 0
  on Tick {
    for i in 1 to 10 do sum := sum + i
    for j in 1 to 10 do if j % 2 == 0 then evens := evens + 1
    while w < 7 do w := w + 2
    for k in 5 to 1 by -2 do neg := neg + k
  }
)vagent"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.init());
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(0)}})) << h.lastError;
  EXPECT_EQ(h.intField("sum"), 55);
  EXPECT_EQ(h.intField("evens"), 5);
  EXPECT_EQ(h.intField("w"), 8);
  EXPECT_EQ(h.intField("neg"), 9);
}

TEST(VagentCompiler, ConjunctionAlternationAndGenerators) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"vagent(
  var x: Int := 0
  var alt: Int := 0
  var negd: Int := 0
  var unreached: Int := 0
  on Tick {
    x := 3 to 9 & x * x > 30
    alt := 1 | 2
    if 1 > 2 | 3 > 2 then alt := alt + 10
    if not 1 > 2 then negd := 1
    if not 2 > 1 then unreached := 1
  }
)vagent"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.init());
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(0)}})) << h.lastError;
  EXPECT_EQ(h.intField("x"), 6);
  EXPECT_EQ(h.intField("alt"), 11);
  EXPECT_EQ(h.intField("negd"), 1);
  EXPECT_EQ(h.intField("unreached"), 0);
}

TEST(VagentCompiler, MethodsReturnValuesAndRecurse) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"vagent(
  var f5: Int := 0
  var fib10: Int := 0
  var twice: Int := 0
  var early: Int := 0
  def fact(n: Int): Int {
    if n <= 1 then 1 else n * fact(n - 1)
  }
  def fib(n: Int): Int {
    if n < 2 then n else fib(n - 1) + fib(n - 2)
  }
  def dbl(n: Int): Int { n + n }
  def pick(n: Int): Int {
    if n > 3 then return 100
    n
  }
  on Tick {
    f5 := fact(5)
    fib10 := fib(10)
    twice := dbl(dbl(3))
    early := pick(9) + pick(2)
  }
)vagent"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.init());
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(0)}})) << h.lastError;
  EXPECT_EQ(h.intField("f5"), 120);
  EXPECT_EQ(h.intField("fib10"), 55);
  EXPECT_EQ(h.intField("twice"), 12);
  EXPECT_EQ(h.intField("early"), 102);
}

TEST(VagentCompiler, FailingMethodFailsTheCaller) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"vagent(
  var a: Int := 0
  var b: Int := 0
  def boom(n: Int): Int {
    fail
  }
  on Tick priority 2 {
    a := 1
    a := boom(1)
  }
  on Tick priority 1 { b := 1 }
)vagent"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.init());
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(0)}})) << h.lastError;
  EXPECT_EQ(h.intField("a"), 0);
  EXPECT_EQ(h.intField("b"), 1);
}

TEST(VagentCompiler, RecordsAreCellsWithFieldsAndCopyOnStore) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"vagent(
  var p: Point := Point{x: 1, y: 2}
  var q: Point := Point{x: 0, y: 0}
  var sum: Int := 0
  on Tick {
    p.x := p.x + 5
    q := p
    p.y := 9
    sum := q.x + q.y
  }
)vagent"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.init());
  const CellRef p0 = h.fieldCell("p");
  EXPECT_EQ(h.arena.textOf(p0), "Point");
  EXPECT_EQ(h.core.render(h.fieldOf(p0, "x")), I(1));
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(0)}})) << h.lastError;
  const CellRef p = h.fieldCell("p");
  EXPECT_EQ(h.core.render(h.fieldOf(p, "x")), I(6));
  EXPECT_EQ(h.core.render(h.fieldOf(p, "y")), I(9));
  const CellRef q = h.fieldCell("q");
  EXPECT_NE(q, p);
  EXPECT_EQ(h.core.render(h.fieldOf(q, "y")), I(2));
  EXPECT_EQ(h.intField("sum"), 8);
}

TEST(VagentCompiler, ListsIterateIndexConcatenateAndCopy) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"vagent(
  var xs: [Int] := [1, 2, 3]
  var ys: [Int] := []
  var total: Int := 0
  var second: Int := 0
  var n: Int := 0
  on Tick {
    for v in xs do total := total + v
    second := xs[1]
    ys := xs ++ [4, 5]
    xs[0] := 10
    n := *ys
  }
)vagent"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.init());
  EXPECT_EQ(h.listValues(h.fieldCell("xs")),
            (std::vector<CellValue>{I(1), I(2), I(3)}));
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(0)}})) << h.lastError;
  EXPECT_EQ(h.intField("total"), 6);
  EXPECT_EQ(h.intField("second"), 2);
  EXPECT_EQ(h.intField("n"), 5);
  EXPECT_EQ(h.listValues(h.fieldCell("ys")),
            (std::vector<CellValue>{I(1), I(2), I(3), I(4), I(5)}));
  EXPECT_EQ(h.listValues(h.fieldCell("xs")).front(), I(10));
}

TEST(VagentCompiler, EmitStagesAnEventEntryAtTheOutboxTail) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"vagent(
  var src: Str := ""
  on Tick as t {
    src := t.source
    emit Ping{n: t.x + 1}
    emit Ping{n: 7} in inner for "bob"
    emit Ping{n: 8} in outer for ["bob", "eve"]
  }
)vagent"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.init());
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(4)}})) << h.lastError;
  EXPECT_EQ(h.strField("src"), "peer");
  EXPECT_EQ(h.intField(std::string(kFieldOrdinal)), 3);
  const auto box = h.outbox();
  ASSERT_EQ(box.size(), 3U);
  for (const CellRef e : box) EXPECT_EQ(h.arena.textOf(e), "event");
  const CellRef ev0 = h.fieldOf(box[0], "event");
  EXPECT_EQ(h.arena.textOf(ev0), "Ping");
  EXPECT_EQ(h.core.render(h.fieldOf(ev0, "n")), I(5));
  EXPECT_EQ(h.core.render(h.fieldOf(box[0], "space")), Str("outer"));
  EXPECT_EQ(h.fieldOf(box[0], "for"), noCell);
  EXPECT_EQ(h.core.render(h.fieldOf(h.fieldOf(box[1], "event"), "n")), I(7));
  EXPECT_EQ(h.core.render(h.fieldOf(box[1], "space")), Str("inner"));
  EXPECT_EQ(h.listValues(h.fieldOf(box[1], "for")),
            (std::vector<CellValue>{Str("bob")}));
  EXPECT_EQ(h.listValues(h.fieldOf(box[2], "for")),
            (std::vector<CellValue>{Str("bob"), Str("eve")}));
}

TEST(VagentCompiler, EmitInAFailedHandlerIsUndone) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"vagent(
  on Tick priority 2 {
    emit Ping{n: 1}
    fail
  }
  on Tick priority 1 { emit Ping{n: 2} }
)vagent"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.init());
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(0)}})) << h.lastError;
  const auto box = h.outbox();
  ASSERT_EQ(box.size(), 1U);
  EXPECT_EQ(h.core.render(h.fieldOf(h.fieldOf(box[0], "event"), "n")), I(2));
}

TEST(VagentCompiler, SpawnStagesNamedEntriesWithAnArgsRecord) {
  Harness h;
  ASSERT_TRUE(h.compile(R"vagent(
agent Worker(n: Int, label: Str) { }
agent Boss {
  var kid: Str := ""
  on Initialize {
    kid := spawn Worker(7, "w") in inner
    spawn Worker(8, "x")
  }
}
)vagent",
                        "Boss"))
      << h.lastError;
  h.setField(std::string(kFieldName), Str("root"));
  ASSERT_TRUE(h.init()) << h.lastError;
  EXPECT_EQ(h.strField("kid"), "root/Worker#0");
  EXPECT_EQ(h.intField(std::string(kFieldSpawnCount)), 2);
  const auto box = h.outbox();
  ASSERT_EQ(box.size(), 2U);
  EXPECT_EQ(h.arena.textOf(box[0]), "spawn");
  EXPECT_EQ(h.core.render(h.fieldOf(box[0], "agent")), Str("Worker"));
  EXPECT_EQ(h.core.render(h.fieldOf(box[0], "name")), Str("root/Worker#0"));
  EXPECT_EQ(h.core.render(h.fieldOf(box[1], "name")), Str("root/Worker#1"));
  const CellRef args = h.fieldOf(box[0], "args");
  EXPECT_EQ(h.core.render(h.fieldOf(args, "n")), I(7));
  EXPECT_EQ(h.core.render(h.fieldOf(args, "label")), Str("w"));
}

TEST(VagentCompiler, ProgramSideStructureHoldsHandlersUnderTheirEventType) {
  Harness h;
  ASSERT_TRUE(h.compile(R"vagent(
event Base { v: Int }
event Sub extends Base { w: Int }
agent A {
  var n: Int := 0
  on Sub [occurrence.w > 1] priority 4 { n := 1 }
  on Sub { n := 2 }
}
)vagent",
                        "A"))
      << h.lastError;
  const auto module = h.core.findModule("vagent:A");
  ASSERT_TRUE(module.has_value());
  CellRef sub = noCell;
  for (CellRef s = h.link(*module, h.core.dims().vars); s != noCell;
       s         = h.link(s, h.core.dims().vars)) {
    if (h.arena.textOf(s) == "Sub") sub = s;
  }
  ASSERT_NE(sub, noCell);
  const CellRef first = h.link(sub, h.core.dims().clause);
  ASSERT_NE(first, noCell);
  EXPECT_NE(h.link(first, h.core.dims().grab), noCell);
  EXPECT_NE(h.link(first, h.core.dims().spin), noCell);
  EXPECT_EQ(h.core.render(h.fieldOf(first, "priority")), I(4));
  const CellRef second = h.link(first, h.core.dims().clause);
  ASSERT_NE(second, noCell);
  EXPECT_EQ(h.link(second, h.core.dims().grab), noCell);
  const CellRef sup = h.link(sub, h.dims.extends);
  ASSERT_NE(sup, noCell);
  EXPECT_EQ(h.arena.textOf(sup), "Base");
}

TEST(VagentCompiler, LocalsSurviveRecursiveCallsAndHandlersMayReturn) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"vagent(
  var tri: Int := 0
  var after: Int := 0
  def sumto(n: Int): Int {
    let here := n
    let rest := if n <= 0 then 0 else sumto(n - 1)
    here + rest
  }
  on Tick {
    tri := sumto(6)
    return
    after := 1
  }
)vagent"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.init());
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(0)}})) << h.lastError;
  EXPECT_EQ(h.intField("tri"), 21);
  EXPECT_EQ(h.intField("after"), 0);
}

TEST(VagentCompiler, RefusesWhatIterationTwoDoesNotCompile) {
  const auto refusal = [](const std::string &body) {
    Harness h;
    EXPECT_FALSE(h.compile(program(body), "A")) << body;
    return h.lastError;
  };
  EXPECT_NE(refusal("on Tick { x := 1 }").find("UnknownName"),
            std::string::npos);
  EXPECT_NE(refusal("on Tick { yield }").find("Unsupported"),
            std::string::npos);
  EXPECT_NE(refusal("on Bogus { }").find("UnknownType"), std::string::npos);
  EXPECT_NE(refusal("var a: Int := 1\nvar a: Int := 2").find("DuplicateName"),
            std::string::npos);
  Harness h;
  EXPECT_FALSE(h.compile(program(""), "Nope"));
  EXPECT_NE(h.lastError.find("UnknownAgent"), std::string::npos);
}

} // namespace

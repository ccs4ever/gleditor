/**
 * @file vagent_frames_test.cpp
 * @brief Activation frames for compiled vagent code (design §5.8, P16): locals
 * and arguments live on a frame cell and are reached through handle
 * registers; the cycle benchmark at the end records what that costs.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <iostream>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/vagent/compiler.hpp"
#include "common/xanadu/vagent/parser.hpp"
#include "common/xanadu/vagent/runtime_abi.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace {

using namespace xanadu::vagent;
using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::noCell;
using zigzag::vortex::CellValue;
using zigzag::vortex::VortexCore;
using zigzag::vortex::VortexVM;

constexpr std::size_t kMaxCycles = 50'000'000;

struct Rig {
  ArenaManifold arena;
  VortexCore core{arena};
  Compiler compiler{core};
  VortexVM vm{core};
  Unit unit;
  CompiledAgent ca;
  std::string lastError;

  bool compile(const std::string &src, const std::string &agent = "A") {
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
    ca = *r;
    (void)registerDimensions(arena);
    vm.setDeweyTracking(false);
    return true;
  }
  CellRef link(CellRef c, zigzag::DimRef d) const {
    return arena.linked(c, d, DimVector::POS);
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
  CellRef fieldCell(const std::string &name) const {
    const CellRef n = nameCellOf(ca.agentCell, name);
    return n == noCell ? noCell : link(n, core.dims().values);
  }
  std::int64_t intField(const std::string &name) const {
    return std::get<std::int64_t>(core.render(fieldCell(name)));
  }
  /// Installs an event of @p type carrying one Int field as the turn's event.
  void setTurn(const std::string &type, const std::string &field,
               std::int64_t v) {
    CellRef prev    = noCell;
    const CellRef e = arena.makeCell(type);
    const auto add  = [&](const std::string &n, CellRef value) {
      const CellRef nc = arena.makeCell(n);
      chain(prev == noCell ? e : prev, core.dims().vars, nc);
      chain(nc, core.dims().values, value);
      prev = nc;
    };
    const auto opt = [&](const std::string &text) {
      const CellRef l = arena.makeCell(std::string(kListContent));
      chain(l, core.dims().grab, arena.makeCell(text));
      return l;
    };
    add(field, arena.makeScalarCell(v));
    add("source", opt("peer"));
    add("space", opt("inner"));
    add("time", arena.makeScalarCell(std::int64_t{1000}));
    add("ordinal", arena.makeScalarCell(std::int64_t{1}));
    const CellRef rec = arena.makeCell();
    const CellRef tn  = arena.makeCell(std::string(kTurnTime));
    chain(rec, core.dims().vars, tn);
    chain(tn, core.dims().values, arena.makeScalarCell(std::int64_t{1000}));
    const CellRef en = arena.makeCell(std::string(kTurnEvent));
    chain(tn, core.dims().vars, en);
    chain(en, core.dims().values, e);
    chain(nameCellOf(ca.agentCell, std::string(kFieldTurn)), core.dims().values,
          rec);
  }
  bool run(CellRef entry) {
    const CellRef cursor = vm.spawnCursor(entry);
    vm.pushChoicePoint(cursor, ca.failOp);
    const auto r  = vm.run(cursor, kMaxCycles);
    lastError     = r.errorMessage;
    const bool ok = r.success && vm.getCursorOpcode(cursor) == ca.haltOp;
    vm.clearChoicePoints();
    return ok;
  }
  bool init() { return run(ca.initEntry); }
  bool dispatch(const std::string &type, const std::string &field,
                std::int64_t v) {
    setTurn(type, field, v);
    return run(ca.dispatchEntry);
  }
  /// Cycles one dispatch takes.
  std::uint64_t cycles(const std::string &type, const std::string &field,
                       std::int64_t v) {
    const auto before = vm.totalCycles();
    EXPECT_TRUE(dispatch(type, field, v)) << lastError;
    return vm.totalCycles() - before;
  }
};

constexpr const char *kPrelude = R"vagent(
event Tick { x: Int }
event Ping { n: Int }
record Point { x: Int, y: Int }
)vagent";

std::string program(const std::string &body) {
  return std::string(kPrelude) + "agent A {\n" + body + "\n}\n";
}

// -- Frames -------------------------------------------------------------------

std::int64_t sumTo(std::int64_t n) { return n * (n + 1) / 2; }

TEST(VagentFrames, DeepRecursionKeepsEveryActivationsLocals) {
  Rig h;
  ASSERT_TRUE(h.compile(program(R"vagent(
  var tri: Int := 0
  def sumto(n: Int): Int {
    let here := n
    let rest := if n <= 0 then 0 else sumto(n - 1)
    here + rest
  }
  on Tick as t { tri := sumto(t.x) }
)vagent")))
      << h.lastError;
  ASSERT_TRUE(h.init());
  ASSERT_TRUE(h.dispatch("Tick", "x", 500)) << h.lastError;
  EXPECT_EQ(h.intField("tri"), sumTo(500));
}

TEST(VagentFrames, RecordLocalsSurviveRecursion) {
  Rig h;
  ASSERT_TRUE(h.compile(program(R"vagent(
  var total: Int := 0
  def depth(n: Int): Int {
    let p := Point{x: n, y: 0}
    let r := if n <= 0 then 0 else depth(n - 1)
    p.x := p.x + 1
    p.x + r
  }
  on Tick as t { total := depth(t.x) }
)vagent")))
      << h.lastError;
  ASSERT_TRUE(h.init());
  ASSERT_TRUE(h.dispatch("Tick", "x", 60)) << h.lastError;
  EXPECT_EQ(h.intField("total"), sumTo(60) + 61);
}

TEST(VagentFrames, MutualRecursionKeepsEachMethodsLocals) {
  Rig h;
  ASSERT_TRUE(h.compile(program(R"vagent(
  var out: Int := 0
  def evens(n: Int): Int {
    let keep := n
    let r := if n == 0 then 0 else odds(n - 1)
    r + keep
  }
  def odds(n: Int): Int {
    let keep := n * 10
    let r := if n == 0 then 0 else evens(n - 1)
    r + keep
  }
  on Tick as t { out := evens(t.x) }
)vagent")))
      << h.lastError;
  ASSERT_TRUE(h.init());
  std::int64_t want = 0;
  bool even         = true;
  for (std::int64_t n = 30; n >= 0; --n, even = !even) {
    want += even ? n : n * 10;
  }
  ASSERT_TRUE(h.dispatch("Tick", "x", 30)) << h.lastError;
  EXPECT_EQ(h.intField("out"), want);
}

TEST(VagentFrames, AMethodCalledFromTwoHandlersGetsAFrameEachTime) {
  Rig h;
  ASSERT_TRUE(h.compile(program(R"vagent(
  var a: Int := 0
  var b: Int := 0
  def twice(n: Int): Int {
    let m := n * 2
    m + n
  }
  on Tick as t {
    let keep := t.x
    a := twice(keep) + keep
  }
  on Ping as p {
    let k := 1
    b := twice(p.n) + twice(k)
  }
)vagent")))
      << h.lastError;
  ASSERT_TRUE(h.init());
  ASSERT_TRUE(h.dispatch("Tick", "x", 4)) << h.lastError;
  ASSERT_TRUE(h.dispatch("Ping", "n", 5)) << h.lastError;
  EXPECT_EQ(h.intField("a"), 16);
  EXPECT_EQ(h.intField("b"), 18);
}

TEST(VagentFrames, LocalsDoNotLeakBetweenCallsOrIntoTheCaller) {
  Rig h;
  ASSERT_TRUE(h.compile(program(R"vagent(
  var o: Int := 0
  var m: Int := 0
  def helper(x: Int): Int {
    let a := x * 2
    let p := Point{x: a, y: 1}
    let s := p.x + p.y
    s
  }
  def wrap(x: Int): Int {
    let a := x
    let p := Point{x: x, y: 5}
    let b := helper(a + 1)
    let c := helper(b)
    let px := p.x * 1000000
    a * 10000 + b * 100 + c + px + p.y
  }
  on Tick as t {
    o := wrap(t.x)
    m := wrap(1)
  }
)vagent")))
      << h.lastError;
  ASSERT_TRUE(h.init());
  ASSERT_TRUE(h.dispatch("Tick", "x", 3)) << h.lastError;
  // helper(4)=9, helper(9)=19.
  EXPECT_EQ(h.intField("o"), 3 * 10000 + 9 * 100 + 19 + 3 * 1000000 + 5);
  // helper(2)=5, helper(5)=11.
  EXPECT_EQ(h.intField("m"), 1 * 10000 + 5 * 100 + 11 + 1 * 1000000 + 5);
}

TEST(VagentFrames, AFailedHandlersFramesAreUnwoundWithIt) {
  Rig h;
  ASSERT_TRUE(h.compile(program(R"vagent(
  var seen: Int := 0
  def boom(n: Int): Int {
    let a := n
    if n == 0 then fail else boom(n - 1) + a
  }
  def deep(n: Int): Int {
    let a := n
    let r := if n <= 0 then 0 else deep(n - 1)
    r + a
  }
  on Tick priority 5 {
    let keep := 7
    seen := 1
    boom(4)
  }
  on Tick priority 1 {
    let k := 3
    seen := seen + deep(4) + k
  }
)vagent")))
      << h.lastError;
  ASSERT_TRUE(h.init());
  ASSERT_TRUE(h.dispatch("Tick", "x", 0)) << h.lastError;
  // The first handler's write was undone with its frames; the second ran on
  // handles that name its own frame.
  EXPECT_EQ(h.intField("seen"), 10 + 3);
  ASSERT_TRUE(h.dispatch("Tick", "x", 0)) << h.lastError;
  EXPECT_EQ(h.intField("seen"), 2 * (10 + 3));
}

// -- Running a body against a frame a host names
// ---------------------------------

TEST(VagentFrames, ABodyRunsTwiceOnTheSameFrameWithItsLocalsKept) {
  Rig h;
  ASSERT_TRUE(h.compile(program(R"vagent(
  def bump(n: Int, p: Point): Int {
    n := n + 1
    p.x := p.x + n
    p.x * 100 + n
  }
)vagent")))
      << h.lastError;
  const auto fb = h.compiler.frameBody("bump");
  ASSERT_TRUE(fb.has_value());
  ASSERT_EQ(fb->args.size(), 2U);

  const auto point = [&](std::int64_t x) {
    const CellRef rec = h.arena.makeCell("Point");
    const CellRef nx  = h.arena.makeCell("x");
    h.chain(rec, h.core.dims().vars, nx);
    h.chain(nx, h.core.dims().values, h.arena.makeScalarCell(x));
    const CellRef ny = h.arena.makeCell("y");
    h.chain(nx, h.core.dims().vars, ny);
    h.chain(ny, h.core.dims().values, h.arena.makeScalarCell(std::int64_t{0}));
    return rec;
  };
  const auto makeFrame = [&](std::int64_t n, std::int64_t x) {
    (void)h.core.value(fb->args[0], 0, -1, n);
    (void)h.core.value(fb->args[1], 0, -1, static_cast<std::int64_t>(point(x)));
    EXPECT_TRUE(h.run(fb->make)) << h.lastError;
    const auto frame = h.arena.handleTarget(fb->frame);
    EXPECT_TRUE(frame.has_value());
    return frame.value_or(noCell);
  };
  const auto enter = [&](CellRef frame) {
    zigzag::expectWritten(
        h.arena.setValueBits(fb->frame, xanadu::ValueKind::OpHandle, frame));
    EXPECT_TRUE(h.run(fb->enter)) << h.lastError;
    return std::get<std::int64_t>(h.core.render(fb->result));
  };
  const auto local = [&](CellRef frame, const std::string &name) {
    const CellRef n = h.nameCellOf(frame, name);
    return n == noCell ? noCell : h.link(n, h.core.dims().values);
  };

  const CellRef a = makeFrame(10, 5);
  const CellRef b = makeFrame(100, 0);
  ASSERT_NE(a, noCell);
  ASSERT_NE(a, b);
  // The frame holds the arguments as named locals.
  EXPECT_EQ(h.core.render(local(a, "n")), CellValue(std::int64_t{10}));
  EXPECT_EQ(h.arena.textOf(local(a, "p")), "Point");

  EXPECT_EQ(enter(a), (5 + 11) * 100 + 11);
  EXPECT_EQ(h.core.render(local(a, "n")), CellValue(std::int64_t{11}));
  // A second frame of the same body runs in between and disturbs nothing.
  EXPECT_EQ(enter(b), 101 * 100 + 101);
  // Back on the first: n and p.x carry on from the first run.
  EXPECT_EQ(enter(a), (16 + 12) * 100 + 12);
  EXPECT_EQ(h.core.render(local(a, "n")), CellValue(std::int64_t{12}));
  EXPECT_EQ(h.core.render(local(b, "n")), CellValue(std::int64_t{101}));
}

// A frame is the checkpoint-safe place for a plan body's locals (iteration 4c
// resumes bodies from an intention's frame after a `resume`): every local,
// the record one holds included, is a link, which promotion remaps. The same
// values kept as integer refs in registers would name other cells afterwards.
TEST(VagentFrames, AFramesLocalsSurvivePromotionIntoAResumedArena) {
  Rig h;
  ASSERT_TRUE(h.compile(program(R"vagent(
  def bump(n: Int, p: Point): Int {
    n := n + 1
    p.x := p.x + n
    p.x * 100 + n
  }
)vagent")))
      << h.lastError;
  const auto fb = h.compiler.frameBody("bump");
  ASSERT_TRUE(fb.has_value());
  const CellRef rec = h.arena.makeCell("Point");
  const CellRef nx  = h.arena.makeCell("x");
  h.chain(rec, h.core.dims().vars, nx);
  h.chain(nx, h.core.dims().values, h.arena.makeScalarCell(std::int64_t{5}));
  (void)h.core.value(fb->args[0], 0, -1, std::int64_t{10});
  (void)h.core.value(fb->args[1], 0, -1, static_cast<std::int64_t>(rec));
  ASSERT_TRUE(h.run(fb->make)) << h.lastError;
  const CellRef frame = h.arena.handleTarget(fb->frame).value_or(noCell);
  ASSERT_NE(frame, noCell);
  ASSERT_TRUE(h.run(fb->enter)) << h.lastError;

  xanadu::Store store;
  auto at             = store.sliceGenesis(xanadu::MicroversionId{});
  const auto promoted = zigzag::promote(store, at, h.arena, frame);
  ASSERT_TRUE(promoted.has_value());
  const auto fold = store.rebuildManifold(promoted->version);
  const ArenaManifold resumed(&fold, store);
  const CellRef root = promoted->cells.front();

  // The resumed base does not register dimension names, so find them by text.
  std::map<std::string, zigzag::DimRef> dim;
  std::vector<CellRef> frontier{root};
  for (std::size_t i = 0; i < frontier.size(); ++i) {
    for (const auto &edge : resumed.dimensionsOf(frontier[i])) {
      dim.try_emplace(resumed.textOf(edge.dim), edge.dim);
      for (const CellRef next : {edge.pos, edge.neg}) {
        if (next != noCell &&
            std::ranges::find(frontier, next) == frontier.end())
          frontier.push_back(next);
      }
    }
  }
  ASSERT_TRUE(dim.contains("d.vars"));
  ASSERT_TRUE(dim.contains("d.values"));
  const auto valueOf = [&](CellRef owner, const std::string &name) -> CellRef {
    for (CellRef n = resumed.linked(owner, dim.at("d.vars"), DimVector::POS);
         n != noCell; n = resumed.linked(n, dim.at("d.vars"), DimVector::POS)) {
      if (resumed.textOf(n) == name) {
        return resumed.linked(n, dim.at("d.values"), DimVector::POS);
      }
    }
    return noCell;
  };
  const CellRef n = valueOf(root, "n");
  ASSERT_NE(n, noCell);
  EXPECT_EQ(resumed.asInt64(n), 11);
  const CellRef p = valueOf(root, "p");
  ASSERT_NE(p, noCell);
  EXPECT_EQ(resumed.textOf(p), "Point");
  const CellRef x = valueOf(p, "x");
  ASSERT_NE(x, noCell);
  EXPECT_EQ(resumed.asInt64(x), 16);
}

// -- Benchmark ----------------------------------------------------------------
//
// Marginal cycles, so the dispatcher's and the statement wrappers' fixed
// costs cancel: the same handler is run for two loop counts, and variants of
// the loop body differ by exactly one construct.

std::string loopProgram(const std::string &extra) {
  return program(R"vagent(
  var r: Int := 0
  def spin(n: Int): Int {
    let a := 0
    let b := 1
    let i := 0
    while i < n do {
      i := i + 1
)vagent" + extra +
                 R"vagent(
    }
    a
  }
  def fib(n: Int): Int { if n < 2 then n else fib(n - 1) + fib(n - 2) }
  on Tick as t { r := spin(t.x) }
  on Ping as p { r := fib(p.n) }
)vagent");
}

double perIteration(const std::string &extra) {
  Rig h;
  EXPECT_TRUE(h.compile(loopProgram(extra))) << h.lastError;
  EXPECT_TRUE(h.init());
  const auto c1 = h.cycles("Tick", "x", 100);
  const auto c2 = h.cycles("Tick", "x", 300);
  return static_cast<double>(c2 - c1) / 200.0;
}

TEST(VagentFramesBenchmark, CyclesPerLocalAccessAndPerRecursiveCall) {
  const double base  = perIteration("");
  const double read  = perIteration("      a := b\n");
  const double read2 = perIteration("      a := b\n      a := b\n");
  const double sum   = perIteration("      a := a + b\n");
  Rig h;
  ASSERT_TRUE(h.compile(loopProgram(""))) << h.lastError;
  ASSERT_TRUE(h.init());
  const auto f10 = h.cycles("Ping", "n", 10);
  const auto f15 = h.cycles("Ping", "n", 15);
  EXPECT_EQ(h.intField("r"), 610);
  // fib(n) makes 2*fib(n+1)-1 calls: 177 for n=10, 1973 for n=15.
  const double perCall = static_cast<double>(f15 - f10) / (1973.0 - 177.0);
  std::cout << "[bench] loop iteration, no extra statement: " << base
            << " cycles\n"
            << "[bench] + one `a := b` (1 local read, 1 local write): "
            << (read - base) << " cycles\n"
            << "[bench] + two `a := b`: " << (read2 - base) << " cycles\n"
            << "[bench] + one `a := a + b` (2 reads, 1 write): " << (sum - base)
            << " cycles\n"
            << "[bench] fib(10)=" << f10 << " fib(15)=" << f15
            << " cycles; marginal per recursive call: " << perCall << "\n";
}

} // namespace

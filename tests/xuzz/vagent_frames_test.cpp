/**
 * @file vagent_frames_test.cpp
 * @brief Activation frames for compiled vagent code (design §5.8, P16): locals
 * and arguments live on a frame cell and are reached through handle
 * registers; the cycle benchmark at the end records what that costs.
 */
#include <gtest/gtest.h>

#include <iostream>
#include <string>
#include <variant>
#include <vector>

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

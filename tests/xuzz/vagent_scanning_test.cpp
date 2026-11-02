/**
 * @file vagent_scanning_test.cpp
 * @brief Unit tests for 1-based string scanning built-ins in std:goal and
 *        vagent front-end compiler lowering (P18.4, P18.5).
 */
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/xanadu/vagent/runtime/compiler.hpp"
#include "common/xanadu/vagent/runtime/parser.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vagent::runtime {
namespace {

using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::noCell;
using zigzag::vortex::CellValue;
using zigzag::vortex::VortexCore;
using zigzag::vortex::VortexStdLib;
using zigzag::vortex::VortexVM;

constexpr std::size_t kMaxCycles = 4'000'000;

struct ScanHarness {
  ArenaManifold arena;
  VortexCore core{arena};
  Compiler compiler{core};
  VortexVM vm{core};
  Unit unit;
  CompiledAgent ca;
  AgentDims dims;
  std::string lastError;

  bool compile(const std::string &src, const std::string &agent = "Scanner") {
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
    setField("$name", CellValue{std::string("me")});
    return true;
  }

  CellRef link(CellRef c, zigzag::DimRef d) const {
    return arena.linked(c, d, DimVector::POS).value_or(noCell);
  }
  void chain(CellRef from, zigzag::DimRef dim, CellRef to) {
    zigzag::expectWritten(arena.link(from, dim, DimVector::POS, to));
  }
  CellRef nameCell(const std::string &name) const {
    for (CellRef n = link(ca.agentCell, core.dims().vars); n != noCell;
         n         = link(n, core.dims().vars)) {
      if (arena.textOf(n) == name) return n;
    }
    return noCell;
  }
  CellRef fieldCell(const std::string &name) const {
    for (CellRef n = link(ca.agentCell, core.dims().vars); n != noCell;
         n         = link(n, core.dims().vars)) {
      if (arena.textOf(n) == name) return link(n, core.dims().values);
    }
    return noCell;
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
  bool boolField(const std::string &name) const {
    return std::get<bool>(field(name));
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

  CellRef optCell(const std::string &value) {
    const CellRef l = arena.makeCell(std::string(kListContent));
    chain(l, core.dims().grab, arena.makeCell(value));
    return l;
  }

  CellRef makeEventCells(
      const std::string &type,
      const std::vector<std::pair<std::string, CellRef>> &declared = {},
      const std::string &space                                     = "inner") {
    const CellRef e                                    = arena.makeCell(type);
    std::vector<std::pair<std::string, CellRef>> cells = declared;
    cells.emplace_back("source", optCell("peer"));
    cells.emplace_back("space", optCell(space));
    cells.emplace_back("time", arena.makeScalarCell(std::int64_t{1000}));
    cells.emplace_back("ordinal", arena.makeScalarCell(std::int64_t{1}));
    CellRef prev = e;
    for (const auto &[n, v] : cells) {
      const CellRef nc = arena.makeCell(n);
      chain(prev, core.dims().vars, nc);
      chain(nc, core.dims().values, v);
      prev = nc;
    }
    return e;
  }

  void setTurn(CellRef event) {
    const CellRef rec = arena.makeCell();
    const CellRef tn  = arena.makeCell(std::string(kTurnTime));
    chain(rec, core.dims().vars, tn);
    chain(tn, core.dims().values, arena.makeScalarCell(std::int64_t{1000}));
    const CellRef en = arena.makeCell(std::string(kTurnEvent));
    chain(tn, core.dims().vars, en);
    chain(en, core.dims().values, event);
    chain(nameCell(std::string(kFieldTurn)), core.dims().values, rec);
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
  bool dispatch(const std::string &type,
                const std::vector<std::pair<std::string, CellValue>> &fs = {}) {
    std::vector<std::pair<std::string, CellRef>> cells;
    for (const auto &[n, v] : fs) cells.emplace_back(n, scalarCell(v));
    setTurn(makeEventCells(type, cells));
    return run(ca.dispatchEntry);
  }
};

} // namespace

TEST(VagentStringScanning, PosBuiltinStartsAtOneAndSupportsNegativeIndexing) {
  constexpr std::string_view kSrc = R"(
event Run

agent Scanner {
  var posInitial: Int := 0
  var posEnd: Int := 0

  on Run {
    posInitial := "hello" ? pos()
    posEnd := "hello" ? (tab(0) & pos())
  }
}
)";
  ScanHarness h;
  ASSERT_TRUE(h.compile(std::string(kSrc))) << h.lastError;
  ASSERT_TRUE(h.init()) << h.lastError;
  ASSERT_TRUE(h.dispatch("Run")) << "dispatch error: " << h.lastError;

  EXPECT_EQ(h.intField("posInitial"), 1);
  EXPECT_EQ(h.intField("posEnd"), 6); // len 5 + 1 = 6
}

TEST(VagentStringScanning, TabAndMoveAdvanceAndProduceTraversedSubstring) {
  constexpr std::string_view kSrc = R"(
event Run

agent Scanner {
  var sub1: Str := ""
  var sub2: Str := ""
  var sub3: Str := ""

  on Run {
    sub1 := "hello world" ? tab(6)
    sub2 := "hello world" ? (tab(6) & move(1))
    sub3 := "hello world" ? (tab(6) & move(1) & tab(0))
  }
}
)";
  ScanHarness h;
  ASSERT_TRUE(h.compile(std::string(kSrc))) << h.lastError;
  ASSERT_TRUE(h.init()) << h.lastError;
  ASSERT_TRUE(h.dispatch("Run")) << h.lastError;

  EXPECT_EQ(h.strField("sub1"), "hello");
  EXPECT_EQ(h.strField("sub2"), " ");
  EXPECT_EQ(h.strField("sub3"), "world");
}

TEST(VagentStringScanning, MatchTestsPrefixWithoutAdvancingPos) {
  constexpr std::string_view kSrc = R"(
event Run

agent Scanner {
  var matchPos: Int := 0
  var prefixMatched: Str := ""

  on Run {
    matchPos := "SPEC:quartz;count=42" ? match("SPEC:")
    prefixMatched := "SPEC:quartz;count=42" ? tab(match("SPEC:"))
  }
}
)";
  ScanHarness h;
  ASSERT_TRUE(h.compile(std::string(kSrc))) << h.lastError;
  ASSERT_TRUE(h.init()) << h.lastError;
  ASSERT_TRUE(h.dispatch("Run")) << h.lastError;

  // Length of "SPEC:" is 5; starting at 1, match produces 1 + 5 = 6
  EXPECT_EQ(h.intField("matchPos"), 6);
  // tab(match(...)) consumes the prefix and produces "SPEC:"
  EXPECT_EQ(h.strField("prefixMatched"), "SPEC:");
}

TEST(VagentStringScanning, ComposabilityTabMatchAndUpto) {
  constexpr std::string_view kSrc = R"(
event Run

agent Scanner {
  var mineral: Str := ""

  on Run {
    mineral := "SPEC:quartz;count=42" ? (tab(match("SPEC:")) & tab(upto(";")))
  }
}
)";
  ScanHarness h;
  ASSERT_TRUE(h.compile(std::string(kSrc))) << h.lastError;
  ASSERT_TRUE(h.init()) << h.lastError;
  ASSERT_TRUE(h.dispatch("Run")) << h.lastError;

  EXPECT_EQ(h.strField("mineral"), "quartz");
}

TEST(VagentStringScanning, FindGeneratorAndComposability) {
  constexpr std::string_view kSrc = R"(
event Run

agent Scanner {
  var header: Str := ""
  var foundPos: Int := 0

  on Run {
    foundPos := "--- START ---\nTitle: Nelsonian\n--- END ---" ? find("Title:")
    header := "--- START ---\nTitle: Nelsonian\n--- END ---" ? tab(find("Title:"))
  }
}
)";
  ScanHarness h;
  ASSERT_TRUE(h.compile(std::string(kSrc))) << h.lastError;
  ASSERT_TRUE(h.init()) << h.lastError;
  ASSERT_TRUE(h.dispatch("Run")) << h.lastError;

  // "--- START ---\n" length is 14; 1-based start of "Title:" is 15
  EXPECT_EQ(h.intField("foundPos"), 15);
  EXPECT_EQ(h.strField("header"), "--- START ---\n");
}

TEST(VagentStringScanning, AnyAndManyCharacterSets) {
  constexpr std::string_view kSrc = R"(
event Run

agent Scanner {
  var digits: Str := ""
  var anyPos: Int := 0

  on Run {
    anyPos := "123" ? any("0123456789")
    digits := "item0042end" ? (tab(match("item")) & tab(many("0123456789")))
  }
}
)";
  ScanHarness h;
  ASSERT_TRUE(h.compile(std::string(kSrc))) << h.lastError;
  ASSERT_TRUE(h.init()) << h.lastError;
  ASSERT_TRUE(h.dispatch("Run")) << h.lastError;

  EXPECT_EQ(h.intField("anyPos"), 2);
  EXPECT_EQ(h.strField("digits"), "0042");
}

TEST(VagentStringScanning, BalBalancedDelimiterScanning) {
  constexpr std::string_view kSrc = R"(
event Run

agent Scanner {
  var firstArg: Str := ""

  on Run {
    firstArg := "f(a, g(b, c)), d" ? tab(bal(","))
  }
}
)";
  ScanHarness h;
  ASSERT_TRUE(h.compile(std::string(kSrc))) << h.lastError;
  ASSERT_TRUE(h.init()) << h.lastError;
  ASSERT_TRUE(h.dispatch("Run")) << h.lastError;

  EXPECT_EQ(h.strField("firstArg"), "f(a, g(b, c))");
}

TEST(VagentStringScanning, BacktrackRestoresPosAutomatically) {
  constexpr std::string_view kSrc = R"(
event Run

agent Scanner {
  var produced: Str := ""

  on Run {
    // First alternative moves to 5 then fails downstream (match "XYZ").
    // Backtracking must restore &pos to 1 so the second alternative starts at 1.
    produced := "abcdef" ? ((tab(5) & tab(match("XYZ"))) | tab(3))
  }
}
)";
  ScanHarness h;
  ASSERT_TRUE(h.compile(std::string(kSrc))) << h.lastError;
  ASSERT_TRUE(h.init()) << h.lastError;
  ASSERT_TRUE(h.dispatch("Run")) << h.lastError;

  EXPECT_EQ(h.strField("produced"), "ab");
}

TEST(VagentStringScanning, DirectStdGoalRoutines) {
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  VortexStdLib stdlib{core, vm};

  // Push a subject onto d.scan
  CellRef subj = arena.makeCell("SPEC:quartz;count=42");
  auto cursor  = stdlib.scanPush(subj);
  ASSERT_TRUE(cursor.has_value());
  ASSERT_NE(*cursor, noCell);

  EXPECT_EQ(stdlib.scanPos(), 1);
  EXPECT_EQ(stdlib.scanMatch("SPEC:"), 6);
  // match does not move &pos
  EXPECT_EQ(stdlib.scanPos(), 1);

  // Tab to 6 produces "SPEC:"
  auto tab1 = stdlib.scanTab(6);
  ASSERT_TRUE(tab1.has_value());
  EXPECT_EQ(*tab1, "SPEC:");
  EXPECT_EQ(stdlib.scanPos(), 6);

  // Upto finds ';'
  auto semis = stdlib.scanUpto(";");
  ASSERT_FALSE(semis.empty());
  EXPECT_EQ(semis[0], 12);

  // Tab to 12 produces "quartz"
  auto tab2 = stdlib.scanTab(semis[0]);
  ASSERT_TRUE(tab2.has_value());
  EXPECT_EQ(*tab2, "quartz");
  EXPECT_EQ(stdlib.scanPos(), 12);

  // Match ";count="
  auto matchCount = stdlib.scanMatch(";count=");
  ASSERT_TRUE(matchCount.has_value());
  EXPECT_EQ(*matchCount, 19);
  auto tab3 = stdlib.scanTab(*matchCount);
  ASSERT_TRUE(tab3.has_value());
  EXPECT_EQ(*tab3, ";count=");

  // many digits
  auto manyDigits = stdlib.scanMany("0123456789");
  ASSERT_TRUE(manyDigits.has_value());
  EXPECT_EQ(*manyDigits, 21);
  auto tab4 = stdlib.scanTab(*manyDigits);
  ASSERT_TRUE(tab4.has_value());
  EXPECT_EQ(*tab4, "42");

  // Negative indexing: tab(0) moves to len + 1
  auto endTab = stdlib.scanTab(0);
  ASSERT_TRUE(endTab.has_value());
  EXPECT_EQ(stdlib.scanPos(), 21); // len 20 + 1 = 21

  stdlib.scanPop(*cursor);
}

} // namespace xanadu::vagent::runtime

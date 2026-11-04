#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vagent/checker.hpp"
#include "common/xanadu/vagent/frontend.hpp"
#include "common/xanadu/vagent/meta.hpp"
#include "common/xanadu/vagent/parser.hpp"

namespace {
using namespace xanadu::vagent;
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::Negward;
using zigzag::Posward;
using zigzag::vortex::CellValue;
using zigzag::vortex::OpcodeKind;

// ---------------------------------------------------------------------------
// Checker-side helpers: the full parse → resolve → expand → check pipeline
// so code"…" literals carry fragment records.
// ---------------------------------------------------------------------------

std::vector<Diagnostic> diagnostics(std::string_view source) {
  auto program = compile(source);
  return program ? std::vector<Diagnostic>{} : std::move(program.error());
}

std::string describe(const std::vector<Diagnostic> &errors) {
  std::string result;
  for (const auto &error : errors) result += formatDiagnostic(error) + '\n';
  return result;
}

void rejects(std::string_view source, DiagnosticCode code) {
  const auto errors = diagnostics(source);
  EXPECT_FALSE(errors.empty());
  EXPECT_TRUE(std::ranges::any_of(errors, [&](const Diagnostic &error) {
    return error.code == code;
  })) << describe(errors);
}

void accepts(std::string_view source) {
  const auto errors = diagnostics(source);
  EXPECT_TRUE(errors.empty()) << describe(errors);
}

// ---------------------------------------------------------------------------
// Engine-side harness: one arena, one core/VM, a minted bind set.
// ---------------------------------------------------------------------------

struct MetaHarness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  CellRef programRoot{zigzag::noCell};
  CellRef agentCell{zigzag::noCell};
  std::optional<MetaRuntime> meta;

  MetaHarness() {
    programRoot = arena.makeCell();
    agentCell   = arena.makeCell();
    auto binds  = mintMetaBindSet(arena, core, programRoot, agentCell);
    EXPECT_TRUE(binds.has_value()) << (binds ? "" : binds.error());
    if (binds) meta.emplace(core, vm, *binds);
  }
  MetaRuntime &rt() { return *meta; }
  const MetaBindSet &binds() { return meta->binds(); }
};

/// A module cell labelled `name` filed on the program root's +d.stdlib.
CellRef mintModule(MetaHarness &h, std::string_view name) {
  const auto module = h.arena.makeCell(name);
  zigzag::expectWritten(
      h.arena.link(h.programRoot, h.core.dims().stdlib, Posward, module));
  return module;
}

/// A symbol cell whose *content* is `name` — §6.1's "symbols are cell
/// content" — on the module's +d.vars rank.
CellRef mintSymbol(MetaHarness &h, CellRef module, std::string_view name) {
  const auto symbol = h.arena.makeCell(name);
  zigzag::expectWritten(
      h.arena.link(module, h.core.dims().vars, Posward, symbol));
  return symbol;
}

/// A clause carrying label `label` on `cell`'s +d.clause rank.
CellRef mintClause(MetaHarness &h, CellRef cell, std::string_view label) {
  const auto clause = h.arena.makeCell();
  zigzag::expectWritten(h.arena.link(clause, h.core.dims().name, Posward,
                                     h.arena.makeCell(label)));
  zigzag::expectWritten(
      h.arena.link(cell, h.core.dims().clause, Posward, clause));
  return clause;
}

/// A fragment head: `+d.entry → entry` plus `+d.name → label`.
CellRef mintHead(MetaHarness &h, CellRef entry, std::string_view label) {
  const auto head = h.arena.makeCell();
  zigzag::expectWritten(
      h.arena.link(head, h.binds().dims.entry, Posward, entry));
  if (!label.empty())
    zigzag::expectWritten(h.arena.link(head, h.core.dims().name, Posward,
                                       h.arena.makeCell(label)));
  return head;
}

/// A descriptor cell with `result`/`produced` records on +d.vars.
struct Descriptor {
  CellRef cell;
  CellRef result;
  CellRef produced;
};
Descriptor mintDescriptor(MetaHarness &h, CellRef head) {
  const auto descriptor = h.arena.makeCell();
  zigzag::expectWritten(
      h.arena.link(head, h.core.dims().values, Posward, descriptor));
  Descriptor out{descriptor, h.arena.makeCell(), h.arena.makeScalarCell(false)};
  const auto record = [&](std::string_view key, CellRef value) {
    const auto rec = h.arena.makeCell(key);
    zigzag::expectWritten(
        h.arena.link(rec, h.core.dims().values, Posward, value));
    zigzag::expectWritten(
        h.arena.link(zigzag::rankTail(h.arena, descriptor, h.core.dims().vars),
                     h.core.dims().vars, Posward, rec));
  };
  record("result", out.result);
  record("produced", out.produced);
  return out;
}

// ===========================================================================
// Checker tests
// ===========================================================================

TEST(VagentMetaTest, StdMetaImportAccepted) {
  accepts(R"(
import std:meta
agent MetaUser {
  def probe(): Int { 0 }
}
)");
}

TEST(VagentMetaTest, ReadSurfaceMembersCheck) {
  accepts(R"(
import std:meta
agent MetaUser {
  def probe(): Int {
    let root := program()
    let mine := agent_cell()
    let mod := module_named("demo")
    let sym := symbol_named("demo", "Forager")
    let plans := clauses_of(root)
    let t := test_of(root)
    let b := body_of(root)
    let p := plan_named("demo:Forager.f")
    let c := code_named("demo:Forager.f")
    let l := label_of(root)
    let ok := validate(root)
    let w := weight_of(root)
    let v := value_of(root)
    let r := next_rand(root)
    0
  }
}
)");
}

TEST(VagentMetaTest, RunIsReservedOnlyUnderTheImport) {
  // Without the import a user `run` is an ordinary decl.
  accepts(R"(
agent Plain {
  def run(): Int { 1 }
  def use(): Int { run() }
}
)");
  // With it, declarations and bindings may not shadow run/value_of.
  rejects(R"(
import std:meta
agent MetaUser {
  def run(): Int { 1 }
}
)",
          DiagnosticCode::InvalidIdentifier);
  rejects(R"(
import std:meta
agent MetaUser {
  def probe(): Int {
    let run := 1
    run
  }
}
)",
          DiagnosticCode::InvalidIdentifier);
  rejects(R"(
import std:meta
agent MetaUser {
  def value_of(x: Cell): Int { 0 }
}
)",
          DiagnosticCode::InvalidIdentifier);
}

TEST(VagentMetaTest, ReservedModulePrefixesRefused) {
  rejects("module src:demo\nagent A { def f(): Int { 0 } }\n",
          DiagnosticCode::InvalidIdentifier);
  // Module-path segments are words — the reserved head is what the
  // refusal keys on, so `reify:m`/`patch:x` exercise it.
  rejects("module reify:m\nagent A { def f(): Int { 0 } }\n",
          DiagnosticCode::InvalidIdentifier);
  rejects("module patch:x\nagent A { def f(): Int { 0 } }\n",
          DiagnosticCode::InvalidIdentifier);
  // A qualified-looking name outside the reserved heads stays legal.
  accepts("module demo:mirror\nagent A { def f(): Int { 0 } }\n");
}

TEST(VagentMetaTest, RunSpecialFormTypesResultAndList) {
  // run(c) on a nullary fragment gives back the body's result type.
  accepts(R"(
import std:meta
agent MetaUser {
  def probe(): Int { run(code"40 + 2") }
}
)");
  // Heterogeneous argument lists are legal — the only call form that
  // allows them.
  accepts(R"(
import std:meta
agent MetaUser {
  def probe(): Int { run(code"n: Int, s: Str | n", [1, "x"]) }
}
)");
  // The second operand must be a list literal.
  rejects(R"(
import std:meta
agent MetaUser {
  def probe(): Int { run(code"1", 5) }
}
)",
          DiagnosticCode::TypeMismatch);
  // A statically-known fragment checks arity at compile time.
  rejects(R"(
import std:meta
agent MetaUser {
  def probe(): Int { run(code"n: Int, m: Int | n", [1]) }
}
)",
          DiagnosticCode::TypeMismatch);
  // … and element types against the declared signature.
  rejects(R"(
import std:meta
agent MetaUser {
  def probe(): Int { run(code"n: Int | n", ["oops"]) }
}
)",
          DiagnosticCode::TypeMismatch);
  // A Code-parameterized fragment is splice-only: a named refusal.
  rejects(R"(
import std:meta
agent MetaUser {
  def probe(): Int { run(code"p: Code<Int> | splice p", [code"1"]) }
}
)",
          DiagnosticCode::InvalidDeclaration);
  // The operand itself must be a fragment.
  rejects(R"(
import std:meta
agent MetaUser {
  def probe(): Int { run(7) }
}
)",
          DiagnosticCode::TypeMismatch);
}

TEST(VagentMetaTest, RunIsAnAction) {
  // A pure context refuses run like every other action.
  rejects(R"(
import std:meta
agent MetaUser {
  var x: Int := run(code"1")
}
)",
          DiagnosticCode::ImpureCondition);
}

TEST(VagentMetaTest, ReifySpecialForm) {
  accepts(R"(
import std:meta
agent MetaUser {
  def probe(): Code<Int> { reify(code"1") }
}
)");
  rejects(R"(
import std:meta
agent MetaUser {
  def probe(): Int { reify(3) }
}
)",
          DiagnosticCode::TypeMismatch);
  rejects(R"(
import std:meta
agent MetaUser {
  def probe(): Int { reify(code"1", code"2") }
}
)",
          DiagnosticCode::TypeMismatch);
}

TEST(VagentMetaTest, MetaMembersQualifiedThroughAlias) {
  accepts(R"(
import std:meta as m
agent MetaUser {
  def probe(): Int {
    let r := m.program()
    let v := m.value_of(r)
    0
  }
}
)");
}

// ===========================================================================
// Engine tests
// ===========================================================================

TEST(VagentMetaTest, BindMintRefusesUnderLiveMarks) {
  MetaHarness h;
  const auto extra   = h.arena.makeCell();
  const auto mark    = h.arena.mark();
  const auto refused = mintMetaBindSet(h.arena, h.core, h.programRoot, extra);
  EXPECT_FALSE(refused.has_value());
  h.arena.release(mark);
}

TEST(VagentMetaTest, ModuleSymbolAndPlanNamed) {
  MetaHarness h;
  const auto demo    = mintModule(h, "demo");
  const auto forager = mintSymbol(h, demo, "Forager");
  const auto clause  = mintClause(h, forager, "demo:Forager.f");

  EXPECT_EQ(h.rt().program(), h.programRoot);
  EXPECT_EQ(h.rt().agentCell(), h.agentCell);
  EXPECT_EQ(h.rt().moduleNamed("demo"), demo);
  EXPECT_FALSE(h.rt().moduleNamed("absent").has_value());
  // Symbols are cell content on the module's d.vars rank.
  EXPECT_EQ(h.rt().symbolNamed("demo", "Forager"), forager);
  EXPECT_FALSE(h.rt().symbolNamed("demo", "Absent").has_value());
  EXPECT_FALSE(h.rt().symbolNamed("absent", "Forager").has_value());
  // clauses_of walks the symbol's +d.clause rank.
  EXPECT_EQ(h.rt().clausesOf(forager).size(), 1U);
  EXPECT_EQ(h.rt().clausesOf(forager).front(), clause);
  // plan_named matches the full qualified spelling on the clause's
  // +d.name label, and the module arm narrows on a resolvable
  // qualifier.
  EXPECT_EQ(h.rt().planNamed("demo:Forager.f"), clause);
  EXPECT_FALSE(h.rt().planNamed("Forager.f").has_value());
  // label_of is the +d.name text.
  EXPECT_EQ(h.rt().labelOf(clause), "demo:Forager.f");
}

TEST(VagentMetaTest, CodeNamedTreatsLabelsAsOpaque) {
  MetaHarness h;
  const auto entry = h.vm.mintOpcode(OpcodeKind::Return);
  const auto head  = mintHead(h, entry, "demo:Forager.f");
  zigzag::expectWritten(
      h.arena.link(h.agentCell, h.binds().dims.code, Posward, head));
  // The registered head rank wins…
  EXPECT_EQ(h.rt().codeNamed("demo:Forager.f"), head);
  // …and a first-`:` split never reroutes to a module arm.
  mintModule(h, "demo");
  EXPECT_EQ(h.rt().codeNamed("demo:Forager.f"), head);
  EXPECT_FALSE(h.rt().codeNamed("Forager.f").has_value());
}

TEST(VagentMetaTest, CodeNamedFallsBackToProgramImage) {
  MetaHarness h;
  const auto demo    = mintModule(h, "demo");
  const auto forager = mintSymbol(h, demo, "Forager");
  const auto entry   = h.vm.mintOpcode(OpcodeKind::Return);
  // A head buried in the program image: symbol +d.values → head.
  const auto head = mintHead(h, entry, "demo:Forager.f");
  zigzag::expectWritten(
      h.arena.link(forager, h.core.dims().values, Posward, head));
  // Nothing on the agent's d.code rank — the program walk finds it.
  EXPECT_EQ(h.rt().codeNamed("demo:Forager.f"), head);
}

TEST(VagentMetaTest, HeadVsOpAttachResolution) {
  MetaHarness h;
  const auto op   = h.vm.mintOpcode(OpcodeKind::Return);
  const auto head = mintHead(h, op, "h");
  const auto bare = h.arena.makeCell();
  EXPECT_EQ(h.rt().graphRoot(op), op);
  EXPECT_EQ(h.rt().graphRoot(head), op);
  EXPECT_FALSE(h.rt().graphRoot(bare).has_value());
}

TEST(VagentMetaTest, TestAndBodyWrapMemoizesClauseSide) {
  MetaHarness h;
  const auto entry  = h.vm.mintOpcode(OpcodeKind::Return);
  const auto attach = mintHead(h, entry, "demo:key");
  const auto clause = h.arena.makeCell();
  zigzag::expectWritten(
      h.arena.link(clause, h.core.dims().grab, Posward, attach));
  zigzag::expectWritten(
      h.arena.link(clause, h.core.dims().spin, Posward, attach));

  const auto first  = h.rt().testOf(clause);
  const auto second = h.rt().testOf(clause);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first, second) << "same clause + same key must memo-hit";

  // The wrapper is a session-side head: +d.entry delegates to the
  // attach (a second +d.entry → entry edge would evict the attach's —
  // -d edges are single-valued), so the chain resolves attach → entry.
  EXPECT_EQ(h.arena.linked(*first, h.binds().dims.entry, Posward), attach);
  EXPECT_EQ(h.arena.linked(attach, h.binds().dims.entry, Posward), entry);
  EXPECT_TRUE(
      zigzag::rankAfter(h.arena, h.agentCell, h.binds().dims.code).empty());

  const auto body = h.rt().bodyOf(clause);
  ASSERT_TRUE(body.has_value());
  EXPECT_NE(body, first) << "test/body memo records coexist";

  // The memo record: clause +d.code → {+d.values → wrapper, +d.name →
  // kind, +d.grab → fresh scalar key cell}.
  const auto record = h.arena.linked(clause, h.binds().dims.code, Posward);
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(h.arena.linked(*record, h.core.dims().values, Posward), first);
  const auto kind = h.arena.linked(*record, h.core.dims().name, Posward);
  ASSERT_TRUE(kind.has_value());
  EXPECT_EQ(h.arena.textOf(*kind), "test");
  const auto keyCell = h.arena.linked(*record, h.core.dims().grab, Posward);
  ASSERT_TRUE(keyCell.has_value());
  EXPECT_EQ(h.arena.textOf(*keyCell), "demo:key");
}

TEST(VagentMetaTest, LabellessAttachNeverMemoizes) {
  MetaHarness h;
  const auto entry  = h.vm.mintOpcode(OpcodeKind::Return);
  const auto attach = mintHead(h, entry, "");
  const auto clause = h.arena.makeCell();
  zigzag::expectWritten(
      h.arena.link(clause, h.core.dims().grab, Posward, attach));

  const auto first  = h.rt().testOf(clause);
  const auto second = h.rt().testOf(clause);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_NE(first, second) << "a label-less attach rewraps every time";
  EXPECT_FALSE(h.arena.linked(clause, h.binds().dims.code, Posward).has_value())
      << "…and no memo record is created";
}

TEST(VagentMetaTest, HeadlessRunRefuses) {
  MetaHarness h;
  const auto op      = h.vm.mintOpcode(OpcodeKind::Return);
  const auto outcome = h.rt().runFragment(op, {});
  EXPECT_EQ(outcome.refusal, MetaRunOutcome::Refusal::NotAHead);
  const auto bare = h.arena.makeCell();
  EXPECT_EQ(h.rt().runFragment(bare, {}).refusal,
            MetaRunOutcome::Refusal::NotAHead);
}

TEST(VagentMetaTest, RunChecksDescriptorArityAndSpills) {
  MetaHarness h;
  // A fragment whose body writes `produced := true` then returns: a
  // Value op on the descriptor's produced cell + a Return tail.
  const auto ret   = h.vm.mintOpcode(OpcodeKind::Return);
  const auto write = h.vm.mintOpcode(OpcodeKind::Value);
  zigzag::expectWritten(h.arena.link(write, h.core.dims().spin, Posward, ret));

  const auto head       = mintHead(h, write, "demo:frag");
  const auto descriptor = mintDescriptor(h, head);
  // Value's in-wing: [target cell, off, len, replacement]. The
  // replacement operand is a cell whose content is `true`.
  const auto zero = h.arena.makeScalarCell(std::int64_t{0});
  const auto yes  = h.arena.makeScalarCell(true);
  h.core.bindInput(write, descriptor.produced);
  h.core.bindInput(write, zero);
  h.core.bindInput(write, zero);
  h.core.bindInput(write, yes);
  const auto sink = h.arena.makeCell();
  h.core.bindOutput(write, sink);

  // A param cell on +d.grab enforces arity.
  const auto param = h.arena.makeCell();
  zigzag::expectWritten(
      h.arena.link(param, h.core.dims().name, Posward, h.arena.makeCell("n")));
  zigzag::expectWritten(
      h.arena.link(descriptor.cell, h.core.dims().grab, Posward, param));
  EXPECT_EQ(h.rt().runFragment(head, {}).refusal,
            MetaRunOutcome::Refusal::ArityMismatch);
  zigzag::expectWritten(
      h.arena.unlink(descriptor.cell, h.core.dims().grab, Posward));

  // Spill: produced=false before the run; the body writes true; the
  // outcome reports the post-run flag and the descriptor is restored.
  const auto outcome = h.rt().runFragment(head, {});
  EXPECT_EQ(outcome.refusal, MetaRunOutcome::Refusal::None) << outcome.detail;
  EXPECT_TRUE(outcome.produced);
  EXPECT_EQ(h.arena.asBool(descriptor.produced), false)
      << "descriptor spill must restore produced";
}

TEST(VagentMetaTest, ValidateWalksOpGraph) {
  MetaHarness h;
  const OpcodeKind ops[] = {OpcodeKind::Nop, OpcodeKind::Return};
  const auto sequence    = h.vm.assembleSequence(ops);
  ASSERT_TRUE(sequence.has_value());
  EXPECT_TRUE(h.rt().validate(*sequence).ok);

  // A run-reachable graph tailing on anything but Return fails.
  const auto dangling = h.vm.mintOpcode(OpcodeKind::Nop);
  const auto report   = h.rt().validate(dangling);
  EXPECT_FALSE(report.ok);
  EXPECT_NE(report.detail.find("Return"), std::string::npos);

  // An out-of-range op int64 fails on the bound ceiling.
  const auto bogus = h.arena.makeScalarCell(std::int64_t{9999});
  EXPECT_FALSE(h.rt().validate(bogus).ok);

  // A bound-output New in a run-reachable graph fails.
  const auto alloc = h.vm.mintOpcode(OpcodeKind::New);
  const auto fin   = h.vm.mintOpcode(OpcodeKind::Return);
  zigzag::expectWritten(h.arena.link(alloc, h.core.dims().spin, Posward, fin));
  const auto cell = h.arena.makeCell();
  const auto dim  = h.arena.makeScalarCell(std::int64_t{h.binds().dims.weight});
  const auto pos  = h.arena.makeScalarCell(std::int64_t{1});
  h.core.bindInput(alloc, cell);
  h.core.bindInput(alloc, dim);
  h.core.bindInput(alloc, pos);
  const auto outSlot = h.arena.makeCell();
  h.core.bindOutput(alloc, outSlot);
  EXPECT_FALSE(h.rt().validate(alloc).ok);

  // An impure op (New) under a pure-flagged descriptor fails.
  MetaValidateOptions pure{.runReachable = true, .expectPure = true};
  EXPECT_FALSE(h.rt().validate(alloc, pure).ok);
}

TEST(VagentMetaTest, ValidateRejectsBrokenControlTargets) {
  MetaHarness h;
  // A Jump whose target slot names a non-op cell.
  const auto jump    = h.vm.mintOpcode(OpcodeKind::Jump);
  const auto notAnOp = h.arena.makeCell("text");
  h.core.bindInput(jump, notAnOp);
  EXPECT_FALSE(h.rt().validate(jump, {.runReachable = false}).ok);
  // A Jump with no slot at all fails the missing-target check.
  const auto bare = h.vm.mintOpcode(OpcodeKind::Jump);
  EXPECT_FALSE(h.rt().validate(bare, {.runReachable = false}).ok);
  // A Jump to an op is fine when the graph isn't run-reachable.
  const auto landing = h.vm.mintOpcode(OpcodeKind::Return);
  const auto good    = h.vm.mintOpcode(OpcodeKind::Jump);
  h.core.bindInput(good, landing);
  EXPECT_TRUE(h.rt().validate(good, {.runReachable = false}).ok);
}

TEST(VagentMetaTest, WeightValueLabelAndNextRand) {
  MetaHarness h;
  // Junction arm: +d.weight scalar.
  const auto junction = h.arena.makeCell();
  zigzag::expectWritten(h.arena.link(junction, h.binds().dims.weight, Posward,
                                     h.arena.makeScalarCell(0.75)));
  EXPECT_EQ(h.rt().weightOf(junction), 0.75);

  // Labelled arm: the agent's d.vars record keyed by the label.
  const auto named = h.arena.makeCell();
  zigzag::expectWritten(h.arena.link(named, h.core.dims().name, Posward,
                                     h.arena.makeCell("syn:key")));
  const auto record = h.arena.makeCell("syn:key");
  zigzag::expectWritten(h.arena.link(record, h.core.dims().values, Posward,
                                     h.arena.makeScalarCell(1.25)));
  zigzag::expectWritten(
      h.arena.link(zigzag::rankTail(h.arena, h.agentCell, h.core.dims().vars),
                   h.core.dims().vars, Posward, record));
  EXPECT_EQ(h.rt().weightOf(named), 1.25);

  // value_of is the rendered content; label_of the +d.name text.
  const auto text = h.arena.makeCell("hello");
  EXPECT_EQ(h.rt().valueOf(text), CellValue(std::string{"hello"}));
  const auto scalar = h.arena.makeScalarCell(std::int64_t{7});
  EXPECT_EQ(h.rt().valueOf(scalar), CellValue(std::int64_t{7}));
  EXPECT_EQ(h.rt().labelOf(named), "syn:key");

  // next_rand: deterministic LCG over the seed cell's Int.
  const auto seed  = h.arena.makeScalarCell(std::int64_t{1});
  const auto first = h.rt().nextRand(seed);
  EXPECT_EQ(first, h.rt().nextRand(seed));
  const auto other = h.arena.makeScalarCell(std::int64_t{2});
  EXPECT_NE(first, h.rt().nextRand(other));
}

// ===========================================================================
// reify
// ===========================================================================

/// A recipe member cell: int64 `pos` content, plus an optional name /
/// refFor / values edge.
struct Recipe {
  /// `from +dim(dir) → to` tuple, expressed as four sequential members.
  static void linkTuple(MetaHarness &h, std::vector<CellRef> &out, CellRef from,
                        std::int64_t dir, std::string_view dim, CellRef to) {
    out.push_back(h.arena.makeScalarCell(static_cast<std::int64_t>(from)));
    const auto anchor = h.arena.makeScalarCell(std::int64_t{0});
    zigzag::expectWritten(h.arena.link(anchor, h.core.dims().name, Posward,
                                       h.arena.makeCell(dim)));
    out.push_back(anchor);
    out.push_back(h.arena.makeScalarCell(dir));
    out.push_back(h.arena.makeScalarCell(static_cast<std::int64_t>(to)));
  }
  /// A literal-born field naming `pos` — the source cell whose content
  /// the twin copies.
  static CellRef literal(MetaHarness &h, CellRef pos) {
    return h.arena.makeScalarCell(static_cast<std::int64_t>(pos));
  }
  /// A ref-capture member: +d.refFor → the source slot, optional
  /// +d.name (dimension ref) and +d.values (cap$N binding hint).
  static CellRef refCapture(MetaHarness &h, CellRef slot,
                            std::string_view dimName = {},
                            CellRef binding          = zigzag::noCell) {
    const auto member = h.arena.makeScalarCell(static_cast<std::int64_t>(slot));
    zigzag::expectWritten(
        h.arena.link(member, h.binds().dims.refFor, Posward, slot));
    if (!dimName.empty())
      zigzag::expectWritten(h.arena.link(member, h.core.dims().name, Posward,
                                         h.arena.makeCell(dimName)));
    if (binding != zigzag::noCell)
      zigzag::expectWritten(
          h.arena.link(member, h.core.dims().values, Posward, binding));
    return member;
  }
  /// Chain members onto the head's +d.recipe (+d.step links).
  static void chain(MetaHarness &h, CellRef head,
                    const std::vector<CellRef> &members) {
    for (std::size_t i = 0; i < members.size(); ++i) {
      if (i == 0)
        zigzag::expectWritten(
            h.arena.link(head, h.binds().dims.recipe, Posward, members[i]));
      else
        zigzag::expectWritten(h.arena.link(members[i - 1], h.core.dims().step,
                                           Posward, members[i]));
    }
  }
};

/// The head's `+d.refs` inventory: head `+d.refs → first record`, then
/// `+d.step` links between records (the §6.1 unified schema).
inline std::vector<CellRef> refsOf(MetaHarness &h, CellRef head) {
  std::vector<CellRef> out;
  for (auto rec = h.arena.linked(head, h.binds().dims.refs, Posward);
       rec && h.arena.contains(*rec);
       rec = h.arena.linked(*rec, h.core.dims().step, Posward))
    out.push_back(*rec);
  return out;
}

TEST(VagentMetaTest, ReifyReplaysRecipeAndMintsLabels) {
  MetaHarness h;
  const auto dim = h.core.findOrMintDimension("d.reify.test");
  ASSERT_NE(dim, zigzag::noCell);

  // Source fragment: a head + two graph cells linked a +d.reify.test→ b.
  const auto a = h.arena.makeCell("content-a");
  const auto b = h.arena.makeCell();
  zigzag::expectWritten(h.arena.link(a, dim, Posward, b));
  const auto entry = h.vm.mintOpcode(OpcodeKind::Return);
  const auto head  = mintHead(h, entry, "src:demo");

  // +d.refs name-record + ref-record on the source head.
  const auto nameRecord = h.arena.makeCell();
  const auto nameCell   = h.arena.makeCell("a");
  zigzag::expectWritten(
      h.arena.link(nameRecord, h.binds().dims.ref, Posward, a));
  zigzag::expectWritten(
      h.arena.link(nameRecord, h.core.dims().name, Posward, nameCell));
  const auto refRecord = h.arena.makeCell();
  zigzag::expectWritten(
      h.arena.link(refRecord, h.binds().dims.ref, Posward, b));
  zigzag::expectWritten(
      h.arena.link(head, h.binds().dims.refs, Posward, nameRecord));
  zigzag::expectWritten(
      h.arena.link(nameRecord, h.core.dims().step, Posward, refRecord));

  // Recipe: literal a, literal b, the link tuple a +d.reify.test→ b.
  std::vector<CellRef> members;
  members.push_back(Recipe::literal(h, a));
  members.push_back(Recipe::literal(h, b));
  Recipe::linkTuple(h, members, a, 1, "d.reify.test", b);
  Recipe::chain(h, head, members);

  const auto copy = h.rt().reify(head);
  ASSERT_TRUE(copy.has_value()) << (copy ? "" : copy.error());
  // The minted label is reify:<src>:<serial> off the shared counter.
  EXPECT_EQ(h.rt().labelOf(*copy), "reify:src:demo:0");

  // Find the twins via the copy's own +d.refs: name-record ref → twinA.
  CellRef twinA{zigzag::noCell};
  for (const auto rec : refsOf(h, *copy)) {
    const auto target = h.arena.linked(rec, h.binds().dims.ref, Posward);
    const auto name   = h.arena.linked(rec, h.core.dims().name, Posward);
    ASSERT_TRUE(target.has_value());
    if (name && h.arena.textOf(*name) == "a") twinA = *target;
  }
  ASSERT_NE(twinA, zigzag::noCell);
  EXPECT_NE(twinA, a);
  EXPECT_EQ(h.arena.textOf(twinA), "content-a");
  const auto twinBLink = h.arena.linked(twinA, dim, Posward);
  ASSERT_TRUE(twinBLink.has_value()) << "the tuple must replay";
  EXPECT_NE(*twinBLink, b);

  // The re-pointed cells carry fresh d.mesh.relocation markers on
  // ref-records; name-records carry none.
  bool sawMarker = false;
  for (const auto rec : refsOf(h, *copy)) {
    const auto target = h.arena.linked(rec, h.binds().dims.ref, Posward);
    const auto name   = h.arena.linked(rec, h.core.dims().name, Posward);
    if (!name && target &&
        h.arena.linked(*target, h.binds().dims.meshRelocation, Posward))
      sawMarker = true;
  }
  EXPECT_TRUE(sawMarker);

  // The copy's own recipe is self-describing — reify again and the
  // replay yields a third graph (serial ticks the counter).
  const auto again = h.rt().reify(*copy);
  ASSERT_TRUE(again.has_value()) << (again ? "" : again.error());
  EXPECT_EQ(h.rt().labelOf(*again), "reify:reify:src:demo:0:1");
}

TEST(VagentMetaTest, ReifyRefCaptureRebindsAndAppendsCloneRank) {
  MetaHarness h;
  const auto a     = h.arena.makeCell();
  const auto entry = h.vm.mintOpcode(OpcodeKind::Return);
  const auto head  = mintHead(h, entry, "src:slots");

  // A slot cell holding an *internal* referent (a), and a cap$N-style
  // binding the slot should append to.
  const auto slot    = h.arena.makeScalarCell(static_cast<std::int64_t>(a));
  const auto binding = h.arena.makeCell();

  std::vector<CellRef> members;
  members.push_back(Recipe::literal(h, a));
  members.push_back(Recipe::refCapture(h, slot, {}, binding));
  Recipe::chain(h, head, members);

  const auto copy = h.rt().reify(head);
  ASSERT_TRUE(copy.has_value()) << (copy ? "" : copy.error());

  // The new slot rebinds to the twin — internal referents go through
  // the map, never verbatim. `d.clone` resolves on the d.dims rank —
  // it is a native system dim, so the arena-local name table does not
  // carry it.
  const auto cloneDim = h.core.findDimension("d.clone");
  ASSERT_TRUE(cloneDim.has_value());
  const auto clones = std::ranges::to<std::vector>(
      zigzag::rankAfter(h.arena, binding, *cloneDim));
  ASSERT_EQ(clones.size(), 1U);
  const auto newSlot = clones.front();
  EXPECT_NE(newSlot, slot);
  const auto referent = h.arena.asInt64(newSlot);
  ASSERT_TRUE(referent.has_value());
  EXPECT_NE(static_cast<CellRef>(*referent), a);
}

TEST(VagentMetaTest, ReifyRefusesDimensionMintUnderMarks) {
  MetaHarness h;
  const auto a     = h.arena.makeCell();
  const auto entry = h.vm.mintOpcode(OpcodeKind::Return);
  const auto head  = mintHead(h, entry, "src:dim");
  std::vector<CellRef> members;
  Recipe::linkTuple(h, members, a, 1, "d.never.filed", a);
  Recipe::chain(h, head, members);

  const auto mark = h.arena.mark();
  const auto copy = h.rt().reify(head);
  EXPECT_FALSE(copy.has_value());
  if (!copy) EXPECT_NE(copy.error().find("live marks"), std::string::npos);
  h.arena.release(mark);

  // Outside marks the same dim mints and the replay proceeds.
  const auto after = h.rt().reify(head);
  EXPECT_TRUE(after.has_value()) << (after ? "" : after.error());
}

TEST(VagentMetaTest, ReifyRefusesNonHeadOperand) {
  MetaHarness h;
  const auto bare = h.arena.makeCell();
  EXPECT_FALSE(h.rt().reify(bare).has_value());
  const auto op = h.vm.mintOpcode(OpcodeKind::Return);
  EXPECT_FALSE(h.rt().reify(op).has_value());
}

} // namespace

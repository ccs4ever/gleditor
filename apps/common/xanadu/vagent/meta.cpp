#include "common/xanadu/vagent/meta.hpp"

#include "common/xanadu/vagent/records.hpp"

#include <bit>
#include <deque>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace xanadu::vagent {
namespace {

/// The twelve apparatus dims §6.1's bind set carries, in file order.
constexpr std::pair<const char *, DimRef MetaDimensions::*> kDimNames[] = {
    {"d.code", &MetaDimensions::code},
    {"d.patch", &MetaDimensions::patch},
    {"d.weight", &MetaDimensions::weight},
    {"d.synapse", &MetaDimensions::synapse},
    {"d.entry", &MetaDimensions::entry},
    {"d.patch-head", &MetaDimensions::patchHead},
    {"d.validate", &MetaDimensions::validate},
    {"d.refs", &MetaDimensions::refs},
    {"d.ref", &MetaDimensions::ref},
    {"d.recipe", &MetaDimensions::recipe},
    {"d.refFor", &MetaDimensions::refFor},
    {"d.mesh.relocation", &MetaDimensions::meshRelocation},
};

/// `d.clone` — the binding dim `capture_slot_clone` appends `cap$N` cells
/// onto (§6.3). Not part of the §6.1 read-surface bind set; reify
/// resolves it by name and mints it lazily outside marks, matching the
/// no-marks dimension rule.
constexpr std::string_view kCloneDimName = "d.clone";

/// `next_rand`'s LCG (§8.6): `a·s + c mod 2^64` — Vortex carries no XOR
/// or shift, so the prelude computes it by `DivMod`/`Mod` halves; the
/// engine arm is plain unsigned wraparound, which is defined.
constexpr std::uint64_t kLcgA = 6364136223846793005ULL;
constexpr std::uint64_t kLcgC = 1442695040888963407ULL;

/// `graphRoot`'s `+d.entry` chain bound: wrapper heads delegate to
/// their attach head (see `wrap`), so resolution hops until an op or
/// a dead end — the bound turns a malformed cycle into a miss rather
/// than a hang.
constexpr std::size_t kMaxEntryChain = 16;

template <std::ranges::input_range R> [[nodiscard]] auto collect(R &&range) {
  return std::ranges::to<std::vector>(std::forward<R>(range));
}

} // namespace

// ---------------------------------------------------------------------------
// Harness bind-set mint (V10/no-marks)
// ---------------------------------------------------------------------------

std::expected<MetaBindSet, std::string>
mintMetaBindSet(zigzag::ArenaManifold &arena, VortexCore &core,
                CellRef programRoot, CellRef agentCell) {
  if (arena.outstandingMarks() != 0)
    return std::unexpected(
        "meta bind mint refuses under live marks: arena-local dimension "
        "state is not trailed and would desync on release");
  if (!arena.contains(programRoot) || !arena.contains(agentCell))
    return std::unexpected("meta bind mint: roots must name arena cells");

  MetaBindSet binds;
  binds.programRoot = programRoot;
  binds.agentCell   = agentCell;
  for (const auto &[name, member] : kDimNames) {
    const auto dim = core.findOrMintDimension(name);
    if (dim == noCell)
      return std::unexpected(std::format(
          "meta bind mint: dimension '{}' could not be filed", name));
    binds.dims.*member = dim;
  }

  // §6.1: the opcode enum's integers arrive as bound data cells, not
  // prelude literals — same for the arity table and the impure-kind
  // set.
  const auto mintInt = [&](std::int64_t value) {
    return arena.makeScalarCell(value);
  };
  binds.validate.linkedCeiling =
      mintInt(static_cast<std::int64_t>(OpcodeKind::Linked));
  binds.validate.arityTable  = arena.makeCell();
  binds.validate.impureKinds = arena.makeCell();

  // Canonical wing arities, one (op, in, out) triple per kind on the
  // table cell's `+d.step` rank — the shapes the scalar compiler emits
  // (and the VM's operand arms consume); the sibling's fragment
  // emitter observes the same table shape.
  static constexpr std::int64_t kArity[][3] = {
      {static_cast<std::int64_t>(OpcodeKind::Add), 2, 1},
      {static_cast<std::int64_t>(OpcodeKind::Sub), 2, 1},
      {static_cast<std::int64_t>(OpcodeKind::Mul), 2, 1},
      {static_cast<std::int64_t>(OpcodeKind::Div), 2, 1},
      {static_cast<std::int64_t>(OpcodeKind::DivMod), 2, 2},
      {static_cast<std::int64_t>(OpcodeKind::Mod), 2, 1},
      {static_cast<std::int64_t>(OpcodeKind::Neg), 1, 1},
      {static_cast<std::int64_t>(OpcodeKind::Eq), 2, 1},
      {static_cast<std::int64_t>(OpcodeKind::Neq), 2, 1},
      {static_cast<std::int64_t>(OpcodeKind::Lt), 2, 1},
      {static_cast<std::int64_t>(OpcodeKind::Lte), 2, 1},
      {static_cast<std::int64_t>(OpcodeKind::Gt), 2, 1},
      {static_cast<std::int64_t>(OpcodeKind::Gte), 2, 1},
      {static_cast<std::int64_t>(OpcodeKind::And), 2, 1},
      {static_cast<std::int64_t>(OpcodeKind::Or), 2, 1},
      {static_cast<std::int64_t>(OpcodeKind::Not), 1, 1},
      {static_cast<std::int64_t>(OpcodeKind::Link), 4, 1},
      {static_cast<std::int64_t>(OpcodeKind::Break), 3, 1},
      {static_cast<std::int64_t>(OpcodeKind::Linked), 4, 1},
      {static_cast<std::int64_t>(OpcodeKind::New), 4, 1},
      {static_cast<std::int64_t>(OpcodeKind::Value), 4, 1},
      {static_cast<std::int64_t>(OpcodeKind::Splice), 4, 1},
      {static_cast<std::int64_t>(OpcodeKind::Clone), 1, 1},
      {static_cast<std::int64_t>(OpcodeKind::Jump), 1, 0},
      {static_cast<std::int64_t>(OpcodeKind::Branch), 2, 0},
      {static_cast<std::int64_t>(OpcodeKind::Call), 1, 0},
      {static_cast<std::int64_t>(OpcodeKind::Return), 0, 0},
      {static_cast<std::int64_t>(OpcodeKind::Bind), 2, 1},
      {static_cast<std::int64_t>(OpcodeKind::Resolve), 1, 1},
      {static_cast<std::int64_t>(OpcodeKind::Assert), 1, 0},
      {static_cast<std::int64_t>(OpcodeKind::Abs), 1, 1},
      {static_cast<std::int64_t>(OpcodeKind::Min), 2, 1},
      {static_cast<std::int64_t>(OpcodeKind::Max), 2, 1},
      {static_cast<std::int64_t>(OpcodeKind::Clamp), 3, 1},
      {static_cast<std::int64_t>(OpcodeKind::Trim), 1, 1},
      {static_cast<std::int64_t>(OpcodeKind::ToLower), 1, 1},
      {static_cast<std::int64_t>(OpcodeKind::ToUpper), 1, 1},
      {static_cast<std::int64_t>(OpcodeKind::Unify), 2, 1},
      {static_cast<std::int64_t>(OpcodeKind::IsVar), 1, 1},
      {static_cast<std::int64_t>(OpcodeKind::MakeVar), 0, 1},
      {static_cast<std::int64_t>(OpcodeKind::MakeTerm), 2, 1},
      {static_cast<std::int64_t>(OpcodeKind::Deref), 1, 1},
      {static_cast<std::int64_t>(OpcodeKind::Choice), 1, 0},
      {static_cast<std::int64_t>(OpcodeKind::Fail), 0, 0},
      {static_cast<std::int64_t>(OpcodeKind::Cut), 1, 0},
      {static_cast<std::int64_t>(OpcodeKind::ChoiceDepth), 0, 1},
      {static_cast<std::int64_t>(OpcodeKind::Halt), 0, 0},
      {static_cast<std::int64_t>(OpcodeKind::Nop), 0, 0},
  };
  const auto stepDim = core.dims().step;
  CellRef tableTail  = binds.validate.arityTable;
  for (const auto &triple : kArity)
    for (const auto field : triple) {
      const auto cell = mintInt(field);
      zigzag::expectWritten(arena.link(tableTail, stepDim, Posward, cell));
      tableTail = cell;
    }

  // The impure set: the structural write ops. `Linked` stays pure (it
  // is a query); `Clone` stays out — §11 refuses it as a reify palette
  // op, not as an impurity.
  static constexpr OpcodeKind kImpure[] = {
      OpcodeKind::Link,  OpcodeKind::Break,  OpcodeKind::New,
      OpcodeKind::Value, OpcodeKind::Splice,
  };
  CellRef impureTail = binds.validate.impureKinds;
  for (const auto kind : kImpure) {
    const auto cell = mintInt(static_cast<std::int64_t>(kind));
    zigzag::expectWritten(arena.link(impureTail, stepDim, Posward, cell));
    impureTail = cell;
  }
  return binds;
}

// ---------------------------------------------------------------------------
// Roots and the reflect family
// ---------------------------------------------------------------------------

std::optional<CellRef> MetaRuntime::moduleNamed(std::string_view path) const {
  for (const auto candidate :
       zigzag::rankAfter(arena(), binds_.programRoot, core_.dims().stdlib))
    if (arena().textOf(candidate) == path) return candidate;
  return std::nullopt;
}

std::optional<CellRef> MetaRuntime::symbolNamed(std::string_view owner,
                                                std::string_view name) const {
  // §6.7: a first-`:` qualifier is a module arm only when it resolves
  // as one; otherwise the whole string is the name verbatim. The
  // `owner` argument IS the qualifier when the caller split upstream —
  // so also try `owner`'s own first-colon arm.
  auto module = moduleNamed(owner);
  if (!module) {
    if (const auto colon = owner.find(':'); colon != std::string_view::npos) {
      module = moduleNamed(owner.substr(0, colon));
      if (module) name = owner.substr(colon + 1);
    }
  }
  if (!module) return std::nullopt;
  // The qualified `module:name` — the spelling unlearn tombstones — is what
  // the suppression check keys on, so a resurrected learn_method symbol stays
  // unresolvable even though its d.vars link never persisted as removed.
  const auto moduleName = arena().textOf(*module);
  for (const auto symbol :
       zigzag::rankAfter(arena(), *module, core_.dims().vars))
    if (arena().textOf(symbol) == name &&
        !unlearned(std::format("{}:{}", moduleName, name)))
      return symbol;
  return std::nullopt;
}

std::vector<CellRef> MetaRuntime::clausesOf(CellRef cell) const {
  std::vector<CellRef> out;
  for (const auto clause :
       zigzag::rankAfter(arena(), cell, core_.dims().clause))
    // A resurrected-but-tombstoned clause stays unresolvable: promote commits
    // the unlink only for lineages written under commitShadowed (Clears) —
    // older stores carry the shadow-drop without the clear, so rank
    // membership alone cannot name it dead; the `unlearn:` record can.
    if (!unlearned(labelOf(clause).value_or(""))) out.push_back(clause);
  return out;
}

std::optional<std::string> MetaRuntime::labelOf(CellRef cell) const {
  const auto label = arena().linked(cell, core_.dims().name, Posward);
  if (!label || !arena().contains(*label)) return std::nullopt;
  return arena().textOf(*label);
}

std::optional<CellRef>
MetaRuntime::planNamed(std::string_view qualified) const {
  // First-`:` split, module arm conditional on the qualifier resolving
  // as a module on the program root; the match itself is the clause's
  // `+d.name` label verbatim against the *full* input spelling.
  std::optional<CellRef> module;
  if (const auto colon = qualified.find(':'); colon != std::string_view::npos)
    module = moduleNamed(qualified.substr(0, colon));

  const auto match = [&](CellRef host) -> std::optional<CellRef> {
    for (const auto symbol :
         zigzag::rankAfter(arena(), host, core_.dims().vars))
      for (const auto clause : clausesOf(symbol))
        if (labelOf(clause).value_or("") == qualified) return clause;
    return std::nullopt;
  };
  if (module) return match(*module);
  for (const auto candidate :
       zigzag::rankAfter(arena(), binds_.programRoot, core_.dims().stdlib))
    if (const auto hit = match(candidate)) return hit;
  return std::nullopt;
}

std::optional<CellRef> MetaRuntime::codeNamed(std::string_view label) const {
  // The agent's registered `d.code` heads first — no first-`:` split;
  // `demo:Forager.f` is an opaque label, never `code_named("f")` on a
  // `demo` module arm.
  for (const auto head :
       zigzag::rankAfter(arena(), binds_.agentCell, binds_.dims.code))
    if (labelOf(head).value_or("") == label) return head;

  // Fallthrough: the program-image label walk — the §6.6 program
  // vocabulary outbound from the program root. A head-shaped cell
  // (carries `+d.entry`) whose `+d.name` text is the label wins.
  const auto &dims          = core_.dims();
  std::vector<DimRef> vocab = {
      dims.stdlib,       dims.vars,        dims.clause,       dims.values,
      binds_.dims.entry, dims.grab,        dims.spin,         dims.step,
      dims.contract,     binds_.dims.refs, binds_.dims.recipe};
  if (const auto extends = arena().dimensionNamed("d.extends"))
    vocab.push_back(*extends);
  std::set<CellRef> seen{binds_.programRoot};
  std::deque<CellRef> frontier{binds_.programRoot};
  std::size_t steps = 0;
  const auto bound  = arena().traversalBound();
  while (!frontier.empty() && steps++ < bound) {
    const auto cell = frontier.front();
    frontier.pop_front();
    if (arena().linked(cell, binds_.dims.entry, Posward).has_value() &&
        labelOf(cell).value_or("") == label)
      return cell;
    for (const auto dim : vocab)
      for (const auto next : zigzag::rankAfter(arena(), cell, dim))
        if (seen.insert(next).second) frontier.push_back(next);
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Memoized test/body wrappers (§6.1's memoization contract)
// ---------------------------------------------------------------------------

bool MetaRuntime::isOpCell(CellRef cell) const {
  const auto ordinal = arena().asInt64(cell);
  if (!ordinal) return false;
  const auto ceiling =
      arena()
          .asInt64(binds_.validate.linkedCeiling)
          .value_or(static_cast<std::int64_t>(OpcodeKind::Linked));
  return *ordinal >= 0 && *ordinal <= ceiling;
}

std::optional<CellRef> MetaRuntime::graphRoot(CellRef attach) const {
  // Op first, head second (§6.1's discriminator): an op cell is the
  // graph root directly; a head carries `+d.entry` toward it. The hop
  // is transitive rather than once — a memoized wrapper head cannot
  // `+d.entry →` the same root its attach does (`-d.entry` is
  // single-valued per cell, so the second link would evict the
  // attach's), so it delegates to the attach head and the chain
  // resolves onward to the op. Bounded against link cycles.
  auto current = attach;
  for (std::size_t hops = 0; hops <= kMaxEntryChain; ++hops) {
    if (isOpCell(current)) return current;
    const auto next = arena().linked(current, binds_.dims.entry, Posward);
    if (!next || !arena().contains(*next)) return std::nullopt;
    current = *next;
  }
  return std::nullopt;
}

std::optional<CellRef>
MetaRuntime::varsRecord(CellRef owner, std::string_view key, bool mint) const {
  return vagent::varsRecord(arena(), core_, owner, key, mint);
}

void MetaRuntime::markUnlearned(std::string_view label) const {
  if (label.empty()) return;
  // varsRecord dedupes on the key, so a re-tombstoned label is a no-op and
  // a single `unlearn:patch:<n>` retires both the patch and the plan/handler
  // clause that shares its label.
  (void)varsRecord(binds_.agentCell, std::format("unlearn:{}", label),
                   /*mint=*/true);
}

bool MetaRuntime::unlearned(std::string_view label) const {
  return !label.empty() &&
         varsRecord(binds_.agentCell, std::format("unlearn:{}", label),
                    /*mint=*/false)
             .has_value();
}

std::expected<std::int64_t, std::string> MetaRuntime::nextSerial() const {
  auto &arena = core_.arena();
  const auto record =
      varsRecord(binds_.agentCell, kMetaSerialKey, /*mint=*/true);
  if (!record)
    return std::unexpected("meta serial: could not mint the d.vars counter");
  const auto value = arena.linked(*record, core_.dims().values, Posward);
  if (!value || !arena.contains(*value)) {
    // The cell holds the *next* serial: mint 1 and hand out 0, or the
    // first two callers would both read 0.
    zigzag::expectWritten(arena.link(*record, core_.dims().values, Posward,
                                     arena.makeScalarCell(std::int64_t{1})));
    return 0;
  }
  const auto current = arena.asInt64(*value);
  if (!current)
    return std::unexpected("meta serial: counter cell is not an int64");
  // Mint-and-repoint, never an in-place value write: promote restates cell
  // content only under ShadowCommit::Values — a stage no consumer opts into
  // (delta-completeness §4.2) — so a setValueBits here rewinds to the folded
  // value at the next genesis and every verb would re-mint patch:<n>
  // serials that already shipped.
  zigzag::expectWritten(arena.link(*record, core_.dims().values, Posward,
                                   arena.makeScalarCell(*current + 1)));
  return *current;
}

std::optional<CellRef> MetaRuntime::wrap(CellRef clause, DimRef attachDim,
                                         std::string_view memoKind) {
  auto &arena       = core_.arena();
  const auto attach = arena.linked(clause, attachDim, Posward);
  if (!attach || !arena.contains(*attach)) return std::nullopt;
  const auto root = graphRoot(*attach);
  if (!root) return std::nullopt;
  const auto key = labelOf(*attach);

  // Clause-side memo records: `clause +d.code → first`, `+d.step`
  // chain; each `{+d.values → wrapper, +d.name → kind label, +d.grab →
  // scalar key cell}`. Records of a different kind, or none, stay put.
  if (key) {
    std::optional<CellRef> previous;
    auto record = arena.linked(clause, binds_.dims.code, Posward);
    while (record && arena.contains(*record)) {
      const auto next     = arena.linked(*record, core_.dims().step, Posward);
      const auto kindCell = arena.linked(*record, core_.dims().name, Posward);
      const auto kind     = kindCell ? arena.textOf(*kindCell) : std::string{};
      const auto keyCell  = arena.linked(*record, core_.dims().grab, Posward);
      const auto recKey =
          keyCell ? std::optional{arena.textOf(*keyCell)} : std::nullopt;
      const bool hit = kind == memoKind && recKey && *recKey == *key;
      if (hit) {
        if (const auto wrapper =
                arena.linked(*record, core_.dims().values, Posward);
            wrapper && arena.contains(*wrapper))
          return *wrapper;
        // The wrapper died under a mark release — a stale record the
        // splice below removes before the fresh wrap.
      }
      if (kind == memoKind && !hit) {
        // Wrong-key or broken memo record: the attach's label moved or
        // the record is malformed. Unlink rather than leave a key that
        // can never hit.
        zigzag::expectWritten(
            arena.unlink(*record, core_.dims().step, Posward));
        const auto leadDim  = previous ? core_.dims().step : binds_.dims.code;
        const auto leadFrom = previous.value_or(clause);
        if (next)
          zigzag::expectWritten(arena.link(leadFrom, leadDim, Posward, *next));
        else
          zigzag::expectWritten(arena.unlink(leadFrom, leadDim, Posward));
      } else {
        previous = *record;
      }
      record = next;
    }
  }

  // Fresh wrap: a session-side head — `+d.entry` to the *attach*, not
  // the root. `-d.entry` is single-valued per cell: pointing the
  // wrapper at the attach's root would evict the attach's own entry
  // edge, so the wrapper delegates one head-hop upstream and
  // `graphRoot` walks the chain to the op. Never registered on the
  // agent's durable `d.code` rank. Its descriptor purity/closed flags
  // are unknown by construction, so a pure-required graft must refuse
  // it (§6.1).
  const auto wrapper = arena.makeCell();
  zigzag::expectWritten(
      arena.link(wrapper, binds_.dims.entry, Posward, *attach));
  if (key) {
    const auto record = arena.makeCell();
    zigzag::expectWritten(
        arena.link(record, core_.dims().values, Posward, wrapper));
    zigzag::expectWritten(arena.link(record, core_.dims().name, Posward,
                                     arena.makeCell(memoKind)));
    // The key rides as a *fresh scalar cell*, not the label cell
    // itself: two attach cells sharing a label compare equal by
    // content, which is the memo hit rule.
    zigzag::expectWritten(
        arena.link(record, core_.dims().grab, Posward, arena.makeCell(*key)));
    // `clause +d.code → first record`, records chained `+d.step`.
    if (const auto first = arena.linked(clause, binds_.dims.code, Posward)) {
      auto tail = *first;
      for (auto next = arena.linked(tail, core_.dims().step, Posward);
           next && arena.contains(*next);
           next = arena.linked(tail, core_.dims().step, Posward))
        tail = *next;
      zigzag::expectWritten(
          arena.link(tail, core_.dims().step, Posward, record));
    } else {
      zigzag::expectWritten(
          arena.link(clause, binds_.dims.code, Posward, record));
    }
  }
  return wrapper;
}

std::optional<CellRef> MetaRuntime::testOf(CellRef clause) {
  return wrap(clause, core_.dims().grab, "test");
}

std::optional<CellRef> MetaRuntime::bodyOf(CellRef clause) {
  return wrap(clause, core_.dims().spin, "body");
}

// ---------------------------------------------------------------------------
// Reads
// ---------------------------------------------------------------------------

MetaValidateReport MetaRuntime::validate(CellRef graphRootCell,
                                         MetaValidateOptions options) const {
  auto &arena      = core_.arena();
  const auto &dims = core_.dims();

  // A head operand resolves through the `+d.entry` chain — transitive
  // because a wrapper head delegates to its attach (see `graphRoot`);
  // an op operand stands.
  const auto resolved = graphRoot(graphRootCell);
  const CellRef root  = resolved.value_or(graphRootCell);

  const auto ceiling =
      arena.asInt64(binds_.validate.linkedCeiling)
          .value_or(static_cast<std::int64_t>(OpcodeKind::Linked));
  if (!isOpCell(root)) return {false, "validate: root is not an op cell"};

  // Decode the bound arity table: `(op, in, out)` triples on `+d.step`.
  std::map<std::int64_t, std::pair<std::size_t, std::size_t>> arity;
  {
    const auto fields = collect(
        zigzag::rankAfter(arena, binds_.validate.arityTable, dims.step));
    if (fields.size() % 3 != 0)
      return {false, "validate: arity table is not a triple chain"};
    for (std::size_t i = 0; i + 2 < fields.size(); i += 3) {
      const auto op  = arena.asInt64(fields[i]);
      const auto in  = arena.asInt64(fields[i + 1]);
      const auto out = arena.asInt64(fields[i + 2]);
      if (!op || !in || !out || *in < 0 || *out < 0)
        return {false, "validate: arity table field is not an int64 triple"};
      arity[*op] = {static_cast<std::size_t>(*in),
                    static_cast<std::size_t>(*out)};
    }
  }
  std::set<std::int64_t> impure;
  for (const auto cell :
       zigzag::rankAfter(arena, binds_.validate.impureKinds, dims.step))
    if (const auto kind = arena.asInt64(cell)) impure.insert(*kind);

  const auto serial = nextSerial();
  if (!serial) return {false, serial.error()};
  const auto tagged = [&](CellRef cell) {
    for (const auto tag : zigzag::rankAfter(arena, cell, binds_.dims.validate))
      if (arena.asInt64(tag) == *serial) return true;
    return false;
  };
  const auto tag = [&](CellRef cell) {
    zigzag::expectWritten(arena.link(cell, binds_.dims.validate, Posward,
                                     arena.makeScalarCell(*serial)));
  };

  // Op-graph reachability, bounded. Wing slots are walked for target
  // referents (control ops carry op refs as int64 slots); slots are
  // data cells and never become visited ops themselves.
  std::set<CellRef> seen{root};
  std::deque<CellRef> frontier{root};
  std::size_t visited = 0;
  const auto bound    = arena.traversalBound();
  while (!frontier.empty()) {
    const auto op = frontier.front();
    frontier.pop_front();
    if (visited++ > bound)
      return {false, "validate: op graph exceeds the arena bound"};
    if (tagged(op)) continue;
    tag(op);

    const auto ordinal = arena.asInt64(op);
    if (!ordinal || *ordinal < 0 || *ordinal > ceiling)
      return {false, "validate: op int64 outside [0, Linked]"};
    const auto shape = arity.find(*ordinal);
    if (shape == arity.end())
      return {false, "validate: op kind absent from the arity table"};
    const auto inCells  = core_.inputsOf(op);
    const auto outCells = core_.outputsOf(op);
    if (inCells.size() != shape->second.first ||
        outCells.size() != shape->second.second)
      return {false, "validate: wing arity differs from the table"};
    if (options.runReachable &&
        static_cast<OpcodeKind>(*ordinal) == OpcodeKind::New &&
        !outCells.empty())
      return {false, "validate: bound-output New in a run-reachable graph"};
    if (options.expectPure && impure.contains(*ordinal))
      return {false, "validate: impure op in a pure-flagged fragment"};

    const auto bad = [&](std::string_view why) {
      return MetaValidateReport{false, std::string{why}};
    };
    const auto enqueue =
        [&](CellRef target) -> std::optional<MetaValidateReport> {
      if (!arena.contains(target) || !isOpCell(target))
        return bad("validate: control target does not resolve to an op");
      if (seen.insert(target).second) frontier.push_back(target);
      return std::nullopt;
    };

    // `+d.spin` continuation; a run-reachable chain must tail Return.
    if (const auto next = arena.linked(op, dims.spin, Posward)) {
      if (const auto fail = enqueue(*next)) return *fail;
    } else if (options.runReachable &&
               static_cast<OpcodeKind>(*ordinal) != OpcodeKind::Return) {
      return bad("validate: run-reachable op chain ends without "
                 "Return");
    }

    // Control-op targets are the op's wing operands directly:
    // `inputsOf`/`outputsOf` already operand-resolve each slot (an
    // Int64 naming a contained cell resolves to it), so re-reading the
    // operand's own content would chase the *op ordinal*, not a cell.
    const auto target = [&](std::size_t index) -> std::optional<CellRef> {
      if (index >= inCells.size()) return std::nullopt;
      return inCells[index];
    };
    switch (static_cast<OpcodeKind>(*ordinal)) {
    case OpcodeKind::Jump:
    case OpcodeKind::Call:
    case OpcodeKind::Choice:
      if (const auto t = target(0)) {
        if (const auto fail = enqueue(*t)) return *fail;
      } else {
        return bad("validate: control op is missing its target slot");
      }
      break;
    case OpcodeKind::Branch:
      if (const auto t = target(1)) {
        if (const auto fail = enqueue(*t)) return *fail;
      } else {
        return bad("validate: Branch is missing its alternate target");
      }
      break;
    default:
      break;
    }
  }
  return {true, {}};
}

std::optional<double> MetaRuntime::weightOf(CellRef cell) const {
  // A junction (synapse edge) reads its own `d.weight` scalar.
  if (const auto weight = arena().linked(cell, binds_.dims.weight, Posward)) {
    if (const auto value = arena().asDouble(*weight)) return *value;
    if (const auto bits = arena().asInt64(*weight))
      return static_cast<double>(*bits);
  }
  // A labelled cell's weight rides the agent's `d.vars` record keyed
  // by the label — the state-side channel §6.1 names.
  if (const auto label = labelOf(cell)) {
    const auto record = varsRecord(binds_.agentCell, *label, /*mint=*/false);
    if (record)
      if (const auto value =
              arena().linked(*record, core_.dims().values, Posward))
        if (const auto number = arena().asDouble(*value)) return *number;
  }
  return std::nullopt;
}

std::optional<CellValue> MetaRuntime::valueOf(CellRef cell) const {
  return core_.render(cell);
}

std::int64_t MetaRuntime::nextRand(CellRef seed) const {
  const auto s = static_cast<std::uint64_t>(arena().asInt64(seed).value_or(0));
  return std::bit_cast<std::int64_t>(s * kLcgA + kLcgC);
}

// ---------------------------------------------------------------------------
// run
// ---------------------------------------------------------------------------

std::optional<CellRef>
MetaRuntime::descriptorField(CellRef descriptor, std::string_view key) const {
  for (const auto record :
       zigzag::rankAfter(arena(), descriptor, core_.dims().vars))
    if (arena().textOf(record) == key) return record;
  return std::nullopt;
}

std::vector<CellRef> MetaRuntime::parameterCells(CellRef descriptor) const {
  // Ordered parameter signature: the descriptor's `+d.grab` chain via
  // `+d.step`, each parameter `+d.name`-labelled and `+d.values`-typed.
  std::vector<CellRef> result;
  for (auto cell = arena().linked(descriptor, core_.dims().grab, Posward);
       cell && arena().contains(*cell);
       cell = arena().linked(*cell, core_.dims().step, Posward))
    result.push_back(*cell);
  return result;
}

MetaRunOutcome MetaRuntime::runBox(CellRef box,
                                   const std::vector<CellValue> &args,
                                   std::optional<CellRef> cursor,
                                   std::size_t maxCycles) {
  const auto referent = arena().asInt64(box);
  if (!referent)
    return {MetaRunOutcome::Refusal::NotAHead, false, std::nullopt,
            "run operand is not a ref box"};
  return runFragment(static_cast<CellRef>(*referent), args, cursor, maxCycles);
}

MetaRunOutcome MetaRuntime::runFragment(CellRef head,
                                        const std::vector<CellValue> &args,
                                        std::optional<CellRef> cursor,
                                        std::size_t maxCycles) {
  auto &arena      = core_.arena();
  const auto &dims = core_.dims();

  // The headless-run arm: `run` takes a head-valued fragment. An
  // op-valued symbol (a `def`'s `+d.values` entry op) is *not* a head —
  // that is the §6.1 refusal, not a silent treat-as-head. The
  // `+d.entry` edge is the head discriminator; the cursor entry is the
  // chain's resolved op, since a memoized wrapper's edge lands on its
  // attach head, not the op.
  if (const auto direct = arena.linked(head, binds_.dims.entry, Posward);
      !direct || !arena.contains(*direct) || isOpCell(head))
    return {MetaRunOutcome::Refusal::NotAHead, false, std::nullopt,
            "run operand carries no +d.entry — not a fragment head"};
  const auto entry = graphRoot(head);
  if (!entry)
    return {MetaRunOutcome::Refusal::NotAHead, false, std::nullopt,
            "run operand's +d.entry chain reaches no opcode"};
  const auto descriptor = arena.linked(head, dims.values, Posward);
  if (!descriptor || !arena.contains(*descriptor))
    return {MetaRunOutcome::Refusal::NoDescriptor, false, std::nullopt,
            "fragment head carries no +d.values descriptor"};

  const auto resultRecord   = descriptorField(*descriptor, "result");
  const auto producedRecord = descriptorField(*descriptor, "produced");
  const auto resultCell =
      resultRecord ? arena.linked(*resultRecord, dims.values, Posward)
                   : std::nullopt;
  const auto producedCell =
      producedRecord ? arena.linked(*producedRecord, dims.values, Posward)
                     : std::nullopt;
  if (!resultCell || !producedCell)
    return {MetaRunOutcome::Refusal::NoDescriptor, false, std::nullopt,
            "descriptor lacks result/produced cells"};

  const auto params = parameterCells(*descriptor);
  if (const auto arityRecord = descriptorField(*descriptor, "arity"))
    if (const auto arityCell = arena.linked(*arityRecord, dims.values, Posward))
      if (const auto declared = arena.asInt64(*arityCell);
          declared && *declared != static_cast<std::int64_t>(params.size()))
        return {MetaRunOutcome::Refusal::NoDescriptor, false, std::nullopt,
                "descriptor arity disagrees with its parameter "
                "chain"};
  if (params.size() != args.size())
    return {MetaRunOutcome::Refusal::ArityMismatch, false, std::nullopt,
            std::format("run args ({}) differ from fragment arity ({})",
                        args.size(), params.size())};

  // Spill before the frame: `result`/`produced` are the head's
  // descriptor state, and a rerun must not leak a previous outcome.
  const auto spillResult   = core_.render(*resultCell);
  const auto spillProduced = core_.render(*producedCell);

  const auto cur = cursor.value_or(vm_.spawnCursor(*entry));
  // The pre-run rank heads: a faulted run never reaches the Return that
  // would pop the frame, and the bound args outlive their use — both are
  // restored below so a caller-supplied cursor survives `run` unchanged.
  const auto priorStack = arena.linked(cur, dims.stack, Posward);
  const auto priorVars  = arena.linked(cur, dims.vars, Posward);
  // The `vortex_vm.cpp:854-893` frame shape: an Int64 cell holding the
  // successor op, pushed on the cursor's `+d.stack`. For a kernel-side
  // run the successor is a fresh Halt — `Return` pops the frame and
  // lands there, ending `run` cleanly.
  const auto landing = vm_.mintOpcode(OpcodeKind::Halt);
  const auto frame   = arena.makeScalarCell(static_cast<std::int64_t>(landing));
  if (const auto top = arena.linked(cur, dims.stack, Posward))
    zigzag::expectWritten(arena.link(frame, dims.stack, Posward, *top));
  zigzag::expectWritten(arena.link(cur, dims.stack, Posward, frame));

  // Arguments bind name-keyed on the cursor's `d.vars` rank — where #BIND
  // writes and #RESOLVE reads (vortex_vm.cpp:896-943), not on the frame
  // cell. Each variable prepends between the cursor and the rank's head,
  // so the last argument sits innermost like a #BIND sequence's.
  for (std::size_t i = 0; i < args.size(); ++i) {
    const auto paramName = labelOf(params[i]).value_or(std::format("arg{}", i));
    const auto variable  = arena.makeCell(paramName);
    const auto value     = std::visit(
        [&](const auto &v) -> CellRef {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, std::string>)
            return arena.makeCell(v);
          else
            return arena.makeScalarCell(v);
        },
        args[i]);
    zigzag::expectWritten(arena.link(variable, dims.values, Posward, value));
    if (const auto head = arena.linked(cur, dims.vars, Posward))
      zigzag::expectWritten(arena.link(variable, dims.vars, Posward, *head));
    zigzag::expectWritten(arena.link(cur, dims.vars, Posward, variable));
  }

  // `run` is an observation — a caller-supplied cursor must survive it
  // unchanged. The bound args always sit on `+d.vars` afterward, and a
  // faulted run never reaches the Return that pops the frame, so both ranks
  // are repointed at their pre-run heads rather than relying on the unwind.
  const auto restoreRank = [&](DimRef dim, const std::optional<CellRef> &top) {
    if (top)
      zigzag::expectWritten(arena.link(cur, dim, Posward, *top));
    else
      zigzag::expectWritten(arena.unlink(cur, dim, Posward));
  };
  const auto restore = [&] {
    restoreRank(dims.stack, priorStack);
    restoreRank(dims.vars, priorVars);
  };

  vm_.setCursorOpcode(cur, *entry);
  zigzag::vortex::ExecutionResult outcome;
  try {
    outcome = vm_.run(cur, maxCycles);
  } catch (...) {
    // A thrown fault (budget exhaustion, an arena invariant) must not
    // strand the pushed frame and bound args on a caller's cursor.
    restore();
    throw;
  }
  restore();

  const auto produced = arena.asBool(*producedCell).value_or(false);
  MetaRunOutcome report;
  report.produced = produced;
  if (!outcome.success) {
    report.refusal = MetaRunOutcome::Refusal::VmFault;
    report.detail  = outcome.errorMessage;
  } else if (produced) {
    report.result = core_.render(*resultCell);
  }

  // Restore the spill so the descriptor is unchanged across `run`.
  if (spillResult) core_.writeValue(*resultCell, 0, std::nullopt, *spillResult);
  if (spillProduced)
    core_.writeValue(*producedCell, 0, std::nullopt, *spillProduced);
  return report;
}

// ---------------------------------------------------------------------------
// reify — the +d.recipe replay
// ---------------------------------------------------------------------------

std::expected<DimRef, std::string>
MetaRuntime::resolveDimension(std::string_view name) const {
  auto &arena = core_.arena();
  // Filed or bound names resolve under marks; a *mint* does not —
  // `arenaDims_` is untrailed (the P17-class desync §6.5 names).
  // Two filing tables exist: `findOrMintDimension` files on the core's
  // `d.dims` rank (where the apparatus dims and every rank-filed name
  // live), while `arenaDims_` names only `ensureDimension`-minted and
  // base-filed dims. Both must be consulted or a rank-filed name
  // re-mints as a second, disconnected DimRef. The mint arm is
  // `ensureDimension` (the spec's name): the arena-local table, which
  // is exactly why a mint under live marks is refused.
  if (const auto dim = core_.findDimension(name)) return *dim;
  if (const auto dim = arena.dimensionNamed(name)) return *dim;
  if (arena.outstandingMarks() != 0)
    return std::unexpected(
        std::format("reify: dimension '{}' is not filed and cannot be minted "
                    "under live marks",
                    name));
  return arena.ensureDimension(name);
}

std::expected<CellRef, std::string> MetaRuntime::reify(CellRef head) {
  auto &arena      = core_.arena();
  const auto &dims = core_.dims();

  const auto entry = arena.linked(head, binds_.dims.entry, Posward);
  if (!entry || !arena.contains(*entry))
    return std::unexpected(
        "reify operand carries no +d.entry — not a fragment head");
  const auto recipe = arena.linked(head, binds_.dims.recipe, Posward);
  if (!recipe || !arena.contains(*recipe))
    return std::unexpected("fragment head carries no +d.recipe chain");

  // The recipe chain in order: head `+d.recipe → first`, `+d.step`
  // links.
  std::vector<CellRef> members;
  for (auto cell = recipe; cell && arena.contains(*cell);
       cell      = arena.linked(*cell, dims.step, Posward))
    members.push_back(*cell);

  // Pass 1 — classify. A member bearing `+d.refFor` is a ref-capture
  // record; a member bearing `+d.name` is a tuple's dim-name field;
  // anything else is a data member (a literal-born field or a tuple
  // endpoint/direction field).
  enum class Kind { Data, DimAnchor, RefCapture };
  std::vector<Kind> kinds(members.size(), Kind::Data);
  std::set<std::size_t> consumed;
  for (std::size_t i = 0; i < members.size(); ++i) {
    const auto m = members[i];
    if (arena.linked(m, binds_.dims.refFor, Posward)) {
      kinds[i] = Kind::RefCapture;
      continue;
    }
    if (arena.linked(m, dims.name, Posward)) {
      kinds[i] = Kind::DimAnchor;
      if (i < 1 || i + 2 >= members.size())
        return std::unexpected("reify: dim-name member has no (from, dir, to) "
                               "neighbours");
      consumed.insert({i - 1, i + 1, i + 2});
    }
  }

  const auto serial = nextSerial();
  if (!serial) return std::unexpected(serial.error());
  const auto sourceLabel = labelOf(head).value_or("");

  // old → new cell map; the head's twin mints first so recipes that
  // reference the head itself resolve.
  std::map<CellRef, CellRef> fresh;
  const auto newHead = arena.makeCell();
  fresh.emplace(head, newHead);
  // The copy is a head like its source: `+d.entry` delegates to the
  // source head rather than duplicating its entry edge — `-d.entry` is
  // single-valued, so `newHead +d.entry → entry` would evict `head`'s
  // own. `graphRoot` walks the chain to the op.
  zigzag::expectWritten(arena.link(newHead, binds_.dims.entry, Posward, head));
  const auto mapCell = [&](CellRef source) {
    if (const auto found = fresh.find(source); found != fresh.end())
      return found->second;
    const auto twin = arena.makeCell();
    fresh.emplace(source, twin);
    return twin;
  };
  const auto writeInt = [&](CellRef cell, std::int64_t value) {
    vagent::writeInt(arena, cell, value);
  };
  const auto mapped = [&](CellRef source) {
    const auto found = fresh.find(source);
    return found != fresh.end() ? found->second : source;
  };
  // A recorded position may arrive OpHandle-kind: a checkpoint-re-kinded
  // donor's marked cells keep `*handleTarget` naming the promoted
  // position of the cell the int64 spelled in-session (spec :2422-2428 —
  // the donor stays re-kinded through (d), (e) rehydrating last), while
  // palette-minted copy cells and in-session recipes stay Int64. Reading
  // either kind through one arm keeps the replay kind-guarded the same
  // way (e)'s ref arm is.
  const auto posOf = [&](CellRef cell) -> std::optional<std::int64_t> {
    if (arena.valueKindOf(cell) == xanadu::ValueKind::OpHandle)
      if (const auto target = arena.handleTarget(cell))
        return static_cast<std::int64_t>(*target);
    return arena.asInt64(cell);
  };

  // Pass 2 — replay: tuples relink through the map, literals copy
  // content, ref-capture slots rebind after every cell exists.
  for (std::size_t i = 0; i < members.size(); ++i) {
    const auto m = members[i];
    if (kinds[i] == Kind::DimAnchor) {
      const auto fromPos = posOf(members[i - 1]);
      const auto dirPos  = posOf(members[i + 1]);
      const auto toPos   = posOf(members[i + 2]);
      if (!fromPos || !dirPos || !toPos)
        return std::unexpected(
            "reify: link tuple member is not an int64 position");
      const auto nameCell = arena.linked(m, dims.name, Posward);
      const auto dim      = resolveDimension(arena.textOf(*nameCell));
      if (!dim) return std::unexpected(dim.error());
      const auto from = mapCell(static_cast<CellRef>(*fromPos));
      const auto to   = mapCell(static_cast<CellRef>(*toPos));
      zigzag::expectWritten(
          arena.link(from, *dim, *dirPos < 0 ? Negward : Posward, to));
      continue;
    }
    if (kinds[i] == Kind::RefCapture || consumed.contains(i)) continue;
    const auto pos = posOf(m);
    if (!pos)
      return std::unexpected(
          "reify: unclassified recipe member is not a position");
    const auto twin = mapCell(static_cast<CellRef>(*pos));
    if (const auto content = core_.render(static_cast<CellRef>(*pos)))
      core_.writeValue(twin, 0, std::nullopt, *content);
  }

  // Ref-capture records: `+d.refFor → the source slot`. Internal
  // referents rebind through the fresh map; external verbatim; a
  // `+d.name`-bearing member re-resolves a dimension *name*.
  std::vector<CellRef> captureSlots;
  for (std::size_t i = 0; i < members.size(); ++i) {
    if (kinds[i] != Kind::RefCapture) continue;
    const auto m    = members[i];
    const auto slot = arena.linked(m, binds_.dims.refFor, Posward);
    if (!slot || !arena.contains(*slot))
      return std::unexpected(
          "reify: ref-capture member lacks a +d.refFor slot");
    const auto newSlot = mapCell(*slot);
    if (const auto nameCell = arena.linked(m, dims.name, Posward)) {
      const auto dim = resolveDimension(arena.textOf(*nameCell));
      if (!dim) return std::unexpected(dim.error());
      writeInt(newSlot, static_cast<std::int64_t>(*dim));
    } else {
      const auto referent = posOf(*slot);
      if (!referent)
        return std::unexpected("reify: ref-capture slot has no int64 referent");
      writeInt(newSlot, static_cast<std::int64_t>(
                            mapped(static_cast<CellRef>(*referent))));
    }
    // `cap$N` arm: the `+d.values` hint names the binding whose
    // `d.clone` rank takes this slot — `New` + `Link`, never `Clone`.
    if (const auto binding = arena.linked(m, dims.values, Posward)) {
      const auto cloneDim = resolveDimension(kCloneDimName);
      if (!cloneDim) return std::unexpected(cloneDim.error());
      zigzag::expectWritten(
          arena.link(zigzag::rankTail(arena, *binding, *cloneDim), *cloneDim,
                     Posward, newSlot));
    }
    captureSlots.push_back(newSlot);
  }

  // `+d.refs` regeneration on the copy — the same chain shape the memo
  // records and the recipe use: head `+d.refs → first`, `+d.step`
  // thereafter. Name-records re-link to fresh verbatim name cells;
  // ref-records re-point through the map and the re-pointed cell takes
  // a fresh `d.mesh.relocation → "cell"` marker.
  std::optional<CellRef> refsTail;
  const auto appendRef = [&](CellRef record) {
    if (!refsTail)
      zigzag::expectWritten(
          arena.link(newHead, binds_.dims.refs, Posward, record));
    else
      zigzag::expectWritten(arena.link(*refsTail, dims.step, Posward, record));
    refsTail = record;
  };
  const auto refChain = [](zigzag::ArenaManifold &a, CellRef host,
                           DimRef refDim, DimRef stepDim) {
    std::vector<CellRef> out;
    for (auto cell = a.linked(host, refDim, Posward); cell && a.contains(*cell);
         cell      = a.linked(*cell, stepDim, Posward))
      out.push_back(*cell);
    return out;
  };
  for (const auto rec : refChain(arena, head, binds_.dims.refs, dims.step)) {
    const auto target = arena.linked(rec, binds_.dims.ref, Posward);
    const auto name   = arena.linked(rec, dims.name, Posward);
    if (!target)
      return std::unexpected("reify: +d.refs record has no +d.ref cell");
    // The referent: an internal cell re-points to its member twin. An
    // *external* one cannot be re-marked verbatim — `+d.ref` is a
    // single-link dim, so the copy's edge would evict the original
    // record's — the copy marks a fresh ref-box carrying the same
    // content instead. A name-record's slot is bookkeeping; a
    // ref-record's still names the same position.
    const auto markedCopy = [&] {
      if (const auto found = fresh.find(*target); found != fresh.end())
        return found->second;
      const auto refbox = arena.makeCell();
      if (const auto content = core_.render(*target))
        core_.writeValue(refbox, 0, std::nullopt, *content);
      return refbox;
    }();
    const auto copy = arena.makeCell();
    zigzag::expectWritten(
        arena.link(copy, binds_.dims.ref, Posward, markedCopy));
    if (name) {
      zigzag::expectWritten(arena.link(copy, dims.name, Posward,
                                       arena.makeCell(arena.textOf(*name))));
      if (const auto role = arena.linked(rec, dims.grab, Posward))
        zigzag::expectWritten(arena.link(copy, dims.grab, Posward,
                                         arena.makeCell(arena.textOf(*role))));
    } else {
      zigzag::expectWritten(arena.link(markedCopy, binds_.dims.meshRelocation,
                                       Posward, arena.makeCell("cell")));
    }
    appendRef(copy);
  }
  for (const auto slot : captureSlots) {
    const auto copy = arena.makeCell();
    zigzag::expectWritten(arena.link(copy, binds_.dims.ref, Posward, slot));
    zigzag::expectWritten(arena.link(slot, binds_.dims.meshRelocation, Posward,
                                     arena.makeCell("cell")));
    appendRef(copy);
  }

  // `+d.recipe` regeneration — a self-describing copy, same taxonomy,
  // positions rewritten through the fresh map. Each member also joins
  // the copy's `+d.refs` inventory with a relocation marker.
  std::optional<CellRef> recipeTail;
  for (std::size_t i = 0; i < members.size(); ++i) {
    const auto m = members[i];
    const auto c = arena.makeCell();
    switch (kinds[i]) {
    case Kind::DimAnchor: {
      const auto nameCell = arena.linked(m, dims.name, Posward);
      const auto dim      = resolveDimension(arena.textOf(*nameCell));
      if (!dim) return std::unexpected(dim.error());
      writeInt(c, static_cast<std::int64_t>(*dim));
      zigzag::expectWritten(arena.link(
          c, dims.name, Posward, arena.makeCell(arena.textOf(*nameCell))));
      break;
    }
    case Kind::RefCapture: {
      const auto slot = arena.linked(m, binds_.dims.refFor, Posward);
      zigzag::expectWritten(
          arena.link(c, binds_.dims.refFor, Posward, mapped(*slot)));
      if (const auto binding = arena.linked(m, dims.values, Posward))
        zigzag::expectWritten(
            arena.link(c, dims.values, Posward, mapped(*binding)));
      // The member is its own endpoint record: its content is its own
      // position in the copy.
      writeInt(c, static_cast<std::int64_t>(c));
      break;
    }
    case Kind::Data: {
      const auto pos = posOf(m);
      if (!pos)
        return std::unexpected(
            "reify: unclassified recipe member on the copy pass");
      if (consumed.contains(i)) {
        // Tuple endpoint/direction field: endpoints remap through
        // the map, the direction sign stays verbatim.
        const auto source = static_cast<CellRef>(*pos);
        writeInt(c, fresh.contains(source)
                        ? static_cast<std::int64_t>(fresh.at(source))
                        : *pos);
      } else {
        writeInt(c,
                 static_cast<std::int64_t>(mapped(static_cast<CellRef>(*pos))));
      }
      break;
    }
    }
    zigzag::expectWritten(arena.link(c, binds_.dims.meshRelocation, Posward,
                                     arena.makeCell("cell")));
    const auto rec = arena.makeCell();
    zigzag::expectWritten(arena.link(rec, binds_.dims.ref, Posward, c));
    appendRef(rec);
    if (!recipeTail)
      zigzag::expectWritten(
          arena.link(newHead, binds_.dims.recipe, Posward, c));
    else
      zigzag::expectWritten(arena.link(*recipeTail, dims.step, Posward, c));
    recipeTail = c;
  }

  // `reify:<src>:<serial>` off the one durable counter — the same
  // counter `patch:<n>` draws in phase 4.
  zigzag::expectWritten(arena.link(
      newHead, dims.name, Posward,
      arena.makeCell(std::format("reify:{}:{}", sourceLabel, *serial))));
  return newHead;
}

// ---------------------------------------------------------------------------
// Mutation surface (§6.1, §6.3, §6.4, §6.6 — the phase-4 half)
// ---------------------------------------------------------------------------
//
// Every write below is an ordinary trailed arena link/unlink/value —
// §6.4's session arm needs nothing else: under live marks the trail
// restores the pre-mutation graph on release, so no session verb needs
// a marks guard of its own. The under-execution/turn-boundary arm is
// the adapter's, not this surface's: mutation mid-spin (a live
// `#Call` frame or choice structure on the *target*) is deferred to
// the next turn boundary by whoever drives the VM, and the kernel
// never sees it. P15's opcode-kind write guard is the sibling's lane —
// the verbs here write edges and fresh cells, never an op's kind.

namespace {

/// The structural-dimension vocabulary `program()` reachability walks —
/// §6.6's promotion set: `d.source` and `d.dims` stay out (the source
/// text is not structure; the dim rank is promotion-exempt).
[[nodiscard]] std::vector<DimRef>
programVocabulary(const xanadu::vagent::MetaBindSet &binds,
                  const zigzag::vortex::VortexCore &core) {
  const auto &dims = core.dims();
  std::vector<DimRef> vocab{
      dims.stdlib,   dims.vars,        dims.clause,     dims.values,
      dims.name,     dims.grab,        dims.spin,       dims.step,
      dims.contract, binds.dims.entry, binds.dims.refs, binds.dims.recipe};
  if (const auto extends = core.arena().dimensionNamed("d.extends"))
    vocab.push_back(*extends);
  return vocab;
}

} // namespace

MetaRuntime::MetaSignature MetaRuntime::signatureOf(CellRef head) const {
  MetaSignature sig;
  const auto descriptor = arena().linked(head, core_.dims().values, Posward);
  if (!descriptor || !arena().contains(*descriptor)) return sig;
  sig.descriptor = true;
  for (const auto param : parameterCells(*descriptor)) {
    const auto name = labelOf(param);
    const auto type = arena().linked(param, core_.dims().values, Posward);
    sig.params.emplace_back(name.value_or(""),
                            type ? arena().textOf(*type) : std::string{});
  }
  if (const auto result = descriptorField(*descriptor, "result")) {
    const auto type = arena().linked(*result, core_.dims().name, Posward);
    if (type) sig.resultType = arena().textOf(*type);
  }
  if (const auto pure = descriptorField(*descriptor, "pure")) {
    const auto flag = arena().linked(*pure, core_.dims().values, Posward);
    if (flag)
      sig.pure = arena().asBool(*flag).value_or(false) ||
                 arena().asInt64(*flag).value_or(0) != 0;
  }
  return sig;
}

std::expected<CellRef, std::string>
MetaRuntime::clauseHost(std::string_view name) const {
  // First-`:` split is a module arm only when the qualifier resolves;
  // a bare name walks the agent's own module's d.vars. The host is the
  // goal/event *symbol* — its +d.clause rank owns the attachment.
  std::optional<CellRef> module;
  std::string_view symbol = name;
  if (const auto colon = name.find(':'); colon != std::string_view::npos)
    if ((module = moduleNamed(name.substr(0, colon))))
      symbol = name.substr(colon + 1);
  if (!module) {
    if (binds_.ownModule == noCell || !arena().contains(binds_.ownModule))
      return std::unexpected(
          std::format("graft host '{}': no own module bound for a bare "
                      "name",
                      name));
    module = binds_.ownModule;
  }
  for (const auto candidate :
       zigzag::rankAfter(arena(), *module, core_.dims().vars))
    if (arena().textOf(candidate) == symbol) return candidate;
  return std::unexpected(
      std::format("graft host '{}': no such symbol on the resolved "
                  "module",
                  name));
}

std::expected<std::pair<CellRef, std::string>, std::string>
MetaRuntime::agentOwner(std::string_view owner) const {
  if (const auto colon = owner.find(':'); colon != std::string_view::npos)
    if (const auto module = moduleNamed(owner.substr(0, colon)))
      return std::pair{*module, std::string{owner.substr(colon + 1)}};
  if (binds_.ownModule == noCell || !arena().contains(binds_.ownModule))
    return std::unexpected(std::format(
        "graft owner '{}': no own module bound for a bare name", owner));
  return std::pair{binds_.ownModule, std::string{owner}};
}

bool MetaRuntime::installed(CellRef head) const {
  auto &arena      = core_.arena();
  const auto &dims = core_.dims();
  // M14's occupancy test verbatim — any install-dim backlink marks the
  // head consumed. No "still on a rank" refinement: a replaced
  // method's dead symbol keeps its +d.values→head wing, and that edge
  // *is* the evidence the head was installed (occupancy, not
  // destruction).
  return arena.linked(head, dims.grab, Negward).has_value() ||
         arena.linked(head, dims.spin, Negward).has_value() ||
         arena.linked(head, dims.values, Negward).has_value();
}

bool MetaRuntime::codeRegistered(CellRef head) const {
  return arena().rankContains(binds_.agentCell, head, binds_.dims.code);
}

std::expected<CellRef, std::string> MetaRuntime::installable(CellRef head) {
  // A d.code-resident head is a durable registration — grafting it in
  // place would couple the registry to the session rank, so the graft
  // installs a fresh reified copy (§6.3).
  if (!codeRegistered(head)) return head;
  return reify(head);
}

MetaRuntime::MetaMutation MetaRuntime::headShape(CellRef head,
                                                 bool pureRequired,
                                                 std::string_view role) const {
  auto &arena = core_.arena();
  if (!arena.contains(head))
    return std::unexpected(std::format("graft {}: operand is not an arena "
                                       "cell",
                                       role));
  if (isOpCell(head))
    return std::unexpected(
        std::format("graft {}: operand is an op cell — attach heads, never ops "
                    "(op wings are single-occupancy)",
                    role));
  if (!arena.linked(head, binds_.dims.entry, Posward).has_value())
    return std::unexpected(std::format(
        "graft {}: operand carries no +d.entry — not a fragment head", role));
  const auto sig = signatureOf(head);
  if (pureRequired && (!sig.descriptor || !sig.pure))
    return std::unexpected(
        std::format("graft {}: attachment is pure-required and the head's "
                    "descriptor does not flag it pure",
                    role));
  return {};
}

MetaRuntime::MetaMutation MetaRuntime::graftable(CellRef head,
                                                 bool pureRequired,
                                                 std::string_view role) const {
  if (const auto ok = headShape(head, pureRequired, role); !ok) return ok;
  if (installed(head))
    return std::unexpected(
        std::format("graft {}: head is already installed — a grafted head is "
                    "consumed",
                    role));
  const auto report =
      validate(head, MetaValidateOptions{.runReachable = true,
                                         .expectPure   = pureRequired});
  if (!report.ok)
    return std::unexpected(std::format("graft {}: {}", role, report.detail));
  return {};
}

MetaRuntime::MetaMutation MetaRuntime::learnGate(CellRef head,
                                                 bool pureRequired,
                                                 std::string_view role) const {
  // No validate arm: the durable twin's gate is closedness, and a
  // run-role record this session cannot resolve is §6.6's
  // fail-until-resume class — admitted here, judged at genesis.
  if (const auto ok = headShape(head, pureRequired, role); !ok) return ok;
  if (installed(head))
    return std::unexpected(std::format(
        "learn {}: head is already installed — an installed head is "
        "consumed (M14)",
        role));
  return learnable(head);
}

MetaRuntime::MetaSignature MetaRuntime::clauseFormals(CellRef host) const {
  for (const auto clause : clausesOf(host)) {
    const auto attach = arena().linked(clause, core_.dims().grab, Posward);
    if (!attach) continue;
    const auto sig = signatureOf(*attach);
    if (sig.descriptor) return sig;
  }
  return {};
}

MetaRuntime::MetaMutation
MetaRuntime::matchAttach(CellRef clause, DimRef dim, CellRef head,
                         std::string_view role) const {
  const auto sig = signatureOf(head);
  if (!sig.descriptor)
    return std::unexpected(std::format(
        "graft {}: head carries no descriptor signature to compare", role));
  const bool bodyArm = dim == core_.dims().spin;
  if (bodyArm && sig.resultType != "()")
    return std::unexpected(
        std::format("graft {}: a body must be Code<()>-shaped (result '{}')",
                    role, sig.resultType));
  const auto attach = arena().linked(clause, dim, Posward);
  if (!attach) return {}; // a clause with no current attach has no formals
  const auto canonical = signatureOf(*attach);
  if (!canonical.descriptor) return {};
  if (sig.params != canonical.params || sig.resultType != canonical.resultType)
    return std::unexpected(std::format(
        "graft {}: signature does not match the clause's formals", role));
  return {};
}

void MetaRuntime::unlinkRank(CellRef cell, DimRef dim) const {
  vagent::unlinkRank(arena(), cell, dim);
}

MetaRuntime::MetaMutation MetaRuntime::graftWhere(CellRef clause,
                                                  CellRef test) {
  if (const auto ok = graftable(test, /*pureRequired=*/true, "test"); !ok)
    return ok;
  if (const auto ok = matchAttach(clause, core_.dims().grab, test, "test"); !ok)
    return ok;
  const auto head = installable(test);
  if (!head) return std::unexpected(head.error());
  // One trailed re-point — link() evicts the clause's old +d.grab; the
  // old graph stays allocated but detached.
  zigzag::expectWritten(
      arena().link(clause, core_.dims().grab, Posward, *head));
  return {};
}

MetaRuntime::MetaMutation MetaRuntime::graftBody(CellRef clause, CellRef body) {
  if (const auto ok = graftable(body, /*pureRequired=*/false, "body"); !ok)
    return ok;
  if (const auto ok = matchAttach(clause, core_.dims().spin, body, "body"); !ok)
    return ok;
  const auto head = installable(body);
  if (!head) return std::unexpected(head.error());
  zigzag::expectWritten(
      arena().link(clause, core_.dims().spin, Posward, *head));
  return {};
}

std::expected<CellRef, std::string>
MetaRuntime::graftPlan(std::string_view goal, std::string_view name,
                       CellRef test, CellRef body) {
  if (reservedName(name))
    return std::unexpected(
        std::format("graft_plan: name '{}' is reserved", name));
  const auto host = clauseHost(goal);
  if (!host) return std::unexpected(host.error());
  if (const auto ok = graftable(test, true, "test"); !ok)
    return std::unexpected(ok.error());
  if (const auto ok = graftable(body, false, "body"); !ok)
    return std::unexpected(ok.error());

  // The goal's canonical clause formals: the first clause whose
  // +d.grab attach carries a descriptor supplies them — a plan's
  // clauses share one formal list.
  const auto canonical = clauseFormals(*host);
  const auto matchSig  = [&](CellRef head, std::string_view role,
                             bool testArm) -> MetaMutation {
    const auto sig = signatureOf(head);
    if (!sig.descriptor)
      return std::unexpected(std::format(
          "graft_plan: {} head carries no descriptor signature", role));
    if (!testArm && sig.resultType != "()")
      return std::unexpected(
          std::format("graft_plan: {} must be Code<()>-shaped (result '{}')",
                      role, sig.resultType));
    if (!canonical.descriptor) return {};
    if (sig.params != canonical.params ||
        (testArm && sig.resultType != canonical.resultType))
      return std::unexpected(std::format(
          "graft_plan: {} signature does not match the goal's clause "
          "formals",
          role));
    return {};
  };
  if (const auto ok = matchSig(test, "test", true); !ok)
    return std::unexpected(ok.error());
  if (const auto ok = matchSig(body, "body", false); !ok)
    return std::unexpected(ok.error());

  const auto testHead = installable(test);
  if (!testHead) return std::unexpected(testHead.error());
  const auto bodyHead = installable(body);
  if (!bodyHead) return std::unexpected(bodyHead.error());

  auto &arena       = core_.arena();
  const auto clause = arena.makeCell();
  zigzag::expectWritten(
      arena.link(clause, core_.dims().name, Posward, arena.makeCell(name)));
  zigzag::expectWritten(
      arena.link(clause, core_.dims().grab, Posward, *testHead));
  zigzag::expectWritten(
      arena.link(clause, core_.dims().spin, Posward, *bodyHead));
  zigzag::expectWritten(
      arena.link(zigzag::rankTail(arena, *host, core_.dims().clause),
                 core_.dims().clause, Posward, clause));
  return clause;
}

std::expected<CellRef, std::string>
MetaRuntime::graftHandler(std::string_view event, std::string_view name,
                          CellRef guard, CellRef body, std::int64_t priority) {
  if (reservedName(name))
    return std::unexpected(
        std::format("graft_handler: name '{}' is reserved", name));
  const auto host = clauseHost(event);
  if (!host) return std::unexpected(host.error());
  if (const auto ok = graftable(guard, true, "guard"); !ok)
    return std::unexpected(ok.error());
  if (const auto ok = graftable(body, false, "body"); !ok)
    return std::unexpected(ok.error());
  // A handler's guard binds `occurrence` — the canonical formals come
  // off the event's existing clauses the way graft_plan's do.
  const auto canonical = clauseFormals(*host);
  const auto guardSig  = signatureOf(guard);
  const auto bodySig   = signatureOf(body);
  if (canonical.descriptor) {
    if (guardSig.descriptor && guardSig.params != canonical.params)
      return std::unexpected(
          "graft_handler: guard signature does not match the event's "
          "clause formals");
    if (bodySig.descriptor && bodySig.params != canonical.params)
      return std::unexpected(
          "graft_handler: body signature does not match the event's "
          "clause formals");
  }
  if (bodySig.descriptor && bodySig.resultType != "()")
    return std::unexpected("graft_handler: body must be Code<()>-shaped");

  const auto guardHead = installable(guard);
  if (!guardHead) return std::unexpected(guardHead.error());
  const auto bodyHead = installable(body);
  if (!bodyHead) return std::unexpected(bodyHead.error());

  auto &arena       = core_.arena();
  const auto clause = arena.makeCell();
  zigzag::expectWritten(
      arena.link(clause, core_.dims().name, Posward, arena.makeCell(name)));
  zigzag::expectWritten(
      arena.link(clause, core_.dims().grab, Posward, *guardHead));
  zigzag::expectWritten(
      arena.link(clause, core_.dims().spin, Posward, *bodyHead));
  // The handler's priority rides clause-side as a keyed d.vars record —
  // session-visible like a clause weight, never a patch field.
  const auto rec = varsRecord(clause, "priority", /*mint=*/true);
  if (rec)
    zigzag::expectWritten(arena.link(*rec, core_.dims().values, Posward,
                                     arena.makeScalarCell(priority)));
  zigzag::expectWritten(
      arena.link(zigzag::rankTail(arena, *host, core_.dims().clause),
                 core_.dims().clause, Posward, clause));
  return clause;
}

std::expected<CellRef, std::string>
MetaRuntime::graftMethod(std::string_view owner, std::string_view name,
                         CellRef code) {
  if (reservedName(name))
    return std::unexpected(
        std::format("graft_method: name '{}' is reserved", name));
  const auto resolved = agentOwner(owner);
  if (!resolved) return std::unexpected(resolved.error());
  const auto &[module, agentName] = *resolved;
  if (const auto ok = graftable(code, false, "code"); !ok)
    return std::unexpected(ok.error());
  // The declared-signature match against the host member is the
  // adapter's arm — the engine cannot read a capacity's decl — so an
  // effect-mismatch refusal never reaches this layer.

  const auto head = installable(code);
  if (!head) return std::unexpected(head.error());

  auto &arena          = core_.arena();
  const auto qualified = std::format("{}.{}", agentName, name);
  // Redefinition is replacement: symbol lookup is first-match-wins, so
  // any existing same-named symbol unlinks first.
  for (const auto symbol : zigzag::rankAfter(arena, module, core_.dims().vars))
    if (arena.textOf(symbol) == qualified) {
      unlinkRank(symbol, core_.dims().vars);
      break;
    }
  const auto symbol = arena.makeCell(qualified);
  zigzag::expectWritten(
      arena.link(symbol, core_.dims().values, Posward, *head));
  zigzag::expectWritten(
      arena.link(zigzag::rankTail(arena, module, core_.dims().vars),
                 core_.dims().vars, Posward, symbol));
  return symbol;
}

MetaRuntime::MetaMutation MetaRuntime::prune(CellRef clause) {
  auto &arena = core_.arena();
  if (!arena.linked(clause, core_.dims().clause, Negward).has_value() &&
      !arena.linked(clause, core_.dims().clause, Posward).has_value())
    return std::unexpected("prune: operand is not on a clause rank");
  unlinkRank(clause, core_.dims().clause);
  return {};
}

// ---------------------------------------------------------------------------
// learn_* — durable twins
// ---------------------------------------------------------------------------

bool MetaRuntime::imageRef(CellRef cell) const {
  const auto extent = binds_.lineage ? binds_.lineage->imageExtent : 0u;
  if (zigzag::isEphemeral(cell))
    // Dense ids only line up with the image extent in a flat arena. On a
    // checkpoint overlay an ephemeral cell is session-minted above the base —
    // always post-image — so the dense-id test must not run: it reads a
    // fresh dense index that does not correspond to a spool position and
    // would classify a reified copy as image (program-side), letting it
    // graft/learn closed-unchecked.
    return arena().base() == nullptr &&
           arena().denseOf(cell).value_or(0) < extent;
  // A persistent ref names its spool position; image cells sit at or
  // before the image's op extent.
  return cell <= extent;
}

bool MetaRuntime::hasProgramLineage() const {
  // §6.6's conjunction, kernel-simulable half: a bound lineage record
  // whose program() is an image-side ref. The reachability half is
  // programCells() — the gate refuses closed without a lineage record
  // at all.
  if (!binds_.lineage) return false;
  const auto ref = binds_.lineage->programRef;
  if (ref == noCell || !arena().contains(ref)) return false;
  return imageRef(ref);
}

std::set<CellRef> MetaRuntime::programCells() const {
  // Outbound walk over the structural vocabulary — image cells join
  // unconditionally (the persistent-image arm), then everything the
  // walk reaches is program-side too (a state→program link would make
  // program structure reachable, and this set is how the closed walk
  // refuses to smuggle one back).
  std::set<CellRef> program{binds_.programRoot};
  std::deque<CellRef> frontier{binds_.programRoot};
  const auto vocab  = programVocabulary(binds_, core_);
  std::size_t steps = 0;
  const auto bound  = arena().traversalBound();
  while (!frontier.empty() && steps++ < bound) {
    const auto cell = frontier.front();
    frontier.pop_front();
    for (const auto dim : vocab)
      for (const auto next : zigzag::rankAfter(arena(), cell, dim))
        if (program.insert(next).second) frontier.push_back(next);
  }
  return program;
}

MetaRuntime::MetaMutation MetaRuntime::learnable(CellRef head) const {
  if (!hasProgramLineage())
    return std::unexpected(
        "learn refuses: no program lineage — the bind set carries no "
        "image-bound program() record (§6.6)");
  const auto report = verifyClosed(head);
  if (!report.closed)
    return std::unexpected(std::format("learn refuses: {}", report.detail));
  return {};
}

std::expected<MetaRuntime::MetaLearnCopies, std::string>
MetaRuntime::learnCopies(CellRef head) {
  // Two reifications: the registered head and the installed copy — the
  // original program-side fragment is never registered or installed.
  // The carve-out: an argument already d.code-registered reuses itself
  // as the registered head, and only the installed copy reifies.
  if (codeRegistered(head)) {
    auto installed = reify(head);
    if (!installed) return std::unexpected(installed.error());
    return MetaLearnCopies{head, *installed};
  }
  auto registered = reify(head);
  if (!registered) return std::unexpected(registered.error());
  auto installed = reify(head);
  if (!installed) return std::unexpected(installed.error());
  return MetaLearnCopies{*registered, *installed};
}

MetaRuntime::MetaMutation MetaRuntime::registerHead(CellRef head) const {
  auto &arena      = core_.arena();
  const auto label = labelOf(head).value_or("");
  for (const auto member :
       zigzag::rankAfter(arena, binds_.agentCell, binds_.dims.code))
    if (labelOf(member).value_or("") == label)
      return std::unexpected(std::format(
          "d.code registration refuses a second head carrying label "
          "'{}'",
          label));
  if (!codeRegistered(head))
    zigzag::expectWritten(
        arena.link(zigzag::rankTail(arena, binds_.agentCell, binds_.dims.code),
                   binds_.dims.code, Posward, head));
  return {};
}

std::optional<std::string> MetaRuntime::patchField(CellRef patch,
                                                   std::string_view key,
                                                   bool descriptor) const {
  return vagent::patchField(arena(), core_, patch, key, descriptor);
}

std::vector<std::string> MetaRuntime::patchHeadNames(CellRef patch) const {
  return vagent::patchHeadNames(arena(), core_, patch, binds_.dims.patchHead);
}

std::optional<std::string>
MetaRuntime::priorAttachName(std::string_view clauseName,
                             DimRef attachDim) const {
  // Latest patch recording this clause walks back: a learn_plan /
  // learn_handler chain is [test, body], a replace chain [installed,
  // prior?] — so the prior for a grab attach is always index 0 of the
  // newest clause-touching patch; for a spin attach index 1 of a
  // plan/handler patch, index 0 of a learn_body patch.
  auto &arena = core_.arena();
  const auto rank =
      collect(zigzag::rankAfter(arena, binds_.agentCell, binds_.dims.patch));
  for (const auto patch : rank | std::views::reverse) {
    if (patchField(patch, "target", true).value_or("") != clauseName) continue;
    const auto verb  = patchField(patch, "verb", true).value_or("");
    const auto names = patchHeadNames(patch);
    if (attachDim == core_.dims().grab) {
      if (verb == "learn_plan" || verb == "learn_handler" ||
          verb == "learn_where")
        if (!names.empty()) return names.front();
    } else {
      if ((verb == "learn_plan" || verb == "learn_handler") && names.size() > 1)
        return names[1];
      if (verb == "learn_body" && !names.empty()) return names[0];
    }
  }
  return std::nullopt;
}

std::expected<CellRef, std::string> MetaRuntime::mintPatch(
    std::string_view label, std::int64_t ordinal, std::string_view verb,
    std::string_view target, std::optional<std::string_view> parent,
    std::optional<std::string_view> alias, std::optional<std::int64_t> priority,
    const std::vector<std::string> &headNames) {
  auto &arena      = core_.arena();
  const auto patch = arena.makeCell();
  zigzag::expectWritten(
      arena.link(patch, core_.dims().name, Posward, arena.makeCell(label)));
  const auto descriptor = arena.makeCell();
  zigzag::expectWritten(
      arena.link(patch, core_.dims().values, Posward, descriptor));
  const auto field = [&](CellRef owner, std::string_view key, CellRef value) {
    const auto rec = arena.makeCell(key);
    zigzag::expectWritten(arena.link(rec, core_.dims().values, Posward, value));
    zigzag::expectWritten(
        arena.link(zigzag::rankTail(arena, owner, core_.dims().vars),
                   core_.dims().vars, Posward, rec));
    return rec;
  };
  field(descriptor, "target", arena.makeCell(target));
  field(descriptor, "verb", arena.makeCell(verb));
  if (parent) field(descriptor, "parent", arena.makeCell(*parent));
  field(patch, "ordinal", arena.makeScalarCell(ordinal));
  if (alias) field(patch, "alias", arena.makeCell(*alias));
  if (priority) field(patch, "priority", arena.makeScalarCell(*priority));

  // The patch-head chain: one name record per registered head this
  // patch installed, +d.values → a fresh text cell (the re-resolution
  // key at genesis — durable identities are names, never CellRefs).
  std::optional<CellRef> chainTail;
  for (const auto &headName : headNames) {
    const auto record = arena.makeCell();
    zigzag::expectWritten(arena.link(record, core_.dims().values, Posward,
                                     arena.makeCell(headName)));
    if (!chainTail)
      zigzag::expectWritten(
          arena.link(patch, binds_.dims.patchHead, Posward, record));
    else
      zigzag::expectWritten(
          arena.link(*chainTail, core_.dims().step, Posward, record));
    chainTail = record;
  }
  zigzag::expectWritten(
      arena.link(zigzag::rankTail(arena, binds_.agentCell, binds_.dims.patch),
                 binds_.dims.patch, Posward, patch));
  return patch;
}

std::expected<CellRef, std::string>
MetaRuntime::learnPlan(std::string_view goal, std::string_view alias,
                       CellRef test, CellRef body) {
  if (reservedName(alias))
    return std::unexpected(
        std::format("learn_plan: alias '{}' is reserved", alias));
  const auto host = clauseHost(goal);
  if (!host) return std::unexpected(host.error());
  if (const auto ok = learnGate(test, true, "test"); !ok)
    return std::unexpected(ok.error());
  if (const auto ok = learnGate(body, false, "body"); !ok)
    return std::unexpected(ok.error());

  const auto testCopies = learnCopies(test);
  if (!testCopies) return std::unexpected(testCopies.error());
  const auto bodyCopies = learnCopies(body);
  if (!bodyCopies) return std::unexpected(bodyCopies.error());
  if (const auto ok = registerHead(testCopies->registered); !ok)
    return std::unexpected(ok.error());
  if (const auto ok = registerHead(bodyCopies->registered); !ok)
    return std::unexpected(ok.error());

  auto &arena = core_.arena();
  // Verbatim alias dedupe on the install rank only — a declared clause
  // carrying the alias label unlinks first (the durable replacement
  // the patch records; learned clauses are patch:<n>-labelled and
  // unreachable by alias).
  for (const auto clause : clausesOf(*host))
    if (labelOf(clause).value_or("") == alias)
      unlinkRank(clause, core_.dims().clause);

  // One serial for the pair — the clause's +d.name and the patch
  // record's +d.name are the same patch:<n> label.
  const auto serial = nextSerial();
  if (!serial) return std::unexpected(serial.error());
  const auto patchLabel = std::format("patch:{}", *serial);

  const auto clause = arena.makeCell();
  zigzag::expectWritten(arena.link(clause, core_.dims().name, Posward,
                                   arena.makeCell(patchLabel)));
  zigzag::expectWritten(
      arena.link(clause, core_.dims().grab, Posward, testCopies->installed));
  zigzag::expectWritten(
      arena.link(clause, core_.dims().spin, Posward, bodyCopies->installed));
  zigzag::expectWritten(
      arena.link(zigzag::rankTail(arena, *host, core_.dims().clause),
                 core_.dims().clause, Posward, clause));

  const auto patch = mintPatch(patchLabel, *serial, "learn_plan", patchLabel,
                               goal, alias, std::nullopt,
                               {labelOf(testCopies->registered).value_or(""),
                                labelOf(bodyCopies->registered).value_or("")});
  if (!patch) return std::unexpected(patch.error());
  return clause;
}

std::expected<CellRef, std::string>
MetaRuntime::learnHandler(std::string_view event, std::string_view alias,
                          CellRef guard, CellRef body, std::int64_t priority) {
  if (reservedName(alias))
    return std::unexpected(
        std::format("learn_handler: alias '{}' is reserved", alias));
  const auto host = clauseHost(event);
  if (!host) return std::unexpected(host.error());
  if (const auto ok = learnGate(guard, true, "guard"); !ok)
    return std::unexpected(ok.error());
  if (const auto ok = learnGate(body, false, "body"); !ok)
    return std::unexpected(ok.error());

  const auto guardCopies = learnCopies(guard);
  if (!guardCopies) return std::unexpected(guardCopies.error());
  const auto bodyCopies = learnCopies(body);
  if (!bodyCopies) return std::unexpected(bodyCopies.error());
  if (const auto ok = registerHead(guardCopies->registered); !ok)
    return std::unexpected(ok.error());
  if (const auto ok = registerHead(bodyCopies->registered); !ok)
    return std::unexpected(ok.error());

  auto &arena       = core_.arena();
  const auto serial = nextSerial();
  if (!serial) return std::unexpected(serial.error());
  const auto patchLabel = std::format("patch:{}", *serial);

  // Additive: learn_handler carries no alias dedupe — a second
  // same-alias clause appends under its own patch:<n>.
  const auto clause = arena.makeCell();
  zigzag::expectWritten(arena.link(clause, core_.dims().name, Posward,
                                   arena.makeCell(patchLabel)));
  zigzag::expectWritten(
      arena.link(clause, core_.dims().grab, Posward, guardCopies->installed));
  zigzag::expectWritten(
      arena.link(clause, core_.dims().spin, Posward, bodyCopies->installed));
  if (const auto rec = varsRecord(clause, "priority", /*mint=*/true))
    zigzag::expectWritten(arena.link(*rec, core_.dims().values, Posward,
                                     arena.makeScalarCell(priority)));
  zigzag::expectWritten(
      arena.link(zigzag::rankTail(arena, *host, core_.dims().clause),
                 core_.dims().clause, Posward, clause));

  const auto patch = mintPatch(patchLabel, *serial, "learn_handler", patchLabel,
                               event, alias, priority,
                               {labelOf(guardCopies->registered).value_or(""),
                                labelOf(bodyCopies->registered).value_or("")});
  if (!patch) return std::unexpected(patch.error());
  return clause;
}

MetaRuntime::MetaMutation MetaRuntime::learnWhere(CellRef clause,
                                                  CellRef test) {
  auto &arena     = core_.arena();
  const auto name = labelOf(clause);
  if (!name)
    return std::unexpected(
        "learn_where: the target clause carries no +d.name — the "
        "durable record needs a resolvable key");
  if (const auto ok = learnGate(test, true, "test"); !ok) return ok;
  if (const auto ok = matchAttach(clause, core_.dims().grab, test, "test"); !ok)
    return ok;

  const auto copies = learnCopies(test);
  if (!copies) return std::unexpected(copies.error());
  if (const auto ok = registerHead(copies->registered); !ok) return ok;

  const auto serial = nextSerial();
  if (!serial) return std::unexpected(serial.error());
  const auto prior = priorAttachName(*name, core_.dims().grab);
  zigzag::expectWritten(
      arena.link(clause, core_.dims().grab, Posward, copies->installed));
  std::vector<std::string> heads{labelOf(copies->registered).value_or("")};
  if (prior) heads.push_back(*prior);
  const auto patch =
      mintPatch(std::format("patch:{}", *serial), *serial, "learn_where", *name,
                std::nullopt, std::nullopt, std::nullopt, heads);
  if (!patch) return std::unexpected(patch.error());
  return {};
}

MetaRuntime::MetaMutation MetaRuntime::learnBody(CellRef clause, CellRef body) {
  auto &arena     = core_.arena();
  const auto name = labelOf(clause);
  if (!name)
    return std::unexpected(
        "learn_body: the target clause carries no +d.name — the "
        "durable record needs a resolvable key");
  if (const auto ok = learnGate(body, false, "body"); !ok) return ok;
  if (const auto ok = matchAttach(clause, core_.dims().spin, body, "body"); !ok)
    return ok;

  const auto copies = learnCopies(body);
  if (!copies) return std::unexpected(copies.error());
  if (const auto ok = registerHead(copies->registered); !ok) return ok;

  const auto serial = nextSerial();
  if (!serial) return std::unexpected(serial.error());
  const auto prior = priorAttachName(*name, core_.dims().spin);
  zigzag::expectWritten(
      arena.link(clause, core_.dims().spin, Posward, copies->installed));
  std::vector<std::string> heads{labelOf(copies->registered).value_or("")};
  if (prior) heads.push_back(*prior);
  const auto patch =
      mintPatch(std::format("patch:{}", *serial), *serial, "learn_body", *name,
                std::nullopt, std::nullopt, std::nullopt, heads);
  if (!patch) return std::unexpected(patch.error());
  return {};
}

std::expected<CellRef, std::string>
MetaRuntime::learnMethod(std::string_view owner, std::string_view name,
                         CellRef code) {
  if (reservedName(name))
    return std::unexpected(
        std::format("learn_method: name '{}' is reserved", name));
  const auto resolved = agentOwner(owner);
  if (!resolved) return std::unexpected(resolved.error());
  const auto &[module, agentName] = *resolved;
  if (const auto ok = learnGate(code, false, "code"); !ok)
    return std::unexpected(ok.error());

  const auto copies = learnCopies(code);
  if (!copies) return std::unexpected(copies.error());
  if (const auto ok = registerHead(copies->registered); !ok)
    return std::unexpected(ok.error());

  auto &arena          = core_.arena();
  const auto qualified = std::format("{}.{}", agentName, name);
  for (const auto symbol : zigzag::rankAfter(arena, module, core_.dims().vars))
    if (arena.textOf(symbol) == qualified) {
      unlinkRank(symbol, core_.dims().vars);
      break;
    }
  const auto symbol = arena.makeCell(qualified);
  zigzag::expectWritten(
      arena.link(symbol, core_.dims().values, Posward, copies->installed));
  zigzag::expectWritten(
      arena.link(zigzag::rankTail(arena, module, core_.dims().vars),
                 core_.dims().vars, Posward, symbol));

  // The record's target is the composed module:Agent.member spelling —
  // unlearn's symbol arm recomposes it from the rank head.
  const auto moduleName = arena.textOf(module);
  const auto serial     = nextSerial();
  if (!serial) return std::unexpected(serial.error());
  const auto patch = mintPatch(
      std::format("patch:{}", *serial), *serial, "learn_method",
      std::format("{}:{}", moduleName, qualified), moduleName, std::nullopt,
      std::nullopt, {labelOf(copies->registered).value_or("")});
  if (!patch) return std::unexpected(patch.error());
  return symbol;
}

MetaRuntime::MetaMutation MetaRuntime::undoPatch(CellRef patch) {
  auto &arena       = core_.arena();
  const auto verb   = patchField(patch, "verb", true).value_or("");
  const auto target = patchField(patch, "target", true).value_or("");
  const auto heads  = patchHeadNames(patch);

  const auto dropDead = [&](std::string_view why) -> MetaMutation {
    // A patch whose install can no longer be resolved is dead — drop
    // it and leave a ConditionFault-shaped fault record on the agent,
    // the way a dropped d.patch names its own failure.
    const auto label = labelOf(patch).value_or("");
    unlinkRank(patch, binds_.dims.patch);
    if (const auto rec =
            varsRecord(binds_.agentCell, std::format("fault:{}", label),
                       /*mint=*/true))
      zigzag::expectWritten(arena.link(*rec, core_.dims().values, Posward,
                                       arena.makeCell(std::string{why})));
    // The unlink commits under regenesisCycle's Clears policy; the unlearn
    // tombstone still matters — a clause sharing `patch:<n>` on an older
    // lineage folded without clears would otherwise come back live.
    markUnlearned(label);
    return std::unexpected(
        std::format("unlearn: dead patch dropped — {}", why));
  };

  if (verb == "learn_plan" || verb == "learn_handler") {
    const auto clause = planNamed(target);
    if (!clause) return dropDead("the learned clause no longer resolves");
    unlinkRank(*clause, core_.dims().clause);
  } else if (verb == "learn_method") {
    // Recompose module:Agent.member → symbolNamed walks the module's
    // d.vars for the Agent.member content.
    const auto colon = target.find(':');
    if (colon == std::string::npos) return dropDead("malformed method target");
    const auto symbol =
        symbolNamed(target.substr(0, colon), target.substr(colon + 1));
    if (!symbol) return dropDead("the learned symbol no longer resolves");
    unlinkRank(*symbol, core_.dims().vars);
  } else if (verb == "learn_where" || verb == "learn_body") {
    const auto clause = planNamed(target);
    if (!clause) return dropDead("the patched clause no longer resolves");
    const auto dim =
        verb == "learn_where" ? core_.dims().grab : core_.dims().spin;
    if (heads.size() > 1) {
      // A recorded prior: re-point through a fresh reify of the named
      // registered head — a closed copy is safe to link state-side.
      const auto prior = codeNamed(heads[1]);
      if (!prior)
        return dropDead("the recorded prior head no longer resolves on d.code");
      auto copy = reify(*prior);
      if (!copy) return std::unexpected(copy.error());
      zigzag::expectWritten(arena.link(*clause, dim, Posward, *copy));
    } else {
      // Program-original attachment: never recorded, never restored
      // through a state→program link — detach and let genesis's image
      // replay put the original back.
      zigzag::expectWritten(arena.unlink(*clause, dim, Posward));
    }
  } else {
    return dropDead("patch carries no replayable verb");
  }

  // The patch drops; every head name it recorded — installs and prior
  // names alike — loses one namer, and a registration no surviving
  // patch still names drops with it (a where/body chain's prior name
  // rides exactly for this undo, so it counts as a namer too).
  const auto survivors =
      collect(zigzag::rankAfter(arena, binds_.agentCell, binds_.dims.patch));
  const auto stillNamed = [&](std::string_view headName) {
    for (const auto other : survivors)
      if (other != patch)
        for (const auto &named : patchHeadNames(other))
          if (named == headName) return true;
    return false;
  };
  unlinkRank(patch, binds_.dims.patch);
  for (const auto &headName : heads)
    if (!stillNamed(headName))
      if (const auto registered = codeNamed(headName);
          registered && codeRegistered(*registered))
        unlinkRank(*registered, binds_.dims.code);

  // regenesisCycle's commitShadowed policy (Clears on state-side shadows)
  // now commits the unlinks above durably; the tombstone still retires the
  // names that must stay dead — for lineages folded before the clears, and
  // as the replay-time belt-and-suspenders: the patch's own label (genesis's
  // tombstone arm skips its replay — and for plan/handler the clause shares
  // `patch:<n>`, so the same record retires it) and, for the method arm, the
  // symbol's module:Agent.method. A where/body unlearn
  // keeps its clause live — only the patch tombstone lands for it.
  markUnlearned(labelOf(patch).value_or(""));
  if (verb == "learn_method") markUnlearned(target);
  return {};
}

MetaRuntime::MetaMutation MetaRuntime::unlearn(CellRef clauseOrSymbol) {
  auto &arena = core_.arena();
  if (!arena.contains(clauseOrSymbol))
    return std::unexpected("unlearn: operand is not an arena cell");
  // A d.patch record rides the same rank it would undo — refuse it by
  // its backlink before any name resolution.
  if (arena.linked(clauseOrSymbol, binds_.dims.patch, Negward).has_value())
    return std::unexpected(
        "unlearn: a d.patch record is not a learnable attachment");

  std::string name;
  if (const auto label = labelOf(clauseOrSymbol)) {
    name = *label;
  } else if (arena.linked(clauseOrSymbol, core_.dims().vars, Negward)
                 .has_value()) {
    // The symbol arm: the rank head supplies the module: prefix.
    const auto head =
        zigzag::rankTail(arena, clauseOrSymbol, core_.dims().vars, Negward);
    const auto owner = arena.textOf(head);
    if (owner.empty() || arena.textOf(clauseOrSymbol).empty())
      return std::unexpected(
          "unlearn: symbol operand's rank head carries no module name");
    name = std::format("{}:{}", owner, arena.textOf(clauseOrSymbol));
  } else {
    return std::unexpected(
        "unlearn: operand names no learnable attachment — neither a "
        "+d.name clause nor a d.vars symbol");
  }

  const auto rank =
      collect(zigzag::rankAfter(arena, binds_.agentCell, binds_.dims.patch));
  for (const auto patch : rank | std::views::reverse)
    if (patchField(patch, "target", true).value_or("") == name)
      return undoPatch(patch);
  return std::unexpected(
      std::format("unlearn: no d.patch records target '{}'", name));
}

MetaRuntime::MetaMutation MetaRuntime::setWeight(CellRef cell, double weight) {
  auto &arena = core_.arena();
  if (!arena.contains(cell))
    return std::unexpected("set_weight: operand is not an arena cell");
  // Junction arm: the cell's own +d.weight edge takes a fresh scalar —
  // mint-and-repoint, never an in-place write, so a shared scalar is
  // never mutated under a reader.
  if (arena.linked(cell, binds_.dims.weight, Posward).has_value()) {
    zigzag::expectWritten(arena.link(cell, binds_.dims.weight, Posward,
                                     arena.makeScalarCell(weight)));
    return {};
  }
  // Clause arm: the agent's keyed d.vars record — minted on first
  // write, keyed by the clause's +d.name verbatim.
  if (const auto label = labelOf(cell)) {
    const auto record = varsRecord(binds_.agentCell, *label, /*mint=*/true);
    if (!record)
      return std::unexpected(
          "set_weight: could not mint the agent's d.vars record");
    zigzag::expectWritten(arena.link(*record, core_.dims().values, Posward,
                                     arena.makeScalarCell(weight)));
    return {};
  }
  return std::unexpected(
      "set_weight: a name-less clause carries no weight channel — it "
      "has neither +d.weight nor +d.name");
}

std::expected<CellRef, std::string>
MetaRuntime::sproutSynapse(std::string_view from, std::string_view to,
                           double weight) {
  auto &arena         = core_.arena();
  const auto junction = arena.makeCell();
  zigzag::expectWritten(arena.link(junction, binds_.dims.weight, Posward,
                                   arena.makeScalarCell(weight)));
  if (const auto rec = varsRecord(junction, "from", /*mint=*/true))
    zigzag::expectWritten(
        arena.link(*rec, core_.dims().values, Posward, arena.makeCell(from)));
  if (const auto rec = varsRecord(junction, "to", /*mint=*/true))
    zigzag::expectWritten(
        arena.link(*rec, core_.dims().values, Posward, arena.makeCell(to)));
  zigzag::expectWritten(
      arena.link(zigzag::rankTail(arena, binds_.agentCell, binds_.dims.synapse),
                 binds_.dims.synapse, Posward, junction));
  return junction;
}

MetaClosedReport MetaRuntime::verifyClosed(CellRef head) const {
  auto &arena = core_.arena();
  MetaClosedReport report;
  const auto fail = [&](std::string detail) {
    report.closed = false;
    report.detail = std::move(detail);
    return report;
  };

  // The recipe-covered internal set: the positions members name as covered
  // cells — literals, tuple endpoints, capture slots — plus the head.
  // Ref-records whose marked cell resolves inside it are internal by
  // construction. Covered positions are persistent *or* ephemeral — an
  // image-resident fragment's members name image-side cells — so ephemerality
  // cannot discriminate; the lone non-position is the `±1` direction flag.
  std::set<CellRef> internal{head};
  std::map<CellRef, CellRef> captureFor;
  for (const auto member :
       stepChain(arena, head, binds_.dims.recipe, core_.dims().step)) {
    if (const auto slot = arena.linked(member, binds_.dims.refFor, Posward))
      captureFor[*slot] = member;
    // Every member value is a covered position — persistent or ephemeral
    // alike (an image-resident fragment's members name image-side cells) —
    // except the link-tuple direction flag, the lone member storing ±1.
    if (const auto pos = arena.asInt64(member))
      if (*pos != 1 && *pos != -1) internal.insert(static_cast<CellRef>(*pos));
  }

  const auto program     = programCells();
  const auto programSide = [&](CellRef cell) {
    return imageRef(cell) || program.contains(cell);
  };

  // A run/call name-record's rebind class: qualified names re-resolve
  // through module→symbol→+d.values; labels through code_named. A
  // resolved head (or a call record's op) is fine; a run record
  // landing head-less is fail-forever; unresolved is
  // fail-until-resume — the emission-fixed slot only genesis fills.
  const auto rebindClass = [&](std::string_view name,
                               std::string_view role) -> std::string {
    std::optional<CellRef> target;
    if (const auto colon = name.find(':'); colon != std::string_view::npos)
      if (moduleNamed(name.substr(0, colon)))
        if (const auto symbol =
                symbolNamed(name.substr(0, colon), name.substr(colon + 1)))
          target = arena.linked(*symbol, core_.dims().values, Posward);
    if (target && arena.contains(*target)) {
      const bool head =
          arena.linked(*target, binds_.dims.entry, Posward).has_value();
      if (role == "run" && !head)
        return std::format(
            "run-role record '{}' resolves head-less — fail-forever", name);
      return {};
    }
    if (codeNamed(name).has_value()) return {};
    return std::format("{}-role record '{}' unresolved — "
                       "fail-until-resume",
                       role, name);
  };

  for (const auto record :
       stepChain(arena, head, binds_.dims.refs, core_.dims().step)) {
    const auto marked   = arena.linked(record, binds_.dims.ref, Posward);
    const auto nameCell = arena.linked(record, core_.dims().name, Posward);
    if (nameCell) {
      // NAME-record: resolved by name at genesis. Only run/call-role
      // records classify a refusal future — an ordinary name-record
      // always rebinds verbatim.
      const auto roleCell = arena.linked(record, core_.dims().grab, Posward);
      const auto role     = roleCell ? arena.textOf(*roleCell) : std::string{};
      if (role == "run" || role == "call") {
        const auto kind = rebindClass(arena.textOf(*nameCell), role);
        if (!kind.empty()) {
          if (kind.ends_with("fail-forever"))
            return fail(std::format("closed violation: {}", kind));
          report.notes.push_back(kind);
        }
      }
      continue;
    }
    if (!marked || !arena.contains(*marked))
      return fail("closed: +d.refs record has no +d.ref cell");
    const auto pos = arena.asInt64(*marked);
    if (pos && internal.contains(static_cast<CellRef>(*pos))) continue;
    // A slot's int64 is the referent; the paired ref-capture record
    // classifies dim and cap$N endpoints.
    const auto member = captureFor.find(*marked);
    if (member != captureFor.end()) {
      if (arena.linked(member->second, core_.dims().name, Posward).has_value())
        continue; // dimension referent — re-resolves by name
      if (const auto binding =
              arena.linked(member->second, core_.dims().values, Posward)) {
        if (programSide(*binding))
          return fail(std::format(
              "closed violation: cap$N capture binds program-side cell {}",
              static_cast<std::uint64_t>(*binding)));
        continue;
      }
    } else {
      // `marked` is a recipe member cell, not a ref-slot: a dim anchor
      // re-resolves by name through its own `+d.name`, and a bare `±1`
      // direction flag is a field value, not a referent — neither is an
      // outbound edge.
      if (arena.linked(*marked, core_.dims().name, Posward).has_value())
        continue;
      if (pos && (*pos == 1 || *pos == -1)) continue;
    }
    if (!pos) {
      // An unwritten ref-record slot carries no referent to smuggle —
      // the emission-fixed unwritten-slot arm rides verbatim.
      report.notes.push_back(
          "ref-record slot carries no referent — emission-fixed "
          "unwritten slot");
      continue;
    }
    if (programSide(static_cast<CellRef>(*pos)))
      return fail(std::format(
          "closed violation: outbound edge to program-side cell {} lacks "
          "name-resolution",
          *pos));
  }
  report.closed = true;
  return report;
}

} // namespace xanadu::vagent

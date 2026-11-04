#include "common/xanadu/vagent/meta.hpp"

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
  for (const auto symbol :
       zigzag::rankAfter(arena(), *module, core_.dims().vars))
    if (arena().textOf(symbol) == name) return symbol;
  return std::nullopt;
}

std::vector<CellRef> MetaRuntime::clausesOf(CellRef cell) const {
  return collect(zigzag::rankAfter(arena(), cell, core_.dims().clause));
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
  const auto vars = core_.dims().vars;
  for (const auto record : zigzag::rankAfter(arena(), owner, vars))
    if (arena().textOf(record) == key) return record;
  if (!mint) return std::nullopt;
  const auto record = arena().makeCell(key);
  zigzag::expectWritten(arena().link(zigzag::rankTail(arena(), owner, vars),
                                     vars, Posward, record));
  return record;
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
  zigzag::expectWritten(
      arena.setValueBits(*value, xanadu::ValueKind::Int64,
                         std::bit_cast<std::uint64_t>(*current + 1)));
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

  vm_.setCursorOpcode(cur, *entry);
  const auto outcome = vm_.run(cur, maxCycles);

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
    // Arena cells carry no clone-master indirection — setValueBits
    // takes the cell itself.
    zigzag::expectWritten(arena.setValueBits(
        cell, xanadu::ValueKind::Int64, std::bit_cast<std::uint64_t>(value)));
  };
  const auto mapped = [&](CellRef source) {
    const auto found = fresh.find(source);
    return found != fresh.end() ? found->second : source;
  };

  // Pass 2 — replay: tuples relink through the map, literals copy
  // content, ref-capture slots rebind after every cell exists.
  for (std::size_t i = 0; i < members.size(); ++i) {
    const auto m = members[i];
    if (kinds[i] == Kind::DimAnchor) {
      const auto fromPos = arena.asInt64(members[i - 1]);
      const auto dirPos  = arena.asInt64(members[i + 1]);
      const auto toPos   = arena.asInt64(members[i + 2]);
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
    const auto pos = arena.asInt64(m);
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
      const auto referent = arena.asInt64(*slot);
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
    const auto copy = arena.makeCell();
    zigzag::expectWritten(
        arena.link(copy, binds_.dims.ref, Posward, mapped(*target)));
    if (name) {
      zigzag::expectWritten(arena.link(copy, dims.name, Posward,
                                       arena.makeCell(arena.textOf(*name))));
      if (const auto role = arena.linked(rec, dims.grab, Posward))
        zigzag::expectWritten(arena.link(copy, dims.grab, Posward,
                                         arena.makeCell(arena.textOf(*role))));
    } else {
      zigzag::expectWritten(arena.link(mapped(*target),
                                       binds_.dims.meshRelocation, Posward,
                                       arena.makeCell("cell")));
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
      const auto pos = arena.asInt64(m);
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

} // namespace xanadu::vagent

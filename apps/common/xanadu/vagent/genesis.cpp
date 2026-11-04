/**
 * @file genesis.cpp
 * @brief The genesis `(a)`–`(e)` ceremony — see genesis.hpp for the
 *        ordering contract. Every arm reads through the fresh context's
 *        arena (base fall-through) and writes ordinary trailed arena
 *        ops; nothing here mints `d.patch` records or draws `patch:<n>`
 *        serials.
 */
#include "common/xanadu/vagent/genesis.hpp"

#include <bit>
#include <format>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common/xanadu/compact_op.hpp"
#include "common/xanadu/publication.hpp"
#include "common/xanadu/scroll.hpp"
#include "common/xanadu/spool.hpp"
#include "common/xanadu/vagent/lineage.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace xanadu::vagent {
namespace {

using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::Negward;
using zigzag::Posward;

// ---------------------------------------------------------------------------
// Rank/record helpers — meta.cpp's private shapes, needed here at ceremony
// distance: same structure, local copies (meta.cpp is a sibling TU's lane).
// ---------------------------------------------------------------------------

/// The `+d.step`-linked chain that follows a single lead edge — the
/// `d.refs`/`d.recipe`/`d.patch-head` shape.
[[nodiscard]] std::vector<CellRef> stepChain(zigzag::ArenaManifold &arena,
                                             CellRef origin, DimRef leadDim,
                                             DimRef step) {
  std::vector<CellRef> out;
  for (auto cell = arena.linked(origin, leadDim, Posward);
       cell && arena.contains(*cell); cell = arena.linked(*cell, step, Posward))
    out.push_back(*cell);
  return out;
}

/// Unlink @p cell off a same-dim rank with predecessor→successor repair
/// (the `prune`/`unlearn` unlink idiom).
void unlinkRank(zigzag::ArenaManifold &arena, CellRef cell, DimRef dim) {
  const auto pred = arena.linked(cell, dim, Negward);
  const auto succ = arena.linked(cell, dim, Posward);
  if (succ) zigzag::expectWritten(arena.unlink(cell, dim, Posward));
  if (pred) {
    if (succ)
      zigzag::expectWritten(arena.link(*pred, dim, Posward, succ));
    else
      zigzag::expectWritten(arena.unlink(*pred, dim, Posward));
  }
}

/// Mint or find the `+d.vars` record keyed @p key on @p owner.
[[nodiscard]] std::optional<CellRef> varsRecord(zigzag::ArenaManifold &arena,
                                                VortexCore &core, CellRef owner,
                                                std::string_view key,
                                                bool mint) {
  const auto vars = core.dims().vars;
  for (const auto record : zigzag::rankAfter(arena, owner, vars))
    if (arena.textOf(record) == key) return record;
  if (!mint) return std::nullopt;
  const auto record = arena.makeCell(key);
  zigzag::expectWritten(
      arena.link(zigzag::rankTail(arena, owner, vars), vars, Posward, record));
  return record;
}

/// A patch record's field text/int: `descriptor` selects the `+d.values`
/// descriptor's `d.vars` chain (target/verb/parent), false selects the
/// patch cell's own (ordinal/alias/priority).
[[nodiscard]] std::optional<std::string>
patchField(zigzag::ArenaManifold &arena, VortexCore &core, CellRef patch,
           std::string_view key, bool descriptor) {
  const auto owner = descriptor
                         ? arena.linked(patch, core.dims().values, Posward)
                         : std::optional{patch};
  if (!owner || !arena.contains(*owner)) return std::nullopt;
  for (const auto record : zigzag::rankAfter(arena, *owner, core.dims().vars)) {
    if (arena.textOf(record) == key) {
      const auto value = arena.linked(record, core.dims().values, Posward);
      if (!value) return std::nullopt;
      if (const auto number = arena.asInt64(*value))
        return std::to_string(*number);
      return arena.textOf(*value);
    }
  }
  return std::nullopt;
}

/// The `+d.patch-head` chain's recorded head names, in order.
[[nodiscard]] std::vector<std::string>
patchHeadNames(zigzag::ArenaManifold &arena, VortexCore &core, CellRef patch,
               DimRef patchHead) {
  std::vector<std::string> names;
  for (const auto record :
       stepChain(arena, patch, patchHead, core.dims().step)) {
    const auto name = arena.linked(record, core.dims().values, Posward);
    names.emplace_back(name ? arena.textOf(*name) : std::string{});
  }
  return names;
}

/// `ensureDimension`'s resolution with meta.cpp's two-table arm: the
/// core's `d.dims` rank first (where rank-filed names live), then the
/// arena's `arenaDims_`/base surface — minting through the arena only
/// under the no-marks rule.
[[nodiscard]] std::expected<DimRef, std::string>
resolveDimension(zigzag::ArenaManifold &arena, VortexCore &core,
                 std::string_view name) {
  if (const auto dim = core.findDimension(name)) return *dim;
  if (const auto dim = arena.dimensionNamed(name)) return *dim;
  if (arena.outstandingMarks() != 0)
    return std::unexpected(
        std::format("genesis: dimension '{}' is not filed and cannot be "
                    "minted under live marks",
                    name));
  return arena.ensureDimension(name);
}

/// `writeInt` — an int64 value write leaving content alone.
void writeInt(zigzag::ArenaManifold &arena, CellRef cell, std::int64_t value) {
  zigzag::expectWritten(arena.setValueBits(
      cell, xanadu::ValueKind::Int64, std::bit_cast<std::uint64_t>(value)));
}

// ---------------------------------------------------------------------------
// (a) — the pairwise band comparand
// ---------------------------------------------------------------------------

/// Scroll identity by global key: `globalKeyOf`'s arm set minus
/// `localScroll` — a break marker names itself, a local span names the
/// (same-machine) local spool, an engaged external its `scrollKey`, a
/// fixed reserved id its own constant — and anything else is a
/// disengaged id, which fails outright.
[[nodiscard]] std::optional<std::string>
scrollIdentityOf(const xanadu::Store &store, const xanadu::ScrollId id) {
  if (id == xanadu::breakMarkerScroll)
    return std::string{xanadu::breakMarkerKey};
  if (id == xanadu::localScroll) return std::string{"local:"};
  if (xanadu::isReservedScroll(id)) return std::format("reserved:{}", id);
  const auto scroll = store.scroll(id);
  if (!scroll) return std::nullopt;
  return xanadu::scrollKey(*scroll);
}

/// Every non-scroll field of a `CompactOpNode`, verbatim. The node's
/// own `==` would drag `scrollId` into the comparison; identity travels
/// through scrollIdentityOf instead.
[[nodiscard]] bool sameNode(const xanadu::CompactOpNode &a,
                            const xanadu::CompactOpNode &b) noexcept {
  return a.parentIndex == b.parentIndex && a.kind == b.kind &&
         a.flags == b.flags && a.branchOrdinal == b.branchOrdinal &&
         a.at == b.at && a.length == b.length && a.to == b.to &&
         a.sourceAt == b.sourceAt && a.sourceLength == b.sourceLength &&
         a.sourceOpIndex == b.sourceOpIndex && a.linkId == b.linkId &&
         a.spanStart == b.spanStart && a.spanLength == b.spanLength &&
         a.value == b.value;
}

} // namespace

std::expected<void, std::string>
verifyLineageBand(const xanadu::Store &lineage, const xanadu::Store &received,
                  const std::uint32_t extent) {
  const auto &kept     = lineage.segmentedOps();
  const auto &incoming = received.segmentedOps();
  if (incoming.size() < extent)
    return std::unexpected(
        std::format("genesis (a): received band holds {} ops, fewer than the "
                    "recorded image extent {} — a prefix-less lineage refuses",
                    incoming.size(), extent));
  for (std::uint32_t i = 1; i <= extent; ++i) {
    const auto *keptNode = kept.get(i);
    const auto *node     = incoming.get(i);
    if (keptNode == nullptr || node == nullptr)
      return std::unexpected(std::format(
          "genesis (a): position {} is absent from a band spool", i));
    // The op's own name first: a permutation moves records, which moves
    // produces-ids off their positions.
    if (kept.idOf(i) != incoming.idOf(i))
      return std::unexpected(std::format(
          "genesis (a): position {} produces a different state — the "
          "received band is permuted or foreign",
          i));
    if (!sameNode(*keptNode, *node))
      return std::unexpected(std::format(
          "genesis (a): operation {} differs from the recorded lineage "
          "band",
          i));
    const auto keptScroll = scrollIdentityOf(lineage, keptNode->scrollId);
    const auto scroll     = scrollIdentityOf(received, node->scrollId);
    if (!keptScroll || !scroll)
      return std::unexpected(std::format(
          "genesis (a): operation {} names a scroll its store never "
          "recorded",
          i));
    if (*keptScroll != *scroll)
      return std::unexpected(
          std::format("genesis (a): operation {} names scroll '{}' where the "
                      "recorded band names '{}'",
                      i, *scroll, *keptScroll));
  }
  return {};
}

namespace {

// ---------------------------------------------------------------------------
// (b) — dim-name sweep + P15 re-registration
// ---------------------------------------------------------------------------

/// `ensureDimension` over one name; reports whether the mint arm ran —
/// a filed name resolves, an unfiled one mints the one arena-side twin
/// per cycle the spec allows (filing onto the kept store is the
/// plumbing's pre-fold job).
[[nodiscard]] std::expected<bool, std::string>
sweepDimName(zigzag::ArenaManifold &arena, VortexCore &core,
             std::string_view name) {
  const auto filed = core.findDimension(name).has_value() ||
                     arena.dimensionNamed(name).has_value();
  const auto dim   = resolveDimension(arena, core, name);
  if (!dim) return std::unexpected(dim.error());
  return !filed;
}

/// The dim *name* surface: the sibling's `dimensionVocabulary` — the
/// 19 genesis names, the 12 apparatus dims, the 13 V10 `std:agent`
/// dims and every `dimensions()` member's content text — plus every
/// `+d.name` a `+d.recipe` member of a registered head carries (the
/// `dimConstant` names the image records).
[[nodiscard]] std::expected<std::pair<std::size_t, std::vector<std::string>>,
                            std::string>
sweepDimensionVocabulary(zigzag::ArenaManifold &arena, VortexCore &core,
                         CellRef agentCell, DimRef codeDim, DimRef recipeDim) {
  auto names      = dimensionVocabulary(arena);
  const auto step = core.dims().step;
  const auto name = core.dims().name;
  for (const auto head : zigzag::rankAfter(arena, agentCell, codeDim))
    for (const auto member : stepChain(arena, head, recipeDim, step))
      if (const auto nameCell = arena.linked(member, name, Posward))
        if (auto text = arena.textOf(*nameCell); !text.empty())
          names.insert(std::move(text));

  std::size_t resolved = 0;
  std::vector<std::string> minted;
  for (const auto &name : names) {
    const auto fresh = sweepDimName(arena, core, name);
    if (!fresh) return std::unexpected(fresh.error());
    if (*fresh)
      minted.push_back(name);
    else
      ++resolved;
  }
  return std::pair{resolved, std::move(minted)};
}

// ---------------------------------------------------------------------------
// (d) — the ordinal d.patch replay
// ---------------------------------------------------------------------------

/// `clauseHost`'s shape: first-`:` split when the qualifier resolves as
/// a module, else the bare name on the agent's own module's `d.vars`.
[[nodiscard]] std::expected<CellRef, std::string>
clauseHost(MetaRuntime &meta, zigzag::ArenaManifold &arena,
           const MetaBindSet &binds, std::string_view name) {
  std::optional<CellRef> module;
  std::string_view symbol = name;
  if (const auto colon = name.find(':'); colon != std::string_view::npos) {
    module = meta.moduleNamed(name.substr(0, colon));
    if (module) symbol = name.substr(colon + 1);
  }
  if (!module) {
    if (binds.ownModule == noCell || !arena.contains(binds.ownModule))
      return std::unexpected(std::format(
          "replay host '{}': no own module bound for a bare name", name));
    module = binds.ownModule;
  }
  for (const auto candidate :
       zigzag::rankAfter(arena, *module, meta.core().dims().vars))
    if (arena.textOf(candidate) == symbol) return candidate;
  return std::unexpected(std::format(
      "replay host '{}': no such symbol on the resolved module", name));
}

/// Drop a dead patch: unlink off the `d.patch` rank and record a
/// `fault:<label>` `d.vars` record on the agent — the tombstone a
/// resurrected tail-record consults before re-running.
void dropPatch(zigzag::ArenaManifold &arena, VortexCore &core,
               const MetaBindSet &binds, CellRef patch, std::string_view label,
               std::string_view why) {
  unlinkRank(arena, patch, binds.dims.patch);
  const std::string key = std::format("fault:{}", label);
  if (const auto rec =
          varsRecord(arena, core, binds.agentCell, key, /*mint=*/true))
    zigzag::expectWritten(
        arena.link(*rec, core.dims().values, Posward, arena.makeCell(why)));
}

/// One patch record's re-enactment. Install copies land in `installed`
/// for (e). Errors return as strings so the caller drops the record —
/// a dead patch is compaction, never a genesis refusal.
[[nodiscard]] std::expected<void, std::string>
replayPatch(MetaRuntime &meta, zigzag::ArenaManifold &arena,
            const MetaBindSet &binds, CellRef patch,
            std::vector<CellRef> &installed) {
  const auto &dims = meta.core().dims();
  const auto verb =
      patchField(arena, meta.core(), patch, "verb", true).value_or("");
  const auto target =
      patchField(arena, meta.core(), patch, "target", true).value_or("");
  const auto parent = patchField(arena, meta.core(), patch, "parent", true);
  const auto heads =
      patchHeadNames(arena, meta.core(), patch, binds.dims.patchHead);
  const auto label = meta.labelOf(patch).value_or("");

  const auto reifyNamed =
      [&](std::size_t index) -> std::expected<CellRef, std::string> {
    if (index >= heads.size())
      return std::unexpected(std::format(
          "patch {}: no recorded head name at index {}", label, index));
    const auto head = meta.codeNamed(heads[index]);
    if (!head)
      return std::unexpected(std::format(
          "patch {}: recorded head '{}' no longer resolves on d.code", label,
          heads[index]));
    auto copy = meta.reify(*head);
    if (!copy)
      return std::unexpected(std::format("patch {}: {}", label, copy.error()));
    installed.push_back(*copy);
    return *copy;
  };

  if (verb == "learn_plan" || verb == "learn_handler") {
    if (!parent || parent->empty())
      return std::unexpected(std::format(
          "patch {}: {} record carries no parent host", label, verb));
    const auto host = clauseHost(meta, arena, binds, *parent);
    if (!host) return std::unexpected(host.error());
    if (heads.size() < 2)
      return std::unexpected(std::format(
          "patch {}: {} record names fewer than two heads", label, verb));
    // learn_plan's verbatim alias re-match: the folded rank's declared
    // clause carrying the alias unlinks again on regeneration — the
    // durable replacement the record describes.
    if (verb == "learn_plan")
      if (const auto alias =
              patchField(arena, meta.core(), patch, "alias", false))
        for (const auto clause : meta.clausesOf(*host))
          if (meta.labelOf(clause).value_or("") == *alias)
            unlinkRank(arena, clause, dims.clause);
    const auto test = reifyNamed(0);
    if (!test) return std::unexpected(test.error());
    const auto body = reifyNamed(1);
    if (!body) return std::unexpected(body.error());
    const auto clause = arena.makeCell();
    // The patch:<n> label is a verbatim re-mint — a fresh label cell
    // written with the record's text, never a re-link of the record's
    // own label cell (its -d.name occupancy would evict and drop the
    // label at the second genesis).
    zigzag::expectWritten(
        arena.link(clause, dims.name, Posward, arena.makeCell(label)));
    zigzag::expectWritten(arena.link(clause, dims.grab, Posward, *test));
    zigzag::expectWritten(arena.link(clause, dims.spin, Posward, *body));
    if (verb == "learn_handler")
      if (const auto priority =
              patchField(arena, meta.core(), patch, "priority", false))
        if (const auto rec = varsRecord(arena, meta.core(), clause, "priority",
                                        /*mint=*/true))
          zigzag::expectWritten(
              arena.link(*rec, dims.values, Posward,
                         arena.makeScalarCell(static_cast<std::int64_t>(
                             std::stoll(*priority)))));
    zigzag::expectWritten(
        arena.link(zigzag::rankTail(arena, *host, dims.clause), dims.clause,
                   Posward, clause));
    return {};
  }

  if (verb == "learn_where" || verb == "learn_body") {
    const auto clause = meta.planNamed(target);
    if (!clause)
      return std::unexpected(
          std::format("patch {}: the patched clause '{}' no longer resolves",
                      label, target));
    const auto copy = reifyNamed(0);
    if (!copy) return std::unexpected(copy.error());
    // The re-point: no prior head is needed — a program-original
    // attachment was never recorded, and the fold restored it.
    zigzag::expectWritten(
        arena.link(*clause, verb == "learn_where" ? dims.grab : dims.spin,
                   Posward, *copy));
    return {};
  }

  if (verb == "learn_method") {
    const auto colon = target.find(':');
    if (colon == std::string::npos)
      return std::unexpected(
          std::format("patch {}: malformed method target '{}'", label, target));
    const auto moduleName = target.substr(0, colon);
    const auto qualified  = target.substr(colon + 1);
    const auto module     = meta.moduleNamed(moduleName);
    if (!module)
      return std::unexpected(std::format(
          "patch {}: module '{}' no longer resolves", label, moduleName));
    // graft_method's unlink-first rides the same verb: a folded symbol
    // carrying the qualified spelling leaves the rank before the fresh
    // one appends.
    for (const auto symbol : zigzag::rankAfter(arena, *module, dims.vars))
      if (arena.textOf(symbol) == qualified) {
        unlinkRank(arena, symbol, dims.vars);
        break;
      }
    const auto copy = reifyNamed(0);
    if (!copy) return std::unexpected(copy.error());
    const auto symbol = arena.makeCell(qualified);
    zigzag::expectWritten(arena.link(symbol, dims.values, Posward, *copy));
    zigzag::expectWritten(
        arena.link(zigzag::rankTail(arena, *module, dims.vars), dims.vars,
                   Posward, symbol));
    return {};
  }

  return std::unexpected(std::format(
      "patch {}: record carries no replayable verb '{}'", label, verb));
}

// ---------------------------------------------------------------------------
// (e) — the +d.refs rehydration sweep
// ---------------------------------------------------------------------------

/// The dim-record arm over one head's `+d.recipe`: every `+d.name`-
/// bearing member is a dim referent — ref-capture records write
/// `Int64(DimRef)` into the `+d.refFor`-paired slot, dim-field members
/// into the member cell itself. `dimHandled` collects the cells this
/// arm owns so the ordinary ref-record arm leaves them alone.
[[nodiscard]] std::expected<std::size_t, std::string>
rebindDimRecords(MetaRuntime &meta, zigzag::ArenaManifold &arena, CellRef head,
                 DimRef recipeDim, std::set<CellRef> &dimHandled) {
  std::size_t count = 0;
  const auto step   = meta.core().dims().step;
  const auto name   = meta.core().dims().name;
  for (const auto member : stepChain(arena, head, recipeDim, step)) {
    const auto nameCell = arena.linked(member, name, Posward);
    if (!nameCell || !arena.contains(*nameCell)) continue;
    const auto dim =
        resolveDimension(arena, meta.core(), arena.textOf(*nameCell));
    if (!dim) return std::unexpected(dim.error());
    if (const auto slot =
            arena.linked(member, meta.binds().dims.refFor, Posward)) {
      writeInt(arena, *slot, static_cast<std::int64_t>(*dim));
      dimHandled.insert(*slot);
    } else {
      writeInt(arena, member, static_cast<std::int64_t>(*dim));
      dimHandled.insert(member);
    }
    ++count;
  }
  return count;
}

/// One head's `+d.refs` records: name-records rebind by role, ordinary
/// ref-records stay kind-guarded.
[[nodiscard]] std::expected<std::pair<std::size_t, std::size_t>, std::string>
rebindRefs(MetaRuntime &meta, zigzag::ArenaManifold &arena, CellRef head,
           const std::set<CellRef> &dimHandled,
           std::vector<std::string> &faults) {
  const auto &dims   = meta.core().dims();
  const auto refsDim = meta.binds().dims.refs;
  const auto refDim  = meta.binds().dims.ref;
  std::size_t names  = 0;
  std::size_t refs   = 0;
  for (const auto record : stepChain(arena, head, refsDim, dims.step)) {
    const auto marked = arena.linked(record, refDim, Posward);
    if (!marked || !arena.contains(*marked))
      return std::unexpected("genesis (e): +d.refs record has no +d.ref cell");
    if (dimHandled.contains(*marked)) continue;
    const auto nameCell = arena.linked(record, dims.name, Posward);
    if (nameCell) {
      const auto label    = arena.textOf(*nameCell);
      const auto roleCell = arena.linked(record, dims.grab, Posward);
      const auto role     = roleCell ? arena.textOf(*roleCell) : std::string{};
      std::optional<CellRef> endpoint;
      if (role == "call") {
        // module:Owner.name → module d.vars → +d.values → the
        // op-or-head hop: a rebind that only resolves *after* (d)'s
        // installs, which is why (e) runs last.
        if (const auto colon = label.find(':'); colon != std::string::npos)
          if (const auto symbol = meta.symbolNamed(label.substr(0, colon),
                                                   label.substr(colon + 1)))
            if (const auto target = arena.linked(*symbol, dims.values, Posward))
              endpoint = meta.graphRoot(*target);
      } else {
        // run-role and ordinary name-records rebind through the
        // image/d.code label walk — Int64(head) verbatim.
        endpoint = meta.codeNamed(label);
      }
      if (endpoint) {
        writeInt(arena, *marked, static_cast<std::int64_t>(*endpoint));
        ++names;
      } else {
        faults.emplace_back(std::format(
            "genesis (e): {}-role name-record '{}' unresolved — the "
            "emission-fixed slot stays unwritten",
            role.empty() ? "plain" : role, label));
      }
      continue;
    }
    // The ordinary ref-record arm — kind-guarded: OpHandle-kind donor
    // slots take Int64(*handleTarget); emission-fixed Int64 slots stay
    // verbatim (the checkpoint re-kind is P16's, and palette-minted
    // copy slots arrive Int64).
    switch (arena.valueKindOf(*marked)) {
    case xanadu::ValueKind::OpHandle:
      if (const auto target = arena.handleTarget(*marked)) {
        writeInt(arena, *marked, static_cast<std::int64_t>(*target));
        ++refs;
      }
      break;
    case xanadu::ValueKind::Int64:
      break;
    default:
      faults.emplace_back(
          "genesis (e): ref-record slot carries a non-position kind — "
          "left verbatim");
      break;
    }
  }
  return std::pair{names, refs};
}

} // namespace

// ---------------------------------------------------------------------------
// The ceremony
// ---------------------------------------------------------------------------

std::expected<GenesisOutcome, std::string>
runGenesis(const xanadu::Store &lineage, const xanadu::Store &received,
           zigzag::ArenaManifold &arena, VortexCore &core, VortexVM &vm,
           const GenesisRoots &roots) {
  // (a) — lineage bind: the pairwise band comparand runs before any
  // root resolves; a permuted, foreign or prefix-less received band
  // fails closed.
  if (roots.imageExtent == 0)
    return std::unexpected(
        "genesis: the roots record names no image extent — nothing to "
        "bind a lineage against");
  if (const auto ok = verifyLineageBand(lineage, received, roots.imageExtent);
      !ok)
    return std::unexpected(ok.error());

  GenesisOutcome out;
  out.opsCompared = roots.imageExtent;

  // Root sanity before (b): a root naming no cell of the received fold
  // is a bind refusal — every later arm resolves through it.
  const auto requireRoot =
      [&](CellRef ref,
          std::string_view which) -> std::expected<void, std::string> {
    if (ref == noCell || !arena.contains(ref))
      return std::unexpected(
          std::format("genesis (c): {} names no cell of the received "
                      "manifold",
                      which));
    return {};
  };
  if (const auto ok = requireRoot(roots.programRoot, "program root"); !ok)
    return std::unexpected(ok.error());
  if (const auto ok = requireRoot(roots.agentCell, "agent cell"); !ok)
    return std::unexpected(ok.error());
  if (const auto ok = requireRoot(roots.imageRecord, "image record"); !ok)
    return std::unexpected(ok.error());
  // programRef is an image-side ref: a persistent position inside the
  // band — an ephemeral ref or one past the extent is no lineage.
  if (zigzag::isEphemeral(roots.programRoot) ||
      roots.programRoot > roots.imageExtent)
    return std::unexpected(
        "genesis (c): program root is not an image-side persistent ref");

  // (b) — dims then opcodes: the sweep's name surface covers the fixed
  // vocabulary, the fold's filed dims, and the dim names the registered
  // heads' recipes record; P15's registration reads the recorded
  // instruction enumeration, never a scan.
  const auto codeDim   = resolveDimension(arena, core, "d.code");
  const auto recipeDim = resolveDimension(arena, core, "d.recipe");
  if (!codeDim || !recipeDim)
    return std::unexpected(codeDim ? recipeDim.error() : codeDim.error());
  auto dims = sweepDimensionVocabulary(arena, core, roots.agentCell, *codeDim,
                                       *recipeDim);
  if (!dims) return std::unexpected(dims.error());
  out.dimsResolved      = dims->first;
  out.dimsMinted        = std::move(dims->second);
  const auto registered = vm.registerProgramImage(roots.imageRecord);
  if (!registered)
    return std::unexpected(std::format("genesis (b): {}", registered.error()));
  out.opsRegistered = *registered;

  // (c) — the bind set: minted through the V10/no-marks mechanism, the
  // lineage record bound beside.
  auto binds = mintMetaBindSet(arena, core, roots.programRoot, roots.agentCell);
  if (!binds)
    return std::unexpected(std::format("genesis (c): {}", binds.error()));
  binds->ownModule = roots.ownModule;
  binds->lineage   = MetaLineage{
      .programRef  = roots.programRoot,
      .imageExtent = roots.imageExtent,
  };
  out.binds = *binds;

  MetaRuntime meta(core, vm, *binds);

  // (d) — the ordinal d.patch replay: rank order is append order, and
  // the recorded ordinal is the tie-break authority. Replay mints no
  // patch records and draws no patch:<n> serial.
  std::vector<std::pair<CellRef, std::int64_t>> queue;
  {
    std::int64_t index = 0;
    for (const auto patch :
         zigzag::rankAfter(arena, roots.agentCell, binds->dims.patch)) {
      std::int64_t ordinal = index;
      if (const auto field = patchField(arena, core, patch, "ordinal", false))
        ordinal = std::stoll(*field);
      queue.emplace_back(patch, ordinal);
      ++index;
    }
  }
  std::ranges::stable_sort(queue, {},
                           &std::pair<CellRef, std::int64_t>::second);
  for (const auto &[patch, ordinal] : queue) {
    const auto label = meta.labelOf(patch).value_or("");
    // The tombstone arm: a record already dropped once (its fault
    // record persists) stays out of replay — a resurrected tail-record
    // never re-runs.
    if (!label.empty()) {
      const std::string faultKey = std::format("fault:{}", label);
      if (varsRecord(arena, core, roots.agentCell, faultKey, /*mint=*/false)
              .has_value())
        continue;
    }
    if (const auto ok =
            replayPatch(meta, arena, *binds, patch, out.installedCopies);
        !ok) {
      dropPatch(arena, core, *binds, patch, label, ok.error());
      ++out.patchesDropped;
      out.faults.emplace_back(ok.error());
    } else {
      ++out.patchesReplayed;
    }
  }

  // (e) — last: the +d.refs rehydration over registered heads *and*
  // (d)'s install set — the copies hang off program-side anchors a
  // head-only sweep never reaches.
  std::vector<CellRef> sweep;
  for (const auto head :
       zigzag::rankAfter(arena, roots.agentCell, binds->dims.code))
    sweep.push_back(head);
  for (const auto copy : out.installedCopies)
    if (std::ranges::find(sweep, copy) == sweep.end()) sweep.push_back(copy);
  for (const auto head : sweep) {
    std::set<CellRef> dimHandled;
    const auto dimsDone =
        rebindDimRecords(meta, arena, head, binds->dims.recipe, dimHandled);
    if (!dimsDone) return std::unexpected(dimsDone.error());
    out.dimRecordsRebound += *dimsDone;
    const auto refs = rebindRefs(meta, arena, head, dimHandled, out.faults);
    if (!refs) return std::unexpected(refs.error());
    out.nameRecordsRebound += refs->first;
    out.refRecordsRebound += refs->second;
  }

  return out;
}

} // namespace xanadu::vagent

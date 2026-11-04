/**
 * @file genesis.hpp
 * @brief The genesis `(a)`–`(e)` ceremony driver (phase 5 of
 *        design/vagent-metaprogramming-implementation.md :421-479;
 *        spec §6.5, §9).
 *
 * `runGenesis` is the priced `std:agent` genesis hook the spec hangs on
 * the fresh-context image bind: given the kept per-agent lineage store,
 * the received lineage it claims to continue, and a *fresh* execution
 * bundle (arena overlaying `rebuildManifold`'s fold — the sibling's
 * `regenesisCycle` produces it, plus a `VortexCore`/`VortexVM` whose
 * opcode map is empty), it runs, strictly in order:
 *
 *  - `(a)` the lineage bind — a *pairwise* decoded-`OpRecord` comparand
 *    over the image band `1..E_ops`: every non-scroll node field
 *    verbatim, scroll identity by global key (`globalKeyOf`'s arm set
 *    minus `localScroll` — `break:` for sealed break-marker spans,
 *    `scrollKey` for engaged externals, and any disengaged id fails
 *    outright). Permutations, merged foreign lineages and missing
 *    prefixes refuse before anything else runs — a positional max-cell
 *    walk is not a lineage test.
 *  - `(b)` the `ensureDimension` find-or-mint sweep over the dim *name*
 *    surface (filed name → canonical cell, unfiled → one arena mint)
 *    plus P15's opcode re-registration off the recorded instruction
 *    enumeration (`VortexVM::registerProgramImage`), never a value
 *    scan. Filing onto the kept store's `d.dims` rank is the plumbing's
 *    pre-fold job — by the time this runs the base is already pinned.
 *  - `(c)` the bind set — `mintMetaBindSet` over the bound roots plus
 *    the lineage record `{programRef, E_ops}`.
 *  - `(d)` `d.patch` replay in ordinal order — verb re-enactment for
 *    `learn_plan`/`learn_handler`/`learn_where`/`learn_body`/
 *    `learn_method`, minting no patch records and drawing no `patch:<n>`
 *    serials (each install copy's own `reify:<src>:<serial>` still
 *    draws). `patch:<n>` labels are verbatim *re-mints*, never re-links
 *    of the record's label cell. A patch whose target or heads no
 *    longer resolve drops with a `fault:<label>` `d.vars` record —
 *    the tombstone that keeps a resurrected tail-record from re-running.
 *  - `(e)` `+d.refs` rehydration last: name-records rebind by role
 *    (`call` → module `d.vars` → `+d.values` → op-or-head hop →
 *    `Int64(entryOp)`; `run`/other → the image/d.code label walk →
 *    `Int64(head)`); `+d.name`-bearing `+d.recipe` members write
 *    `Int64(ensureDimension(name))` into the `+d.refFor`-paired slot or
 *    the member itself; ordinary ref-records stay kind-guarded —
 *    `OpHandle` slots take `Int64(*handleTarget)`, `Int64` slots are
 *    emission-fixed and stay verbatim (the P16 checkpoint re-kind that
 *    makes donors arrive handle-kind is phase 6).
 *
 * Ordering is the contract: (b) precedes (c) because the bind set's
 * dims resolve through the sweep; (e) follows (d) because name-records
 * may bind to symbols the replay installs. Phase 6 wires the real
 * spawn/resume transport paths; this phase stays kernel-simulable —
 * stores are local objects and the fresh context arrives by argument.
 */
#ifndef COMMON_XANADU_VAGENT_GENESIS_HPP
#define COMMON_XANADU_VAGENT_GENESIS_HPP

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/vagent/meta.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vagent {

using zigzag::CellRef;
using zigzag::noCell;
using zigzag::vortex::VortexCore;
using zigzag::vortex::VortexVM;

/// The roots (c) binds — the adapter-bound non-dim members. Every ref
/// is a persistent position on the received fold: `programRoot` is the
/// lineage record's `programRef` (≤ `imageExtent`), `agentCell` the
/// durable-state root whose ranks carry `d.code`/`d.patch`/`d.vars`,
/// `ownModule` the agent's own module, `imageRecord` the phase-3
/// `finalizeProgramImage` cell whose `"ops"` record enumerates the
/// program's instruction positions for (b).
struct GenesisRoots {
  CellRef programRoot{noCell};
  CellRef agentCell{noCell};
  CellRef ownModule{noCell};
  CellRef imageRecord{noCell};
  /// `E_ops` — the image band's op extent (the `MetaLineage::imageExtent`
  /// the bind set's `≤` classification compares against). Absent names no
  /// program image — genesis refuses to bind a lineage without one.
  std::optional<std::uint32_t> imageExtent;
};

/// Per-arm tallies and the reconstructed bind set. `installedCopies`
/// is the install set (d) hands (e) — the replay-minted attach copies
/// hang off program-side anchors no registered-head-only sweep reaches.
struct GenesisOutcome {
  MetaBindSet binds{};
  // (a): band positions compared pairwise — `imageExtent` on success.
  std::size_t opsCompared{0};
  // (b): name-surface resolutions vs arena-side mints.
  std::size_t dimsResolved{0};
  std::vector<std::string> dimsMinted;
  std::size_t opsRegistered{0};
  // (d): ordinal-order replay vs fault-dropped records.
  std::size_t patchesReplayed{0};
  std::size_t patchesDropped{0};
  std::vector<CellRef> installedCopies;
  // (e): the three sweep arms.
  std::size_t nameRecordsRebound{0};
  std::size_t dimRecordsRebound{0};
  std::size_t refRecordsRebound{0};
  /// Fault strings: (d) drops and (e) unwritten slots. A genesis that
  /// replays cleanly leaves this empty; faults here are records of
  /// compaction, never refusals.
  std::vector<std::string> faults;
};

/// The `(a)` comparand alone, exposed for the plumbing's "is this the
/// lineage I was seeded with" question: pairwise over positions
/// `1..extent` of both ops spools — `produces` names, every non-scroll
/// node field verbatim, scroll identity by global key. A `received`
/// band shorter than `extent` is a missing-prefix refusal.
[[nodiscard]] std::expected<void, std::string>
verifyLineageBand(const xanadu::Store &lineage, const xanadu::Store &received,
                  std::uint32_t extent);

/// The full `(a)`–`(e)` ceremony over a fresh context. `arena` must
/// overlay the fold of `received`'s current head (the plumbing's
/// `rebuildManifold` product) — roots, records and ranks resolve
/// through it. `lineage` is the kept store whose canonical spool the
/// received band compares against; a same-store cycle passes `lineage`
/// in both slots. Refusals fail closed before (c): nothing after (a)
/// runs when the band does not verify.
[[nodiscard]] std::expected<GenesisOutcome, std::string>
runGenesis(const xanadu::Store &lineage, const xanadu::Store &received,
           zigzag::ArenaManifold &arena, VortexCore &core, VortexVM &vm,
           const GenesisRoots &roots);

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_GENESIS_HPP

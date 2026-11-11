#ifndef COMMON_XANADU_VAGENT_LINEAGE_HPP
#define COMMON_XANADU_VAGENT_LINEAGE_HPP

// Phase 5 plumbing: the per-agent lineage Store, the dimension-filing
// sweep, and the promote → rebuildManifold → fresh-context re-genesis
// cycle (design/vagent-metaprogramming-implementation.md lines 374-524).
// genesis.{hpp,cpp} consumes this surface for its (a)-(e) ceremony.
//
// CONTRACT NOTE: this header is the agreed seam between two work items
// landed in parallel. Signatures are settled as listed; extend only.

#include <expected>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/worker_context.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace xanadu::vagent {

using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::Manifold;
using zigzag::noCell;
using zigzag::Promoted;
using zigzag::vortex::WorkerContext;

// ---------------------------------------------------------------------------
// The fixed filing vocabulary — the names the sweep ensures even when no live
// arena carries them yet. Filing is name-table entries, not cells, so these
// are spellings, never DimRefs.
// ---------------------------------------------------------------------------

/// The genesis names: "d.dims" (the rank's own dim — `home` beside it is a
/// plain genesis cell, not a dimension) plus the nineteen system dims
/// `VortexCore::initGenesis` mints (vortex_core.cpp:51-74) and `attach()`'s
/// "d.store-refs" (arena_manifold.cpp:289-290). Twenty-one cells, twenty
/// filed names, plus the one attach() adds.
inline constexpr std::string_view kGenesisDimensionNames[] = {
    "d.dims",   "d.grab",     "d.step",       "d.spin",
    "d.stack",  "d.contract", "d.clone",      "d.cursors",
    "d.cache",  "d.vars",     "d.values",     "d.pinning-cursors",
    "d.name",   "d.stdlib",   "d.clause",     "d.scan",
    "d.stores", "d.role",     "d.store-refs", "d.left",
    "d.right",
};

/// The twelve apparatus dims §6.1's bind set carries — filed on the
/// persistent `d.dims` rank like every other minted name. The bind set's
/// bound non-dim members (program root, agent cell, `validate` constants)
/// bind at genesis (c) and never file.
inline constexpr std::string_view kMetaDimensionNames[] = {
    "d.code",    "d.patch",           "d.weight",      "d.synapse",
    "d.entry",   "d.patch-head",      "d.validate",    "d.refs",
    "d.ref",     "d.recipe",          "d.refFor",      "d.mesh.relocation",
    "d.lineage", "d.kind-registered", "d.pending-reg", "d.ast.kinds",
};

/// The twenty-three `std:agent` dims the adapter `ensureDimension`-mints
/// (vagent-agent-language.md V10, actor-executor §8's M0 set) — the V10
/// sweep precedes filing, or filing performs it. The executor's six
/// (d.agents/d.dirs/d.seen/d.evidence/d.verb/d.holder) file here so
/// ensureDimension never mints them under a live mark — resolveDimension
/// refuses unfiled names there (actor-executor §3.1). d.owner scopes
/// varsRecord's records to their cell, d.inbox/d.delivered carry the
/// adapter's dispatch and dedupe ranks — all three must file with the
/// vocabulary or a session mint forks the fold's resolution into a
/// promoted twin nothing else can read.
inline constexpr std::string_view kAgentDimensionNames[] = {
    "d.behavior", "d.skill",     "d.capacity", "d.space",  "d.member",
    "d.holon",    "d.intention", "d.subgoal",  "d.outbox", "d.belief",
    "d.pending",  "d.source",    "d.extends",  "d.agents", "d.dirs",
    "d.seen",     "d.evidence",  "d.verb",     "d.holder", "d.uses",
    "d.inbox",    "d.delivered", "d.owner",
};

// ---------------------------------------------------------------------------
// seed
// ---------------------------------------------------------------------------

/// What seedLineageStore hands back: the kept per-agent Store (job.base
/// pattern — the caller owns it), its span source bound so artifact-local
/// span names resolve.
struct LineageStore {
  std::unique_ptr<Store> store;
  /// The external id `from` files under in `store` — the scroll the sealed
  /// ops call local, bound as the published local scroll.
  ScrollId selfScroll{localScroll};
};

/// Construct and seed the lineage store: historyFromSeal over the
/// artifact's sealed ops, then setContentSource +
/// bindPublishedLocalScroll(addScroll(from))
/// (publication.cpp:1129-1131; store.cpp:1418/:1425) — without the
/// pair, artifact-local span names dead-read "".
///
/// @param segments   the sealableOps() segments, oldest first.
/// @param from       the scroll the ops were sealed beside — what their
///                   ScrollId zero means.
/// @param scrolls    the seal's scroll table (foreign keys → Scroll).
/// @param source     the content source `from`'s bytes resolve through;
///                   borrowed, must outlive the store. May be null when
///                   the bytes arrive another way (setExternalLiveBytes,
///                   a later real source) — the bind still happens.
/// @param documentId phase-6 (v): the artifact's persisted identity —
///                   adopt it on a resume-side re-seed so the document
///                   keeps one DocumentId across checkpoint cycles; null
///                   mints a fresh id as before.
[[nodiscard]] std::expected<LineageStore, std::string>
seedLineageStore(std::span<const std::string_view> segments, const Scroll &from,
                 const std::map<std::string, Scroll> &scrolls,
                 const ContentSource *source,
                 const DocumentId *documentId = nullptr);

/// The single-segment spelling: the artifact was sealed at once.
[[nodiscard]] std::expected<LineageStore, std::string>
seedLineageStore(std::string_view sealed, const Scroll &from,
                 const std::map<std::string, Scroll> &scrolls,
                 const ContentSource *source,
                 const DocumentId *documentId = nullptr);

/// The one-time program-rooted promote — how an unsealed compile arena's
/// image band reaches the lineage store (adoptOpRecords,
/// store.cpp:3836-4018, is the other spelling). `root` is the agent cell
/// alone for every promote after this one — program-side cells are never
/// under a promote root again (spec :375-377, :635-636, :1791-1799).
[[nodiscard]] std::expected<Promoted, std::string>
seedProgramImage(Store &lineage, ArenaManifold &arena, CellRef programRoot);

// ---------------------------------------------------------------------------
// mint/file
// ---------------------------------------------------------------------------

/// The name set the sweep files: the fixed vocabulary above ∪ every name
/// readable off `arena.dimensions()` — the base∪arenaDims_ surface
/// (arena_manifold.cpp:34-46, :87-88), names read off each dim cell's
/// interned content via textOf. NEVER the arena-local `d.dims` rank, which
/// appends only under `dimDims && home` (:91-103) and would miss exactly
/// the session-minted class filing exists to cover.
[[nodiscard]] std::set<std::string>
dimensionVocabulary(const ArenaManifold &arena);

/// What a filing sweep leaves behind: the head it advanced to and the
/// rebuilt fold — the base-injected fold the pin consumes
/// (getOrCreate's rebuild IS the injected base,
/// dimension_registry.cpp:194-217). Pin this, not a second rebuild:
/// a filing after the pin lands outside the sealed fold and the arena
/// mints twins regardless (spec :3031-3039).
struct DimensionFiling {
  MicroversionId head;
  Manifold fold;
  /// The names swept, in file order (deduplicated by the caller's set).
  std::vector<std::string> names;
};

/// The dimension-filing sweep, contract shape: enumerates
/// dimensionVocabulary(arena) and files each missing name on the kept
/// lineage Store's persistent `d.dims` rank through
/// DimensionRegistry::getOrCreate → Store::makeDimension, at the store's
/// current head. Idempotent by name; the sweep re-enumerates per cycle.
/// The returned-void form drops the fold — callers about to pin want the
/// three-argument overload instead.
[[nodiscard]] std::expected<void, std::string>
fileDimensionVocabulary(Store &lineage, ArenaManifold &arena);

/// The sweep's carry form: files at @p head (zero → latest()) and hands
/// back the advanced head and the filed fold for pinning.
[[nodiscard]] std::expected<DimensionFiling, std::string>
fileDimensionVocabulary(Store &lineage, ArenaManifold &arena,
                        MicroversionId head);

/// The emit-lineage arm: file an explicit name list with no live arena
/// to enumerate. The artifact's own image emission files the same
/// vocabulary this way before ITS compile arena's `initGenesis`
/// (vortex_core.cpp:14, :29-70 — the invariant is "file before first
/// resolution" on both lineages), source-named dimConstant mints
/// (compiler.cpp:769-772) included in the list.
[[nodiscard]] std::expected<DimensionFiling, std::string>
fileDimensionNames(Store &lineage, MicroversionId head,
                   std::span<const std::string_view> names);

// ---------------------------------------------------------------------------
// fold → pin/inject: one re-genesis cycle
// ---------------------------------------------------------------------------

/// What regenesisCycle hands back: the pinned fold and the fresh
/// WorkerContext over it, plus the bookkeeping the ceremony needs.
/// `fold` and `context` are heap-pinned — the context's arena holds a
/// raw pointer into the fold, so the result must move only by pointer.
struct RegenesisResult {
  /// The head `fold` was rebuilt over — after the promote and the filing
  /// sweep both appended.
  MicroversionId head;
  /// `agentRoot`'s persistent ref: the minted twin when it was an arena
  /// cell (promoted.cells.front()), itself when it already named a base
  /// cell. This is what the next cycle's bind set and promote root take.
  CellRef agentCell{noCell};
  /// The pinned fold — the fresh context's base, heap-held so the arena's
  /// raw `base_` pointer never dangles behind a moved-from result.
  std::unique_ptr<Manifold> fold;
  /// The fresh non-movable bundle pinned on `fold` at construction —
  /// arena/core/vm/stdlib, `core`'s initGenesis having run its
  /// first-resolution over the filed fold already. The caller runs the
  /// genesis (a)-(e) ceremony over `context->arena`/`context->vm`.
  std::unique_ptr<WorkerContext> context;
};

/// One re-genesis cycle over the kept lineage Store:
///
///   1. promote(Store&, latest, arena, agentRoot) — into
///      arena.base()->store()'s successor band; `agentRoot` is the agent
///      cell alone, never a program-side cell. A same-Store cycle needs
///      no sealLocalSpool — op records and local spans ride verbatim.
///      The shadowed cells at or below `imageExtent` stay out of
///      extraRoots: program-side writes are verb-replayed state — a
///      learn_* install or a graft_* link — which genesis re-derives
///      and must not also promote (a promoted install plus its replay
///      twin is a double-install). The durable session cells — patch,
///      vars and synapse records — all hold refs above the image band,
///      so the ref-space boundary is the sideness test.
///   2. the filing sweep re-runs on the still-live session arena —
///      enumerating its session mints — BEFORE the rebuild seals the
///      fold, so the names ride this fold rather than landing outside it.
///   3. Store::rebuildManifold over the new head, heap-pinned.
///   4. a fresh WorkerContext over the fold (non-movable; its arena pins
///      `base` at construction, so each cycle is a new bundle — the fresh
///      VM's empty opcodeMap_ is what genesis (b) then re-fills).
///
/// `imageExtent` is the recorded program-image band size the lineage
/// was seeded with — 0 for a lineage that holds no image prefix.
///
/// The caller then re-runs the (a)-(e) ceremony over the result.
/// Promote-visible mutations only: content ops for isEphemeral cells,
/// link-writes on holdsOwn cells — a shadowed base cell's rewritten
/// content and a removed base edge do not cross (arena_manifold.cpp
/// :1867-1870, :2020-2053), which is why durable counters take the
/// mint-and-repoint form.
[[nodiscard]] std::expected<RegenesisResult, std::string>
regenesisCycle(Store &lineage, ArenaManifold &arena, CellRef agentRoot,
               std::uint32_t imageExtent);

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_LINEAGE_HPP

# r8-codebase: Round-8 codebase verification of `design/vagent-metaprogramming.md` v1.7

**Orchestrator summary (≤200 words):** Both my r7 findings are correctly resolved and the R7-A
lineage root-fix is mechanically real. Verified the full chain: `job.base` shares the base's store
(`mesh_jobs.cpp:304-318` — `store = base->store.get()` at :314, `base->manifold` as arena base at
:319-320); `publishValue` calls `sealLocalSpool` with default `opsAlreadySealed=0`
(`mesh_value.cpp:227-228`), so `sealableOps(store,0)` emits `[1..opCount]` — the segment
`at=0,length=opsTotal` (`publication.cpp:611-617`, `:666-667`) — and every checkpoint's ops segment
does carry the image prefix; `receiveValue`→`restorePublication`→`rebuildManifold` folds ops
index-for-index with `birthOp = opIndex` (`manifold.cpp:434,442`, inside `applyStructure`). Every
v1.7-delta citation re-verified: `setOpcodeKind` exists at `vortex_vm.cpp:98-100` with the cited
signature; `initGenesis` adopts `*base->home()` (`vortex_core.cpp:30-36`); `home_` is the first
MakeCell's opIndex (`manifold.cpp:450-451`); `ForeignOpHandle` translation at
`arena_manifold.cpp:1952-1954`; pump ordering honestly re-scoped as adapter discipline. ~45 unique
citations in the deltas; exactly one wrong: "it dequeues whatever is pending, `:659-672`" — the
dequeue is `mailbox.front()`/`pop_front()` at `:467-468`; `:659-672` is `pending`/`checkpointView`.
Counts: **0 BLOCKING, 0 MAJOR, 0 MINOR, 2 NITS**.
**VERDICT: CONVERGED.**

## Verdicts on r7 findings

### Finding 1 (`home` "arena-minted and ephemeral") — RESOLVED

v1.7 :1385-1394 applies the suggested fix almost verbatim: "`home` is the session's cursor anchor:
the checkpoint's own persistent `home` whenever the base has one — `initGenesis` adopts
`*base->home()` (`vortex_core.cpp:30-36`), and the base's `home_` is the store's first MakeCell
(`manifold.cpp:450-451`), a pre-extent persistent ref — or arena-minted on a base-less genesis."
Both cites verified exactly: `initGenesis` sets `home_ = *baseHome` when `arena_.base()->home()`
has a value (:30-36); `if (!home_) home_ = opIndex` inside `applyStructure`'s MakeCell case
(:450-451) — a persistent pre-extent ref. The exclusion now rests on the operative arms: no `+d`
edge points *into* `home` (`spawnCursor` links `rankTail(home, d.cursors) → cursor`, outbound at
`vortex_vm.cpp:157-160`; `d.stack` edges are `cursor → frame`, :206, :865-887) — the directional
claim is exactly right — and were `home` program-side the prune excludes the same frames. The
"conclusion holds either way" framing is the honest version of my suggested fix.

### Finding 2 (`ForeignOpHandle` missing from P16 promote clause) — RESOLVED

Fixed at both occurrences: M11 :363-364 now reads "translating only links and handle-kind values
(`OpHandle`/`ForeignOpHandle`, `arena_manifold.cpp:1934-1954`)", and the P16 row :2218 carries the
same wording plus "the handle-translation arm covering both". Verified: `OpHandle` →
`makeOpHandle(isEphemeral(target) ? real.at(target) : target, …)` at :1947-1951, `ForeignOpHandle`
→ `makeForeignOpHandle(out.version, *foreignOpTarget(from, arena))` at :1952-1954 — both inside the
cited range.

## R7-A verification — the store-lineage model is mechanically real

The central claim chain, link by link:

- **Base store shared, lineage preserved.** `mesh_jobs.cpp:304-318`: `job.base` fetches the
  received value, `store = base->store.get()` (:314) — the child publishes into the *base's own
  restored store*; `evaluate(…, base->manifold.get(), store)` (:319-320) makes the folded base the
  arena base. New ops append at `opCount+1…`, above the inherited prefix. This is precisely the
  "low ops prefix" lineage the doc names (cite :303-318 — the block proper is :304-318; the extra
  top line is the prior verify's `return`, harmless).
- **Every checkpoint's ops segment carries `[1..opCount]`.** `sealLocalSpool` is incremental-
  capable, but `publishValue` always invokes it with the default `opsAlreadySealed=0`
  (`mesh_value.cpp:227-228`), so `newOps = sealableOps(store, 0)` iterates `index = 1..size`
  (`publication.cpp:982-983`, `exportBinaryOps(0)` at :1019) and the published segment is
  `at=0, length=opsTotal` (:665-667); `publish` receives exactly that one segment
  (`{*seed.history.opsSegment}`, :241). So positions `1..E` do ride inside every checkpoint's ops
  segment, as claimed.
- **The fold preserves positions verbatim.** `receiveValue` restores the store from
  `publication.opsSegments` (`mesh_value.cpp:330-345`) then `rebuildManifold(version)` (:350-351);
  `Store::rebuildManifold`→`rebuildManifoldFromIndex` (`store.cpp:524-527`) drives
  `Manifold::applyStructure` per op, which assigns `.birthOp = opIndex` and
  `byRef.emplace(opIndex, dense)` (`manifold.cpp:434,442`). A persistent `CellRef` *is* a spool
  position — `manifold.hpp:295` "`birthOp{0}; ///< 4: the MakeCell index; == this CellRef`" — so
  `[image 1..E][state >E]` classifies by construction. `noCell == 0` (`dim_vector.hpp:23`) is the
  only ref below `1..E`; ops are 1-indexed.
- **`resume` installs the folded manifold as the arena's const base.** `mesh_actor.cpp:260-271`:
  `receiveValue` at :260, `actor->checkpoint` at :269, `WorkerContext(actor->checkpoint->manifold.get())`
  at :270-271 — the ctor passes `const Manifold *baseManifold` into `arena(baseManifold)`
  (`worker_context.hpp:52-55`); `arena` is public at `worker_context.hpp:63`. `importValue` is not
  invoked; the doc's resume-vs-import dichotomy (`mesh_value.cpp:485-501` demotes unmarked
  `OpHandle`→`None` :485-490, re-kinds marked via `local.at(*target)` :494-500;
  `arena_manifold.cpp:1947-1951` writes `real.at(target)` for ephemeral targets) verified again.
- **Chain-stability.** The continuation pins the artifact: `continuation->programRef` is checked
  against `request.programRef` (`mesh_actor.cpp:199`) and `grant->programRef` (:215-216), and
  `ActorContinuation::programRef` is a field (`mesh_actor.hpp:96`). The `E` record on the adapter's
  `programRef` bind record is adapter-side spec, correctly labelled as such.

The genesis narration — (a) lineage bind, (b) `ensureDimension` + `setOpcodeKind`, (c) `programRef`
bind, (d) `d.patch` replay, (e) `+d.refs` sweep last, (f) turns — is consistent with the machinery:
`receiveValue` does fold the whole band inside `rebuildManifold` before any genesis step runs, so
"binds what arrived, nothing re-materializes an image" is accurate. The (e)-after-(d) dependency is
coherent: `reify` copies slot contents verbatim per the doc's own recipe spec, so a replayed
install's slots carry the head's current (post-checkpoint) contents and a head-only sweep cannot
reach them.

## R7-B and other disposition items — verified

- **`setOpcodeKind` pass (:1268-1281, :1667-1675):** `vortex_vm.cpp:98-100` is exactly
  `void VortexVM::setOpcodeKind(CellRef opCell, OpcodeKind op) { opcodeMap_[opCell] = op; }`.
  `opcodeMap_` is a `VortexVM` member (`vortex_vm.hpp:186`), per-`WorkerContext`
  (`worker_context.hpp:65`), empty after `resume` — the rationale moved off "stale indices"
  correctly. `mintOpcode` mints via `arena().makeScalarCell` (:73-82) — an `ephemeralBit|dense`
  ref (`arena_manifold.hpp:468-470`, `manifold.hpp:154-158`) — so re-running it would register
  ephemeral twins, never the folded persistent cells: the "one wrong shape" claim is exact.
- **`d.name` as label dim (:1332-1333):** removed from the bare-`d.` rank list; `vortex_core.cpp:61`
  is `dims_.name = mintNamedDimension("d.name", lastDim)` — the dim's mint site; the "single-link"
  semantics follow from one `-d.name` edge per cell (eviction, `arena_manifold.cpp:1352-1373`).
- **M6 `#Call` reconciliation (:235-238 of diff):** "`run` reads the head's `+d.entry` itself —
  the operand slot is head-valued — and pushes the §5.8 `#Call` frame … `Call` executes
  `toInt(inputs[0])` (`vortex_vm.cpp:854-856`)" — exact: `targetOp = toInt(inputs[0])` at :856
  inside the Call case :854-876.
- **`run`-role records / headless-`run` (:1529-1554):** the `+d.grab → "call"/"run"` role label
  keeps the writeback keyed on record role, `+d.name` shape selecting only the resolution path;
  `exportSymbol` binds `sym +d.values → entryOp` directly (`vortex_core.cpp:123`) so a `def`
  symbol is head-less by construction — the refusal class is real.
- **Prefix reservation (:1749-1751):** `src:`/`reify:`/`patch:` reserved at module declaration; the
  "module literally takes that name" arm is deleted; the only `patch:` occurrences left in live
  spec are the clause-label prefix and `patch:near_water` (a label, correctly inside `code_named`).
- **`plan_named`/`symbol_named` representations (:1766-1778):** symbol names live as cell *content*
  (`exportSymbol` mints `makeCell(symbolName)`, no `+d.name` edge — verified :119-127); clause
  names live as `+d.name` label cells. "Both compare the name string verbatim — it is the cell
  carrying it that differs" — correct.
- **Pump ordering re-scope (:1603-1623):** honestly framed as proposed adapter discipline; the
  supporting cites verify — `pending` = `mailbox.size()` (:659-662), `pumpOne` skips empty-mailbox
  actors (`:462`), `checkpointView` refuses non-empty (`:672`), `executor_(…, *actor->context)`
  invoked synchronously per turn (`:473-477`), `ActorDown` stops re-entry (`:501`).
- **`vortex_vm.cpp:157-160`** (was `:159-160`) — now covers the comment + link; better.
- **`foreign_op_handle.cpp:98-117`** (`isCellRelocationHandle`, both overloads — kind AND marker),
  **`ops.hpp:154-163`** (`OpHandle=4`, `ForeignOpHandle=7`), **`arena_manifold.cpp:1766`**
  (`d.dims` edge skip), **`:1782-1783`** (`discover(edge.pos); discover(edge.neg)` — both
  directions), **`:1759-1764`** (handle→seen/frontier), **`:1797-1809`** (`getCompactOp` gate),
  **`:1423-1439`** (`insertIntoRank`, `rankContains` no-op :1430-1432; idempotent doc
  `arena_manifold.hpp:624-628`), **`cell_views.hpp:258`** (`rankTail`),
  **`vortex_stdlib.cpp:4238-4241`** (`addClause` rankTail precedent), **`arena_manifold.cpp:78-105`**
  (`ensureDimension`), **`:1489-1490`** (shadow birthOp = base ref), **`:1744-1788`** (BFS),
  **`arena_manifold.hpp:235-240`** (`Promoted`), **`arena_manifold.hpp:420-426`** (`consumeWork`
  throw), **`:2123`** (`outstandingMarks_ = 1` inside `hydratePrefix`), **`or_parallel.cpp:349`**
  (`hydratePrefix` call), **`vortex_vm.cpp:102-114`/`:103-105`** (`getOpcodeKind`),
  **`:73-82`/`:904`/`:139-143`/`:854-893`/`:1269-1273`/`:1287-1294`/`:337`/`:343-346`/`:771-806`/
  `:839-873`/`:1150-1168`/`:1220-1225`/`:238-243`/`:206`/`:865-887`/`:50-67`**,
  **`vortex_core.cpp:196-254`/`:196-229`/`:113-126`/`:384,402`/`:30-36`/`:61`**,
  **`compiler.cpp:108-110`/`:516-525`/`:769-772`/`:853-855`/`:475-483`/`:484-486`**,
  **`mesh_actor.cpp:668-694`/`:473-486`/`mesh_actor.hpp:78-84`/`:102-105`/`mesh_value.hpp:69-74`/
  `mesh_evaluator.cpp:723-735`/`vagent-agent-language.md:1670`/`vql/compiler.cpp:36-80`,`:71`/
  `cell_path.hpp:31`/`lexer.cpp:14-24`,`:379-398`/`parser.cpp:851`/`vortex_stdlib.cpp:3800-3802`,
  `:3841-3843`,`:4051`/`arena_manifold.cpp:256-267`/`mesh_value.cpp:485-501`** — all exact.

## New findings (v1.7 deltas only)

### Finding 1 — NIT

- **Location:** :1613-1614 (P16 step-2 pump-ordering clause).
- **Problem:** "current `pumpOne` gives no such ordering on its own (it dequeues whatever is
  pending, `:659-672`)". The dequeue is `auto [message, size] = std::move(actor->mailbox.front())`
  + `pop_front()` at `mesh_actor.cpp:467-468`. `:659-672` is `pending()` (:659-662),
  `localCursor` (:663-667) and the head of `checkpointView` (:668-672) — a carry-over from the
  `pending`/`checkpointView` cites two sentences earlier. The claim itself is correct.
- **Concrete fix:** `:467-468` (or `:461-468` for the scan-through-dequeue region).

### Finding 2 — NIT

- **Location:** :595-596, :1347-1348, :1662-1663 (and the changelog paraphrase at :2632-2633).
- **Problem:** "`sealLocalSpool` seals the whole spool (`publication.cpp:611-617`)". The claim is
  true in the checkpoint path, but not because of anything at the cited lines: `sealLocalSpool` is
  incremental-capable — `newOps = sealableOps(store, opsAlreadySealed)` covers
  `[opsAlreadySealed..opsTotal)` (:612-617) — and the whole-spool property comes from
  `publishValue` always passing the default `opsAlreadySealed=0` (`mesh_value.cpp:227-228`),
  producing the single segment `at=0, length=opsTotal` (:666-667) handed to `publish` as
  `{*seed.history.opsSegment}` (:241). A reader landing on :611-617 sees incremental machinery and
  must chase the call site to learn the prefix ships too — this is the round's load-bearing cite.
- **Concrete fix:** cite the call convention alongside: "`sealLocalSpool` seals the whole spool in
  the checkpoint path — `publishValue` passes `opsAlreadySealed=0`, so the segment covers
  `[0..opsTotal)` (`publication.cpp:611-617`, `:666-667`; `mesh_value.cpp:227-228`,`:241`)".

## Observation (not a finding)

`rebuildManifold` "assigns `birthOp = opIndex`" is shorthand — the assignment lives in
`Manifold::applyStructure`'s MakeCell arm (:434), driven per op by the rebuild; `home_`/`dimsDim_`
capture (:447-454) is the same fold. Attribution is defensible; flagging only because the cited
range is the fold step, not `rebuildManifold` itself (`store.cpp:524-527`).

VERDICT: **CONVERGED** — 0 blocking, 0 major, 0 minor, 2 nits. Both r7 findings resolved verbatim;
the named store lineage is the first fully mechanized version of the checkpoint model — the
`job.base` → `sealLocalSpool(opsAlreadySealed=0)` → `rebuildManifold(birthOp=opIndex)` chain holds
end to end, and the genesis reorder's dependencies are stated correctly.

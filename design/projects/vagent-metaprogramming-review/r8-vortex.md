# Round 8 critique (vortex role): vagent-metaprogramming.md v1.7

Lane: VortexCore/VM execution semantics, trail/marks, handle relocation, promote,
MeshActorRuntime invariants. Convergence check on the v1.7 deltas only
(§4.5 :583-613 lineage model; §6.6 :1332-1395 sideness/E/fail-closed/home;
:1496-1576 slot-role writeback + headless-`run`; :1578-1700 genesis reorder +
sweep-last; :2187-2196 adapter seeding + P16 row; :2625-2672 changelog),
verified against the tree: `job.base` continuing store
(`mesh_jobs.cpp:303-318`), whole-spool seal (`mesh_value.cpp:226-241`,
`publication.hpp:337-340` `opsAlreadySealed = 0`, `publication.cpp:611-617`),
contiguous replay (`publication.cpp:1139-1165`), verbatim fold
(`manifold.cpp:427-442`, `manifold.hpp:295`, `dim_vector.hpp:23`),
`receiveValue`/`rebuildManifold` install (`mesh_actor.cpp:260-271`,
`mesh_value.cpp:345-352`), `programRef` pin (`mesh_actor.cpp:199`),
`initGenesis` base-home adoption (`vortex_core.cpp:30-36`,
`manifold.cpp:450-451`), `exportSymbol` `+d.values → entryOp`
(`vortex_core.cpp:119-127`), `mintOpcode`/`setOpcodeKind`/`getOpcodeKind`
(`vortex_vm.cpp:73-114`), `resolveCell` dense-range + base fall-through
(`vortex_vm.cpp:50-67`, `arena_manifold.cpp:256-267`, `:791-797`), `refOf`
(`arena_manifold.hpp:468-470`, `manifold.hpp:154-158`), shadow birthOp
(`arena_manifold.cpp:1485-1494`), `ensureDimension` find-or-mint
(`arena_manifold.cpp:78-105`), promote BFS/handleTarget/d.dims-skip/gate/mint
arms (`arena_manifold.cpp:1744-1788`, `:1797-1813`, `:1934-1956`),
`isCellRelocationHandle` kind+marker (`foreign_op_handle.cpp:98-117`),
marker precedent (`mesh_evaluator.cpp:723-735`), `Call` operand
(`vortex_vm.cpp:854-857`), `d.stack`/cursors/`Halt`
(`vortex_vm.cpp:157-160`, `:204-208`, `:864-893`, `:1218-1228`),
`importValue` demote (`mesh_value.cpp:483-501`).

## Verdicts on r7 findings

### 1. MAJOR — store lineage (R7-A): RESOLVED, mechanically grounded

v1.7 names exactly the lineage the fix demanded, and every leg of it checks
out against the tree:

- **The prefix is real.** The `job.base` pattern cited (`mesh_jobs.cpp:303-318`)
  is a continuing store: `fetchValue` → `receiveValue` → `restorePublication`
  replays *all* `opsSegments` contiguously — `segment.at != count` refuses
  (`publication.cpp:1139-1165`) — so the restored store holds the whole
  spool `1..N`; `evaluate`/`publishValue` then append above it on the same
  `Store`. The agent's store *is* the artifact's lineage store; the image is
  at positions `1..E` before any state op (`noCell == 0`,
  `dim_vector.hpp:23`, ops 1-based).
- **"Seals the whole spool" is literally true on this path.** `publishValueImpl`
  calls `sealLocalSpool` *without* `opsAlreadySealed` (`mesh_value.cpp:226-228`;
  default `0`, `publication.hpp:340`) → `sealableOps(store, 0)` covers
  `1..opCount` in one segment, and `publish` gets `{*seed.history.opsSegment}`
  (`:241`) — so `1..E` ships inside every checkpoint's *single* ops segment,
  not merely transitively. (The incremental `SealState`/`opsAlreadySealed`
  delta path exists for document stores; the vortex value path re-seals whole.)
- **The fold preserves positions.** `rebuildManifold` assigns `birthOp =
  opIndex` verbatim (`manifold.cpp:427-442`: `byRef.emplace(opIndex, dense)`,
  `.birthOp = opIndex`); a persistent `CellRef` *is* a spool position
  (`manifold.hpp:295`); `receiveValue` installs the folded manifold const as
  the arena's base (`mesh_actor.cpp:260-271`, `mesh_value.cpp:345-352`). The
  `≤ E` discriminator classifies **by construction** — the extent-inversion
  root cause is closed, not narrated around.
- **V12 reconciled honestly.** §4.5 rows now read "not republished — rides the
  shared base lineage" / "folded back verbatim with the lineage's image
  prefix" (:588-589, :591-598) — `promote` still never re-emits skeleton
  cells; the seal carries them as already-present ops. "Reloaded from
  `programRef`" is withdrawn. The byte-cost consequence (the image re-seals
  inside every checkpoint) is owned in the text.
- **`home` claim corrected and verified.** `initGenesis` adopts `*base->home()`
  (`vortex_core.cpp:30-36`); `home_` = the fold's first MakeCell
  (`manifold.cpp:450-451`) → a pre-extent persistent ref → program-side; the
  frames exclusion is now stated directionally ("no `+d` edge reaches *into*
  `home`", :1390-1394) — my r7 inside-out nit fixed.
- **Fail-closed wired.** Prefix-less lineage → empty program-side set →
  `learn` refused; `programRef` naming no received cell → genesis bind refusal
  (:1368-1375, :1657-1664). The default-broken outcome from r7 is gone.
- **New code registered.** §10.1 adapter row seeds the artifact's ops segment
  verbatim at load and carries `E` on the `programRef` bind record
  (:2187-2195); the pin is real (`continuation->programRef ==
  request.programRef`, `mesh_actor.cpp:199`).

### 2. MINOR — `plan_named`/`symbol_named` `run` operands: RESOLVED — in the stronger direction (see new finding 1)

:1505-1506 enumerates them ("a `plan_named`/`symbol_named` operand spelled
with a literal qualified name → that qualified name"), and the writeback is
re-keyed on **record slot role carried on the record** — a `+d.grab →
"call"`/`"run"` label cell, the same fresh-scalar convention the M8 memo key
already uses (:1419) — never derived from name shape (:1529-1536). `call`
writes `int64(entryOp)`, `run` writes `int64(head)`; the r7 inversion
(qualified shape → entryOp into a head-valued slot) can no longer arise.
The **headless-`run`** refusal class is named on both sides of the boundary:
record malformed → patch drops (:1550-1555), and in-session `run` refuses the
same target at the descriptor lookup (§6.2 :1122-1126) — coherent with
`run(c: Code<T>, args)`: a `def` symbol's `+d.values` names an entry op
(`exportSymbol`, `vortex_core.cpp:119-127`), which lacks the head layout
`run` derefs.

### 3. MINOR — `opcodeMap_` re-registration: RESOLVED

Genesis (b) is now "a registration *pass* over the folded cells, never a
re-run of `mintOpcode`" through `setOpcodeKind` (`vortex_vm.cpp:98-100`),
with the rationale correctly moved to "the empty map, not stale indices:
`opcodeMap_` is per-`WorkerContext` and empty after `resume`" (:1667-1675);
P15's row repeats the corrected account (:2217). `getOpcodeKind`'s
`asInt64` fallback (`:102-114`) means the pass can decode each folded op
cell's kind from the image alone; registered folded refs are exactly what
P15's `masterOf` predicate needs (shadows keep the base ref, so the key is
stable under overlay writes).

## R7-B — (d) replay before (e) sweep: verified sound

The reorder is stated **and** justified at :1682-1699: the sweep covers
"every `learn`-registered head's records **and every replay-installed copy's
regenerated records**"; copy-inheritance is the named dependency — "an
`OpHandle`-kind source slot yields an `OpHandle`-kind copy, an int64 operand
the head's last-written value — so a head-only sweep cannot reach the copies'
slots." This is the right account mechanically: `reify` regenerates `+d.refs`
records and fresh `d.mesh.relocation` markers on copies (:1038), so the sweep
has records to hit; a replayed copy's `OpHandle` slot carries the *promoted*
target verbatim (the registered head's handle content copied at mint), so
`Int64(*handleTarget)` on the copy writes the same rehydrated value the head
gets; and `ArenaManifold::slot` falls through to `base_->slot` for
non-ephemeral refs (`arena_manifold.cpp:791-797`), so the persistent
`int64(ref)` resolves. The second reason — name-records must see
**post-installation** symbols (a `learn_method`/`learn_plan` replayed at (d)
was invisible to a pre-replay sweep) — is the r7-B rationale retained.
No defect in the order.

## New findings (v1.7 deltas only)

### 1. MINOR — a `plan_named`-operand name-record rebinds through the wrong walk

**Location:** :1505-1506 (records "that qualified name" for both
`plan_named`/`symbol_named` operands) vs :1537-1549 (qualified names resolve
through `module_named` + `symbol_named`'s module-`d.vars` walk only) vs
§6.7's own split (:1454-1462, :1770-1775).

**Problem.** The doc elsewhere states the two lookups "walk different ranks
and match different representations": a plan's qualified name lives on its
clause's `+d.name` label, "**not in a `d.vars` symbol**" (:1774-1775), and
clause patch-targets resolve through `plan_named` precisely because methods
have no `+d.name` (:1457-1462). But a `run`-role name-record stores only the
qualified string, and genesis routes every qualified name down
`symbol_named`. For `run(plan_named("demo:Forager.p"))` in a learnable
fragment, the rebind either finds no `d.vars` symbol (the name is a clause
label) → unresolved → the patch drops with a fault — the promised
`int64(head)` writeback never happens and the learned binding dies at first
resume — or resolves a *same-named* `d.vars` symbol (a goal or method sharing
the `Agent.member` namespace, the routine case in this model) → binds the
wrong `+d.values` target: headless-`run` drop if it lands on an op, **silent
misbind** if it lands on a different head. The shape test cannot recover the
distinction — both builtins spell `module:Agent.member`.

**Fix:** one clause — carry the producing builtin on the record (the role
label already carries `"call"`/`"run"`; a second marker for
`plan_named`- vs `symbol_named`-produced names suffices), or route `run`-role
qualified records through the clause-`+d.name` walk: the plan head's own
`+d.name` is verbatim the recorded string, so the label→head lookup would
find it were the shape test not committed to `symbol_named` first.

## Items verified clean in the v1.7 deltas

- **`run`-role enumeration completeness** (:1503-1508): literal → head's
  `+d.name`; `code_named(L)` → `L`; `plan_named`/`symbol_named` → qualified
  name; `let`-alias/computed/`reflect`-born → opaque → closedness refusal.
  The class my r7 minor named is now enumerated.
- **Prefix-less fail-closed set** (:1368-1371): foreign/fresh-store publish,
  `importValue` ephemeral-twin ingress, sibling manifold — under ingress the
  *program cells themselves* are `ephemeralBit`-ref twins, which can never
  satisfy the persistent arm, so refusing `learn` outright is the correct
  (only) move.
- **Shadow/arena/promoted classification** (:1358-1367): shadow keeps the
  base ref verbatim (`arena_manifold.cpp:1489-1490`); arena mints are
  `ephemeralBit | dense`; promoted cells land post-extent — all three arms
  verified.
- **Reserved prefixes** (:1749-1752): `src:`/`reify:`/`patch:` may name no
  module — compile-time refusal; the contradictory "module literally takes
  that name" arm is deleted (survives only in the changelog :2654).
- **`plan_named` verbatim** (:1770-1775): matches clause `+d.name` cells
  verbatim; `patch:near_water` "is just a label" to `code_named`; the ergo
  split landed.
- **Foreign literal heads** (:1763-1766): no re-resolution spelling this
  round, deferred as U-M11, classified prefix-less/fail-closed rather than
  silently aliased — the r6 residual is now stated, not implied.
- **Pump-ordering re-scope** (:1610-1617): "proposed adapter discipline, not
  an attribution to the pump" — `pumpOne` dequeues whatever is pending; the
  synchronous-callback claim is honestly labelled.
- **`+d.grab` role label**: overload is deliberate — the M8 memo record
  already keys on a `+d.grab` scalar (:1419), record-cell annotation, no
  walk enumerates records by `d.grab`.
- **Patch replay non-recording** (:1678) and the two discipline rules
  (:1704-1715): consistent — the image prefix's verbatim fold is what makes
  "restores the program's own attachment for free" true (:1707-1708).

## Nits (non-blocking)

- :1657-1658 — "the persistent high-water below which program cells sit
  equals `E`" is inside-out: the band is `1..E`, the high-water *is* `E`.
- "sealLocalSpool seals the whole spool" cites `publication.cpp:611-617`,
  which displays the delta-capable signature; the whole-spool property is
  the *caller's* (`mesh_value.cpp:226-228` passes no `opsAlreadySealed` →
  `0`). True in context; a reader could implement incremental sealing and
  the claim still holds via the `opsSegments` chain, but "inside every
  checkpoint's ops segment" is only literally true because this path
  re-seals from `0`.
- Same-artifact siblings share the artifact's `storeId`/`documentId`
  lineage under the `job.base` pattern — two agents off one artifact
  publish divergent histories under one store identity. Pre-existing
  `job.base` exposure, not new, but the doc now makes every agent an
  artifact-lineage store.

## Summary for the orchestrator

All three r7 vortex findings are resolved: the store lineage is named and
mechanically verified end-to-end (`job.base` continuing store → contiguous
replay → verbatim fold → whole-spool re-seal), the `run`-operand enumeration
is keyed on record slot role, and `opcodeMap_` re-registration is a
`setOpcodeKind` pass over folded cells. The R7-B reorder is stated with the
copy-inheritance justification. New: **0 BLOCKING / 0 MAJOR / 1 MINOR /
3 nits** — the minor is a `plan_named`-operand name-record routing through
`symbol_named`'s `d.vars` walk when the name lives on a clause `+d.name`
label (drop-or-misbind at genesis; the record needs the producing builtin or
the clause-label walk). The loop's deepest root — unmechanized store lineage —
is closed in v1.7.

VERDICT: CONVERGED

# Round 7 critique (vortex role): vagent-metaprogramming.md v1.6

Lane: VortexCore/VM execution semantics, trail/marks, handle relocation, promote,
MeshActorRuntime invariants. Convergence check on the v1.6 deltas only
(:1313-1347 extent/transit/frames; :1442-1493 shape-split writeback; :1528-1605
publish/genesis ordering; :1636-1670 labels/plan_named; :2103-2106 P16 row;
:970 legend), verified against the tree: `birthOp == CellRef` and verbatim index
fold (`manifold.hpp:295`, `manifold.cpp:427-442`), `ephemeralBit`
(`manifold.hpp:154-158`, `arena_manifold.hpp:468-470`), shadow base ref
(`arena_manifold.cpp:1489-1490`), promote arms (`arena_manifold.cpp:1744-1788`,
`:1797-1813`, `:1867-1869`, `:1934-1954`), `ensureDimension` find-or-mint
(`arena_manifold.cpp:78-105`), `insertIntoRank` idempotence
(`arena_manifold.cpp:1423-1439`, `arena_manifold.hpp:624-628`), `link` eviction
(`arena_manifold.cpp:1352-1373`), `isCellRelocationHandle`
(`foreign_op_handle.cpp:98-117`), `exportSymbol` (`vortex_core.cpp:119-127`),
`writeValue` kinds (`vortex_core.cpp:196-229`), `bindInput`/`bindOutput` wing
slots (`vortex_core.cpp:384,402`), `mintOpcode` arena mints
(`vortex_vm.cpp:73-82`), `Call`/`d.stack`/cursors (`vortex_vm.cpp:854-857`,
`:206`, `:864-887`, `:157-160`), `importValue` demote/re-mint
(`mesh_value.cpp:416-501`), `receiveValue`/`rebuildManifold`
(`mesh_value.cpp:345-352`, `store.cpp:508-522`), incremental seal
(`publication.cpp:585-680`), `pending`/`pumpOne`/`checkpointView`
(`mesh_actor.cpp:659-662`, `:459-505`, `:668-694`), executor seam
(`mesh_actor.cpp:473-480`, `worker_context.hpp:63`), marker precedent
(`mesh_evaluator.cpp:723-735`), `dimConstant` (`vagent/compiler.cpp:769-772`),
`reference()` (`:516-525`), drain/marks/consumeWork/hydratePrefix
(`vortex_stdlib.cpp:3800-3802`, `:3841-3843`; `arena_manifold.hpp:773`,
`:420-426`; `arena_manifold.cpp:1657-1698`, `:2123`; `or_parallel.cpp:349`).

## Verdicts on r6 findings

### 1. `#label` uniqueness (r6-vortex-1) — RESOLVED

:1640-1642 — "User `#label`s are uniqueness-checked **at emission** — a
duplicate is a compile-time refusal, since labels are the re-resolution key and
a duplicate would misbind." The enforcement point and scope are now stated
exactly where the fix asked: compile-time, across the program's literals (the
emission set the label→head walk later enumerates verbatim). The verbatim
matcher at :1488-1489 is now sound.

### 2. `reflect`-born `run` arm (r6-vortex-2) — RESOLVED

:1448-1450 — "a `reflect`-born operand is a session-side wrapper with no
durable referent, so an opaque or `reflect`-born `run` target in a learnable
fragment is a closedness refusal at emission." The arm is dropped to the
refusal — the recommended honest v1 answer; consistent with wrappers being
session-side, never `d.code`-registered (:1366-1377) and with the changelog
(:2489-2491).

### 3. `run`-operand slot convention (r6-vortex-3) — RESOLVED

§6.2 now states the convention explicitly: :1084-1088 — "the operand slot is
**head-valued by construction**: the emitter writes `int64(head)` into it, and
`run` derefs the head's `+d.entry` *and* `+d.values` … so the slot names the
head, never the entry op." Genesis splits writeback with it: Call-operand slots
are entry-valued (`Call` executes `toInt(inputs[0])`, `vortex_vm.cpp:854-856` —
verified) while `run`-target/`Code`-valued (fragment-identity) slots get
`writeValue(slot, int64(head))` — "the head itself, no `+d.entry` hop"
(:1471-1475, :1490-1492). The qualified→`symbol_named`→op-or-head discriminator
hop (:1480-1485, via `exportSymbol`'s `+d.values → entryOp`,
`vortex_core.cpp:119-127`) is correct on the Call arm. R6-B implemented.

### 4. `closed` two-arm clause (r6-vortex-4) — RESOLVED

:1420-1422 — "a capture *link* landing on a *program-side* cell (an
artifact-initial binding cell) is a program-side reference with no record, the
refused class on both arms: arm (a) admits only state-side cells and arm (b)
requires the record." The doc picked the refusal arm and named the
artifact-initial-field consequence; :1425-1427's blessing ("captures of
*declared fields and member vars* … hang on the agent's or member's `d.vars`
and so are state-side") is consistent — declared field/var cells are the
state-side class, the program-side edge case is the refused class.

## New findings (v1.6 deltas only)

### 1. MAJOR — "the image's ops are the spool's low prefix" presumes a store lineage nothing produces, and it contradicts V12 as written

**Location:** :1318-1325 (the discriminator's premise), :1574-1584 (genesis
(a)/(b)), against :579-582 (§4.5: program cells "not promoted (V12)", "do not
checkpoint", "reloaded from `programRef`") and :2279 ("the published value's own
transient store"). Changelog :2476-2483.

**Problem.** The `≤ E` discriminator requires program cells and checkpointed
state cells to share **one** persistent numbering: refs are the spool's op
indices (`birthOp == CellRef`, `manifold.hpp:295`), and `applyStructure`
assigns the passed index verbatim (`manifold.cpp:427-442`) — there is no
index translation on fold. The genesis manifold is the *checkpoint's own*
manifold: `receiveValue` → `store->rebuildManifold(publication.version)`
(`mesh_value.cpp:345-352`), handed to `WorkerContext` at
`mesh_actor.cpp:270-271`. Under the documented topology that manifold contains
**only promoted state cells**: `publishValue` promotes reachable state into "the
published value's own transient store" (:2279;
`vagent-agent-language.md:412`), program cells "do not checkpoint" (:582), and
the program image itself loads into the *arena* — `mintOpcode` mints arena
cells (`vortex_vm.cpp:73-82`), the artifact decode builds `ArenaManifold`
cells (`mesh_evaluator.cpp`), `bootstrap` rebuilds modules in the arena every
incarnation (`vagent-agent-language.md:425-427`). Arena cells are
`ephemeralBit`-refs by construction.

Step (a)'s claim — "the image's ops are the spool's low prefix … program cells
return at their recorded positions `0..E`" — is mechanically true in exactly
one topology: **the checkpoint spool must itself contain the program image's
ops as its low prefix** (the publish store seeded with the image before
`promote` appends state, or a continuing actor store that minted the image at
program load). Nothing else works:

- Fold the artifact's spool into a fresh manifold (program at `0..E`), then
  apply the checkpoint's state ops: impossible — ops carry absolute index
  references, and replaying them at shifted positions misbinds every baked ref
  while replaying them at their original positions collides with the image's
  cells. No store→store renumbering exists; `promote`'s `real` remap is
  arena→store only.
- Append the image ops to the checkpoint store: they land *above* the state
  ops — the inversion :1322-1324 explicitly says the scheme avoids ("a flat
  append after state would land them above it and invert the test").
- The text's literal two-fold reading — "replay the program artifact's own
  spool" (a), then "the checkpointed state cells join the persistent numbering"
  (b) — describes two spools; the only implementable version is one fold over a
  checkpoint spool that already *is* `[image prefix][state suffix]`, which is
  the seeded topology.

Either the doc commits to that topology — in which case **the program image is
serialized into every checkpoint**, the precise thing V12 exists to prevent
(`vagent-agent-language.md:244-247`), "program cells do not checkpoint" (:582)
is false in fact (the honest surviving invariant is "not re-promoted"), and
"reloaded from `programRef`" (:580) is wrong (the reload is the checkpoint's
own prefix fold); or it keeps V12's letter — in which case the resumed manifold
has *no* program cells, the recorded extent names nothing, and `ref ≤ E`
returns the empty set or misclassifies low-numbered state cells.

The second arm is worse than vacuous: :1324-1325/:1581-1582 bless the
empty-program-side outcome "by design, not an error" — but under today's
load path it is the *default*, not an ingress edge case. With nothing
classifying program-side, the transit-pruning never fires, a learned
fragment's link into the ephemeral program skeleton classifies *state-side*
(reachable, nothing pruned), `closed` admits it, and `promote` drags the
skeleton into the checkpoint — the V12 failure re-enters under a flag named
`closed`.

**Fix:** name the store lineage. The constructible version: program load
replays the artifact's ops into the actor's persistent spool *before* any state
op (making "the spool's low prefix" a fact of construction, recorded as `E` on
the `programRef` bind record); publish seals deltas off that continuing store
(`sealLocalSpool`'s `opsAlreadySealed`,
`publication.cpp:611-617`, already supports sealing the image once and
referencing it) — then genesis (a)+(b) is just `rebuildManifold` on the
checkpoint and every stated property follows. The doc must then reword V12's
letter ("not re-promoted — already in the base spool"), reconcile §4.5's
"reloaded from `programRef`", and register the persistent-image load path in
§10 — it is new code, not an adapter duty: today's load mints arena cells.

### 2. MINOR — the shape-keyed writeback leaves one unenumerated record kind: a qualified name on a head-valued slot

**Location:** :1446-1449 (operand enumeration), :1470-1493 (shape split).

**Problem.** The writeback convention is keyed on the *recorded name's shape*:
qualified → `int64(entryOp)`, label → `int64(head)`. The emitter enumerates
statically-known `run` operands as "a literal operand → its `src:` label, a
`code_named`-resolved operand → its `d.name` label" and refuses "opaque or
`reflect`-born". A `run(plan_named("demo:Forager.p"))` or
`run(symbol_named("demo","Feed"))` operand resolves a runtime session handle
whose literal name is *statically visible* — the same position
`code_named("…")` occupies, unlisted. If an implementation name-records it,
the record is qualified-shaped → genesis writes `int64(entryOp)` into a
head-valued slot → the first post-resume `run` derefs `+d.entry` of an op
cell — R6-B inverted. The defensible reading (runtime-resolved handle ⇒
opaque ⇒ closedness refusal) exists but is never stated for this class.

**Fix:** one clause — "a `run` operand resolved through `plan_named`/
`symbol_named` is an opaque operand" — or carry the slot convention on the
record itself rather than deriving it from name shape.

### 3. MINOR — `opcodeMap_` repopulation assumes arena-minted op cells; the "stale indices" clause is wrong

**Location:** :1585-1587 ("re-point `opcodeMap_` … and the overlay carries
stale indices into it"), :1253-1255 ("re-running `mintOpcode` at genesis"),
P15 row :2105 ("re-runs `mintOpcode` … repopulating the empty
per-`WorkerContext` map").

**Problem.** Under the image-first fold, program op cells are persistent
folded cells. `mintOpcode` *mints* a fresh arena cell (`vortex_vm.cpp:73-82`)
— re-running it at genesis mints ephemeral twins and registers the wrong
refs. `getOpcodeKind` still decodes correctly via `asInt64` fallback
(`vortex_vm.cpp:102-114`), but P15's `opcodeMap_.contains(masterOf(cell))`
predicate then misses every folded op cell — the kind-immutability funnel
loses coverage on exactly the cells it exists to guard. Also "the overlay
carries stale indices into it" is wrong as stated: `opcodeMap_` is empty
after `resume` (the doc's own :1253-1254), so nothing is stale. Under today's
all-ephemeral load the `mintOpcode` re-run is genuinely correct — the defect
is downstream of finding 1, but it needs its own sentence either way.

**Fix:** name a registration pass over the folded image — `setOpcodeKind`
(`vortex_vm.cpp:98-100`) per folded op cell — not a re-mint; and drop the
"stale indices" clause for an empty map.

## Items verified clean in the v1.6 deltas

- **Extent carrier and stability** (:1313-1317, :1589-1592, :2082-2083):
  `programRef` bind record, adapter-side, recorded once at load, asserted
  never re-derived — the per-incarnation-extent hazard stays refused; the r6
  nit ("carried as durable state" unnamed) is resolved.
- **`ensureDimension` find-or-mint** (:1587-1588, :1309-1311): resolves
  `dimensionNamed` (base) then `arenaDims_` before any mint
  (`arena_manifold.cpp:78-105`) — a promoted dim cell re-binds.
- **Shadow arms** (:1330-1333): base ref via untouched `birthOp`
  (`arena_manifold.cpp:1489-1490`); pre-extent base ⇒ program-side,
  post-extent ⇒ state-side — both verified.
- **Frames/`home`** (:1341-1347): `d.stack` hangs off *cursor* cells
  (`vortex_vm.cpp:206` choice-point read, `:864-887` Call push/Return pop);
  cursors sit on `home`'s `+d.cursors` rank (`:157-160`); `home` is
  arena-minted ephemeral; `##` anchors are ref-box ints invisible to the link
  walk. Mechanically correct.
- **Re-`learn` dedup** (:1355-1362): `insertIntoRank` no-ops on an
  already-present rank entry (`arena_manifold.cpp:1430-1431`,
  `arena_manifold.hpp:624-628` "Idempotent if @p cell is already in the rank").
- **Marker protocol** (:1515-1529, :980-987): per-ref fresh `"cell"` marker —
  `mesh_evaluator.cpp:723-735` mints inside its per-reference loop; `link`
  gives one `-d` neighbour so a shared marker is impossible
  (`arena_manifold.cpp:1352-1373`); `isCellRelocationHandle` requires
  kind *and* marker (`foreign_op_handle.cpp:98-117`); unmarked `OpHandle`
  fails `promote` outright (`arena_manifold.cpp:1797-1813`).
- **Publish ordering** (:1530-1560): `pumpOne` is one-message-per-call,
  skips stopped/empty (`:462`); `pending` is `mailbox.size()` (`:659-662`);
  `checkpointView` refuses non-empty mailbox/running/choiceDepth/marks/
  in-flight deliveries (`:672-681`). "The pump orders publish ahead of all
  queued turns while the flag is armed" is an *adapter*-loop obligation —
  sound as stated: `pumpOne` itself is flag-agnostic but the adapter owns
  both the executor callback (mutable arena, `:473-477`,
  `worker_context.hpp:63`) and the pump loop. Every interleaving path lands
  on a refusal, not a torn window: a message arriving mid-flagged-turn leaves
  the mailbox non-empty → `checkpointView` refuses → restore on the next
  turn's head → flag stays armed for retry; `ActorDown` clears outright; the
  P4 wake arm correctly *needs* the wake precisely because `pumpOne` skips
  empty mailboxes.
- **Genesis letter ordering** (:1587-1605): `ensureDimension` (c) precedes
  `+d.refs` rehydration (e) — dim ref-cells exist before `Int64` writes;
  `programRef` bind (d) precedes the shape test's module-resolution walk (e)
  and patch replay (f); rehydration (e) precedes replay (f); replay
  re-records nothing. `importValue` vs `resume` kind split is exact:
  demote-to-`None` + local-twin re-mint (`mesh_value.cpp:483-501`) vs
  verbatim `OpHandle` carrying promoted targets
  (`arena_manifold.cpp:1947-1951` writes `real.at(target)`).
- **Name-record shape machinery** (:1475-1493, :1644-1649): first-`:` split
  gated on module resolution; `src:`/`reify:` label-reserved so no module
  can reroute them; `#label`s cannot spell `:` — the three label shapes
  cannot collide with qualified names.
- **Labels/plan_named** (:1635-1670): M13 clone ordinal `+n` on the
  expansion-site `src:` consistent with the `+o:c` convention (:662,
  :926-927); durable per-agent `reify:` serial on `d.vars` survives
  `resume`; `code_named` `d.code`-only with literal heads on the
  program-image walk; `symbol_named` module-only, `plan_named` matching
  clause `+d.name` — both verbatim, no agent arm.
- **P16 row** (:2106) and **legend** (:970): consistent with §6.6 —
  qualified→`int64(entryOp)`, label→`int64(head)`, `code_named` fallback,
  flag protocol, round-trip acceptance.

## Nits (non-blocking)

- :1478's "unless a module literally takes that name (which degrades to a
  dropped patch, never a wrong bind)" is now unreachable — :1648-1649 makes
  `src:`/`reify:` label-reserved so no module can take them. Conversely
  `patch` is *not* reserved, so a module literally named `patch` can still
  reroute a `patch:x` clause name down the qualified path (same accepted
  degradation, but inconsistent with the reservation story).
- :1341-1347 — "`home` carries no link the agent cell can reach" reads
  inside-out: `home` does carry links (its `+d.cursors` rank); the claim is
  that `home` is not link-*reachable from* the agent cell.
- :1324-1325/:1581-1582 — "reports an empty program-side set by design"
  frames as an edge case what today's load path makes the default (finding
  1); the clause should be scoped to the ingress cases it names or the
  doc should say ephemeral-loaded programs get no durability.

## Summary for the orchestrator

All four of my r6 findings are resolved in v1.6 — `#label` uniqueness is
enforced at emission, the `reflect`-born arm is dropped to a closedness
refusal, the run-slot convention is stated head-valued in §6.2 and the
writeback splits with it, and the two-arm `closed` clause names the refused
class. New: **0 BLOCKING / 1 MAJOR / 2 MINOR / 3 nits.** The major sits at
the centre of the R6-A fix: "the image's ops are the spool's low prefix" is
only constructible by serializing the program image into the checkpoint
spool — which contradicts "program cells do not checkpoint" (:582), "reloaded
from `programRef`" (:580) and the transient-store model (:2279); under the
V12-faithful reading the extent names nothing, the sideness test is empty,
and `closed` admits the state→skeleton links it was built to refuse. The
doc must name the store lineage (seeded publish store or continuing actor
store) and own the V12 consequence. The minors are a qualified-name record
on a head-valued slot (`plan_named`/`symbol_named` `run` operands
unenumerated) and the `opcodeMap_` re-mint wording that the persistent-image
model breaks.

VERDICT: CONVERGED

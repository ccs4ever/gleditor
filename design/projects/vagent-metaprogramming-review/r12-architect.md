# R12 — System Architect critique (v1.11)

Scope: layering, ownership, adapter-vs-core boundaries; verification of my two r11 majors
(R11-A — `patch:<n>` ordinal recycling; R11-B — verify-arm publish destination) plus new defects in
the v1.11 deltas only. Re-verified against the tree (`manifold.{hpp,cpp}`, `arena_manifold.{hpp,cpp}`,
`store.{hpp,cpp}`, `publication.{hpp,cpp}`, `mesh_actor.cpp`, `mesh_value.{hpp,cpp}`,
`worker_context.hpp`, `vortex_core.cpp`, `vortex_stdlib.cpp`, `segmented_ops_spool.hpp`).

## R11-A — FIXED: `patch:<n>` now draws the durable serial, and the serial itself is durable

- **Mint rule.** `<n>` is "the patch's **durable monotone serial**, drawn from the same per-agent
  `d.vars` counter `reify`'s `<serial>` already rides (§6.7), never the rank position" (§6.1
  `:1158-1161`; §6.6 `:1671-1674`; §3 `:390`; glossary `:136`; changelog `:3314-3319`). The
  non-recycling half is stated: "Removing a patch never reclaims `<n>` — the `d.vars` serial stays
  spent; a later `learn_*` on the same target mints the next ordinal, and replay-dropped patches
  likewise leave their serials consumed" (`:1745-1747`), repeated at §6.7 `:2176-2179`.
- **Replay transport.** Step (d) now says it verbatim: replayed grafts "mint no patch cells, so no
  serial is consumed", and the copy's `+d.name` comes "**verbatim off the patch record** (the same
  `patch:<n>` the verb minted; replay transports the label, it never re-derives `<n>` from the
  position the replayed graft lands at)" (`:2068-2074`). Gaps in the serial are harmless —
  `plan_named`/`unlearn` match the label, never the position.
- **Counter durability — verified state-side.** The counter rides `d.vars` — on the agent-cell
  reading it is a state-side scalar: link-reachable from `agent_cell()` without transiting a
  program-side cell, ephemeral-at-mint then promoted like every agent `d.vars` member (member vars
  live on the agent's `d.vars` per `:1762-1763`; `d.vars` is in the agent-leg list `:1449`). So it
  folds back at each resume at or above every issued serial — **the ABA closes**: drops renumber the
  rank but cannot recycle the counter, and replay transports rather than re-mints, so no live
  `patch:<n>` can ever be re-issued. Sharing one counter with `reify:` is safe — the label spaces
  are disjoint (`code_named` vs `plan_named` spaces) and gaps are legal.
- **Dedupe consequence** (companion ruling) is stated at `:1173-1184`: a *qualified* alias matching
  a declared clause's stored `+d.name` unlinks it and installs `patch:<n>`, leaving the user name
  dead; unqualified aliases append; `learn_method`'s replace-vs-`learn_plan`-append asymmetry is
  drawn; `unlearn` picks twins "in any order" under unique labels, LIFO surviving only among patches
  recording one target (`:1735-1738`). `patch:`'s module-declaration reservation (`:2171-2175`) keeps
  `plan_named("patch:<n>")` on the verbatim-label path — verified coherent.
- One wording caveat worth a word: "`d.vars`" does not pin *whose* — see finding 2.

## R11-B — FIXED: the publish destination is named at every boundary and reachable today

- **Destination named uniformly.** "`publishValue` runs against the store holding the *received*
  band — the runtime's base store, `arena.base()->store()`" (`:1579-1584`), with the supply arm's
  fold making it the same object (`:1526-1535`, `:1581-1583`). The seeded store is demoted
  consistently: "only the **spawn-side lineage anchor**" (`:1524-1525`; §10.1 `:2620-2623`; §10.2
  preamble `:2681-2684`; P16 row `:2703`). The wrong-store failure is now stated on the publish
  side: "published state ops must append above the *received* band, and the seeded store's positions
  above `E_ops` are stale image-lineage" (`:1584-1586`).
- **Reachable — verified in code.** `Manifold::store()` is `const noexcept` returning non-const
  `Store*` (`manifold.hpp:486`, the mutable const-hole the doc names); `ArenaManifold::store()` is
  const-only, falling through to `base_->store()` (`arena_manifold.hpp:269-271`); `receiveValue`'s
  manifold is `store->rebuildManifold(version)` (`mesh_value.cpp:351`) and `rebuildManifold` runs
  `folded.setStore(this)` (`store.cpp:532`), so `arena.base()->store()` **is** the received store —
  non-null, mutable through the hole. `publishValue` takes `Store&` first (`mesh_value.hpp:69-74`),
  so `publishValue(*arena.base()->store(), parent, arena, root, …)` compiles today — the doc calls
  this the "de-facto supply shape" and honestly owes *sanctioned* supply/verification, not raw
  reachability (`:1596-1599`).
- **Every boundary.** Spawn seed: `historyFromSeal` fold + `setContentSource` +
  `bindPublishedLocalScroll(store->addScroll(from))` (`:1466-1477`; `historyFromSeal` discards
  `selfScrollInHistory` at `publication.cpp:1098`, `addScroll`'s `sameContentAs` returns `i+1` at
  `store.cpp:1388-1405` — the idiom works), then base-injection via `WorkerContext`'s two-arg ctor
  before `initGenesis` (`:2640-2651` — an owed seam, named). Verify destination: the comparand reads
  both public surfaces (`segmentedOps().indexOf`/`idOf`/`contains` — `store.hpp:183-187`,
  `segmented_ops_spool.hpp:272,280,287`). Checkpoint ceremony: the `Executor` callback gets
  `*actor->context` (`mesh_actor.cpp:473-477`), `WorkerContext` hands the mutable arena
  (`worker_context.hpp:63`), and the destination is the same `arena.base()->store()` reach.
- **The twin is named owed, not assumed:** "a kept-in-step adapter *twin* … is **owed a
  segment-apply surface** `adopt`/`historyFromSeal` do not provide (`adoptOpRecords` takes
  already-decoded records, `historyFromSeal` mints its own store; nothing applies a received segment
  into a store the caller already holds)" (`:1543-1547`) — accurate to the code
  (`historyFromSeal` mints `Store()` at `publication.cpp:1097`; `applyOpsSegment` is file-local).

## New findings (v1.11 deltas only)

### 1. MAJOR — the ref-capture record's `+d.grab → slot` evicts the op's own input wing: `-d.grab` on a first-position wing slot is already occupied, exactly the M14 collision the record exists to avoid (§6.1 `:1129-1132`, `:1046-1050`; §6.6 `:2093-2097`)

**Problem.** R11-C's new recipe record correlates record→slot as "`+d.grab → the same slot cell`
the ref-record names by `+d.ref`", reasoning that "a second `+d.ref` edge cannot correlate them —
the slot's `-d.ref` backlink is single-occupancy and the later link would evict the first, M14's
rule". Correct as far as it goes — but the chosen `+d.grab` hits the *same* occupancy law on a
different dim: a **first-position input wing slot's `-d.grab` is already held by the op**.
`bindInput` mints the slot and links `op +d.grab → slot` for the wing's first cell
(`vortex_core.cpp:384-390`; only *subsequent* wing cells chain `+d.step` and keep `-d.grab` free).
`link(record, +d.grab, slot)` then displaces the op: `link` clears whatever each end held
(`arena_manifold.cpp:1347-1377` — `displacedIt` arm `setOneSide(op, d.grab, POS, noCell)`), so
`linked(op, grab, POS)` returns `nullopt` and `inputsOf(op)` comes back **empty** — the record write
severs the op's *entire* input wing (slot plus the `+d.step` chain behind it orphans), both when the
emitter writes the recipe at emission and again on every `reify` replay's `Link`.

The affected class is routine, not exotic: ref-records cover "both wing slots, `reference()` cells,
and dimension-constant slots" (`:1914-1916`), and a `dimConstant`/`reference()` operand bound as an
op's **first** input is a common shape — the same shape `Call`'s `inputs[0]` operand occupies
(`vortex_vm.cpp:854-856`), differing only in carrying no `+d.name`. An implementer writing the
record as specified corrupts the fragment graph it exists to reconstruct — silently: the evicted
wing produces no error, the op just runs with no inputs.

**Fix.** Pick a correlation whose backlink on the slot is free. In order of house-style fit: (a) a
dedicated dim on the recipe record (`d.refslot`-style — "more cells and dimensions over bit-packing"
is the standing preference, and `+d.values`/`+d.step`/`+d.ref` are all already occupied or excluded);
(b) correlate through the ref-record instead — `+d.grab → the ref-record` (a no-`+d.name` record
carries no `+d.grab` role label, so its `-d.grab` is free) and reach the slot via
`refRecord +d.ref → slot`, one extra hop; (c) order ref-capture records parallel to `+d.refs` and
drop the edge — weakest, inherits ordering fragility. The eviction reasoning the doc already makes
for `+d.ref` (`:1130-1132`) should be repeated for whichever dim is chosen, since *both* the slot's
`-d.ref` (ref-record) and `-d.grab` (op wing, first-position inputs) are occupied today.

### 2. MINOR — "durable per-agent counter on `d.vars`" does not pin the agent cell's `d.vars` — the module reading silently reintroduces the ABA (§6.1 `:1158-1159`; §6.6 `:1671-1672`; §6.7 `:2176-2179`)

**Problem.** `d.vars` is overloaded in this doc: module `d.vars` holds *symbols*
(`symbol_named`'s walk, `:2201-2204`), the agent cell's `d.vars` holds member state (`:1762-1763`).
The counter must sit on the **agent's** `d.vars` — a counter appended to the *module's* rank hangs
off a program-side rank head, so it is reachable from the agent cell only by transiting a
program-side cell → fails the state-side transit clause (`:1606-1609`) → excluded from `promote` →
resets to zero each resume → recycled `patch:<n>`, exactly the ABA this fix kills. The intended
reading is the only consistent one, but the word that carries it is absent. **Fix:** "the same
per-agent counter on the *agent cell's* `d.vars`".

### 3. MINOR — verify-arm wording conflates the comparand's subject with the comparison's operands (§6.6 `:1526-1535` vs `:1989-1996`)

**Problem.** "Under the verify arm the comparand is the runtime's base store's own `1..E_ops` band"
reads as a same-store self-comparison — while (a) specifies the operative shape explicitly: the
adapter "decodes its adopted lineage store's own `1..E_ops` band … and asserts the received store's
decoded `1..E_ops` records equal it pairwise". The seeded store remains the comparand's *reference*;
the received store is its *subject*. The trailing `real.at` clause is a publish-destination argument
wearing comparand vocabulary. (a) is explicit enough to recover the right construction, but the seam
sentence could send an implementer to compare the received band against the `programRef` enumeration
and drop the seeded store's role. **Fix:** "under verify, the comparand's *subject* is the runtime's
base store's `1..E_ops` band, reached through `arena.base()->store()`, still compared pairwise
against the adapter's seeded band as (a) states".

### 4. MINOR — (d)'s "verbatim off the patch record" does not pin re-mint vs re-link; a re-link evicts the record's own label and silently drops `patch:<n>` at the *second* genesis (§6.6 `:2068-2074`)

**Problem.** `+d.name` is a single-link label dim (`:1441-1442`): the patch record's label cell has
its `-d.name` held by the record. If replay transports by *linking* that folded cell onto the
installed clause, the same M14 eviction strips the patch record's `+d.name` — the record then has no
label to transport at the next resume and `patch:<n>` silently disappears across a second
checkpoint. The natural reading is re-mint ("content"), but `verbatim` is doing load-bearing work
unmarked. **Fix:** "the copy's `+d.name` re-minted as a fresh scalar with the patch record's
verbatim label *content*".

### 5. MINOR — qualified-alias dedupe unlinks a declared clause with no recorded displaced clause; `unlearn` of the install leaves it session-dead until resume (§6.1 `:1173-1184`; §6.6 `:1731-1745`)

**Problem.** `learn_plan("Feed", "demo:Forager.travel", …)` unlinks the *declared* `travel` clause —
a session mutation recorded nowhere on the patch (the patch records the installed `patch:<n>`
target, not the displaced clause). `unlearn(patch:<n>)` then pops the install but cannot re-link
`travel`; it returns only at the next resume, when the image folds it back and the dropped install
patch no longer unlinks it — so in-session state and post-resume state diverge. Symmetric with
`learn_method`'s long-standing redefinition semantics, so arguably accepted discipline — but the
learn_plan arm is new this round and the divergence is unnamed. **Fix:** one clause: either the
patch records the displaced clause so `unlearn` re-links it, or "a dedupe-displaced declared clause
is dead until the next genesis" is stated as the rule.

## Nits

- **`:1166` "the ordinal read off the `d.patch` rank"** — the readable datum is the patch *record's*
  `ordinal` field (= the serial per `:1671`,`:390`), not the rank position; the phrasing invites
  exactly the positional reading the fix forbids.
- **`:1547` "owed surface under §10.2"** — §10.2's preamble and P16's `Where` cell carry
  base-injection + supply/verify only; the segment-apply surface is named at `:1543-1547` but the
  forward pointer lands nowhere.
- **The serial counter is a plain `d.vars` scalar** — writable through `cells"…"` like any member;
  corrupting it now poisons `patch:` labels as well as `reify:` labels (self-inflicted class,
  pre-existing for `reify:`; a reserved key name would pin it).

## Verified v1.11 deltas (no finding)

- **(b) enumeration pinned to `produces`/`post-fold order`**: `opsSpool.contains(record.produces)`
  (`store.cpp:3841`), `indexOf`/`idOf` public on `SegmentedOpsSpool`
  (`segmented_ops_spool.hpp:272,280,287`) via `store.segmentedOps()` (`store.hpp:183-187`);
  topo-sort with min-`produces` heap verified (`store.cpp:3836-3940`); the linear-`parent`-chain
  invariant is stated as the required condition for positional recording, not assumed.
- **`d.contract` op-anchored**: `vortex_stdlib.cpp:603` ("Preconditions along `-d.contract`") and
  `:628` ("Postconditions along `+d.contract`") — on op cells inside `curOp` walks, correctly moved
  to the program() walk's vocabulary; the agent-leg list (`+d.code`, `d.patch`, `d.synapse`,
  `d.weight`, `d.belief`) and the second walk's consumer (`closed`'s frontier enumeration,
  `:1447-1457`) are coherent.
- **Seed stakes split**: `sealPendingMetadataAsCells`' `publishedLocalScroll_ == localScroll` arm at
  `store.cpp:4081`, `scrollRegistry`'s resolved-scroll set at `manifold.cpp:1503`, the storeTables
  round-trip at `store.cpp:4176`,`:4454` — all verified; `setContentSource` carries the read path,
  the bind carries scroll identity.
- **Seed resolve loop**: `publication.cpp:1138-1162` verified (`metainfo` → `Scroll::ofTorrentFile`
  → `resolver.resolve` → `isVerified`/length → `applyOpsSegment`), signed-inventory skip honestly
  stated.
- **`learn_method` corollaries** (`:1805-1808`), the `unlearn` symbol arm's `module:` re-composition
  via the `-d.vars` walk (`:1738-1745`), and the alias-as-write-only-metadata idiom
  (`clauses_of` + `patch:` prefix / `plan_named("patch:<n>")` / `label_of`) — consistent end to end.

## Structural notes

- Publishing into `arena.base()->store()` under verify mutates `actor->checkpoint`'s private store —
  the doc names the `job.base` shared-store precedent for exactly this shape (`mesh_jobs.cpp:314`,
  `:322`) and asks for *sanctioned* surface rather than blessing the const-hole; the honest framing
  holds.
- Under the supply arm `arena.base()->store()` and the adapter's seeded store coincide by
  construction, so the single destination spelling covers both arms — the doc says so at `:1581-1583`.

VERDICT: NOT CONVERGED

# r6-codebase: Round-6 codebase verification of `design/vagent-metaprogramming.md` v1.5

**Orchestrator summary (≤200 words):** All four r5 minors are fixed correctly — no `CellImport`,
no `MeshOp`, no "pre-promotion targets", `rankTail` now at `cell_views.hpp:258`; canonical
`reify:<src>:<serial>` holds. The R5-A rewrite is mechanically **sound**: a persistent `CellRef`
is the ops-spool index (`store.hpp:677-681` `cellRefOf` → `opsSpool.indexOf`), so "the maximum
persistent `CellRef` at program load" is a real monotone boundary; every ephemeral ref
(`ephemeralBit|dense`, `manifold.hpp:154`, `arena_manifold.hpp:468-470`) is ≥2³¹ and therefore
always above it; shadows answer to their base ref (`arena_manifold.cpp:1489-1490`,
`overlay_`/`hydratePrefix` `:2110-2113`), and post-load promotes mint strictly larger indices, so
post-extent persistent refs stay state-side exactly as :1300-1308 claims. Every file:line cite in
the v1.5 deltas verifies (~35 checked). Counts: **0 BLOCKING, 0 MAJOR, 2 MINOR, 3 NIT**. Top
issues: **(1)** "`home`'s `+d.cursors`/`+d.stack` ranks" (:1314) — `d.stack` hangs off *cursor*
cells (`vortex_vm.cpp:206`,`:865-870`), never home (:159-160 put cursors on home's `+d.cursors`).
**(2)** "`ensureDimension` re-mints dim cells at genesis" (:1295) — `ensureDimension` is
find-or-mint (`arena_manifold.cpp:78-105`): base dims resolve first via `dimensionNamed` (:58-70),
session dims via `arenaDims_` (:83-86, hydrated `:2105`); it re-binds, re-minting only absent
names. **VERDICT: NOT CONVERGED** — wording-level minors only, converged in substance.

## Verdicts on r5 findings (codebase-1..4 + disposition items)

### Finding 1 (`CellImport{OpHandle}` fabricated, inverted `importValue` semantics) — RESOLVED

v1.5 :1532-1535: "`importValue` re-mints each relocation-marked cell `OpHandle`-kind naming the
*local* twin of its promoted target and demotes unmarked `OpHandle` cells to `None`
(`mesh_value.cpp:485-501`)". Verified exactly: the demote arm is `:485-490`
(`OpHandle → None`, bits 0), the marked re-kind is `:494-501`
(`setValueBits(local.at(ref), OpHandle, local.at(*target))`) — "local twin" = `local.at`, correct.
Zero `CellImport` occurrences repo-wide (doc included). The wrong-file cite
(`arena_manifold.cpp:1934-1954`) no longer attaches to the `importValue` claim and correctly marks
promote's own mint switch at :1509-1510 and :1537.

### Finding 2 ("handles carrying pre-promotion targets" — inverted) — RESOLVED

v1.5 :1536-1538: "handles carrying their *promoted* targets (`arena_manifold.cpp:1947-1951`
writes `real.at(target)`) — and rehydration `writeValue(slot, Int64)`s each marked cell back with
the promoted target". `:1947-1951` is exactly the `OpHandle` mint arm:
`isEphemeral(target) ? real.at(target) : target`. Direction now correct.

### Finding 3 ("no `MeshOp` kind" — ghost `ValueKind`) — RESOLVED

v1.5 :1501-1502: "no handle-kind cell (`OpHandle`/`ForeignOpHandle`, `ops.hpp:154-163`) is
executable op content". Enum verified at `ops.hpp:154-163` (`None=0 … OpHandle=4 …
ForeignOpHandle=7`); the claim's mechanism holds two ways: `getOpcodeKind` is map-first then
`asInt64` (`vortex_vm.cpp:102-114`), and `asInt64` requires `ValueKind::Int64`
(`arena_manifold.cpp:1083-1086`), so an `OpHandle`-kind cell can never decode — and `Choice`
independently enforces `getOpcodeKind` on its target (`vortex_vm.cpp:1156-1158`). No `MeshOp`
remnant anywhere.

### Finding 4 (`rankTail` cited at wrong file) — RESOLVED

v1.5 :1357-1358 now reads "`insertIntoRank`/`rankTail` (`arena_manifold.cpp:1423-1439`,
`cell_views.hpp:258`; the `addClause` precedent `vortex_stdlib.cpp:4238-4241`)". All three
verified: `insertIntoRank` :1423-1439 with the `rankContains` idempotent early-return :1430-1432
(declaration comment "Idempotent if already in the rank" `arena_manifold.hpp:624-628`, correctly
cited at :1326); `rankTail` at `cell_views.hpp:258-262` (plus the `optional` overload :266);
`addClause`'s `link(rankTail(arena, predCell, clause), clause, clauseCell)` at
`vortex_stdlib.cpp:4238-4241`.

### `reify:<serial>` spelling (disposition minor, ergo/writer/zigzag's) — RESOLVED

Canonical `reify:<src>:<serial>` at :1571-1572; the remaining `reify:` mentions (:1329, :1439,
:2395) are generic prefix references, not malformed spellings. No bare `reify:<serial>` remains.

## R5-A mechanism verification — ref-space ordering is sound

- `birthOp` semantics exactly as the disposition ruled: arena mints get
  `.birthOp = refOf(dense)` — self-identity, not a timestamp
  (`arena_manifold.cpp:1183-1186`, comment "An arena cell has no birth operation"); a shadow's
  `birthOp` comes across untouched — the *base* ref (`:1489-1490`).
- **Ordering is real**: persistent `CellRef` = ops-spool index (`store.hpp:677-681`), monotone
  with append order. Ephemeral refs carry bit 31 (`manifold.hpp:154`; `refOf`,
  `arena_manifold.hpp:468-470`), so *every* arena ref is structurally above *any* persistent
  extent — :1301-1303's "always above the boundary" holds by construction, not just mint order.
- Shadow addressing verified: `overlay_.emplace(ref, dense)` at :1501 maps base ref → dense;
  `hydratePrefix` rebuilds it from non-ephemeral `birthOp`s (:2110-2113). A walk reaching a
  program cell through a program edge sees the persistent base ref → pre-extent → program-side,
  :1305-1307's "resurrected program image" case ✓. A graft promoted in a later checkpoint gets a
  larger op index → post-extent → state-side across `resume`, :1307-1308 ✓.
- `d.dims` skip at `arena_manifold.cpp:1766` (`textOf(edge.dim) == "d.dims"`) — exact ✓;
  both-direction walk `discover(edge.pos)`/`discover(edge.neg)` at :1782-1783 ✓.
- Relocation dichotomy at :1426-1429 verified against the gate: a `handleTarget` is enqueued
  into `seen`/`frontier` at :1759-1764 *before* the :1755 base-skip, so a program-cell target
  lands in `order` and is re-minted (the "drag in" arm); a missing target gives `nullopt` at
  :1761 and an ephemeral-but-unseen one at :1802-1805 (the "excluded" arm). Both arms real.
- Base-target arm at :1516-1519 verified: non-ephemeral `*target` skips the deferred path and
  passes `:1809` `getCompactOp` (every persistent ref is an existing op index), mints verbatim
  via `:1950`'s `target` fallback — restore's `Int64(*handleTarget)` writes the identical value.
- `outbound-following` program-side walk vs `promote`'s either-direction state-side walk — the
  asymmetry is stated explicitly (:1290, :1309) and each matches its purpose.
- The `d.extends`/`d.belief`/`d.source`/`d.code`/`d.refs`/`d.recipe`/`d.entry` names are
  doc-schema dims (no `d.extends` string exists in the tree; `extends` is a real vagent keyword,
  `parser.cpp:552,577`, `token.cpp:28`) — cited as design vocabulary, not claimed code, so no
  citation error; `d.dims`/`d.grab`/`d.step`/`d.spin`/`d.vars`/`d.values`/`d.name`/`d.stdlib`/
  `d.clause`/`d.contract` are real (`vortex_core.cpp:44-63`).

## Flag/genesis ceremony — all cites verified

`publishValue` `const ArenaManifold&` `mesh_value.hpp:69-74`; `checkpointView` `const ArenaManifold&`
view `mesh_actor.cpp:668-693`, `mesh_actor.hpp:102-105`; `executor_(…, *actor->context)` inside
`pumpOne` `:473-477`; `WorkerContext::arena` public `worker_context.hpp:63`; `pending` =
`mailbox.size()` `:659-662`; `pumpOne` empty-mailbox/stopped skip `:462`; `checkpointView`
non-empty-mailbox refusal `:672`; stopped-on-failure `:501`; P4 `wake(pid)` row verbatim at
`vagent-agent-language.md:1670` (approved, unimplemented — matches the softened "no new mechanism
beyond P4" wording). `isCellRelocationHandle` requires `OpHandle` kind *and* the
`d.mesh.relocation`→`"cell"` edge (`foreign_op_handle.cpp:98-117` both overloads; strings
`foreign_op_handle.hpp:24-25`). Per-ref marker mint inside the loop `mesh_evaluator.cpp:723-736`.
One-`-d`-neighbour eviction `arena_manifold.cpp:1352-1373`. `setValueBits` signature
`arena_manifold.hpp:590-591`; `handleTarget` `arena_manifold.cpp:1091-1098` (returns
`optional<CellRef>` — `*handleTarget` derefs correctly). Uniform rule "every
`makeScalarCell(int64(ref))`" holds at all four emission sites: `bindInput` :384, `bindOutput`
:402, `reference()` `vagent/compiler.cpp:516-518`, `dimConstant` `:769-772`
(`literal(int64(findOrMintDimension))`). Mark guard `vagent/compiler.cpp:108-110`. `mintOpcode`
`vortex_vm.cpp:73-82`; `opcodeMap_` private `vortex_vm.hpp:186`. `resume` overlay without
`importValue` `mesh_actor.cpp:260-271` (`WorkerContext(actor->checkpoint->manifold.get())` at
:270-271). `consumeWork` throws `arena_manifold.hpp:420-426`. `hydratePrefix`
`outstandingMarks_ = 1` `:2123`; `or_parallel.cpp:349`. Int64 control-flow targets: `Jump` :839,
`Branch` :848, `Call` :856, `Choice` :1150-1168 — the §11-door clause at :1316-1318/:1399-1403 is
mechanically accurate. `insertIntoRank`/`rankTail`/`addClause` — above. `exportSymbol`
`vortex_core.cpp:119-127` binds `sym +d.values → entryOp` at :123 — the §6.1 head/op
discriminator restated at :986-988 and :1434-1437 is uniform. M8 memo records
`{+d.values, +d.name→label, +d.grab→fresh scalar key}` at :286-296 match the :1337-1339 restate —
the `+d.name` key-eviction ruling applied correctly.

## New findings (v1.5 deltas only)

### Finding 1 — MINOR

- **Location:** :1313-1314.
- **Problem:** "live frame cells … sit behind program-side anchors (… `home`'s
  `+d.cursors`/`+d.stack` ranks)". `d.stack` never hangs on `home`: `spawnCursor` links the new
  cursor onto `rankTail(home, d.cursors)` (`vortex_vm.cpp:159-160`), while every `d.stack` edge is
  on a *cursor* cell (`:206`,`:865-870`,`:881-887` — all `link(*cursor, dims().stack, …)`). Frames
  do sit behind home transitively (home `+d.cursors` → cursor `+d.stack` → frames), so the
  transit-clause conclusion is unaffected — only the attribution is wrong.
- **Concrete fix:** "`home`'s `+d.cursors` rank (and each cursor's `+d.stack` frames)".

### Finding 2 — MINOR

- **Location:** :1295.
- **Problem:** "`ensureDimension` re-mints dim cells at genesis". `ensureDimension` is
  find-or-mint (`arena_manifold.cpp:78-105`): `dimensionNamed` resolves base dims *first*
  (`:58-70`), then the `arenaDims_` cache (`:83-86`, which `hydratePrefix` restores verbatim,
  `:2105`); a cell is minted only for a name absent from both. At genesis over a checkpointed
  base the data-minted dims were promoted into it (`discover(edge.dim, false)`, `:1781`) and
  resolve there — nothing is re-minted, and under current semantics nothing *can* be (a base hit
  short-circuits before the mint). The `d.dims` exclusion stands on the first arm alone
  (`promote` skips `d.dims`, `:1766`); the second justification overstates the mechanism.
- **Concrete fix:** "genesis re-binds data-minted dims through `ensureDimension` (minting only
  names absent from the base)" — or drop the clause; do not claim re-mint unless the design
  requires bypassing `dimensionNamed`.

### Finding 3 — NIT

- **Location:** :1475.
- **Problem:** "The mark dimension is `d.ref`/`d.mesh.relocation`". The mark dim is
  `d.mesh.relocation` alone (`foreign_op_handle.hpp:24`); `d.ref` is the `+d.refs` *record* edge
  and — separately — a real generic lens dim for cross-edges (`projection_lens.hpp:54`). The
  slash-joined phrasing reads as one dim pair; the unambiguous usage is the :94 inventory's
  "`+d.refs`/`d.ref`/`d.mesh.relocation` marks" and the :960-963 diagram.
- **Concrete fix:** "the mark is `+d.ref` on the record plus `d.mesh.relocation` on the cell".

### Finding 4 — NIT

- **Location:** :2030 (P16 row).
- **Problem:** "`writeValue` covers Int64/Double/Bool/None, `vortex_core.cpp:203-227`" — the
  function is :196-229; :203-227 names only the kind arms, omitting the signature/guard
  (:196-202). Elsewhere the doc cites the whole function (:2029's P15 row uses `:196-254` for the
  funnel). The claim itself is accurate — cosmetic range only.
- **Concrete fix:** cite `:196-229` (or `:196-254` for the funnel pair).

### Finding 5 — NIT

- **Location:** :1408-1410 (adjacent to the :1424-1453 delta; same machinery).
- **Problem:** "a `code_named`/`reflect`-born operand → its `d.name` label". A `code_named` head
  has one — the label is the lookup key — but a reflect wrapper carries **no** `+d.name`: M8
  (:286-296) and the :1337-1339 restate put the memo-kind label on the memo *record*, never on
  the wrapper head; `label_of` (:1006) would return `Opt` empty on one. As spec'd, the
  reflect-born arm names a label that nothing mints — the real path for a wrapper operand is the
  "opaque → closedness refusal" fallback (:1410-1411).
- **Concrete fix:** either give wrappers a derivable `+d.name` (e.g. clause qualified name +
  memo-kind) and say so, or drop the `reflect`-born arm and let wrappers hit the refusal.

VERDICT: NOT CONVERGED — 0 blocking, 0 major, 2 minor, 3 nit; all wording-level and localized.
Every r5 finding is correctly resolved, and the ref-space-ordering rewrite is the first sideness
discriminator with a sound mechanism backing (spool-index ordering, not a timestamp).

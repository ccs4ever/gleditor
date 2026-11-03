# Round 5 critique (vortex role): vagent-metaprogramming.md v1.4

Lane: VortexCore/VM execution semantics, trail/marks, handle relocation, promote,
MeshActorRuntime invariants. Convergence check on the v1.4 deltas, verified against the
tree: `ArenaManifold` `birthOp` semantics (`arena_manifold.cpp:1183-1186`,
`arena_manifold.hpp:43-46`), overlay/hydrate (`arena_manifold.cpp:2099-2125`,
`worker_context.hpp:52-54`), promote arms (`arena_manifold.cpp:1755-1813`,
`:1934-1954`), `pending`/`pumpOne`/`checkpointView` (`mesh_actor.cpp:659-662`, `:462`,
`:668-677`), executor seam (`mesh_actor.cpp:473-480`), `exportSymbol`
(`vortex_core.cpp:119-127`), `insertIntoRank` (`arena_manifold.cpp:1423-1439`,
`arena_manifold.hpp:624-628`), `isCellRelocationHandle` (`foreign_op_handle.cpp:98-117`),
per-reference markers (`mesh_evaluator.cpp:723-735`), `Call`/`toInt`
(`vortex_vm.cpp:854-856`), `bindInput`/`bindOutput` int64 wings
(`vortex_core.cpp:384,402`), `dimConstant` (`vagent/compiler.cpp:769-772`), drain idiom
(`vortex_stdlib.cpp:3800-3802`, `:3841-3843`), `ephemeralBit = 0x8000'0000`
(`manifold.hpp:154`), P4 wake (`design/vagent-agent-language.md:81`, `:320-322`).

## Verdicts on r4 findings (vortex-1..8)

1. **vortex-1 (MAJOR — sideness dim enumeration) — resolved.** The walk now names the
   full structural vocabulary — `d.stdlib`, `d.vars`, `d.clause`, `+d.values`, `+d.code`,
   `+d.entry`, `+d.name`, `d.name`, `+d.refs`, `+d.recipe`, `+d.grab`, `+d.spin`,
   `+d.step`, `d.contract`, `d.belief`, `d.source` (:1270-1272) — with the
   exhaustiveness obligation and bound ≥ skeleton depth stated (:1272-1273), the
   int64-endpoint door clause applied to both arms (:1279-1282, :1356-1360), the
   `code"…"` anchor edge named (a `+d.values` edge from the emission record,
   :1262-1265), and `closed`'s state-side arm sharing the test (:1279). The added
   conjunct — the `birthOp` mint window — is the new mechanism and carries the round's
   new defect (finding 1 below).
2. **vortex-2 (serial/`textOf`) — resolved.** "a *fresh `Int64` scalar cell minted per
   visited cell per pass* holding the pass serial — the serial itself drawn from the
   durable per-agent `d.vars` counter, the same source `reify`'s `<serial>` uses …
   `value_of(tag) == serial`" (:1576-1579). Stale-tag eviction and the promoted-tag
   path are handled: a post-resume pass draws a new serial from the *durable* counter,
   so no remint can collide with a promoted tag's stored value; an old tag fails the
   comparison (:1582-1584).
3. **vortex-3 ("Int64-stale" contradiction) — resolved.** Genesis (e) now reads
   "`resume`/`genesis` receives the `OpHandle`-kind cells verbatim … and rehydration
   `writeValue(slot, Int64)`s each marked cell back with the promoted target"
   (:1465-1470); the name-record class is re-bound by name in the same step, no longer
   conflated.
4. **vortex-4 (kind-skip uniformity) — resolved.** "Each of the three passes — re-kind,
   restore, genesis rehydrate — skips cells already in its target kind before reading
   `asInt64`/`handleTarget`" (:1451-1453); the restore pass additionally guards on
   "still in `OpHandle` kind" (:1448-1450). Matches `handleTarget`'s `nullopt` on
   non-`OpHandle` (`arena_manifold.cpp:1090-1097`).
5. **vortex-5 (release parenthetical) — resolved.** "`choiceDepth() == 0` *and*
   `outstandingMarks() == 1` — `m0` itself, nothing else standing" (:1189-1191);
   `checkpointView` requires `outstandingMarks() == 0` outright with the
   `hydratePrefix` `==1` floor named as the parallel-worker artifact
   (:1191-1194; `mesh_actor.cpp:672-675`, `arena_manifold.cpp:2123`,
   `or_parallel.cpp:349`).
6. **vortex-6 (M11 seam + `checkpointView` cite) — resolved.** M11 now describes the
   flag-driven `Executor`-callback ceremony and defers to §6.6 (:352-357);
   `checkpointView` is correctly cited to `mesh_actor.cpp:668-694` /
   `mesh_actor.hpp:102-105` (:1406).
7. **vortex-7 (re-learn carve-out) — resolved.** "Re-`learn`ing an already-registered
   head is the carve-out: the argument is reused as its own registration — the second
   `reify` is skipped and `insertIntoRank` no-ops … Only that arg-form dedupes"
   (:1287-1294), matching `insertIntoRank`'s documented idempotence
   (`arena_manifold.hpp:624-628`, `rankContains` at `arena_manifold.cpp:1430`).
8. **vortex-8 (`code_named` "both reachable") — resolved.** "`code_named` walks
   `d.code` only: the program-image twin … is unreachable by construction — a
   `code"…"` literal is never `d.code`-registered — so no rank-order tie-break is ever
   needed" (:1514-1516).

## New findings (v1.4 deltas only)

### 1. MAJOR — §6.6: the `birthOp` mint window rests on an undefined boundary and a misdescribed field — the test works only through unstated premises

**Location:** §6.6 :1273-1278 (sideness mint-window conjunct); changelog :2274.

**Problem.** The new conjunct reads: "minted within the program's own mint window:
`birthOp` at or below the program-image boundary the genesis `programRef` bind marks
(`birthOp` is recorded per slot, `arena_manifold.cpp:1183-1186`, and consulted by the
relocation path, `mesh_value.cpp:399`)".

- **The cited field is not what the sentence implies.** `arena_manifold.cpp:1183-1186`
  mints `.birthOp = refOf(dense)` under the comment "**An arena cell has no birth
  operation.** birthOp carries its own ref so that code shared with Manifold — which
  reads slot.birthOp to name the cell a slot belongs to — means the same thing here."
  For a *shadow*, `birthOp` is the base cell's persistent ref
  (`arena_manifold.hpp:43-46`; `hydratePrefix` rebuilds `overlay_[birthOp]` from
  non-ephemeral birthOps, `arena_manifold.cpp:2110-2113`). `mesh_value.cpp:399` consults
  it as an *identity* test (`slot(*target)->birthOp != *target` ⇒ "not a cell"), not a
  timestamp. There is no per-slot mint time in the arena — the "mint window" reduces to
  **persistent-ref ordering**, and it works only because of two premises the doc never
  states: (i) in `Manifold`, a cell's ref *is* its birth op, so persistent refs order by
  append — program-image ops precede every promoted op; (ii) arena refs are
  `ephemeralBit | dense` (`refOf`, `arena_manifold.hpp:468-470`; `manifold.hpp:154`),
  i.e. numerically above *any* persistent boundary, so every arena-minted cell —
  including a grafted fragment's cells — is automatically outside the window.
- **The boundary itself is never constructed.** "The program-image boundary the genesis
  `programRef` bind marks" is the only definition offered; the genesis step (d) merely
  "bind `programRef → the program root` by the V10 mechanism" (:1461). Three readings
  are open: (a) the bound root cell's own persistent ref — correct only if the root is
  the image's high-water op (unstated); (b) a recorded extent of the program image —
  correct and durable, but never said to be recorded or carried; (c) an extent measured
  at each genesis — *wrong*: the overlaid checkpoint already contains every promoted
  state/learned-fragment cell, so a per-incarnation extent absorbs them all into the
  window, classifying post-resume state and learned fragments as program-side and
  turning every post-resume `learn` capture/link into a `closed` refusal. The doc's own
  example ("a `learn`ed clause sitting on a program `d.clause` rank does not become
  program-side", :1276-1278) is true only under readings (a)/(b) — which the text does
  not choose between, because it never says what the mark *is*.
- **Unstated consequence.** Artifact-*initial* state cells (bindings the shipped image
  contains) fall at or below the boundary → program-side → a `learn`ed fragment
  capturing one is refused unless name-resolved. The doc's capture rule names "fields
  and module vars" as capture-able (:1347) without noting the artifact-initial case is
  program-side; whether that restriction is intended should be said.

**Evidence:** `design/vagent-metaprogramming.md:1273-1278, :1461, :2274`;
`arena_manifold.cpp:1183-1186` (self-ref + "no birth operation" comment),
`arena_manifold.hpp:43-46` (shadow birthOp = base ref), `:468-470` (`refOf` =
`ephemeralBit|dense`), `manifold.hpp:154` (`ephemeralBit`), `mesh_value.cpp:399`
(identity test), `arena_manifold.cpp:2099-2125` (hydrate preserves slot/birthOp),
`worker_context.hpp:52-54` (actor arena overlays the checkpoint `Manifold` — program
cells are base cells), `arena_manifold.cpp:1755` (promote skips non-ephemeral cells:
program cells are never re-minted, keeping op ids stable below the boundary).

**Fix:** one clause replacing the parenthetical — state that the boundary is the
program image's persistent high-water ref **recorded once at artifact build and carried
in the checkpoint** (not recomputed at genesis, since the overlay already contains
promoted cells), and name the two premises: persistent refs order by append; arena refs
carry `ephemeralBit` and therefore always exceed it ("`birthOp` answers 'which cell',
not 'when': for persistent cells it *is* the birth op — ordered by append — and for
arena-minted cells an `ephemeralBit` self-ref above any persistent boundary"). Optionally
note the artifact-initial-state consequence for captures.

### 2. MINOR — §6.6: genesis (e)'s rehydration walk is unscoped; read literally it contradicts the unlearned-graft stale-call boundary it sits beside

**Location:** §6.6 :1442-1445 (boundary claim) vs :1463-1465 (genesis e).

**Problem.** Step 3 scopes the *re-kind* walk to "each registered head's `+d.refs`
ref-records" and concludes: "an installed-but-unlearned graft's ref cells are never
re-kinded … so an unlearned graft may structurally survive a checkpoint with stale
calls; `learn_*` is what makes a graft's references durable." But genesis (e) says
"`+d.refs` rehydration — per ref-record `Int64(*handleTarget)`, per name-record the
`symbol_named → +d.values → … → writeValue` chain above" with **no walk domain**.
Every emitted fragment head carries `+d.refs` records — including unlearned grafted
heads (:1419, :944-948). If (e) walks every reachable head's records — the natural
reading — an unlearned graft's *name*-records get rebound at genesis: its name-bound
`Call`s are durable without `learn`, contradicting the very sentence that motivated
the boundary claim (the kind-guard makes its *ref*-records no-op safely, since they
were never re-kinded). Either (e) must be scoped to registered heads' records — which
makes the stale-call claim exactly true — or the claim must be qualified to "unlearned
grafts' anonymous refs stay stale; name-bound calls rebind".

**Evidence:** `design/vagent-metaprogramming.md:944-948, :1419, :1442-1445, :1463-1465`.

**Fix:** "…(e) `+d.refs` rehydration over **each registered head's** records — per
ref-record …", or qualify the boundary claim.

### 3. MINOR — §6.6 genesis (e): "handles carrying pre-promotion targets" inverts what `makeOpHandle` stores

**Location:** §6.6 :1468-1470.

**Problem.** "`resume`/`genesis` receives the `OpHandle`-kind cells verbatim — handles
carrying pre-promotion targets — and rehydration `writeValue(slot, Int64)`s each marked
cell back with the promoted target". `promote` writes
`store.makeOpHandle(version, isEphemeral(target) ? real.at(target) : target, …)`
(`arena_manifold.cpp:1947-1951`) — the stored handle carries the *promotion-remapped*
(persistent) target, not the pre-promotion session value; the pre-promotion `Int64`
is exactly what is *not* recoverable from the wire. The operative next clause ("writes
back the promoted target") is correct, so the sentence is self-contradictory on its
literal reading.

**Fix:** "handles carrying the *remapped* (promoted) targets".

## Items verified clean in the deltas

- **P16 flag protocol** (:1427-1456): `pending(pid)` = `mailbox.size()`
  (`mesh_actor.cpp:659-662`); `pumpOne` skips empty mailboxes (:462); `checkpointView`
  refuses non-empty mailbox plus `running`/`choiceDepth`/`outstandingMarks`
  (:672-675) — so `pending(pid) == 1` arming is sound, a late-arriving message degrades
  to the publish-refusal path, and the empty-mailbox P4 `wake` is a defined mechanism
  (`vagent-agent-language.md:81`, `:320-322`) that bypasses the `pumpOne` skip.
  Two-phase flag + kind-guarded restore + drain-before-normal-turns is consistent on
  every exit path including `ActorDown` (stopped actor discards the arena; the
  published checkpoint already carries the handles).
- **`+d.name` discriminator** (:1379-1385, :1413-1419): name-records get no marker and
  are never re-kinded — matches `promote`'s `isCellRelocationHandle` gate
  (`foreign_op_handle.cpp:98-117` requires OpHandle *and* marker) and its
  `handleTarget`-enqueue (`arena_manifold.cpp:1759-1764`).
- **Name-record rebind chain** (:1386-1394): `exportSymbol` binds `sym +d.values →
  entryOp` directly (`vortex_core.cpp:119-127`), so the op-or-head discriminator at the
  hop is correct; the `run`-target `code_named → +d.entry` path is consistent.
- **Verbatim labels** (:1502-1516): opaque `src:`/`reify:`/`#label` strings, first-`:`
  split scoped to qualified-name lookups only when the segment resolves as a module —
  internally consistent; `code_named` walks `d.code` only.
- **Release/checkpoint asides** (:1189-1194), **same-dim rank convention**
  (:1317-1319; `insertIntoRank`/`rankTail` `arena_manifold.cpp:1423-1439`, `addClause`
  `vortex_stdlib.cpp:4238-4241`), **`opcodeMap_` genesis step** (:1459-1461),
  **navigate_to recast** (:1399-1403), **unlearned-graft boundary wording**
  (:1442-1445, modulo finding 2), **M11 rewrite** (:352-357), **uniform ref-cell
  marking incl. `dimConstant`** (:1416-1418; `vagent/compiler.cpp:769-772` mints int64
  dim refs — including them is correct since data-minted dims are arena cells),
  **`consumeWork`/`ActorDown` channel** (:1198-1202; `arena_manifold.hpp:420-426`,
  `mesh_actor.cpp:473-486`).

---

**Summary for the orchestrator:** all eight of my r4 findings are resolved in v1.4.
New: **0 BLOCKING / 1 MAJOR / 2 MINOR.** The major: the mint-window conjunct — the new
half of the R4-A fix — cites `birthOp` as if it recorded mint time, but the cited lines
say the opposite ("An arena cell has no birth operation"); the window works only via
unstated premises (persistent refs order by append; `ephemeralBit` lifts arena refs
above any persistent boundary), and the boundary value itself is never constructed —
"the `programRef` bind marks" admits a per-incarnation extent that absorbs promoted
cells and breaks post-resume `learn`. Fix is ~2 clauses. Minors: genesis-(e) walk
domain vs the unlearned-graft stale-call claim; "pre-promotion targets" inversion. The
rest of the delta verifies mechanically: flag protocol, discriminator, name-record
chain, verbatim labels, kind-guards, serial provenance.

VERDICT: NOT CONVERGED

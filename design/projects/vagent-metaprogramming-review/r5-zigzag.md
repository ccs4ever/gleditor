# R5 — Xanadu / ZigZag topology critique (v1.4)

Lane: cell-graph topology — dimension discipline, rank invariants, link eviction, promotion
reachability, cell/name identity. Convergence check on the r4 disposition.

Verified against `apps/common/xanadu/zigzag/arena_manifold.cpp` (`link` reciprocal eviction
1347–1377; `insertIntoRank`/`rankContains` same-dim 1381–1440; `promote` BFS 1744–1788, base-cell
endpoint skip 1755–1757, relocation pull-in 1759–1764, `OpHandle` deferred/base arms 1797–1828,
Int64 verbatim 1934–1954; **`birthOp` = `refOf(dense)` = `ephemeralBit|dense`, self-identity not an
op timestamp** 1179–1187; shadow restore "birthOp comes across untouched" 1489; snapshot
serialize/restore 2233/2302; `isEphemeral` = high bit, `manifold.hpp:154-157`),
`apps/common/xanadu/vortex/mesh_value.cpp:395-400` (`birthOp != *target` identity check on
relocation targets), `vortex/vortex_core.cpp` (`exportSymbol` `sym +d.values → entryOp` 119–126;
wing convention `+d.grab`/`−d.grab` + `+d.step` 386–414; `+d.name → nameCell` convention 485,
`vortex_stdlib.cpp:4053`), `vortex/mesh_evaluator.cpp:723-735` (per-ref marker precedent),
`vortex/mesh_actor.cpp` (`pending` = `mailbox.size()` 658–661; `pumpOne` skips empty/stopped 462,
executor seam 473–477, `ActorDown` stop 477–491; `checkpointView` NotQuiescent incl.
`outstandingMarks()==0` 668–678; **resume rehydrates via `receiveValue` into a base `Manifold`,
`WorkerContext(base)` overlaying the arena — restored state is non-ephemeral** 253–296),
`vortex/worker_context.hpp:52-63` (`ArenaManifold arena(baseManifold)` copy-on-write overlay).

## R4 verdicts (zigzag-1..9)

| r4 finding | verdict |
|---|---|
| 1 MAJOR — `+d.values → head` assumption breaks on `def` symbols | **Resolved.** :1388-1391 applies the §6.1 head/op discriminator at the hop verbatim as proposed: "the resolved cell is the entry op when it is an op (`exportSymbol` binds `sym +d.values → entryOp` directly, `vortex_core.cpp:119-127`), else follow its `+d.entry` once (a head) → `writeValue(slot, int64(entryOp))`", applied "wherever a name-record chain lands on a symbol, genesis and patch replay alike" (:1391-1392), repeated in the genesis list (:1464), the P16 row (:1954) and the appendix (:2243). The `run`-target variant (label → `code_named` → head `+d.entry`) is stated at :1392-1394. `exportSymbol` verified unchanged. |
| 2 MAJOR — `:`-label parse breaks `code_named` | **Resolved.** :1502-1508: labels are "opaque label strings, matched verbatim"; the no-`:`/`"` ban scopes to *user* `#label`s; first-`:` split applies "only to qualified-name lookups — `plan_named`, `symbol_named`, patch target names" and only "when it resolves as a module on `programRef`'s walk, otherwise the whole string is the name — never to `code_named`, where `patch:near_water` is just a label". The twin/rank-first sentence is dropped (:1514-1516 — `code_named` walks `d.code` only, twin unreachable by construction). |
| 3 MAJOR — sideness enumeration omits `+d.values` et al. | **Resolved in enumeration; the added arm breaks — new finding 1 (MAJOR).** :1269-1273 now walks "the program's **full structural dim vocabulary** — `d.stdlib`, `d.vars`, `d.clause`, `+d.values`, `+d.code`, `+d.entry`, `+d.name`, `d.name`, `+d.refs`, `+d.recipe`, `+d.grab`, `+d.spin`, `+d.step`, `d.contract`, `d.belief`, `d.source`" — every dim my fix named plus `+d.code` (which reaches memo chains). The second arm — "`birthOp` at or below the program-image boundary" (:1273-1276) — is new finding 1: the comparison domain is unstated and the literal reading inverts post-resume. Residual overstatement "every dim program structure can hang on" — new finding 4 (MINOR). |
| 4 MAJOR — P16 step 1 marks every ref cell | **Resolved.** :1413-1419 scopes to "every emitter-minted int64 ref cell **whose `+d.refs` record carries no `+d.name`** … the `+d.name` discriminator above governs both halves of the ceremony: a name-bound `Call` operand is still an emitter-minted int64 ref cell, but its record carries `+d.name`, so it gets the record and no marker". Head layout (:951-954) and P16 row (:1954) agree. |
| 5 minor — `d.validate` tag serial provenance | **Resolved.** :1577-1579: "a *fresh `Int64` scalar cell minted per visited cell per pass* holding the pass serial — the serial itself drawn from the durable per-agent `d.vars` counter, the same source `reify`'s `<serial>` uses; membership is the edge existing **and** `value_of(tag) == serial`". Both halves answered: durable serial source, value-bits (not `textOf`) read. Int64 promotes verbatim (:1934-1936), so stale promoted tags keep non-colliding serials — the tag survives its own promotion story. |
| 6 minor — rank convention mis-cites `d.clause` | **Resolved, one cosmetic nit.** :1314-1319: "`d.patch-head` is a dedicated first-edge dimension … a chain's first edge and its step edges are different dims (`+d.refs`/`+d.step`, `+d.recipe`/`+d.step`, the memo chain's `+d.code`/`+d.step`); the agent's top-level ranks themselves — `d.code`, `d.patch`, `d.synapse`, `d.clause` — are *same-dim* chains per `insertIntoRank`/`rankTail` (`arena_manifold.cpp:1423-1439`, `addClause` precedent `vortex_stdlib.cpp:4238-4241`)". Verified — `insertIntoRank` splices on one dim. Nit: `d.clause` is a per-goal/handler rank, not "the agent's top-level rank" — folded into finding 6. |
| 7 minor — memo mechanics under-specified | **Substantially resolved; occupancy residual — new finding 3 (MINOR).** :284-289 and :1301-1303 adopt the proposed shape verbatim: memo **records** `{+d.values → the wrapper head, +d.name → the clause's current attach-cell key}`, `+d.step`-linked records only ("a wrapper's own `+d.step` never enters `validate`'s op-graph walk"), "a key-mismatching memo is unlinked at re-wrap". The record's own `+d.values`/`+d.name` slots are free-by-construction, so it does **not** collide with descriptor occupancy (the descriptor is the *wrapper's* outgoing `+d.values`). What remains unstated is the key's representation and the wrapper's `-d.values` single-slot — finding 3. Separately, the v1.4 birthOp window flips these records to state-side under literal reachability, contradicting "die with the program" (:289-291) — finding 2. |
| 8 minor — §8.2 residual + eviction rationale | **Resolved.** :1661-1662 reads "a weight record naming the clause is its strength" — the `d.vars`-record rule, contradiction gone. :1642-1644 now gives the mechanical reason: "each junction owns a distinct `FloatCell` by mechanics, not merely convention: `link` gives `W` one `-d.weight` slot, so a second junction's `+d.weight → W` edge would evict the first's". |
| 9 minor — editorial leftovers | **Resolved.** `symbol_named` is invoked two-arg at the genesis chain (:1387 — "`symbol_named(owner, name)` on the rest") and the split grammar is stated once (:1517-1520). `learn_*` idempotency is scoped to the arg-form (:1288-1294 — "the argument is reused as its own registration … Only that arg-form dedupes"). The duplicated marker paragraph is merged into the head layout (:933-964). |

Disposition items in this lane verified landed: serial provenance (:1578), kind-skip uniformity
(:1438, :1448-1453), release guard + checkpoint aside naming both regimes (:1189-1191 —
`outstandingMarks()==1` for `release`, `==0` for `checkpointView`, matches
`mesh_actor.cpp:672-673`), unlearned-graft `+d.refs` boundary (:1442-1445), `closed` content arm
(:1351-1354), `run`-operand name-records (:1363-1365, :1392-1394), genesis `opcodeMap_` step
(:1459-1461), flag protocol `pending(pid)==1` + P4 wake + two-phase flag + kind-guarded restore on
all exit paths (:1427-1456 — every cite verified against `mesh_actor.cpp`).

## New findings

### 1. MAJOR — the `birthOp` mint-window's comparison domain is never stated; the literal reading misclassifies every restored cell, and the cited line contradicts the framing

"program-side" now requires "*and* minted within the program's own mint window: `birthOp` at or
below the program-image boundary the genesis `programRef` bind marks (`birthOp` is recorded per
slot, `arena_manifold.cpp:1183-1186`, and consulted by the relocation path, `mesh_value.cpp:399`)"
(:1273-1276). Three facts the text never supplies decide whether this works:

- **`birthOp` is not a per-slot birth timestamp for arena cells** — the cited lines say the
  opposite: "An arena cell has no birth operation. `birthOp` carries its own ref"
  (`arena_manifold.cpp:1183-1186`, `= ephemeralBit|dense`, `refOf` :468-469). `mesh_value.cpp:399`
  uses it as an *identity* check (`birthOp != *target` ⇒ not a genuine arena cell), which confirms
  the field names the cell, not the time. For non-ephemeral (base/store) cells `birthOp` is the
  producing-op ref — a durable, monotone store op index (`:621-630` feeds it to
  `segmentedOps().idOf`). The comparison therefore mixes two domains: ephemeral self-refs (numeric
  ≥ 2³¹) vs store op indices (small ints).

- **The boundary's representation is unstated.** "The program-image boundary the `programRef` bind
  marks" — a `CellRef`? an op index? in which domain? The only reading under which the test
  classifies correctly at all: program cells are **non-ephemeral base cells** (the artifact lives
  in the `Manifold` the `WorkerContext` arena overlays — `mesh_actor.cpp:267-269`,
  `worker_context.hpp:52-54`) and the boundary is the program-image's last op ref. Then program
  cells satisfy `birthOp ≤ boundary`, ephemeral runtime cells auto-exceed it, and restored state —
  non-ephemeral, birthOps = promote-op indices minted *after* the image — exceeds it too. That
  reading works end-to-end; nothing in the text selects it.

- **The wrong reading inverts the durable set.** If the boundary is recorded as an *arena* position
  (the natural reading of "the program's own mint window" over cells the doc calls "arena cells",
  :276): post-resume every restored cell is non-ephemeral (`receiveValue` → base `Manifold`,
  `mesh_actor.cpp:253-269`; resume bypasses `importValue`, P16 row :1954), so every restored
  `birthOp` is numerically below the ephemeral boundary → **the entire restored state classifies
  program-side** — `closed` re-verification refuses every capture of checkpointed cells, and the
  durable set collapses to cells minted this incarnation. If instead the program image were ever
  arena-minted, the symmetric inversion puts the skeleton state-side — the r3 promote-the-program
  failure rebuilt. Genesis ordering doesn't save it either: (a) overlay → (d) `programRef` bind
  (:1458-1463) never states when the image's cells are (re)minted relative to the state overlay.

**Fix.** One sentence naming the domain: "the boundary is the program image's last op ref — a
non-ephemeral ref — recorded at genesis; `birthOp`'s ephemeral bit alone settles the comparison
(every arena-minted cell postdates the window); program cells are non-ephemeral artifact cells with
`birthOp` ≤ the boundary, and a promoted cell's store birthOp necessarily postdates it." Plus a
half-sentence on when the image is minted relative to the overlay. Without it, an implementer
chooses between a correct-by-accident build and a total-misclassification build on a coin flip.

### 2. MAJOR — "link-reachable *from the agent cell*" has no transit restriction: every program-adjacent runtime cell classifies state-side and promotes — contradicting the doc's own lifecycle claims and duplicating replay-derived installs

"'State-side' is link-reachable *from the agent cell* and *not* program-side" (:1278-1279),
repeated for `closed` at :1342-1343. Read literally — plain graph reachability over all dims, the
same walk `promote` runs — any runtime cell hanging off a program cell is agent-reachable (agent →
program dims → clause → `+d.code` → memo → `+d.values` → wrapper) and, being a post-window mint
under the new second arm, *not* program-side → **state-side → `promote` carries it**. Direct
contradictions in the same revision:

- :283/:289-291 claim reflect wrappers and memo records are "session-side", "unpromotable by
  construction", and "die with the program at `resume`" — the stated reason ("clause cells are
  program-side") was true under v1.3's reachability-only test and is false under v1.4's window: the
  window is exactly what reclassifies program-adjacent runtime cells as non-program-side.
- An *installed* `learn` copy sits on a program `d.clause` rank reachable only through program
  cells; under literal reachability it promotes as an orphan (its rank anchor dies) while patch
  replay re-installs a fresh copy — duplicated graphs and zombie cells where the doc's EDL story
  intends re-derivation.
- `d.validate` tag cells and `+d.refs` markers on program-side cells promote as orphan garbage.

The alternative reading — the walk prunes at program-side cells, so durability flows only through
state-side transit — makes every claim consistent at once (wrappers/memos die, installed copies are
replay-derived, learned heads promote via `d.code` directly, the `d.vars` serial counter promotes
via the agent). But it is not stated, and the doc's own formula — "link-reachable **and** not
program-side" — is set-difference phrasing that picks the broken reading.

**Fix.** "State-side is link-reachable from the agent cell **without transiting a program-side
cell** — the walk prunes at the program-side boundary — and is not itself program-side." One
clause; the doc's claims then hold verbatim.

### 3. MINOR — memo `+d.name → key` representation unstated; a link-to-cell key makes the two memos evict each other

"each `{+d.values → the wrapper head, +d.name → the clause's current attach-cell key}`"
(:284-286, :1302) never says what the key *is*. The codebase's `d.name` convention is
`cell +d.name → nameCell` holding text (`vortex_core.cpp:485`, `vortex_stdlib.cpp:4053`). If the
key is read as a link **to the attach cell itself**, `link`'s reciprocal eviction gives the attach
cell a single `-d.name` slot: the `test_of` and `body_of` memos key the same attach cell and the
second `+d.name → attach` edge evicts the first's — the evicted memo then reads keyless/mismatched
and is unlinked at the next re-wrap, contradicting "`test_of`/`body_of` memos coexist" (:288-289).
The working representation — a per-memo scalar/text cell holding the attach cell's label or id —
needs one clause, plus one line noting the memo's `+d.values → wrapper` occupies the wrapper's
single `-d.values` slot (safe only because a builtin-minted wrapper has no emission-record anchor —
worth stating since `+d.values` is elsewhere the head-anchor dim, :1263-1264).

### 4. MINOR — "full structural dim vocabulary … every dim program structure can hang on" omits the runtime/system dims; `d.dims` cells then classify state-side

The enumeration (:1270-1272) drops `d.dims`, `d.stack`, `d.cursors`, `d.pinning-cursors`,
`d.cache`, `d.clone`, `d.scan`, `d.stores`, `d.role`. Most omissions are defensible (cursors,
stacks and scan cells are runtime, not skeleton), but `d.dims` is a real hole: `promote` skips only
`d.dims` *edges* (:1766), not dim cells reached by another edge — a `cells"##/+d.dims"`-resolved
dim cell captured into a fragment classifies state-side under the enumeration, promotes, and is
re-minted by genesis's `ensureDimension` — two cells answering the same dim name. Either complete
the list or scope the claim: "every dim program *skeleton* hangs on" with `d.dims` named as the
explicit exclusion.

### 5. MINOR — `reify:` label spelling now inconsistent

:1503 mints "`reify:<serial>`" (sufficient — the serial is durable-unique) but :1035 and :2229
still say "`reify:<src>:<serial>`". Pick one spelling; the serial-source sentence at :1509-1511
supports the short form.

### 6. NIT — `d.clause` is not "the agent's top-level rank"

:1317-1318 groups `d.clause` with `d.code`/`d.patch`/`d.synapse` under "the agent's top-level
ranks" — clause ranks hang off goal/handler cells (`addClause` chains per-goal), not the agent
cell. The same-dim claim itself is correct; the grouping is not.

## Verified accurate (v1.4)

`link`'s two-sided reciprocal eviction (:1347-1377 — both `displacedUs` and `displacedIt` cleared)
underlies the per-marker, per-junction-scalar and `+d.patch-head`-vs-`+d.values` reasoning, all of
which is stated correctly. `insertIntoRank`/`rankContains` same-dim semantics (:1381-1440) match
the rank-convention text. `promote`'s bidirectional `edge.pos`/`edge.neg` walk (:1781-1783),
relocation pull-in (:1759-1764), `OpHandle` deferred/base arms (:1797-1828), Int64 verbatim
(:1934-1936) and `d.dims` skip (:1766) behave as v1.4 assumes. `isCellRelocationHandle` requires
kind *and* marker (`foreign_op_handle.cpp:98-117`); the precedent mints one marker per reference
(`mesh_evaluator.cpp:735`). The flag protocol's cites are all real: `pending` =
`mailbox.size()` (:658-661), `pumpOne` skips stopped/empty actors (:462), the `executor_(…,
*actor->context)` seam (:473-477) hands the mutable arena (`worker_context.hpp:63`), a stopped
actor never re-enters `pumpOne` (:462, :491), `checkpointView` refuses non-empty mailbox and
non-zero marks (:668-678). `exportSymbol` still binds `sym +d.values → entryOp` directly
(`vortex_core.cpp:119-126`), making the head/op discriminator load-bearing and correctly applied.
The P16 two-phase flag, kind-guarded restore, and `pending==1`/`P4 wake` window are mechanically
coherent against the actor loop as written.

VERDICT: NOT CONVERGED — 0 blocking, 2 majors (birthOp domain, state-side transit), 4 minors.

# R5 — System Architect critique (v1.4)

Scope: layering, ownership, adapter-vs-core boundaries, Makefile/dependency hygiene; convergence
check against the r4 disposition plus new defects in the v1.4 deltas only. All claims re-verified
against the tree (`apps/common/xanadu/{vortex,zigzag}/`, `Makefile`, `design/vagent-agent-language.md`).
v1.4 lands every r4 mechanism ruling faithfully: the flag protocol now arms on `pending(pid) == 1`
with a P4 wake for the empty case, the restore arm is kind-guarded, the sideness test gained the
`birthOp` mint window, the `+d.values` hop applies §6.1's discriminator, and the Makefile inventory
is complete. Residuals are all clause-level: the wake arm quietly re-opens the "no new core
mechanism" claim and leaves the between-turns invariant stated in pre-wake terms, the genesis
paragraph gains a factually wrong "pre-promotion" modifier, and the mint window's boundary
provenance is unstated with a self-contradictory wrong reading available.

## R4 finding verdicts

### 1. MAJOR (P16 flag: no executor window on empty mailbox; phase/kind-guard unstated) — RESOLVED

§6.6 `:1427-1435` adopts all three fix arms verbatim: the flag arms "when the next turn is the one
whose end calls `publishValue` — and only when `pending(pid) == 1`" (`pending` exposes
`mailbox.size()`, `mesh_actor.cpp:659-662` — verified: `:658-662` returns `mailbox.size()`;
`pumpOne`'s empty-mailbox skip at `:462` verified: `if (actor->stopped || actor->mailbox.empty())
continue;`), and "a checkpoint decision that lands on an empty mailbox instead mints a P4 `wake`
turn so the flag has a window". The two-phase flag — re-kind pending → restore pending → drained —
is stated with callback ownership, cleared on `publishValue` failure/refusal and on `ActorDown`
(`:1432-1435`). The restore arm mirrors the re-kind's guard exactly as prescribed: "restores
`setValueBits(Int64, *handleTarget)` on every marked cell **still in `OpHandle` kind** — the
kind-guard makes an unmatched or repeated restore a no-op — on every exit path including publish
refusal/failure and a `consumeWork` throw" (`:1448-1451`), with the kind-skip uniformity clause for
all three passes at `:1452-1453`. The empty-mailbox starvation/verbatim-promotion hole is closed.
Residuals → new findings 1–2 (wake-era scheduling precision; the P4 dependency's status in the
"no new core mechanism" claim).

### 2. MINOR (`frontend.cpp` absent from `VAGENT_FRONTEND_SRCS`; `ast.hpp` unlisted) — RESOLVED

§10.1 now lists `frontend.{hpp,cpp}` joining `VAGENT_FRONTEND_SRCS` with the correct reasoning —
"source→checked-Program driver, no arena dependency — it belongs with its siblings, not the kernel
glob" (`:1896-1899`) — and `ast.hpp` is inventoried (`ExprKind::{Code,Splice,Static}` + the `Expr`
scope-stamp field, `:1900-1901`). The Makefile edit names all five sources (`:1933-1935`) with
accurate cites: `Makefile:643` is the `COMMON_XANADU_SRCS` glob, `:688-690` is
`VAGENT_FRONTEND_SRCS`, `:695` is `VAGENT_SYNTAX_OBJS`. Placement verified against dependencies:
`frontend` drives parse→resolve→expand→check, and `token/lexer/parser/checker` are already inside
the same list, so the dependency is intra-list; both consumers link `$(VAGENT_FRONTEND_OBJS)`
(`build/vagent` `Makefile:1147`, `vagent_frontend_test` `:1151`). The `filter-out` on
`vagent/*.cpp` (`:698`) keeps the explicit list the only route into the vagent test binaries —
the edit puts `frontend.cpp` on exactly that route.

### 3. MINOR (genesis kind-labeling: "Int64-stale" marked cells) — RESOLVED, with one new misstatement

§6.6 `:1466-1470` now distinguishes the paths correctly in kind: `importValue` converts the
`OpHandle`-kind cell itself (`CellImport{OpHandle}` → `Cell` with the promoted target), while
`resume`/`genesis` "receives the `OpHandle`-kind cells verbatim" and rehydration writes the
promoted target back. The unmarked-int64/name-record contrast is carried by the name-record chain
(`:1463-1464`). But the new clause says the received handles carry "**pre-promotion** targets"
(`:1469`) — backwards: `promote` writes `makeOpHandle(out.version, real.at(target))`
(`arena_manifold.cpp:1947-1952`), so handles arrive carrying the *promoted* target (that is the
point of the ceremony), and the very next clause ("writeValue … back with the promoted target")
contradicts the modifier. → new finding 3.

### 4. MINOR (`d.validate` serial provenance) — RESOLVED

§7.2 `:1578-1579` now states "the serial itself drawn from the durable per-agent `d.vars` counter,
the same source `reify`'s `<serial>` uses (§6.7)" and the read names `value_of` (`:1579`). §6.7
`:1509-1511` confirms the counter is "a *durable per-agent counter* on `d.vars`, not a
per-incarnation one" — a state-side scalar, so it promotes and cannot re-issue a serial a promoted
tag carries. Exactly the prescribed fix.

### 5. MINOR (genesis name-record chain assumes `+d.values` lands on a head) — RESOLVED

`:1388-1391` applies §6.1's head/op discriminator at the hop: "the resolved cell is the entry op
when it is an op (`exportSymbol` binds `sym +d.values → entryOp` directly,
`vortex_core.cpp:119-127`), else follow its `+d.entry` once (a head)". Verified:
`exportSymbol` links `symCell +d.values → entryOp` with no head (`vortex_core.cpp:122-124`). The
rule is stated to apply "wherever a name-record chain lands on a symbol, genesis and patch replay
alike" (`:1391-1392`), and the `run`-target variant rebinds through `code_named` (`:1393-1394`).

### 6. MINOR (`program()` walk blind to int64-referenced cells) — RESOLVED

`:1279-1282` adds the prescribed door clause: "a program cell reachable only through int64 content
— `Jump`/`Call`/`Branch`/`Choice` control-flow targets — is invisible to it, and an outside edge
to one is a §11-door limitation, not claimed soundness", and the `closed` frontier repeats it at
`:1357-1359`. The dim-vocabulary extension is the full R4-A list (`:1270-1272`).

### 7. MINOR (`outstandingMarks() == 1` checkpoint aside contradicts actor API) — RESOLVED

`:1189-1194` now separates the regimes correctly: `release(m0)` only when `m0` is the only
outstanding mark (`choiceDepth() == 0` *and* `outstandingMarks() == 1` — m0 itself), while
"`checkpointView` requires `outstandingMarks() == 0` outright (`mesh_actor.cpp:672-675`) — the
`== 1` regime is `hydratePrefix`'s floor (`arena_manifold.cpp:2123`), a parallel-worker import
artifact (`or_parallel.cpp:349`), not the actor path". Verified `checkpointView`'s `!= 0` refusal
at `mesh_actor.cpp:673-674`.

### 8. MINOR (marker-cardinality paragraph duplicated) — RESOLVED

§6.1 `:957-964` carries the single consolidated statement — per-ref-cell marker, the
single-`-d`-neighbour eviction reason, the `+d.ref` inventory role, and the
`isCellRelocationHandle` cite folded in. The second copy is gone.

### 9. MINOR (`reify` `+d.refs` regeneration didn't name marker re-mint) — RESOLVED

The table row at `:996` now reads "regenerated `+d.refs` records **and** fresh per-ref-cell
`d.mesh.relocation` markers", and the mechanism paragraph `:1033-1034` spells out records
remapped to new ref cells with name-records re-linked.

### 10. MINOR (P16 row's "Where" mislabeled `fragment.{hpp,cpp}` as adapter-side) — RESOLVED

The P16 row (`:1954`) now reads "emission-side marking: `vagent/fragment.{hpp,cpp}` (kernel-glob
TU, §10.1); the flag/checkpoint/genesis ceremony: the adapter's own executor TU — all new
adapter/vagent code reusing `foreign_op_handle`/`publishValue`/`checkpointView` as-is", matching
the inventory comment (`:1906-1908`). Kernel-vs-adapter ownership is now stated correctly —
residual scope caveat → new finding 2.

## New findings

### 1. MINOR — the between-turns invariant is stated in pre-wake terms; a queued P4 wake can fire the restore before `publishValue` (§6.6 `:1436-1453`)

**Problem.** The ceremony's ordering is: flagged turn's end → re-kind → "`publishValue` executes
between turns" → "head of the *next* turn" restore. That was unambiguous when only mailbox
messages made turns — but this same delta introduces P4 wake turns, which the adapter schedules
*between* message turns under the `vagent.ready_per_message` policy
(`vagent-agent-language.md:1575`, `:1638-1653`). If the pump runs a queued wake between the
flagged turn's end and the publish, the wake's head-of-turn sees the flag in restore-pending and
reverts the re-kind; the subsequent `publishValue` then promotes the Int64 slots verbatim — the
silent stale-aliasing bug P16 exists to prevent, re-entered through the mechanism added to fix it.
The exposure is real precisely in the new arm's scenario: an empty mailbox does not mean no queued
work — due timers and ready intentions queue wakes with `pending() == 0`. The adapter owns both
the pump and the flag so the correct order is enforceable, but the text never says the publish is
the *immediate* next step after the flagged turn — before any subsequent `pumpOne` of either turn
kind — or that "the next turn" means the first turn *after* publish returns. Companion ambiguity:
"the flag is … cleared on `publishValue` failure or refusal" (`:1433-1434`) sits next to "restores
… on every exit path including publish refusal/failure" (`:1449-1451`) — if "cleared" is read as
flag→drained, the head-of-turn restore has no trigger and marked cells sit `OpHandle`-kind into
the next turn's execution (an `OpHandle`-kind operand fails `asInt64`/decodes `Nop` — silent wrong
execution). The consistent reading is that the restore is driven by the marked-`OpHandle` scan and
the flag's clear only ends checkpoint arming, but the text should not require charity.
(Also a nit: "two-phase" names three states — re-kind pending → restore pending → drained.)

**Fix.** One clause in step 2 or 4: "no turn — message or P4 wake — runs between the flagged
turn's end and `publishValue`; the restore's 'next turn' is the first turn after `publishValue`
returns. Clearing the flag ends checkpoint arming only; the head-of-turn restore is unconditional
on marked cells still in `OpHandle` kind."

### 2. MINOR — the empty-mailbox arm takes a P4 dependency that the "no new core mechanism" claim doesn't name (§6.6 `:1431-1432`; P16 row `:1954`; M11 `:352-355`)

**Problem.** "Mints a P4 `wake` turn" is honest about *which* mechanism, but P4 is not existing
machinery: it is the adapter register's approved-but-unimplemented local-wake proposal that
modifies `mesh_actor.{hpp,cpp}` — `ActorExecution::message` becomes `std::optional`, "touching the
existing mesh executor and its tests" (`vagent-agent-language.md:1670`, approved at `:81`). So the
empty-mailbox arm is *not* implementable against the current tree, and the P16 row's "adapter-side
ceremony needing **no new core mechanism**" / "reusing `foreign_op_handle`/`publishValue`/
`checkpointView` as-is" is accurate only for the `pending() == 1` arm — the wake arm needs a
mesh-layer change (granted, one already registered and approved elsewhere). An implementer
triaging the row's "no VM change" claim could miss that the ceremony's second arm is gated on P4
landing first. This is a dependency-declaration defect, not a coupling defect — the P4 call-out
belongs in the row.

**Fix.** Amend the P16 row's Where or Why: "the `pending() == 1` arm needs no new mechanism; the
empty-mailbox arm takes the adapter register's P4 (local wake — an approved `mesh_actor` change),
the only part of the ceremony outside current code."

### 3. MINOR — genesis receives handles carrying *promoted* targets, not "pre-promotion" ones (§6.6 `:1468-1469`)

**Problem.** "`resume`/`genesis` receives the `OpHandle`-kind cells verbatim — handles carrying
pre-promotion targets". `promote` mints `makeOpHandle(out.version, real.at(target))`
(`arena_manifold.cpp:1947-1952`): the handle's target is the relocated position — that is the
whole reason the re-kind is worth doing. "Pre-promotion" describes the verbatim-Int64 failure P16
prevents, and it contradicts the sentence's own next clause ("`writeValue(slot, Int64)`s each
marked cell back with the promoted target"). Read literally it tells the implementer rehydration
receives stale handles and must fix them — which would double-apply a remap.

**Fix.** Drop the prefix: "handles carrying the *promoted* targets" (or "post-promotion targets").

### 4. MINOR — the `birthOp` mint window's boundary provenance is unstated; re-marking per genesis contradicts the test's own rationale (§6.6 `:1273-1278`; genesis ordering `:1458-1464`)

**Problem.** "Program-side" now requires "`birthOp` at or below the program-image boundary the
genesis `programRef` bind marks". But `programRef` binds at *every* genesis — step (d) of the
fixed ordering (`:1461`) runs on each resume, after the overlay. If the mark is re-derived at each
bind as the then-current arena extent, every overlaid cell — all prior-incarnation state, learned
grafts, registered heads — falls at-or-below it, and *everything* classifies program-side: a
post-resume `learn` of a fragment capturing any legacy state cell then fails `closed`'s
program-side arm (no `+d.refs` name-record for a state cell), and the text's own next sentence —
"a `learn`ed clause sitting on a program `d.clause` rank does not become 'program-side' and
forfeit the durable path" — is contradicted. The only consistent reading is a boundary recorded
once at the initial program load and carried durably thereafter (the ordering dependency this
creates is real: sideness can only be evaluated after program load marks the extent, and the mark
must outlive the incarnation that made it). Second precision edge: `birthOp` is a *naming* field,
not a serial — plain arena cells get `refOf(own dense)` (`arena_manifold.cpp:1183-1186`), but a
shadow/overlaid cell's `birthOp` is the *base cell's* ref and "comes across untouched"
(`:1489-1490`, indexed under `!isEphemeral` at `:2111-2112`). The `≤ boundary` comparison is a
dense-position test for ordinary cells; for a state-side shadow whose `birthOp` names a
non-ephemeral base cell the ordering is undefined — a stated arm is needed ("a non-ephemeral
`birthOp` is outside the window").

**Fix.** Two clauses: (a) "the boundary is the program-image extent recorded at the initial
`programRef` bind and carried as durable state; genesis's (d) re-asserts it and never re-bases it
to the post-overlay extent"; (b) "the window test compares dense position; a cell whose `birthOp`
is non-ephemeral (a shadow naming its base) is outside the window by construction."

## Structural notes (no finding)

- **Wake arming vs. `pending` under P4 is otherwise consistent.** A queued wake is invisible to
  `pending()` (wakes are not mailbox entries), so `[msg][queued-wake]` with `pending() == 1` still
  arms correctly — `checkpointView`'s `mailbox.empty()` gate also ignores wakes, and the wake then
  serves as the restore turn. The `pending() > 1` case is implicitly a defer-until-drain policy;
  starvation under sustained arrival is a scheduler-policy question, not this document's.
- **`checkpointView`'s uncompleted-deliveries gate** (`mesh_actor.cpp:677-681`) is a third refusal
  path the flag protocol doesn't enumerate; it lands inside "publish refusal" already, so the
  restore covers it. No text needed.
- **`fragment.cpp` glob placement remains correct.** `apps/common/xanadu/vagent/*.cpp` all ride
  `COMMON_XANADU_SRCS` (`Makefile:643`) into the xuzz-family link and are filtered out of
  `VAGENT_RUNTIME_CORE_OBJS` (`:698`); `frontend.cpp` joining the explicit list mirrors its
  siblings (`checker.cpp`, `cell_path.cpp`) exactly — "not the kernel glob" is accurate in the
  sense that matters (list membership, not glob exclusion).
- **The re-`learn` carve-out and name-recorded heads** (`:1287-1294`, `:1324-1331`) keep the
  registered-vs-installed ownership split intact; `insertIntoRank`'s `rankContains` no-op cite
  (`arena_manifold.cpp:1423-1439`) is accurate.

## Verdict

All ten r4 findings are resolved in substance; architect-3 carries a one-word residual rolled into
new finding 3. The remaining work is four clause-level minors in the new deltas — the wake-era
scheduling clause (finding 1) and the P4 dependency call-out (finding 2) are the two worth doing
first, since both live at the seam the round-4 major occupied. No blocking or major defects.

VERDICT: CONVERGED — 0 blocking, 0 major, 4 minors + 1 nit

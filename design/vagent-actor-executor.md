# vagent Actor Executor: Turn-Level Runtime Design

**Document Version:** 0.1 (dialectic converged) **Status:** converged — ready to implement by
milestones; three spec-level rulings taken by the owner (§9)

> **Dialectic outcome (two rounds, three lenses — CONVERGED):** the turn-level executor is
> implementable on today's substrate with two mesh changes (deficit-counter arbitration, P8 scan
> rotation), one confirmed VM defect (§5.1), and no new primitives. Round 1 produced a Nelsonian
> thesis, a systems critique that exposed two structural impossibilities (mailbox-beats-wakes makes
> §11.1's starvation bound inexpressible adapter-side; nothing bounds committed garbage for a busy
> agent), and a codebase map that settled clause registration (`d.clause`), the M0-lowering gap, and
> the meta-verb invocation seam. Round 2 adjudicated: eager `d.worder` died on write-dominance
> arithmetic and was replaced by **virtual weight-order** (owner-ratified); the Purist's step-path
> staleness worry was **verified real** by the codebase expert and ruled `body_replaced` (owner);
> `d.agents` + `d.seen`/`d.evidence` adopted (owner); eleven spec-level questions collapsed to the
> three the owner answered plus forced rulings (§9).\
> **Related Documents:**

- [`vagent-agent-language.md`](vagent-agent-language.md) — §11.1 executor contract, §11.2 P-table,
  §11.3 deterministic harness, V2/V4/V5/V13, §4.2 turn steps, §6.3 selection, §6.5–6.6 intentions
- [`vagent-metaprogramming.md`](vagent-metaprogramming.md) — §6.1–6.6 the `std:meta` verbs,
  graft/learn durability, §8–§9 plasticity exercises this design gates
- [`vagent-metaprogramming-implementation.md`](vagent-metaprogramming-implementation.md) — phases
  0–7 delivered; "everything the spec calls 'the adapter' is greenfield" (:36-38)
- [`vagent-runtime-grounding.md`](vagent-runtime-grounding.md) — the adapter/VM boundary rules (one
  cursor, adapter drives, no transport/native calls inside a graph)
- [`arena-promotion-delta-completeness.md`](arena-promotion-delta-completeness.md) —
  `commitStateSide`/tombstone conventions the state-side ranks ride

______________________________________________________________________

## 1. Why this document exists

vagent compiles agent programs into a ZigZag `ArenaManifold` image; `MeshActorRuntime` already owns
the actor lifecycle (spawn/resume/receive/pumpOne/wake/checkpoint/P16). What is **owed** is the
executor — the `MeshActorRuntime::Executor` callback plus the agent-structure lowering that feeds
it:

1. **Delivery → dispatch.** A mailbox `ActorSend` or a queued wake becomes a turn: the event is
   decoded into an immutable event cell and `on Event as x` handler bodies run.
1. **Beliefs.** `believes pred(args)` reads and `believe`/`revise`/`forget` write the agent's
   `d.belief` predicate store, with provenance.
1. **Plan selection.** `plan name for Goal(args) where test { body }` clauses live on the goal
   symbol's `d.clause` rank; selection enumerates the rank fresh, applies the `where` test per
   candidate, respects `tried`, and — per the owner ruling — orders candidates by **virtual
   weight-order** (§3.4).
1. **Intentions.** Effect calls suspend a plan mid-body; host deliveries resume it. Intentions are
   cell records, never suspended cursors (V4).
1. **Commit discipline.** The turn is one mark (`m0`); staged outbox entries and arena writes commit
   or discard together (V2/V5); the adapter drains the outbox after commit.
1. **Boundedness.** Turn fuel covers *all* constituents; arbitration between mailbox, wakes,
   deliveries and deadlines is runtime-owned and finite-bounded (§4.1).

The concrete exit criteria are spec §9.2/§9.3 (`vagent-metaprogramming.md:2808-2880`): a plan that
tightens its own `where` on `Attack`, and Hebbian clause growth on `Sighting`/`Meal` — today's
`VagentPlasticityTest.ActorExecutorBoundaryRemainsGated` skip.

## 2. Established mechanics (verified against the code)

### 2.1 What already exists

- **The mesh seams.** `pumpOne` runs one turn — mailbox message or queued wake
  (`mesh_actor.cpp:510-621`), invokes
  `executor_(ActorExecution{pid,grant,message,cursor,stateRoot}, *context)` inside try/catch →
  `ExecutionFailed`; `ActorExecution.message` is `std::optional`, absent on wakes
  (`mesh_actor.hpp:89-97`). `wake(pid)`/`pendingWakes` exist (`:781-793`). `GenesisHook` runs at
  spawn/resume (`:200-210,:333-347`). `checkpointView` refuses non-quiescent actors — residual
  mailbox, wakes, marks, choice depth, incomplete deliveries (`:829-855`) — which is V2 enforced for
  free.
- **The clause bank.** `d.clause` rank on the goal/event **symbol** cell (a `d.vars` member of the
  module), clause cells shaped `+d.name→label`, `+d.grab→test head`, `+d.spin→body head`;
  `clausesOf` walks it tombstone-filtered (`meta.cpp:210-220`), `clauseHost` resolves names
  (`:1167-1192`), and `graftPlan`/`learnPlan` mint exactly this shape (`:1396-1407,:1732-1741`) —
  **learned clauses join selection automatically** on the next enumeration.
- **The meta surface.** `MetaRuntime` (`meta.hpp:186-450`) owns the structural verbs: `runFragment`
  (binds args on the cursor's `+d.vars`, pushes the `#Call` frame, `vm_.run`, restores scope,
  `meta.cpp:664-800`), `testOf`/`bodyOf`/`planNamed`/`clausesOf`/`weightOf`/ `setWeight`,
  `graft_*`/`learn_*`/`prune`/`unlearn`/`verifyClosed`/`nextRand`/`serial`.
- **Weight storage.** A clause cell *is* a junction: `setWeight`/`weightOf` hit the scalar
  `+d.weight` edge arm — O(1) trailed edge read/write (`meta.cpp:2038-2064`).
- **Agent dims.** `d.belief`/`d.intention`/`d.subgoal`/`d.outbox`/`d.pending`/`d.source`/
  `d.extends` are name-filed already (`kAgentDimensionNames`, `lineage.hpp`); coverage asserted at
  `vagent_lineage_test.cpp:107-127`.
- **Emission machinery.** `compileFragment` (fragment.hpp:166-168) builds `+d.entry`/`+d.name`/
  `+d.values`/`+d.refs`/`+d.recipe`; `finalizeProgramImage` + `seedProgramImage` + genesis (a)–(e)
  replay the whole thing (`genesis.cpp`, `lineage.hpp:123-124`). `Compiler` lowers **scalar
  statements only** — `imageFloor_` is once-per-compiler (`compiler.hpp:200-208`).

### 2.2 What does not exist

- **Any declaration-level lowering.** `agent`/`event`/`goal`/`predicate`/`plan`/`on`/`def` `Decl`
  nodes are parsed and checked (`ast.hpp:185-230`) but nothing consumes them below the checker —
  preflight refusal at `compiler.cpp:716`. **M0 owns this.**
- **Intentions.** No suspended-continuation machinery; `checkpointView` requires
  `choiceDepth==0 && outstandingMarks==0`. The `d.intention`/`d.subgoal` record shape
  (`vagent-agent-language.md:596-615`) is owed, including the `step` path — which implies plan
  bodies must lower as **statement-segmented resumable graphs** (§3.5).
- **Belief verbs.** `believes`/`believe`/`forget`/`revise` are typed by the checker, refused by the
  scalar lowering, and absent from `MetaRuntime`.
- **Turn scaffolding.** No `d.outbox` entry schema, no `$turn` binding, no settle loop, no drain.

## 3. The converged model

### 3.1 Agent topology — roles as dims, minted as data

Every semantic role is a named dimension filed at genesis (R12 discipline — no dim mints under a
live mark; `mintMetaBindSet`/`ensureDimension` both refuse then):

```text
home ─(+d.agents)─► [agent cell]                    ← NEW
      ├─(+d.vars)───► fields, meta.serial, weight/… records (closed schemas only)
      ├─(+d.belief)─► [predicate cell]─(+d.clause)─► [fact]─(+d.grab)─► head term
      │                                   └─(+d.source)─► [peer/time record]
      │                                   └─(+d.evidence)─► journaled percept   ← NEW edge role
      ├─(+d.intention)─► [intention]─(+d.subgoal)─► [subgoal]…
      ├─(+d.seen)────► bounded ringrank of last K event cells ↻   ← NEW
      ├─(+d.pending)─► internal events (raised by bodies, drained in settle)
      ├─(+d.outbox)──► staged send/spawn/effect entries
      ├─(+d.code)────► registered fragment heads   ├─(+d.patch)─► the learn EDL
      └─(+d.synapse)─► [junction]─(+d.weight)─► scalar
program side (the folded image):
 module ─(+d.vars)─► [goal/event symbol]─(+d.clause)─► [clause]─(+d.grab)─►test (+d.spin)─►body
```

Two adopted additions:

- **`d.agents`** (owner-ratified): `home +d.agents → agent` — a post-`E_ops` link op minted at
  genesis. Verified safe: promote's frontier follows both directions; `home` shadows `holdsOwn`
  under **Additive** policy so the add commits, but `commitStateSide` never returns `Clears` for
  home — **membership is durable-append-only**. Delisting is therefore a tombstone `d.vars` record
  *on the agent cell* (state-side `Clears` territory), never a rank unlink — the same convention
  `unlearn:` already uses. Payoff: `cells"##/+d.agents"` reaches the whole mind from the anchor.
- **`d.seen`** (owner-ratified): bounded ringrank — append posward, evict the head at capacity K
  (`vagent.journal_size` tunable; K is a rolling window, not per-turn). Written in-graph —
  `Link`/`Break`/`Linked` opcodes suffice (`vortex_vm.cpp:745-829`), packaged as a std:agent
  fragment so the writer is emitted and testable. Journal cells hold **identity + `d.evidence`
  edges, never payload copies**. Consumers named (the adoption condition): §9.3 provenance and
  `await`'s evidence link (§3.6).

Everything else follows the spec's existing mapping: `d.vars` only for closed fixed schemas
(intention system attributes); `d.synapse` junctions for weighted edges; `d.code`/`d.patch` for the
durable program.

### 3.2 The turn — one mark, one decision

```
1. m0 = mark(); setExecutionLimits(remaining grant, m0-relative)
2. decode: message→bencode→event cell (immutable; +d.name type,
           d.vars fields, implicit source/space/time/ordinal; sequence==0 for ActorSend)
   or wake→adapter delivery queue drain (coalesced, §4.1)
3. bind $turn / journal: append event cell to d.seen ring (bounded K)
4. dispatch: event type symbol → d.clause handlers (priority→mount→position merge);
             each body = runFragment on the actor cursor under per-statement choice points
5. settle: drain d.pending internal events; belief-event rounds ≤ belief_event_depth,
           all drawing one aggregate condition-fuel account (§4.3)
6. ready: service ready intentions/timers (bounded by arbitration counters)
7. commit: discard(m0) → writes keep, marks release; assert choiceDepth==0 && marks==0
8. drain: C++-only rankAfter walk of +d.outbox — host bindings dispatch
          (send/spawn/effect), consumeWork-free by construction (§4.4)
9. post: update ready/timer derivations; expose staged outbox + stats for §11.3 harness
```

- **Failure inside 2–6** → `release(m0)`; outbox cells die with the mark; the intention records the
  named fault; `fail_step` runs as a short turn with **reserved fuel** (§4.3). Nothing propagates:
  `consumeWork` throws `std::runtime_error` — the callback must translate it to a turn fault or
  `pumpOne` maps it to `ExecutionFailed` and the actor stops forever (`mesh_actor.cpp:549-551`).
- **Auto-reply**: `pumpOne` replies to every `ActorSend` with the executor's returned string — the
  adapter encodes the language-level reply there (≤ `grant.maxOutputBytes`).
- **One message per turn is ratified**: batching merges failure domains; the settle loop inside `m0`
  is the finest granularity that is still one decision.

### 3.3 Event delivery — copy of decision, transclusion of content

A delivered event is a **new fact in the receiver's space** carrying its provenance triple
(source/ordinal/requestId) — not a transclusion of the sender's cell (cross-arena links are V1-
refused and stay refused). Decode is `xanadu::bencode::decode` of `ActorMessage.value` into an
immutable event cell. **Encoding rule (non-foreclosure):** field encoding must admit a future
span-typed payload — a `{scroll,span}` handle decodes to a span value, never inline bytes (U-A3's
rule: equal text is not shared identity).

**Wake semantics (ratified):** `wake(pid)` means *"service pending adapter work,"* not
one-turn-per-item — the adapter calls it only on its delivery queue's 0→1 transition and drains the
whole queue in the wake turn. 1000 host deliveries ≈ 1 wake turn, not 1000.

### 3.4 Beliefs & plan selection

**Beliefs** are clause-cells on `d.belief` predicate ranks — the same shape `d.clause` uses: head
term on `+d.grab`, provenance on `+d.source` (peer + assertion turn), optional `+d.evidence` edge to
the journaled percept. `believes`/`believe`/`forget`/`revise` are host-routine verbs (§3.8), never
opcodes. Unification reuses `VortexStdLib::unify`/`deref` **adapter-side** — the Vlog pure-job
solvers are off-limits (a `vprolog::Compiler` owns a second VM, V3 violation).

**Fuel-cliff handling** (realist finding): a predicate with B clauses can't finish a copy-list+unify
inside `condition_fuel` — and silent permanent staleness is worse than a fault. Required:
`maxClausesPerPredicate` population cap with a **visible** `condition` fault at the cap (§4.3); the
real fix (per-key clause sub-ranks / U1 indexing) is noted, not designed here.

**Plan selection (owner-ratified virtual weight-order):** each selection walks `d.clause` fresh —
never a cached snapshot, so `learn_plan`/`unlearn`/`learn_where` take effect at the next selection
for free. Per candidate: collect `(weight, clause)` in one walk (O(1) `+d.weight` edge reads), sort
a small buffer descending, evaluate `where` per candidate in that order under per-candidate
condition fuel drawn from the turn's aggregate account, first applicable untried wins; **rank
position breaks weight ties** (the deterministic fallback — no unordered-container order ever
reaches a decision). Spec §6.3's "first applicable in rank order" is amended to this; §9.3's
`set_weight`/`weight_of` now actually drive selection.

### 3.5 Intentions — data, not cursors

Intention cells on `d.intention` (+`d.subgoal`) carry the spec'd record shape
(`id,kind,goal,plan,tried,step,waiting,ready,blocked_until,faulted,frame`) as `d.vars` fields
(closed schema — the one legitimate record-table use). Because V4 forbids suspended cursors, plan
bodies lower as **statement-segmented resumable graphs**: entry dispatches on the persisted `step`
path into the segment spine. An effect call emits an outbox entry, records `waiting`, and ends the
turn; the host delivery arrives on a (coalesced) wake and resumes at `step`.

**Suspension records the bound body identity** (owner-ratified, the verified staleness fix): at
adoption the intention records its clause's `+d.spin` target identity — the head's `+d.name` label
for installed copies (`patch:<n>`/`reify:` labels), the persistent ref int for image-original op
targets. Resume compares; a re-pointed `+d.spin` (`graft_body`/`learn_body` repoint at a fresh
installed head — `meta.cpp:1850-1851`) or a clause that left `clausesOf(host)` (prune/unlearn) fails
the plan with **`body_replaced`** / detached-clause faults — never a silent wrong-statement resume.
Per-level labeled segments (resumable-across-compatible-grafts) were considered and deferred — they
need a new emission contract for no current requirement.

**Failure ladder** (reuses existing machinery, no parallel states): exhaustion on user work → `fuel`
→ `fail_step` → `faulted`/`blocked_until` backoff → `drop when`. `ActorDown` only when recovery
itself cannot run.

### 3.6 `await` provenance

`await` does **not** copy the matched event — it links `+d.evidence →` the journaled `d.seen` cell.
That edge is the ownership that carries the cell through promote (`agent→waiting→event` is
frontier-reachable); ring eviction drops only the index edge. Contract required and ratified:
**event cells are immutable post-delivery.**

### 3.7 Outbox & drain

Entries are cell-legible records on `+d.outbox` (`d.vars`: `kind,target,payload,id`) — visible to
`cells"…"` and to the §11.3 harness at the commit boundary. The drain loop is **adapter C++**
(`rankAfter`/`linked` reads): the host-binding boundary is the explicitly priced one, no
host-reaching opcode exists, and C++ reads never call `consumeWork` — so post-commit drain carries
zero fuel risk by construction (§4.4). `requestId` stays the deterministic triple (sender-name hash
\+ turn counter + outbox position); **effect entries get the same key shape** so host bindings can
detect duplicates (`EffectFailed{cause:"duplicate"}` — detectable, not exactly-once; honestly
bounded).

### 3.8 The meta-verb seam — statement-boundary host routines

`learn_where`, `believes`, `set_weight`, `clauses_of`… are C++ verbs that Vortex statements name. A
graph cannot invoke C++, and a suspending-effect encoding is the wrong shape (these are synchronous,
in-turn, infallible-or-refusing operations). The convention — same one M4's segmentation needs
anyway: such a call lowers as a **statement-boundary host step**: the emitted segment ends at the
call, the executor runs the `MetaRuntime` verb synchronously between segments (cursor quiescent —
where `mintMetaBindSet`'s no-marks rule is satisfied by construction), binds results to the
continuation's inputs, and resumes at the next segment. This is the one invocation channel for
everything the spec calls "the adapter."

## 4. Boundaries, budgets, invariants

### 4.1 Arbitration — mesh-owned, finite-bounded (needs a P4 amendment)

`pumpOne` serves `mailbox.front()` whenever non-empty; wakes decrement only at `mailbox.empty()`
(`mesh_actor.cpp:520-531`). §11.1's starvation bound is **inexpressible adapter-side** — the
source-selection branch is inside the pump. Required mesh change: deficit counters on `Actor`
(`wakeStreak`/`msgStreak`) — serve wake if
`wakes>0 && (mailbox.empty() || wakeStreak < ready_per_message)`, else message and reset. Provable
bounds: `B_msg(p)=(K+1)·p`, `B_wake(q)≤q+⌈q/K⌉+1` turns at `K=ready_per_message=4`. Policy is
runtime-owned and versioned; the adapter declares work and owns within-source order. **P8's rotating
pump-scan cursor** is the second mesh change (cross-actor: any busy actor served within `maxActors`
calls).

### 4.2 Turn budget discipline

- Turn fuel covers **all** constituents — dispatch, handlers, every settle-round condition,
  selection `where`s — one aggregate `vagent.condition_turn_fuel` account, not 1000 steps per
  condition × 16 rounds.
- Nested `runFragment` `maxCycles` draws **from the turn's remaining fuel**, never a fresh 100k.
- `fail_step` recovery runs on **reserved fuel** held back at turn start.
- Cooperative deadline check *between* `run()` constituents → ordinary hard-failure path.
- Population caps (`maxIntentions`, `maxTimers`, `maxPending`, `maxHostDeliveries`,
  `maxClausesPerPredicate`) are named settings — the arbitration bound's finiteness depends on them.

### 4.3 The `believes`/settle accounting

Exhausted condition evaluations **defer remaining conditions to the next turn** (the turn is one
decision; partial settle commits what it evaluated) — they do not fail the turn. A permanently
un-evaluatable condition surfaces as a `condition` fault, never silent staleness.

### 4.4 Post-commit invariant (stated, debug-asserted)

Steps after `discard(m0)` mint/intern nothing and open no marks: drain = C++ `rankAfter` reads,
journal bookkeeping = field writes. Assert `choiceDepth()==0 && outstandingMarks()==0` before the
callback returns — a violation is an adapter bug, not a user fault.

### 4.5 Speculation discipline (the confirmed leak's rule)

**No interleaved second-cursor speculation while the actor cursor has open choice points** — nested
`stdlib.solve`/`call` runs at drained choice depth or is replaced by graph-walk matching.
`backtrack(nullopt)` matches any CP and would pop the actor's (see §5.1).

### 4.6 Memory — the stated invariant

`liveCells(turn boundary) ≤ grant.maxLiveCells`, measured where `checkpointView` would accept.
Enforcement path: drain-mode GC (pause ingress → wakes to quiescence → checkpoint+resume) now;
same-pid context replacement (U-A1) when it lands — needs only a turn boundary, which is guaranteed
after every turn. Until then, quiescence inside `vagent.reclaim_deadline` is a **named host duty**
escalated via `$stats` — not an environmental hope. "Bounded memory" without this is struck from the
claims.

### 4.7 Determinism/replay

Per-turn replay record: work source + item identity + arbitration counters + `$turn` time + mock
outcomes. Unordered-container iteration never reaches observables (verified: mesh maps are
`std::map`; VM unordered_sets are membership-only; `meta.cpp` internal maps get one sweep before M1
lands).

## 5. Confirmed defects adjacent to this design

### 5.1 `backtrack` drops foreign-cursor choice points without releasing their marks

`vortex_vm.cpp:315-331`: `backtrack(cursor)` pops `choiceStack_.back()` unconditionally; the cursor
test decides only whether to *use* it — a foreign CP is popped and dropped **without
`release(cp.mark)`** → `outstandingMarks_` stays elevated permanently → `checkpointView` refuses
forever. Verified by both the realist and codebase lenses. **Separate fix** (vortex patch + test),
not part of the executor — the executor's rule is §4.5 avoidance.

### 5.2 `step()` allocation profile

Per step: `inputsOf`/`outputsOf` vector allocs, `masterOf` clone-chain walks per operand, two
`unordered_set` heap allocs, `render()` string materialization per text operand. ~4 small allocs +
walks per step on vagent graphs (`attachPipeline` never invoked → pipelines inert). A freelist for
per-step sets is a reasonable optimization, not a correctness gate — measure before tuning.

## 6. Options declined (and why)

| Option                         | Verdict     | Reason                                                                                                                                                                                                         |
| ------------------------------ | ----------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Eager `d.worder` junction rank | **Dead**    | `set_weight` writes ≫ selections in Hebbian regimes (every `Meal` touches R clauses; selection is per-goal-instance); O(rank) relink per write loses ~10× to sort-at-selection. Purist conceded on arithmetic. |
| Virtual weight-order           | **Adopted** | ~30 lines in the dispatcher; same greedy semantics + early exit; rank-position tie-break; no maintained invariant under rollback. Owner-ratified.                                                              |
| `d.synapse`-based order        | Moot        | Subsumed by virtual-order — no maintained structure needed at all.                                                                                                                                             |
| `d.agents` rank                | **Adopted** | Mind reachable via `cells"##/+d.agents"`; tombstone delisting (durable-append-only under `commitStateSide`'s Additive-for-home).                                                                               |
| `d.seen` + `d.evidence`        | **Adopted** | Bounded-by-construction journal; consumers named (§9.3 provenance, `await` link). No payloads — identity + edges only.                                                                                         |
| `await` copies event           | Rejected    | `+d.evidence` link owns the cell through promote; copying severs provenance. Requires event immutability (ratified).                                                                                           |
| `body_replaced` fault          | **Adopted** | Silent wrong-statement resume verified possible; honest named fault beats snapshot-continue (masks divergence) and labeled segments (unneeded emission contract).                                              |
| Meta-verbs as graph ops        | Rejected    | `learn_*` needs C++ validation; statement-boundary host steps match "adapter drives the VM" and ride M4's segmentation for free.                                                                               |
| Outbox drain in Vortex         | Rejected    | No host-reaching opcode exists; C++ `rankAfter` is consumeWork-free by construction — the post-commit invariant is cheapest there.                                                                             |
| Turn batching                  | Rejected    | Merges failure domains; one message per turn defended by both lenses.                                                                                                                                          |
| Cross-arena event links        | Refused     | V1 stays; span-identity payloads are the transclusion channel (future U-A3).                                                                                                                                   |

## 7. Dialectic record

| Lens     | Round 1                                                                           | Round 2                                                                                                                                                                                                                                       |
| -------- | --------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Purist   | Full topology thesis; `d.worder`; step-path labels                                | **Conceded** `d.worder` (arithmetic), refined step-path to bound-identity check; 5 new non-negotiables adopted from realist (runtime-owned arbitration, stated memory invariant, fuel→ladder, effect idempotence keys, aggregate settle fuel) |
| Realist  | Starvation hardwired; unbounded garbage; fuel cliffs; unordered-determinism probe | **Found the third option** (virtual weight-order); verified backtrack leak; confirmed `d.agents` safe-with-tombstone; endorsed `d.seen` conditionally; mandate table classified mesh/adapter/spec                                             |
| Codebase | M0 gap; `d.clause` settlement; meta-verb seam; milestone order M0–M4              | **Verified step-path hazard real**; verified backtrack mechanics exactly; `d.agents`/`d.seen` implementable with named deltas; refined milestones (M0.5 mesh seams); enumerated 7 owner-level questions → 3 asked                             |

## 8. Milestones

- **M0 — declaration lowering + image assembly** (unowned prerequisite): walk `Decl` → module cells
  on programRoot `+d.stdlib`; agent/event/goal/predicate symbols on module `d.vars` (`exportSymbol`
  shape); each `plan`/`on`/`def`/`method` → clause/symbol cells with `compileFragment`-shaped heads;
  predicate decls → `d.belief` hosts; agent cell + `$`-reserved seeds + `home +d.agents`; file
  `d.agents`/`d.seen`/`d.evidence` in `kAgentDimensionNames`; `finalizeProgramImage` +
  `seedProgramImage` + lineage seed.
- **M0.5 — mesh seams**: deficit-counter arbitration + P8 scan rotation (both mesh-level, testable
  without the executor); `ActorOptions` tunables (`ready_per_message`, `journal_size`,
  `condition_turn_fuel`, `maxClausesPerPredicate`, `reclaim_deadline`, population caps).
- **M1 — wake + handler dispatch**: the Executor callback skeleton (steps 1–9 of §3.2) — mark →
  decode → `$turn`/`d.seen` → `on Event` dispatch via `clausesOf` + `runFragment` on the actor
  cursor → commit/rollback → C++ drain → zero-residual assert. Translates `consumeWork` → turn
  fault; no second-cursor speculation under open CPs; §11.3 harness shape.
- **M2 — beliefs**: `d.belief`/`d.seen`/`d.evidence` ranks live; `believes`/`believe`/`forget`/
  `revise` host verbs; `d.pending` internal events through the settle loop.
- **M3 — plan selection**: adoption → `clausesOf` → virtual weight-order → per-candidate `where`
  under condition fuel → `tried`; resume guards (`body_replaced`, detached clause); statement-
  segmented body lowering lands here (with M4's consumers).
- **M4 — intentions/effects**: intention records + `step` resume; `d.subgoal`; waits/awaits with
  `+d.evidence` links; outbox staging + post-commit drain + idempotence keys; cancellation/
  deadlines; arbitration-counter consumption.
- **§9.2/§9.3 gate**: `learn_where`/`learn_plan`/`set_weight`/`believes`/`clauses_of` reachable from
  handler bodies via §3.8 → `ActorExecutorBoundaryRemainsGated` un-skips.

## 9. Decisions

**Owner-ratified (this session):**

1. **Virtual weight-order** for plan selection — spec §6.3 amended; rank-position tie-break.
1. **`body_replaced`** fault on resume-mismatch — never silent wrong-statement resume.
1. **`d.agents` + `d.seen`/`d.evidence`** adopted (tombstone delisting; bounded journal; §9.3
   provenance + `await` as named consumers).

**Forced rulings (no real alternative):**

1. Tombstone-on-agent delisting (not `Clears` on home — blast radius too wide).
1. Meta-verbs = statement-boundary host routines (not suspending effects, not graph ops).
1. Event cells immutable post-delivery (required by `await` evidence linking).
1. Wake = coalesced "service pending" flag; starvation still needs §4.1's mesh counters.
1. Outbox drain = adapter C++ (host boundary; consumeWork-free post-commit).
1. Effect idempotence keys = the existing requestId triple shape.

**Deferred with stated shape:** labeled segment heads (needs emission contract — revisit only if
resumption-across-grafts becomes required); `d.worder` lazy re-sort for selection-dominated goals
(data decides); U-A1 same-pid rebuild (promoted from deferred to required direction); span-typed
payloads (U-A3); agent-authored xanadocs (U-A9); turn journal to `system://activity` (U-A8).

## 10. Proposed test plan

- **M0**: module/agent structure present post-lower; `cells"##/+d.agents"` reaches the agent;
  dim-coverage test extended (`vagent_lineage_test.cpp:107-127` shape).
- **M0.5**: mailbox-flood starvation test — K+1 bound asserted turn-exactly; P8 rotation test.
- **M1**: scripted turn harness — deliver event → handler fires → outbox staged → committed;
  hard-failure turn leaves zero marks/choice depth; `checkpointView` accepts after commit.
- **M2**: `believes`/`believe` round-trip through a turn; provenance edges present; fuel-cliff fault
  (not silent staleness) at `maxClausesPerPredicate`.
- **M3**: weight-informed selection order asserted; tie-break = rank position; `learn_where` affects
  the *next* selection; `body_replaced` fault on mid-suspension `learn_body`; detached- clause
  (`unlearn` mid-suspension) fault.
- **M4**: suspend→wake→resume across an effect; `await` evidence link survives a checkpoint → resume
  round-trip; duplicate-drain detection via idempotence key.
- **§9.2/§9.3**: the two worked examples run green end-to-end; `ActorExecutorBoundaryRemainsGated`
  un-skips.
- **Regression**: backtrack mark-leak test (§5.1) lands with its fix.

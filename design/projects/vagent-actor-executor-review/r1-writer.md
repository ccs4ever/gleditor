# r1-writer — document-coherence critique of `design/vagent-actor-executor.md` (v0.1)

**Verdict:** internally consistent on the headline rulings (virtual weight-order, `body_replaced`,
`d.agents`/`d.seen`/`d.evidence`, coalesced wakes) but carries three wrong code claims, one silent
divergence from the spec turn it claims to implement, and a milestone-attribution conflict. Counts:
**5 MAJOR, 11 MINOR, 5 NIT** (21 findings). mdformat `--check --wrap 100` and `mdl -i` both run
clean on the file as committed.

## Findings

1. **MAJOR — §2.1 "Weight storage" bullet + §3.4 selection cost.** The doc claims "A clause cell
   *is* a junction: `setWeight`/`weightOf` hit the scalar `+d.weight` edge arm — O(1) trailed edge
   read/write (`meta.cpp:2038-2064`)". The cited code refutes it: `setWeight`'s clause arm
   (meta.cpp:2050-2059) mints/updates a **`d.vars` record on `binds_.agentCell` keyed by the
   clause's `+d.name`** — the junction arm fires only when a `+d.weight` edge already exists
   (synapse cells, not clauses). `weightOf` is the same two-channel read at meta.cpp:602-619 —
   outside the cited range — and `varsRecord` is a linear `rankAfter` scan (records.hpp:69-79), not
   O(1). §3.1's own cell-map correctly files "weight/… records" under `+d.vars`, so §2.1 contradicts
   the doc's own figure, and §3.4's per-candidate "O(1) `+d.weight` edge reads" inherits the error.
   **Fix:** rewrite the bullet to name the keyed-`d.vars` channel for clauses (meta.cpp:602-619,
   2038-2064), re-price §3.4's selection walk honestly (one keyed `d.vars` scan per candidate, or a
   stated new indexing channel), and extend the cite.

1. **MAJOR — §3.2's nine-step turn silently drops two spec §4.2 steps.** Spec §4.2 has ten steps;
   the doc's list omits **step 4 "Base"** (`setCurrentDeweyPath({})` + a base choice point whose
   alternative is `std:agent`'s step-failure routine — the thing that makes "no remaining choice
   points" unreachable, spec :334-336, :360) and **step 8 "Scope"** (unlink the cursor's
   `+d.vars`/`+d.stack` ranks or "the cursor's scope would grow for the agent's life"; plus
   `compact_every`, spec :344-347). Neither is mentioned anywhere in the doc (grep: no "Dewey",
   "base choice", "compact", cursor "scope" outside runFragment's per-call restore). Since M1 makes
   "steps 1–9 of §3.2" the normative skeleton, the omission is a missing work item, not a
   simplification — unless deliberately deviated, which must then be recorded. Spec §4.2 step 9's
   prescribed **drain order** (local spawns → messages → effect cancels → `preempts` effects → other
   effects → timer changes, spec :348-350) is likewise absent; §3.7 says only "host bindings
   dispatch (send/spawn/effect)" while the requestId key uses outbox *position*, leaving the drain
   order ambiguous (position order vs the spec's kind order). **Fix:** add Base and Scope steps to
   §3.2 (or an explicit "differs from §4.2" ruling), and state the drain order in §3.7.

1. **MAJOR — §3.1 parenthetical enforcement claim is false.** "(R12 discipline — no dim mints under
   a live mark; `mintMetaBindSet`/`ensureDimension` both refuse then)". `mintMetaBindSet` does
   refuse under outstanding marks (meta.hpp:131-134), but `ensureDimension` does not and cannot — it
   returns `DimRef`, has no mark check, and mints unconditionally (arena_manifold.cpp:78-105). The
   no-marks rule is a *discipline*, enforced at exactly one seam. An implementer trusting the stated
   guard gets the desync the comment warns about. **Fix:** "no dim mints under a live mark —
   `mintMetaBindSet` enforces it; `ensureDimension` itself never checks, so the adapter must mint
   only quiescent."

1. **MAJOR — milestone ownership of body segmentation is contradictory, and handler-body
   segmentation is unowned.** §8 assigns the work to M3: "statement-segmented body lowering lands
   here (with M4's consumers)". But §3.8 calls the host-step convention "the same one **M4's
   segmentation** needs" and §6's meta-verb row says host steps "ride **M4's segmentation** for
   free". Worse, the §9.2/§9.3 gate needs `learn_where`/`set_weight` "reachable from **handler
   bodies** via §3.8", and §3.8's mechanism ("the emitted segment ends at the call") requires the
   *handler* body emitted segmented too — yet every segmentation mention names plan bodies (§3.5's
   resumable graphs; §8 M3's "body lowering" in a plan-selection milestone). No milestone owns
   handler-body segmentation. **Fix:** decide one milestone owner, name handler-body and plan-body
   segmentation explicitly in §8, and align the §3.8/§6 phrasing with §8.

1. **MAJOR — §3.5's quoted intention record shape drops `delegator`.** Spec §5.6
   (vagent-agent-language.md:598-603) lists twelve `d.vars` fields:
   `id, kind, goal, plan, tried, step, waiting, ready, delegator, blocked_until, faulted, frame`.
   The doc's parenthetical "the spec'd record shape" enumerates eleven and omits `delegator` (a peer
   cell and correlation id — load-bearing for delegated `achieve`, spec :844, :953). **Fix:** add
   `delegator` to the §3.5 enumeration.

1. **MINOR — cite range for `MetaRuntime` is wrong.** §2.1 says `meta.hpp:186-450`; the class is
   declared at meta.hpp:196 and closes at :614. Fix: `meta.hpp:196-614`.

1. **MINOR — three §3.2 cross-references point at the wrong §4 subsection.** Step 5's "(§4.3)" for
   "aggregate condition-fuel account" and the failure bullet's "(§4.3)" for "reserved fuel" both
   belong to §4.2 (bullets 1 and 3); §4.3 is the deferral rule. Step 2's "(coalesced, §4.1)" cites
   arbitration for a rule stated in §3.3. Fix the pointers (§4.2 ×2, §3.3 ×1).

1. **MINOR — "provenance triple (source/ordinal/requestId)" (§3.3) is an undefined term that
   contradicts the doc's own field list.** §3.2 gives the event cell "implicit
   source/space/time/ordinal" (matching spec §5.5's four implicit fields, :572-574); §3.3 names a
   different three — dropping `space`/`time` and adding `requestId`, which is the *wire* key
   (§5.5:583-586), not an event field. Fix: either define "provenance triple" deliberately or write
   "the four implicit fields plus the wire requestId".

1. **MINOR — `grant.maxLiveCells` does not exist.** §4.6's invariant reads
   `liveCells(turn boundary) ≤ grant.maxLiveCells`, but `ActorGrant` has `maxCells`, not
   `maxLiveCells` (mesh_actor.hpp:55-65), and M0.5's tunables list omits it. If it is a new setting,
   it needs to be named as such and homed (grant vs `ActorOptions`). Fix: add it to M0.5's list and
   mark it new.

1. **MINOR — dialectic arithmetic does not reconcile.** Header: "eleven spec-level questions
   collapsed to the three the owner answered plus forced rulings"; §7 codebase row: "enumerated 7
   owner-level questions → 3 asked". 11 vs 7 is unexplained (presumably 11 total, 7 owner-level).
   Fix: one clause stating both numbers' classes.

1. **MINOR — `§9.2`/`§9.3` self-collision.** This document has a §9 (Decisions); every bare
   "§9.2/§9.3" (§3.1, §6 rows, §8 gate, §10) actually means vagent-metaprogramming.md §9.2/§9.3.
   Internally consistent to a careful reader, ambiguous to anyone else — exactly the citation this
   spec gates on. Fix: qualify as "metaprogramming §9.2/§9.3" throughout.

1. **MINOR — §4.1's stated policy is not self-contained.** `msgStreak` is declared and never used in
   the rule; the "provable bounds" `B_msg(p)=(K+1)·p`, `B_wake(q)≤q+⌈q/K⌉+1` never say what `p`/`q`
   count or what the bound measures (turns until service of the other class?); the counters'
   increment/reset rules are half-given ("else message and reset" — reset which?). Fix: state both
   counters' update rules, define `p`/`q`, and sketch the argument or drop "provable".

1. **MINOR — "step" is overloaded five ways, never disambiguated.** §3.2's turn steps; the spec's
   `step` resumption-path field (§3.5); `VortexVM::step()` (§5.2); §3.8's "statement-boundary host
   step"; `fail_step`. Each use is locally readable but the terms collide across sections (M4's
   "`step` resume" vs §3.8's "host step" in adjacent subsections). "settle-round"/"rounds"/
   "belief-event rounds" similarly vary. Fix: a two-line glossary or consistent qualifiers.

1. **MINOR — two stale/inexact cites.** §3.4 quotes Spec §6.3 as "first applicable in rank order" —
   the spec's words are "the first plan in the plan list" (:758) / "first applicable untried"
   (:2857); quoting a paraphrase misleads. §3.5 cites `meta.cpp:1850-1851` for "`graft_body`/
   `learn_body` repoint" — those lines are `learn_body` only; `graft_body` repoints at
   meta.cpp:1345-1346. Fix: de-quote the paraphrase; cite both repoint sites.

1. **MINOR — §9 deferred list mislabels U-A8.** "turn journal to `system://activity` (U-A8)" — spec
   U-A8 (§18.8, :3045-3074) is per-agent deterministic *replay recording*; a journal to the
   `system://activity` store is a different concept (R8 lineage). The doc's own §4.7 replay record
   is the honest U-A8 item. Fix: split into "deterministic replay recording (U-A8)" and, if
   intended, a separately-named activity-journal deferral.

1. **MINOR — "Forced rulings (no real alternative)" overstates its members.** Items 4 (coalesced
   wake) and 6 (requestId triple reuse) each had a *real* alternative — one-wake-per-item was priced
   out, a distinct key shape was feasible — the doc just rejects them well. The other four are
   closer to forced. Fix: rename the class ("ratified defaults with priced alternatives") or show
   per-item why the alternative is not real.

1. **NIT — §3.1 cell-map glyph bugs.** Under `[fact]` both children render `└─` (the first should be
   `├─`); `├─(+d.code)… ├─(+d.patch)…` packs two siblings on one row with the second connector
   floating mid-line. The figure is the normative topology — fix the tree characters. (Verified
   content: `d.agents`/`d.seen`/`d.evidence` correctly marked NEW; all other named dims are filed —
   `d.belief`/`d.intention`/`d.subgoal`/`d.outbox`/`d.pending`/`d.source`/`d.extends` in
   `kAgentDimensionNames`, lineage.hpp:67-71; `d.code`/`d.patch`/`d.synapse`/`d.weight` in
   `kMetaDimensionNames`, :58-62; core `d.clause`/`d.grab`/`d.spin`/`d.vars` as engine dims.)

1. **NIT — §6 coverage/vocabulary.** The `d.synapse`-based-order row is the only place that option
   is discussed — absent from §3's body prose (a one-line mention would complete the record). The
   adopted order appears under three spellings — "virtual weight-order" (§3.4/§6/§9, consistent),
   "sort-at-selection" (§6 reason cell), "virtual-order" (§6 row) — harmless but pick one. No
   residual "rank order" or "argmax" wording survives for plan selection — good. Every other
   declined option in §6 is discussed in the body (§3.1–§3.8, §3.3) — verified.

1. **NIT — cite imprecision.** `mesh_actor.cpp:549-551` (§3.2) covers the catch→`ExecutionFailed`
   mapping; the "actor stops forever" mechanism is `:612-617`. Extend the range.

1. **NIT — miscellany.** M0's "(unowned prerequisite)" never says whose prerequisite; each
   milestone's exit criteria live only in §10 (traceable — every milestone M0/M0.5/M1/M2/M3/M4 + the
   gate + regression has a test row — but not cross-linked); §3.2 step 5 writes bare
   `belief_event_depth` vs the spec's `vagent.belief_event_depth`; the related-docs bullet "§6.5–
   6.6 intentions" mislabels (§5.6 is intentions; §6.5/§6.6 are the deliberation cycle and
   suspension points); §2.1 has a stray space in "`weightOf`/ `setWeight`".

## Verified-accurate citations (no action)

`mesh_actor.cpp:510-621` pumpOne, `:520-531` wake/mailbox arbitration, `:549-551` catch path,
`:781-793` wake/pendingWakes, `:200-210` + `:333-347` GenesisHook, `:829-855` checkpointView;
`mesh_actor.hpp:89-97` optional `ActorExecution.message`; `meta.cpp:210-220` clausesOf, `:1167-1192`
clauseHost, `:1396-1407` + `:1732-1741` graft/learn clause minting, `:664-800` runFragment;
`vortex_vm.cpp:315-331` backtrack foreign-CP drop (§5.1 claim confirmed: pop at :317 unconditional,
`release` at :319 conditional), `:745-829` Link/Break/Linked; `compiler.cpp:716` lowering refusal;
`ast.hpp:184-231` Decl/DeclKind; `fragment.hpp:166-168` compileFragment; `compiler.hpp:200-208`
imageFloor\_; `vagent_lineage_test.cpp:107-127` dim-coverage test;
`vagent-metaprogramming.md:2808-2880` = §9.2/§9.3 worked examples exactly;
`-implementation.md :36-38` "greenfield" quote verbatim; `vagent-agent-language.md:596-615` = §5.6;
V1/V2/V3/V4/V5/ V10/V13, R12, P4/P8/P15/P16, U-A1/U-A3/U-A9 all exist as cited. §11.1's
starvation-bound requirement is at :1644-1657. `consumeWork` throws `std::runtime_error`
(arena_manifold.hpp:481-486). §4.2's "1000 × 16" matches the spec defaults
(condition_fuel/belief_event_depth, :1569-1570); `ready_per_message=4` (:1575). All six §9 forced
rulings and all three owner rulings appear in the body with their refused alternatives; no hedging
("TBD", "will", "should", "convention needed") anywhere; the `d.synapse` dim itself remains live for
weighted edges (its "Moot" applies only to the ordering option — correctly scoped).

VERDICT: NOT CONVERGED

# Convergence record — vagent-actor-executor design review

**Status: CONVERGED at v0.5** (design-draft scope), declared after the round-4 all-lanes
verification returned **7/7 CONVERGED** with 0 blocking / 0 major. The document remains explicitly a
design draft — nothing in it is implemented.

## Round trajectory

| round | blocking | majors | converged lanes | dominant theme                                                                                                       |
| ----- | -------- | ------ | --------------- | -------------------------------------------------------------------------------------------------------------------- |
| r1    | 8        | ~19    | 0/7             | false code claims: weight channel, `release(m0)` under CP marks, `d.agents` blast radius, replay-redrawn identity    |
| r2    | 1        | 8      | 0/7             | r1-introduced break: exit protocol wrong on both exits; §3.8 seam gaps (expression verbs, M6 purity, belief channel) |
| r3    | 1        | 4      | 2/7             | one shared mechanism: `cut(1)→release(m0)` rested on unstated `E_base==E_m0` zero-write-gap invariant                |
| r4    | 0        | 0      | **7/7**         | verification — base-CP-before-`m0` reorder verified "stronger than claimed"                                          |

## Verified-core mechanisms (no open majors)

- **Turn protocol**: `m0` with base CP pushed *before* it — commit `cut(0)→discard(m0)`, fail
  `cut(1)→release(m0)→backtrack(cursor)` onto the surviving pad; `release(m0)` unconditionally
  well-nested; `choiceDepth==0 && outstandingMarks==0` assert reachable on both exits.
- **Plan selection**: virtual weight-order (owner-ratified, §6.3 amended) reading the real channel —
  `d.vars` label-keyed weight records snapshotted once per selection, rank-position tie-break.
  Clauses never carry `+d.weight` (V12).
- **Intentions**: statement-segmented resumable bodies; suspension records the clause's
  registered-head *name* (stable across genesis replay — `reify:` labels are not);
  `body_replaced`/detached-clause faults on resume mismatch.
- **Meta-verb seam**: split by position — statement-position host steps (`+d.verb` ABI, `+d.vars`
  marshaling, ANF-hoist for expression-position calls) vs fragment-position inline clause-walk
  graphs (fragments pure; narrows reference-spec M6 — flagged for owner).
- **Topology**: `d.agents` off a purpose-minted directory cell (`+d.dirs`), `d.seen` ring closed
  through the agent cell, `+d.evidence` `await` provenance, `+d.holder` symbol-side ownership for
  multi-agent dispatch.
- **Bounds**: mesh deficit-counter arbitration (P4 amendment — mailbox-beats-wakes is structural);
  virtual sub-account fuel (per-constituent step counters); population caps; owed
  `maxLiveCells`/`liveCells()`; post-commit consumeWork-free drain.
- **Milestones**: M0 decl lowering + dispositions → M0.5 mesh seams + §5.1/§5.2 defect fixes → M1
  turn skeleton → M2 beliefs + segmentation + host-step ABI → M3 selection → M4 intentions →
  §9.2/§9.3 gate.

## Confirmed adjacent defects (owned by M0.5, not the executor)

- §5.1: `backtrack` pops foreign-cursor CPs without `release(cp.mark)` → permanent `checkpointView`
  refusal.
- §5.2: `home +d.cursors` accretes a dead cursor twin per incarnation (pre-existing).

## Spec-level rulings flagged for owner confirmation

1. ANF-hoist for expression-position verb calls (§3.8).
1. Fragment purity — narrows `vagent-metaprogramming.md` M6's impure-verb-in-`code"…"` allowance
   (§3.8).
1. `+d.holder` symbol-side ownership replaces per-clause `d.owner` (r3 collision).

## Process verdict

Four rounds: 8→1→1→0 blocking. The dialectic's converged positions survived contact with the code
only after the panel refuted four "verified" claims that had drifted — the workflow's value was
precisely there. Remaining minors are implementation-note level and carried in the r-reports.

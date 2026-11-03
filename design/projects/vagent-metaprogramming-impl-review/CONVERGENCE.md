# Convergence record — vagent-metaprogramming implementation-plan review

**Status: CONVERGED at v5** — round 5 of a maximum-5 critique run. All seven
lanes converged with 0 blocking / 0 majors; residual findings are nits and one
cite-scoping minor, all applied in the final pass.

## Round trajectory

| round | converged | blocking | majors | dominant finding |
| ----- | --------- | -------- | ------ | ---------------- |
| r1    | 0/7 | ~12 | ~30 | structural: inverted genesis order, std:meta/phase-3↔4 inversion, phase-5↔6 boundary through genesis, unowned inventory, silent substrate preconditions |
| r2    | 2/7 (+1 substance) | 0 | 2 | (e)'s third arm (DimRef dim records) unassigned; re-genesis fold loop unproduced |
| r3    | 5/7 | 0 | 2 | splice call-by-value marshalling unstated; promote-visibility overclaimed (ephemeral-only, mint-and-repoint) |
| r4    | 4/7 | 0 | 1 | spanning-root promote alternative republishes grafts (three-lane, inside r3's own fix text) |
| r5    | **7/7** | 0 | 0 | verification round — the root fix landed correctly; nit-level residues only |

## What the rounds bought

- The phase structure was re-cut at the real boundaries: `std:meta`
  scaffolding + read group before the verbs that consume it (phase 3↔4);
  session `learn_*` machinery before durable semantics (phase 4↔5); genesis
  machinery + lineage seed + filing grouped with durability, phase-5↔6
  boundary at *transport*.
- Genesis letters/order now match the spec exactly ((d) replay → (e) rebind
  last; (f) = turns) — the plan's own inversion would have reintroduced the
  misbind class R7-B removed.
- Every consumed artifact gained a producer: `programRef`/`E_ops`/instruction
  enumeration, the bind set, the genesis driver (`genesis.{hpp,cpp}`), the
  re-genesis fold loop (promote → rebuild → fresh `WorkerContext` → re-file →
  (a)–(e)), P16 marking split (phase-3 record schema, phase-6 marker mint).
- The promote semantics are now stated precisely: agent-cell-only root,
  `isEphemeral`-only content ops, mint-and-repoint counters (no-ABA),
  unlink-vs-tombstone removal classes, first-fold orphan residual.
- A preconditions section names the owed substrate (actor executor, `#Call`
  ABI, V10 dims, P4 wake) and every phase marks exits kernel-simulable vs
  adapter-gated — the plan no longer assumes vagent iterations 4–9 exist.

## Parked nits (non-blocking)

- `mesh_actor.cpp:473-477`→:474-476 for `executor_`; spec :2163-2172 covers
  both the `pending(pid)==1` flag test and the no-turn veto.
- Phase-3's "non-mutation" label covers `run`/`reify` which are impure-by-
  function — named, understood.
- `vagent/prelude/` is forward placement, worded as convention.
- mdformat slash-wraps at a few sites (formatter output, accepted).

## Verdict

The plan is implementable end-to-end: every phase's exit is checkable against
machinery its own or earlier phases deliver; every spec inventory item
(§10.1/§10.2) has a phase; every phase has a Requires line honest about the
missing executor substrate. Further critique rounds parked — implementation is
the next verification surface.

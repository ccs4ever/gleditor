# Round 7 critique (vortex role): vagent-agent-language.md v3.4

Paths are under `apps/common/xanadu/` unless stated. Checked in this worktree against the current
document (v3.4) and the round-6 disposition. P1-P11 are not re-argued; P12-P14 are treated as
pending.

## 1. Status of round-6 BLOCKING and MAJOR findings

- F1 (Dewey path grows without bound): RESOLVED. Step 4 resets with `setCurrentDeweyPath({})`
  (declared at vortex_vm.hpp:133), 4.4 prices the residual O(k^2) and P12 removes it. One stale
  sentence remains (minor list).
- F2 (trail and relocated runs after `discard(m0)`): RESOLVED. 4.4 lists the trail, step 8 calls
  `compact()` every `vagent.compact_every` turns with no mark outstanding, and P13 is the pending
  fix. I re-read `discard` (arena_manifold.cpp:1595-1606) and `compact` (1698-1722): `compact`
  refuses only on `outstandingMarks_` and rewrites `links_` and `content_` without touching
  `trail_`, which is harmless because no mark is outstanding and later marks record `trailSize`
  afresh.
- F3 (floor signature; silent senders): RESOLVED. P3 now has the signature and P14 covers
  `forgetSender`; the fallback bound is stated in P14's "Without it".
- F4 (`Initialize` ordering and `resume`): PARTLY. The ordering and `$initialized` are specified and
  `ActorContinuation` does keep `spawnRequest` (mesh_actor.cpp:683-686). The failure of the first
  turn is not (F1 below).

## 2. Findings

### F1. MAJOR (4.1, 4.2 "Recovery after a hard failure"): a hard failure in the first …

Full title: F1. MAJOR (4.1, 4.2 "Recovery after a hard failure"): a hard failure in the first turn
leaves `$initialized` unset, so genesis re-runs on every later turn and every message is dropped

- 4.1 says the first turn of any kind runs genesis and `Initialize`, then sets `$initialized`. The
  flag is an ordinary reserved field written under `m0`. On a hard failure step 7 runs `release(m0)`
  (the arena truncates and the trail restores everything written since the mark,
  `ArenaManifold::release`, arena_manifold.cpp:1608 onward), so the flag, and genesis's cells, are
  undone. The recovery paragraph then says the message or wake is consumed and, for an event that is
  not an `ask` or an intention, "dropped and counted in `$stats`".
- Genesis is deterministic code (field initialisers, mounted behaviors) plus `Initialize` handlers.
  If it exhausts cells, work, scratch bytes or `maxCycles` once, it does so again at the next turn,
  because the next turn also starts uninitialised. Failure scenario: an agent whose field
  initialiser builds a 40 KiB string, or whose `on Initialize` loops, receives ten messages. Each
  message triggers genesis, hits the same fuel limit, undoes it, and is dropped. The agent stays
  "alive" (the executor returns success, so `pumpOne` does not stop it), answers every `ask` with
  `remote_fault`, and never runs a handler. Nothing in 4.2 or 15 says so, and `$stats` is undone by
  `release` along with the rest unless it is written after the undo (the text does not say it is).
- A handler failure inside `Initialize` is different and fine: it is caught by the statement's
  choice point (V13), and `$initialized` is set. Only the hard failures (limits, cycle exhaustion,
  `Assertion failed`, event depth) are affected.
- Fix: add to the recovery paragraph: "A hard failure in a turn that was running genesis or
  `Initialize` is fatal for the agent: the adapter returns an error from the executor, which stops
  the actor (`pumpOne` sets `stopped`, mesh_actor.cpp:502 area), stages
  `ChildStopping{cause:\"init\"}` to the parent, and does not retry." State also that `$stats`
  counters are written after `release(m0)`, in the recovery turn, so the drop is counted. Add the
  row to 15.

## 3. Minor items

- 4.2 closing paragraph of the steps still says "vagent never sets an oracle path, so the VM's Dewey
  path is unused". Step 4 and 4.4 now say it grows and is reset each turn. Delete the clause or
  write "so the only consumer of the Dewey path is the cut callback, which vagent does not set".
- 4.1 has a comma splice: "so they survive `resume`, The executor's first turn of any kind" should
  be a full stop.
- 4.2 step 7 clears choice points then calls `discard(m0)`. Say that `cut(0)` discards each choice
  point's mark in LIFO order (vortex_vm.cpp:218-232), so `m0` is the last mark and P13's trigger
  condition is reached at that call.

VERDICT: NOT CONVERGED

# Round 8 critique (vortex role): vagent-agent-language.md v3.5

Paths under `apps/common/xanadu/`. Only BLOCKING and MAJOR reported.

## Previous findings

- R7 F1 (first-turn hard failure leaves `$initialized` unset): RESOLVED. 4.2 "Recovery after a hard
  failure" now makes it fatal: `ChildStopping{cause: "init"}` staged, executor error returned, no
  retry, `$stats` written after the undo. Verified against code: a failing executor result sets
  `actor->stopped = true` (vortex/mesh_actor.cpp:501), and `pumpOne` skips stopped actors (:462), so
  the stated behaviour is what the runtime does. The runtime also sends `ActorDown` ("execution
  failed") to a requesting source (:484-496), which matches the doc's note that the parent may see
  `ActorDown` after `ChildStopping`.
- Earlier rounds' MAJORs (F1 Dewey path, F2 trail/compact, F3 floor/P14, F4 init ordering): RESOLVED
  or PARTLY closed by R7 F1 as above.

## New BLOCKING / MAJOR findings

None.

## Minor

- The event table (about line 1451) lists `ChildStopping{child}` without the `cause` field that 4.2
  now stages (`cause: "init"`); add the field there.
- "returns an error from the executor" in 4.2 relies on the executor's `std::expected` error path
  (mesh_actor.cpp:481-501); a one-clause citation would help the implementer.

VERDICT: CONVERGED

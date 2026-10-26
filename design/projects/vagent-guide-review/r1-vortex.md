# Vortex/runtime review — R1

Verdict: CONVERGED — no actionable runtime-contract or teaching errors found.

I audited the complete frozen guide against the vagent v3.9 design and the Vortex/vicon grounding
notes, concentrating on turn and statement rollback, scanning, waits and captured values, effect
staging, scheduling, checkpoints, and limits. The guide consistently labels these passages as
specified behavior rather than claiming the frontend checker executes them.

The runtime-sensitive explanations agree with the contract:

- Handler writes and staged actions are discarded when that handler fails; a later ordinary plan
  statement failure preserves prior successful statements, while hard turn exhaustion rolls back the
  whole turn. The guide distinguishes these scopes.
- Scanning is presented as a cursor environment: `match("ID:")` checks and advances past the prefix,
  while `upto(";")` finds a stopping position and `tab(...)` returns the text consumed up to that
  position. The example yields `"bolt"`; absent delimiters fail.
- Belief awaits check existing facts first, event awaits are future-only, and the matching
  payload/bindings are copied for resumption. Deadlines and late completion behavior are described
  at the intention boundary.
- Effect calls stage host work and suspend plans; handler effects cannot return results into handler
  locals. Recovery handles wait failure and does not reverse an already-performed physical action.
- Scheduling statements avoid promising hard real-time service or priority preemption. Checkpoint
  readiness constraints, pid changes on resume, and operational limits are identified as
  specified/future adapter behavior.

No concrete correction is warranted within the requested guide scope.

# Round 5 disposition (orchestrator) — document is now version 3.3

OWNER DECISION: all eleven proposals P1–P11 in §11.2 are APPROVED. Treat them as approved changes
the design may depend on; do not re-litigate whether they are needed. You may still flag a missing
proposal (a dependency on a change to existing code that is not in the register) or a proposal whose
described change is wrong or insufficient.

Round 5 fixes applied (v3.3):

- ZigZag BLOCKING: d.extends is a flattened rank of copied supertype-name cells.
- Lists/maps use std:collections layouts (list value cell +d.grab → first element, d.step chain; map
  = std:collections map). Minting in Vortex by walking d.dims (ensureDimension equivalent);
  attachment by tail walk in Vortex.
- Vortex N1: P3 is an ActorOptions::floorOf hook (wire unchanged). N2: erasure deferred while
  pumpOne runs; step 9 is re-entrant. N3: P1 marked load-bearing, std:goal host routines refused
  with reason, touch list corrected. N4: std:agent is a prelude linked into the artifact (no C++
  module); second artifact format justified vs sealVortexProgram (512-token SyntaxLimits, pure
  allowlist, no stdlib refs). N5: P4's real fallback (host-pid wake messages). N6-N10: `stopped` in
  quiescence, Mark.cellCount, 16 KiB capability limit with fallback, $turn_count raised on resume,
  Dewey path unused.
- AOP: hazard handler — halt preempts Mobility effects (Chassis contract), drain does cancels before
  new effects, restorers dropped via restorers(i); back-off cleared only by time; §12.3 notes P1
  dependency; §12.4 keyed by order_id and spawn-failure text corrected; "own writes" defined; goal
  intention on restorer end.
- Ergo: skills may `uses`; effect bodies in vagent skills rejected; return only in methods, answers,
  defs; implicit event field types; std:agent imported implicitly; Moved → Relocated.
- Writer/Ergo: settings (P9), logging category (P10), build rules (P11) added to register;
  prerequisite fallbacks stated.

ADDENDUM (applied mid-round-6, before the writer, vortex and ergonomics critics ran):

- ZigZag R6 MAJOR: dimensions are registered by the adapter via ArenaManifold::ensureDimension (so
  arenaDims\_ knows them) and handed to std:agent; Vortex no longer mints them (V10, §10).
- AOP R6 MAJOR: `effect preempts` modifier on capacity methods (grammar MethodSig, §5.3 host
  contract: preempting effect starts at once, ends the in-flight effect of the same capacity with
  EffectFailed{cause: "preempted"}); drain order in §4.2 step 9 is spawns, messages, effect cancels,
  preempting effects, other effects, timers; rover's halt is `effect preempts`.
- Minors: `from self` on battery reads in KeepCharged and travel; list unification is element-wise,
  equal length; §15 notes constant back-off retries for ever (bound with `drop when`).

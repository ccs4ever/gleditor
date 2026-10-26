# Round 2 disposition (orchestrator) — document is now version 3.0

Accepted, with the design changes they forced:

- Vortex F1 (bounded statements unbuildable with existing opcodes; vicon/compiler.cpp:12-22): vagent
  now owes ONE opcode, `#ChoiceDepth`, and lowers a bounded statement as depth-at-entry +
  `#Cut depth` (V13, §9, §11.1). The "no new opcode" claim is withdrawn. Adapter uses
  `clearChoicePoints()` at turn end (§4.2).
- Vortex F2 (self-sends collide with `remembered`): wake-ups are no longer messages. Ready
  intentions, timers and host deliveries are data in cells; a local `wake(pid)` (mesh change d) runs
  the executor with no message (§4.2, §11.3).
- Vortex F3: delivery retries + abandonment (§7.3, mesh change a now includes abandon).
- Vortex F4: spawn placeholder peer cells; issuer for child capabilities; Initialize carries
  arguments (ActorSpawn value must be empty) (§4.1, §8.6).
- Vortex F5: 32 KiB value, sequence==0, no ordering guarantee (ordinal carried in value), turn time
  written into the arena by the adapter (§5.5, §7.3, §4.2).
- Vortex F6 + AOP #1, #5: effects are now STAGED and ASYNCHRONOUS. `effect` methods are only
  implemented by `native skill`s bound to host code; in plans an effect call is a suspension point
  resumed by EffectDone/EffectFailed; in handlers it is staged without waiting (V6). This makes
  forward failure real (writes before the last suspension committed) and the agent never blocks on a
  device. Perception: host drivers post percept events (§7.5).
- Vortex F7 + ZigZag #2: checkpoint admitted to be the one promotion (into the value's transient
  store), with ops ceiling, permascroll growth, and a mesh change (e) to make ValueLimits an option
  (V9, §4.4, §16).
- Vortex F8 + ZigZag #1: capacities matched by NAME; no state cell links into program/home (V12).
- Vortex F9: resolved by staged effects.
- Vortex F10: calls use vicon's Bind+Jump convention, not #Call (§9.2).
- Vortex F11 + Writer #1: Vlog §6 database and vicon/std:goal are listed as prerequisites, not built
  (header, §1).
- Vortex F12: mailbox limits and settings table (§10, §15, §12.1 note).
- Vortex F13: leases and renewal (§4.5); ChildDown latency stated; ChildStopping added (§5.4).
- ZigZag #3: intention ids and deadlines are cells; timer heap derived (§5.6).
- ZigZag #4: internal events on `d.pending` (eleventh dimension), emptied each turn; TurnAborted
  replaced by adapter recovery turn `fail_step` (§4.2).
- ZigZag #5: program-side handler/plan ranks reuse `d.clause` (§5.7).
- ZigZag #6: V11 copy-on-store.
- ZigZag #7: skill frame cell as clone master; install replaces whole skills (§5.3).
- ZigZag minors: frame cell for user locals; "handle" renamed "peer cell"; subgoal placement stated;
  extends → one instance per transitive capacity.
- AOP #2: reconsideration of all live intentions at turn end (§6.4).
- AOP #3: recovery turn after hard failure (§4.2).
- AOP #4: belief set semantics with provenance rank; belief triggers are terms (§6.1, §8.5).
- AOP #6: `await` on belief term or event (§6.6).
- AOP #7: maintain back-off (§6.3).
- AOP minors: cancel/refuse performatives, delegator in intention, latency stated, goal clause
  scope, unhandled events counted, reselection in a later turn.
- Ergo #1: `if` without `else` is total; `c & e` is the failing form (§8.2).
- Ergo #2: contextual keywords; module paths accept keywords (§8.1).
- Ergo #3: record literals must be parenthesised in where/contract/goal-clause expressions.
- Ergo #4, Writer #2: ThrusterNav is now a native skill; examples revised.
- Ergo #5: Trigger is a term; `believes t from s`.
- Ergo #6: Schedules signatures; Event and Timer types.
- Ergo #7: Bool rejected in conditions; `not` binds looser than comparison.
- Ergo #8: V14 now says "goal-directed structure, statically typed spellings"; §8.11 lists
  comparison, concatenation, generator procedures.
- Ergo #9, Writer #3: comparison does not chain.
- Writer #5-#7, minors: reserved list fixed, broken table rows fixed, glossary rows added, "round 1"
  references removed from body, size estimate replaced with measured wc of vprolog/vicon.

Deferred (open questions): fast lane for reactions (U-A11), overridable selection (U-A12), protocol
declarations (U-A13), tests form (U-A10, now with a sketched syntax), `Agent<T>` typed peers (not
added).

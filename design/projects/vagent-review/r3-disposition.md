# Round 3 disposition (orchestrator) — document is now version 3.1

All BLOCKING and MAJOR findings accepted. Design changes:

- Vortex N1 (BLOCKING): renewal by resume is refused (WrongEpoch, mesh_actor.cpp:202-204) and resume
  changes the pid (:284). Now: (V16) agents have stable NAMES (parent name / spawn counter); peer
  cells hold names; `$peers` maps name→pid, updated from sender name+pid carried in every envelope.
  Renewal is a new owed mesh change (f) `renew(pid, capability)` in place. Migration changes pid;
  `Moved{name}` notices; effect waits fail with `migrated` (§4.5).
- Vortex N2: remote spawn via send; (a) now makes ActorSpawnAck complete the delivery; local spawn
  via spawn() creates no delivery.
- Vortex N3: `while` lowered with a choice point per iteration, cut on success (§9.1, V7).
- Vortex N4: Icon `\` dropped from vagent (§8.4, §13); §11.1 no longer claims to fix vicon's `\`.
- Vortex N5 + N6 + ZigZag #4: activation frames on cursor +d.stack hold args/locals; live
  temporaries (incl. recorded depth) spilled to the frame across calls, so recursion is safe;
  vicon's Bind not used; field writes are `#Value` on the place's value cell; adapter unlinks cursor
  +d.vars/+d.stack each turn (§4.2 step 7, §5.8). V12 reworded: no STATE cell links into the
  program; opcode output cells are scratch. Per-agent program copy priced (§4.4), shared base is
  U-A14.
- Vortex N7: fuel: setExecutionLimits sets work absolutely; VM steps don't consume work, so adapter
  passes maxInstructions as run()'s maxCycles; exhaustion is an exception the adapter catches; order
  clearChoicePoints → release(m0) → reset cursor (§4.2 step 6).
- Vortex N8: requestId is fixed-width hex (turn counter + outbox position), ordered per sender; (b)
  uses a per-sender watermark.
- Vortex N9: adapter resends via send() regardless of ack; abandons at horizon or deadline.
- Vortex N10: Initialize gating in the child's adapter; parent sends Initialize first.
- Vortex N11: owed (g) stop-and-reap; `stop a` and `terminate` actions; on Destroy semantics.
- Vortex N12: programRef names a sealed vagent artifact with its own loader/verifier
  (`program.cpp`), not the mesh pure-job validator.
- Vortex minors: wake() needs ActorExecution::message optional; if lowering fresh depth; opcode
  touchpoints listed; mintNamedDimension find-or-mint stated (V10); per-issuer budget; local
  children rely on ChildStopping (ChildDown remote only); (h) fair pumping; citations updated.
- ZigZag #1: identities are names (Agent = name, Intention/Timer = scalar id, Space = host name +
  space name), values copied (V11 rewritten).
- ZigZag #2: dispatcher merges handler ranks across sources and supertypes by priority scalar on
  each handler cell (§5.7).
- ZigZag #3: plans recorded by qualified name, not ordinal.
- ZigZag minors: `$turn` cell; `$`-prefixed reserved fields; projectProvenance never used in agent
  arenas; subgoal-chain walks and rankTail O(n) priced; clause layout (+d.grab head, +d.source
  sources); child = two peer cells; diagram legend.
- AOP #1: reconsideration isolated per condition with fuel slice; faults → ConditionFault, state
  unchanged; cannot fail the turn.
- AOP #2: succeed-when not tested while an effect is in flight; drop stages host cancel for effects.
- AOP #3: plans vs handlers failure semantics stated: plan statements are individually bounded; only
  the failing statement is undone; handlers are all-or-nothing (§6.7).
- AOP #4: maintain goal = permanent goal intention (kind=maintain) with restorer subgoal; hazard
  example drops only non-maintain intentions; KeepCharged added to the rover.
- AOP minors: remote_fault typed reply; EffectFailed event for handler effects; host bindings must
  not block; in-flight effects block checkpoint; await tests base first, matched before handlers,
  consume can't hide; delegate deadline stages cancel; outcome/refuse/stop rows; `revise`; device
  contention is the binding's contract; rover awaits the final position percept.
- Ergo #1 + Writer #1: Bool rule is syntactic (outermost operator not a comparison).
- Ergo #2: keywords accepted after `.`, in field/param declarations, record keys.
- Ergo #3: `predicate` declarations type belief terms; strings serve as atoms.
- Ergo #4: `await E as e [g]` returns the event; `await t` binds variables.
- Ergo #5: conditions are pure (no actions/assignments/effects).
- Ergo #6: `var` requires initialiser; `Opt<T>` with `none`; reads succeed or fail.
- Ergo #7: scatter/gather example now targets one worker per sub-task and replies to source.
- Ergo minors: line continuation excludes `-`/`*`; `<-` spacing warning; else-ambiguity check;
  if-without-else body type (); `to` inclusive; plan header may omit params; Behaviors capacity
  replaced by mount/unmount actions; TermArg `_` removed; resolution order; `Tick{}`. Overloaded
  `in`/`for`/`by` kept and recorded as refused rename with reason.
- Writer #2: rover capacities and skills moved to `mars:hardware`; no import cycle.
- Writer minors: change count fixed; vicon status wording; atoms; "host delivery" in glossary;
  `let x := effect()` suspension; nested effect call rejected; member-form timing; §12.4
  delegation/ask/await/tell example; lexical nonterminals note; handler effect results.

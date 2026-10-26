# Round 2 critique (vortex role): Vortex / Vlog / vicon / ArenaManifold / Spool-Mesh …

Full title: Round 2 critique (vortex role): Vortex / Vlog / vicon / ArenaManifold / Spool-Mesh
review of vagent-agent-language.md v2.0

Paths are relative to apps/common/xanadu/ unless stated. Everything below was checked against the
worktree.

## 1. Status of round-1 BLOCKING and MAJOR findings

| R1  | Topic                            | Status                                                                                                         | One line                                                                                                                                                                                                                                                                                               |
| --- | -------------------------------- | -------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| 1   | "asks the C++ core for nothing"  | PARTLY                                                                                                         | Section 11 now lists the front end, adapter and two retention fixes. It still omits the bounded-statement mechanism (F1), the host-call path for `effect` methods (F6), a time source (F5), spawn plumbing (F4), and delivery cancellation (F3). Section 11.1 "Nothing in the VM" is not demonstrated. |
| 2   | Quiescence / fire-and-forget ack | PARTLY                                                                                                         | The "completes on ActorAck" claim is withdrawn and replaced by "completes on reply". Self-sends and abandoned deliveries still never complete (F2, F3).                                                                                                                                                |
| 3   | Spaces / owner-only send         | RESOLVED in design (host relay, single-owner stated). Wire details are still wrong: size cap, `sequence` (F5). |                                                                                                                                                                                                                                                                                                        |
| 4   | Speculation, ArenaSlice, #Abort  | PARTLY                                                                                                         | Turn-bounded speculation is coherent. It silently depends on vicon bounded evaluation, which vicon itself did not build (F1).                                                                                                                                                                          |
| 5   | mark/release/promote             | RESOLVED for the per-event path. The checkpoint path is promote under another name (F7).                       |                                                                                                                                                                                                                                                                                                        |
| 6   | `ask` suspension / deadlock      | PARTLY                                                                                                         | Mandatory deadlines fix the cycle. An expired `ask` leaves an uncompletable delivery behind (F3).                                                                                                                                                                                                      |
| 7   | Grant semantics / failure model  | PARTLY                                                                                                         | Per-turn fuel re-arm and the base choice point are right. Lease expiry (`expiresAt`) is not mentioned anywhere (F8).                                                                                                                                                                                   |
| 8   | Agent identity (cell vs actor)   | PARTLY                                                                                                         | One actor per agent is settled. "Handle computed in advance" is not implementable (F4).                                                                                                                                                                                                                |
| 9   | Dual-wing / `Call`               | OPEN                                                                                                           | Section 9.2 still says "the callee reads them through the frame", which is false (F10).                                                                                                                                                                                                                |

## 2. Findings

### F1. BLOCKING (3 V6/V6a, 6.7, 8.2, 9.3, 11.1): bounded evaluation is assumed from …

Full title: F1. BLOCKING (3 V6/V6a, 6.7, 8.2, 9.3, 11.1): bounded evaluation is assumed from vicon,
but vicon's compiler documents that it does not work

- Vagent leans on "a statement is bounded: its first result is taken and its choice points are
  dropped (vicon 4.4)". This underpins V6 (an effect is safe at statement level), the "fails
  forward" story, and the claim that choice points do not survive to turn end.
- The built vicon compiler says the opposite (vicon/compiler.cpp:12-22, "Deviation note for
  iteration 2"): the flag+drain bounded evaluation cannot work, because every flag write is trailed
  and undone. `emitBounded` just calls `emitInto` (compiler.cpp:121-123), so no `discard` happens at
  a statement boundary. The same note says `\` is wrong ("`\n` currently yields all of `e`"), and
  that "doing that inline needs a new opcode or a new ChoicePoint field, both out of scope for I2".
  The std:goal driver does the cutting host-side with a second cursor (vicon/driver.cpp:5-9).
  design/vicon-implementation.md:104-122 still specifies flag+drain, so the design doc and the code
  disagree.
- Consequence for vagent:
  - Stale generator choice points from earlier statements stay live. A later failing statement then
    backtracks into them and re-runs the statements after them, including effects (`follow(route)`
    in 12.3).
  - Section 4.2 step 7 ("no choice point is live") is false unless the adapter calls
    `vm.clearChoicePoints()` (`cut(0)`, vortex_vm.cpp:242). The adapter list in 11.2 does not say
    so.
  - The `\` operator in the grammar (8.4) inherits vicon's defect.
  - Section 9.3 lowers the dispatcher as "`#Choice` ... `m = mark` ... `discard(m)` ... `#Cut`".
    There is no mark opcode (OpcodeKind, vortex_vm.hpp:27-96); `#Choice` already takes the mark
    (vortex_vm.cpp:180-195). `#Cut` with no input is `cut(0)` (vortex_vm.cpp:975-981), which
    discards every choice point including the adapter's base choice point. The next soft failure in
    that turn then returns `success=false` and kills the agent, which is exactly what 6.7 says
    cannot happen.
  - A mark/release/discard pseudo-code appears in 4.2, 6.1, 6.5 and 9.1 as if these were Vortex
    operations. In Vortex code only `#Choice` (mark and push), `#Fail` (release and jump to altOp)
    and `#Cut` (discard to a barrier) exist.
- Fix:
  - Either specify a working bounding scheme using only existing opcodes, or price a new primitive
    in 11.1.
  - A candidate that needs no new opcode: `#Cut` takes a literal barrier from `inputs[0]`
    (vortex_vm.cpp:975-979). Since every statement is bounded, the live choice-point count at a
    statement's start is the static structural nesting depth (base choice + enclosing
    `if`/`while`/`for` choice points). The compiler can emit `#Cut` with that constant. This must be
    shown to hold under resume-at-step-path, and a vortex test written before the claim "no VM
    change" stands.
  - Otherwise state that vagent owes a depth-reading primitive or a `ChoicePoint` field, and drop
    "No opcode, no ChoicePoint field" from 1 and 11.1.
  - Rewrite 9.3 in `#Choice`/`#Fail`/`#Cut` terms with explicit barriers, and add
    `clearChoicePoints()` to the adapter's per-turn steps.

### F2. BLOCKING (4.2, 6.5, 6.6, 9.1, 7.3): self-sends (`Resume`, `yield`, `adopt` …

Full title: F2. BLOCKING (4.2, 6.5, 6.6, 9.1, 7.3): self-sends (`Resume`, `yield`, `adopt`,
`achieve`, reply-to-self) cannot complete under the runtime as built

- The design sends `Resume` to `self` through `d.outbox` and `MeshActorRuntime::send` (9.1 rows
  `adopt`, `achieve`, `yield`). Trace of one self-send with requestId R:
  1. `send` records `deliveries_[R]` (mesh_actor.cpp:518-526).
  1. `receive` enqueues it, and `enqueue` stores `remembered[R] = deliveryContentId(request)`
     (mesh_actor.cpp:352).
  1. `pumpOne` replies with an `ActorDeliverValue` that has the same requestId R and target = the
     actor itself (mesh_actor.cpp:486-499).
  1. `receive` accepts it as `pendingReply` (:421-437), then calls `enqueue`. `remembered.find(R)`
     hits the stored request, the content ids differ (type, value, source/target), so `enqueue`
     returns `DuplicateMismatch` (:333-337).
  1. `receive` returns the error before `completed = true` (:441-448).
- So the delivery is never completed and the agent can never be checkpointed (:678-683). Each
  self-send also occupies a `deliveries_` slot and a `remembered` slot forever (no erase in the
  file). A non-self pair is not affected, because the sender's `remembered` does not contain R.
- Timers have a related problem. The adapter "enqueues timer messages through `receive`" (11.2).
  That needs `trustNode(node_, owner)` for the agent's own node (mesh_actor.cpp:360-364), and
  `pumpOne` then emits a reply with no matching delivery, which is rejected `NotAuthorized`
  (:429-437). It is harmless noise but unmentioned.
- Even without the collision, the design costs two turns plus a `remembered` slot per resume, and
  spends the 4096-message lifetime budget (fix 11.3 (b)) on the agent's own scheduling.
- Fix: Resume, yield, subgoal-ended and timer wake-ups are not `ActorMessage`s. The adapter holds a
  ready queue (a rank on a new `d.ready`, or an adapter-side queue rebuilt from `waiting` cells on
  resume) and drains it between mailbox messages. Quiescence for checkpoint then has to include the
  adapter's ready queue, which 4.2 step 7 must say. If self-messages are kept, 11.3 needs a runtime
  change (replies keyed by `(type,requestId)`), priced as such.

### F3. MAJOR (6.6, 7.3, 7.4, 11.3, 14): a delivery that never completes blocks …

Full title: F3. MAJOR (6.6, 7.3, 7.4, 11.3, 14): a delivery that never completes blocks checkpoint
and leaks a slot; the retention fix does not cover it

- Fix (a) reaps *completed* deliveries. A delivery completes only on `ActorDeliverValue`,
  `ActorDeliverWriteSet` or `ActorDown` for it (mesh_actor.cpp:441-448). Cases where none arrives:
  - an `ask` or delegated `achieve` whose deadline fires first (the design drops the late reply and
    counts it, 6.6);
  - a send to a pid that does not exist or is stopped: `receive` returns `UnknownProcess` (:395,
    :343) and emits no reply;
  - a send refused with `MailboxFull` (:344-349): no ack, no reply;
  - a target whose lease expired (`NotAuthorized`, :438-440).
- Each leaves `completed == false` for ever. `checkpointView` then refuses for ever, and the slot
  counts against `maxOutstandingDeliveries` for the whole runtime (`deliveries_` is runtime-wide,
  not per agent, mesh_actor.cpp:521). With 128 agents on one runtime the per-agent share is about 32
  messages.
- 14 says "every wait is bounded", but the *delivery* behind a bounded wait is not.
- The doc also promises "at least once" delivery (7.3). Retransmission is only `retry(requestId)`
  (mesh_actor.hpp ~162), which a caller must drive. The adapter's duties in 11.2 do not include
  driving retries.
- Fix: add to 11.3 a (d) "cancel or expire a delivery" (`abandon(requestId)`, called when a deadline
  fires, after which the reply is ignored and the slot freed) and say who calls `retry`. Document
  `MailboxFull` as back-pressure with a policy (the sender retries after a delay).

### F4. MAJOR (8.6, 6.7, 5.4, 9.1 row `spawn`): the spawn story is not implementable as written

- "The handle it returns is computed in advance (`ActorPid` is derived from the spawn request, the
  node, the epoch and a sequence, mesh_actor.cpp:159)." The pid is
  `{node_, contentId(request), epoch_, nextSpawn_++}`. `nextSpawn_` is private, there is no
  accessor, a refused spawn does not consume a number, and a remote spawn's pid is allocated by the
  remote runtime. `ActorSpawnAck` handling records only `acknowledged = true`
  (mesh_actor.cpp:381-392) and drops the target pid. Only a local `spawn()` returns the pid,
  synchronously.
- Therefore a handle cannot be valid during the turn that spawned. Fix: a handle cell is created
  with an unresolved placeholder; staged envelopes reference the handle *cell*; the drain spawns
  first, fills the handle's content, then sends. A failed spawn is reported by the adapter as a
  `ChildDown`/`SpawnFailed` event.
- "If the spawn is refused ... the first message to it produces `ActorDown`" is false: a send to an
  unknown pid returns an error and emits nothing (item F3).
- `ActorSpawn` as validated (identity/actor_wire.cpp:87-90) requires a non-empty `capability`, a
  bounded `programRef`, and an **empty `value`**. So spawn arguments cannot ride in the spawn;
  `Initialize{args}` (10) has to be a first `ActorSend`. The grant comes from the `Authorizer`
  verifying a signed capability (mesh_actor.cpp:131-139): `grant->programRef` must equal
  `request.programRef`, `baseRef` must be non-empty, and `maxOutputBytes` is capped at 32768. A
  running agent cannot mint a capability, so who issues the child's grant, fuel and lease is
  unspecified. The "fuel figures passed to the grant ... from system://settings" (10) cannot be
  true; settings can inform the Authorizer, nothing more.
- `spawn()` also requires `trustNode(request.source->node, identity)` for the spawner's own node
  (mesh_actor.cpp:130-134).
- Fix: rewrite 8.6 and 11.2 to include the Authorizer/capability source, the placeholder handle, the
  first-message argument delivery, and a failure event produced by the adapter.

### F5. MAJOR (7.3, 5.5, 10 `Clock`, 14): wire and runtime facts that are claimed but not true

- **Ordering.** 7.3 says "the runtime numbers them (`ActorMessage::sequence`)". The runtime never
  reads or writes `sequence` (only a comment at mesh_actor.cpp:158 contains the word). For
  `ActorSend` and `ActorDeliverValue`, `ActorMessage::isValid` *requires* `sequence == 0`
  (identity/actor_wire.cpp:83-96). No per-sender order is guaranteed by the runtime; `enqueue` is
  FIFO by arrival. Fix: carry an event ordinal in the bencoded `value` and have the receiver adapter
  reorder, or drop the ordering claim.
- **Payload size.** 5.5 says the frame limit is 64 KiB. `isValid` rejects `value.size() > 32768`
  (actor_wire.cpp:80). Events and `ask` answers are both capped at 32 KiB.
- **Clock.** 10 says `Clock.now()` "reads the wall time the runtime passed to `pumpOne`". The
  executor receives `ActorExecution{pid, grant, message, cursor, stateRoot}` and a `WorkerContext&`
  (mesh_actor.hpp:75-81, 114-116); the `wallNow` argument of `pumpOne` is not passed on. Either the
  adapter reads its own clock (then 14's "a turn is a function of its message, its wall time and its
  state" holds only if the adapter records that time in the arena), or add a field to
  `ActorExecution` and list it in 11.3.
- Add all three to 11.3 or correct the text.

### F6. MAJOR (1, 8.5, 12.2, 12.3, 11): `effect` methods have no execution path

- The whole capacity/skill model exists to call devices (`thrusters.set`, `drill_core`). 12.3 closes
  with "native host functions ... outside this document". 1 admits that `OpcodeKind` has no
  host-call opcode and that `WorkerContext` has no runtime handle. An `effect` method therefore has
  no lowering. Nothing in 11.2 or 11.3 lists a host-function registry, an opcode, or a stdlib
  routine for it.
- Vortex's existing route for native work is `VortexStdLib` routines resolved by name
  (vortex_stdlib.cpp, e.g. `resolve("std:gc/sweep")`, vortex_host.cpp:119). The doc should say
  whether `effect` calls compile to a stdlib routine call, who registers device routines, and how
  they run inside the turn without being undone by the base mark (see F9).
- Fix: add 11.4 "Host bindings for effects" with the mechanism and price, or restrict v2.0 to skills
  written in Vortex plus the explicitly owed bridge.

### F7. MAJOR (3 V8, 4.4, 4.3, 14, 15): the checkpoint path is `promote` into a Store …

Full title: F7. MAJOR (3 V8, 4.4, 4.3, 14, 15): the checkpoint path is `promote` into a Store, with
ceilings and permascroll growth that V8 denies

- 4.4 reclaims by "publish the checkpoint, then `resume` it on a fresh arena". The checkpoint is
  `publishValue(Store&, parent, arena, root, ...)` (vortex/mesh_value.hpp:88-93: "Promotes the
  reachable answer locally, then seals and signs its full history"), and `resume` rehydrates from
  authenticated history under `ValueLimits` (mesh_actor.cpp:255-262). So:
  - V8 "vagent never calls `promote` implicitly" and 13 "per-event promote flooding the spool is
    refused" are undercut: reclamation promotes the *whole live state* every time, and a busy agent
    that wants GC does it often.
  - Promotion is capped by `PromotionBudget.maxOps = 65536` (arena_manifold.hpp:232-234) and
    `ValueLimits.operations` (65536, further clamped to `grant->maxCells`, mesh_actor.cpp:259-260).
    `promote` returns nothing when exceeded (arena_manifold.hpp:815-816). So a belief base larger
    than about 65k cells can never be checkpointed, resumed, moved or reclaimed. That is a ceiling
    users cannot work around, which the house style forbids. It belongs in 15 and 17 with a proposed
    answer (a grant-level limit, or sharding beliefs across agents).
  - Constructed bytes (every string field, event text, belief constant) are spooled into the
    author's permascroll at promotion (arena_manifold.hpp:799-810 comment, step 1). The permascroll
    is append-only, so every checkpoint re-appends the live strings for ever. Price it.
  - Reachability is "along any dimension" (same comment). From the agent cell that includes the
    capacity masters through `d.clone`, and from them the module and program cells. 4.3 says
    "Compiled program: named, not copied". It would be copied (or fail the budget) unless the state
    root is arranged so program cells are not reachable. See F8.
- Fix: add a "checkpoint costs" paragraph in 4.4/15: ops ceiling, permascroll growth, reachability
  rule; amend V8 to say that checkpoint is the one implicit promotion and agents that checkpoint do
  write operations.

### F8. MAJOR (5.3, 4.3, V10, 9.2): capacity identity and program mutation across checkpoint/resume

- A skill instance links to a view appended to the *capacity master's* `d.clone` rank (5.3). The
  master is a program cell. So installing a skill mutates program cells, and the call site
  `inputs[0]` is "patched at run time" (9.2), another mutation of program cells. The checkpoint
  carries `programRef` only; on resume the program is loaded afresh, without the views or patches.
- The dispatcher test is `cloneMaster(...)` equals the wanted capacity *cell* (5.3). If state and
  program are both rehydrated, the capacity cell reachable from state (promoted copy) is not the
  freshly loaded program's capacity cell, so no skill matches after a resume. If the promotion walk
  is reachability along any dimension (F7), the whole capacity master is dragged into the checkpoint
  as well.
- Fix: identify a capacity by a name cell (content), not by `CellRef`, and keep skill-to-capacity
  links by name on state side; keep the program region immutable (patch a state-side jump table or
  use `Bind` of the callee cell instead of rewriting `inputs[0]`). Say how `resume` reloads the
  program next to the restored base.

### F9. MAJOR (6.7, V6, 8.3, 6.5): "its effects stand" contradicts the base-choice-point rollback

- 6.7: "a plan that fails after acting fails forward: its effects stand ... `PlanFailed` can
  compensate." The same section runs every intention step "under a base choice point whose
  alternative is the failure path", and 6.5 uses `release(m)` for a failed handler. A failure that
  reaches the base choice point *releases* its mark, which undoes every trailed arena write of the
  step: field assignments, `believe`/`forget`, staged outbox envelopes, even
  `samples := samples + 1`. Only external effects (device calls) stand. So the record of what was
  done is erased while the world keeps the change, the opposite of what compensation needs.
- Also, with F1 unresolved, `:=` is not irreversible in the VM: vicon emits `Bind` for both `:=` and
  `<-` and both revert on backtrack (vicon/compiler.cpp:354-363, where its own comment says so).
  8.3's "`x := e` assigns irreversibly" only holds if no live choice point precedes it, i.e. only
  above a *cut*, not under the base choice point.
- Fix: choose. (a) A failing step keeps its arena writes: then the failure path must `discard` the
  base mark (commit) before running the failure handler, and the step is not atomic. (b) A failing
  step loses them: then say so, drop "its effects stand", and have the failure handler receive an
  effect log written to a cell *after* release (the adapter holds it host-side). Whichever is
  chosen, restate V6 and 8.3 to match.

### F10. MAJOR (9.2, 5.3): the "dual-wing calling convention as built" description of …

Full title: F10. MAJOR (9.2, 5.3): the "dual-wing calling convention as built" description of `Call`
is still wrong

- 9.2: "`#Call` pushes a return frame ... Argument cells are the call opcode's `+d.grab`/`+d.step`
  operands; the callee reads them through the frame." Actual (vortex_vm.cpp:665-687): the frame cell
  holds one `Int64` (the return opcode), linked on the cursor's `+d.stack`. The callee has no access
  to the call opcode's operands; nothing binds them.
- vicon, the authority for in-turn code, does not use `#Call` at all. It emits `Bind` per parameter
  and a `Jump`, with the return continuation passed as a bound name (vicon/compiler.cpp:692-760).
- Fix: specify argument binding as vicon does (`Bind` params on the cursor scope, `Jump` to entry,
  continuation carried as a binding), or say how `#Call` obtains arguments (a convention the
  compiler must emit, with the bill). "The dual-wing convention unchanged" should go: the dual-wing
  wings are an *opcode's own* operands and outputs.

### F11. MAJOR (1 table, 6.1, V7): the belief database is labelled "built (Vlog)" but …

Full title: F11. MAJOR (1 table, 6.1, V7): the belief database is labelled "built (Vlog)" but Vlog 6
and 7 are not built

- vlog-logic-extension.md:3-7 says "§6 and §7 are not \[built\]: no clause selection, no solver."
  AGENTS.md says the same. The 1-table row for Beliefs cites "Vlog §6" and the state column says
  "built (Vlog)". `assertz`, `retract`, clause walk and copy-term as Vortex routines are owed. (The
  C++ clause store at vortex_stdlib.cpp:1370-1412 and 4166-4178 is vprolog's, not a Vortex-level
  `std:logic` API.)
- Also, `retract` in Vlog 6.2 is `link(clause, ±d.clause, 0)`, "REARRANGE TO LIMBO"
  (design/vlog-logic-extension.md:560-566), a link to nothing. The vagent text in 5.1 describes
  removal as "links its predecessor to its successor". The two disagree on rank repair; the
  logical-update-view argument depends on which one.
- Fix: mark beliefs "owed in Vortex (std:logic routines unbuilt: Vlog 6-7)". Add the per-clause term
  copy and rank walk to the `std:agent` bill. Reconcile the retract semantics.

### F12. MAJOR (4.1, 8.1, 12.1, 15, 10 settings): mailbox capacity and reply traffic are unpriced

- `ActorOptions::maxMailboxMessages` is 128 (mesh_actor.hpp:58-59). Every `ActorSend` produces a
  reply into the *sender's* mailbox (7.1 admits this, 15 says "two turns per message"). A
  coordinator that fans out N sub-tasks gets N replies plus N results, so N above about 60
  overflows: `enqueue` returns `MailboxFull` (mesh_actor.cpp:344-349) and the message is dropped
  without ack. 12.1 takes `partitions` as a parameter with no bound.
- The settings list (10) names `vagent.max_actors` but not `maxMailboxMessages`,
  `maxRememberedMessages`, `maxOutstandingDeliveries`, `maxMonitors`, or the grant
  `maxMailboxBytes`, all of which are ceilings. These are constructor options of the runtime, set by
  C++ at start-up; "read from system://settings" is fine only if the launcher does the reading.
- Fix: add the limits to 15 with their defaults, state the back-pressure policy (F3), and either
  bound fan-out in 12.1 or show windowed fan-out.

### F13. MAJOR (5.4, 6.7, 1 table `Child death`): death and expiry notification is …

Full title: F13. MAJOR (5.4, 6.7, 1 table `Child death`): death and expiry notification is
overstated; lease expiry is missing

- `expiresAt` appears nowhere in the doc. A grant's lease ends the agent: `receive` refuses
  (`NotAuthorized`, mesh_actor.cpp:438-440) and `pumpOne` sets `stopped = true` at expiry (:462-466)
  with *no* notification to anyone. Long-lived agents therefore need a renewal story:
  `checkpointView`, publish, `resume` under a fresh signed capability (mesh_actor.hpp: comment on
  `resume`), which needs quiescence (F2, F3) and a signer for the capability. This is the same gap
  as R1-7 and the disposition did not address it.
- `ActorDown` generation: (i) `pumpOne` sends an `ActorDown` only to the *sender of the failed
  message* (:486-499), and vagent's adapter never fails by design; (ii) the monitor path (`tick`,
  :640-660) sends `ActorDown` to watchers only after phi-accrual suspicion, the 60 s default lease,
  *and* externally supplied `observe()` evidence (`noSeeders`, `ledgerChecked`, `noNewerEpoch`). So
  `ChildDown` as described ("monitoring each one through `monitor`, so `ActorDown` reaches it") is a
  node-loss signal that arrives minutes later, not a child-stopped signal. The citation
  mesh_actor.cpp:425-448 is the *receive* side of a Down, not its source.
- Fix: describe `ChildDown` as node-unreachable with that latency; make the child tell its parent
  itself on `Destroy`/lease end (needs a `Destroy` hook, which does not exist: `Destroy` in 10 has
  no mechanism, the runtime just stops). Add lease and renewal to 4.1 and 15.

## 3. MINOR

- m1. Markdown breaks: 6.7 table row 1 (line 570) ends with an unterminated `` `&`/\` `` and the 13
  table's last row (line 1300) is a broken `` `&&`/\` `` row with empty cells. The `||` or `\|`
  inside backticks defeats the table. mdformat/mdl will flag or mangle them. Write `&&` and `||`
  with escaped pipes outside code or restate the row in prose.
- m2. Stale or imprecise line references: mesh_actor.cpp:441-447 is now 443-448; mesh_actor.hpp:131
  is `trustNode` and `spawn` is 132-134; 7.3's "mesh_actor.cpp:669-696" for `checkpointView` is
  right but 1's "669-696" mixes with "673-675" for the conditions (fine).
  `VortexCore::mintNamedDimension` starts at vortex_core.cpp:16 (20-26 is its body). Cite functions
  rather than ranges where possible.
- m3. 4.2 step 6: on hard failure `release(m0)` undoes the arena, but the VM's `choiceStack_` still
  holds `ChoicePoint`s whose marks are above `m0`. The adapter must `clearChoicePoints()` *before*
  releasing `m0` (it discards inner marks first) and reset VM-local state (`currentDeweyPath_`,
  cursor opcode). State the order.
- m4. 7.1/6.7 "fuel exhaustion: `TurnAborted` queued for the next turn": queued where? The arena
  write is undone by the same `release(m0)`; it has to be adapter-held. Also the failed turn's
  message is already consumed and remembered, with an empty-success reply, so it is silently lost.
  That contradicts "at least once" (7.3). Say "poison message dropped, counted".
- m5. Timers are adapter state (heap) and are not in the checkpoint; after `resume` they vanish and
  every `waiting` intention hangs. Rebuild the heap from `waiting` cells (store absolute deadlines
  as scalars) and say so.
- m6. 5.6 vs 6.3: it is unclear whether a subgoal intention also sits on the `d.intention` rank.
  `intentions()` and `drop i` ("subgoals dropped recursively") and Resume routing depend on it.
- m7. `Initialize` (10) has no delivery rule: the runtime sends nothing at spawn, so the adapter
  must enqueue it, and (F4) its args cannot ride in the spawn.
- m8. The 11.2 estimate "adapter about 800 lines" does not include the fixes above (ready queue,
  host bindings, spawn bridge, retry/expiry driver, clock, clearChoicePoints ordering). Re-estimate
  after F1-F6.
- m9. 12.1 and 12.3 use `*findall(...)`, `*xs` size and `findall` as `std:agent` functions; they
  rely on std:collections list construction from a Vlog walk. Fine once F11 is paid, but note it in
  the bill.
- m10. 5.1 reuses `d.source` with "the meaning result_slice.cpp:34 gives it". Check that this does
  not collide with `d.source` used on the same cells by Store-side projections; the cell is
  arena-only so it should be fine, but cite why.

## 4. What checks out (spot-verified this round)

- `ActorPid` identity_layout.hpp:304, `ActorMessage` :315-327, `ActorGrant`, `ActorContinuation`
  (mesh_actor.hpp:92-98), `checkpointView` conditions (mesh_actor.cpp:673-683), owner-only
  `ActorSend` (:402-405), `stopped` on executor error (:501), `maxActors` 128, `remembered` and
  `deliveries_` never erased (grep: no `erase` in the file), 4096 limits (:344, :521),
  `maxOutputBytes` cap 32768 (:135).
- `VortexVM::backtrack` pops other cursors' choice points without release (vortex_vm.cpp:198-216),
  so V3 is correct. `#Fail` with an empty stack returns `success=false` (:962-972).
- `setExecutionLimits` re-arm is public (arena_manifold.hpp:400-405) and `cellCount()` exists
  (:468). `compact()` frees no cells (:656-672) and `VortexHost::triggerGarbageCollection` is
  `gcSweep`, which I did not find to free cells either, so 4.4's "no reachability sweep is built"
  stands.
- `mark`, `release`, `discard`, `outstandingMarks`, `insertIntoRank`, `cloneMaster`, `promote`
  signatures match (arena_manifold.hpp:617, 523, 649, 835).
- `d.scan` and the other system dimensions exist; minting a dimension as a cell on `d.dims` is how
  `mintNamedDimension` does it (vortex_core.cpp:16-27).
- The honesty of the "owes C++" row in the header table is much improved; the withdrawn items in the
  appendix match what I flagged in round 1.

## 5. Net assessment

Round 2 fixed the structural misreadings of round 1. What remains is (a) a dependency on vicon
behaviour that vicon's own compiler says it could not deliver (F1), (b) a message-passing scheme
whose self-sends cannot complete (F2), and (c) a set of runtime facts (lease, spawn, clock,
sequence, size, checkpoint-as-promote) that the new "owed C++" section has not yet absorbed. None is
hard to fix on paper, but F1 and F2 change the lowering and the turn structure, so they must be
settled before the document can claim "asks the VM for nothing".

VERDICT: NOT CONVERGED

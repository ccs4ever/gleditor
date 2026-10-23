# Round 3 critique (vortex role): vagent-agent-language.md v3.0

Paths are relative to `apps/common/xanadu/` unless stated. Everything below was checked against the
worktree. "Doc" means `design/vagent-agent-language.md` v3.0.

## 1. Status of round-2 BLOCKING and MAJOR findings

| R2  | Topic                                  | Status   | One line                                                                                                                                        |
| --- | -------------------------------------- | -------- | ----------------------------------------------------------------------------------------------------------------------------------------------- |
| F1  | Bounded evaluation from vicon          | PARTLY   | `#ChoiceDepth` is a sound idea for the statement cut, but `while`, `\`, assignment and the static output cell are not covered (N3, N4, N5, N6). |
| F2  | Self-sends cannot complete             | RESOLVED | Wake-ups are no longer messages; the collision is correctly explained (§11.3 d). Signature detail left open (N13).                              |
| F3  | Incomplete deliveries block checkpoint | PARTLY   | Retry and abandon added, but acknowledged-but-incomplete deliveries and spawn deliveries are not covered (N2, N9).                              |
| F4  | Spawn story                            | PARTLY   | Placeholder cells and `Initialize` added. `Initialize` has no ordering guarantee (N10); the spawn delivery never completes (N2).                |
| F5  | Wire facts                             | RESOLVED | 32 KiB, `sequence == 0`, no ordering: all match `identity/actor_wire.cpp:76-96`. But §11.3 (b) now contradicts them (N8).                       |
| F6  | `effect` path                          | RESOLVED | Staged, asynchronous, native skills, host bindings in the adapter. Coherent.                                                                    |
| F7  | Checkpoint is `promote`                | RESOLVED | Admitted in V9, §4.4. The renewal consequence is wrong (N1).                                                                                    |
| F8  | Capacity identity                      | RESOLVED | Name matching and V12 are right. The V12 wording is too strong (N5).                                                                            |
| F9  | "effects stand" vs rollback            | RESOLVED | Staged effects make forward failure real.                                                                                                       |
| F10 | `Call` description                     | PARTLY   | Now says vicon's Bind+Jump convention. That convention is untested and not re-entrant as read (N6).                                             |
| F11 | Belief DB labelled built               | RESOLVED | Header and §1 list Vlog §6 as a prerequisite. Confirmed: `design/vlog-logic-extension.md:6` says §6 and §7 are not built.                       |
| F12 | Mailbox capacity                       | RESOLVED | Settings table and §12.1 note.                                                                                                                  |
| F13 | Death and lease notification           | PARTLY   | `ChildStopping` and latency added. The renewal path itself is refused by the runtime (N1) and `Destroy` has no mechanism (N11).                 |

## 2. Findings

### N1. BLOCKING (4.5, 4.4, 5.4, 11.3): lease renewal by checkpoint and `resume` is …

Full title: N1. BLOCKING (4.5, 4.4, 5.4, 11.3): lease renewal by checkpoint and `resume` is refused
on the same runtime, and it changes the agent's `ActorPid`

- §4.5 says "the renewal path the runtime offers is checkpoint, publish, and `resume` under a fresh
  capability". `MeshActorRuntime::resume` returns `WrongEpoch` when
  `node_ == continuation->pid.node && epoch_ <= continuation->pid.epoch` (mesh_actor.cpp:218-220).
  An in-place renewal on the node that holds the live actor has the same node and epoch, so it is
  always refused. `resume` is a restart or migration primitive (the header says "Re-admit a signed
  message-boundary checkpoint", mesh_actor.hpp:136).
- If it were allowed, the resumed actor gets a new pid:
  `actor->pid = ActorPid{node_, content, epoch_, nextSpawn_}` where `content` is the content id of
  the resume request (mesh_actor.cpp:284). The old actor stays in `actors_` (nothing erases,
  mesh_actor.cpp:110-116 are the only lookups). Every peer cell in the world that names the agent
  (`d.member`, `d.holon`, belief sources, a delegator in a waiting intention, a child's `parent`)
  holds the old pid. A message to it either reaches the old, stale actor or is refused with
  `UnknownProcess`. In-flight conversations (`achieve ... by a` waiting for an `outcome`) are lost
  for the same reason, though the checkpoint conditions say nothing prevents them.
- Consequence: §4.5, the 80% renewal in §10, and the claim in §1 that "Checkpoint and migration" is
  "built" for the use vagent makes of it, do not hold. Lease expiry silently ends an agent
  (mesh_actor.cpp:463-466) and the only workaround today is a very long `expiresAt` from the issuer.
- Fix, one of:
  1. Add §11.3 (f): `resume` with an `inPlace` option that retires the old actor (erase, see N11)
     and carries over the pid's identity-bearing fields, or an explicit `rebind(old, new)` that
     redirects deliveries; and state that migration to another node changes the pid.
  1. Or give agents a stable name above `ActorPid`: a name cell plus a directory kept by the
     launcher, with peer cells holding the name; a `Moved{name, pid}` notice on every resume. This
     costs a lookup per send and fits "solve with cells".
  1. Whichever is chosen, withdraw "renewal is built" from §4.5 and state which of the two the
     adapter uses.

### N2. MAJOR (4.2, 8.6, 11.3 a, 5.4): a spawn sent through the runtime never completes …

Full title: N2. MAJOR (4.2, 8.6, 11.3 a, 5.4): a spawn sent through the runtime never completes, so
a remote spawner can never be checkpointed

- `MeshActorRuntime::send` records every message in `deliveries_`, including `ActorSpawn`, whose
  `source` is required to be a pid (actor_wire.cpp:87-90) and which §5.4 says is the parent. The
  `ActorSpawnAck` branch only sets `acknowledged` (mesh_actor.cpp:391); `completed` is set in one
  place only, the reply branch at :448. `checkpointView` refuses while any delivery with
  `message.source == pid` is not `completed` (mesh_actor.cpp:678-683).
- So a holon that spawns a child on another node is never quiescent, and its delivery slot is never
  reaped (a: "reap completed deliveries" does not see it). A same-node spawn through the public
  `spawn()` creates no delivery, but the doc never says which path the adapter takes, and V1/§13
  promise holons "distributed".
- Fix: say that the adapter spawns locally through `spawn()` and remotely through `send`, and add to
  §11.3 (a) that an `ActorSpawnAck` marks the delivery completed (one line at mesh_actor.cpp:391).
  Add a test: spawn remote, ack, `checkpointView` succeeds.

### N3. MAJOR (9.1, 8.2, 8.9): `while` has no lowering, and vicon's `while` would undo …

Full title: N3. MAJOR (9.1, 8.2, 8.9): `while` has no lowering, and vicon's `while` would undo the
loop's own writes at exit

- `while` is in the grammar (8.9) and in the quiescence story (6.6), but has no row in 9.1. The
  table says vagent reuses "vicon's lowering" for `&`, `|`, `\`, `to` and `<-`; `while` is not
  named.
- vicon's `emitWhile` (vicon/compiler.cpp:636-656) pushes **one** `#Choice` (alt = DONE) at loop
  entry and `Jump`s back to the condition, not to the `#Choice`. The body therefore runs for every
  iteration under that one mark. When the condition finally fails, `#Fail` pops the loop's choice
  point and `release()` undoes every write made since loop entry (arena_manifold.hpp:636-639: "undo
  everything since m"). In vicon this is unobservable, because the driver collects `write` output
  host-side (tests/xuzz/vicon_driver_test.cpp:48-54). In vagent it would erase every `believe`,
  field write and outbox entry made by the loop, exactly the failure V7 exists to prevent.
- Fix: add the `while` row. A lowering that works with the built opcodes is a per-iteration choice
  point:
  `LOOP: d := #ChoiceDepth; #Choice alt=DONE; c; #Cut d; body (a statement); #Jump LOOP; DONE: ()`.
  On failure of `c` only that iteration's partial writes are released. Add a Vortex test that a
  loop's final-iteration and early-iteration writes both survive loop exit.

### N4. MAJOR (11.1, V13, 8.4, 9.1): `#ChoiceDepth` does not remove vicon's deviation for `\`

- §11.1 says the opcode "removes vicon's recorded deviation, which names 'a new opcode or a new
  `ChoicePoint` field' as the fix for bounded evaluation and `\`". It fixes bounded evaluation to
  one result (cut on first produce). It cannot fix `e \ n` for n > 1: the counter must survive
  backtracking into `e`, and the arena trail restores any cell older than `e`'s mark when control
  resumes `e` (arena_manifold.hpp:636-639; compiler.cpp:12-19 states this for flags). A counter is
  the same problem as a flag.
- The doc keeps `\` in the grammar (8.4 level 3, 8.9 `Limit`) and in 9.1 (row 2, "vicon's lowering,
  beyond: none"), so a reader concludes `\ 3` works. The vicon test passes only because the driver
  cuts host-side (tests/xuzz/vicon_driver_test.cpp:32-38 against compiler.cpp:12-22).
- Fix: either restrict `\` to `e \ 1` (lowered as bounded subexpression with `#ChoiceDepth` at its
  entry and `#Cut` on its first result) and say `\ n` is refused at compile time, or price a counter
  primitive in §11.1. Drop "and `\`" from §11.1.

### N5. MAJOR (V12, 4.4, 9.1, 9.3, 11.1): compiled code writes program cells at run …

Full title: N5. MAJOR (V12, 4.4, 9.1, 9.3, 11.1): compiled code writes program cells at run time,
and `#ChoiceDepth` stores `d` in one of them

- V12 and §5.7 say "the program region is never written at run time". In Vortex every opcode writes
  its outputs into fixed output cells linked at compile time (vortex_vm.cpp:1031-1041,
  `core_.value(outCells[i], ...)`; for pipelines :301-316). `d := #ChoiceDepth` (9.1 row 1, 9.3
  `d0`, `h`), every `Add`, every `Resolve` result and vicon's procedure result cells
  (`procResultCells_`, compiler.cpp:733) are such cells. They are written on every execution.
- Two consequences the doc does not state:
  1. A method that calls itself (or two methods that call each other) clobbers `d` and the result
     cell of the outer activation. The statement's later `#Cut d` then cuts to the wrong depth,
     either dropping a live choice point or keeping stale ones. No rule in vagent forbids recursion.
  1. The program must exist as a per-arena copy (or a copy-on-write base `Manifold`, which shadows
     on write). That is O(program cells) per agent against `maxActors` of 128, unpriced.
- Fix: reword V12 to "no state cell links into the program", which is what the promotion argument
  needs, and say run-time temporaries live in program-side cells that are scratch. Then either make
  recursion a compile-time error (a call graph check in `checker`), or hold `d` and call results in
  frame cells created per activation. Price the per-agent program copy in §15, or specify the shared
  base-manifold arrangement and its COW shadowing cost.

### N6. MAJOR (5.6, 9.1, 9.2, 8.3, 1 status table row 71): assignment and calls are …

Full title: N6. MAJOR (5.6, 9.1, 9.2, 8.3, 1 status table row 71): assignment and calls are lowered
with vicon's `Bind`, which does not write state and is not re-entrant

- 9.1 row 2 sends `<-` to "vicon's lowering"; row `capacity call` uses "Bind per argument, Bind of
  the return continuation, Jump"; §1 cites `vicon/compiler.cpp:358-361` as the reversible
  assignment. That code emits a `Bind` that "shadows the name on the cursor's +d.vars rank"
  (compiler.cpp:352-361). `Bind` mints a name cell and a value cell and prepends the name to the
  **cursor's** scope rank (vortex_vm.cpp:705-728); `Resolve` is a linear walk of that rank
  (:731-745).
- For vagent this means:
  1. `targets_seen := targets_seen + 1` (a field on the agent cell's `d.vars`/`d.values`) and a
     frame variable (5.6) cannot be written by `Bind`. They need `Value` writes to the place's
     `d.values` cell. That write is trailed by the arena and gives the reversible semantics; the doc
     should say that is the mechanism and name `Value` in 9.1, with a row for `:=`/`<-`.
  1. Every `Bind` allocates two cells that nothing frees except `release`; a successful turn only
     discards. Cursor scope therefore grows on every call and every turn, `Resolve` slows linearly,
     and the cells are unreachable garbage that §4.4 does not list. The adapter's turn steps need
     "unlink the cursor's `+d.vars` and `+d.stack` ranks" (or a `release` to a mark taken above
     them).
  1. As read, nested calls return wrongly: the return continuation is bound by name `__vicon_ret`
     (compiler.cpp:737-740) and never unbound, so after an inner call returns, an outer return
     resolves the inner continuation (the most recent binding). Arguments leak the same way, and the
     result cell per procedure is static. I could not find a vicon test with a procedure
     (tests/xuzz/vicon_driver_test.cpp has six tests: sequence, alternation, limitation, every,
     while, until). This is a reading, not a run; say it is untested and write the test first.
- Fix: replace the "vicon's call convention" sentence in 9.1/9.2 with a convention vagent owns (an
  activation frame cell on the intention or turn, popped on return), or write the vicon recursion
  test and fix vicon before relying on it. Mark "Reversible assignment `<-`" in §1 as owed lowering,
  not built, since the built part is only the arena trail.

### N7. MAJOR (4.2 step 1 and 3, 4.2 recovery, 11.2): fuel is not charged by the VM, and …

Full title: N7. MAJOR (4.2 step 1 and 3, 4.2 recovery, 11.2): fuel is not charged by the VM, and
exhaustion is a C++ exception, not a `#Fail`

- Step 1 re-arms `setExecutionLimits(cellCount() + maxCells, maxInstructions, maxOutputBytes)` and
  the doc treats `maxInstructions` as the per-turn instruction budget. `consumeWork()` is called
  from `ArenaManifold::mintSlot` and `intern` (zigzag/arena_manifold.cpp:1155, 1168) and from the
  mesh, VPL and vlace evaluators. `VortexVM::step` and `run` never call it (no hit in vortex_vm.cpp;
  the existing mesh executor charges per step itself at mesh_evaluator.cpp:697). A non-allocating
  loop (`Jump` to itself, a long `while` over scalars) is bounded only by `run`'s own `maxCycles`
  default of 100,000 (vortex_vm.hpp:153), which the adapter would have to set per turn.
- Exhaustion throws `std::runtime_error` (arena_manifold.hpp:410-417; arena_manifold.cpp:1170-1174).
  Step 3 says the base choice point's alternative is "the adapter's failure routine", and step 7
  says "backtrack to the base choice point" on hard failure. A thrown exception does not jump
  anywhere. The adapter must catch it, then `release` its base mark after discarding inner ones, and
  clear `choiceStack_` and VM-local state (`currentDeweyPath_`, cursor opcode) itself; R2 m3 is
  still open. Also the limits "remain active across choice-point rollback: failed work costs fuel"
  (arena_manifold.hpp:399), so the recovery turn must re-arm before it runs.
- Distinguish in §4.2: a soft `#Fail` with no other choice point reaches the base alternative (a
  Vortex routine); fuel, cell, byte and depth exhaustion are exceptions the adapter catches. State
  that the adapter calls `consumeWork()` per `step` or sets `maxCycles` from `maxInstructions`.

### N8. MAJOR (11.3 a, b, 5.5): the watermark in (b) needs an ordered request id, and …

Full title: N8. MAJOR (11.3 a, b, 5.5): the watermark in (b) needs an ordered request id, and §5.5
says the id is a hash

- (b) proposes "keep ids above a per-sender watermark; checkpoints carry the watermark" and (a) "a
  window of recent ids". §5.5 defines
  `requestId = sha256(emitter pid ‖ turn number ‖ position in the outbox)`, and `sequence` must be
  zero on `ActorSend` (actor_wire.cpp:94-96; `requestId` is bounded text of 128, actor_wire.cpp:77).
  A hash has no order, and the runtime cannot read the application ordinal inside `value`. A
  watermark or window cannot be computed from the wire as specified.
- Fix, pick one: (i) make `requestId` structured, `<emitter-epoch>:<turn>:<pos>` in fixed-width hex,
  and have the runtime parse and compare it; (ii) allow `sequence != 0` on `ActorSend` and key the
  watermark on `(source pid, sequence)`, a wire change owed in §11.3 and fuzzed; (iii) time-bounded
  retention: erase a `remembered` entry once its reply is acknowledged and older than the sender's
  retry horizon (`send_retries x send_retry_interval` plus slack), which needs no wire change but
  makes the duplicate-after-horizon case refuse rather than re-run. State which, and say what the
  runtime does with a duplicate after erasure.

### N9. MAJOR (7.3): upkeep covers unacknowledged sends only …

Full title: N9. MAJOR (7.3): upkeep covers unacknowledged sends only; acknowledged-but-incomplete
deliveries still block checkpoint for ever

- §7.3 drives `retry` "for unacknowledged sends". `retry` returns success without sending when
  `acknowledged` is true (mesh_actor.cpp:538), so for an acknowledged delivery the loop does nothing
  and never reaches abandonment. An acknowledged delivery whose reply never arrives remains
  incomplete: the receiver's lease ended while the message sat in its mailbox (`pumpOne` stops the
  actor without popping, mesh_actor.cpp:463-466), the receiver's reply was itself refused with
  `MailboxFull` at the sender, or an `ask` deadline elapsed first. `ActorDown` for a failed executor
  is sent only to the requester of the failing message (mesh_actor.cpp:480-499).
- Fix: abandon every delivery still incomplete at `max(ask deadline, retries x interval)`, whichever
  the statement waits on, acknowledged or not; and say that for acknowledged ones the retry is a
  re-send of the **request** (the receiver re-emits its stored reply on a duplicate, mesh_actor.cpp:
  336-341), which the runtime's `retry` does not do after an ack, so either add a
  `resend(requestId)` or have the adapter bypass `retry` and call `send` again (allowed, same
  message, mesh_actor.cpp: 514-520).

### N10. MAJOR (8.5, 8.6, 5.4, 7.3): `Initialize` is the first message, but delivery is unordered

- 8.5: "Agent parameters are fields, set from the `Initialize` message before `on Initialize` runs".
  4.1: spawn carries no arguments, so `Initialize` is a separate `ActorSend`. 7.3: "There is no
  ordering guarantee". Across nodes (or after one `MailboxFull` retry) a user event can arrive
  before `Initialize`, and a handler would then read unset parameter fields. The same-node deque
  keeps FIFO (mesh_actor.cpp:350), but §8.6 stages messages to a fresh child in the same turn as the
  spawn.
- Fix: the adapter gates the child: it dispatches nothing but `Initialize` until it has been
  handled, buffering other messages in the adapter (count against `max_mailbox`), and the parent's
  adapter sends `Initialize` before any message staged to that peer. Say so in §4.1 and §8.5.

### N11. MAJOR (4.5, 10 events, 5.4, 12.1, 11.3, V1): there is no way to stop an agent …

Full title: N11. MAJOR (4.5, 10 events, 5.4, 12.1, 11.3, V1): there is no way to stop an agent, and
a stopped or expired actor keeps its slot for ever

- `Destroy` appears in §4.5 and in the §10 events list. The runtime has no stop, destroy or reap
  operation: `actors_` is never erased (mesh_actor.cpp:148,232 check
  `actors_.size() >= options_.maxActors`; the only inserts are :177 and :298; no `erase` in the
  file), and an actor is `stopped` only by an executor error (:501) or lease expiry (:464).
  `spawns_` is likewise append only. So `maxActors` (128) is a lifetime count of spawns and resumes,
  not a population bound.
- §12.1 shows the cost: every `SearchCoordinator.Initialize` spawns `partitions` workers that can
  never end, so a node serves about 128 / partitions coordinators in total. This is precisely the
  "ceiling users work around" the owner rejects, and §11.3 (a), (b), (e) already treat the other
  lifetime ceilings that way.
- Fix: add §11.3 (f): `stop(pid)` that marks the actor stopped, emits `ActorDown` to its monitors,
  and erases it (and its `spawns_` entry) once its deliveries drain; give the language a `terminate`
  action or define `Destroy` precisely (who raises it, whether it runs `on Destroy` before the stop,
  §8.6 row). Without it, "no ceilings" and §14 "agents never leak" cannot be said.

### N12. MAJOR (4.1, 4.5, 11.2, 11.3, 1): `programRef` is never defined, and the mesh's …

Full title: N12. MAJOR (4.1, 4.5, 11.2, 11.3, 1): `programRef` is never defined, and the mesh's
program path rejects every opcode vagent needs

- §4.2, V12 and §11.2 say the program is "loaded from `programRef`", and the Authorizer checks
  `programRef` equality (mesh_actor.cpp:131-139). The only executor in the tree for mesh programs
  decodes a sealed artifact: `validateProgram` rejects any instruction for which `pureOpcode` is
  false (mesh_evaluator.cpp:184-231, 267-270) and the decoder rejects an opcode above
  `OpcodeKind::Cut` (mesh_evaluator.cpp:470). A new `#ChoiceDepth` appended after `Cut` is "Unknown
  Vortex artifact opcode" there, and it is not pure. vagent therefore needs its own program format,
  loader and verifier on every node that hosts an agent, because remote spawn runs bytes received
  from another node (V1, §13 "could be distributed").
- The doc names no format, no allowlist, no seal, and does not say whether `Link`/`New`/`Break` and
  `Bind` are permitted in code from a remote spawner. §11.2 lists `agent_executor` and
  `host_bindings` but no `program_loader`.
- Fix: add a short subsection "What `programRef` names": the artifact is the compiled opcode graph
  sealed by content hash, with an opcode allowlist (all of `std:agent`'s opcodes including
  `#ChoiceDepth`, none that reaches the host), verified before spawn; and list the loader and
  verifier in §11.2 with a fuzz target.

### N13. MINOR (11.3 d): `wake(pid)` needs an `Executor` signature change

- `Executor` takes `const ActorExecution &` whose `message` is a non-optional `ActorMessage`
  (mesh_actor.hpp:77-84, 115-116). A wake has no message. State that `ActorExecution::message`
  becomes `std::optional<ActorMessage>` (touching the existing mesh executor and its tests), that
  `wake` sets `running`, applies the lease check of :463-466, stops the actor on error as :501 does,
  and discards the result string. "About forty lines" may hold; the signature change is the part to
  price.

### N14. MINOR (9.1, 9.3, 11.1): small lowering gaps

- `if c then a [else b]` (9.1): the `#Cut d` must use a depth read at the `if`'s own entry, not the
  enclosing statement's, or `(1 to 3) & if c then a` would cut the `to` generator. The row says
  `#ChoiceDepth` is needed but shows `d` unqualified. Add `Jump END` after `a` and say `d` is fresh.
- 9.3 dispatcher: "`Choice alt=NEXT` ... `Cut h` ... `Jump NEXT_AFTER`" uses `h` which is a single
  static cell (N5); fine while the dispatcher is non-recursive, which it is, but say so.
- 11.1 "about ten lines in `vortex_vm.{hpp,cpp}`": the other touch points are `pureOpcode`
  (mesh_evaluator.cpp:184-231, a full switch), the artifact range check (:470), the name tables in
  `vql/compiler.cpp:121` and `vpl/compiler.cpp:321` (cosmetic, `default: "UNKNOWN"`), and an
  `exportSymbol` for `std:logic`. List them.
- `mintNamedDimension` is find-or-mint by name (vortex_core.cpp:16-19), which is what makes
  `std:agent` genesis safe after `resume`; say so in V10, since a duplicate `d.intention` would hide
  the restored state.

### N15. MINOR (4.1): the "no larger than the parent's grant" rule does not bound a tree

- A child's figures are each at most the parent's, so N children may total N times the parent. The
  only global bound is `maxActors`, which never decreases (N11). State a per-issuer budget the
  adapter debits (cells, instructions, actor count) so a holon cannot multiply its grant.

### N16. MINOR (5.4, §1 row "Child loss"): the death evidence for a local child has no source

- `ActorDown` reaches a watcher only when `tick` sees phi >= 8, the lease has passed, and
  `observe()` supplied `noSeeders`, `ledgerChecked` and `noNewerEpoch` evidence
  (mesh_actor.cpp:601-631). A quiet same-node child that has stopped produces no heartbeat and no
  evidence, so `ChildDown` never fires unless the adapter fabricates evidence, which the header of
  `ActorDiscoveryEvidence` forbids ("Missing evidence is never interpreted as absence"). State that
  local children rely on `ChildStopping` (N11 `stop`) and that `ChildDown` is for remote children
  only.

### N17. MINOR (14, 15): inter-agent fairness

- `pumpOne` serves the first non-empty actor in `std::map` key order and returns
  (mesh_actor.cpp:461-490). Two chatty agents with the lowest keys starve the rest. §14 says "an
  agent never blocks" and "reaction latency is not bounded by the language"; add "and across agents
  the runtime is not fair" or change `pumpOne` to round-robin from the last served key.

### N18. MINOR (stale or imprecise citations)

- `mesh_actor.cpp:640-660` (§1, §5.4) is `liveness`/`redispatch`; the heartbeat, evidence and
  `ActorDown` logic is `tick`, 601-631, and `observe` 592-600.
- `mesh_actor.cpp:486-499` (§7.1) is now 480-499 and covers the reply; fine to widen.
- `mesh_actor.hpp:132-134` is `spawn`, 132-134, correct; `ActorOptions` 57-65 correct; the executor
  comment 107-109 correct. `identity_layout.hpp:304` and `:315-327` correct.
- `vortex_vm.cpp:944-985` and `974-981` (Choice/Fail/Cut/barrier) correct; `198-215` is 198-216.
- `arena_manifold.hpp:638-646` is now 636-647; 656-672 correct; 617 correct; 522-523 correct.
- Prefer function names over ranges, as R2 m2 asked.

## 3. What checks out (spot-verified this round)

- `ActorGrant`, `ActorOptions` figures, `checkpointView` conditions (mesh_actor.cpp:669-696),
  owner-only `ActorSend` (:402-405), `stopped` on executor error (:501), the reply being the
  executor's string capped at `maxOutputBytes` (:480-499), `deliveries_` and `remembered` with no
  erase, and the `ValueLimits` clamp at :258-259.
- `VortexVM::backtrack` pops other cursors' choice points without release (vortex_vm.cpp:198-216);
  `cut(n)` discards marks down to `n` and keeps writes (:219-239); `clearChoicePoints() == cut(0)`
  (:242); `choiceDepth()` exists (:240-ish, hpp:128); `#Cut` reads its barrier from `inputs[0]`
  (:974-981). The `#ChoiceDepth` + `#Cut d` scheme is therefore implementable and the right shape
  for a bounded statement once N3, N5 and N6 are handled.
- `VortexCore::mintNamedDimension` is idempotent (vortex_core.cpp:16-19). `setStateRoot` exists
  (mesh_actor.hpp). `setExecutionLimits` is public and re-armable (arena_manifold.hpp:400).
- Line counts quoted in §11.2 match: vprolog 1,632 plus vicon 3,176 is 4,808 by `wc -l`.
- Vlog §6/§7 are unbuilt (`design/vlog-logic-extension.md:6`), as the header says.

## 4. Net assessment

Round 3 fixed the structural round-2 defects: wake-ups, staged effects, the opcode bill, and the
wire facts. What remains is at the boundary the doc now claims is small. (1) The renewal and
identity story rests on `resume` doing something it refuses to do (N1). (2) The Vortex-level
lowering reuses vicon constructs (`Bind`, `while`, `\`, the call convention) that are either not
state writes, not re-entrant, or would release the loop's own writes (N3-N6). (3) The runtime's
lifetime ceilings are larger than listed (`actors_`, spawn deliveries, delivery upkeep, request-id
watermark, N2, N8, N9, N11). Each fix is local and can be done on paper: N1 and N11 add one or two
mesh changes, N3-N6 replace four table rows and a paragraph, and N7 adds three sentences to §4.2.
None needs a redesign, but N1 is BLOCKING because §4.5 and §1 assert as built a path the runtime
refuses.

VERDICT: NOT CONVERGED

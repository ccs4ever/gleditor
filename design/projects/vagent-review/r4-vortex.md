# Round 4 critique (vortex role): vagent-agent-language.md v3.1

Paths are relative to `apps/common/xanadu/` unless stated. Everything below was checked against the
worktree. "Doc" means `design/vagent-agent-language.md` v3.1.

## 1. Status of round-3 BLOCKING and MAJOR findings

| R3  | Topic                                    | Status   | One line                                                                                                                      |
| --- | ---------------------------------------- | -------- | ----------------------------------------------------------------------------------------------------------------------------- |
| N1  | Renewal by `resume` refused, pid changes | RESOLVED | V16 names, `$peers`, owed `renew` (f). `WrongEpoch` at mesh_actor.cpp:203 and the new pid at :283-284 are cited correctly.    |
| N2  | Spawn delivery never completes           | RESOLVED | (a) makes `ActorSpawnAck` complete it; local `spawn()` creates no delivery (mesh_actor.cpp:380-391 only sets `acknowledged`). |
| N3  | `while` lowering                         | RESOLVED | Per-iteration choice point cut on success is executable (`cut()`, vortex_vm.cpp:221-240). Depends on F4 below for failure.    |
| N4  | `\` and `#ChoiceDepth`                   | RESOLVED | `\` dropped; §8.11/§13 agree.                                                                                                 |
| N5  | Program cells written at run time        | PARTLY   | V12 reworded correctly; frames on `+d.stack` are right but the existing `#Call`/`#Return` are never mentioned (F9).           |
| N6  | `Bind` not re-entrant                    | PARTLY   | Frames replace `Bind`; method entry points are not in the program symbol table (F12).                                         |
| N7  | Fuel                                     | PARTLY   | Cycle fuel via `run`'s `maxCycles` is right; the byte fuel in step 1 is wrong for a long-lived arena (F5).                    |
| N8  | Ordered request id and watermark         | OPEN     | The id as specified collides across senders (F1) and the watermark rule drops live messages (F2).                             |
| N9  | Resend of acknowledged deliveries        | OPEN     | `send` returns early on an acknowledged delivery (mesh_actor.cpp:519), so the stated mechanism does not exist (F3).           |
| N10 | `Initialize` ordering                    | PARTLY   | Gating is specified but cannot answer an `ask` and holds messages outside the checkpoint (F7).                                |
| N11 | Stop and reap                            | PARTLY   | (g) erases actors only; `spawns_` and `monitors_` keep growing (F6).                                                          |
| N12 | `programRef`                             | RESOLVED | `program.cpp` with an allowlist; honest about single-owner scope.                                                             |

## 2. Findings

### F1. BLOCKING (5.5, 11.3 b, 7.3): the request id is unique per sender, but the …

Full title: F1. BLOCKING (5.5, 11.3 b, 7.3): the request id is unique per sender, but the runtime
keys on the id alone

- §5.5 defines `requestId` as the sender's turn counter (16 hex digits) then the outbox position (4
  digits). Nothing identifies the sender.
- The runtime's tables are keyed by `requestId` alone: `deliveries_` is one map per runtime (`send`,
  mesh_actor.cpp:510-526, `find(message.requestId)`), and each receiving actor's `remembered` is
  keyed by `message.requestId` (`enqueue`, :332-349). A reply carries the original id (:487) and
  lands in the *sender's* `remembered` (:446).
- Consequence: two agents on one runtime whose turn counters coincide (every pair, constantly, since
  each starts at 1) both produce id `00000000000000010000`. The second `send` finds a delivery with
  a different message and returns `DuplicateMismatch` (:516-518). A receiver hearing from two
  senders with equal ids refuses the second the same way (:332-337). The §12.1 scatter/gather fails
  at its first fan-out.
- Fix: put a sender token in the id: `<sender-name hash, 16 hex>:<turn, 16 hex>:<pos, 4 hex>`. Use a
  hash of the agent name, not the name, because `boundedText(requestId, 128)`
  (identity/actor_wire.cpp:8-12, 77) bounds the id and names grow one component per tree level. The
  watermark of (b) then keys on the hash prefix. The same applies to spawn ids (`spawns_` key is
  `identity:requestId`, :140; all agents share the owner identity).

### F2. MAJOR (11.3 b, 5.5, 7.3): a per-sender watermark drops fresh messages, because …

Full title: F2. MAJOR (11.3 b, 5.5, 7.3): a per-sender watermark drops fresh messages, because
delivery is unordered

- (b) keeps ids above a per-sender watermark and drops "a duplicate at or below it". §7.3 says there
  is no ordering guarantee and the adapter resends. So id 5 is refused `MailboxFull` (:344-349), id
  6 is accepted and advances the watermark, and the resend of 5 is "at or below" and is dropped. The
  message is lost silently; the sender abandons it after `send_retries` and raises `DeliveryFailed`
  for a message the network never lost.
- A contiguous-prefix watermark is not computable from the §5.5 ids: the turn counter advances on
  turns that send nothing and abandoned sends leave permanent holes.
- Fix: the sender carries a floor in every envelope: "every id of mine below F is complete or
  abandoned" (F is the smallest id its adapter still resends). The receiver forgets ids below the
  floor and treats an id below it as a stale duplicate; ids above it are never windowed out. This is
  SACK-style, needs no ordering, and the adapter already knows its own incomplete set. Say what
  `checkpointView`'s `completed` list holds after trimming.

### F3. MAJOR (7.3, 11.3 a): `send` cannot resend an acknowledged delivery

- §7.3 resends "by calling `send` again with the same message (allowed, :514-520), whether or not it
  was acknowledged, because `retry` does nothing after an acknowledgement (:538)". But `send` has
  the same early return: `if (prior->second.acknowledged) return {};` (:519). The acknowledged but
  incomplete case (receiver enqueued, reply lost or refused `MailboxFull` at the sender, :330-349)
  is exactly the one R3 N9 raised, and neither `send` nor `retry` can emit for it.
- The receiver would re-emit its stored reply if it saw the original again (`enqueue`, :337-341), so
  the mechanism is sound; only the emission is missing.
- Fix: add to (a) `redeliver(requestId)` that emits regardless of `acknowledged`, or say the adapter
  re-emits through its own `Sender` (`setSender`, mesh_actor.hpp:121) from the envelope it holds.
  Correct the citation.

### F4. MAJOR (9.1 "a statement", 6.7, 12.3): a failing statement undoes the turn, not the statement

- §6.7 and §12.3 promise that when a plan statement fails "only that statement's own writes and
  staged entries are undone", so `emit SurveyFailed{...}` followed by `fail` leaves the emit
  standing.
- The statement lowering is `d := #ChoiceDepth; expression; #Cut d`: no `#Choice`. `#Fail`
  backtracks to the top choice point (`backtrack`, vortex_vm.cpp:198-216). In a plan run from a wake
  turn the only live one is the base choice point of §4.2 step 3, so `fail` releases its mark
  (:204): every write and staged entry of the *turn* since step 3 goes, including the `emit`.
  `SurveyFailed` is never sent.
- Fix: a statement is `d := #ChoiceDepth; #Choice alt=SFAIL; expression; #Cut d; ...`, with `d` read
  before the `#Choice` so the cut drops it. `SFAIL` is context dependent and the doc must say so: in
  a plan body it is a `#Jump` to the plan-failure code (the statement's own release has already
  undone only its writes, and the code must not `#Fail` again); in a handler or method body it is
  `#Fail`, so the failure propagates to the handler's or the caller's choice point. Price: one mark
  and one choice point per statement, which §15 should list.

### F5. MAJOR (4.2 step 1, 4.4): the byte limit is absolute, so a long-lived agent dies …

Full title: F5. MAJOR (4.2 step 1, 4.4): the byte limit is absolute, so a long-lived agent dies
after 32 KiB of strings

- Step 1 re-arms `setExecutionLimits(cellCount() + maxCells, maxInstructions, maxOutputBytes)`. The
  cell limit is relative (the sum), but `intern` compares the third argument to the arena's total
  scratch: `scratch_.size() > *scratchLimit_ || text.size() > *scratchLimit_ - scratch_.size()`
  (zigzag/arena_manifold.cpp:1155-1160). `maxOutputBytes` is capped at 32,768
  (mesh_actor.cpp:136-139). Every string cell, event type name, field name and string value is
  interned there, and only `release` shrinks `scratch_`.
- Consequence: after about 32 KiB of accumulated strings (a few hundred events) every turn throws
  "Vortex evaluation byte budget exhausted", i.e. a hard failure, for ever, until a checkpoint and
  resume that §4.4 says needs quiescence.
- Fix: take `m0` first and pass `m0.scratchSize + maxOutputBytes` (and `m0.cellCount + maxCells`);
  `Mark` already carries both (arena_manifold.cpp:1571-1580), so no `ArenaManifold` change. Reorder
  steps 1 and 3. Add scratch bytes to the list of garbage that only checkpoint and resume reclaims
  (§4.4); they are unbounded between checkpoints even with a per-turn cap.

### F6. MAJOR (11.3 g, 14, 4.1): "stop and reap" leaves `spawns_` and `monitors_` growing for ever

- Neither table has an `erase` (grep of mesh_actor.cpp: `spawns_` at :142, 178, 224, 299;
  `monitors_` at :555-561). `monitor` refuses at `maxMonitors` (4096, :557-558), and every holon
  that monitors a child adds an entry that survives the child.
- So a holon that spawns and stops a courier per order hits `ResourceLimit` on its 4097th child, a
  ceiling users can only work around (§14 says "Agents do not leak slots", which is then false for
  two of the three tables). It is the owner's standing "no ceilings" rule.
- Fix: (g) erases the actor's `spawns_` entry (keeping a tombstone only while its spawn id could
  still be replayed), erases `monitors_[pid]` and the pid from every watcher list when the actor is
  reaped, and says so in the Price column. Also say `ActorDown` from `stop(pid)` and the adapter's
  `ChildStopping` are deduplicated (a local child now reports twice).

### F7. MAJOR (4.1, 4.2): `Initialize` gating cannot hold an `ask`, and the held …

Full title: F7. MAJOR (4.1, 4.2): `Initialize` gating cannot hold an `ask`, and the held messages
are outside the checkpoint

- "The child's adapter dispatches nothing but `Initialize` until `Initialize` has been handled,
  holding other messages in order." `pumpOne` has already popped the message and replies with the
  turn's result (mesh_actor.cpp:473-499), and the sender's delivery completes. A held `ask` or
  delegated `achieve` cannot be answered later; the result is this turn's string.
- Held messages live in the adapter, not in the mailbox, so `checkpointView` (:672-683) does not see
  them, and a crash or migration loses them. Returning an error instead stops the actor (:501). A
  typed "not ready" reply is stored in `remembered`, so a resend of the same id gets the same
  refusal (:337-341).
- Fix: hold events only, which the checkpoint rule then lists; answer `ask` and delegation before
  `Initialize` with a typed `not_ready` fault, whose sender statement fails with its own cause so
  the program retries under a fresh id. Alternative worth pricing: carry the initial parameters
  inside the issuer-signed `capability` (`ActorMessage::capability` allows 16 KiB, actor_wire.cpp
  :79) and read them from the grant, which removes the race and the gate.

### F8. MAJOR (header "prerequisites", 10, 12.3): `std:goal` has no `tab`, `match` or `upto`

- The header says the scanning functions are "registered in `vortex_stdlib.cpp`". `buildGoalModule`
  exports `to`, `from`, `seq`, `repeat`, `write`, `writeSeq`, `limit`, `cmp`, `alt`, `fail`,
  `succeed`, `scan`, `subject`, `pos`, plus the host routines `every`, `bang`, `if`, `while`,
  `until` (vortex_stdlib.cpp:3148-3700). `tab`, `move`, `find`, `many`, `upto`, `match`, `any` and
  `bal` are specified in vicon §6 but not registered. §12.3 imports `tab, match, upto`.
- Likewise `import std:math (Point)` (§12.3): `std:math` is functions only (vortex_stdlib.cpp:2694
  onward); no `Point` exists anywhere.
- Fix: list the real exports as the prerequisite and the rest as vicon-owed; give `Point` a home (a
  `record Point { x: Float, y: Float }` in `mars:hardware`, or `std:agent`).

### F9. MINOR (5.8, 9.1): the VM already has `#Call` and `#Return` on `+d.stack`; use …

Full title: F9. MINOR (5.8, 9.1): the VM already has `#Call` and `#Return` on `+d.stack`; use them
and say how frames interact with backtracking

- `#Call` pushes a frame cell whose value is the return opcode onto the cursor's `+d.stack`;
  `#Return` pops it (vortex_vm.cpp:664-698). The doc's activation frame "holds the return
  continuation" on the same dimension but lowers calls as `#Jump` and never mentions them. Two stack
  protocols on one dimension would corrupt each other.
- `pushChoicePoint` records the stack top and `backtrack` relinks it (vortex_vm.cpp:179-181,
  208-214), so a failed call unwinds its frames for free. That is what the dispatcher sketch of §9.2
  ("pop frame" at `NEXT`) relies on and the doc should say so.
- `#Call` pushes no frame when the call has no successor op (`nextOp == noCell`, :664-672), and the
  later `#Return` then pops the caller's frame. The compiler must always follow a call with a
  `#Nop`.
- Fix: lower method, handler and answer calls to `#Call`/`#Return`, and the frame's locals to
  `d.vars`/`d.values` on the cell `#Call` minted (found through `linked(cursor, d.stack)`).

### F10. MINOR (4.2 step 6, 6.7): contract violations and cycle exhaustion are …

Full title: F10. MINOR (4.2 step 6, 6.7): contract violations and cycle exhaustion are
`ExecutionResult` failures, not `#Fail`

- A precondition or postcondition violation makes `step` return `success=false` with
  `contractViolation` set (vortex_vm.cpp:1006-1013, 1088-1095) and `run` returns it. It does not
  backtrack, so "as plan failure, with cause `contract`" (§6.7) needs the adapter to call
  `backtrack(cursor)` (public, vortex_vm.hpp:126) and `run` again. Reaching `maxCycles` and "no
  remaining choice points" are also only `errorMessage` strings (:1154-1157, 962-966). Say the
  adapter distinguishes by `contractViolation` and message, or add a cause enum (a VM change §11.1
  denies).

### F11. MINOR (4.1, 7.3, 11.2): transport and spawn message details the adapter must supply

- `send` reaches the peer through `Sender` (`emit`, mesh_actor.cpp:322-325). Same-node delivery
  needs a loopback `Sender` that calls `receive` with the owner's identity, after
  `trustNode(local, owner)`. Neither appears in §11.2.
- `ActorSpawn` is valid only with a `source` pid, a non-empty `hashcashStamp`, a non-empty
  `capability` and an empty `value` (identity/actor_wire.cpp:87-90). The runtime never checks the
  stamp (no `hashcash` in mesh_actor.cpp), so the adapter can fill a fixed one; say so, and say who
  the `source` pid of a launcher spawn is.

### F12. MINOR (5.3, 5.7): the program symbol table has no entry for skill methods

- §5.7 lists goals and event types. A capacity call takes the instance, then the skill's name, then
  the method entry opcode, and V12 forbids a link from state into the program. Add method, answer
  and `def` entries (`skill.method`) to the module symbol table, and say the frame cell's content is
  the skill name that dispatch reads.

### F13. MINOR (stale citations)

- `vortex_vm.cpp:1031-1041` (output cells, §V12, §5.8) is :1070-1078.
- `mesh_actor.hpp`: Authorizer `111-113` is 113-114; Executor/WorkerContext `114-116` is 115-116;
  executor comment `107-109` is 108-111; `ActorOptions` `57-65` is 56-64; "missing evidence" `67-68`
  is 70. `mesh_actor.cpp:131-139` is 129-138.
- `result_slice.cpp:34` is `apps/common/xanadu/result_slice.cpp:34`; say the directory.
- Prefer function names to line ranges, as R2 asked.

## 3. Honesty of "owes one opcode and nothing else in the VM"

True of the opcode set. It holds only if F10's adapter loop is accepted, and §1's "no change to
`ArenaManifold`" holds if F5's fix uses `Mark` fields. The C++ owed in `mesh_actor.cpp` is now eight
items once F3 and F6 are counted (the header says seven).

VERDICT: NOT CONVERGED

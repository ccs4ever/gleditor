# Round 6 critique (vortex role): vagent-agent-language.md v3.3

Paths are under `apps/common/xanadu/` unless stated. Everything below was checked in this worktree.

## 1. Status of round-5 BLOCKING and MAJOR findings

- N1 (floor unreadable by the runtime): PARTLY. The `floorOf` hook is the right choice, but its
  signature, the runtime's way of attributing `remembered` entries to a sender, and the entries of
  senders that go silent are unspecified (F3).
- N2 (erasure during `pumpOne`): RESOLVED. P2, P3, P7 and the end of 4.2 say erasure is deferred and
  step 9 is re-entrant.
- N3 (P1 fallback and touch list): RESOLVED. P1 is marked load-bearing and the `std:goal` host
  routines are refused with a reason. The `mesh_evaluator.cpp` allowlist (`pureOpcode`, line 231,
  and the range check at 470) is correctly left alone for artifacts vagent never loads there.
- N4 (`programRef`, `std:agent`): RESOLVED as a design choice. One cost the text still omits is in
  the minor list.
- N5 (P4 refused alternative): RESOLVED. P4 now prices the host-pid wake (mailbox slot, `remembered`
  entry, `MailboxFull` loss).

## 2. Findings

### F1. MAJOR (4.2 last paragraph, end of 11): "the VM's Dewey path is unused" is false …

Full title: F1. MAJOR (4.2 last paragraph, end of 11): "the VM's Dewey path is unused" is false, and
the path grows without bound

- `VortexVM::pushChoicePoint` always does
  `altDewey = currentDeweyPath_; altDewey.push_back(1); currentDeweyPath_.push_back(0);` and stores
  `altDewey` in the `ChoicePoint` (vortex_vm.cpp:178-196). `Choice` calls it whenever no oracle path
  is set (vortex_vm.cpp:944-958). Only `backtrack` restores `currentDeweyPath_` (vortex_vm.cpp:204).
  `cut` never pops it (vortex_vm.cpp:218-232, it only fires the optional callback), and
  `clearChoicePoints()` is `cut(0)`.
- vagent emits a `#Choice` per statement and per handler (V13, 9.1, 9.2) and ends statements with
  `#Cut`. On the success path the path therefore grows by one entry per statement executed, for the
  life of the VM, and every later push copies the whole vector. Across turns that is a memory leak
  and a cost per statement that grows linearly with the agent's age. A checkpoint does not help,
  because `resume` builds a new `WorkerContext` and therefore a new VM.
- Failure scenario: an agent handling 10 statements per message for 10^6 messages holds a 10^7-entry
  path (20 MB) and copies it, twice, at every `#Choice`. The doc's statement that nothing needs
  doing steers the implementer away from the one-line reset.
- Fix: in 4.2 step 1 add "`setCurrentDeweyPath({})`" (public, vortex_vm.hpp:136-139), and replace
  the final sentence of the paragraph with "pushChoicePoint appends to the Dewey path and cut does
  not pop it, so the adapter clears it at the start of every turn. A turn's path is bounded by its
  `maxCycles`." Better, a fifth VM change under P1, "cut truncates `currentDeweyPath_` to the popped
  point's length", which also helps vicon and vprolog. Record whichever is chosen in the register,
  since §1 says "no other change to the VM".

### F2. MAJOR (4.2 step 7, 4.4, 1): committing a turn with `discard(m0)` leaves the …

Full title: F2. MAJOR (4.2 step 7, 4.4, 1): committing a turn with `discard(m0)` leaves the trail
and relocated runs for ever, so a long-lived agent has an unlisted growth source

- `ArenaManifold::discard` says "The trail entries stay" and only decrements `outstandingMarks_` and
  pops `floors_` (arena_manifold.cpp:1595-1606). `trail_` is cleared only by `restore`
  (arena_manifold.cpp:2112) and truncated only by `release`. `trail()`
  (arena_manifold.cpp:1505-1560) records one `TrailEntry` (a full `CellSlot` copy) for each cell
  older than the innermost mark that is first written under it, and also copies that cell's whole
  link run and content run to the tails of `links_` and `content_`.
- `m0` is outstanding for the whole turn and every pre-existing cell the turn writes (`$turn`,
  counters, intention state, fields) is older than `m0`. So every successful turn appends a trail
  entry and a relocated run per touched cell, and `discard(m0)` keeps all of it. Once
  `outstandingMarks_` is zero nothing can ever undo those entries.
- 4.4 lists what accumulates (events, frames, intentions, clauses, strings) and says an arena "frees
  cells and scratch bytes only by `release` truncation". The trail and the dead pre-relocation runs
  are a separate, per-turn, per-written-cell growth that is not on the list, is proportional to
  turns and not to live state, and is reclaimed only by checkpoint and resume, which needs
  quiescence (U-A1). `compact()` is allowed when no mark is outstanding (arena_manifold.cpp:1687)
  and would drop the dead link entries, but nothing tells the adapter to call it, and it does not
  shrink `trail_`.
- §1 says "no change to `ArenaManifold`", which this makes dishonest for a long-lived agent.
- Fix: add to 4.4 a bullet "Trail and relocated runs: one entry and one copy per written old cell
  per turn, never released by `discard`". Either (a) add a register proposal, "when `discard` drops
  `outstandingMarks_` to zero, clear `trail_`, since nothing can undo it", about three lines in
  `arena_manifold.cpp`, and have the adapter call `compact()` at the end of a quiescent turn; or (b)
  state the growth as a cost in §15 with the per-turn figure and say it makes checkpointing
  necessary rather than an optimisation.

### F3. MAJOR (11.2 P3, 7.3, 5.5): the floor mechanism is under-specified in the …

Full title: F3. MAJOR (11.2 P3, 7.3, 5.5): the floor mechanism is under-specified in the runtime,
and leaves a lifetime ceiling for senders that stop sending

- Signature. The register says "an optional hook returning the sender's floor from a message".
  `Actor::remembered` is a `std::map<std::string, Remembered>` keyed by `requestId`, with
  `Remembered{content, response}` (mesh_actor.cpp:47, enqueue 333-348). An entry does not record its
  sender until a response exists (`response.target`). So the runtime cannot tell which entries are
  "that sender's", nor compare ids, without knowing the id layout of 5.5, which P3's "why" says the
  hook hides. The implementer has to invent the hook's return type.
- Fix: write the signature into P3: `std::function<std::optional<FloorMark>(const ActorMessage &)>`
  with `FloorMark{std::string prefix; std::string floorId;}`. The runtime erases the map range
  `[prefix, floorId)` (valid because the ids are fixed-width hex with a common 16-digit sender
  prefix, and the map is ordered) and refuses a fresh id inside that range as stale. This keeps the
  id layout in the adapter and needs no per-entry sender field.
- Silent senders. A floor arrives only on a later envelope from the same sender. In the central
  pattern of 12.1 a parent receives one reply or `ChildStopping` from each short-lived child, and
  the child never sends again. Those entries are never trimmed, and `enqueue` refuses with
  `MailboxFull` once `remembered.size() >= maxRememberedMessages` (mesh_actor.cpp:344). After 4096
  dead children the parent cannot receive anything. That is the "limit users work around" the
  owner's house rule rejects, and 15 does not list it.
- Fix: add to P3 (or P7) `forgetSender(prefix)`, which erases that range, and have the adapter call
  it when it processes `ChildDown` or `ChildStopping` for a child (and when a space member or peer
  is dropped from `$peers`). State the accepted risk (a late network duplicate from a dead sender is
  then processed once more) in 7.3 and 15.

### F4. MAJOR (4.1, 4.2): `Initialize` is neither ordered before the first message nor …

Full title: F4. MAJOR (4.1, 4.2): `Initialize` is neither ordered before the first message nor
guarded against `resume`

- 4.1 says `Initialize` runs "in the agent's first wake turn. No message can overtake it, because
  nothing reaches a handler before that turn." The runtime does not order a wake against the
  mailbox: `pumpOne` serves any actor with a non-empty mailbox (mesh_actor.cpp:460-465), and P4's
  `wake` is a separate entry point. A parent that spawns a child and sends it a message in the same
  turn (drain order: spawns, then messages, 4.2 step 9) can have that message pumped before the
  wake. The statement is false unless the executor itself guarantees it.
- On `resume`, the new actor is a fresh runtime actor with its own spawn request
  (mesh_actor.cpp:272-286) and the agent's reserved fields come back with the checkpoint. Nothing
  says the executor skips `Initialize` then. As written, a migrated or restored agent whose
  `on Initialize { courier := spawn Courier() ... }` (12.4) runs again would spawn a second child.
- Fix: add a persistent reserved field `$initialized` (5.6, 4.3). State that the executor's first
  turn of any kind, a message turn included, runs genesis and `Initialize` before dispatching the
  event, and that a turn on an agent whose `$initialized` is set never does. Say that `resume`
  leaves the flag as the checkpoint has it.

## 3. Minor items

- P10's "why" is wrong: categories are free strings passed to `gleditor::logging::category(name)`
  (`include/gleditor/logging.hpp:15`), and `vortex.stdlib` and `vortex.sched` appear in no header.
  The change is only a name in `design/logging-migration.md` and cannot be refused by the code.
- 4.4 "Program per agent" omits the standard library. `spawn` and `resume` build a bare
  `WorkerContext` (mesh_actor.cpp:150, 262), whose `stdlib` is not bootstrapped. Resolving
  `std:logic`, `std:collections` and `std:string` "against the agent arena's `VortexStdLib`" (11.1)
  means `bootstrap()` (vortex_stdlib.cpp:1455-1487), which builds all 17 modules, UI, nav and bridge
  included, in every arena and again after every `resume`. Say so, give a measured cell count (not
  measured here), or name the selective `build*Module` calls.
- 4.2 closing paragraph: `checkpointView` refuses while `actor->running` (mesh_actor.cpp:672), and
  step 10 runs inside the executor, so the agent is "quiescent" only after `pumpOne` returns. Say
  the adapter checkpoints between `pumpOne` calls.
- 4.2 "Hard failures": `run` also returns `success == false` for `Assertion failed` and for
  `Logic failure: no remaining choice points` (vortex_vm.cpp:756, 966), with
  `contractViolation == None`. List them among the cases the adapter classifies, and say the base
  choice point makes the second unreachable.
- 15: the `ActorSend` value and the reply are each capped at 32 KiB and the per-turn scratch budget
  is the grant's `maxOutputBytes` (at most 32 KiB, `spawn` check at mesh_actor.cpp:146). State that
  a turn building more than 32 KiB of strings is a hard failure with cause `fuel`.

VERDICT: NOT CONVERGED

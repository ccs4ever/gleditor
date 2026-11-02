# I3-2 design review, round 2: distributed-systems expert (persona distributed)

Scope: `i3-2-design.md` revision 2 (commit `cae4a6ed`, unchanged at HEAD `3ae8d384`), sections 6, 7
and 9 in depth and the rest for context, against P19 sections 2.1, 4, 5.1 and 8.2. Every claim is
checked against `apps/common/xanadu/vagent/runtime/agent_executor.cpp` at HEAD `3ae8d384`
(`agent_executor.cpp:N` below), with `compiler.cpp` for the settle and source-change routines. New
persona, so there is no "Earlier findings" section. Scratch work: `/tmp/p19-distributed/calc.py`
(limit arithmetic) and `/tmp/p19-distributed/clk.cpp` (clock check). Nothing in the repository was
edited except this file.

All the checks below are on the one owner thread that runs `execute`, `settle` and `deliverHost`,
and on the host threads that call `observe`, `bindSource` and the new `healthAction`. The state
shared between them is the `PostBox` mutex and the `Ingress` mutex (`agent_executor.cpp:540-700`,
`:697`). I read the design for what it makes of that boundary.

## What is sound

- **The fault in the current code is real.** `emitSourceChange` calls `rtSettle_` inside the advance
  (`compiler.cpp:5089`, `:5136`), so a failing `-pred` handler undoes the advance and the retire.
  Separating the state change from the handlers is the right shape for a repair path.
- **Isolation by the arena, not by the host.** The design keeps the arena latch as the authority
  (`SourceFaulted` from the routine) and calls the host mirror an optimisation. A reading that races
  the mirror is still refused by the routine, because control items run ahead of observations.
  Finding M7 asks for this sentence to be written down, but the structure is right.
- **Queue-time and turn-time grant checks.** The design adds the second check that round 1 asked
  for. The binding check in `drainPosted` (`:3958-3985`) is the model and it is the right pattern.
- **Event-indexed estimator needs no clock.** There is no steady-clock rebase, no resume rebase, no
  suspend question for health state. That removes the whole class of problems item (5) asks about,
  for the estimator. (Grant lifetimes still use the steady clock; see m3.)
- **Control-first ordering is reachable.** `pushControl` (`:642`) and the `hostEvents` insertion
  (`:3910-3915`) already give "ahead of observations, behind earlier control items, in call order".
  Adding one more kind to that group is a small change in two places.
- **Reset keeps dedup and streams.** Committed pre-reset events remain `Duplicate` after a reset
  (the dedup window and streams stay), so committed replays cannot become "new eligible" readings.
- **Quiescence guards exist.** `checkpoint` refuses a non-empty `hostEvents` or `PostBox` (`:4939`),
  and `upgrade` refuses the same plus a linked `d.pending` (`:5068-5077`), so a queued health
  action, and the host-only epoch with it, cannot cross a checkpoint.

Nothing in this design contradicts the approved spec, so there is no BLOCKING finding. Eight MAJOR
findings follow. Each is a missing rule in the protocol rather than a flaw in the shape of the
design.

## BLOCKING

None.

## MAJOR

### M1. `discardPendingEntry` turns all-or-nothing delivery into at-most-once, at batch granularity

**Problem.** Section 6 runs one `settleEntry` turn over everything on `d.pending`. If it fails, its
undo restores `d.pending` and `discardPendingEntry` detaches the whole queue. Three consequences,
all absent from the design:

1. **A correct handler loses events because another handler failed.** The settle turn is one turn.
   Handlers for events A, B and C run in sequence (`emitSettle`, `compiler.cpp:5685-5745`, one
   `inner` loop over the detached round). If the handler for C fails, the undo removes the effects
   of the handlers for A and B as well, and the discard then removes all three events. No handler
   ever sees A or B. Section 6 says "a correct handler still sees every event once"; that is true
   only when no handler fails. When one fails, the guarantee is "no handler sees it", for every
   predicate, including events the failing handler is not subscribed to.
1. **Exhaustion looks the same as a defect.** The turn fails on cycles, fuel or depth as well as on
   a handler error. Round 1 priced a reset at O(K·H) records with one settle per dropped support.
   The upper bound on events in one batch from the limits is `keysPerSource` per bound predicate (4
   each at the defaults); an isolate of every source on a fleet of 128 with one predicate is up to
   512 events (`calc.py`). Before 2b, running out of fuel refused the whole change and the host
   could retry with a larger grant. After 2b the change commits and the events are dropped, so a
   legitimate large batch is silently lost for want of fuel.
1. **The loss is invisible to the program.** The only trace is `stat()` and the outcome code. A
   program that maintains secondary state from `-pred` events (a cached derived fact, a timer, an
   intention) keeps that state for ever, because the signal that its premise went away is gone and
   nothing re-derives it.

The delivery guarantee therefore drops from "events run atomically with the change that raised them"
to "events run at most once, all or none per batch, loss unreported to the program". Section 6
should state the guarantee it now provides.

**Evidence.** `compiler.cpp:5685-5745` (`emitSettle`: one turn, detach of the whole round,
`failNow()` at depth); `agent_executor.cpp:2241-2247` (a failed turn is `arena.release(m0)`, nothing
partial survives); `design §6` lines 227-248.

**Consequence.** An advance, retire, isolate or reset on a program with any failing or expensive
`-pred` handler loses belief-removal notifications permanently and silently. The repair path works,
but the program it repairs is left inconsistent without a way to learn that.

**Required fix.** Pick a guarantee and write it into section 6. Recommended: settle in per-event
turns (or chunks of a bounded size) so a failure discards only the event, or chunk, that failed. On
failure, move the failed event to a bounded `d.discarded` list on the agent (type, predicate,
round), not into a detach that drops it, and expose its count and head through `$stats` so a program
can see what it missed. If the owner prefers one turn per change for cost, state at-most-once and
all-or-nothing per batch, give the loss a counter per event type, and make `SettleFailed` carry the
number of events discarded. Add a test where one correct handler and one failing handler subscribe
to different predicates, and a test where the batch is too large for the turn fuel but each event
alone is not.

### M2. Settle and discard must run inline, with the pending queue guarded at every other boundary

**Problem.** Section 6 says the executor "runs, in the same wake and before any other item", but the
design never says how. The obvious way in this code, queueing a `HostEvent` for `settleEntry`, does
not give that, and the surrounding code has four boundary gaps:

1. **A queued settle event is not first.** `settle()` swaps `state.hostEvents` out as `batch` and
   delivers it in order (`:1983-1999`). Anything `deliverHost` adds to `state.hostEvents` while it
   runs goes to the next round, after the rest of the current batch (observations behind the
   change). When the ingestion cap trips, the tail of the batch is re-inserted at the front
   (`:1989-1993`), ahead of the queued settle event. So a queued settle event would run after
   observations, which is the wedge moved into ingestion that the design wants to avoid, and it can
   fall in a later wake once `kMaxHostEventRounds` (8, `:88`) is reached. The cap counts only
   `batch[k].ingest`, so an inline call is not subject to it. The design must say inline: call
   `runTurn` for the settle turn, and for the discard turn on failure, from `deliverHost` directly
   after the change turn returns.
1. **The `ArenaExhausted` guard skips the settle turn.** `deliverHost` returns before running any
   entry when `cellCount() >= arenaSlots` (`:1873-1880`). It applies to every `event.entry`, so
   `settleEntry` and `discardPendingEntry` would be skipped too. The events then stay on `d.pending`
   and the next ordinary turn's `rtSettle_` picks them up (the wedge moves to that turn). `upgrade`
   refuses while `d.pending` is linked (`:5077`), so the actor can never be upgraded. The same guard
   blocks `healthEntry`, contradicting section 7.4's "exempt from `ArenaExhausted`".
1. **Checkpoint does not check `d.pending`.** `checkpoint` tests `hostEvents`, `PostBox`, mailbox,
   sends, flights and the runtime view (`:4939-4961`), not `d.pending`. `restoreState` copies
   fields, profiles, beliefs, spaces and intentions and not `d.pending` (`:4664-4850`), so events
   left on `d.pending` would be lost silently across a resume. `upgrade` does check it. The two
   should match.
1. **The outcome is written twice.** `finishSourceChange` records `Committed` right after the change
   turn (`:1888-1889`, `:1936-1941`), before the settle turn runs. A host thread reading
   `sourceChangeOutcome` between the two lock acquisitions sees `Committed`, then `SettleFailed`.
   The same applies to the health outcome log. The outcome should be written once, after the settle
   turn, from the pair of results. (The advance's floor update in `finishSourceChange` is correct on
   the change turn's result and should stay there.)

**Evidence.** `agent_executor.cpp:88`, `:1851-1891`, `:1936-1965`, `:1980-2005`, `:4932-4961`,
`:5068-5077`; `compiler.cpp:5089`.

**Consequence.** With a queued settle turn the ordering claim fails exactly in the case it exists
for (a flooded actor with a poisoned handler). With the guard in place the exhausted-arena repair
path is not repaired.

**Required fix.** Write into section 6: (a) settle and discard run inline in `deliverHost` for any
entry that can leave `d.pending` non-empty (`sourceChangeEntry`, `healthEntry`); (b) both are exempt
from the `ArenaExhausted` guard, with the reason (they mint at most a few cells; the discard mints
none); (c) the invariant "`d.pending` is empty between wakes" is enforced: `checkpoint` refuses when
it is linked (a new `CheckpointRefusal`, or reuse `HostDeliveryPending`), and the change turn's
outcome is published once, after the settle step; (d) skip the settle turn when `d.pending` is not
linked, so a program with no `-pred` handlers pays no extra turn
(`arena.linked(agentCell, dims.pending)` is the check `upgrade` uses). Add tests for (b) and (c).

### M3. The reset epoch is bumped at call time, which is neither authorised nor transactional

**Problem.** Section 7.3 bumps the principal's epoch and drops its coalesced charge "at
`healthAction(Reset)`", under the ingress mutex, before the reset turn exists. Three defects:

1. **A refused or failed reset still destroys queued evidence.** The grant is checked again at turn
   time (section 7.2). If the token is revoked or expires while the action waits, or the reset turn
   fails (`TurnExhausted`, `NoProfile` for an unbound principal, `ArenaExhausted`), the epoch has
   already moved. Every queued observation of that source then refuses `SupersededByReset`
   (Terminal), and its coalesced charge is gone, although nothing was reset. A holder who is about
   to lose authority can discard up to `ingressQueuePerSource` readings (8 at the default) of any
   source the grant names by issuing a reset and having it revoked. The same happens without any
   malice when a reset fails.
1. **The check must be in `deliverHost`, not `drainPosted`.** The existing binding check for queued
   ingestions is in `drainPosted` (`:3958-3985`). Items it has already converted to `HostEvent`s are
   not checked again, and the ingestion cap defers them across wakes (`:1989-1993`). A reset issued
   after such a conversion is inserted ahead of them (`:3910-3915`), and they then run after it
   carrying the old epoch with no check. They would become the "new eligible observation" that
   section 7.3 forbids. The same applies to refusal notes already converted to bookkeeping
   `HostEvent`s: the design says notes "record the epoch" but `HostEvent` has no field for it.
   `HostEvent` (`:913-923`) and `IngestKey` need an epoch, and `deliverHost` must compare it with
   the current one under the mutex.
1. **Bump and push are not specified as one critical section.** If the bump and `pushControl` are
   separate, an observation accepted between them carries the new epoch but runs after the reset, or
   carries the old epoch and is wrongly superseded. `observe` already pushes under the ingress mutex
   (`:3760-3772`); `healthAction` should bump, stamp and push in the same section.

**Evidence.** Design 7.3 lines 321-326; `agent_executor.cpp:913-923`, `:1851-1891`, `:3910-3915`,
`:3958-3985`, `:3760-3772`.

**Consequence.** Evidence loss without a reset, and a window in which pre-reset readings count after
a reset.

**Required fix.** Give every queued item (observation, refusal note, control item) a monotonically
increasing host sequence number stamped under the ingress mutex. A reset records
`resetFloor[source] = its own number` when its turn commits (not when it is called), after the
turn-time grant check. An item is superseded iff its number is below the floor of its source. The
check is in `deliverHost`. Because control items run first, "queued before the reset" and "numbered
below the reset" coincide, so the effect is the one section 7.3 wants, and a failed or refused reset
supersedes nothing. Drop the coalesced charge entry at commit as well, not at call time. Add tests:
a reset revoked while queued leaves queued readings intact; a reading drained before the reset and
deferred by the cap is refused `SupersededByReset`.

### M4. A superseded reading can come back as a new one: retransmits, and a second host

**Problem.** `SupersededByReset` is Terminal and records nothing in the arena (the turn is refused
and undone), so the event is not in the dedup window and its sequence is not in the stream. An
at-least-once transport (which is why event IDs, dedup and `Duplicate` exist) resends it. It arrives
after the reset, carries the new epoch, and is accepted as a new reading unless its age exceeds
`max_age`. That contradicts section 7.3's own wording that the reading "must never count as the new
eligible observation" and is inconsistent with a reading that had committed before the reset, which
stays `Duplicate`. The host-only epoch and the outcome log are also gone after a resume on a second
host (`forgetIngress` on `retire`, `:5407-5418`), so the second host cannot refuse the retransmit
either.

**Evidence.** Design 7.3; `Ingress::log` keeps an outcome per event but `observe` consults it only
to decide charging (`:3598-3612`), never to refuse a resend; dedup is 16 entries per source at the
default (`epistemic_limits.hpp:27`).

**Consequence.** The invariant the epoch exists to give holds for events queued before the reset
only until the device resends them.

**Required fix.** Record the cut in the arena, where it survives resume: the reset turn writes, per
stream of the profile, a sequence floor equal to the highest sequence the host had admitted for the
source at the reset's number (the host tracks `highestQueuedSequence[source, incarnation]` under the
mutex), so `StaleSequence` refuses any superseded sequence for ever, on any host. The streams record
is already kept across a reset for exactly this. If the owner prefers not to touch streams, the
fallback is an acquisition-time cut (`acquired_at` before the reset's wall stamp, uncertainty
widened) and the design must say that it depends on I3-3's clock mapping. Add a test: reading
queued, reset, same reading resent, refused.

### M5. Health actions are not idempotent, receipts are host-local, and the outcome log has gaps

**Problem.** Section 7.2 gives a receipt "with a ticket" and an outcome log. It does not give a way
to retry safely, which every distributed caller of an operator API needs because `Undelivered` is
`Retryable` (`outcomeClassOf`, `:3399-3402`) and the host can crash or be resumed elsewhere.

1. **No idempotency key.** A retried `Reset` is not a no-op. If the first reset committed and the
   source has since supplied good readings, the retry re-zeroes R and N, drops the new supports and
   bumps the epoch again, destroying the evidence the first reset asked for. A retried `Isolate` or
   `Degrade` appends an `actions` record each time, and `actions` is bounded to 8 with oldest
   evicted, so eight retries of a no-op push the original isolate record out of the audit list
   (`calc.py`). Section 5 says the audit list lets the profile "always say that a fault existed".
1. **Tickets are host-local.** A ticket from a per-executor counter collides with a different action
   after a restart or a resume on another host, and `healthOutcome` answers with the wrong outcome.
   The executor already has a random per-instance name (`Ingress::clockInstance`, `:761-770`); the
   ticket should carry it.
1. **`nullopt` means three things.** Not yet run, evicted, and forgotten (the log is erased with the
   ingress state by `forget`, `:860-872`, on stop, `retire` and lease end) all return the same
   value. For the ingestion log the entry is created at queue time as pending (`:3763-3771`); the
   design does not say the health log does the same, and `Ingress::log` evicts the oldest entry
   regardless of state (`:773-785`).
1. **Accounting on every exit path.** `kMaxPendingHealthActions` needs a count that is released on
   each path that ends a queued action. Today's analogue is released in `finishIngest` and `abandon`
   only; `orderlyStop` clears `hostEvents` without `dropHostEvents` (`:2019`, `:2022`),
   `deliverHost`'s exhausted path calls `finishIngest` and `finishSourceChange` only (`:1874-1879`),
   and the `else` at the end of `drainPosted` calls `finishEffect` for any item it does not
   recognise (`:4018-4019`). A health item added without its own branch would be handed to
   `finishEffect` as an effect result.
1. **`SettleFailed` invites the wrong retry.** It is `NeedsHostAction` (section 9), which the host
   reads as "the change did not happen". For a reset that retry is the destructive one in point 1.

**Evidence.** `agent_executor.cpp:773-785`, `:860-872`, `:1874-1879`, `:2019-2022`, `:3399-3402`,
`:4018-4019`; design 5 line 203-207, 7.2.

**Consequence.** An operator that does the correct thing on a lost reply (resend) can lose evidence
or audit history.

**Required fix.** (a) The request carries a caller-chosen `requestId`, copied into the `actions`
record; a retry whose `requestId` is found in the outcome log or in `actions` returns the original
receipt and appends nothing. (b) `Reset` carries the fault generation it intends to clear (a small
Int on the profile bumped by every isolate and reset); a mismatch refuses `StaleHealthState` and
commits nothing, which makes a duplicate reset a no-op by construction even if the log is lost. (c)
Ticket = (instance name, counter). (d) `healthOutcome` returns a tri-state: pending, known, unknown
(evicted or lost with the host); the log entry is created pending at queue time and pending entries
are never evicted. (e) Every exit path finishes the action: `deliverHost`, `dropHostEvents`,
`abandon`, `orderlyStop`, plus a `drainPosted` branch of its own. (f) `SettleFailed` is reported as
a committed change with a warning, not as a refusal, so `outcomeClassOf` should not map it to
`NeedsHostAction`. (g) Retries that change nothing append no `actions` record.

### M6. The "one predicate" claim misdescribes the four sites, and new codes are chargeable

**Problem.** Section 7.3 says four places "today test `sourceChange.has_value()`": `pushControl`,
the `hostEvents` insertion, and the `epistemic` and `abandon` lambdas. Only the first two do
(`:645`, `:3912-3914`); they order items. The `epistemic` lambda (`:3859-3861`) is
`ingest || refusalsOf || sourceChange`, the set of items that must be kept, not dropped, while a
turn is running, and it includes non-control items. `abandon` (`:3844-3858`) does per-kind
accounting. Replacing either with an "is control" predicate would either drop ingestions mid-turn or
lose the accounting. The design needs two named predicates, "is control" for ordering and "owes
epistemic accounting" for the other two, and a per-kind branch in `abandon`.

Further sites the design does not list, each of which needs a change for a health item:

- the `if / else if` chain in `drainPosted` whose last `else` is `finishEffect` (`:4018`);
- `PostBox::putBack`, which reinserts kept items at the front (`:650-654`). A control item pushed
  while a pre-first-turn item was kept ends up behind the older observations (`[c1,o1,o2]` + new
  `c2` → `[c1,o1,o2,c2]`), so "ahead of queued observations" fails in that window. The fix is a
  stable partition (controls first) in `putBack`;
- `deliverHost`'s `admit` lambda, chosen from `event.ingest` and the bookkeeping entry
  (`:1862-1870`), needs the action kind so `Degrade` is admitted and `Isolate`, `Clear` and `Reset`
  are not;
- `Ingress::chargeable` (`:902-910`) is a deny list: any code not named is charged. `SourceFaulted`
  and `SupersededByReset` are not in it, so as written both are charged to the source's
  `refused_count`, post a bookkeeping turn, and a faulted source's refused readings would keep
  mutating the profile of the source the design says "records nothing". The design says "never
  charged" for `SourceFaulted` and nothing for the other. Both must be added to the list, with a
  test;
- `outcomeClassOf` defaults to `Terminal` (`:3410`), so a code left out of section 9's mapping is
  silently Terminal.

**Evidence.** As cited.

**Consequence.** A faulted sensor flooding readings generates charge notes and bookkeeping turns
against a latched profile; a health action in the pre-first-turn window runs after observations; a
health item can be mis-dispatched as an effect result.

**Required fix.** Name two predicates (`isControl`, `owesAccounting`) as members of `PostBox::Item`
and `HostEvent`, use `isControl` in `pushControl`, `putBack` and the insertion, and `owesAccounting`
in the `epistemic` lambda; give `abandon` and `dropHostEvents` a per-kind branch; add a health
branch to the `drainPosted` chain; extend `chargeable()`; list these sites in section 9 with a test
for each (a charge never made for `SourceFaulted`; a control item pushed during a kept window runs
first).

### M7. The host latch mirror has no failure semantics, and it is keyed on the wrong result

**Problem.** Section 7.4: "the executor mirrors the latch host-side once the isolate commits". Four
gaps:

1. **Keyed on the final outcome.** Once M2 is applied the change turn can commit and the settle turn
   fail (`SettleFailed`). An implementation that sets the mirror when the final code is `Committed`
   leaves the arena latched and the mirror clear. The mirror must be set from the change turn's
   result, or better from the arena state after the turn.
1. **A failed isolate leaves ingestion open.** If the isolate turn fails (`TurnExhausted`,
   `EpistemicBudget`, the revocation check) nothing latches, nothing is mirrored, and the flooding
   source keeps filling the queue. The design's own justification for the mirror is to stop that
   flood. A safety action should fail toward containment: set the mirror when the isolate is
   accepted at call time, and clear it only if the turn ends in a refusal that does not latch
   (unauthorised, unbound). The arena stays the authority, so a wrongly set mirror cannot create
   trust, only refuse readings that the operator wanted stopped.
1. **Reset by another road does not clear the mirror.** Section 10 says I4's `reset source`
   directive lowers onto `healthEntry`, which then runs inside a handler turn and never passes
   through the host API's post-turn hook. The mirror stays set after the arena unlatches, and the
   source is refused at ingress for ever, with `SourceFaulted` that never reaches the arena. The
   mirror update must come from the result of every health turn, whatever started it: have the
   routine write the latch it left into a host-readable field of the turn result, and apply it in
   one place.
1. **Order of checks.** The mirror test belongs after the binding check in `observe` (`:3580-3590`),
   so an unbound principal learns nothing about another principal's latch state, and it must log the
   refusal the way `refuse` does without charging (M6).

After resume the mirror is empty and the arena's refusal rebuilds it; this is correct only because
the arena refuses first. After an upgrade the mirror (executor-level, `Ingress`) survives and the
latch fields must be carried by the owned-state translator; with no translator registered the
upgrade already refuses a populated profile (`:5317-5320`), which is safe. Say both in the design.

**Evidence.** `agent_executor.cpp:1936-1965`, `:3580-3612`, `:5317-5320`; design 7.4 lines 338-345,
section 10.

**Consequence.** Permanent ingress refusal of a healthy, reset source; a flood that the mirror was
meant to stop continues when the isolate turn fails.

**Required fix.** As above: (1) set the mirror from the change turn's result or the arena; (2)
pre-latch the mirror at call time for `Isolate` and define when it is cleared; (3) one apply point
for every health turn, with the routine's result carrying the latch; (4) place the check after the
binding check and add a test where I4's path is simulated by running `healthEntry` without the host
API. State that the mirror is a cache and the arena is the only authority.

### M8. Grant authority is not rechecked where it can change

**Problem.** Section 7.1 refuses a grant whose issuer is "a principal bound to the actor", checked
once, in `grantHealth`. `bindSource` can bind that principal later (`:3414-3490`), and nothing
rechecks. A sensor identity that is bound after the grant is issued then holds health authority over
itself and its peers, which is the self-clear P19 4 forbids. Related gaps:

1. **The recheck at turn time must key on the token, not the ID.** The ID is "an audit label"
   readable by programs; `revokeHealthGrant(target, id)` removes by ID. A revoke followed by a new
   grant with the same ID and a wider scope must not authorise an action queued under the old token.
   The queued item holds the token, and `deliverHost` looks the grant up by token, rechecks the
   principal and kind against the current record, and rechecks the issuer against the current
   bindings.
1. **The recheck belongs in `deliverHost` (see M3).** A check in `drainPosted` does not cover
   actions already converted to `HostEvent`s.
1. **Revocation has no guarantee to offer.** `revokeHealthGrant` returns `AgentExecutor *`, so it
   cannot report that nothing matched. The linearisation point should be stated: an action is
   authorised iff the token is live at its check under the ingress mutex; the turn then runs without
   the lock, so a revoke that returns does not stop an action already past its check. Callers who
   need a hard stop use `unbindSource` or the pre-latch in M7.

**Evidence.** `agent_executor.cpp:3414-3490`, `:3958-3985`; design 7.1 lines 270-280, 7.2 lines
310-312.

**Consequence.** The "a sensor cannot hold health authority over itself" rule holds only for the
order grant then bind in the wrong direction; a revoked token can be revived by an ID reuse.

**Required fix.** Recheck the issuer rule in `bindSource` (refuse binding a principal equal to a
live grant's issuer, `InvalidHealthGrant`'s sibling code) and at turn time; look grants up by token;
return an `expected` or a count from `revokeHealthGrant`; write the linearisation point into 7.2.
Tests: a bind after the grant is refused; a revoke and regrant with the same ID leaves an old queued
action unauthorised.

## MINOR

- **m1. Fairness, in detail.** Neither side starves the other in the long run. Control items run
  first in every wake and the ingest cap counts only ingestions (`:1983-1999`), so ingestion cannot
  starve control; health actions are bounded at 16 queued, so control cannot starve ingestion
  indefinitely. Two gaps remain. Control is not counted against the wake: `bindSource` and
  `retireSource` push unbounded control items (`:3485-3489`, `:3515-3518`), and each is now two
  turns, so one wake can run a long chain before the pump moves on. And `pumpUntilIdle` counts only
  `runtime_.wake` calls toward `maxTurns` (`:3220-3262`), not the inner turns. Add a per-wake budget
  for control and settle turns (a setting, not a literal) and defer the remainder as the ingest cap
  defers ingestions.
- **m2. Quiescence for checkpoint and upgrade.** Both refuse while `PostBox` holds anything for the
  actor (`:4939`, `:5068`), so a hot source that keeps the queue non-empty prevents either. The
  mirror in M7 is what makes quiescence reachable after an isolate; say so, and note that a failed
  isolate (M7.2) leaves the actor un-checkpointable.
- **m3. Steady clock across suspend.** `AgentClock::system().steady` is `std::chrono::steady_clock`
  (`:461-464`), which is `CLOCK_MONOTONIC` here (`clk.cpp` prints a 0 ms difference from
  `clock_gettime(CLOCK_MONOTONIC)`). `CLOCK_MONOTONIC` does not advance while the system is
  suspended, so a one-hour grant stays valid for one hour of running time, however long the host
  slept. Bindings share this today. For a grant that can destroy evidence, say so in 7.1, bound
  `kMaxHealthGrantLifetime` accordingly, and consider a wall-clock cross-check when
  `wallSynchronised`. The injected `AgentClock` seam makes `CLOCK_BOOTTIME` a one-line change for
  tests and production.
- **m4. Durability of a safety action.** The arena is volatile; a latch is durable only once a
  checkpoint follows it. A crash, or a resume from an older checkpoint on a second host, drops the
  isolate and the source flows again. The host's own log is the replay source (section 5 already
  says the host keeps one), so the design should state that the log must be durable and replays safe
  (M5). Resuming from a checkpoint while the old instance still runs also yields two actors with
  divergent latches; `resume` does not fence the old one, so this is a question (below).
- **m5. Token source.** "128 random bits from the executor's random source" is `std::random_device`
  in this file (`:761-770`), whose entropy is implementation defined. A bearer credential should
  come from the OpenSSL CSPRNG, a project dependency already.
- **m6. The wedge remains on the ingestion path.** `rtObserve` still calls `rtSettle_` inside the
  ingestion turn (`compiler.cpp:5668`), so a reading that replaces a key and raises `-old` with a
  failing handler is refused for ever (`IngestionFault`, `NeedsHostAction`) and that key cannot
  update. Isolate and reset do now unwedge it. Record it as accepted, or ask the owner.
- **m7. Audit of refused actions.** `actions` records accepted actions only; an
  `UnauthorizedHealthAction` appears only in the bounded host log. Say so, since "who tried" is an
  audit question as much as "who did".
- **m8. Extra turns.** Each source change or health action is now two or three turns, each
  incrementing `$turn_count`, draining and deriving the schedule (`:2275-2291`). Request IDs and
  `ready` stamps depend on the count only being monotonic, so this is safe; note it.
- **m9. Reset epoch in `Ingress::forget`.** The epoch and floor maps must be erased in `forget` and
  not by `unbind`, or an unbound and rebound source inherits a stale floor.

## Questions for the owner

1. Is "at most once per batch, loss visible" acceptable for belief events (M1), or must a correct
   handler's event never be lost because of another handler? The second needs per-event turns.
1. Should the owner's "fix the `-pred` handler wedge" decision also cover the ingestion path (m6)?
1. Is a cross-host fence for resume required (m4)? Today `resume` does not stop an instance that is
   still running elsewhere.
1. May a reset of one sequence floor per stream be written into the profile (M4), or is an
   acquisition-time cut preferred even though it depends on I3-3?

## Fit to later iterations

- **I3-3 (heartbeats, clock mapping, sensor health reports).** The event-indexed estimator does not
  constrain it. Three records are owed: a sensor health report path must not use `healthEntry` with
  a sensor issuer (the 7.1 rule); the mirror (M7) and the epoch/floor (M3, M4) need to cover its
  `suspect` and `degraded` reasons, which are added by a producer that is not an operator; and the
  acquisition-time fallback in M4 depends on its clock mapping. The heartbeat deadline rebase must
  not use the grant clock (m3).
- **I4 (queries, scoring, gates, directives).** The in-language directives run `healthEntry` inside
  handler turns, bypassing the host API's hooks: the mirror (M7.3), the epoch (M3) and the receipt
  and idempotency record (M5) must be driven from the routine's result so both roads behave the
  same. I4's eligibility recomputation at query time is sound under 2b, because `d.pending` is empty
  between wakes only if M2 is applied. I4's calibration reset must be inside the same reset turn
  (the design already says so) and covered by the generation check in M5.
- **Live-upgrade translator.** It must carry `faulted`, `fault`, `reasons`, `actions`, R, N and the
  generation in M5 (the spec's "never clears a hardware fault"). It must also treat a linked
  `d.pending` as non-quiescent, as `upgrade` already does (`:5077`). A missed remap of `settleEntry`
  or `discardPendingEntry` in `program.cpp` is a silent no-op after an upgrade (section 9 says so);
  add a test that fails when either entry is absent after instantiate, since absent means the settle
  step is skipped and `d.pending` wedges ordinary turns. I3-2 should record that the mirror and
  grants are host state that survive an upgrade and do not survive a retire.

Tally: 0 BLOCKING, 8 MAJOR, 9 MINOR.

Verdict: FAIL

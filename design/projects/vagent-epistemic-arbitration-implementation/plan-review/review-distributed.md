# Remaining-work plan review, round 1: distributed-systems expert (persona distributed)

Scope: `remaining-plan.md` (draft 1) and the rows of `traceability.md` for T-1 to T-16, U-1 to U-23,
DF-1 to DF-7, 8.P5-1 to 8.P5-4 and 2.1-45, against spec sections 2.1, 2.2, 5.1, 6.3 and 8.2, code at
HEAD `3ae8d384`. Line numbers below are HEAD (`git show HEAD:<file>`). The working tree already
carries uncommitted edits to `agent_executor.cpp` (the X-27 wakeup); I cite HEAD.

Focus: the verification loop split, idempotency keys and replay floors across checkpoint, resume and
upgrade, the upgrade quiescence rule, ordering of control items, settle turns and ingestion,
host-only state, the signed-ingress adapter (O4), and whether each transport change is tested before
the learning that depends on it.

## What is sound

- The four-way split in spec 6.3 (issuer pending record, intake journal, processing turn, outcome
  response) maps onto I3-4b and I3-4c in the right dependency direction. Transport receipt
  (`ActorAck` on enqueue, `vortex/mesh_actor.cpp:517-534`) is correctly treated as carrying no
  application meaning.
- Putting I3-4a to I3-4c before I5-1 means no label can be applied through an untested transport.
  The plan refuses the second, test-only write path (question B, option 2), which is right.
- V-D is scheduled as a design before I3-4a, and listed as a design that must settle idempotency
  keys, retry and backoff. Those are the right topics.
- The plan notes that grants and the ingress mirror are host state and are not carried across
  upgrade (T1). That is the correct default; my findings below are about where the plan lets host
  state leak into durable decisions.
- `ReconcileOutcome`/`UpgradeGraph` already price peak copy (U-23), and the second `upgrade`
  overload's missing preconditions are in T0.

## BLOCKING

None.

## MAJOR

### M1. Replay floor and idempotency key undefined; transport floors are not durable

Problem. I3-4b closes 6.3-26 ("floors survive checkpoint, resume and the horizon") and DF-5 without
saying what a floor over verification event IDs is. Spec 6.3 requires an ID that is "globally unique
from its authenticated issuer", not an ordered one. A floor needs an order. Observations have one
(`incarnation`, `sequence`; `epistemic_ingress.hpp` `CheckedObservation`); verification events as
specified have none. A count-bounded window of recent IDs (like `vagent.epistemic_dedup`) is not a
floor: once an ID leaves the window, an identical retry is neither "cached outcome" nor "too old".

Evidence. Both existing floors are host or runtime memory and are not checkpointed:

- the mesh floors `Actor::floors` (`vortex/mesh_actor.cpp:74`, advanced at `:512-518`) are absent
  from `ActorContinuation` (`vortex/mesh_actor.hpp:117-123`), which carries only `completed`
  (`remembered`, bounded by `maxRememberedMessages`, erased when a floor advances, `:40-51`);
- the agent-level `State::floors` (`agent_executor.cpp:498`, used at `:1660-1668`, capped by
  `kMaxTrackedFloors = 4096`, `:97`) is not read by `restoreState` or `checkpoint`.

So after a resume, a request id below a former floor is accepted again at transport level. The
application journal is therefore the only possible home of the verification floor, and the plan has
not said so.

Consequence. Without a defined floor, 6.3-23, 6.3-25 and 6.3-26 cannot all hold. An identical retry
of a committed, evicted event gets `VerificationTooOld`; the issuer then holds a pending record
whose terminal state it will never learn. The plan also gives the issuer no lifetime, retransmit
driver or expiry (I3-4b lists "the issuer's pending record" but not who retransmits, how often, or
what the issuer does on `VerificationTooOld`). Closing a requirement whose subject is undefined is a
silent drop.

Required fix. V-D (and the I3-4b slice text) must state:

1. The floor is arena-side, per authenticated issuer, over an issuer-assigned incarnation and
   sequence carried in the signed verification payload (the same shape as observations), so that
   "below the floor" is decidable and bounded. If the owner prefers unordered IDs, state the
   retention rule instead (a time horizon) and what is answered after it.
1. The decision order at intake, fixed and tested: authenticate issuer, floor check, journal lookup
   (cached outcome, or `DuplicateVerificationPayload` on payload mismatch), authorization, target
   resolution and expiry. Cached lookup precedes epoch or target expiry, so an identical retry after
   an epoch change returns the cached outcome (6.3-23), not `ExpiredVerificationTarget`.
1. The issuer side: who retransmits (host timer or actor), the backoff and bound, the pending
   record's maximum life, and that `VerificationTooOld` is a terminal outcome of unknown commit
   state which the issuer surfaces to an operator and does not retry.
1. Tests for each: retry inside the window, at the floor, below it, after resume, and after an
   upgrade that opens a new epoch.

### M2. A cached terminal outcome must not depend on host-only authorization state

Problem. I3-4a puts authorization in host grants (`EpistemicVerifyGrant`); T1 says grants are not
carried across resume or upgrade; I3-4c caches "a terminal typed rejection ... recorded and
reported". Nothing says which refusals are cached and which are retryable.

Evidence. Bindings, aliases and floors in `Ingress` are host-only (`agent_executor.cpp:687-748`),
and `observe`'s own queued-ingestion check re-reads the binding when the item runs (`:3957-3975`),
so the result of an event can differ between intake and processing, and between hosts.

Consequence. After a resume on a second host there are no grants until the operator re-grants. A
redelivery in that window that is turned into `UnauthorizedVerification` and cached as terminal
makes the event permanently refused although the issuer did nothing wrong; the issuer removes its
pending record on that "checked outcome". The reverse also occurs: a grant revoked between intake
and processing yields a host-dependent label.

Required fix. V-D must fix two classes: (a) pre-intake refusals that depend on host state
(`UnauthorizedVerification`, unknown issuer, no grant, quota) are transport-level typed errors,
journal nothing, cache nothing, and leave the issuer pending; (b) once intake commits, every input
the processing decision needs (grant identity and scope, maximum weight, epoch, expiry, evidence
scope) is copied into the journal record at intake, so processing reads only arena state. Add a
test: resume with no grants, redeliver, expect a retryable refusal and no cached outcome; then
re-grant, redeliver, expect one application.

### M3. Actor identity across resume and checkpoint rollback is not addressed

Problem. The issuer record and the outcome "name the exact actor" (6.3 step 4). `resume` mints a new
pid: `actor->pid = ActorPid{node_, content, epoch_, nextSpawn_}` (`mesh_actor.cpp:330`), and
`remembered` responses are rewritten to the new source (`:337-343`). A pid-keyed pending record or
outcome check fails after any resume or move to a second host.

A second issue: a checkpoint is a point-in-time copy. If host A commits verification V after the
checkpoint, sends the outcome, and the issuer removes its pending record, a failover to the older
checkpoint on host B has no journal entry for V and the issuer will never retransmit. The label is
lost with a committed-looking acknowledgement. Resuming the same checkpoint twice also yields two
live actors with separate journals (each resume gets a new `requestId`, `agent_executor.cpp`
`resume` via `resumes_++`).

Consequence. The "exactly once, survives resume" claim of 6.3 holds only along a single unbroken
lineage, and the plan does not say so. T-12 and 6.3-26 would be accepted against tests that never
roll back.

Required fix. V-D must (1) key pending records, journals and outcome checks by a stable agent
identity stored in the arena (name plus lineage id), never by pid; (2) rule on durability: either
the outcome is released to the issuer only after a checkpoint covers it (stating the cost), or the
outcome carries a lineage generation and the issuer retains terminal records until the receiver
reports a checkpoint at or beyond them, or the spec states that rollback to an older checkpoint
loses acknowledged verifications (an owner-visible limit, recorded at F2); (3) state that two
resumes of one checkpoint are a fork the journal cannot detect and name who prevents it. Add a
checkpoint-rollback test to I3-4c.

### M4. Control items overtake queued observations with no stamp

Problem. Control posts are inserted ahead of every non-source-change post (`PostBox::pushControl`,
`agent_executor.cpp:642-648`), and the drain inserts a source change ahead of `hostEvents`
(`:3882-3925`). That is safe for an advance or retire because each queued observation carries an
incarnation and is refused as `StaleIncarnation` when it runs (`:3969`). The plan adds reset, clear,
isolate and degrade through the same lane (I3-2c1, I3-2c2, I4-10) with no equivalent stamp.

Consequence. An operator `reset` jumps ahead of readings posted earlier. Those readings run after
it, and the plan's own rule ("the source is eligible only after a new reading") is satisfied by a
reading that predates the repair. A similar inversion is possible through `putBack`, which places
held items ahead of newer control items (`:650-654`), so a held observation can precede a later
isolate.

Required fix. I3-2c1 must define the order contract: each post carries a host per-pid monotone
ordinal assigned at admission; a control action records its ordinal; an observation with a lower
ordinal for that source is refused or ordered before the action (choose one, test both outcomes),
and `putBack` keeps control items first. This host ordinal is not durable and need not be: the queue
is host memory and the checkpoint requires it empty. The durable fact (reset epoch, `retired_by`)
stays in the arena, as I3-2D plans. They do different jobs, so the arena epoch does not remove the
host counter (answer to the plan's question 2). Tests: reset between two queued readings; isolate
behind a held item; wake after `putBack` (T0).

### M5. Upgrade quiescence rule is incomplete and contradicts journal survival

Problem. T1 lists "a pending settle, a queued health action or a pending delivery blocks the
upgrade". Three gaps:

1. The rule as written does not say whether a non-empty journal or processing queue blocks or is
   carried. T-12, U-22 and 6.3 require journals and outcomes to survive upgrade, and T2 closes U-22.
   If they block, an entry backing off or failing terminally can starve an upgrade forever. If they
   are carried, T1's wording is wrong and the carried state needs a translator in the slice that
   adds it (the plan's own parity rule), not at T2, which comes after I6.
1. Nothing schedules processing after resume or upgrade. The processing turn runs "after a
   successful non-speculative turn" (I3-4c); an idle actor resumed with queued entries has no such
   turn. `needsFirstTurn_` exists for resume (`agent_executor.cpp` `resume`), but the plan does not
   use it. Retry attempt counts and backoff deadlines, if host-only, restart on every resume, so
   "bounded retry" becomes unbounded across restarts.
1. The checks happen on the owner thread but posts arrive from other threads. The shared-pointer
   overload tests `posts_->pendingFor(pid)` then proceeds (`agent_executor.cpp:5068-5077`); a post
   landing before the swap is consumed by the new program. That is benign only if every queued item
   kind is program-independent. A verification intake item must be listed as such, or the upgrade
   must close admission for the pid during the swap.

Required fix. State the rule as: upgrade requires no turn in flight, no un-drained host event, no
staged unsent outbox, no unreplied send, no flight; it does not require an empty journal or queue.
Journal entries, queue entries with attempt counts, cached outcomes and floors are arena state,
carried by the translator in the slice that creates them (add a populated-journal case to T1's test
list and move that part of U-22 and T-12 to I3-4b/I3-4c). After resume and upgrade, a processing
trigger scans the arena queue and re-arms backoff from the stored attempt count. Add a test for a
post racing the swap. A migration that opens a new epoch turns queued entries into `ExpiredLabel`
with their cached outcome, not into a silent drop.

### M6. Signed-ingress adapter (O4): the signed bytes must be specified now

Problem. I agree with O4 option (a): the actor `Envelope` has no origin proof (`wire.hpp`, `from` is
"the emitter's name"; `ActorMessage` in `identity_layout.hpp:315-326` has no signature), and a
node-level mesh check does not authenticate a sensor behind a gateway or relay. But the I3-3a text
and test list cover only a `CheckedObservation`, and omit properties that decide whether the
authentication is end to end.

Required content for I3-3D and I3-3a:

1. Audience binding. The signed bytes name the receiving agent's stable identity (see M3) and the
   predicate with its policy hash. Without it, an observation signed for actor A is replayed to
   actor B on any gateway or relay. Add the test "replayed to a different actor or predicate".
1. Domain separation. A versioned context string distinct from provenance, delegation and any other
   Ed25519 use, so a signature over one structure cannot validate as another.
1. Canonical bytes. Fixed encodings for doubles (bit pattern, NaN and `-0` rejected or normalized by
   rule), the tuple, strings and optionals; the tests must include non-canonical but equal inputs.
1. Relay marking from the transport only. `relayedBy` must be taken from the authenticated transport
   peer, never from a field a gateway can omit or a device can forge.
1. A gateway cannot forge but can drop, delay, duplicate and reorder. The design should say which of
   those the incarnation and sequence floors, the time-mismatch refusal and the heartbeat (I3-3c)
   catch, and that delay is attributed per the I3-3b path/device split.
1. Certificate validity: time of receipt and, for a delegated key, validity at acquisition time or a
   stated rule; revocation through the ledger.
1. Extensibility. Verification percepts are authenticated input from an issuer (6.3: "its own
   authenticated percept"). The signed message must be a tagged union from I3-3a (observation,
   verification, later drift and reference), or I3-4a changes an accepted wire format.

On the plan's decision: this is a lead decision as long as the signature travels inside the
application payload, with no change to `Envelope`, `ActorMessage` or the mesh wire. It becomes an
owner decision only if the design needs a new field in the mesh wire.

### M7. The processing turn cannot be validated with a no-op outcome

Problem. I3-4c's outcome is `RecordedNotApplied` and I5-1 "turns the outcome into `Applied`". The
heart of 6.3 step 3 is that label, ledger entry, cached outcome and staged response publish
atomically, and that rollback leaves the intake journal pending with no outcome. With nothing
written beyond the outcome there is nothing to roll back, no `EpistemicBudget` path from a full
ledger, and no case where a later plan failure could undo a published model change. I3-4c would be
accepted without testing the property it exists to prove, and I5-1 would then change the
transactional write set of an accepted slice.

Also, a cached terminal outcome is immutable by 6.3-23; `RecordedNotApplied` cannot later become
`Applied` for the same ID without violating it. The plan covers this only by "no production caller
before F2".

Required fix. Give I3-4c a real write set that is not learning: the immutable verification-version
record and the effective-label ledger entry with predecessor link (both are specified in 6.3 and are
owned state), bounded and charged, without the Beta recompute. Then I3-4c tests atomic publication,
`EpistemicBudget` on a full ledger, rollback of record and outcome together, and a failing later
plan. I5-1 adds the recompute inside the same transaction and the outcome set (`Applied`) is defined
once, at I3-4c, with the fixtures regenerated at I5-1 under the version bump rule. Delete
`RecordedNotApplied` as an outcome name, or restrict it to a test-only build flag recorded in the
validation file.

## MINOR

1. Dependency edges missing from section 4.1: I3-4a on I3-3a (verification percept is authenticated
   input); I3-4c on I3-3c (the backoff timer wake is its scheduler).
1. 8.P5-2 (audit of mesh receipt semantics) is scheduled in I3-4b. The audit decides what V-D may
   assume, so it belongs in V-D. The spec cites `agent_executor.cpp:1160-1180` and `:1284-1292`; at
   HEAD the ordinary-dispatch ignore of the turn outcome is near `:1715` and `:1755` (traceability
   row T-13 is right). E0 should refresh the citations.
1. T0's wake fix must cover `putBack` as well as `pushControl` (the working tree does), with a test
   that a posted control item wakes a blocked reactor.
1. Host-only coalesced charges (`Ingress::charges`) and the outcome log are dropped on resume and
   upgrade. State this as acceptable (they only delay a health charge) in I3-2D, and say charges are
   flushed at the upgrade point if cheap.
1. U-22 and T-12 are closed at T2, after the parity rule would require them earlier (M5). Re-point
   their last slice to I3-4b/I3-4c.
1. Per-pid quotas on control posts: a flood of control items can starve ingestion, since control
   items sit ahead of it. Bound them in I3-2c2.
1. Token retention (O3) must be at least the issuer pending record's maximum life, or legitimate
   in-flight verifications become `ExpiredVerificationTarget`. Record the relation in I4-D.

## Answers to the plan's questions for this persona

1. **Is the split at the right granularity?** The shape is right; the content needs M1 to M3 and M7
   first. The issuer side (retransmit, expiry, terminal handling, authentication of the returned
   outcome) has no owner. Put it in I3-4b and name both issuers: the host (operator and adapter) and
   an actor.
1. **Does the arena reset epoch remove the host counter?** No (M4). Two jobs: durable meaning
   (arena) and queue order (host).
1. **Is the quiescence rule complete?** No (M5).

## Ordering questions and owner decisions

| Item | Recommendation | My position                                                                                                                                                                                                                                                                                                      |
| ---- | -------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| A    | Option 2       | Agree, on the conditions that V-D is accepted before I4-D closes and that the I3-3a message format already carries the verification kind (M6). The transport slices do not need tokens; if V-D finds the executor changes large, split I3-4a at the token match and run the rest before I4 (the lead may decide) |
| B    | Option 3       | Agree (one write path). Makes M7 necessary: the transport is accepted with a real, non-learning write set                                                                                                                                                                                                        |
| C    | Keep I4        | No objection from DIST. Group admission through a grant must copy the audit provenance into the arena at admission (M2)                                                                                                                                                                                          |
| D    | Option 3       | Agree. T1's quiescence list needs M5, and the parity rule must also cover journals and queues                                                                                                                                                                                                                    |
| E    | Option 2, O1   | Agree. Owner must decide: it changes the 8.2 gate 0 claim                                                                                                                                                                                                                                                        |
| O1   | (b)            | Owner must decide (amends an approved acceptance gate)                                                                                                                                                                                                                                                           |
| O2   | (a)            | No DIST objection                                                                                                                                                                                                                                                                                                |
| O3   | (b)            | Agree: verification arrives in a later turn, and the anchor must be arena state to survive resume. Add the retention relation in MINOR 7                                                                                                                                                                         |
| O4   | (a)            | Agree. Lead decision if the signature stays in the application payload (M6); owner only for a mesh wire change                                                                                                                                                                                                   |
| O5   | Approve        | Agree. Add: verification replay floors live in the arena because the mesh floors are not checkpointed; refresh the 6.3 line citations                                                                                                                                                                            |
| O6   | As written     | No objection. A host-granted reference lost on resume leaves cold start `UNTESTED`, which fails safe                                                                                                                                                                                                             |
| O7   | (a) then (b)   | No objection. Journals, queue, ledger and pending records draw on the same cell ceiling; V-D must price them per entry                                                                                                                                                                                           |
| O8   | Arena audit    | Agree. The host log is lost on restart, so the arena list is the only durable record of `clear` and `reset`                                                                                                                                                                                                      |

Where host-only state is acceptable: bindings, aliases, grants, the ingress outcome log, queued
counters, coalesced charges, the host post ordinal and the reference grant, because losing any of
them can only cause a refusal or a delayed charge and must never change a cached outcome (M2). Not
acceptable: replay floors, deduplication, journals, cached outcomes, reset epochs, latches, issuer
pending records and retry attempt counts. All of these are arena state.

## Process economy

- I3-4b and I3-4c may be reviewed in one round after M1 to M3 are settled in V-D; they share the
  same executor path and the same reviewers. That saves a round with no weaker gate.
- The V-D design should include a one-page message sequence table (every message, every state
  change, every crash point) so the three persona reviews work from the same artifact.
- Missing gate: a crash-point test matrix for I3-4c (crash after intake, after processing, after
  response staged, after response sent) run against resume and upgrade; add it to the slice's
  acceptance.

Tally: 0 BLOCKING, 7 MAJOR, 7 MINOR.

Verdict: FAIL

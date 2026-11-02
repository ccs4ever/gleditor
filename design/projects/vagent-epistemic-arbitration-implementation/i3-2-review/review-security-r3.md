# I3-2 design review, round 3: security expert (persona security)

Scope: `i3-2-design.md` revision 3 (uncommitted, 1,136 lines), all sections, with the weight on 3.1
to 3.2 (what a hostile relay or replayer can do to the estimator), 5 (grants, tokens, requests,
ordering), 6.1 to 6.7 (audit, budgets, reserves) and 11 to 13. Also `decisions.md` section 1 and
`findings-r2.md`. Against P19 v1.5 sections 2.1, 4 (lines 431-482), 5.1 and 8.2. Code at HEAD
`8a4089d9` plus the uncommitted T0 change: `agent_executor.cpp` (`observe` at `:3566`, `Ingress` at
`:706`, `retireSource` at `:3503`), `compiler.cpp` (`emitProfileFor` at `:4691`, the retire walk at
`:4975-5000`). Calculations are in `/tmp/p19-security/calc3.py`. Nothing was built or run.

Attacker models, as in round 2: (a) an agent program, (b) a sensor, or a relay or on-path party that
holds recorded signed events, (c) a narrowly scoped grant holder, (d) a revoked or compromised
holder. The embedder is trusted (design 5.1 now says so).

## Earlier findings

All eight MAJOR and ten MINOR findings of round 2 are addressed except as noted. "Resolved" means
the section named closes the finding as I wrote it, or closes it by a recorded decision whose
consequence I checked.

| Round-2 finding                                     | Status                | Where and why                                                                                                                                                         |
| --------------------------------------------------- | --------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| M1 caller boundary, issuer unauthenticated          | Resolved (limit kept) | 5.1 names the five host-only entry points, makes `HostBindings` unable to reach them, and adds the P19 section 4 test. The embedder-trust ruling is recorded; see m4. |
| M2 token source                                     | Resolved              | 5.2: `RAND_bytes` 16 bytes, fail closed (`EntropyUnavailable`), SHA-256 digest, `CRYPTO_memcmp`, deleted formatter, tests for logs, checkpoint, 10^5 draws, zero.     |
| M3 self-certification bypasses                      | Resolved (m1)         | 5.2 enforces at grant, bind and revive, and turn time; canonical tokens; `everBound` host-wide; tombstones count. Residual points in m1.                              |
| M4 degrade-only holder erases audit                 | Resolved (M2, m5)     | 6.5: eviction by class, `count` merges, `actions_evicted`, the reset entry copies the fault, a sink. The sink's contract is thin (M2 below).                          |
| M5 pending bound lets one holder block isolate      | Resolved (m6)         | 5.3: per-grant bound 2, total 16, a reserved lane of 4, `control_per_wake`. A per-grant rate or action budget is still absent (m6).                                   |
| M6 exemption unbounded in slots                     | Resolved              | 4.1 and 6.6: `slotReserve = arenaSlots / 64`, no-op allocates nothing, slots per maximal reset measured in 2c2, runbook.                                              |
| M7 reset epoch bumped before authorization          | Resolved              | 5.4: `ResetMark` splits charges at the call but supersedes only on commit; a failed, revoked or no-op reset supersedes nothing; checked twice.                        |
| M8 degrade-only grant consumes the ceiling          | Resolved in intent    | 6.6 charges a per-profile health reserve and makes `Degrade` ordinary. The arithmetic fails for tombstones (M3 below).                                                |
| m1 grant ID reuse                                   | Resolved              | 5.2: the label is `id@instance8`; the issuer is in every record.                                                                                                      |
| m2 grant bound, expiry, `kMaxHealthGrantLifetime`   | Resolved              | 5.2: purge of expired and revoked grants, 24 hours, steady-clock caveat recorded.                                                                                     |
| m3 `code` charset, reserved names                   | Resolved              | 5.3: `[a-z0-9_.-]{1,64}`, reserved `refusal_fraction`, `rejection_streak`, `fault`.                                                                                   |
| m4 issuer and grant exposure                        | Resolved              | 13 (I4): scored results omit issuer, grant and operator codes.                                                                                                        |
| m5 revoke by ID                                     | Resolved              | 5.2: returns the number of queued requests cancelled; the linearisation point is written.                                                                             |
| m6 `SourceFaulted` is an oracle                     | Resolved              | 6.5: accepted, not charged, not logged per event.                                                                                                                     |
| m7 discarded belief events silent                   | Resolved              | 4.1: `SettleReport` in the outcome, `stat()` count, names.                                                                                                            |
| m8 request field validation                         | Resolved              | 5.3: `InvalidPrincipal`, `InvalidHealthCode` for all three code-bearing kinds.                                                                                        |
| m9 reset as a separate grant bit                    | Resolved              | 10: recorded as an open choice, not built. Acceptable for an optional item.                                                                                           |
| m10 `bindSource` and `unbindSource` unauthenticated | Resolved              | 5.1 puts them on the same footing and says the embedder is trusted.                                                                                                   |

## What is sound

- The token model is now complete and fail-closed. The grant table keeps digests only, lookup is
  constant-time over at most 16 entries, the check runs at the call and again at the turn by digest,
  and the linearisation point is written (5.2, 5.3). A revoke-then-regrant under one ID cannot
  authorize a queued item, because lookup is by token and not by label.
- The self-certification rule is enforced where the bypasses were: grant (issuer in `sources`, in
  `everBound`, or with a profile), bind and revive (issuer of a live grant), and turn time. Making
  the rule host-wide and on canonical tokens removes the case-aliasing and the cross-actor cases I
  raised. I traced the order of operations in both directions (grant then bind, bind then grant) and
  each is refused.
- The reset design no longer has a side effect on refusal. At call time `ResetMark.requested` only
  splits the charge, so a reset that is revoked, stale or a no-op loses no reading and no evidence
  (5.4). `Reset` naming the epoch it clears (`expectResetEpoch`) makes a retry or a replay by a
  second holder refuse `StaleHealthState` durably, because the epoch is arena state (6.3).
- Eviction by class in the audit window (6.5) closes the specific attack: a degrade-only grant can
  neither evict an isolate or reset entry nor add one beyond its own repeats, and the live fault's
  entry cannot be evicted while it is the fault.
- The settle-after-commit split (4.1) keeps the security-relevant property I praised in round 2: a
  sensor that makes its own `-pred` handler fail cannot block the operator's repair. Per-event
  settle turns also stop one failing event from silencing the others. The executor-side discard
  needs no VM budget, so it cannot be starved.
- The slice split puts the whole authorization surface in 2c1 against a stub that changes no state
  (5.5). That is the right order for review: the grant, token, ordering and bound logic can be
  tested for abuse before any turn can isolate or reset.
- The reserved lane (`pending - 4` cap on `Degrade` and `Clear`) is the right shape: a degrade-only
  holder cannot keep `Isolate` from queueing, and the test names that case.

## BLOCKING

None.

## MAJOR

### M1. Slice 2a: a recorded signed event can be replayed to move `R` and `N` without bound

Problem. Section 3.1 argues that relayed copies count only for refusals "decided on signed fields",
so that "a hostile relay could [not] make its victim SUSPECT". Section 3.2 makes the estimator count
each (incarnation, event ID) once. The once-only memory is `Logged::counted`, an entry in the
per-source outcome log of `outcome_log` (64) entries (`agent_executor.cpp:706-745`, `Ingress::log`).
Section 3.2 accepts that "an event whose log entry was evicted can count twice; that is the bound on
the error." That bound is true for an honest device and false for a replayer:

- Every check that decides a health refusal in `observe` (`ObservationFrameMismatch`,
  `MissingVariance`, `InvalidVariance`, the new `InvalidValue`) reads only signed fields. The
  unsigned fields (`acquiredAt`, uncertainty, clock epoch and mapping, `relayedBy`) are exactly the
  ones `signedFields` leaves out (`agent_executor.cpp:3677-3686`), and they are the ones the time
  checks at `:3654-3657` use. A relay chooses them, so it can give an old recorded event a fresh
  `acquiredAt` and pass the time checks (`StaleObservation` is not a health refusal anyway).
- The signed fields are valid however old the event is. Nothing in `observe` compares the event's
  `sequence` with anything. The sequence check is in the arena, after the queue, and a health
  refusal is decided before it.
- So a relay (or any on-path party that recorded signed events from the wire) holding 65 distinct
  signed bad events of the victim can replay them cyclically. Each replay meets an evicted log entry
  and charges `R` and `N` again. Replaying a 65-event cycle holds the victim at SUSPECT for as long
  as it likes after the device is repaired. The relay needs no key and gains nothing the device did
  not once sign.
- The pass credit (3.2) is the same weakness in the other direction. A replayed old **good** signed
  event that is dropped by `IngressBackpressure` is credited to `N` once per eviction cycle. A relay
  that fills a source's queue share with replays of old good events lifts `N` and lowers `f`, which
  masks a faulty sensor from SUSPECT. The design adds the credit for a legitimate reason (load
  bias), but it now lets a relay buy trust for another party's device.

Evidence. `agent_executor.cpp:3566-3800` (`observe`), `:706-745` (`Logged`), the design text at 3.1
and 3.2 quoted above. The existing code charged these replays too, but only into diagnostic counters
nothing read. Rev 3 makes them drive the state that I4 inflates and that the latch decision is based
on. No test in section 3.8 replays an old event after the log has cycled.

Consequence. The security argument stated for the signed-basis rule does not hold. A relay can force
a healthy source SUSPECT or shield a failing one, with only events the device already signed. The
SUSPECT state is "for display, the latch and audit" (3.3), and I4 builds eligibility on `f`.

Required fix. Make "new evidence" a property of the event, not of the log. Keep a host-side
high-water mark per (actor, principal, incarnation, predicate) of the largest `sequence` that has
updated the estimator (the 2c1 design already adds the structure `Ingress::admittedSeq`; hoist it
into 2a). A health refusal and a pass credit update `R`/`N` only when the event's `sequence` is
above the mark, then raise the mark. An event at or below it is a replay or a reorder and updates
nothing; it still refuses as it does today and may still feed the lifetime diagnostic counters. The
log's `counted` flag stays as the per-event guard inside the window. State the price: a device whose
counter regresses (a lost counter) is not counted until it exceeds the mark, which errs toward trust
and is the case I3-3's `sequence_regression` diagnostic is for. Add tests: a relay with an archive
of 65 and of 1,000 distinct signed bad events replays them for 10^4 steps and `f` stays at the
device's true fraction; the same with old good events under a stalled pump and `f` does not fall; a
restart of the executor clears the mark (host memory), so say whether the first event after a
restart is counted, and bound the replay that this allows to one cycle of the archive.

### M2. Slices 2c1 and 2c2: the audit sink is the record of truth, and its contract is unspecified

Problem. Round 2 M4 asked for a durable audit callback "and say it is the record of truth ... State
its delivery guarantee." Section 6.5 adopts the callback and says "the durable record is the audit
sink ... Durability is the sink's job". What it leaves open is what makes an audit sink safe to call
from the pump:

1. **Context.** It is "called on the pump thread after a health turn finishes and before its outcome
   is visible". The design does not say whether `ingress_->mutex` (a non-recursive `std::mutex`,
   `agent_executor.cpp:740`) or any other lock is held. If it is, a sink that calls `healthOutcome`,
   `sourceHealth`, `revokeHealthGrant` or `healthAction` (the natural thing for an operator console
   that reacts to an isolate) deadlocks the pump for every actor. If it is not, the design must say
   which state the sink may read.
1. **Blocking.** The sink runs on the pump thread, so a sink that writes to a network or a full disk
   stalls every actor in the executor. The design says a throwing sink is caught and logged and
   nothing about one that is slow.
1. **Install.** `setHealthAuditSink` replaces a `std::function` that the pump may be calling. With
   no stated lock or "before first wake" rule this is a data race, and replacing the sink is also
   the way to turn the audit off.
1. **Failure is silent.** The sink returns `void`. The committed isolate or reset is already in the
   arena when the sink runs, so an operator who needs "no isolate without a durable record" cannot
   have it, and cannot even learn that a record was lost: there is no counter of sink failures and
   no `stat()` entry. The design's own justification for the sink is that the arena `actions` window
   is only recent history.
1. **Content.** The sink receives the entry, the outcome and the `removed` text. The design should
   say it never receives the token or its digest.

Consequence. The only durable record of who isolated or reset a source has no stated threading,
failure or ordering contract. The accountability that M4 of round 2 asked for cannot be tested and
cannot be relied on, and the first embedder that does the obvious thing in a sink can hang the host.

Required fix. State in 6.5, and test in 2c1 (the sink exists there and receives the stub's `Refused`
outcomes): (a) the sink is called with no executor lock held and may call the read-only host API and
`revokeHealthGrant`; (b) it must not block, and the contract is a queue handoff, with a documented
recommendation; (c) `setHealthAuditSink` is serialized with delivery (take the lock the pump takes
to read it) and a null sink is allowed and logged at WARN; (d) delivery is at most once, in commit
order per actor, and the executor counts calls, throws and (if the signature returns a status)
refusals, in a `stat()` entry and in `SourceHealth`; (e) nothing in `HealthAudit` is secret. If the
owner wants "no durable record, no isolate", that is a different, stronger design (sink before
commit) and should be asked (see the question below).

### M3. Slice 2c2: the per-profile health reserve counts tombstones, and nothing bounds them

Problem. Section 6.6 charges the reserve "per existing profile, tombstones included":
`cellsWithin(all) + sum over profiles of (healthBound - health_cells) <= cells`. Three facts make
this unsound.

1. **Tombstones have no bound at HEAD.** `emitProfileFor` counts only live principals against
   `maxSources` ("a tombstone keeps its replay floor but no share", `compiler.cpp:4684-4688`,
   `:4701-4713`). `retireSource` (`agent_executor.cpp:3503-3531`) creates a tombstone for any
   well-formed principal, bound or not, and never fails for count. Every key rotation and every
   retired device leaves one for the life of the actor (6.5 says a latched tombstone "holds its
   profile ... for the life of the actor" and that the operator retires the old principal).
1. **Each tombstone now costs 186 cells permanently:** `kProfileBaseCells` 44 plus the full unused
   reserve of 142. The ceiling is 24,576, so 132 tombstones (`24,576 / 186`) use all of it. With 8
   live sources 124 tombstones leave no room for a reading; with 64 live, 68. After that every
   ordinary admission refuses `EpistemicBudget`, including the bookkeeping that records refusals and
   the `Degrade` that is meant to be safe. A checkpoint and resume does not help, because tombstones
   are state that survives.
1. **The cap the design adds contradicts the ceiling.** Section 6.6 and 6.2 bound exempt profile
   creation at `2 x max_sources` = 256 profiles. By the formula in the same section 256 profiles
   reserve 47,616 cells against a ceiling of 24,576. `validate()` checks
   `maxSources x (healthBound + kProfileBaseCells) <= cells` (23,808), which covers 128 profiles,
   not 256. Also at 128 **live** profiles the reserve leaves 768 cells (9 records) for all readings,
   so the bound "a profile can always reach its health bound" is bought by leaving nothing to read
   with. I computed these in `/tmp/p19-security/calc3.py`.

The round-2 finding (M8) was a lowest-privilege grant starving ingestion. Rev 3's reserve closes
that for health records and opens it for ordinary operation: an operator who rotates keys loses the
actor's ingestion capacity, and an isolate-only holder, which cannot create profiles for unbound
principals, still cannot be blamed; the cost lands on routine maintenance.

Consequence. The budget invariant of 6.6 cannot hold, the `2 x max_sources` cap is incoherent with
it, and a plain operational pattern (retire and rebind) ends the actor's ability to ingest. The 2c2
test "a full budget with every source isolated checkpoints" cannot catch this, because it never
builds tombstones.

Required fix. Pick one rule and make `validate()` and the test prove it in arithmetic:

1. Charge the full reserve to **live** profiles only (at most `max_sources`, as the validation
   assumes). A tombstone is charged the `health_cells` it actually holds and nothing more.
1. A health action on a tombstone is exempt and bounded by the headroom, or refused with
   `NoProfile`; say which. An isolate of a retired principal is meaningful (a latched identity), a
   `Degrade` of one is not.
1. Bound tombstones. Either `retireSource` refuses past `max_sources` tombstones with a typed
   refusal (a recorded departure from HEAD) or the oldest unlatched tombstone with no current record
   is reclaimed by the retire turn. Delete or restate the `2 x max_sources` cap in terms that fit
   the ceiling.
1. Test: 200 retire-and-rebind cycles on an actor with 8 live sources leave readings admitted; 128
   live profiles plus a full tombstone set checkpoint.
1. Say plainly that `max_sources = 128` with the default `cells` leaves almost nothing to read, and
   either raise the default `cells` (and `kMaxEpistemicCheckpointCells`) or lower the default
   `max_sources`; this is a lead decision, not mine, but the design must not present 128 sources as
   supported.

## MINOR

- **m1. Self-certification: lock, atomicity and growth.** Section 5.2 says the three checks happen
  but not that grant and bind take the same lock. Say that `grantHealth`, `bindSource` and the
  revive path each run the check and the change under one `ingress_->mutex` hold (as `retireSource`
  and `unbindSource` already lock, `agent_executor.cpp:3503`, `:3531`), so a concurrent bind and
  grant cannot both pass. `Ingress::everBound` only grows: a host that binds and unbinds many
  principals over time grows it without bound (64 bytes each). Bound it (for example by the number
  of principals ever bound per actor times the actor count, or by keeping the set only for
  principals the arena has a profile for) or say it is unbounded and why that is acceptable. After a
  restart `everBound` is empty while another resumed actor's arena still holds the profile; the
  bind-side refusal still holds (it reads live grants), and the turn-time check reads the target's
  profiles, but add a test of "grant for actor A, issuer is a profile on actor B, after resume".
- **m2. Order of checks in `healthAction`.** Say that authentication (token digest, then principal
  and kind) runs before request validation, `UnknownActor` and `IngressBackpressure`, so an
  unauthenticated caller learns nothing about which actors exist, how full their queue is, or which
  code names are reserved. All five are host calls, but the P19 section 4 test ("no token, valid
  transport signature") should assert the refusal is the same `UnauthorizedHealthAction` whatever
  else is wrong with the request. Say whether `HealthBatchRequest` is all-or-nothing at the call
  when one principal is outside the grant, and what each item does when the grant is revoked
  mid-batch.
- **m3. Token residency.** The queued item carries the token (`HealthActionRequest { token; ... }`).
  Store only the digest in the queued item and the pending map, so the secret is not copied into the
  post box, `hostEvents` or the outcome ring. State how an embedder obtains a token from bytes
  (`HealthGrantToken::fromBytes` with exactly 16 bytes, no zero-length or default form) and that the
  class clears its bytes in the destructor (`OPENSSL_cleanse`). The "zero token" test in 5.6 needs
  that constructor to exist.
- **m4. The host-only list and the issuer.** The 5.1 source test greps for five names. The executor
  also has `sourceHealth`, `sourceHealths`, `setHealthAuditSink`, `bindSource`, `unbindSource` and
  `retireSource`, all of which a native skill must not reach, and the first two return issuer and
  grant labels. Assert the structural property (a `HostBindings` or skill closure holds no
  `AgentExecutor` handle) and extend the name check to all of them. `HealthIssuer` checks only the
  format of a fingerprint, so say in section 13 that I3-3's adapter (O4) must authenticate the
  issuer on **every** request, before the token is presented, so that a leaked token alone is not
  the whole credential.
- **m5. Cheap actions can evict an operator's outcome.** The 64-entry per-actor ring that holds
  finished health outcomes is shared by every grant. Sixty-four `NoChange` or `Refused` outcomes
  from a degrade-only holder push out an isolate's outcome, and the operator then sees `Unknown`,
  the same as a request that never ran. A retry is safe (a repeated isolate is `NoChange`, a stale
  reset refuses) and `sourceHealth` shows the state, so this is not an integrity problem. Do not put
  `NoChange` outcomes in the ring, or partition it per grant, and say that `Unknown` after a long
  gap means "read `sourceHealth`".
- **m6. No per-grant rate or action budget.** A holder with `Degrade` and `Clear` can alternate them
  without limit, each a real turn with an action record. `Degrade` is an ordinary entry, so it stops
  at `ArenaExhausted` (`arenaSlots`), but that is the arena exhausting for everyone. At 60 to 250
  slots a pair (`/tmp/p19-security/calc3.py`) 2^30 slots is 4.3 to 17.9 million pairs, 1.2 to 25
  hours at 200 to 1,000 turns a second, inside a 24 hour grant, and it keeps `pendingFor` true so
  `checkpoint` and the upgrade refuse. Revoking the grant is the remedy and the sink shows the
  abuse. Add a cheap `maxActions` to `EpistemicHealthGrant` (committed turns over the grant's life)
  or a steady-clock rate, so the remedy is not the only control. This supersedes the rate-limit
  request of round-2 M5, which rev 3 did not take and did not decline.
- **m7. The reset sequence floor can strand the source.** 6.4 raises each stream floor to the
  highest sequence "the host admitted", and 5.4 defines `admittedSeq` as raised for every reading
  that passes the checks in `observe`, including readings the arena later refuses. A source (not a
  relay: `sequence` is signed) can send one reading with a very large `sequence` and make the next
  reset set its own floor there, locking itself out of every smaller sequence until a higher
  incarnation. The effect is on the sender alone, but a bound sensor that is merely buggy gets it,
  and the operator who reset it gets a source that cannot recover. Raise the floor only to the
  highest sequence queued **and** not later refused by the arena for a reason that makes the number
  untrustworthy, or cap the jump (a setting, `max sequence gap`), and say which.
- **m8. The runbook cannot checkpoint while health items are queued.** 6.6's runbook says isolate,
  `checkpoint`, `resume`. `checkpoint` refuses while `posts_->pendingFor(pid)`
  (`agent_executor.cpp`, `checkpoint`, now shifted by T0) and a holder or a flooder keeps that true.
  Add "revoke every grant on the actor first (this cancels queued requests, 5.2), retire or unbind
  the flooder (m23)".
- **m9. Reset calls split charges without committing.** Each `healthAction(Reset)` increments
  `ResetMark.requested` and so opens a new charge generation (5.4), even if the call ends
  `StaleHealthState` or `NoChange`. A reset-holder that loops (bounded by the pending bound and the
  calls it can make) multiplies bookkeeping notes. Increment `requested` only for a reset that
  `wouldChange` accepts (that test runs on the executor's snapshot, 6.3), or cap the generations an
  actor may have outstanding per principal.

## Questions for the owner

1. Should a committed isolate or reset require a durable audit record (the sink acknowledged before
   the commit), or is the current "record after commit, best effort, counted" acceptable? M2 asks
   only that the design says which, and tests it.
1. What should happen to a retired principal's health state: is a tombstone ever allowed to be
   isolated or degraded after the retire, or does `retireSource` end the principal's health history?
   M3 point 2.
1. Is a relay allowed to supply `acquiredAt` and uncertainty (that is the design's current model),
   or should they be signed by the origin, as the sequence is? Signing them would also close M1 and
   give I3-3 its path-lateness separation.

## Slice verdicts

- 2a: **FAIL**. M1 (replay). The rest of 2a (estimator, derived state, settings) has no open
  security finding.
- 2b: **PASS**. No security finding. The settle and discard contract and the ingestion wedge
  residual (A4) are sound; the wedge is reachable only by the program's own handler.
- 2c1: **FAIL**. M2 (audit sink contract). Grants, tokens, ordering and bounds are acceptable once
  the minors (m1 to m4) are recorded or fixed.
- 2c2: **FAIL**. M3 (tombstone reserve); M2 applies to its sink calls. The idempotency, epoch and
  audit-eviction design is sound.

## Fit to later iterations

- **I3-3.** The sensor-originated health path must also use the sequence high-water mark of M1: a
  replayed device report must not renew a warning. Sensor warnings stay a separate path with
  `device` origin, as 13 says.
- **I4.** The directive lowering must refuse everywhere except through a token at the host edge (13
  says it); add that a handler entered through an authenticated operator percept is the only place a
  directive can run. The sink record, not the program, is the audit authority.
- **Translator (T1).** Section 13 lists what to carry. Add the replay mark (host state, not carried,
  so a resume starts a cycle, M1) and the sink (host state, reinstalled).

Tally: 0 BLOCKING, 3 MAJOR, 9 MINOR.

Verdict: FAIL

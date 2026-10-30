# I3-1 design: checked ingress and per-source observation history

Status: revision 5. Revision 4 passed the ML gate and the compiler/codebase review; epistemology and
hardware raised two MAJOR findings each, resolved here. Revision 1 failed all four reviews. In
revision 2 only the ML gate passed. Revision 3 failed all four reviews, with one codebase blocker;
the ML gate let slice 1a proceed. Section 12 maps every finding to its resolution. Authority:
approved P19 v1.5 sections 2.1, 2.2, 4, 5.1 and 6.3, with the
[I2 scope ABI](../../vortex-scoped-evaluation-abi.md) as its read path. Baseline: `7a2dd07d`.

## 1. Scope and split

I3-1 makes an authenticated observation enter an actor once, atomically, and land in the section 2.1
topology, so that I4's Vortex scoring graphs read real records. It ships in four slices, each with
its own gate:

| Slice | Content                                                                                                         |
| ----- | --------------------------------------------------------------------------------------------------------------- |
| 1a    | `arbitrated by` syntax, checker and policy master; static refusal of every other clause writer; policy schema   |
| 1b    | grants, `observe`, typed turn results, the ingestion routine with own-source transfer, dedup, budgets, outcomes |
| 1c    | incarnation advance turns and supersession, gated separately but shipped with 1b                                |
| 1d    | checkpoint and resume, preceded by a restore spike                                                              |

1b starts with the turn-result spike (section 5) and 1d with the restore spike (section 10).

Not in I3-1: gates and verdicts other than `UNTESTED`, calibration records, decay, learning, labels,
verification, health actions, heartbeat turns, queries, query tokens, cross-key retained transfer,
and live-upgrade translation (section 11).

## 2. Surface syntax and checker (1a)

```ebnf
PredicateDecl = 'predicate' Ident '(' Params ')' [ 'arbitrated' 'by' Strategy '(' PolicyArgs ')' ] ;
Strategy      = 'truest' | 'ranked' | 'consensus' ;
PolicyArgs    = PolicyArg { ',' PolicyArg } ;
PolicyArg     = Ident ':' Literal ;
```

`by` is the reserved `TokenKind::By`. `arbitrated` and the strategy names are contextual
identifiers, recognised only in this position. A closed key table in a new
`vagent/runtime/epistemic_policy.hpp` drives the checker, which produces an
`std::optional<ArbitrationPolicy>` on `PredicateDecl` and `PredicateInfo`. It refuses
`InvalidEpistemicPolicy` for an unknown or duplicate key, a wrong literal type, or a value outside
section 8.1's domains.

Required keys:

- `model`: I3-1 accepts only `"beta_reputation"`.
- `min_confidence`: in [0,1].
- `prior_alpha` and `prior_beta`: both > 0, so I4 never invents a prior.
- `clock`: the acquisition time basis, either `"wall"` or `"actor_mapped"`. `actor_mapped` times are
  seconds on the executor's steady timeline, mapped by the host, and need no wall clock.
- `max_age`: a positive duration. An acquisition older than receipt by more than this refuses
  `StaleObservation`, so an old, genuinely signed event can never enter as fresh evidence.
- Continuous predicates also need `cluster_tolerance` > 0, `frame`, `units`, and `min_variance` and
  `max_variance` in squared units. A predicate is continuous when it has any non-key Float
  parameter.
  - The variance bounds must satisfy 1e-150 ≤ min ≤ max ≤ 1e150. Then even a sum of `max_candidates`
    reciprocals stays finite, with headroom for I4's inflation, which must rescale its sums rather
    than rely on these bounds alone.
  - A missing `cluster_tolerance` refuses `MissingClusterTolerance`; any other missing key refuses
    `InvalidEpistemicPolicy`.
  - Continuous-only keys on a discrete predicate, and `consensus` on a discrete predicate, refuse
    `InvalidEpistemicPolicy`.
- Optional keys, each with its domain; absent ones have no field except where a resolved default is
  stated:
  - `clock_skew`: non-negative duration, resolves to 0.
  - `max_uncertainty`: positive duration at most `max_age`, resolves to `max_age`. A claimed
    acquisition uncertainty above it refuses `UncertainAcquisition`, because the uncertainty widens
    both the age and the future window (section 14).
  - `decay`: positive duration, the freshness half-life of P19 section 3.2.
  - `alignment`: non-negative duration.
  - `score_tolerance`: finite and ≥ 0, resolves to 1e-9 (P19 section 5.3), so the hash records the
    value in force.
  - `allow_untested`: Bool, resolves to false; true lets truest/ranked expose cold-start hypotheses
    and is refused for consensus.
  - Continuous only: `max_cluster_diameter` (finite, ≥ `cluster_tolerance`), `max_slew` (> 0, in
    units per second) and `innovation` (> 0, the gate quantile).
- Global bounds: priors ≤ 1e12; durations ≤ 10^12 ms (about 31 years), so I4's age arithmetic stays
  in Int64; an integer literal widens to Float only up to 2^53, where the double is exact; a leading
  minus is part of a numeric or duration constant, so negatives reach the domain rules.
- Keys I4 needs that 1a does not decide (epsilon_w, epsilon_floor, feature names, health thresholds)
  arrive with their slices. Each addition bumps the canonical encoding tag `vagent-policy-1`, so
  older hashes never compare equal to newer ones.

**Arbitrated facts follow ingestion semantics, not belief-set revision.** On an arbitrated predicate
the ingestion routine is the only clause writer. The checker refuses `EpistemicIngressOnly` for:

- the `believe`, `forget` and `revise` statements (all of `EBeliefChange`) and keyed `believe`;
- `believes … from <expr>`, because supports are named by authenticated principal and an alias must
  not match silently. Source selection returns in I4 through an authenticated binding.

`rtBSet_` has exactly two call sites, and `rtBDrop_` three plus the retire turn: `beliefChange`
(`compiler.cpp` near line 4584), which also lowers every belief an `Initialize` handler or other
handler states, and the peer `tell` bodies (near line 4757). Percepts and host deliveries are
events, never beliefs, and there is no separate load-time fact or rule-head path in the actor
dialect. The compiler emits no `tell`/`untell` test for an arbitrated predicate, so a peer's tell is
counted unhandled; a test asserts this. Both call sites also assert that they never reach `rtBSet`
for an arbitrated predicate, as a compiler-side guard for any future emission site. Restore and
upgrade copy existing structure and never call `rtBSet`. Queries and `+pred`/`-pred` handlers still
work.

The policy is lowered once per program as a policy master record on the predicate descriptor's
`d.epistemic` rank, with typed scalar cells. `EpistemicPolicySchema` in `upgrade_schema.hpp` is
populated with the full typed policy and a canonical policy hash (new work, alongside the plan
`astHash`). A changed policy is then a typed upgrade change and a typed restore mismatch.

## 3. Identity, grants and provenance (1b; incarnations in 1c)

A source is an authenticated origin principal: the **origin signer**, never the transport peer.
`CheckedObservation` carries no signature. The host's ingress adapter verifies the origin signature
before calling `observe`, so `principal` and `relayedBy` are claims of that trusted host boundary,
and the executor trusts them only as far as it trusts its host. The identity token is the lowercase
hex fingerprint, validated as 64 hex digits.

Supports and profiles are keyed by principal. The incarnation is a field of the profile and of every
record, not part of the support name. One principal has at most one support per key, whatever its
incarnation.

```cpp
struct EpistemicSourceGrant {
  std::string alias;      // routing and display only
  std::string principal;  // 64 hex digits
  std::uint64_t incarnation;
  std::string predicate;  // one arbitrated predicate per grant
  std::chrono::steady_clock::duration lifetime; // from binding, on the executor's steady clock
};
```

The executor's binding calls (each returns `std::expected<AgentExecutor *, EpistemicRefusal>`):

- `bindSource(pid, grant)` refuses an alias bound to another principal, a second alias for the same
  (principal, predicate), an unarbitrated predicate and a malformed principal.

- `unbindSource(pid, principal, predicate)` revokes authorization only.

- `retireSource(pid, principal)` is the operator's decommission path. It revokes every binding of
  that principal and queues a retire turn, which:

  - marks the principal's current records `retired`;
  - drops its supports through the ordinary belief path, settling `-pred`;
  - excises unpinned history, the dedup window, streams and the profile.

  The retire turn keeps a **tombstone**: principal, highest incarnation seen, and a retired flag,
  about ten cells charged to `$epistemic_cells`. A later `bindSource` for that principal must
  present a strictly higher incarnation, so the replay floor survives retirement. A principal with a
  pinned record also keeps those records until I3-4 can release the pin. The rest of the logical
  budget is returned; physical slots are not (section 9).

Bindings are authorization state in the executor under their own mutex, never in the arena. Grant
lifetimes run on the steady clock, so a wall-clock step cannot expire or extend them.

**Incarnations (1c).** A device that restarts its sequence counter, or might reuse event IDs, must
present a higher incarnation. The device or its host identity layer owns that boot counter.

- Incarnations per principal are strictly increasing. A lower one, at binding or at ingestion,
  refuses `StaleIncarnation`.
- Binding a higher incarnation supersedes the lower binding atomically and queues an **advance
  turn**. That turn sets the profile's incarnation and marks every record of a lower incarnation, on
  every key, `retired`. A silent replacement device therefore never leaves its predecessor's
  readings current. The turn also clears the dedup window and resets the streams. Its cost is
  bounded by `keys_per_source` times the history.
- Only `bindSource` advances an incarnation. An observation must carry exactly its binding's
  incarnation; any other refuses `StaleIncarnation` (lower) or `UnboundIncarnation` (higher), so a
  corrupt boot counter or one signed outlier cannot retire a source's history.
- The advance turn is queued ahead of that actor's pending ingestions and queries, so nothing runs
  between supersession and the advance.
- Latched `FAULTED` and degradation reasons live on the profile, so a restart cannot shed them.
  Their reset authority arrives with I3-2.
- I4 eligibility requires a record to be `current`, with its incarnation equal to its profile's. The
  profile is resolved once per support, not once per record, so the check is one integer comparison
  per record after one clone-rank walk per support.
- Calibration records keyed by a lower incarnation stay on the profile. I4 must define their expiry
  together with the reset rule of P19 section 5.1.

Until 1c lands, a grant's incarnation is fixed and any other refuses `UnsupportedIncarnation`.
Because a rebooted device cannot then recover, 1b ships only together with 1c, gated separately.

## 4. Ingress and outcomes (1b)

```cpp
struct CheckedObservation {
  std::string principal; std::uint64_t incarnation;  // origin signer, host-verified
  std::optional<std::string> relayedBy;              // transport principal when relayed
  std::string eventId;   std::uint64_t sequence;     // origin event ID
  std::string predicate; WireRecord tuple;
  double acquiredAt; double acquiredUncertainty;     // mapped seconds, policy clock basis
  std::string clockBasis; std::uint64_t clockEpoch, clockMappingVersion;
  std::string frame, units;
  std::optional<double> variance;                    // squared units
};
AgentExecutor::observe(pid, CheckedObservation)
    -> std::expected<TransportReceipt, EpistemicRefusal>;
AgentExecutor::ingestOutcome(pid, principal, incarnation, eventId) -> std::optional<IngestOutcome>;
AgentExecutor::setWallClockSynchronised(bool) -> AgentExecutor *;
```

The executor stamps two receipt times, and the host supplies neither them nor the digest:

- `receivedSteady` comes from `clock_.steady()`. It drives liveness, contact times and lease and
  grant expiry, and is the receipt time an `actor_mapped` acquisition is compared with.
- `receivedWall` comes from `wallNow()`. It is compared only with a `wall` acquisition.
- Steady times are tagged with the executor's clock-instance ID, so a restore on another executor
  can rebase or invalidate them (1d); they are never compared across instances.
- The digest is SHA-256 over the canonical bencoding of the origin-signed fields: principal,
  incarnation, event ID, sequence, predicate, tuple, frame, units and variance. Host-mapped times
  and `relayedBy` are excluded, so two relays of one event share a digest even if they mapped its
  clock differently; the first mapping committed is kept.

Detecting a wall-clock step or an unsynchronised boot is the host's job (its time service). While
the host reports the wall clock unsynchronised, an observation on a `wall`-basis predicate refuses
`ClockUnsynchronised`; `actor_mapped` predicates are unaffected, so an offline or RTC-less node
still ingests them. The refusal is retryable, clears when the host reports synchronisation, and is
never charged to the source.

Before queueing, `observe` checks everything that does not need the arena:

| Refusal                                   | Cause                                                                                                                                     |
| ----------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------- |
| `UnknownSource`                           | no live binding for (principal, predicate)                                                                                                |
| `StaleIncarnation` / `UnboundIncarnation` | below or above the binding's incarnation (1c); `UnsupportedIncarnation` before 1c                                                         |
| `InvalidObservation`                      | a uint64 that does not fit Int64, a wrong tuple arity or type, a non-finite time, negative uncertainty                                    |
| `ClockUnsynchronised`                     | a `wall`-basis predicate while the host reports the wall clock unsynchronised                                                             |
| `ObservationTimeMismatch`                 | another clock basis; acquisition after the matching receipt time by more than uncertainty plus `clock_skew`                               |
| `UncertainAcquisition`                    | a claimed acquisition uncertainty above `max_uncertainty` (a content refusal)                                                             |
| `StaleObservation`                        | acquisition before the matching receipt time by more than `max_age` plus uncertainty                                                      |
| `ObservationFrameMismatch`                | frame or units that differ from the policy                                                                                                |
| `MissingVariance` / `InvalidVariance`     | absent on a continuous predicate, present on a discrete one, outside \[`min_variance`, `max_variance`\], non-finite                       |
| `IngressBackpressure`                     | the actor queue or this principal's share is full; the queue drains round-robin by principal, so a babbling source cannot starve the rest |

Every refusal, at `observe` or in the turn, also posts a `RefusalNote` to the actor's post box.
Whether the principal has a profile to charge is decided later by `drainPosted` on the worker
thread, which owns the arena.

Outcomes are typed `Committed`, `Duplicate` or `Refused{code, class}`, and the outcome log is keyed
by (principal, incarnation, event ID), one log per source (section 14).

- **Terminal:** `StaleSequence`, `ConflictingEvent`, `StaleIncarnation`, `UnknownSource`,
  `StaleObservation`, `EpistemicKeyLimit` and every content refusal. The host must not resend the
  same bytes.
- **Retryable:** `IngressBackpressure`, `ClockUnsynchronised` and `Undelivered`, which clear by
  themselves.
- **Needs host action:** `EpistemicBudget` (cell ceiling or `max_sources`) clears only after
  `retireSource` or a configuration change; `ArenaExhausted` clears only after a checkpoint and
  resume (section 9); `UnboundIncarnation` after the host binds the new incarnation; `TurnExhausted`
  after the grant grows, because the same bytes on the same state cost the same. The outcome names
  the action.
- A failed turn that set no `$turn_result` (statement failure, settle depth) is `Refused` with code
  `IngestionFault`, class needs-host-action: it indicates a program or runtime defect, not data.

`TransportReceipt` means queued, never committed. The outcome log (`vagent.epistemic_outcome_log`)
is a bounded host log of execution outcomes, not learned state. Ingestion events are host
deliveries. They drain before a checkpoint is accepted, and the queue bound caps how long a flood
can delay one.

## 5. Typed turn results (1b, shared turn path)

A refusal cannot use the VM's `Fail`. The turn's base choice point records an arena mark, and
backtracking releases it before control reaches `failOp`, so anything Vortex wrote to explain the
refusal is gone. Instead:

- The routine writes the reserved `$turn_result` field (a symbolic `Str` from a closed set) and
  jumps to `haltOp`.
- `runTurnOnce` reads `$turn_result` on the halt path, records it in a new `TurnOutcome::result`,
  and then:
  - commits for `Committed`;
  - releases the turn's mark `m0` itself for `Duplicate` and for every refusal, so a duplicate also
    commits nothing.
- A turn that does not set the field behaves as today. Fuel and cycle exhaustion keep the generic
  failure path, whose outcome is a retryable refusal.

The routine reaches halt from inside nested internal calls, so 1b begins with a spike. The spike
shows that a jump to `haltOp` with a non-empty call stack is accepted and leaves no stack frames
behind, or it adds an explicit unwind. This is an adapter change to the shared turn path, priced as
part of P19.5's intake journals; it adds no VM mechanism.

## 6. Ingestion turn (1b)

Each queued observation runs in its own wake turn under the actor grant, through a new
`observeEntry` beside `dispatchEntry`. Its inputs travel in `TurnSpec::turnFields`: the metadata
record (Double times and variance, Int64 sequence and epochs), the identity token, and the budgets.
The adapter first re-checks the binding (`UnknownSource` if it expired or was revoked while queued).
Then the generated `rtObserve` routine runs, in this order:

1. **Profile.** Find the profile on `$agent +d.trust` by token. If it is absent, create it only
   within `max_sources` and the cell budget (step 9), else refuse `EpistemicBudget`.

1. **Incarnation (1c).** The observation's incarnation must equal the profile's; any other refuses
   (`StaleIncarnation` below, `UnboundIncarnation` above). Because `observe` already requires it to
   match the binding and the advance turn runs first, the step is defensive. A rebooted device's
   reused event ID therefore never meets the old window, which the advance cleared.

1. **Dedup.** The window (`vagent.epistemic_dedup`, newest first) holds (incarnation, event ID,
   digest) triples and is cleared on advance. The same triple sets `Duplicate`, which refreshes and
   commits nothing, as P19 section 2.1 requires of an exact retry. The same (incarnation, event ID)
   with another digest refuses `ConflictingEvent`. An ID older than the window falls to the sequence
   rule, so the replay horizon is the window.

1. **Transport order.** A stream is (profile, predicate) and holds only the last sequence, clock
   epoch and mapping version.

   - A sequence not above the last refuses `StaleSequence`.
   - A clock epoch below the stream's refuses `ObservationTimeMismatch`. Within one epoch, a lower
     mapping version does too.

1. **Acquisition order, per key.** Compare against this principal's current record for the same key,
   if it has one in the same clock epoch. An acquisition earlier than that record's by more than the
   two uncertainties plus `clock_skew` refuses `ObservationTimeMismatch`. Different keys never order
   each other, so a multi-target sensor may report keys in any order. A new clock epoch resets the
   comparison.

1. **Support with own-source transfer.** `rtObserveSet` extends `rtBSet`'s matching with
   `rtFindClause`/`rtFindSrc` helpers and returns the support cell; the support's text is the token.

   - If this principal already supports another value for the same key, the old support's
     `d.epistemic` edge is broken (`breakOp`) and the history is linked onto the new support before
     `rtBDrop` drops the old one. One owner holds the history throughout, every edge stays
     reciprocal, and exactly one support exists per (principal, key).
   - A new key beyond `vagent.epistemic_keys_per_source` refuses `EpistemicKeyLimit`.

1. **Record.** Append a record, with an owned value cell per field:

   - event ID, observation version 1, the executor's digest;
   - token, incarnation, relay principal if any, and a profile clone occurrence spliced posward
     after the master, retaining the old successor;
   - predicate signature and a tuple view;
   - domain/schema ID (the predicate signature with the policy hash), model `beta_reputation`, model
     epoch 0 and calibration epoch 0;
   - feature schema 0 and an empty feature list;
   - sequence, acquisition time and uncertainty, clock basis, epoch, mapping version, and both
     receipt times;
   - frame, units and variance;
   - health at ingest;
   - verdict `UNTESTED` at verdict epoch 0, with an empty `verdict_revisions` list;
   - status `current` and pinned false.

   The previous current record becomes `retired`. The verdict is immutable; a later gate appends a
   revision with a higher verdict epoch.

1. **Stream and profile.** Update the stream, the dedup window and the profile's last receive time.
   Only a directly received (no `relayedBy`) committed event refreshes last contact.

1. **Bound and budget.** First excise the oldest unpinned retired records beyond
   `vagent.epistemic_history`, with their clone occurrences; current and pinned records are never
   excised. Then check the net increase against the ceiling. Every cell minted or excised, including
   dedup and list evictions and pinned records, adjusts a maintained `$epistemic_cells` counter on
   the agent. Admission refuses `EpistemicBudget` when the net increase would pass
   `vagent.epistemic_cells`.

1. **Settle and result.** Run `rtSettle_`, set `$turn_result` to `Committed` and halt. Settling runs
   only on this commit path, so `+pred`/`-pred` handlers never see a refused observation.

The adapter turns each `RefusalNote` for a principal that has a profile into a bookkeeping turn
through `observeRefusalEntry`. That turn records the code and one of two counters on the profile,
both kept apart from the gate `reject_count`:

- `content_refusals` counts frame, unit, variance and time refusals of events received directly. A
  direct content refusal also refreshes last contact, because the device demonstrably sent it.
  Health policy may use this counter in I3-2.
- `refused_count` counts every other refusal, and any refusal of a relayed copy. It is diagnostic
  only: it never feeds health and never refreshes contact. A replay or a hostile relay therefore
  cannot make a source look either faulty or alive.

`ClockUnsynchronised`, `IngressBackpressure` and `UnknownSource` are never charged. A principal with
no profile has nothing to charge; the outcome log still records it.

The routine's fuel is O(S + D + H + K) walks for S profiles, a dedup window D, history H and keys K
per source, all charged to the grant. The advance and retire turns enumerate a principal's records
by walking its profile master's `d.clone` rank, which holds one occurrence per record, and going
negward from each occurrence to its record and support: O(K·H) cells, charged to the grant they run
under.

`UNTESTED`, health NOMINAL and calibration epoch 0 mean **unscored, not endorsed**. I4 must treat a
record with no calibration record as ineligible, or refuse; it never reads these defaults as trust.

## 7. Why Vortex for the routine

Every write happens in the arena with the arena's undo. Clause coalescing and belief events already
exist as compiled Vortex, so a C++ copy would fork the semantics. C++ is limited to:

- verifying the origin signer's identity and the grant table;
- validating the numbers the host supplies;
- the receipt-time stamp and the payload digest;
- queuing, the outcome log, and reading `$turn_result`.

## 8. Profile and stream records

Each profile holds the fields later slices need, so they extend it rather than reshape it:

- **Identity:** token and current incarnation.
- **Health:** a `faulted` latch (Bool) and lists of degradation and suspicion reasons.
- **Counters (Int64):** rejection streak, gate `reject_count`, `content_refusals` and
  `refused_count`.
- **Times (Double):** last receive and last contact; the heartbeat interval is absent until I3-3.
- **Owned ranks:** the stream list, the dedup window, and the calibration-record rank head (empty
  until I4).

## 9. Settings and the cell budget

`EpistemicLimits` becomes a named-field struct replacing the positional constructor (tests are
updated). Each setting gets a `defaultSettingSpecs` entry with schema notes:

| Setting                                     | Default | Meaning                                       |
| ------------------------------------------- | ------- | --------------------------------------------- |
| `vagent.epistemic_history`                  | 4       | records retained per (principal, key) support |
| `vagent.epistemic_dedup`                    | 16      | dedup triples per profile                     |
| `vagent.epistemic_keys_per_source`          | 4       | keys one principal may support per predicate  |
| `vagent.epistemic_ingress_queue`            | 64      | queued ingestions per actor                   |
| `vagent.epistemic_ingress_queue_per_source` | 8       | queued ingestions per principal               |
| `vagent.epistemic_outcome_log`              | 64      | host outcome entries per source of an actor   |
| `vagent.epistemic_ingest_per_wake`          | 4       | ingestion turns per wake before others run    |
| `vagent.epistemic_arena_slots`              | 2^30    | slot allowance past which `ArenaExhausted`    |
| `vagent.epistemic_cells`                    | 24576   | maintained epistemic cell ceiling             |

The cell estimate comes from the field list:

- A record has about 35 fields, so with name cells, values, tuple view and clone occurrence it costs
  about 100 cells.
- A principal with 4 keys at a history of 4 costs about 1,600 cells. Its profile, streams and a
  16-pair dedup window add about 150.
- So the 24,576 default holds about 18 fully active principals at the measured 80 cells a record
  (section 14). `max_sources` (128) remains the logical ceiling, and the cell ceiling is the binding
  one.

**Physical slots.** Excision and retirement return logical budget only; unlinking never reclaims
arena slots, as P19 section 2.2 states. Every ingestion therefore mints slots that are never reused:
measured at about 250 (section 14), of which the record itself is about 80 and the program's own
handler frames most of the rest. The bound is `vagent.epistemic_arena_slots`, A, which memory
usually caps well below 2^31, since each slot costs at least 32 bytes. An actor lasts about A / (250
× R) seconds at an aggregate rate of R ingestions per second: the 2^30 default gives roughly 50 days
at 1 Hz in aggregate, and an allowance of 2^24 slots under a day. Past the allowance, ingestion
refuses `ArenaExhausted` before running, with the host action stated. A checkpoint and resume (1d)
copies only the reachable graph into a fresh arena, so it is the reclamation path. 1b's tests bound
the aggregate rate they exercise, and the deployment note in section 5.2's sense is that the host
keeps the aggregate ingestion rate at what arbitration needs.

The checkpoint's `vagent.value_operations` (65536) counts every reachable cell of the agent, not
only epistemic ones. The epistemic ceiling is therefore set well under it, and 1d adds a test that a
full epistemic budget still checkpoints. A deployment with more sources raises both settings
together. High-rate sensors arrive through an upstream estimator at the rate arbitration needs
(section 5.2); a history of 4 is not a raw-sample buffer.

## 10. Checkpoint and resume (1d)

`restoreState` must copy profiles, streams, dedup windows and every support's `d.epistemic` history
inside one arena, with one ref map per restore and clone ranks rebuilt once. `UpgradeGraph` assumes
separate source and target arenas, throws on unlisted dimensions, and is not drop-in. `restoreState`
has no `d.clone` handling today. 1d therefore starts with a spike on a two-source, two-key fixture
that copies these graphs inside one arena. The spike decides between generalising `UpgradeGraph` and
writing a role-mapped single-arena copier, and it is reviewed before the slice is implemented.

The predicate policy hash is compared on restore, and a mismatch refuses the resume. Steady-clock
fields from another clock instance are rebased by the checkpoint's recorded offset or, without one,
marked invalid so liveness restarts from the first new contact. Bindings and the outcome log are not
checkpointed: the host rebinds after resume. Live upgrade keeps refusing populated roots until the
typed translator exists.

## 11. Tests, files and later slices

New tests go in `tests/xuzz/vagent_epistemic_policy_test.cpp` (1a),
`vagent_epistemic_ingress_test.cpp` (1b, 1c) and `vagent_epistemic_resume_test.cpp` (1d). The
Makefile filters `tests/xuzz/vagent_*` into the actor suite, so no entry is needed. The ingress
tests also check:

- `$epistemic_cells` against the measured cell delta of each operation, including eviction;
- a refused observation that fires no `+pred` handler;
- the retire turn returning the budget;
- advance turns retiring lower-incarnation records on every key;
- relayed copies that never refresh contact.

1d copies the counter and rechecks it on restore.

| Slice | Tests                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| ----- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1a    | every key, type and domain; duplicates; missing required keys; each refused writer (statements, `from`, peer tell); plain predicates unchanged; `truest` as a predicate name; policy hash                                                                                                                                                                                                                                                                                                                                                                                                     |
| 1b    | each ingress refusal leaves nothing and logs an outcome; alias refusals; two sources share a clause; one source: same tuple retires its record, changed value transfers history with one current record and support (reciprocal edges checked), retry `Duplicate` refreshes nothing, other digest `ConflictingEvent`, stale sequence and older acquisition refuse; relayed copies count once and charge only `refused_count`; queue and per-source share; cell budget and keys per source; fuel atomicity; excision spares current and pinned records; typed `$turn_result` for every outcome |
| 1c    | a higher incarnation supersedes the binding, its advance turn runs first, starts at sequence 1 and retires lower records on every key; a lower or unbound incarnation refuses, also after the window is cleared; retire then rebind needs a higher incarnation and an old event refuses `StaleObservation`; the latch persists                                                                                                                                                                                                                                                                |
| 1d    | the spike fixture; steady times rebased or invalidated across clock instances; resume on a second host keeps topology and clone ranks; a changed policy refuses; a full epistemic budget still checkpoints                                                                                                                                                                                                                                                                                                                                                                                    |

Later slices:

- I3-2: cross-key retained transfer and user-level revision; health action grants and the latch
  reset.
- I3-3: heartbeat turns, timeout eligibility and clock-mapping resolution.
- I3-4: verification percepts and the application journal (P19.5). Pinning targets must happen
  before a label uses them, since retired records are excised eagerly.

### Noisy-sensor simulation and fuzzing (1b and 1c acceptance)

The owner asked for noisy sensors to be simulated. Two complementary tools are part of 1b's and 1c's
acceptance evidence:

1. **`tests/fuzz/fuzz_epistemic_ingress.cpp`**, a libFuzzer target in `make fuzz` under ASan and
   UBSan like the existing targets. The fuzz bytes decode into a script over a fixed program with
   one continuous and one discrete arbitrated predicate and at most four principals: bind, unbind,
   retire, rebind at a new incarnation, set the wall clock synchronised or not, observe and pump.
   Observation fields are drawn adversarially: NaN, infinities, subnormal and huge variances,
   sequence reversals, duplicate and conflicting event IDs, relays, skewed or future acquisition
   times and foreign frames. After every step the harness checks:

   - no crash or undefined behaviour, and every outcome is typed;
   - each ingestion is all-or-nothing, so the epistemic ranks are unchanged after a refusal;
   - at most one current record per (principal, key), with the profile's incarnation;
   - `$epistemic_cells` equals a walk of the epistemic graph;
   - resending committed bytes is `Duplicate` and changes nothing;
   - no refusal of a relayed copy changes health counters or contact.

   A seed corpus covers each refusal code.

1. **A seeded noisy-sensor simulator**, a test-support header used by gtest property tests over many
   seeds. It is deterministic and not a fuzzer. It generates ground-truth trajectories and reports
   from:

   - GPS with Gaussian noise, dropouts and multipath spikes;
   - odometry whose variance grows with drift;
   - a stuck sensor;
   - clock skew;
   - reboots that reset sequences and bump the incarnation;
   - relay duplication;
   - bursty rates that reach backpressure.

   1b and 1c use it to show the invariants hold under realistic noise. I4 reuses it with its ground
   truth as evidence for scoring and fusion at the ML gate. Its distributions are stated
   assumptions, not validation of a deployed sensor.

## 12. Findings and their resolution

Revision 1 (all four FAIL):

- Alias merging principals, `believe … from` spoofing: principal-keyed supports and the
  `EpistemicIngressOnly` refusal (sections 2 and 3).
- Same-source value changes (blocker for ML, hardware and codebase): own-source transfer (6.5).
- Reboot lock-out: incarnations (3, 1c).
- Receipt, failure channel and grant recheck: `TransportReceipt`, typed results, the outcome log and
  the ingestion-time recheck (4, 5, 6).
- Relays, idempotency, sequence scope, time and variance validation, profile fields, record fields,
  priors, pinned and queued-at-checkpoint: sections 3, 4, 6, 8 and 2.

Revision 2:

- ML gate (PASS, six minors): verdict revisions list; separate refusal counter; executor-computed
  canonical digest; lower-incarnation retirement on every key; domain/schema ID; honest cell count
  (sections 4, 6, 9).
- Epistemology N1 and hardware M1, incarnation ordering and lifecycle: strictly increasing
  incarnations, supersession, `StaleIncarnation` and `unbindSource` (3).
- Epistemology N2, unrefused writers, and codebase m, compiler guard: every writer enumerated and
  refused, with a compiler assertion (2).
- Epistemology N3, replays damaging health: `content_refusals` versus diagnostic `refused_count`,
  and no charge for relayed copies (6).
- Hardware M2 and codebase M4, predecessor supports double counting: principal-keyed supports and
  rule-based ineligibility (3).
- Hardware M3, fairness: a per-principal queue share and keys per source (4, 9).
- Hardware M4, variance ceiling: `min_variance` and `max_variance` (2, 4).
- Hardware M5 and codebase M2, cell budget: the field-based estimate, a maintained counter, the
  ceiling below the checkpoint limit and smaller defaults (9).
- Codebase M1, refusal codes lost on undo: `$turn_result` and `TurnOutcome::result` (5).
- Codebase M3, `from <alias>` matching nothing: refused statically (2).
- Codebase M5, restore copier: a spike before 1d (10).
- Codebase M6, bookkeeping write: `observeRefusalEntry`, observe-time refusals and the no-profile
  case (6).
- Smaller items:
  - `Duplicate` refreshes contact; clock skew and the stream's time fields (2, 6).
  - Checkpoint drain bounded by the queue (4); `wallNow()` instead of `ActorClock` (3).
  - The policy-schema hash is new work (2); fuel complexity is stated (6).
  - Test files and Makefile entries (11); terminal versus retryable classes (4).
  - Token charset (3); latch reset deferred (3); acquisition-order retirement (6.4).
  - Pinning precedes labels (11).

Revision 3:

- Codebase B1, `$turn_result` lost on backtrack: refusals halt rather than fail, `runTurnOnce`
  releases the mark itself, and a spike checks the nested-call halt (5).
- ML M1 and hardware M1, dedup before incarnation: incarnation is step 2, dedup is keyed by
  incarnation and cleared on advance (6).
- ML M2, epistemology R3-3 and hardware M4, acquisition order per stream: compare per (principal,
  key) with uncertainty and skew; the stream keeps transport order only (6).
- Epistemology R3-1 and ML lazy retirement: advance turns retire lower-incarnation records on every
  key at binding time (3).
- Epistemology R3-2 and ML `Duplicate` contact: a duplicate refreshes nothing; only direct committed
  events and direct content refusals refresh contact (6).
- Hardware M2, retired principals leaking budget: `retireSource` and its retire turn (3).
- Hardware M3, wall-clock receipt: steady and wall receipt times, steady-clock grant lifetimes and
  `ClockUnsynchronised` (3, 4).
- Codebase M, `Duplicate` committing a write: a duplicate commits nothing (5, 6).
- Codebase M, bookkeeping on the wrong thread: `RefusalNote` decided in `drainPosted` (4).
- Codebase M, settle on refusal: settle only on the commit path (6).
- Smaller items:
  - Key-limit refusals are terminal `EpistemicKeyLimit` (4); outcome log keyed by incarnation (4).
  - The host verifies the origin signature (3); producers named (2); variance bounds (2).
  - Round-robin draining (4); admission on net increase after excision (6).
  - Eligibility cost per support (3); calibration expiry left to I4 (3).
  - Clock epoch and mapping regressions (6); 1b ships with 1c (1, 3).
  - Counter tests and globbed test files (11).
  - Heartbeat, timeout and freeze fields remain I3-3 additions to the profile record.

Revision 4:

- Epistemology R4-1 and ML/hardware minors, retire erasing the replay floor: a tombstone with the
  highest incarnation, and the required ingress `max_age` with `StaleObservation` (2, 3, 4).
- Epistemology R4-2, observations advancing the incarnation: only `bindSource` advances;
  `UnboundIncarnation` (3).
- Hardware M1, wall-clock coupling: the `clock` key selects `wall` or `actor_mapped`, matching
  receipt stamps, `ClockUnsynchronised` only for `wall` (2, 4).
- Hardware M2, physical slots: priced with time to exhaustion, `ArenaExhausted`, and checkpoint and
  resume as reclamation (9).
- ML minors:
  - Variance bounds 1e-150 to 1e150 with rescaled sums (2); clock-instance tags and restore rebasing
    (4, 10).
  - The advance turn is queued first (3); stale test-row and "pairs" text fixed (9, 11).
  - Per-key tolerance stated: within it, an earlier reading may retire a later one, so I4 ages
    records from acquisition time.
- Codebase minors:
  - Enumeration through the profile clone rank (6); `breakOp` before relinking (6).
  - `IngestionFault` for turns without a result (4); host-action classes (4); `rtBDrop_` callers
    (2).
  - The four entries are `std::optional<CellRef>`, emitted only for programs with an arbitrated
    predicate.
- Hardware minors:
  - A reboot without an incarnation bump shows as `StaleSequence` refusals in `refused_count`.
  - `current` means latest by sequence within the acquisition tolerance.
  - The host owns wall-step detection (4).
- Epistemology minors:
  - The 1c tests cover rebinding a superseded incarnation after the window is cleared.
  - Liveness for relay-only sources is defined in I3-3.

## 13. Acceptance and carried minors

Revision 5 passed all four reviews: ML gate and compiler/codebase in round 4, epistemology and
hardware in round 5, each with no BLOCKING or MAJOR finding. Reports are in
[i3-review/](i3-review/). The round-5 minors are folded in as follows:

- **Digest coverage (epistemology M1):** origin-signed fields only (section 4).
- **Incarnation step (epistemology M2):** defensive, not an advance (section 6).
- **Bind-time floor (epistemology M3, M4):** `bindSource` accepts on host-side authorization only.
  The arena is authoritative: the advance turn refuses `StaleIncarnation` when the bound incarnation
  is not above the profile's or tombstone's recorded maximum. That turn also revokes the binding and
  logs the outcome. The tombstone records the maximum of the bound and profile incarnations.
- **`relayedBy` contract (epistemology M5):** the host must set `relayedBy` whenever the transport
  peer differs from the origin signer. A test covers a relayed and a direct copy.
- **Query-time age (epistemology M6):** I4 applies `max_age` to current records at query time as
  well as at ingress, so an unbound or expired source's last reading ages out.
- **Slot horizon (hardware 1):** computed from the configured allowance (section 9).
- **Recovery at exhaustion (hardware 2):** at `ArenaExhausted`, recovery is checkpoint and resume
  only; no retire, advance or bookkeeping turn is attempted. Events refused `ArenaExhausted` may be
  resent after the resume.
- **`UnboundIncarnation` (hardware 3):** classed needs-host-action, resendable after the host binds
  the new incarnation, so a reboot's first readings are not lost.
- **Host trust (hardware 4):** the wall-sync flag and the `actor_mapped` mapping are host-trusted.
  Mapping resolution stays on I3-3's list.
- **Boot counters (hardware 5):** they must be persisted by the device or issued by its host
  identity layer. A RAM-only counter repeats after a power loss, and the device then refuses
  `StaleIncarnation`. Retirement does not reset it: the tombstone keeps the floor, so the host must
  issue the device an incarnation above it (section 14).

## 14. Amendments from the implementation review (1b, 1c)

The four personas reviewed the implementation at `4ad3f53a` ([reports](i3-1bc-review/)); every
persona failed it with MAJOR findings and none BLOCKING. The fixes change the design where it was
silent or wrong:

- **Outcome log.** One bounded log per (actor, principal), one entry per event updated in place, so
  no source's flood evicts another's outcomes and an accepted event costs one entry. A `Committed`
  or `Duplicate` entry is never overwritten: a later resend, relay or refusal of the same key is a
  retry of an event the store holds, and is not charged to the source.
- **Unauthenticated input.** A malformed principal or relay, an event ID over 256 bytes, a name over
  256 bytes or signed fields over 64 KiB refuse `InvalidObservation`, and an unbound source
  `UnknownSource`, without being logged: the return value is the answer, and the log keeps only what
  a binding authorized. An incarnation that does not fit Int64 refuses `InvalidIncarnation`.
- **Queue.** Slots are held until the ingestion's turn completes, not until it is drained. Each
  principal, and each relay, may hold a fair share of the actor queue: the per-source bound, shrunk
  to the queue divided among the bound sources. A wake runs at most
  `vagent.epistemic_ingest_per_wake` ingestions, so a flooded actor yields the pump. An ingestion
  that can never run (its actor stopped or could not be woken) is logged `Undelivered`.
- **Wall clock.** The synchronised flag starts unknown: wall-basis observations refuse
  `ClockUnsynchronised` until the host's time service reports. A host that steps its clock clears
  the flag around the step, or every source is charged the time refusals the step causes.
- **Incarnation floor.** The host keeps the floor each advance and retire reports, so `bindSource`
  refuses `StaleIncarnation` synchronously below a live profile's incarnation or at or below a
  tombstone's. The advance turn remains the authority (a resumed actor's arena may know more) and
  `sourceChangeOutcome` reports its result. Every bind queues the source-change turn; at the
  profile's incarnation it changes nothing, so rebinding retries an advance that failed. A revived
  tombstone takes a share of `max_sources` again. Retiring a principal that nothing names leaves
  nothing.
- **Keys and clocks.** A key counts against `keys_per_source` only while it holds a current record,
  so a predecessor's retired supports do not silence its successor. A keyless arbitrated predicate
  is one quantity per source: its new reading supersedes the last. Streams record the last sequence,
  clock epoch and mapping version, and a regression of either refuses `ObservationTimeMismatch`;
  only a higher epoch resets the per-key acquisition order.
- **Record identity.** Each record carries observation version 1, its domain (predicate name and
  arity with the policy hash), model, model and calibration epochs 0, feature schema 0 with an empty
  feature list, the executor's steady-clock instance and the source's health at ingest.
- **Failed turns.** A turn that set no code and ran out of cycles or fuel is `TurnExhausted`, needs
  host action; any other is `IngestionFault`.
- **Open for I4** (options in [i3-open-questions.md](i3-open-questions.md)). Decided in round 2: an
  advance drops supports left with no current record (see below); how plain queries of an arbitrated
  predicate answer is I4's. Counters are lifetime-cumulative across incarnations and revivals; I3-2
  decides whether health needs them per incarnation or windowed. A source refused from its first
  event has no profile to charge; I3-2 says how health treats one.

**Uncertainty bound (hardware M3).** The owner chose a policy key: `max_uncertainty`, optional,
resolving to `max_age` and hashed, so an existing program keeps working and the bound in force is
recorded. The canonical encoding is now `vagent-policy-2`. A reading claiming more refuses
`UncertainAcquisition`, a content refusal; the in-turn acquisition order, which adds both
uncertainties, is bounded by the same key.

**Checkpoint and resume (1d).** `restoreState` copies the source profiles (with their streams, dedup
windows and clone ranks) and every support's history through one copy map, so each record's clone
occurrence is the copy its profile's rank made and the ranks come out rebuilt once: the single-arena
`cloneGraph`, given a shared map, needed no generalised `UpgradeGraph`. Profiles drop `last_receive`
and `last_contact`, so liveness starts again from the first contact on the new executor; records
keep their steady stamps, tagged with the clock instance that made them. Bindings, queues and the
outcome log are not checkpointed: the host rebinds, and the arena's floor refuses a binding below it
on the advance turn, which revokes it. A changed policy is refused by `resume` itself, since the
program reference covers it; `restoreState` compares the policy hashes again. A checkpoint encodes
about two operations per epistemic cell: at the old 32,768-cell default a full budget did not
checkpoint under `vagent.value_operations` (65,536), so the default is now 24,576, and a test fills
it and resumes.

**Review round 2.** ML passed; hardware, epistemology and codebase each failed one MAJOR, fixed:

- **Source changes keep call order.** `bindSource` and `retireSource` put their change ahead of
  every other post but behind the changes already waiting, so a retire followed by the next boot's
  bind before one pump runs in that order (it ran newest first, tombstoning the live binding).
- **An advance drops the predecessor's supports** (the owner's decision on open question 1). Every
  support of the principal holds only retired records after an advance, so each is dropped through
  `rtBDrop`, settling `-pred`, and its records move to a retired-history chain on the profile's own
  `d.epistemic`, trimmed to `history × keys_per_source` (unpinned oldest first, with their clone
  occurrences). A belief then means a current boot asserts it, calibration keeps the records, and a
  device that reports new keys after every reboot stays bounded: it filled the cell ceiling at about
  86 boots and then refused every source on the actor. The retire turn and the resume copy walk the
  chain like any history.
- **Smaller fixes.** No host floor for a principal nothing names (the floor's 0 default was a
  sentinel). A revive the budget refused revokes its binding. Queue counter entries are erased at
  zero and never made by a refused observation. `bindSource` refuses an unknown pid
  (`UnknownActor`). A resend of a held or queued event, or of one refused for the same reason,
  neither overwrites the log entry nor is charged again. `observe` checks its field order against
  `kObservationOrder`, and `readAt` refuses a record shorter than the order.
- **Recorded, not changed.** A reading can be accepted up to about `max_age + max_uncertainty` old,
  at most twice `max_age`. The default queue (64 slots) is shared fairly among bound sources, so
  more than 64 sources get one slot each and no more. A source's own flood can evict its oldest
  outcomes from its log. After a resume the outcome log starts empty, so a retry of an event
  committed before the checkpoint is refused and charged as such. Every exception in a turn is
  classed as a fuel failure, which predates this slice.

**Review round 3.** All four personas passed `08425c60` (ML, epistemology, hardware, codebase), with
no BLOCKING or MAJOR finding. The closing changes: a refusal is charged once per event and code,
with one more charge for the first direct copy after relayed ones, wherever it is refused;
`IngestOutcome::code` is optional and `FrameBody` has no unset defaults; a field-order mismatch is a
host error, never charged; `recordOf` refuses a broken clone rank; and `ArenaExhausted` gates
source-change and bookkeeping turns as well as ingestions. Recorded and open:

- **A failing `-pred` handler blocks a source's lifecycle.** The advance and the retire settle the
  `-pred` they raise in their own all-or-nothing turn, as every belief change does, so a handler
  that always fails makes both refuse (`TurnExhausted` or `IngestionFault`) until the program is
  fixed, and a rebind meets the same. Options for a later slice: deliver those events in ordinary
  handler turns after the source change commits, or let an operator retire without settling.
- **The retired history is per profile.** It is not age-ordered within one boot (its order is the
  clone rank's), and a later support for the same key starts a fresh history, so I3-2 and I4 read
  the profile's chain for a source's past.
- **Smaller.** A stopped actor's outcome log goes with it rather than reading `Undelivered`. A
  changed policy is refused by `resume`; the `restoreState` check behind it throws untyped. Every
  exception in a turn is a fuel failure (older than this slice). No test drives an advance that
  fails and is then retried successfully, since no setting fails an advance while ingestion fits.

**Fuzz evidence.** The fuzzer program gained a keyless predicate, and the harness two scripted steps
(a principal past its key limit, an event ID reused with other bytes) that random bytes rarely
reach. Report mode counts unlogged refusals and the duplicate and conflicting turns a sticky entry
hides. The seed corpus reaches 17 outcome codes, every one a reading can meet except
`StaleSequence`, which unit tests pin with the host-condition codes (`TurnExhausted`,
`ArenaExhausted`, `Undelivered`). Four regression inputs from fuzzing are in the corpus: the
non-finite reading and three harness corrections for the sticky, per-source log.

**Cost.** The routine read every field by a search and copied the observation, a recursive copy with
a frame per cell. It now adopts the observation the executor wrote as the record and reads it in one
walk in a fixed field order (`kObservationOrder`), each name checked, and writes scalars in place. A
committed ingestion went from 12,096 to 4,362 VM cycles (12.1 ms to 4.6 ms at -O3 on the lead's
machine) and from about 670 to about 250 arena slots. The remaining slots are mostly the record and
the program's handler frames, which every vagent activation allocates and never reclaims, a
compiler-wide cost outside this slice.

# I3-2 design: source health, decayed counters and operator health actions

Status: revision 1, for review by the four personas (ML gate, epistemology, hardware, codebase).
Authority: approved P19 v1.5 sections 2.1 (health preservation), 4 (`EpistemicHealthGrant`,
`UnauthorizedHealthAction`) and 5.1 (latch, reasons, displayed state), and the owner's decisions on
[open question 2](i3-open-questions.md) (latches for life, counters as decayed rates with an
ML-reviewed half-life) and question 3 (built in I3-1). Baseline: `8ec71fc6`, with I3-1a to 1d
accepted ([validation](i3-validation.md)).

## 1. Scope and split

I3-2 gives each source profile a health that the arena keeps, that a reboot cannot shed, that an
authorized operator can change, and that evidence can heal. It ships in two slices:

| Slice | Content                                                                                                  | Gate             |
| ----- | -------------------------------------------------------------------------------------------------------- | ---------------- |
| 2a    | decayed refusal and rejection counters, the derived health state, its stamp on records, resume rebasing  | ML, then all     |
| 2b    | `EpistemicHealthGrant`, the host health action API, and the Vortex health turn (isolate, degrade, reset) | all four, ML too |

Moved out, with reasons (section 9): cross-key retained transfer and user-level revision go to I4;
sensor-originated health reports go to I3-3 with heartbeats and device telemetry; the `reset`,
`isolate` and `degrade` source directives in the actor language go to I4's lowering, onto the turn
2b builds.

## 2. What exists

From I3-1 (`compiler.cpp` `emitProfileFor` and `emitObserveRefusal`, `runtime_abi.hpp:631-650`), a
profile on `$agent +d.trust` holds:

- `faulted`, a Bool latch that nothing sets yet, which ingestion reads to stamp a record's `health`
  as `FAULTED` instead of `NOMINAL` (`compiler.cpp:5257`);
- four Int64 counters, all lifetime-cumulative across advances and revivals: `rejection_streak` and
  `reject_count` (gate rejections, which nothing produces until I4's gates), `content_refusals`
  (direct frame, unit, variance, time and uncertainty refusals) and `refused_count` (every other
  chargeable refusal, and any refusal of a relayed copy);
- `last_receive` and `last_contact`, steady seconds, dropped on resume; `last_refusal`, a code.

Nothing reads a counter. The spec asks for more: separate degradation and suspicion reasons with
provenance, a displayed state FAULTED, else SUSPECT, else DEGRADED, else NOMINAL, and authorized
`isolate`, `degrade` and `reset`.

## 3. Decayed counters (2a, ML gate)

### 3.1 Which counters decay

| Counter            | Today        | I3-2                      | Why                                                                     |
| ------------------ | ------------ | ------------------------- | ----------------------------------------------------------------------- |
| `content_refusals` | lifetime Int | decayed rate, Double      | feeds health; a repaired device must recover on evidence                |
| `reject_count`     | lifetime Int | decayed rate, Double      | the same, for I4's gate rejections                                      |
| `rejection_streak` | lifetime Int | unchanged, a streak       | P19 5.1: consecutive rejections; an accepted measurement clears it (I4) |
| `refused_count`    | lifetime Int | unchanged, lifetime total | diagnostic only, never feeds health; a total answers "how much replay"  |

Keeping `refused_count` lifetime is deliberate: it never reaches a health rule, so its decay would
only lose the audit total. Its overflow is already saturating.

### 3.2 The update

Each decayed counter is a pair of profile fields: the value `c` (Double, at least 0) and the steady
time `t_c` (Double seconds on the executor's clock) at which `c` was last brought up to date. With
half-life `h` seconds, a charge of `n` events at time `t`:

```math
c \leftarrow c \cdot 2^{-\max(0,\,t - t_c)/h} + n,
\qquad t_c \leftarrow \max(t_c,\, t).
```

The value of the counter at any later time `s`, without writing anything, is
`c * 2^(-max(0, s - t_c)/h)`. The routine computes `2^(-x)` as `Exp(-x * ln 2)` (I1's `Exp`,
correctly rounded within 1 ulp per its contract).

Properties the tests pin:

- **Order.** Coalesced charges can arrive out of time order (I3-1 section 14: a refusal received
  before a later commit is charged after it). The `max(0, ...)` clamp charges a late event at full
  weight instead of up-weighting it by a negative age, and `t_c` never moves back. An event late by
  `d` counts 1 at `t_c` where its exact weight is `2^(-d/h)`: an overcount by a factor of at most
  `2^(d/h)`, which for the coalescing delay (one pump, milliseconds) against a one-minute `h` is
  below 1e-4 relative.
- **Steady state.** A source refused at a constant rate `lambda` per second converges to
  `lambda * h / ln 2` (the integral of the kernel). A burst of `k` refusals reads `k`, then halves
  every `h`.
- **Range.** `c` is finite: each update is a contraction plus a bounded `n` (charges coalesce per
  pump, bounded by the ingress queue), so `c <= n_max / (1 - 2^(-Δ/h))` for the minimum spacing; a
  test drives 10^6 charges and checks finiteness. A non-finite intermediate refuses `IngestionFault`
  (the VM's own float checks), never a stored NaN.
- **Zero.** A fresh profile has `c = 0` and `t_c` absent: the first charge sets `c = n`, `t_c = t`.
  An absent `t_c` is the arena's absence (no field), not a 0 time.

### 3.3 The half-life and the threshold

Health is per profile, and a principal may be bound to several predicates with different policies,
so the half-life cannot be a predicate policy key without a rule for which policy wins. I3-2 makes
both health parameters system settings, read by the executor and passed in the turn record like
`max_sources` already is:

| Setting                             | Default | Domain         | Meaning                                                               |
| ----------------------------------- | ------- | -------------- | --------------------------------------------------------------------- |
| `vagent.epistemic_health_half_life` | 60 s    | (0, 31 years\] | half-life `h` of `content_refusals` and `reject_count`                |
| `vagent.epistemic_suspect_refusals` | 3       | (0, 1e6\]      | decayed `content_refusals` at or above which the source reads SUSPECT |

The defaults, argued. P19 5.1 sets three consecutive rejections for suspicion; three refusals within
much less than a half-life is the same evidence count. With `h = 60 s`, a source reads SUSPECT under
a burst of three refusals, or under a sustained content-refusal rate above `3 ln 2 / 60`, about one
every 29 s; a 1 Hz sensor with a 3% refusal rate stays NOMINAL and one with 5% does not. A repaired
device's count halves every minute: a burst of 10 falls below 3 within about 105 s. P19 calls these
"configurable engineering heuristics, with a declared sample-rate or window interpretation"; this
table is that declaration, and the system-docs schema notes repeat it.

Refused alternatives:

- **A policy key per predicate** (`health_half_life`): ambiguous for multi-predicate principals, and
  the policy hash would make a tuning change refuse resume.
- **A field on `EpistemicSourceGrant`**: the same ambiguity across a principal's bindings, and a
  host could reset the evidence's weight by rebinding.
- **A fixed window count** (the last N minutes): needs a ring of timestamps per counter, cells
  proportional to traffic, which the budget would have to price.
- **Hysteresis** (raise at 3, clear at 1.5): the decayed value is smooth, so it crosses the
  threshold once per burst; flapping needs refusals arriving at exactly the threshold rate. Recorded
  as a question for the ML gate rather than built.

`reject_count` gets no threshold in I3-2: its producer is I4's gates, which will add the streak
thresholds of P19 5.1 (three for suspicion, five for fault) and say how a rejection rate maps to
DEGRADED with the gates' false-alarm rates in hand.

### 3.4 Where the time comes from

- **Bookkeeping turns** (`observeRefusalEntry`): the refusal note gains `at`, the steady receipt
  time of the newest refusal it charges (the executor already stamps `contact` for direct content
  refusals; `at` is stamped for every charge, direct or relayed, since `refused_count` is the only
  counter a relayed copy reaches and it does not decay).
- **Ingestion** reads the record's `received_steady` to stamp health (section 4); it charges nothing
  in I3-2.
- **Health turns** (2b) carry the steady time the executor queued them.

A turn never reads a clock: each time is a turn input, so a replayed turn decays identically.

### 3.5 Resume

Steady times are not comparable across executors (I3-1 1d drops `last_receive` and `last_contact`).
`restoreState` rewrites each `t_c` to the restoring executor's steady time at restore: the counter
keeps its value and does not decay over the downtime. Crediting the downtime would let an operator
launder a suspect source by checkpointing it for a few half-lives; P19 2.1 says unknown clock
mapping cannot clear a latched fault, and the same reasoning applies to evidence that decides
SUSPECT. The cost: a device repaired during the downtime recovers a downtime late.

## 4. Health state (2a)

The displayed state is derived, never stored, by one shared IR routine
`rtHealth(profile, now, threshold, h)`:

1. `faulted` latched: **FAULTED**.
1. Any suspicion reason on the profile, or decayed `content_refusals` at `now` at or above the
   threshold: **SUSPECT**.
1. Any degradation reason: **DEGRADED**.
1. Otherwise **NOMINAL**.

Ingestion stamps the result on each record's `health` field at `received_steady` (today it writes
NOMINAL or FAULTED), so I4 and calibration see the health the source had when the reading arrived.
I4's queries call the same routine at query time without writing anything (P19 5.1: queries never
advance counters). Deriving rather than storing SUSPECT means there is no "clear the suspicion"
event to miss: a quiet source's count decays and the state follows on the next read.

A profile created by refusals alone (I3-1 question 3) has no `last_receive`. That state is named,
not special-cased: it has no current record, so nothing of it is eligible; its counters and latch
apply as to any profile; and I3-3's heartbeat rules treat an absent `last_receive` as "never
supplied a reading", never as time 0.

## 5. Reasons and provenance (2b)

The profile gains one owned list, `reasons`, on the I3-1 list shape (`d.grab` head, `d.step`
elements). Each element is a record:

| Field     | Type   | Meaning                                                                      |
| --------- | ------ | ---------------------------------------------------------------------------- |
| `kind`    | Text   | `degraded` or `suspect`                                                      |
| `code`    | Text   | the operator's reason code, at most `kMaxObservationNameBytes`               |
| `grant`   | Text   | the health grant's ID                                                        |
| `issuer`  | Text   | the grant's issuing principal                                                |
| `at_wall` | Double | the host's wall time of the action, for audit only; nothing computes with it |

The latch gets the same provenance as a record in a `fault` field (absent until the first isolate).
Reasons are bounded by `vagent.epistemic_health_reasons` (default 8 per profile): a `degrade` past
the bound refuses `EpistemicBudget` rather than evicting an older reason, since a reason is evidence
an operator placed and only `reset` removes it. Each reason is about 12 cells, counted in
`$epistemic_cells` like every profile cell.

`suspect` reasons have no producer in I3-2; the kind exists so I3-3's telemetry and I4's gates add
reasons without reshaping the list.

## 6. Health grants and actions (2b)

### 6.1 The grant

```cpp
enum class HealthActionKind : std::uint8_t { Isolate, Degrade, Reset };

struct EpistemicHealthGrant {
  std::string id;                       // audit name, bounded
  std::string issuer;                   // 64-hex fingerprint of the operator identity
  std::vector<std::string> sources;     // principals it may act on; never empty
  std::vector<HealthActionKind> actions;// never empty
  std::chrono::steady_clock::duration lifetime{};
};
```

`grantHealth(target, grant)` installs it for one actor, deny by default: an empty source or action
list, a malformed principal, a duplicate ID or a zero lifetime refuses `InvalidHealthGrant`. There
is no wildcard source: P19 4 grants "for specified source identities", and an operator who means
every source lists them. `revokeHealthGrant(target, id)` removes it. Grants live in the executor's
ingress state beside the bindings: they are host authorization (the brief's permitted C++), and no
learned state is in them. They are not checkpointed, as bindings are not.

### 6.2 The request

```cpp
struct HealthActionRequest {
  std::string grant;     // the grant's ID
  std::string issuer;    // checked equal to the grant's
  std::string principal; // the source acted on
  HealthActionKind kind;
  std::optional<std::string> code; // required for Isolate and Degrade, absent for Reset
  double atWall{};       // audit only
};
```

`healthAction(target, request)` refuses synchronously `UnauthorizedHealthAction` when the grant is
missing, expired (steady clock), issued by another principal, does not name the source, or does not
allow the kind; and `InvalidObservation` for a missing or oversized code. Otherwise it queues a
health turn with `pushControl`, so it keeps call order with source changes (a reset queued after a
rebind runs after its advance). The outcome is read like a source change's:
`healthActionOutcome(target, principal)` returns the newest. Authentication of the issuer is the
host adapter's, exactly as for observations: the executor checks the claim against the grant, not a
signature.

### 6.3 The turn

`healthEntry`, emitted Vortex IR beside `sourceChangeEntry`, reads `$turn.health` (source token,
incarnation when the principal is bound, kind, code, grant, issuer, wall time, steady time, the
reason bound and `max_sources`) and finds the profile through `emitProfileFor` when bound, so an
operator can isolate a bound source before its first reading; an unbound principal with no profile
refuses `NoProfile`. Then:

- **Isolate** sets `faulted` and writes the `fault` provenance (a second isolate keeps the first
  provenance and adds nothing: the latch records when it latched). Supports stay: eligibility
  excludes a faulted source's readings (P19 5.1), and plain queries become arbitrated answers in I4
  (question 1).
- **Degrade** appends a `degraded` reason.
- **Reset** clears the latch and its provenance, empties `reasons`, zeroes both decayed counters and
  the streak, and invalidates the source's readings: every current record is retired and every
  support dropped through `rtBDrop`, settling `-pred`, with the records moving to the profile's
  retired-history chain, exactly as an advance does (the advance body is shared, parameterised on
  whether the incarnation changes). The dedup window and streams stay, so replays and old sequences
  still refuse (P19 2.1: floors still reject replay). Calibration records do not exist until I4,
  which must reset them to the declared prior in the same turn (P19 5.1).

A reset source supplies a new reading before it is eligible again, because nothing current survives.
`refused_count` and `last_refusal` stay: they are diagnostics, not health.

ArenaExhausted and the cell ceiling gate the health turn like every epistemic turn (I3-1 section
14). A reset that the budget refuses changes nothing and reports its code.

## 7. Why Vortex, and the C++ that is not

The counter update, the state derivation, the reasons list and all three actions are emitted IR run
by the VM, sharing `emitProfileFor`, `putScalar` and the advance body. The C++ is grant storage and
checking (authorization), the steady and wall timestamps (clocks), queueing and the outcome map
(transport), and the `t_c` rebase in `restoreState`, which is the resume copy that already rebases
steady fields.

## 8. Tests, budget and settings

| Slice | Tests                                                                                                                                                                                                                                                                                                                                                                                                                      |
| ----- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 2a    | decay against a closed-form reference (burst, constant rate, out-of-order charge, gaps of 0 and 10h); SUSPECT at the threshold and NOMINAL after it decays; the stamp on records; a relayed refusal charges nothing that decays; resume keeps value, rebases time, no downtime decay; 10^6 charges stay finite; the noisy-sensor simulation's faulty source reads SUSPECT and its honest peers NOMINAL                     |
| 2b    | each unauthorized case (missing, expired, issuer, source, kind); isolate before the first reading sticks and stamps FAULTED; a reboot keeps the latch and reasons; reset clears them, retires every current record, raises `-pred`, keeps the dedup floor (a replay still refuses), and the source is eligible only after a new reading; the reason bound; order with a queued rebind; resume keeps reasons and provenance |

New settings get `defaultSettingSpecs` entries with schema notes: the two in section 3.3 and
`vagent.epistemic_health_reasons`. The fuzzer gains a health-action step (isolate, degrade, reset
under a valid and an invalid grant) and the invariant that a refused action changes nothing.

Cost: the decay adds one `Exp` and about 20 instructions to a bookkeeping turn; the state adds a
reasons walk (at most 8) and one `Exp` to an ingestion. A health turn is bounded like an advance,
O(K * H) for a reset.

## 9. Moved out of I3-2

- **Cross-key retained transfer.** P19 2.1 moves a removed support's retained records "to an owned
  retained-observation list under the keyed calibration record". That record is I4's, and until it
  exists I3-1 already keeps every removed support's history on the profile's retired chain. I4
  re-homes it when it builds the calibration record.
- **User-level revision.** I3-1a statically refuses every program writer of an arbitrated predicate.
  Letting a program assert or retract one (`assert ... with variance`, P19 4) needs the actor AST
  and lowering that I4 adds, so the revision rule belongs to the same change.
- **Sensor health reports.** "A sensor's authenticated health report may add a configured warning"
  needs a schema for configured warnings and a telemetry path; it lands with I3-3's heartbeats and
  device telemetry, adding `suspect` and `degraded` reasons to this list.
- **The language directives.** `reset source`, `isolate source` and `degrade source` lower in I4
  onto `healthEntry`, under the same grant check, with `UnauthorizedHealthAction` outside an
  authorized context.

## 10. Open for review

- Is a 60 s half-life and a threshold of 3 a defensible default, and should the state have
  hysteresis (section 3.3)?
- Should `degrade` be clearable without a full `reset` (P19 lists only three directives; a recovery
  percept clears its own reason, but there is no operator "undegrade")?
- The I3-1 carried wedge applies to reset: a `-pred` handler that always fails makes a reset refuse,
  as it makes an advance and a retire refuse.

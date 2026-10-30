# I3-2 design: source health, settle-after-commit and operator health actions

Status: revision 2. Revision 1 (`e226c507`) failed all four reviews with no BLOCKING finding and 24
MAJOR ones ([reports](i3-2-review/)); section 12 maps each to its resolution. Authority: approved
P19 v1.5 sections 2.1 (health preservation), 4 (`EpistemicHealthGrant`, `UnauthorizedHealthAction`)
and 5.1 (latch, reasons, displayed state); the owner's decisions on
[open question 2](i3-open-questions.md) (latches for life, counters as decayed rates with an
ML-reviewed half-life); and two owner decisions taken on revision 1's review: **add an operator
`clear`** that removes degradation reasons without a reset and never unlatches, and **fix the
`-pred` handler wedge in I3-2**. Baseline: `8ec71fc6`, with I3-1a to 1d accepted
([validation](i3-validation.md)).

## 1. Scope and split

I3-2 gives each source profile a health that the arena keeps, that a reboot cannot shed, that an
authorized operator can change, and that evidence can heal. It ships in three slices:

| Slice | Content                                                                                                     | Gate         |
| ----- | ----------------------------------------------------------------------------------------------------------- | ------------ |
| 2a    | the decayed content-refusal fraction, the derived health state and its stamp on records                     | ML, then all |
| 2b    | settle after commit: source changes (and 2c's actions) commit first, then their belief events run           | all four     |
| 2c    | `EpistemicHealthGrant`, the host health-action API, the Vortex health turn (isolate, degrade, clear, reset) | all four, ML |

2b comes before 2c because isolate and reset drop supports, and a failing `-pred` handler must not
be able to block them (section 6).

Moved out, with reasons (section 10): cross-key retained transfer and user-level revision go to I4;
sensor-originated health reports go to I3-3 with heartbeats and device telemetry; the `reset`,
`isolate`, `degrade` and `clear` directives in the actor language go to I4's lowering, onto 2c's
turn.

## 2. What exists

From I3-1 (`compiler.cpp` `emitProfileFor` and `emitObserveRefusal`, `runtime_abi.hpp:631-650`), a
profile on `$agent +d.trust` holds:

- `faulted`, a Bool latch that nothing sets yet, which ingestion reads to stamp a record's `health`
  as `FAULTED` instead of `NOMINAL` (`compiler.cpp:5257`);
- four Int64 counters, all lifetime-cumulative across advances and revivals: `rejection_streak` and
  `reject_count` (gate rejections, which nothing produces until I4's gates), `content_refusals`
  (direct frame, unit, variance, time and uncertainty refusals, `agent_executor.cpp:845`) and
  `refused_count` (every other chargeable refusal, and any refusal of a relayed copy);
- `last_receive` and `last_contact`, steady seconds, dropped on resume; `last_refusal`, a code.

Nothing reads a counter. The spec asks for more: separate degradation and suspicion reasons with
provenance, a displayed state FAULTED, else SUSPECT, else DEGRADED, else NOMINAL, and authorized
health actions.

## 3. The decayed content-refusal fraction (2a, ML gate)

### 3.1 Which refusals are evidence against a source

A refusal feeds health only when the bytes the device signed are unusable whatever the path did to
them: `ObservationFrameMismatch`, `MissingVariance`, `InvalidVariance`, `UncertainAcquisition` (the
device itself claims it cannot place the reading in time), each of a directly received event. These
are **health refusals**. `InvalidObservation` (a non-finite or oversized field) is refused before a
charge is classified as content and stays diagnostic, as in I3-1.

`StaleObservation` and `ObservationTimeMismatch` are not. A link outage followed by a buffer flush,
a host clock-mapping fault or a congested relay produces them from a healthy device, and one host
fault would move every source at once (a common cause read as a fleet failure). They stay in the
lifetime `content_refusals` total and still refresh contact, as today, but feed no health rule until
I3-3 can tell device lateness from path lateness.

Relayed copies charge only `refused_count`, as today, so a relay cannot make a victim SUSPECT.

### 3.2 The estimator

Health uses an exponentially weighted fraction indexed by **events**, not seconds. Two Double fields
on the profile, `health_refused` (R) and `health_events` (N), both starting at 0. With half-life `H`
events and `b = 2^(-1/H)`:

- a committed ingestion (an accepted reading, direct or relayed, never a duplicate): `R <- b R`,
  `N <- b N + 1`;
- a bookkeeping turn charging `k` health refusals: `R <- b^k R + (1 - b^k)/(1 - b)`,
  `N <- b^k N + (1 - b^k)/(1 - b)`, computing `b^k` as `Exp(k * ln b)` (I1's `Exp`).

The refusal fraction is `f = R / N` when `N > 0`. It is a weighted proportion of the source's recent
outcomes: the newest event has weight 1, one `H` events back weight 1/2.

Why events rather than seconds (the revision 1 rule was a time-decayed count, and every reviewer
showed it unsound):

- **Traffic-independent.** A count of refusals per unit time scales with the sample rate: at 60 s
  and threshold 3, a 100 Hz sensor refusing 1 in 2000 read SUSPECT, and a sensor reporting slower
  than every 35 s never could. A fraction is a property of the source's quality, the same at 0.01 Hz
  and 100 Hz.
- **Bounded.** `0 <= R <= N <= 1/(1 - b)`: no flush, coalesced batch or zero spacing can push it
  past the bound, and nothing can become non-finite (every operand is finite, `b^k` is in (0, 1\]).
- **No clock.** The update needs no time, so there is no out-of-order clamp, no coalescing-delay
  overweight, no suspend or steady-clock question, and no resume rebase: a checkpoint carries R and
  N and resumes them unchanged. A source that falls silent keeps its fraction, which is right: P19
  5.1 says silence establishes missing data, not its cause, and a silent source has no fresh reading
  to inflate.
- **Recovery on evidence.** A repaired device clears its fraction by sending good readings, at a
  rate set by its own traffic.

The owner's ruling (counters as decayed rates with a half-life) is kept: this is a decayed rate of
refusal per event, and `H` is the half-life the ML gate reviews.

**Order.** Charges coalesce, so a batch of refusals can be charged after accepted readings received
after them. The fraction then weights the batch as newer than it was. For a batch of `k` charged `m`
events late, each member's weight is off by a factor `b^(-m)`; with the defaults that is below 2 for
`m < H`. It errs toward suspicion, never toward trust, and only for the events in flight.

### 3.3 Defaults and their interpretation

| Setting                               | Default | Domain    | Meaning                                               |
| ------------------------------------- | ------- | --------- | ----------------------------------------------------- |
| `vagent.epistemic_health_half_life`   | 32      | [1, 10^6] | half-life `H` of the fraction, in events              |
| `vagent.epistemic_suspect_fraction`   | 0.25    | (0, 1\]   | `f` at or above which the source reads SUSPECT        |
| `vagent.epistemic_suspect_min_events` | 3       | [1, 10^6] | `N` below which `f` decides nothing (a fresh profile) |

Simulated (Bernoulli refusals, 4×10^5 events, `/tmp/i3-2-lead/sim.py`, reproduced in the test
suite's noisy-sensor simulation), fraction of time SUSPECT and state changes per hour at 1 Hz:

| True refusal rate | H = 16     | H = 32 (default) | H = 64       |
| ----------------- | ---------- | ---------------- | ------------ |
| 5%                | 0, 0       | 0, 0             | 0, 0         |
| 10%               | 0.2%, 6/h  | 0, 0.1/h         | 0, 0         |
| 15%               | 3.9%, 76/h | 0.7%, 13/h       | 0.04%, 0.5/h |
| 20%               | 20%, 256/h | 12%, 132/h       | 5%, 48/h     |
| 30%               | 76%        | 85%              | 93%          |
| 50%               | 100%       | 100%             | 100%         |

Events to read SUSPECT from a long clean history: 14 when every reading is refused, about 33 at a
50% rate. From SUSPECT at a 100% rate back to NOMINAL on good readings: 64 events. A fresh profile
reads SUSPECT after its first three readings if all three are refused.

The declared interpretation (P19 5.1): **SUSPECT means about a quarter or more of the source's last
few dozen outcomes were unusable content.** A source refusing under 10% stays NOMINAL. Between 15%
and 25% the state is noisy by nature: the fraction near the threshold is a fair coin. The ML gate
judged hysteresis a bad trade (it needs stored state and doubles the time a source stays SUSPECT for
fewer transitions), so instead the state is for display, the latch and audit, and I4 must use the
continuous fraction, not the thresholded state, in scoring (section 4).

This is not P19's "three consecutive rejections". That is a streak of gate rejections, conditioned
on adjacency, which I4 produces into `rejection_streak` and adds to the state rule with its own
thresholds (three for SUSPECT, five for FAULTED). `content_refusals`, `reject_count` and
`refused_count` stay lifetime Int64 totals for diagnostics; I4 converts gate rejections into the
same estimator when it adds them.

Refused alternatives:

- **A time-decayed count** (revision 1): traffic-dependent, unbounded under flushes, and unreachable
  for slow sources (section 3.2).
- **A time-decayed fraction** (both R and N decayed by time): bounded and traffic-normalised, but a
  slow source's N never reaches the evidence floor within a half-life, so the half-life would have
  to follow each binding's cadence; the cadence is I3-3's heartbeat interval, not known here.
- **A policy key or a grant field**: health is per profile and a principal may be bound to several
  predicates, so there would be no rule for which value wins; a policy key would also make a tuning
  change refuse resume.
- **A window of the last N outcomes**: exact, but a ring of cells per profile proportional to N.

### 3.4 Settings are owner-written

The three settings are system-xanadoc values (`system://settings`), validated at load (NaN, infinite
and out-of-domain values refuse). A stamp is the state under the settings in force at ingest; the
turn record carries the values used, so a later reader can tell.

## 4. Health state (2a)

The state is derived, never stored, by one shared IR routine `rtHealth(profile, limits)` returning
the state and the code of the reason that decided it:

1. `faulted` latched: **FAULTED**, with the fault's code.
1. A `suspect` reason on the profile, or `N >= min_events` and `f >= suspect_fraction`: **SUSPECT**,
   with that reason's code or `refusal_fraction`.
1. A `degraded` reason: **DEGRADED**, with its code.
1. Otherwise **NOMINAL**.

An absent field reads as its initial value (R and N 0, latch false, no reasons), so a tombstone or a
profile restored from before 2a has a state.

**Two consumers, two rules.** Ingestion stamps the state on each record's `health` (today NOMINAL or
FAULTED) for audit and for calibration's view of the source at arrival. The stamp is computed from
the charges made before the ingestion turn ran, so it is pump-order dependent. I4's eligibility and
scoring must recompute the state at query time, through the same routine, without writing (P19 5.1:
queries never advance counters), and must never read the stamp for eligibility: an isolated source's
earlier records stay stamped NOMINAL and are not rewritten. I4 takes the continuous `f` as a smooth
penalty and applies any inflation once (P19 5.1). I4 must also train calibration on readings
whatever their stamp, or on gate-passed readings only, so that excluding SUSPECT readings cannot
stop calibration learning the drift that made them suspect.

A profile created by refusals alone (I3-1 question 3) has no `last_receive`. That state is named,
not special-cased: it has no current record, so nothing of it is eligible; its fraction (which can
read SUSPECT with no reading ever accepted) and latch apply as to any profile; a reset of it is
valid and retires nothing; and I3-3's heartbeat rules treat an absent `last_receive` as "never
supplied a reading", never as time 0.

## 5. Profile health records (2c)

Three owned structures on the profile, all on the I3-1 shapes (records on `d.vars`/`d.values`, lists
on `d.grab`/`d.step`), all copied with the profile on resume:

- **`fault`**, a record, absent until the first isolate: kind `isolate`, the operator's code, the
  grant ID, the issuer, the incarnation at the time, and the wall time when the executor's wall
  clock was synchronised (otherwise absent).
- **`reasons`**, a list of records, unique by (kind, code): kind `degraded` or `suspect`, code,
  grant, issuer, incarnation, wall time. A repeated degrade with the same code adds no reason (the
  first provenance stands) and is recorded only in `actions`. Bounded by
  `vagent.epistemic_health_reasons` (default 4); a new reason past it refuses `EpistemicBudget`.
- **`actions`**, the audit list: one record per accepted action (kind, code, grant, issuer,
  incarnation, wall time; for a reset or clear, the codes it removed). Bounded by
  `vagent.epistemic_health_actions` (default 8), oldest evicted. A reset never clears it, so the
  profile always says that a fault existed, who latched it and who cleared it. The host keeps its
  own log for anything older.

An advance keeps all three, as it keeps the latch: a replacement device inherits its predecessor's
fault and reasons until the host's reset says otherwise (P19 2.1). Each record carries its
incarnation, so a reader can tell a reason placed on the previous boot.

Price: a reason is about 14 cells, an action about 16, the fault record about 14. A profile that
reaches both bounds grows by about 200 cells. Only operators produce these, so the typical profile
pays nothing; the worst case is priced in section 9.

`suspect` reasons have no producer in I3-2; the kind exists so I3-3's telemetry and I4's gates add
reasons without reshaping the list.

## 6. Settle after commit (2b)

Today a source change drops supports through `rtBDrop`, which queues `-pred` events on
`$agent d.pending`, and `rtSettle_` runs their handlers inside the same all-or-nothing turn. A
handler that always fails therefore makes the advance, the retire, and (in 2c) the reset and isolate
refuse, for ever: the repair path fails exactly when a source sends garbage.

From 2b, the entries `sourceChangeEntry` and `healthEntry` end without settling. The turn commits
its state change with the belief events still queued on `d.pending`. The executor then runs, in the
same wake and before any other item for that actor, a **settle turn**: `settleEntry`, emitted IR
that runs `rtSettle_` and halts. If it commits, the events have run as they would have. If it fails
(a handler failed, cycles or fuel ran out, the event depth was exceeded), its undo restores
`d.pending`, and the executor runs `discardPendingEntry`, which detaches `d.pending` and commits, so
no later turn inherits the events. The change's outcome becomes `SettleFailed` (`NeedsHostAction`:
the program's handlers need fixing), its state change stands, and `stat()` counts the discarded
events.

Running the settle turn first is what keeps the semantics: an ordinary turn's own `rtSettle_` would
otherwise pick the leftover events up and fail on their account, moving the wedge into ingestion.

What a program sees: its `-pred` handlers run after the belief change, as before, and in their own
turn, so a handler that fails no longer undoes the source change it reacts to. A handler that needs
the change undone on its failure had no such guarantee for other belief changes either (an ordinary
turn's failure undoes the turn that raised the event, which here is the operator's change, and the
operator's authority wins).

Tests: a `-pred` handler that always fails; an advance, a retire and (2c) a reset each commit, the
outcome reads `SettleFailed`, later ingestion runs normally, and a correct handler still sees every
event once.

## 7. Health grants and actions (2c)

### 7.1 The grant

```cpp
enum class HealthActionKind : std::uint8_t { Isolate, Degrade, Clear, Reset };

struct EpistemicHealthGrant {
  std::string id;                       // audit label, at most kMaxObservationNameBytes
  std::string issuer;                   // 64-hex fingerprint of the operator identity
  std::vector<std::string> sources;     // principals it may act on: 1 to maxSources
  cpp26::inplace_vector<HealthActionKind, 4> actions; // 1 to 4, no repeats
  std::chrono::steady_clock::duration lifetime;       // (0, kMaxHealthGrantLifetime]
};

[[nodiscard]] std::expected<HealthGrantToken, EpistemicRefusal>
grantHealth(const identity::ActorPid &target, const EpistemicHealthGrant &grant);
AgentExecutor *revokeHealthGrant(const identity::ActorPid &target, std::string_view id);
```

`grantHealth` installs the grant for one actor, deny by default, and returns a `HealthGrantToken`:
128 random bits from the executor's random source, kept host-side and never written to the arena.
The ID, which is written into every reason and action and so readable by programs, stays an audit
label and authorizes nothing. Refusals (`InvalidHealthGrant`): an empty or oversized source list, an
empty or repeated action list, a malformed principal, a duplicate ID, a zero or oversized lifetime,
more than `kMaxHealthGrants` (16) grants on the actor, or an issuer that equals a principal in
`sources` or any principal bound to the actor: a sensor identity can never hold health authority
over itself or its peers (P19 4: a sensor cannot clear its own fault). There is no wildcard source.
Grants live in the executor's ingress state beside the bindings: they are host authorization, carry
no learned state, are not checkpointed (as bindings are not), and are erased with the actor's
ingress state.

### 7.2 The request

```cpp
struct Isolate { std::string code; };
struct Degrade { std::string code; };
struct Clear   { std::optional<std::string> code; }; // one code, or every degraded reason
struct Reset   {};
using HealthAction = std::variant<Isolate, Degrade, Clear, Reset>;

struct HealthActionRequest {
  HealthGrantToken token;
  std::string principal;   // the source acted on
  HealthAction action;
};

[[nodiscard]] std::expected<HealthReceipt, EpistemicRefusal>
healthAction(const identity::ActorPid &target, const HealthActionRequest &request);
[[nodiscard]] std::optional<IngestOutcome>
healthOutcome(const identity::ActorPid &target, const HealthReceipt &receipt) const;
```

`healthAction` refuses synchronously `UnauthorizedHealthAction` for an unknown or expired token, a
principal the grant does not name, or a kind it does not allow; `InvalidHealthCode` for an empty or
oversized code; `IngressBackpressure` past `kMaxPendingHealthActions` (16) queued on the actor. The
executor stamps the wall time (when synchronised) and the steady time. A receipt carries a ticket;
outcomes live in their own bounded per-actor log (`epistemic.outcomeLog` entries), never in the
source-change map, whose entries drive binding revocation.

The grant is checked again when the queued action becomes a turn: a token revoked or expired while
the action waited refuses `UnauthorizedHealthAction` into the outcome log, as a queued observation
whose binding lapsed refuses `UnknownSource`.

### 7.3 Order

Health actions are control items, like source changes: they go ahead of queued observations and
behind control items already waiting, in call order. "Control item" becomes one predicate used in
all four places that today test `sourceChange.has_value()`: `PostBox::pushControl`, the `hostEvents`
insertion in the pump, and the `epistemic` and `abandon` lambdas.

A reset must not be undone by evidence received before it. At `healthAction(Reset)` the executor,
under the ingress mutex, bumps the principal's **reset epoch** and drops its coalesced charge entry.
Every queued observation and refusal note records the epoch it was queued under; when one becomes a
turn under an older epoch it refuses `SupersededByReset` (Terminal: the reading predates the reset
and must never count as the new eligible observation P19 5.1 requires). The epoch is host state, not
checkpointed: a checkpoint already requires an empty queue.

### 7.4 The turn

`healthEntry` is emitted Vortex IR beside `sourceChangeEntry`. It reads `$turn.health` (source
token, incarnation when bound, kind, code, grant ID, issuer, wall time when synchronised,
`retained`, the reasons and actions bounds, `max_sources`) and finds the principal's profile,
creating it through `emitProfileFor` when the principal is bound and has none (so a bound source can
be isolated before its first reading). An unbound principal with no profile refuses `NoProfile`
(`NeedsHostAction`: bind it first; a binding carries no readings). A tombstone is acted on like any
profile, and its latch survives a revive.

- **Isolate.** Sets `faulted` first; writes `fault` provenance if absent (a second isolate keeps the
  first and adds an action); then drops every support of the principal through the shared drop body
  (section 7.5), so the source's readings stop supporting beliefs at once, not only in I4's
  eligibility. While latched, ingestion records nothing: the routine refuses `SourceFaulted`
  (`NeedsHostAction`, never charged), and the executor mirrors the latch host-side once the isolate
  commits, so `observe` refuses at ingress without spending arena slots. After a resume the mirror
  is empty and the arena's refusal rebuilds it. Nothing is lost by refusing: the only way out of
  FAULTED is a reset, which invalidates every old reading anyway.
- **Degrade.** Adds a `degraded` reason (unique by code).
- **Clear.** Removes the named `degraded` reason, or every `degraded` reason; never touches the
  latch, `suspect` reasons, readings or (in I4) calibration. A mistaken degrade no longer costs
  learned state. Only reset unlatches FAULTED (P19 5.1).
- **Reset.** Clears the latch and `fault`, empties `reasons`, zeroes R, N and `rejection_streak`,
  and drops every support (section 7.5). The dedup window and streams stay, so replays and old
  sequences still refuse (P19 2.1). `refused_count`, `content_refusals` and `last_refusal` stay:
  they are diagnostics. The action record keeps the removed fault and reason codes. Calibration
  records arrive in I4, which resets them to the declared prior in this same turn (P19 5.1) and
  tests that a reading after a reset is weighted from the prior.

A device reflashed without a persistent boot counter restarts its sequence and refuses
`StaleSequence` after a reset; the runbook is reset, then rebind at a higher incarnation.

**Budgets.** Isolate, clear and reset are exempt from the cell ceiling and from `ArenaExhausted`, as
source changes are from the ceiling: each removes or holds cells except for the bounded provenance
and action records, which are counted in `$epistemic_cells`, so ingestion sees them. Profile
creation for an isolate is exempt from `max_sources` (bounded by bindings). Degrade is admitted
against the ceiling like any ingestion. The safety action therefore cannot be refused by the
exhaustion a flooding source causes, and the ingress mirror stops the flood.

### 7.5 One drop body

The advance's walk in `emitSourceChange` (inline IR: retire records below the new incarnation, drop
the emptied supports, move their records to the retired chain, trim to `retained`) becomes one
emitted routine, `rtSourceDrop(profile, mode, retained)`, called by the advance (mode: incarnation
below), the retire (mode: every record, then excise), isolate and reset (mode: every current
record). `freshWindows` (clearing dedup and streams) stays in the advance's caller only. Every I3-1c
advance and retire test is a gate for this refactor.

## 8. Why Vortex, and the C++ that is not

The estimator, the state derivation, the three structures, all four actions, the drop body and the
settle and discard turns are emitted IR run by the VM. The C++ is grant storage and checking with
the token (authorization), the wall and steady stamps (clocks), queueing, the reset epoch, the latch
mirror and the outcome log (transport), and the choice to run the settle turn after a committed
change (scheduling).

## 9. Vocabulary, budget, settings and tests

**Vocabulary.** `EpistemicRefusal` gains `InvalidHealthGrant`, `UnauthorizedHealthAction`,
`InvalidHealthCode`, `SourceFaulted` and `SupersededByReset`, with `epistemicRefusalName` cases.
Turn results gain `SourceFaulted`, `SupersededByReset` and `SettleFailed`; `outcomeClassOf` maps
`SourceFaulted`, `SettleFailed`, `UnauthorizedHealthAction` and `NoProfile` to `NeedsHostAction`,
`SupersededByReset` to `Terminal`. Constants: `kObsHealthSuspect`, `kObsHealthDegraded`,
`kProfileHealthRefused`, `kProfileHealthEvents`, `kProfileFault`, `kProfileReasons`,
`kProfileActions`, `kTurnHealth` and the record field names, pinned by the ABI test. `CompiledAgent`
gains `healthEntry`, `settleEntry` and `discardPendingEntry`, each remapped in `program.cpp` (a
missed remap is a silent no-op after upgrade).

**Budget.** Worst case per profile: about 200 cells of health records (section 5). Across the
default 128 sources that is 25,600 cells, above the 24,576 ceiling, so the bounds are per profile
and operator actions on many sources can fill the budget: degrade is admitted against it, and the
exempt actions are bounded by the action list. The default ceiling is not raised; the schema notes
say that operators acting on every source of a large fleet should raise it.

**Settings.** Six new `EpistemicLimits` fields with defaults in `epistemic_limits.hpp`, `validate()`
cases and `EpistemicLimitError` enumerators: the three in section 3.3, the two bounds in section 5,
and none for the grant constants (fixed bounds in the header). Each has a `defaultSettingSpecs`
entry, a `settings::` name, the expected list in `system_docs_test.cpp`, and Schema page text with
the interpretation of section 3.3.

**Tests.**

| Slice | Tests                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| ----- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 2a    | the estimator against a closed form (k refusals, m accepts, a coalesced batch); bounds after 10^6 updates; SUSPECT at the threshold and min-events edges, NOMINAL after good readings; stale and time-mismatch refusals and relayed copies move nothing; a 1000-reading stale flush leaves a good source NOMINAL; the stamp; resume keeps R and N; the noisy-sensor simulation's 30% source reads SUSPECT and its 5% peers NOMINAL at 0.1, 1 and 100 Hz; a tombstone and a restored profile have a state                                                                                                                                                    |
| 2b    | an always-failing `-pred` handler: advance and retire commit, `SettleFailed`, no later turn fails on their events, and the next ingestion commits; a correct handler sees each event exactly once, after the change; the settle turn runs before a queued observation                                                                                                                                                                                                                                                                                                                                                                                       |
| 2c    | each refused grant and request; a revoked token while queued; an issuer that is a bound source; isolate before the first reading sticks, drops supports, refuses later readings at ingress and in the arena (after resume too) and fits a full arena; a reboot, a retire and revive, and a checkpoint keep latch, reasons and actions; degrade is unique by code; clear removes only degraded reasons and never unlatches; reset clears latch, reasons, R, N and streak, keeps dedup and actions, refuses a reading queued before it, and the source is eligible only after a new reading; order with a queued rebind; reset with a failing handler commits |

The fuzzer gains health-action steps (each action under a valid, a revoked and a wrong-kind grant),
the invariants that a refused action changes nothing and that a faulted source's later readings
change nothing, and the estimator bounds.

**Cost.** The estimator adds one `Exp` and about 20 instructions to a bookkeeping turn and about 10
to an ingestion; the state adds a reasons walk (at most 4). A settle turn costs what settling cost
inside the change. A reset or isolate is bounded like an advance, O(K·H) records with a settle per
dropped support: a reset of 16 records is measured against the default turn fuel before 2c lands.

## 10. Moved out of I3-2

- **Cross-key retained transfer.** P19 2.1 moves a removed support's retained records "to an owned
  retained-observation list under the keyed calibration record". That record is I4's; until it
  exists, I3-1 keeps every removed support's history on the profile's retired chain, and reset and
  isolate use the same chain. I4 re-homes it, keeping retained IDs, outcomes and floors (P19 2.1).
- **User-level revision.** I3-1a statically refuses every program writer of an arbitrated predicate.
  Letting a program assert or retract one needs the actor AST and lowering that I4 adds.
- **Sensor health reports.** "A sensor's authenticated health report may add a configured warning"
  needs a schema for configured warnings and a telemetry path; it lands with I3-3, adding `suspect`
  and `degraded` reasons to this list, and its recovery percept clears only reasons that source
  added. The self-clear rule holds from 2c: no sensor identity can hold a health grant (section
  7.1).
- **The language directives.** `reset`, `isolate`, `degrade` and `clear source` lower in I4 onto
  `healthEntry`, under the same grant check, with `UnauthorizedHealthAction` outside an authorized
  context. Until then P19 8.2's "unauthorized reset is rejected" is met at the host API only.
- **Gate rejections.** `rejection_streak` thresholds (three SUSPECT, five FAULTED) and the gate
  estimator are I4's, with the gates that produce them.

## 11. Spec amendment

`clear` is the owner's addition to P19 5.1's health actions: "`clear` removes degradation reasons an
operator placed; it never unlatches FAULTED and never touches calibration." The spec's change
history records it with I3-2's acceptance.

## 12. Revision 1 findings and their resolution

| Finding                                                          | Resolution                                          |
| ---------------------------------------------------------------- | --------------------------------------------------- |
| ML M1, M2, hardware M1, codebase m1: count flaps, traffic-bound  | event-indexed fraction with an evidence floor (3.2) |
| ML M3, epistemology M2, hardware M6: latency scored as quality   | only health refusals feed it (3.1)                  |
| ML m1-m3, hardware m2, codebase m2-m3: clamp, `h` domain, ranges | no clock in the estimator; bounded by construction  |
| epistemology M6, ML m3, hardware m6: resume and downtime decay   | nothing to rebase (3.2)                             |
| epistemology M1, hardware M2, codebase M3, M4: reset overtaken   | reset epoch, one control predicate (7.3)            |
| epistemology M3, hardware M5, codebase M7: no audit, forgeable   | `actions` list, token, executor-stamped time (5, 7) |
| epistemology M4: FAULTED changes no answer                       | isolate drops supports and refuses readings (7.4)   |
| epistemology M5, hardware M3: isolate refusable under pressure   | exempt from ceiling, arena gate, max_sources (7.4)  |
| hardware M4, codebase m7: reasons fill, cost half the budget     | unique by code, bound 4, `clear`, priced (5, 9)     |
| hardware M7: wedge blocks reset                                  | settle after commit, slice 2b (6)                   |
| codebase M1: authorized only at queue time                       | rechecked at turn time (7.2)                        |
| codebase M2: advance body not a parameter                        | `rtSourceDrop` refactor, I3-1c tests gate it (7.5)  |
| codebase M5: Int-to-Double breaks readers                        | lifetime counters stay Int; R, N are new fields     |
| codebase M6: absence rules, tombstones                           | absent reads as initial; tombstones acted on (4, 7) |
| codebase M8: vocabulary missing                                  | listed (9)                                          |
| ML m4-m6, epistemology m6, codebase m9: stamp versus query time  | two consumers, two rules; I4 obligations (4)        |
| epistemology m1: self-issued actions                             | issuer never a bound or named source (7.1)          |
| epistemology m4, m5: reason identity, derived reason             | unique (kind, code); `rtHealth` returns the code    |
| epistemology m9, m10: reboot cases, refusal-only reset           | tested (9)                                          |
| hardware minors: grant bounds, outcome map, reflash, suspend     | bounded (7.1, 7.2), own log, runbook, no clock      |
| codebase m5, m6: API shapes, settings plumbing                   | variant, `inplace_vector`, chaining; listed (9)     |
| codebase m10: reset fuel                                         | measured before 2c (9)                              |

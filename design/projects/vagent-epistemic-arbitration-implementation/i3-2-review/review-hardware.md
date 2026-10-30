# I3-2 design review: hardware persona (fault tolerance, real devices, hostile hosts)

Scope: `i3-2-design.md` revision 1 at e226c507, against the P19 spec sections 2.1, 4 and 5.1, the I3-1
code under `apps/common/xanadu/vagent/runtime/`, and the owner's Q2 ruling (latches for life,
decayed rates; not reviewed, only its realisation). Numeric checks: `/tmp/i3-2-hardware/sim.py`.

## BLOCKING

None.

## MAJOR

### M1. The default SUSPECT rule is unreachable for slow sources and misses "three in a row" (design 3.2, 3.3)

The design says three refusals "within much less than a half-life is the same evidence count" as
P19's three consecutive rejections. For a sampled source it is not. With `h = 60 s`, the decayed sum
after `k` consecutive refusals at period `T` is `sum_{i<k} 2^(-iT/h)`:

| Period T           | 3 consecutive refusals | Max ever, all refused: `1/(1 - 2^(-T/h))` |
| ------------------ | ---------------------- | ------------------------------------------ |
| 1 s (1 Hz)         | 2.966 (below 3)        | 87                                         |
| 10 s (0.1 Hz)      | 2.685                  | 9.2                                        |
| 30 s               | 2.207                  | 3.41                                       |
| 36 s and slower    | below 3                | 2.94, so never SUSPECT                     |
| 100 s, 600 s       | far below              | 1.46, 1.00                                 |

So at the defaults any source with a sampling period above about 35 s (the sum reaches 3 only if
`2^(-T/h) >= 2/3`, i.e. `T <= 0.585 h = 35 s`) can never read SUSPECT through counters, however wrong every
reading is. That covers most battery-powered field sensors (minute and hourly reports). Even 1 Hz
needs a fourth refusal for the P19 "three" case. At the other end, 100 Hz at a refusal rate of 1 in
2000 (0.05 per s) settles at `0.05*60/ln 2 = 4.3` and reads SUSPECT, while 0.1 Hz needs about 35%
refused (`3/8.66`). One absolute threshold and one global half-life cannot carry "a declared
sample-rate interpretation" over 0.1 to 100 Hz; the table in 3.3 declares it only for 1 Hz.

Remedy: make the evidence rate-aware rather than adding settings. Options in order of preference:
(a) per-source half-life derived from the binding's declared cadence (a `Binding` already carries a
policy; `h = c * expected_period`, floor at the system setting); (b) a threshold on
`decayed_refusals / decayed_total_events` with a minimum evidence count, which needs a second decayed
counter for accepted events; (c) at minimum, change the default threshold to 2.5 (so three at 1 Hz
trips), state the 35 s limit in the schema notes, and add tests at 0.01, 0.1, 1 and 100 Hz.

### M2. Health actions can be ordered wrongly against source changes and already-queued work (design 6.2, 6.3)

The design says `healthAction` "queues a health turn with `pushControl`, so it keeps call order with
source changes". Two code facts defeat that as written.

1. Ordering is implemented twice, and both key on `sourceChange.has_value()`:
   `PostBox::pushControl` (`agent_executor.cpp:592-598`) and the `find_if_not` over
   `state.hostEvents` in `drainPosted` (`agent_executor.cpp:3758-3763`). A health item stored in its
   own optional stops the leading-run scan, so a later `retireSource`/`bindSource` is inserted ahead
   of a queued isolate or reset, reversing call order. The same field gates `epistemic()`
   (`:3702`, keeps the item while a turn runs) and `abandon()` (`:3695`, records `Undelivered`).
   The design must say that health items are control items in all four places, or give them one
   shared predicate.
1. `pushControl` places the item ahead of every ingestion and refusal note already queued. After a
   `reset`, observations from the faulty source received before the call, and bookkeeping notes for
   refusals already coalesced (`ingress_->charges`), still run and re-establish current readings and
   re-charge the zeroed counters. The reset the operator asked for does not hold at the moment it
   reports success. Isolate is safe (later readings stamp FAULTED); reset is not.

Remedy: for Reset, either run it behind items queued before the call (record the queue position),
or have the reset bump a host-side epoch per principal that `drainPosted` checks beside the binding
check at `:3790` (queued ingestions older than the epoch refuse `Undelivered`) and clear
`charges` for the principal at call time. Add the test "observation queued, then reset, then pump:
no current record".

### M3. Isolate (the safety action) is gated by the budgets a broken source exhausts (design 6.3 last paragraph)

`deliverHost` returns `ArenaExhausted` before any epistemic turn once
`cellCount >= arenaSlots` (`agent_executor.cpp:1776-1782`), and `admit` refuses `EpistemicBudget`
for ingestion and bookkeeping turns. The design gates the health turn "like every epistemic turn".
A flooding or misbehaving device is the usual cause of both. Slots are never reused, at about 250
per ingestion: `2^30 / 250 = 4.3e6` ingestions, which is 11.9 h at 100 Hz (not the 50 days quoted
for 1 Hz). Then the operator cannot isolate the very source driving the exhaustion, and a reset
cannot free anything. Source changes are already exempt from `admit` ("or a full budget would pin a
source to its old boot", `:1768-1770`); health should follow that precedent for Isolate and Reset
(both net-reduce or hold cells), and Isolate needs either a slot reserve above `arenaSlots` or a
host-side quarantine set consulted in `observe()` that takes effect at call time, independent of the
arena. The host-side set also closes the window between the call and the pump.

### M4. Reasons can be exhausted into a state only a destructive reset clears, and they cost half the cell budget (design 5, Q2 in 10)

- The bound refuses `EpistemicBudget` past 8 reasons and nothing deduplicates, so a retrying
  script or a careless operator with a Degrade grant fills a profile with repeats of one code; a
  legitimate later reason, including I3-3's sensor reports, is then refused. The only removal is
  Reset, which discards every current reading and (in I4) the calibration.
- Cost: 8 reasons x about 12 cells = 96 cells per profile, 128 profiles = 12,288 cells, half of
  `kDefaultEpistemicCells = 24576` (`epistemic_limits.hpp:30`) which holds about 18 active principals.
  A Degrade grant over many sources can therefore push ingestion into `EpistemicBudget` for every
  source. Reason text is bytes too: id (unbounded in 6.1, "bounded"), code 256, issuer 64.
- Mistaken Isolate has the same shape: undo is Reset.

Remedy: deduplicate by (kind, code, issuer); bound reasons per profile at 2 to 4 by default and add
a per-actor total counted in the budget; bound grant `id` explicitly; add a non-destructive
clear (below, Q2).

### M5. The grant check authenticates nothing a careless host does not already hold (design 6.1, 6.2)

`request.issuer` is "checked equal to the grant's", but the issuer is in the grant the host installed
and the grant ID is written into every reason (`grant`, `issuer` fields, section 5), readable by
program code and displayed. Anyone who can call `healthAction` and has read a reason can forge a
valid request. The design concedes that authentication of the issuer is the adapter's, but the
check then adds a false sense of security and the audit record names an issuer the caller merely
asserted. Also `atWall` is caller-supplied, so the audit time can be forged though the executor owns
a wall clock (`clock_.wall`, `agent_executor.hpp:120`).

Remedy: `grantHealth` returns an unguessable capability token (128 random bits, not stored in the
arena) that the request must present; the ID stays an audit label. Stamp `at_wall` from `clock_.wall`
in the executor. Record the actions applied (kind, grant id, issuer, steady/wall time) in a bounded
host-side audit ring, because Reset deletes the latch provenance and the reasons it clears, so a
compromised operator's reset leaves no trace of the isolate it undid.

### M6. Backlog and clock-fault bursts are charged as content refusals and recover slowly (design 3.1, 3.3)

`isContentRefusal` includes `StaleObservation` and `ObservationTimeMismatch`
(`agent_executor.cpp:846-849`). A direct sender that buffers during a link outage (a data logger)
and flushes on reconnect submits N stale events; each is a first charge. Charges coalesce but the
count is `n`, uncapped. Recovery from a burst of N below 3: `h * log2(N/3)`: N=100 gives 304 s,
N=1000 503 s, 8,656 (100 Hz wrong RTC for a minute, steady state `100*60/ln 2`) 690 s, 8.7e5 (host
flood) 1,088 s. An RTC step on a 100 Hz sensor is a realistic clock-step fault; the healthy source
sits SUSPECT for 5 to 11 minutes, and a hostile flood can push `h` effects without bound by choosing
the setting maximum (31 years leaves a burst effectively permanent). Coalescing also stamps the
whole note with the newest `at` (3.4), so after a pump stall `d` the older events are over-weighted
by `2^(d/h)`: a 10-minute stall inflates by 1024x, not the "below 1e-4" quoted for milliseconds.

Remedy: cap each charge, `n_eff = min(n, cap)` with a cap of a few times the threshold, and cap
`c` itself (for example at 8x threshold, bounding recovery at 3 half-lives); do not charge
`StaleObservation` to the decaying counter (it measures transport age, and the policy refusal is
already logged), or weight it. State the stall bound in the tests: charge a note after a simulated
10 minute pump stall.

### M7. The `-pred` handler wedge hits reset exactly when the source is faulty (design 6.3, Q3 in 10)

A garbage reading (divide by zero, out-of-range index) is the likeliest cause of a failing `-pred`
handler, and it is the faulty source whose supports Reset must drop through `rtBDrop`. The reset
turn is all-or-nothing, so the repair fails with `TurnExhausted`/`IngestionFault`
(`agent_executor.cpp:1784-1793`, and I3-1 section 14 carried item) and the latch also stays, which
is safe but leaves the operator no path: Isolate works, Reset never does until the program is
changed. Because the outcome code does not name the handler, the operator cannot tell. This should
be solved here, not carried.

Remedy: split Reset into two commits in one queued pair: (1) clear latch, reasons, counters and mark
every current record `retired` (no `-pred` delivery); (2) settle `-pred` for the dropped supports in
ordinary handler turns, whose failure is the program's, reported as a distinct outcome
(`ResetHandlerFailed`) and does not undo (1). Or add the "retire without settling" mode I3-1
already lists as an option.

## MINOR

- **Hysteresis (design 3.3).** "The decayed value crosses once per burst" is true only for bursts.
  For a sustained intermittent fault the sum is shot noise with mean `m = lambda*h/ln 2` and
  variance `m/2`; simulation (seed 1, 3 days) gives 13 up-crossings at 0.3 refusals/min, 165 at
  0.6, 794 at 1.0 (one every 5.4 min), 2,522 at 1.7. Stamps flap for exactly the half-broken
  sensors operators care about. Add a clear threshold of about half the raise threshold, but note a
  deadband needs one stored bit (derived state cannot remember which side it was on); weigh that
  against leaving the dwell time to I4.
- **`h` domain (design 3.3).** `(0, 31 years]` admits denormals; `(t - t_c)/h` then overflows to
  infinity and the VM refuses a non-finite Exp input (`vortex_vm.cpp:928-930`), so every bookkeeping
  turn faults and the charge is lost (`charges` is erased at `agent_executor.cpp:3767`). Set a floor
  (for example 1 s) and reject non-finite thresholds. A compromised settings writer can also set a
  threshold of 1e6 or `h` of 31 years: say in the schema notes that these settings are
  safety-relevant and who may write them, and stamp the values used into the turn record.
- **Health stamp semantics (design 4).** The stamp is computed at `received_steady` but from counters
  that include charges made after receipt and before the queued ingestion turn runs, so "the health
  the source had when the reading arrived" is approximate. Say so, or compute from charge `at`
  times only.
- **Grant lifecycle (6.1, 6.2).** Expiry is checked at call time only; a request queued before
  expiry or `revokeHealthGrant` still runs. `drainPosted` already rechecks bindings for queued
  ingestions (`:3790-3797`); recheck the grant there. Bound `lifetime` (adding a huge duration to a
  `time_point` overflows), the number of grants per actor, `sources` per grant and pending health
  requests per actor: `pushControl` is an O(queue) scan under the mutex that every observation also
  takes, and control turns run ahead of all ingestion, so a loop of requests starves it. `forget(pid)`
  must erase grants.
- **Outcome map (6.2).** If `healthActionOutcome` shares `changes`, a successful health action hides a
  refused advance (the host retries by rebinding, `agent_executor.cpp:3318`), and two queued actions
  on one principal keep only the newest outcome. Use a separate map and return a per-request token
  with a bounded per-principal log like `outcomes`.
- **Repaired device restart (6.3).** Reset keeps streams and the dedup floor, so a reflashed
  device without a persistent boot counter has its new sequences refused (`StaleSequence`, charged to
  `refused_count`, which feeds nothing) until the host advances its incarnation. Document the
  runbook "reset, then rebind at a higher incarnation" and test it, or let Reset take an optional
  new incarnation.
- **Steady clock (3.5, 6.1).** `std::steady_clock` is `CLOCK_MONOTONIC` on Linux and macOS and does
  not count suspend, so decay and grant expiry pause while the host sleeps but not on Windows.
  Harmless in the safe direction; note it with the resume rebase. The rebase also runs in the same
  executor on a private upgrade (`restoreState` is shared), which forgives nothing but also freezes
  decay across each upgrade.
- **Sentinel check.** `HealthActionRequest::atWall{}` and the uninitialised `kind` invite a 0 default;
  once the executor stamps the wall time, drop `atWall`. Absent `t_c` and `fault` are arena
  absences, acceptable. No `noCell` is needed anywhere in the design.
- **Tombstones.** State what Reset and Isolate do to a retired principal's profile (tombstone with
  incarnation floor): suggest both allowed, Reset skipping the record walk.

## Answers to design section 10

1. **60 s and 3.** Not defensible as a global default: see M1 (35 s sampling limit, 1 Hz needs four
   refusals, a 1 in 2000 refusal rate trips 100 Hz). Hysteresis: yes if SUSPECT will gate eligibility
   in I4 (flapping rates in MINOR); no if I4 applies a dwell time itself. Decide before I4 reads it.
1. **Clear degrade without Reset.** Yes. Add `clear` (by code and issuer, removes reasons only) and
   `reinstate` (clears the latch, keeps readings and calibration) as separate action kinds with their
   own grant bits, so a granted Degrade cannot also undo another operator's reasons. The spec lists
   three directives for the language; the host API may carry more, and a mistaken isolate of a fleet
   member should not cost its calibration (M4).
1. **Carried wedge.** Do not carry it for Reset; see M7.

Verdict: FAIL

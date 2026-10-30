# I3-2 design review: ML gate (persona ml)

Scope: `i3-2-design.md` revision 1 at commit e226c507 (sections 3, 4, 3.5, 6.3 reset, 10), against
P19 sections 2.1 and 5.1 (spec lines 560-580), `agent_executor.cpp:696-865` (charge/coalesce) and
`compiler.cpp:4555-4580` (contact clamp). Numbers computed in `/tmp/i3-2-ml/sim.py`. Owner decision
Q2 (latches for life, decayed counters) is taken as given; this reviews how it is realised.

## What is sound

- The update `c <- c*2^(-max(0,t-t_c)/h) + n` is a correct exponentially weighted event count; the
  value at a later time is exact without writing (section 3.2). Steady state `lambda*h/ln2` is the
  correct kernel integral (checked: 1 Hz at 5% gives 4.33). "Burst of 10 falls below 3 in about
  105 s" checks: 60*log2(10/3) = 104.2 s.
- Not decaying `refused_count`, and charging relayed copies only to it (`agent_executor.cpp:829-837`),
  is right: a relay cannot make a victim SUSPECT.
- Deriving SUSPECT rather than storing it, and having queries recompute and never write, is the
  correct shape. Keeping time as a turn input keeps replay deterministic.
- Resume without decay over downtime is the conservative, defensible choice (see MINOR 3).

## BLOCKING

None.

## MAJOR

### M1. The section 3.3 sensitivity claims are false for a stochastic stream; SUSPECT will flap

Section 3.3: "a 1 Hz sensor with a 3% refusal rate stays NOMINAL and one with 5% does not." The mean
(2.60 vs 4.33) is right, but the value is a shot-noise process, not its mean. Variance of a Poisson
decayed count is `lambda*h/(2 ln2)` = 1.30 at 3%, sd 1.14, so the threshold of 3 sits under one sd
above the mean. Simulation (Bernoulli per second, 2e6 s, threshold 3, no hysteresis):

| refusal rate (1 Hz) | fraction of time SUSPECT | transitions per hour |
| ------------------- | ------------------------ | -------------------- |
| 1%                  | 0.7%                     | 4                    |
| 2%                  | 9.8%                     | 35                   |
| 3%                  | 34%                      | 72                   |
| 5%                  | 82%                      | 50                   |
| 10%                 | 100%                     | 0.3                  |

So the "NOMINAL at 3%" source reads SUSPECT a third of the time and changes state every 50 s. Since
the state is stamped on every record (section 4), the same source yields a mix of NOMINAL and
SUSPECT records, and I4 would apply covariance inflation 4 (spec 5.1) to a random third of its
readings. For a periodic refusal pattern the peak, not the mean, decides: one refusal every T s peaks
at `1/(1-2^(-T/h))`, which crosses 3 at T = 60*log2(1.5) = 35.1 s, not the "29 s" the text gives.

Remedy: replace the sensitivity sentence with the simulated table (or the honest statement that the
threshold detects a rate of order 0.03/s with a false-alarm fraction that is steep between 1% and 5%
at 1 Hz), and either raise the default to a value whose false-alarm fraction at the *declared
nominal refusal rate* is below a stated bound (a threshold near 5-6 gives under 1% at 3% on this
simulation class; the owner or I4 can pick) or state that SUSPECT is deliberately sensitive. The
design must declare the sample-rate interpretation P19 5.1 requires; at present it declares only a
rate in events per second.

### M2. "Same evidence count as three consecutive rejections" is not true; the thresholds are not comparable

Section 3.3 equates "three refusals within much less than a half-life" with P19's "three consecutive
rejections". A streak is conditioned on adjacency (no accepted reading between) and so is a
*proportion-like* test. A windowed count is not: it is blind to the accepted readings in between.
Evidence from the simulation: at 1 Hz and 3% the chance of three consecutive refusals is `p^3` =
2.7e-5 per sample, about 0.1 events/hour, against 72 state transitions/hour for the windowed count
(M1). The two rules differ by three orders of magnitude in false-alarm rate at the same nominal
quality. It also scales with traffic, not quality: a 100 Hz sensor at 0.1% content refusals has
steady value 8.66 and reads SUSPECT 99.9% of the time (simulated); a 0.1 Hz sensor at 30% (clearly
bad) has steady value 4.3 and flaps. The count is rate-dependent, the streak is not.

Remedy: state that `content_refusals` is an absolute-rate detector (refusals per minute), not the
P19 consecutive test, and drop the equivalence; or normalise by accepted traffic (a second decayed
counter of accepted ingestions, state on the ratio with a minimum-evidence floor). Without one of
these I4 cannot interpret SUSPECT as "bad fraction of readings". Note the streak itself, which the
design keeps, is the faithful P19 quantity; consider SUSPECT on `rejection_streak >= 3` (when I4
produces it) in addition, as the spec's defaults say, and keep the decayed count for DEGRADED-like
inflation only.

### M3. What counts as a content refusal can drive SUSPECT from conditions that are not the source's

`isContentRefusal` includes `StaleObservation` and `ObservationTimeMismatch`
(`agent_executor.cpp:852-858`). A healthy sensor that reconnects and flushes a store-and-forward
buffer of old readings, or a link with a clock-offset episode, produces a burst of these. The design
charges the batch at full weight, each event at the newest event's time (`Charge::content` counted
and one `contact`; section 3.4). A flush of 1000 stale readings reads 1000 and stays SUSPECT for
60*log2(1000/3) = 503 s; 10 readings for 104 s. That is a transport episode being scored as a
device-quality signal, and it is unbounded per pump (the range argument in 3.2 bounds `c` only for a
minimum spacing Δ; for Δ=0 the bound is infinite: at 1 ms spacing the bound is 86,562, section 3.2's
formula).

Remedy: cap the weight any one bookkeeping turn can add (for example `n` saturating at the
threshold, so one episode is at most one "SUSPECT-worth"), or charge episodes (a refusal after a
gap) rather than events; at least exclude `StaleObservation` from the decayed counter when the
refusal is a late arrival of a previously valid stream, leaving it in `refused_count`. Add a test:
a 1000-reading stale flush from a good sensor.

## MINOR

1. **Out-of-order clamp (3.2).** The clamp is sound but needlessly inexact: when `t < t_c` the
   exact update is `c <- c + n*2^(-(t_c-t)/h)` with `t_c` unchanged, a one-line change in the same
   routine. Errors for the clamp: 1 ms late 1.2e-5, 1 s late 1.2%, 10 s 12%, 60 s 100%. The "below
   1e-4, milliseconds" bound assumes a one-pump delay; the coalescing window is until the worker
   runs the bookkeeping turn (`agent_executor.cpp:696`), unbounded under backpressure, and a batch
   is all stamped with the newest `at`, so older members are also over-weighted by their span.
   Use the exact form, or bound `h` below (see 2). Both cost nothing.
1. **Domain of `h`.** `(0, 31 years]` lets `h` below the coalescing delay, where the clamp error is
   order 1, and denormal `h` where `Δ/h` overflows (gives 0 correctly, but only by luck of
   `Exp(-inf)`). Give a lower bound, for example 1 s, and test `Exp` at -inf and at x=600
   (2.4e-181, fine). 31 years is arbitrary; say it is "finite and below 2^30 s".
1. **Resume rule (3.5).** Agree with it: not decaying over downtime is the right asymmetry (a false
   SUSPECT costs inflation 4 for minutes; laundering costs trust in a faulty source). Two
   observations. It couples recovery to *uptime*, so a deployment that restarts more often than
   `h/ln(…)` cannot decay past threshold: say so in the notes. And checkpointed `t_c` on a profile
   never charged is absent, which is correct; test that rebasing leaves absent absent.
1. **Hysteresis (open question 1).** Not recommended as stored state: M1 shows flapping *is*
   present near threshold, but a two-threshold rule needs the previous state and breaks "derived,
   never stored" and query-time purity. Simulated with clear-at-1.5: transitions drop for p=3%
   from 72/h to 15/h but the SUSPECT fraction doubles (34% to 69%): it trades false alarm for slow
   clearing, not worth it. Better: fix the threshold (M1) and let I4 consume the *continuous*
   decayed value as a soft penalty (inflation as a function of `c`), reserving the discrete state
   for display. State this as the I4 contract.
1. **Stamp versus query-time.** The record stamp is the health at `received_steady`, correct for
   audit and calibration eligibility. But I4's scoring must recompute health at query time (section
   4 says so) and must not read the stamp: a stamped NOMINAL on a record from before a source went
   SUSPECT is stale evidence. Write this as a hard rule in section 4, and note that the stamp
   reflects only refusals already charged, so a refusal received before the record but charged
   after (coalescing) is missed: the stamp is pump-order dependent, not just time dependent.
1. **Feedback hazard for I4.** SUSPECT derived from refusals is independent of calibration residuals
   today. When `reject_count` joins (I4), excluding SUSPECT readings from calibration while
   suspicion depends on innovation rejections creates a selection loop (a drifting sensor is
   excluded, so calibration never learns the drift, so it keeps being rejected). I3-2 is fine; record
   a requirement in section 9 that I4 train calibration on readings regardless of health stamp, or
   on gate-passed readings only.
1. **`degrade` has no operator inverse (open question 2).** Stat view: reset also clears calibration
   to the declared prior (P19 5.1) and retires all readings, which is a large loss of learned
   state to clear what is a metadata flag (a degradation reason says nothing about the learned
   bias). Operators will reset to clear a stale warning and forget months of calibration. Recommend a
   fourth action (clear one reason by code, no effect on learned state) though P19 lists three and
   this needs the owner's approval; at minimum say so in section 6.3 and make `reset`'s cost
   explicit in the operator documentation.
1. **Reset and calibration (6.3, P19 5.1).** The design is correct that I4 must clear calibration to
   the declared prior in the same turn, and that dedup floors and streams survive. One addition:
   the `rejection_streak`, both decayed counters and their `t_c` must all clear (set `t_c` absent,
   not leave stale), and I4's reset test must assert that an accepted-after-reset reading is
   weighted from the prior, not from the pre-reset calibration. As 2b ships before calibration
   exists, add an `I4-owes` line to the carried list so this is not lost.

## Answers to section 10

1. **60 s and 3: defensible?** The half-life is a reasonable engineering default for second-scale
   sensors (a burst of 10 clears in 104 s). The threshold 3 is not defensible as stated: with
   Poisson arrivals it is under one sd above the mean for a 3% refusal rate (M1) and scales with
   traffic (M2). Not hysteresis: see MINOR 4. Prefer a larger default (5 or 6), the traffic
   statement of M2, and a cap per bookkeeping turn (M3).
1. **Clearable `degrade` without `reset`?** Yes, statistically better; see MINOR 7. Needs an owner
   decision because it extends P19's list.
1. **Wedge applies to reset.** Agreed, as for advance and retire; nothing statistical.

## Fit to I4

As designed, health would be a statistically unsound input to I4 scoring *as a thresholded state*
(M1-M3: flapping, traffic dependence, transport episodes scored as device quality). The counters
themselves are sound; I4 should take the continuous decayed value, recomputed at query time, with
inflation a smooth function of it, and the discrete state for display and latch only.

Verdict: FAIL

# I3-2 design review, round 2: ML expert and ML gatekeeper (persona ml)

Scope: `i3-2-design.md` revision 2 (commit `cae4a6ed`, unchanged at HEAD), sections 3, 4, 7.4, 9 and
the "Fit" questions, against P19 sections 2.1, 5.1 and 8.1, code at HEAD `3ae8d384`. Scripts are in
`/tmp/p19-ml/`: `sim.py` (the section 3.3 table), `checks.py` (closed forms, bounds, thresholds,
fresh-profile and relay cases), `order.py` (the "Order" claim), `fresh.py` (evidence floors),
`bound.py` (floating-point bound). Round 1 report: `review-ml.md` (3 MAJOR, 8 MINOR).

## Earlier findings

| Round 1                                                                | Revision 2                                                                                                                                              | Resolved?    |
| ---------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------ |
| M1 shot noise: a thresholded count flaps                               | The estimator is now a fraction `f = R/N` over events. Its noise is stated: "between 15% and 25% the state is noisy by nature" (3.3). Reproduced below. | Yes          |
| M2 "same evidence as three consecutive rejections" is false            | 3.3 drops the equivalence and assigns the streak to I4. `f` is traffic-independent by construction (no clock enters it).                                | Yes          |
| M3 stale or late readings scored as device quality, unbounded per pump | `StaleObservation` and `ObservationTimeMismatch` feed nothing (3.1) and `N <= 1/(1-b)` bounds any flush. I qualify the exclusion in m1.                 | Yes, with m1 |
| m1 out-of-order clamp                                                  | No clock in the estimator.                                                                                                                              | Yes (moot)   |
| m2 domain of `h`                                                       | Now `H` in events, `[1, 10^6]`. The low end creates a new problem, see M3.                                                                              | Partly       |
| m3 resume, uptime coupling                                             | Nothing to rebase; R and N resume unchanged.                                                                                                            | Yes          |
| m4 hysteresis                                                          | Not stored; the state is derived. Continuous `f` is the I4 input (section 4).                                                                           | Yes          |
| m5 stamp versus query time                                             | Section 4, "two consumers, two rules", with pump-order dependence named.                                                                                | Yes          |
| m6 feedback hazard                                                     | Section 4: calibration trains whatever the stamp. Placement: m6.                                                                                        | Yes, with m6 |
| m7 `degrade` has no inverse                                            | `clear` (7.4) removes only `degraded` reasons and touches no learned state.                                                                             | Yes          |
| m8 reset and learned state                                             | 7.4 zeroes R, N and `rejection_streak`, keeps dedup and streams, defers calibration to I4 with a test.                                                  | Yes, with m5 |

## What is sound

- **The update rules are exact.** I implemented both and compared them. The per-accept rule
  `R <- bR, N <- bN + 1` and the batch rule `R <- b^k R + (1-b^k)/(1-b)` (same for N) agree with k
  sequential refusals to 12 digits for k = 1, 2, 7, 50, 1000 (`checks.py` section 1; for k = 1000
  `N` differs at the 1e-8 absolute level, `R` at none).

- **The bounds claim holds in floating point, not only in real arithmetic.** After 10^6 random
  updates with batches up to 40: max N = 46.668046 against the bound 1/(1-b) = 46.668046, `R > N` in
  0 steps, max f = 0.9986. For H = 1, 2, 32, 1000, 10^6 the iterate `N <- bN + 1` never exceeded
  1/(1-b) (`bound.py`). `R <= N` is preserved because rounding is monotone: both fields get the same
  `b` and the same additive term. A batch of k up to 2^62 gives `b^k = 0` and `N = 1/(1-b)` exactly,
  so no flush can push either field past the cap.

- **The simulation table reproduces.** My independent script (Bernoulli refusals, 4e5 events, mean
  of 5 seeds, threshold 0.25, min 3):

  | rate | H = 16       | H = 32        | H = 64       |
  | ---- | ------------ | ------------- | ------------ |
  | 5%   | 0, 0         | 0, 0          | 0, 0         |
  | 10%  | 0.21%, 6.0/h | 0.01%, 0.1/h  | 0, 0         |
  | 15%  | 3.8%, 74.9/h | 0.63%, 12.0/h | 0.03%, 0.4/h |
  | 20%  | 19.4%, 252/h | 11.65%, 128/h | 4.8%, 46/h   |
  | 30%  | 76%, 279/h   | 85%, 150/h    | 93%, 60/h    |
  | 50%  | 99.98%       | 100%          | 100%         |

  The design's entries (for example 12% and 132/h, 85%, 0.7% and 13/h) agree within sampling noise.
  The shape also has a closed form: the effective sample size of the weights is `(1+b)/(1-b)` =
  92.8, so the sd of `f` is `sqrt(p(1-p)/92.8)`: 0.037 at p = 0.15, putting 0.25 at 2.7 sd (0.6%),
  and 1.3 sd at p = 0.20 (11.7%). The "declared interpretation" paragraph is honest. The fraction of
  *accepted* readings that carry a SUSPECT stamp (what I4 would see) is 0.44% at 15% and 9.4% at
  20%.

- **"Events to read SUSPECT": 14 when every reading is refused** reproduces (k = 14). "About 33 at a
  50% rate" is 32 mean-field and 31 for strict alternation: say "about 31 to 33".

- **Section 4 is the right shape.** Derived never stored, absent reads as initial, queries recompute
  and never write, and the stamp is audit only. A refusal-created profile with no `last_receive` is
  named rather than special-cased.

- **Reset's clean slate is complete for the estimator.** There is no `t_c` to leave stale, and a
  charge entry plus the epoch drop old evidence (7.3), so a pre-reset refusal note cannot re-enter R
  or N: it is refused `SupersededByReset`, which is not a health refusal.

- **Vortex implementability: yes.** The estimator needs only `Mul`, `Add`, `Sub`, `Div` and `Exp`.
  `b = Exp(-ln2/H)` with `ln2` a literal, so `Ln` is not needed at all and the "finite-only" `Ln`
  contract is never reached. `b^k = Exp(k * (-ln2/H))`: `k` as Int64 times a Float uses the mixed
  `Mul` path (`vortex_vm.cpp:500-505`), the product is finite for every Int64 k because
  `|ln b| < 0.7`, and `Exp` of a large negative argument underflows to a finite 0, which I1 and P19
  8.1 permit (`vortex_vm.cpp:925-945`). `1 - b` is nonzero for `H <= 10^6` (6.9e-7). Two cautions:
  `Div` by zero silently yields 0.0 (`vortex_vm.cpp:516-520`), so `f = R/N` must be guarded by
  `N >=  min_events > 0` before dividing (a silent 0 reads NOMINAL), and see m7.

## BLOCKING

None.

## MAJOR

### M1. The estimator is asymmetric across the relay path, so the relay can bias `f`

Problem. Section 3.2 updates `N` for "an accepted reading, direct or relayed", and section 3.1
charges a relayed refusal to `refused_count` only (`agent_executor.cpp:882`,
`!relayed && isContentRefusal`). A relayed accepted reading therefore enters the denominator and a
relayed refused one enters neither field. The estimator is then
`R_direct / (N_direct + N_relayed_accepts)`, which is not a refusal fraction of anything.

Evidence. With a true content-defect rate of 20% and a share `s` of events arriving through a relay
(same share for good and bad events), the long-run `f` is 0.200 at s = 0, 0.158 at 0.25, 0.111 at
0.50, 0.059 at 0.75 (`checks.py` section 7). A source whose defect rate puts it in the SUSPECT band
(0.25 or more) reads NOMINAL once about a quarter of its traffic is relayed. A source reached only
through a relay has `f = 0` for ever. Which copy of an event commits is also a race (the second copy
is a duplicate and counts nowhere), so a source's `f` depends on network timing.

Consequence. The detector's sensitivity moves with topology, and a relay (or ordinary multipath)
dilutes the evidence against a bad device. Section 3.1 itself argues that these refusals concern
"bytes the device signed ... whatever the path did to them", which is an argument for charging
relayed copies, not against. The only protection claimed for excluding them is "a relay cannot make
a victim SUSPECT", which an estimator that ignored relayed events entirely would give equally.

Required fix. Make the estimator symmetric and say which rule it is. Either (a) `R` and `N` count
direct outcomes only (a relayed accept updates neither field), recording that a relay-only source
has no `f`, or (b) relayed content refusals also update both fields, justified by the signature
check preceding classification (state the order in `observe`). Add a test: the same 20% stream
delivered 0%, 50% and 100% via a relay yields the same `f` under (b) or no change to `f` under (a).

### M2. The "Order" claim is false: coalescing errs toward trust as well, unboundedly in `m`

Problem. Section 3.2 says a batch charged `m` events late is over-weighted by `b^(-m)`, "it errs
toward suspicion, never toward trust". That is the case where the note runs after later accepted
readings. The code has the opposite case. The charge entry is created at the first refusal and the
bookkeeping note is posted once, at that moment (`agent_executor.cpp:880-891`). The note takes the
whole entry when it is drained (`agent_executor.cpp:3917-3924`), so refusals that arrived *after*
accepted readings queued behind the note's slot are charged before those accepts.

Evidence (`order.py`, H = 32, 60 clean events then `r a^m r^(k-1)` in truth versus `r^k a^m` as
charged):

| m   | k   | f true | f as charged | ratio | b^m   |
| --- | --- | ------ | ------------ | ----- | ----- |
| 4   | 3   | 0.0799 | 0.0753       | 0.94  | 0.917 |
| 8   | 5   | 0.1253 | 0.1087       | 0.87  | 0.841 |
| 31  | 5   | 0.1063 | 0.0599       | 0.56  | 0.511 |
| 60  | 8   | 0.1554 | 0.0463       | 0.30  | 0.273 |

The late-charge direction the design describes is also correct (`f` 0.0822 for the first row, ratio
1.03, up to `b^-m` as stated; R ratio equals `b^-m` exactly at 1.091, 1.189, 1.957 for m = 4, 8,
31). Both directions exist, `m` is bounded only by the ingress queue (64), so the understated case
reaches a factor of 0.25, and the design's own "below 2 for m < H" says nothing about the trust-ward
side.

Consequence. A false safety statement about the direction of an estimator error, in the one place
the design justifies tolerating the error. The effect is small for the common case (k = 1 has no
error) but grows with queue depth, which is when health matters.

Required fix. Either restate the claim honestly (error in both directions, factor `b^(±m)` on the
displaced members, bounded by `queue` events, so within [0.25, 4] at defaults) and add a test that
measures it; or remove it cheaply: close a principal's charge entry when an accepted reading is
queued for that principal after the note was posted, so a batch only ever contains consecutive
refusals and the closed form is exact apart from cross-principal order.

### M3. The evidence floor fails: one refusal in a fresh profile's first four readings reads SUSPECT

Problem. Section 3.3 says "a fresh profile reads SUSPECT after its first three readings if all three
are refused" and `min_events` 3 is meant to keep a thin history from deciding. But `N` after three
events is `1 + b + b^2` = 2.936, below 3, so the floor is crossed at the fourth event (`checks.py`
section 4: N = 1.000, 1.979, 2.936, 3.873). The threshold 0.25 equals 1/4, and with N = 3.873 a
single refusal at the third or fourth position gives `f` = 0.2526 or 0.2582, which is SUSPECT.

Evidence. Probability that a profile reads SUSPECT at some point in its first 12 events, at the
declared nominal refusal rates (200000 trials per cell, `fresh.py`):

| true rate              | 0%  | 2%   | 5%    | 10%   |
| ---------------------- | --- | ---- | ----- | ----- |
| min_events 3 (default) | 0   | 4.5% | 13.0% | 29.9% |
| min_events 8           | 0   | 0.1% | 1.9%  | 10.7% |
| min_events 12          | 0   | 0    | 0     | 0     |

The steady-state table in 3.3 is unaffected; the false alarm lives entirely in the start-up window,
which is precisely what `reset`, a revive of a tombstone and a new binding return to (section 7.4:
reset zeroes R and N). Separately, the domain `H` in `[1, 10^6]` and `min_events` in `[1, 10^6]` are
independent, yet `N` can never exceed `1/(1-b) = 1.4427 H + 0.5`. With H = 1 (allowed) the default
min 3 is unreachable and SUSPECT can never fire; H = 2 needs 7 events to reach N = 3; any
`min_events` above about `1.44 H` silently disables the detector.

Consequence. The operator's clean slate after a reset carries a 13% false-alarm chance at a healthy
5% refusal rate, with inflation 4 applied to the first eligible readings, and a mis-set pair of
settings disables source health without any error.

Required fix. (1) Correct the sentence: four readings, not three. (2) Raise the default `min_events`
to 8 (1.9% at 5%) or require `R >= 2` as well (1.8% at 5%, same cost); state the start-up
false-alarm rate next to the table. (3) Reject at load any `min_events` that is not below
`1/(1-2^(-1/H))` with margin (say 0.9 of it), and raise the lower bound of `H` to at least
`min_events`. (4) Test: a fresh profile with one refusal in four readings stays NOMINAL.

## MINOR

1. **Excluding `StaleObservation` and `ObservationTimeMismatch` is sound as censoring, with one
   split.** The estimator is a proportion over events that reach it; if the two codes touch neither
   `R` nor `N` the proportion is conditionally unbiased for the remaining events, and a flush of
   1000 stale readings changes nothing (each such event is also bounded by the cap). State that they
   update *neither* field; "feed no health rule" can be read as "counted in N". The split: the
   `ObservationTimeMismatch` code covers three conditions. A future-dated reading
   (`agent_executor.cpp:3646`) depends on a host clock and is rightly excluded. A clock-basis
   mismatch (`agent_executor.cpp:3633`: the device signed a basis that differs from the policy's) is
   a signed-field defect of the same kind as `ObservationFrameMismatch` and meets 3.1's own test.
   The IR paths (`compiler.cpp:5369, 5375, 5554, 5565`) need the same classification. Give the basis
   mismatch its own code or reason, or record that a device persistently mis-declaring its basis is
   never SUSPECT and that I3-3 revisits it.
1. **The host cannot supply the health count from the existing note.** `Charge` keeps `content`
   (including stale and time-mismatch), `other`, and only the newest `code`
   (`agent_executor.cpp:748-755`). The IR cannot recover how many of `content` were health refusals.
   Add a `health` counter to `Charge`, a wire field in the note, and its constant to the ABI list in
   section 9.
1. **Event identity.** Count each (incarnation, event ID) at most once in `R` and `N`. Today
   `firstCharge` is once per refusal code (`agent_executor.cpp:800-815`), so one event refused, then
   retried and accepted, or refused under a second code, would contribute twice. Test with a resend.
1. **The recovery figure sits on a knife edge.** "64 events" back to NOMINAL from saturation is
   exact only because `0.25 = 2^-2` and `b^(2H) = 2^-2`: `f` equals the threshold at event 64, and
   the double computed is 0.24999999999999942 (`checks.py` section 3). Under "at or above" the exact
   answer is 65. A different libm can move it. Say "about 64" and do not pin the count in a test.
1. **State what an advance does to R and N.** Section 5 says an advance keeps the latch and reasons;
   the lifetime counters survive it, and 9 tests that resume keeps R and N, but nothing says whether
   a new incarnation inherits the fraction. I recommend it does (same device, new boot;
   event-indexed so it heals on traffic), and that this is written down together with reset's
   zeroing.
1. **The I4 obligations need a home and two refinements.** They exist only as prose in sections 4
   and 7.4. Add an "I4 owes" list that I4 design review can tick: (a) query-time recomputation, no
   stamp for eligibility; (b) calibration trains whatever the stamp, and the wording "calibration's
   view of the source at arrival" should say the stamp may weight, not exclude; (c) a post-reset
   reading is weighted from the declared prior; (d) the smooth penalty is a function of `(R, N)`,
   not of `f` alone, with evidence shrinkage (for example `R/(N + n0)`), because `f` is
   discontinuous at the evidence floor (a single refusal on a new profile is `f` = 1); (e) discrete
   inflation (2 for degraded, 4 for suspect, P19 5.1) and the smooth term combine by maximum, not
   product, so inflation is applied once; (f) the smooth form is an owner-visible reading of P19's
   discrete defaults.
1. **Price the writes.** Section 9's "about 10 instructions" for ingestion omits that R and N are
   two more Double scalar writes per accepted reading, next to `last_receive` and `last_contact`
   (`compiler.cpp:5460-5468`). Arena slots are never reused and a Double scalar spools text, so
   measure slots and scratch bytes per ingestion against `kDefaultEpistemicArenaSlots`'s 250 slots
   per ingestion estimate. Also test that R decaying through subnormals to 0 (about 34000 clean
   readings at H = 32) round-trips through the scalar encoding and a checkpoint.
1. **Reproducibility of the table.** The design cites `/tmp/i3-2-lead/sim.py` and "the test suite's
   noisy-sensor simulation". A path in `/tmp` is not a record. Name the test and its seed, or commit
   the generator under `tools/`.

## Questions for the owner

1. Relay policy (M1): should a source seen through a relay have a refusal fraction, and which path
   do you count when both deliver an event?
1. P19 5.1 gives discrete inflation (2, 4) as defaults. Is a smooth penalty from `(R, N)` in I4 an
   accepted reading of "configurable heuristics", or does it need a spec amendment like `clear`?

## Fit to later iterations

- **I3-3 (heartbeats, clock mapping, sensor reports).** The estimator needs no clock, so nothing
  here blocks mapping or heartbeats. I3-3 must reinstate the censored time refusals once path
  lateness is separable (m1), and use an absent `last_receive` as "never supplied a reading".
  Heartbeat-driven decay must not be added to `f`: silence stays outside it (3.2) and belongs to a
  separate reason.
- **I4 (queries, scoring, gates, directives).** Obligations are listed in m6. The gate-rejection
  estimator should use the same `(R, N)` shape and the same floor, so fix the floor (M3) here; the
  streak rule is separate (3 and 5, P19 5.1).
- **Live-upgrade translator.** `health_refused` and `health_events` are new profile fields with an
  initial value 0, and an absent field reads as initial (section 4), so a profile restored from
  before 2a translates by doing nothing. If a later revision changes `H` semantics, the stored
  `(R, N)` carries the old `b`; record that a changed `H` takes effect from the next update and does
  not rescale history.

Tally: 0 BLOCKING, 3 MAJOR, 8 MINOR.

Verdict: FAIL

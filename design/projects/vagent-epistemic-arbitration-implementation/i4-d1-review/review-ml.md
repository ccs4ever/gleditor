# ML review of I4-D1 (statistical soundness)

**Verdict: FAIL** (2 MAJOR, 6 MINOR). No BLOCKING. Reproduced: the section 7.4 tables (own
pure-Python Chernoff minimisation; all 15 cells agree to 2 digits, W = 92.83, p=0.60 and 0.55 bounds
1.7e-2 and 1.3e-4), the `check_vectors.py` host, perturb and mutant runs, the section 7.2 event
counts (18th and 68th). `numpy` and `mpmath` are not installed here, so `nums.py`, `lcb_table.py`,
`gate_bound*.py` and the reference generator were not re-run; those figures are checked by formula
or by my own code. M4 to M7 are each addressed in substance (see findings for the residue).

## MAJOR

### ML-1. Section 7.4 / 7.5

**The "10^-6 per device-year" guarantee holds only under an independence that real gate residuals
violate; the latch is for life.**

The proof is correct for independent events or blocks of up to 6. Gate innovations from a real
filter are autocorrelated (coloured sensor noise, unmodelled dynamics, a reference that lags a
manoeuvre). The design's own region is m \<= 4 at p \<= 0.15, and the distribution-free bound it
admits is 0.14 per event.

Failure scenario (simulated, `/tmp/mlr/ar.py`, H=64, theta_F=0.70, N_F=48, gate at the 95% two-sided
quantile, so the marginal rejection rate of a healthy source is exactly 5%): a stationary AR(1)
Gaussian residual with lag-1 correlation 0.99 (decorrelation time about 1 s at 100 Hz) latches
FAULTED at event 8,261 (83 s); at 0.999 at event 14,088. At 0.5 and 0.9 no latch in 2e6 events. So
the headline holds for rho \<= 0.9 and fails by many orders of magnitude at 0.99. Spec 5.2 already
says the nominal rate does not survive model mismatch. A reference excursion of about 70 events (0.7
s) latches the whole predicate for life with n_t = 1 or 2, where the 7.5 guard (needs

> = max(2, ceil(n_t/2)) sources) cannot help a single source at all. The old streak of 5 was worse,
> but "proved" is claimed on a premise the section itself lists as unverified, and Q1 asks the lead
> to confirm the number without that caveat.

Fix (any one, plus the test):

- Restate the guarantee as conditional on a stated maximum residual autocorrelation (or run length m
  \<= 4), and put that condition in the user guide and in Q1.
- Make the gate path unable to latch alone: FAULTED from `gate_fraction` requires f >= theta_F on
  two disjoint windows separated in time (for example H and 8H, or two reference epochs), or the
  latch is recoverable by hysteresis (clear at f < theta_S/2 after N_F events) with only
  `isolate`-like severity.
- Add a vector/IR test with AR(1) innovations at 0.9 (must not latch) and 0.99 (documents that it
  does), so the boundary of the region is measured, not asserted.
- Measure the run-length statistic (mean rejection-run length) at runtime and fall to SUSPECT-only
  when it exceeds the proved m.

### ML-2. Section 4.5 / 3.5

**The vectors do not test the compensated-sum claim; the plain-sum mutant passes.**

Section 3.5 and 3.1 justify `sumC` by the 128 mixed-sign logits (plain sum bound 2.5e-11 > guard
7e-12). Running `check_vectors.py vectors.tsv mutant`: `pool` and `fusion` report 0 failures, ratio
0.333 and 0.331. The table in 4.5 lists only decay, logit and shrink as rejected, and the text
"vectors can fail" is not true for the sum. Cause: the 128-source vector (`reference.py:405`)
alternates +13.8 / -13.8 (0.999999, 1e-6), so every plain partial sum is 13.8 or about 0 and the
plain sum is exact. The 2.5e-11 figure needs all positives first (partial sums up to 1768, 128
roundings). Since the canonical order is by ID, an adversarial ID order is reachable in production.

Fix: add a pool vector with 64 positive then 64 negative logits (and a fusion vector with the same
shape on `inv_i * x_i`); make "every mutant is rejected by at least one case of its routine" a test
assertion, including `sum`, and list the result per mutant in 4.5.

## MINOR

### ML-3. Section 7.2 / 7.4.3

**Start-up and record multiplicity are not in the per-device-year figure.**

The 2.3e-7 at (p=0.10, m=4) is per start. A start is a new calibration record, a `reset`, or a new
epoch (counters are per record and zeroed by reset, 7.3). A device with K predicate records and R
resets a year has K+R starts, each adding up to 2.3e-7 on top of the steady-state 1.5e-12 (m=4) that
the headline sums. At the headline region (p=0.15, m=4, 3.8e-7 steady) start-ups are comparable to
the stated total. Fix: state the figure as steady-state plus per-start, with the start count in Q1.

### ML-4. Section 7.2

**No hysteresis, and the interaction of SUSPECT inflation (x4) with the gate is unspecified.**

If the gate tests the inflated variance, a source at true rejection 0.40 goes SUSPECT, x4 inflation
drops its rejection rate (for a 3-sigma bias roughly from 0.4 to about 0.1), f decays below 0.30, it
returns to NOMINAL, and the cycle repeats with a period of tens of events: a flapping state (and an
epistemic-state version bump each flip, I4-1b). It also means a moderately biased source can never
reach 0.70. If the gate uses the uninflated variance, say so. Fix: state which variance the gate
uses; add exit hysteresis (clear SUSPECT below 0.5\*theta_S) and a flap test.

### ML-5. Section 7.5 / Q4: the common-cause guard and per-record counters can be gamed

(a) Per-record counters: a source that sends fewer than N_F=48 tested events per predicate record
never reaches the minimum evidence; fan out bad readings over 50 predicates and none latches nor
reads SUSPECT (its readings are still gate-rejected, so the harm is bounded, but the health state
lies). Aggregate a profile-level counter (all records) beside the per-record one. (b) Guard
threshold ceil(n_t/2): colluding or Sybil bad sources at half of the tested set suppress every latch
on the predicate, including their own, and no guard exists for n_t = 1. State that the guard is a
degrade (still inflated, still excluded when rejected) and bound the suppression duration so a
permanent "reference_suspect" cannot hide a permanently bad source.

### ML-6. Section 7.6: the smoke test has low power against implementation errors

"observed \<= bound and >= bound/100" with observed 2.3e-3 vs bound 1.7e-2: a halved half-life or a
doubled `hit` weight still passes a one-sided Chernoff cap, and the lower bound is two decades
loose. Fix: a differential test — feed the same seeded rejection sequences to the IR routine and to
the reference and compare R, N bit-for-bit on T0 steps and within the `fracStep` cap otherwise —
plus a two-sided binomial interval around the pinned Monte Carlo rate (4,000 chains give +-4%
relative at 2.3e-3 only if events are counted once per chain; define the per-event frequency
estimator, since chains stop at the first latch and events inside one chain are strongly dependent).

### ML-7. Section 4.5: the perturb result contradicts the claim of slack

`perturb` gives `sigmoid` ratio 1.000 (and `logit` 0.74, `libm_*` 0.78): the written bound has zero
slack at 2 ulp, so a host with a 3 ulp `Exp` (older musl, some vendor libm) fails by design, while
the text says the host run's largest error is 0.66 of its bound. Fix: report the perturb maxima in
4.5 and decide whether the assumed L=4u should be 8u in the bound with 4u in the probe (so the
probe, not the vector bound, is what rejects a bad libm).

### ML-8. Section 3.3: the expm1 truncation claim

"17 terms ... truncation under 1e-24 on |y| \<= 1/2": the first omitted term after y^17/17! is
0.5^18/18! = 6e-22 (1e-24 needs about 20 terms). Harmless against a 1e-16 bound, but a stated number
that is false in a design that says everything was computed. Recompute or restate.

## Checks that passed (not praise, scope record)

O9 shrink: pair identity (p_mu - t, q_mu + t) matches the formula, fresh Beta(1,1) contributes 0 at
any c (cold-start Sybil-neutral), monotone in c, odd about 1/2; the order lcb then decay equals the
spec 3.4 composition. Decay by pair form is exact in sum. n_eff endpoints (rho 0, 1) and the 0.8/0.2
marginal example are consistent. The 7.4 block-alignment worst case is the full newest block
(computed at p=0.15, m=4: 3.8e-7 full vs 1.8e-7 to 2.4e-7 partial). Consistency with
`epistemic_emit_health.cpp`: `b` is `Exp(Div(-ln2, H))` identical to `Exp(-(ln2/H))`; at n=1 `geo`
is exactly 1 so the batch update reduces bit-for-bit to `R<-bR+hit, N<-bN+1`; rescale to cap and
`R<=N` guards match the design; `f = R/N` is computed unconditionally in code (Div by 0 is 0) and
gated on `min_events` afterwards, equivalent to the design's text; the 18th/68th event counts hold.

## Not in scope of this document but affects the gate (record for I4-D2/spec)

Beta counts use only verified labels, so a source that tells the truth when checked and lies when
not is invisible to reputation (selective reporting); nothing in I4-D1 can fix it, but the O9 "fresh
source contributes 0" rule makes the first labelled stretch the whole of the cold-start defence. The
Beta(1,1) prior is neutral only for binary claims; for K-way categories 1/K is the chance level.

## Round-1 confirmation

Checked against revision 2 at 72ce6145 (sections 4.5, 7.1 to 7.6, 12, 13 and the files named below).
Nothing from the design's numbers was taken on trust where I could rerun it.

### ML-1: CLOSED

- **Simulation spot-check (own runs of the committed `ar.c`, other seeds).** At rho 0.99, 1e9
  events, seed 7: old rule 63,167 latches (first at 12,102), new rule 0. At rho 0.995, 5e8 events:
  old 95,114, new 0. At rho 0.998, 5e8 events: old 189,402, new 0. The design's 8e9-event old-rule
  count at 0.99 is 503,379, which is 63,000 per 1e9 and agrees. The 0-latch claim reproduces.
- **Bound spot-check (`slow_bound.py` under `/tmp/p19venv`).** H=4096, theta=0.7, m=199 to 200:
  p=0.05 gives 1e-30 and p=0.10 gives 1e-18; m=400 gives 1e-11 and 1e-5. These match the 7.4.2
  slow-pair table, and 1e-11 is the stated bound at (p=0.05, rho=0.995). Block length
  m=(1+rho)/(1-rho) is the correct effective-sample-size mapping for an AR(1) mean.
- The guarantee is now stated as conditional (rho \<= 0.99, or \<= 0.995 at p \<= 0.05), the region
  is printed up to where it fails (rho 0.998 prints ">1"), start-ups are counted as (K+R) starts,
  and the user-guide wording is specified. At 0.998 the simulation latches 0 times although the
  bound is vacuous; the bound is conservative there, not wrong.
- The rule changes the failure from "a lag-1 correlation of 0.99 latches in minutes" to "a
  correlation time above about 400 events is not claimed". It does not remove the dependence, and
  the design says so. A reference that is wrong for longer than the slow window (a few thousand
  events) still latches a lone source (n_t = 1, no guard possible); that is the stated residual and
  is consistent with spec 5.2 ("inconsistency, not which sensor").
- ML-3, ML-4, ML-5a and ML-6 residue (not asked, noted): profile-level counters, the
  nominal-variance gate, the flap table (537 changes per 1e4 events at rate 0.30, hysteresis
  declined with cost stated), the differential test and the pinned AR(1) file are all present. I did
  not re-run `gate_flap.py` or `gate_startup_slow.py`.

### ML-2: CLOSED

`check_vectors.py all` under `/tmp/p19venv` on the committed 704-case file: host and perturb pass
with 0 failures (largest perturb ratio 0.77, no vector at 1.000); the mutant mode rejects all six
mutants by their target routine (`lcb` 243, `decay_naive` 14, `logit_diff` 33, `sum_plain` 23 on
`sum` plus 8 on `fusion`, `var_old` 31, `eps_const` 142). A surviving mutant fails the run, as
specified. The plain-sum mutant still does not fail `pool` (design 3.5 gives the reason: the pool's
libm-dominated bound, 1.9e-13, exceeds the plain-sum error on that vector). That is honest and `sum`
is the targeted routine; it is not a gap in the claim "the compensated sum is exercised". 3 ulp is
rejected at the libm probe, so the ML-7 slack concern is closed as well.

### New findings introduced by the fix

None MAJOR. One MINOR:

- **N-1 (MINOR, section 7.2).** The latch now needs 2,515 events from a clean start (6,812 from f_s
  = 0.05), against the spec 5.1 default of five consecutive rejections. Spec 5.1 and 5.2 state the
  intent as "a rejection establishes inconsistency, not which sensor is wrong" and the latch as
  recoverable only by `reset`; they set no latency requirement, and the safety property that matters
  (a rejected reading enters no fusion) does not wait on the latch. SUSPECT with inflation 4 arrives
  at the 18th event. The cost is wall-clock: 25 s at 100 Hz, 42 min at 1 Hz, about 7 h at 0.1 Hz,
  during which a source that is wrong but intermittently accepted (rejection rate 0.5 to 0.7) stays
  SUSPECT and its accepted readings are used at 1/4 weight. Fix (documentation only): state in the
  user guide that FAULTED from the gate is an event-count latch, and that a low-rate sensor should
  be isolated by the health-timer or operator path instead; list the 0.1 Hz figure beside the 1 Hz
  one.

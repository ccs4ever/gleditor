# I3-2 design review, round 3: ML expert and ML gatekeeper (persona ml)

Scope: `i3-2-design.md` revision 3 (uncommitted), sections 3.1 to 3.4, 9, 11 to 13 and the tests
table, with `decisions.md` "Round-3 rulings" R3-1 to R3-11, against P19 sections 5.1 and 8.1. Code
baseline HEAD `3b86b36d`. This is the owed report: my first round-3 attempt was lost when its
simulation ran for over an hour. Everything below was recomputed with closed forms or vectorised
numpy under `/tmp/p19-ml/r3b/` (each script ran under `timeout 120`, total run time a few seconds).
Scripts: `common.py` (blocked EWMA filter), `relay.py`, `order.py`, `floor.py`, `credit.py`,
`clamp.py`, `table.py`. I did not read the round-3 code paths; the claims about what `observe` and
the pump do are the design's, taken as stated, and I mark where a result depends on them.

## Earlier findings

| Round 2 finding                                      | Revision 3                                                                                                                                     | Resolved?         |
| ---------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------- | ----------------- |
| M1 estimator asymmetric across the relay path        | 3.1/3.2: a relayed copy counts when the refusal is decided on signed fields; one update per (incarnation, event ID).                           | Yes, with m1, m2  |
| M2 "Order" claim false (coalescing errs both ways)   | 3.2 "Order": close-on-accept for `observe`-decided refusals; arena-decided refusals displaced by at most 8 events.                             | Partly: M1 below  |
| M3 evidence floor fails at the fourth event          | 3.3: `min_events` 8, capped at `floor(0.9/(1-b))` by `validate()`; sentence corrected; start-up rates stated.                                  | Yes, with m3, m4  |
| m1 stale and time-mismatch censoring, basis mismatch | 3.1 puts the whole host time class outside `R` and `N`; a device that mis-declares its basis is then never SUSPECT, which is not written down. | Mostly (m5)       |
| m2 host cannot supply the health count               | `Charge::health` and `passed`, note fields `health`, `passed` (3.8 files table).                                                               | Yes               |
| m3 event identity                                    | `Logged::counted` (3.2), now `firstCount` and a high-water mark (R3-3, R3-5).                                                                  | Yes (see rulings) |
| m4 recovery figure on a knife edge                   | 3.3 says "about 64 good readings; tests assert bounds, not these counts". The double is 0.24999999999999928 at 64.                             | Yes               |
| m5 what an advance does to `R` and `N`               | 3.4 last paragraph: a new incarnation inherits them, a reset zeroes them.                                                                      | Yes               |
| m6 I4 obligations need a home                        | Section 13 lists them, including query-time recomputation and max-not-product combination.                                                     | Yes               |
| m7 price of the writes                               | Section 9 gate: slots and scratch per ingestion; test for a subnormal round trip through the scalar encoding.                                  | Yes               |
| m8 the table was cited from `/tmp`                   | The table is now the test `HealthEstimatorSimulation` (seed 1, 4x10^5 events, 3H warm-up, 2 percentage points).                                | Yes (see M1)      |

## What is sound

- **The 3.3 table reproduces.** `table.py` (5 seeds of 4x10^5 events per cell, vectorised) gives
  these shares of events spent SUSPECT and transitions per hour at 1 Hz:

  | rate | H = 16        | H = 32 (default) | H = 64       |
  | ---- | ------------- | ---------------- | ------------ |
  | 5%   | 0, 0          | 0, 0             | 0, 0         |
  | 10%  | 0.22%, 6.2/h  | 0, 0.1/h         | 0, 0         |
  | 15%  | 3.90%, 76.5/h | 0.67%, 12.2/h    | 0.02%, 0.4/h |
  | 20%  | 19.5%, 253/h  | 11.65%, 128/h    | 4.5%, 45/h   |
  | 30%  | 76.4%         | 85.0%            | 93.1%        |
  | 50%  | 99.98%        | 100%             | 100%         |

  Every cell agrees with the design within 0.7 points, so the test's 2-point tolerance on a single
  seed is a sound gate (the sd of a single run is near 0.4 points at the busiest cells).

- **The edge figures hold.** Refusing every reading from a saturated clean history reads SUSPECT at
  event 14; strict alternation at event 31; recovery from `f` = 1 takes 64 good events (the double
  is `0.24999999999999928`, so the count is libm-dependent, and "about 64" with bound assertions is
  the right wording).

- **The floor is right now.** `N` after 8 events is 7.425 and after 9 is 8.266 at H = 32, so the
  ninth reading is the first that can decide, as 3.3 says. At the floor, one or two refusals can
  never reach SUSPECT in events 9 to 13 (best case, newest positions: `f` stays under 0.25); three
  newest refusals give `f` = 0.355 at event 9. So the effective evidence rule at the default is
  "three recent refusals", with no extra `R >= 2` rule needed.

- **`validate()`'s bound is correct and its number is right.** `floor(0.9/(1-b))` is 42 at H = 32
  (0.9 x 46.668 = 42.0012, so the floor is not on a rounding edge), 10 at H = 8, 21 at H = 16 and 83
  at H = 64. The default `min_events` 8 is valid for every `H >= 8`. At the edge (H = 32,
  `min_events` 42) the first eligible reading is the 107th, so a high setting delays but does not
  disable.

- **Start-up false alarm, as stated.** Exact enumeration of all 2^12 outcome sequences (not Monte
  Carlo, `floor.py`) gives P(SUSPECT at some reading in the first 12) = 0.0189 at a true 5% and
  0.1074 at 10% for `min_events` 8, and 0.1303 and 0.2992 for `min_events` 3. These are the design's
  1.9%, 10.7%, 13% and 30%. See m3 for what the 12-event window leaves out.

- **The relay rule is right for what it covers.** With every defect decided on signed fields, the
  long-run `f` at a 20% defect rate is 0.199 to 0.201 at 0%, 25%, 50%, 75% and 100% of events
  relayed (`relay.py`, q = 0 column), against 0.199, 0.111 and 0 for revision 2's rule (my round-2
  figure). The design's sentence is correct for that case.

- **The one-sided displacement bound is arithmetic-correct.** A refusal charged `m` events late is
  over-weighted by `2^(m/H)`: 1.189 at m = 8, H = 32, and exactly 2.000 at m = 8, H = 8, so "at most
  2 for any valid H" holds. Close-on-accept removes the trust-ward case I found in round 2 for
  refusals decided in `observe`, provided the pump drains a note in queue order, which I take from
  the design.

- **Pre-rescale is exact** (see M2 for the claim it supports).

## BLOCKING

None.

## MAJOR

### M1. Applying credits before refusals defeats the pass credit; a healthy source reads SUSPECT

Problem. 3.2 says "a bookkeeping turn applies its pass credits first, then its refusals, so any
within-note disorder errs toward suspicion." That is the right direction and no bound. Once the
per-source queue is full, no accept is queued, so close-on-accept never closes the charge, and
everything the source offers until the pump runs is one open `Charge`: passes (credits) and refusals
interleaved in arrival order. The note then applies all `c` credits and afterwards all `k` refusals,
so every refusal enters as one of the newest events whatever its true position. The pass credit
exists for exactly this case (a stalled pump or a store-and-forward flush) and the tests table lists
it ("stalled pump, 1000 offered, 5% bad: NOMINAL with the credit").

Evidence (`credit.py`, `order.py`). Healthy history `f` = 0.05 at H = 32, a note of `n` offered
readings with true refusal rate 5%. With `N` saturated by the credits the final fraction depends
only on `k`: `f = b^k f0 + (1 - b^k)`.

| refusals k (offered n) | design order f | order-free value (mean over true orders) |
| ---------------------- | -------------- | ---------------------------------------- |
| 1 (20)                 | 0.054          | 0.050                                    |
| 5 (100)                | 0.108          | 0.050                                    |
| 10 (200)               | 0.195          | 0.050                                    |
| 12 (240)               | 0.229          | 0.050                                    |
| 14 (280)               | 0.250          | 0.050                                    |
| 20 (400)               | 0.352          | 0.050                                    |
| 50 (1000)              | 0.661          | 0.050                                    |

Across 20,000 simulated stalls at a true 5%, the share that ends SUSPECT with the design's order is
0% at n = 64, 0.5% at 128, 40.6% at 256, 99.6% at 512 and 100% at 1000. In true order and under
either alternative below it is 0% at every n. For the design's own case (1000 offered, 5% bad) the
mean `f` is 0.658 with the design's order, 0.675 at the peak with no credit at all (final 0.567),
and 0.050 in true order. The credit buys 0.02 of the 0.63 it was introduced to remove. A single
refusal followed by `m` passes in one note is over-weighted by `b^-m`: 1.19 at m = 8, 2.0 at 32, 4.0
at 64, 16 at 128. The queue bounds `m` only for arena-decided refusals; credits are not bounded by
any queue, because `observe` is any-thread and a dropped reading never enters it.

Consequence. The stalled-pump test as written cannot pass against the stated order rule, and the
design's claim that the credit removes load bias is false at the order it specifies. A reconnect
flush of a few hundred readings from a healthy source with a normal 5% refusal rate stamps SUSPECT
for about 45 further readings (until `R` decays under 0.25 N), writes the audit entry, and in I4
would apply inflation 4. The error is in the direction 3.2 names, which is why it is dangerous to
accept as "errs toward suspicion": that statement is true for a bound of 8 and unbounded here.

Required fix. Replace the note's two-phase application by an order rule with a declared two-sided
bound. Either:

- (a) Apply the whole note as one mean-field mix: with `n = c + k` and `n > 0`,
  `R <- b^n R + (k/n) S_n`, `N <- b^n N + S_n`, `S_n = (1 - b^n)/(1 - b)`. It needs one `Exp`, one
  `Div` behind the `n > 0` guard, and no extra notes. It is exact in expectation for any order of
  the events inside the note (`R` is linear in the indicators and `N` does not depend on order), so
  the SUSPECT share is 0 in the table above. Its limit, to be stated: a failure that begins inside a
  long note is seen late. A device that fails for the last 50 of 1000 offered readings has true `f`
  = 0.661 and reads 0.050 after the note; the next 14 or so refusals recover it. Or
- (b) Close the open charge when it reaches `ingress_queue_per_source` events, so each note holds at
  most 8 events and the weight error is within `b^-8` (1.19 at the defaults), as for arena-decided
  refusals. In the 1000-offer case that is 125 notes; mean-field within each chunk gives 0.050 for
  the healthy case and 0.661 for the end-of-stall failure. Alternatively close on alternation of
  kind: about 96 notes per such stall (max 141 in 4,000 trials), exact order. The note capacity is
  the codebase and distributed reviewers' to price.

I prefer (a) for its cost and its unbiasedness, and (b) if the owner wants a hard two-sided bound.
Whichever is chosen, rewrite the sentence in 3.2, replace "NOMINAL with the credit" by a test that
runs the design's actual order (credits and refusals interleaved, queue full, n = 1000, 5% bad), and
add `n` = 256 and 512 cases, where the current order fails.

### M2. The clamp after an update scales `f` by `cap_old/cap_new` on a shrunken `H`

Problem. 3.2 still says "after each update `N <- min(N, 1/(1-b))` and `R <- min(R, N)`, so a
shrunken `H` takes effect at the next update". This is the epistemology reviewer's M3, and ruling
R3-9 accepts the fix (rescale both). I re-derived the defect independently and found a second one in
the corrected form, which the rescale's placement decides.

Evidence (`clamp.py`). A mature source at `N` = `cap_old` and fraction `f0`. After the next accepted
reading under the new `H`:

| H old to new | f0    | rev 3 clamp | rescale | no clamp |
| ------------ | ----- | ----------- | ------- | -------- |
| 64 to 32     | 0.126 | 0.245       | 0.125   | 0.125    |
| 64 to 32     | 0.150 | 0.292       | 0.148   | 0.148    |
| 32 to 16     | 0.150 | 0.284       | 0.147   | 0.147    |
| 128 to 32    | 0.100 | 0.388       | 0.099   | 0.099    |

The factor is `cap_old/cap_new`: 1.99 for 64 to 32, 3.97 for 128 to 32, 30.9 for 1000 to 32. Any
source above `f0` = 0.25 x `cap_new`/`cap_old` becomes SUSPECT at the next reading (0.126 for 64 to
32). The fuzzer invariant in 6.8 (`R <= N <= 1/(1-b)` against the largest bound in force) passes the
distorted pair. Second, a rescale placed *after* the update does not commute with batching: after a
shrink, a batch of `k` refusals and `k` single-refusal updates, each followed by the rescale, differ
by 0.009 in `f` at k = 2, 0.034 at 5, 0.068 at 10 and 0.137 at 30 (accepts: 0.001, 0.005, 0.010,
0.020). That breaks the property 3.2 and the closed-form test rely on (a batch of `k` equals `k`
sequential updates), for exactly one update after each shrink. It is not harmless: the order in
which a flush is chunked then changes the state.

Consequence. A fleet-wide setting change, or a resume under new settings (A2 item 3), flips every
source with a moderate fraction to SUSPECT at once, until `R` decays (about `H` readings). The
corrected rule, if placed after the update, would still make state depend on batch size.

Required fix. Rescale before the update, from the setting in force at that update: if `N > cap_new`
then `R <- R cap_new/N` and `N <- cap_new`, then apply the batch or single update. After the rescale
`N = cap_new` is a fixed point of `N <- bN + 1`, so no clamp can trigger again and the batch rule is
exactly the sequential rule (`clamp.py` section C and `credit.py`: batch and sequential agree to 6
digits for k = 1, 5, 30). Keep `N <- min(N, cap)` only as a floating-point guard. State this in 3.2
and in A2 (the amendment text), and test: `f` unchanged by the rescale for H pairs (64 to 32, 128 to
32, 32 to 16); a source at `f` = 0.15 stays NOMINAL across 64 to 32; batch equals sequential across
a shrink for k up to 30. A growth of `H` needs no step: `N` approaches the new cap (46.7 to 69.8
after 64 readings, 81.3 after 128 for 32 to 64) and `f` is continuous.

## MINOR

1. **The relay claim holds only for signed-field defects.** 3.2 says relay share does not change `f`
   ("0.199 at 0%, 50% and 100%"). 3.1 does not count host-mapped refusals (`InvalidAcquisition`,
   `ClockEpochRegression`) from a relayed copy, so those defects are censored, and the censoring
   depends on the outcome. With `q` the share of defects decided on host-mapped fields and `s` the
   relayed share, the long-run fraction is `p (1 - s q) / (1 - p s q)` (`relay.py`: closed form and
   simulation agree to 0.001). At p = 20%, q = 1: 0.200, 0.158, 0.111, 0.059, 0 for s = 0, 25, 50,
   75, 100%. At p = 30%, q = 1 the SUSPECT share is 85%, 43%, 4%, 0, 0 for the same s. A device
   whose failure mode is a bad clock epoch, reached only through a relay, is never SUSPECT. This was
   a decision (F1(b)) and I do not reopen it, but the sentence should read "for defects decided on
   signed fields", the table of 3.2 should show the host-mapped column, and the test should run both
   q = 0 and q = 1. Record that `ClockEpochRegression` through a relay-only binding is invisible to
   the estimator until I3-3.
1. **A relay on the only path can raise `f` by withholding good events.** 3.1's protection ("a relay
   cannot make a victim SUSPECT") holds against forged outcomes, not against censorship. For a
   device with a genuine bad rate of 2% a sole-path relay that delivers only 6% of good events (94%
   withheld) holds `f` at 0.25; at 5% the figure is 84% (`credit.py`). That cost is small for an
   attacker that already controls the path, but the claim should be stated as "cannot fabricate
   outcomes" and the residual recorded.
1. **The start-up window is longer than 12 events.** 3.3 reports 1.9% and 10.7% "within its first 12
   events". The exposure continues while `N` grows: the probability of at least one SUSPECT reading
   in the first 32, 100, 300 and 1000 events after a reset or a new binding is 2.3%, 2.2%, 2.2%,
   2.2% at a true 5% and 14.3%, 15.2%, 15.2%, 15.6% at 10% (`floor.py`, 10^5 trials; exact
   enumeration for L = 16 and 20 gives 2.1% and 2.2%, 12.3% and 13.2%). The steady-state share at
   those rates is 0, so the false alarm is a start-up effect that settles by about event 100. Give
   the whole-window figures next to the 12-event ones, since reset is the operator's clean slate.
1. **The `H >= ingress_queue_per_source` relation should be a WARN under R3-2.** The relation exists
   so `b^-m <= 2` for arena-decided refusals. `ingress_queue_per_source` is limited only by
   `ingress_queue` (64 by default), so a legitimate per-source queue of 40 makes the default `H` =
   32 fail `validate()`. The relation does not decide whether the estimator works; the bound on the
   weight error is `2^(m/H)` for the configured `m` and `H`, which the design can state. The
   relation between `min_events` and `H` is different: it silently disables the detector, so it
   stays an error. Also note what the smallest `H` buys: at H = 8 the steady-state SUSPECT share at
   a true 5% is 0.12% (2.1 entries per hour at 1 Hz) and at 10% is 2.0% (31 per hour); H = 16 gives
   0 and 0.17%. Add an H = 8 row to the table of 3.3, so that the lower bound is a documented
   choice.
1. **A device that mis-declares its clock basis is never SUSPECT.** My round-2 m1 asked for a code
   of its own, or a recorded consequence. 3.1 puts the clock-basis check in the host time class,
   which is my second option, but the consequence is not written. Add it to section 13 next to the
   I3-3 reinstatement.
1. **`HealthEstimatorSimulation` should assert the whole-window figures and the batching property.**
   The listed cases are the table, the relay shares, the stalled pump, the start-up rates and the
   order bound. Add: the table at H = 8; the start-up rate for 100 events; and a randomised
   batch-versus-sequential check across mixed accept, refusal and credit runs (including across an
   `H` change), since that equivalence is what the closed forms rest on.

## Lead rulings

I read R3-1 to R3-11 for what touches the estimator and its state.

- **R3-3 (`firstCount` gates only the estimator's increments): sound.** It also fixes a problem I
  had not raised: with `counted` gating `firstCharge`, a relayed refusal would hide the direct
  copy's lifetime counters. Revision 4 must say which key `firstCount` uses once R3-5 exists: the
  high-water mark can replace the per-event `counted` flag, or the two can both exist with their
  separate jobs (the flag for same-sequence duplicates inside the log, the mark for replay beyond
  it). Do not leave both without saying which decides.
- **R3-4 (a closed charge moves into its note; the map keeps the open one): sound** and it is the
  mechanism that makes close-on-accept order-exact. The consequence for M1: a note's content is
  whatever was open when the charge closed, so the order rule for a note (M1) is what bounds the
  error, not the closing rule.
- **R3-5 (high-water mark per actor, principal, incarnation, predicate): sound as a defence, with
  three things revision 4 must specify.** (1) *What a mark advances on.* A refusal's sequence is not
  vetted; a device with a glitching sequence counter that emits one garbage sequence near the
  maximum in a refused reading (or a relay that replays one such genuine signed refusal) would raise
  the mark and make every later reading "below the mark", so the faulty device mutes its own
  estimator. Raise the mark only from readings that committed, or by a refusal whose sequence is no
  more than a declared distance (for example the dedup window) above it. (2) *What a below-mark
  event does.* A late genuine reading (reordered by a store-and-forward relay) that was never
  credited or counted lands below the mark; the rule must say whether it updates `N`. If it does
  not, the result is censoring of good and bad events alike, unbiased if the reorder does not depend
  on the outcome, and it should be tested with a reordered stream. (3) *The credited resend.* "Tells
  the arena not to count it again" needs a precise predicate: a commit whose sequence is at or below
  the mark *and* which was credited, not any commit below the mark. A mark alone cannot tell the
  two, so the credited set must be kept (the credit count per note is bounded by M1's fix; the
  per-sequence record is not). The restart bound (one replay pass can count again) is the relay's
  holdings, not 64: a relay that holds 14 or more distinct old refusals can force one SUSPECT
  episode of about 45 readings per executor restart. State that as the bound.
- **R3-9 (rescale `R` and `N` together): sound, and the placement matters.** See M2: rescale before
  the update. Revision 4 must carry it in 3.2, A2 and the fuzzer invariant (the invariant should
  assert `f` unchanged by the rescale, not only `R <= N <= cap`).
- **R3-2 (capacity relations become a WARN plus admission arithmetic): sound for the capacity
  relations; extend it to the `H >= ingress_queue_per_source` relation** (m4), and keep the
  `min_events <= floor(0.9/(1-b))` relation as an error, because violating it disables SUSPECT
  without a trace.
- **R3-11 (the epistemic-state version moves to I4): sound, with one definition to fix.** "Every
  health change ... must increment it once it exists" must not mean every `R` or `N` update, or the
  version moves on every reading. Define a health change as a change of derived state class (or a
  reason, latch or reset), and say that `R`, `N` movement inside a class is not one.
- **R3-1, R3-6, R3-7, R3-8, R3-10:** no estimator content. No ML objection.

## Questions for the owner

1. M1 option (a) accepts that a failure beginning inside a long note is seen late (about one note,
   then 14 refusals). Option (b) costs notes. Which does the owner prefer?
1. Should the SUSPECT state, which in 2a feeds only display, audit and the stamp, be allowed to fire
   from a note that holds more than `ingress_queue_per_source` events at all? Under (a) the answer
   is yes and bounded; if the answer is no, a flush should be applied as censored events.

Slice verdicts (ML discipline only):

- 2a: FAIL (M1, M2).
- 2b: PASS (no estimator content).
- 2c1: PASS (no estimator content; the host API does not touch `R`, `N`).
- 2c2: PASS (reset zeroes `R`, `N` and the streak; no new estimator rule). It inherits M2 only if
  the health turn applies the clamp; the rescale rule belongs to the 2a update routine.

Tally: 0 BLOCKING, 2 MAJOR, 6 MINOR.

Verdict: FAIL

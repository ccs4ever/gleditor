# Remaining-work plan review, round 2: ML expert and ML gatekeeper (persona ml)

Scope: `remaining-plan.md` draft 2 (uncommitted), `traceability.md`, `decisions.md` section 2 (O1 to
O10, E1, T0, D1 to D3, amendments A1 to A4), `i3-2-design.md` revision 3 where the plan relies on
it, and my round-1 report `plan-review/review-ml.md`. Checked against P19 v1.5 sections 3.1 to 3.4,
5.1 to 5.3, 6.3 and 8.1. The code differs from the plan's baseline `3ae8d384` only in documents
(`git diff --stat 3ae8d384..HEAD -- . ':!design'` is empty), HEAD `8a4089d9`. Scratch work is in
`/tmp/p19-ml/`: `rows.py` (matrix against slice assignments) and `r2_gate.py` (false-latch bounds
and test power; round 1's `plan_gate.py` is the earlier record).

## Earlier findings

| Round-1 finding                                         | State                                 | Where, and why                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| ------------------------------------------------------- | ------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| M1 provisional acceptance, fixtures write learned state | Resolved, one gap (M1 below)          | Section 3 B and I5-1a. `ledgerApply` is the single writer of alpha and beta, lands right after I4-2, is driven by a harness (the I3-2c1 precedent), and section 2 drops provisional acceptance and requires "claims passed, claims reserved" in each ML report. I4-2 passes formulas only; I4-6 claims reputation separation through `ledgerApply`. The gap: nothing says how a reset acts on the ledger.                                                                                                                                   |
| M2 "freeze the whole record" wrong in content           | Resolved                              | Section 3 C and I4-D2 freeze the envelope. The two record kinds carry keys and parents (I4-1b: full key with source, parented on the profile; I4-1a: domain policy record owning the group registry). The key table is closed once in I4-1a, `online_sgd` stays refused until I5-4, and SGD internals are additive under the shape-version rule (section 2, "Version rules"). The alternative-set declaration is an I4-1a key. Residual: minor 7.                                                                                           |
| M3 ledger cap makes a source stop learning              | Not resolved (M2 below)               | The cap moved to a design, which is right, but the plan and `decisions.md` disagree on which design, and the one the plan names is drafted before the structures it prices exist.                                                                                                                                                                                                                                                                                                                                                           |
| M4 gate-rejection streak latches FAULTED on noise       | Resolved as a routing, open as a rule | O10 goes to I4-D1; I4-8 starts from my recommendation (shared routine and floor, separate state, settings and nominal rate; fraction for SUSPECT and FAULTED with a minimum event count; a streak only with a window), and tests rejection-trains-nothing. Minor 2 and 3 below: two records still pre-commit to the streak, and the 10^7-event test has no power at the rates that matter.                                                                                                                                                  |
| M5 cross-host contract incomplete                       | Resolved                              | Decision D1 and section 2 "Numeric decisions" carry all six parts: high-precision reference (I4-D1); stored bits authoritative, never recomputed on resume or upgrade (I5-4, T2, each parity test); same-host replay bit-equal (I5-4); margin on every decision vector, fail below 1e-12 except exact ties; no boundary-pinned counts (I3-2a says "about 64 events over a band"); the user-guide sentence (F1). VM-owned `Exp`/`Ln` is the recorded rejected alternative (7.2).                                                             |
| M6 reference vectors not independent                    | Resolved                              | I4-D1 writes the reference before any IR, reviewed as an artefact, at 50 digits with `decimal`, with closed forms, metamorphic properties, a seeded differential run and an SGD one-step case; I4-2 states the vectors are "not by its author". I checked that CPython's `decimal` `exp` and `ln` are correctly rounded and do not call the host libm (`Decimal(0.1).exp()` gives 1.10517091807564763094..., which differs from `math.exp(0.1)` in the 17th digit), so the objection to a libm-sharing reference is met. Residual: minor 8. |
| M7 `lcb` at an uninformative prior                      | Resolved as routing                   | O9 goes to I4-D1; I4-2 carries "`lcb` at a symmetric prior behaves as I4-D1 ruled" as a test; a formula change is a proposed amendment. Acceptable: the rule has a named owner, a named test and a named consumer of the golden vectors.                                                                                                                                                                                                                                                                                                    |
| Minor 1 gate placement                                  | Resolved                              | Named-item ML reads are in section 5 for I3-2c1, I4-5, I4-7a, I4-10, I3-4b1.                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| Minor 2 tie tolerance saturates                         | Resolved as routing                   | I4-D1 (W-space or logit space) and an I4-3 test.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| Minor 3 weaker corroborating source                     | Resolved                              | I4-3 test (0.9 and 0.55 give 0.768) and an I4-11 example.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| Minor 4 priors-only demos                               | Resolved                              | Section 3 B and I4-6: a priors-only run says it selects by age; the guide shows no "most reliable source wins" before labels exist.                                                                                                                                                                                                                                                                                                                                                                                                         |
| Minor 5 O6 and 5.2-6 need authority                     | Not resolved (minor 1)                | Section 9.2 says the profile-variance drop is a proposed amendment in `decisions.md` and that row 5.2-6 is marked Deferred there. Neither is true of the files.                                                                                                                                                                                                                                                                                                                                                                             |
| Minor 6 receive order against first-accept order        | Resolved                              | Section 4.1 puts it in I4-D2 and I5-D.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| Minor 7 abstention criterion                            | Resolved with a seam                  | I5-D defines it. Seam: minor 7 below.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| Minor 8 `decay` semantics                               | Resolved                              | I4-D1 settles; I4-1a implements (absent means lambda = 0).                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| Minor 9 F2 Standing check, stable-sum test              | Resolved                              | I5-4 re-runs row 8.P6-3; I4-2 lists the 128-logit mixed-sign case.                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |

## What is sound

- **Ordering of the learning path.** I5-1a after I4-2 and before I4-6 gives I4-3 and I4-4 a fixture
  route that is the production writer. I4-6 can then claim reputation separation, and I5-1b the
  whole loop. This is the option I asked for and it removes the second write path.
- **Matrix closure.** `rows.py` reads `traceability.md` (468 rows: 231 Missing, 111 Partial, 90
  Done, 12 Designed, 9 Deferred, 8 Standing, 7 Informational) and the `**Closes.**` lines of the
  slices. All 354 open rows are assigned, none twice, and no Done, Deferred, Standing or
  Informational row is assigned. The "rows closed" column is therefore true as written.
- **The envelope rule.** The shape version, the state counter and the additive-field rule give the
  translator a stable seam without pretending SGD internals are known (section 2). The three numbers
  (policy tag, shape version, state counter) are kept apart.
- **Single key-table bump.** One `vagent-policy-3` with every key of 3.1 to 3.4 and 6.3, with `-4`
  only as an escape hatch, is correct. It avoids a hash change per slice.
- **Numerics in the VM and the reference at 50 digits.** Using `decimal` instead of `math` removes
  the shared-libm blind spot; margin-carrying vectors and the "about 64" wording remove the flaky
  boundary tests.
- **Translator parity.** Bit equality for alpha, beta, the ledger, weights and normalizer through
  both copiers is the property that makes cross-host disagreement confined to new computation.
- **W1 and W2.** They put the learning loop in a binary a user can run, which is the only way the
  user-guide claims about ranking become testable. No ML gate is needed beyond minor 9.

## BLOCKING

None.

## MAJOR

### M1. A health reset and the ledger: the next label brings the old learning back

Problem. Spec 5.1 says reset "clears calibration/learned state to the declared prior". Plan I4-1b
makes a reset call "a calibration reset in the same turn (5.1-13)" and tests that "reset returns it
to the prior". I5-1a then defines alpha and beta as the epoch prior plus the sums over the **whole
ledger**, recomputed on every apply. These two rules do not meet. After a reset the ledger still
holds every earlier entry (clearing it belongs to "an authorized epoch reset", row 6.3-20, closed at
I5-5). The next `ledgerApply` recomputes from the prior plus all old entries, so the first new label
restores everything the reset removed. A source that drifted, was labelled badly, was repaired and
reset is silently put back to its drifted reputation. No slice owns the interaction: 5.1-13 closes
in I4-1b (before the ledger exists), 6.3-20 in I5-5 (three slices after the ledger is first
written), and the reset epoch and `retired_by` of I3-2c1 are said only to "expire labels later".

Evidence. `remaining-plan.md` I4-1b ("A reset calls a calibration reset in the same turn"), I5-1a
("recomputes alpha and beta from the epoch prior plus the ledger sums ... never incrementally"),
I5-5 ("an authorized epoch reset clears the ledger"); `traceability.md:358` (5.1-13, closed I4-1b)
and `:486` (6.3-20, closed I5-5); spec 5.1 (`vagent-dynamic-...md:570`) and 6.3 (`:825`). I4-1b's
own test list would pass, because the ledger does not exist yet.

Consequence. The slice that first writes labels (I5-1a) passes its tests and its ML gate while the
reset guarantee of 5.1 is false for any source with a label. A second effect: the turn budget of
I3-2c1 ("a 16-record reset within half the grant") is measured before this work, and clearing a
ledger of L entries for each of up to 16 dropped records adds O(16 L) to the same turn, which that
measurement does not cover. Third, a source at its ledger cap (M2) has no remedy before I5-5 unless
reset is the remedy, so the cap and the reset must be designed together.

Required fix.

1. I4-D2 states the rule. Recommended: a reset opens a new calibration epoch for each of the
   principal's calibration records in O(1) (the epoch is part of the record key and of each ledger
   entry), the recompute reads only entries of the current epoch, and entries of an earlier epoch
   are history that no longer counts against the cap (excised in bounded steps, or counted
   separately). The other option, excising all entries in the reset turn, must state its cost and
   its fuel gate. The rule must say what a pending label for the old epoch gets (`ExpiredLabel`).
1. I5-1a adds the tests: reset, then apply a label, and the result equals the prior plus the new
   label only; a reset of 16 records with full ledgers fits the grant (budget parity); a label
   naming an observation from before the reset is refused.
1. Matrix: 5.1-13 gets a second closing slice, or its last slice moves to I5-1a with I4-1b as the
   first; the plan says so in the I4-1b and I5-1a **Closes** lines.
1. I4-1b's file list names `epistemic_emit_health.cpp`, since `healthEntry`'s reset turn changes.

### M2. The ledger cap is owned by two documents and priced before its structure exists

Problem. This is round-1 M3, not closed. The cap is the single number that decides whether a source
can learn at all, because Beta is recomputed from the whole ledger and `EpistemicBudget` is the
refusal at the cap (spec 6.3). Draft 2 moves the decision into a design, which is right, but the
records disagree and the order is wrong.

- `decisions.md` O7: "**I4-D2** prices every structure and sets the label and ledger caps".
- Plan section 4.4 and 7.1 O7: **I4-D1** "prices every structure and sets the label and ledger
  caps", and I4-1b, I5-1a and I4-11 refer to "the values I4-D1 measured".
- Plan section 4.1 starts I4-D1 now (it "reads only the spec and the accepted I1 record") and I4-D2
  later, after I3-2D. A design that reads neither the ledger entry's field list nor the record
  layout cannot price them. The entry's field list is I4-D2's deliverable.

Evidence. `decisions.md` section 2, row O7; `remaining-plan.md` section 4.4 (I4-D1 and I4-D2 rows),
section 4.1 (the parallel table), section 7.1 (O7).

Consequence. Whichever design is read as the owner, the other cannot be accepted: if I4-D1 sets the
cap it does so without the field list; if I4-D2 sets it, I4-D1 is accepted with a required topic
missing. In both cases the figure round 1 asked for (a measured cell price) has no source, because
"measured on the IR" is not possible in a design that precedes the IR.

Required fix.

1. One owner: I4-D2, because the price follows from the entry's fields. Remove the cap and cell
   price from I4-D1's scope in section 4.4, and correct section 7.1 to match `decisions.md`.
1. I4-D2 states: the cap per (source, domain, epoch), its default, the cell price of one entry
   **computed from the field list and confirmed by building one entry in a throwaway fixture** (it
   needs no actor IR), and the minimum number of labels per source the default must hold. The
   minimum matters: with 24,576 cells, 128 sources and 40 to 60 cells per entry (my estimate, not
   measured), the ceiling holds a few hundred labels in total; the design must state the number it
   accepts and what the operator does at the cap, which is M1's epoch reset.
1. I4-D2 states the interaction with the checkpoint limit (65,536 value operations), because a full
   ledger is the largest single structure.

## MINOR

1. **Record discrepancy on 5.2-6 and the profile-variance drop.** Section 9.2 (minor 5) claims a
   proposed amendment in `decisions.md` and a Deferred mark on 5.2-6. The amendments table has A1 to
   A4 only and `traceability.md:378` still lists 5.2-6 as Missing, closed by I4-8. The decision is
   delegated to I4-D1, which is acceptable; correct the sentence, or add the row when I4-D1 rules.
1. **O10 text and the I3-2 design.** Section 7.1 O10 says amendment A2 "keeps consecutive streaks
   for gate rejections"; `decisions.md` A2 says gate rejections "are I4-D's (O10)". The I3-2 design,
   which I3-2a to I3-2c2 are built from, still commits the opposite way: `i3-2-design.md:195` ("a
   streak ... that I4 adds to the state rule with its own thresholds (three SUSPECT, five
   FAULTED)"), `:975` and `:1111` ("`rejection_streak` thresholds apply to the state rule only").
   Align all three with O10 in I3-2D, so I3-2 does not pre-commit the rule I4-D1 must be free to
   choose.
1. **False-latch test has no power at the rates that matter.** I4-8 and the risk table test "10^7
   simulated healthy events at p = 0.05 and 0.01". With zero latches seen in n events the rate is
   only bounded by about 3/n (script `r2_gate.py`): 3.0e-7 per event, which is one latch per 9.3 h
   at 100 Hz, 93 h at 10 Hz, 926 h at 1 Hz. A 5-streak at p = 0.01 latches once in 1.0e10 events
   (round-1 table) and 10^7 events cannot tell it from never. Neither the requirement ("the rate a
   fleet can accept") nor the method is stated. I4-D1 should (a) state the accepted false-latch rate
   per device-year at a stated event rate, and (b) prove it by a bound, not by simulation. For the
   event-indexed fraction at H = 32 (n_eff = 92.3) the weighted Chernoff bound per event is 8.7e-9
   at threshold 0.25 and p = 0.05, 2.1e-31 at threshold 0.5, and 4.8e-57 at 0.5 and p = 0.01
   (script; a Monte Carlo at p = 0.10 and threshold 0.25 gave 2.2e-5, under its bound of 3.4e-4).
   The simulation then serves as a smoke test of the implementation against the bound.
1. **Which health changes bump the state counter.** I3-2c1 says health changes bump it. `R` and `N`
   change on every ingested or refused event; bumping on each would stale every `ranked`
   continuation on traffic alone, and bumping only on a belief change misses a refusal run that
   moves a source to SUSPECT. Say in I3-2D: bump on a change of the **derived state** (and on any
   action), never on `R`/`N` drift.
1. **Reuse of the health routine.** I4-8 shares "the update routine and the evidence-floor rule"
   with I3-2a. I3-2a should emit that routine parameterised by the two state cells, the decay factor
   and the floor (not hard-wired to `health_refused_weight` and `health_weight`), or I4-8 rewrites
   accepted code. A sentence in I3-2D is enough.
1. **"Pure scalar routine" and list-valued algorithms.** Section 2 says each `std:` numeric is "a
   pure scalar routine with a fixed call ABI". Pooling (a sum over a candidate's sources), the
   single-linkage walk (I4-9) and SGD (vectors of the feature count) are not scalar. State where the
   summation loop lives (the emitted query IR calling scalar `std:` routines per element, or a
   `std:` routine over an arena list head) and how the bare-`WorkerContext` unit test feeds it. This
   decides how the stable-sum test of I4-2 is written and whether the sum order is a numeric
   decision of the reference.
1. **Abstention keys before the criterion.** I4-1a closes the key table with "the abstention
   parameters" in I4-1a, while the criterion that fixes them is I5-D's (my round-1 minor 7). The
   `vagent-policy-4` escape in I5-3 covers a miss. Better: I4-D2 fixes the criterion's shape
   (prequential loss against the Beta baseline, minimum sample, one margin) so the key names are
   right the first time; I5-D fixes the numbers.
1. **Reference file format.** Say in I4-D1: inputs are stored as hex-float bit patterns (so
   `Decimal(float)` is exact), outputs as 50-digit decimal strings, the comparison rounds the
   decimal to a double and states the allowed relative error per routine from its operation count,
   and the reference imports no `math` or `numpy` function for `exp`, `log` or `sqrt` (a test greps
   for it).
1. **ML read of the guide in W1 and W2.** Both edit `docs/vagent-user-guide.md`. The guide is where
   "most reliable source wins" before labels, or a score read as a probability, would first appear
   (round-1 minor 4). Give W1 and W2 a named-items ML read of the guide text only.
1. **Digest for the translator.** T1-D defines a digest "equal modulo" the listed differences. State
   that doubles compare by bit pattern (so `-0.0` against `0.0` differs), since bit equality of
   learned state is the claim.
1. **SGD-only keys before I5-4.** I4-1a adds `eta0`, `lambda_reg`, clip and floor while `online_sgd`
   is refused. A program that sets them under `beta_reputation` would be accepted and ignored. The
   checker should refuse an SGD-only key under any other model with a typed error. Tests of I4-1a
   ("every new key has a boundary test") then include the refusal.

## Questions for the owner

None new. Two items stay with the lead:

1. Within M1, whether a reset forgets labels (the 5.1 text, my reading) or keeps them as history
   that no longer scores. The recommendation keeps them as uncounted history, which is within the
   approved spec.
1. The accepted false-latch rate per device-year (minor 3) is a fleet-policy figure; I4-D1 should
   propose one and the lead confirm it.

## Answers to the round-2 brief

**Plan section 10, ML questions.**

- *Is I5-1a after I4-2 enough for I4-3 to I4-6 to claim what they claim?* Yes for the claims the
  plan lists: formulas and extremes at I4-2; pooling arithmetic and the group-count property at
  I4-3/I4-4; reputation separation at I4-6. It is not yet enough for any claim involving a reset or
  a cap, which M1 and M2 close.
- *Do the keys of I4-1a cover every parameter of 3.1 to 3.4 and 6.3?* Yes by my reading of the spec:
  `risk`, `c`, `epsilon_w`, `epsilon_floor`, `rho` (a group field, not a policy key), SGD
  hyperparameters, warm-up, horizon, abstention, features, the alternative-set declaration, `decay`,
  `score_tolerance`. Missing only the shape of the abstention rule (minor 7).
- *Is `decimal` at 50 digits independent enough of the VM's libm?* Yes: it is correctly rounded and
  does not call libm (checked above). 50 digits is far more than the 17 a double needs.

**Can the next work start on this plan?**

- **I3-2 implementation (I3-2a, 2b, 2c1, 2c2).** Yes from the ML side, after I3-2D revision 3 passes
  its own gate (that is the only blocker; this plan does not add one). Before the design is
  accepted: minor 2 (align the streak sentences), minor 4 (the bump rule) and minor 5 (parameterise
  the routine). None blocks I3-2a. M1 does not touch I3-2: it needs only that I3-2c1 keeps the reset
  epoch durable and per-principal, which the design already does.
- **I3-3D.** Yes. ML is named as a reviewer; no change needed.
- **T1-D.** Yes. Add the bit-pattern digest rule (minor 10).
- **I4-D1.** Yes, after the plan makes these changes: remove the cap and cell price from its scope
  (M2); add the false-latch rate and the bound-based method (minor 3); add the reference file format
  (minor 8). It must not wait for I4-D2 for anything else.
- **I4-D2.** Not before the M1 and M2 rules are in its brief, and not before I3-2D is accepted (as
  the plan already says).

Tally: 0 BLOCKING, 2 MAJOR, 11 MINOR.

Verdict: FAIL (narrow: both MAJOR findings are one-paragraph corrections to the plan and the briefs
of I4-D2, I4-1b and I5-1a)

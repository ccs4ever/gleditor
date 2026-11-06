# P19 completion: lead decisions and proposed spec amendments

Authority: the owner's instruction of 2026-11-02 to complete P19 in iterations with worker agents,
gated by expert reviewers. A lead decision chooses between options within approved P19 v1.5 and the
owner's earlier decisions. A decision that changes what the spec says is also listed under "Proposed
amendments". Those are folded into spec v1.6 at F1, marked pending, and take effect as spec text
only when the owner ratifies them. Reviewers may contest any decision with evidence.

## 0. Review cadence (owner steering, 2026-11-02)

The owner directed that work is not reviewed after every small change. From here:

- Each iteration's design is reviewed by its personas until no BLOCKING or MAJOR finding is open. A
  fix that implements a lead ruling or a MINOR point is verified by the lead, without a confirmation
  round.
- Implementation slices land on the lead's build and the full headless suites. Code findings are
  fixed by a worker and verified by the lead with tests.
- Each iteration gets one multi-persona implementation review before acceptance. For ML-related work
  the ML expert is in it, and an open ML MAJOR finding closes only on the ML expert's confirmation
  (the README gate).

Acceptances under this rule: the I3-2 design, revision 4, proceeds to implementation (round 4 PASS
from architect, distributed and codebase; ML, epistemology, hardware and security confirm their
round-3 findings in the I3-2 implementation review). Remaining-plan draft 3 is the working plan
(round-2 MAJOR findings resolved by P2-1 to P2-6; open MINOR points are fixed in later updates). E1
is accepted (codebase and architect PASS); its MINOR m1 (`inline constexpr`) goes to 2a.

### 0.1 Milestone reviews (owner steering, 2026-11-04)

The 2026-11-02 rule still produced four review rounds of up to seven personas for I3-2 alone, about
1.1 MB of reports, and the lead ran out of budget before I3-2c1 closed. The owner directed a further
cut. This section supersedes the per-slice gate table in `remaining-plan.md` section 5 and the "fix,
then re-review" procedure under it. The README's ML gate is unchanged: the ML expert reads each ML
design before implementation and the source before acceptance, and no ML work is accepted with an ML
BLOCKING or MAJOR finding open. It is now applied per milestone, not per slice.

- **The slice is the commit unit, not the review unit.** A slice lands when the lead's
  `make -j$(nproc)` and the full headless suites pass. No persona reads a single slice.
- **One review round per milestone**, at most three personas, run in parallel. Each reads the git
  range for the milestone and the design sections it names, and nothing else whole:
  `remaining-plan.md`, `traceability.md`, the design files and earlier review folders are searched
  by slice ID, not read. A report holds findings only (severity, `file:line`, failure scenario,
  fix), at most 200 lines, and no restatement of the design.
- **No confirmation rounds.** A worker fixes the findings; the lead verifies them with tests. Only
  an ML BLOCKING or MAJOR finding goes back to a reviewer, and then only to the ML expert, who reads
  that finding and the fix diff. A non-ML BLOCKING finding is rechecked by the persona that raised
  it, on that finding alone. MINOR findings are fixed or recorded without review.
- **No separate response or findings documents.** The lead records each finding's disposition as a
  table row in the milestone's validation file.
- **Designs still in the plan** (I3-3D, I4-D2, V-D, I5-D, I6-D) get one round with at most two
  personas, ML being one where ML is in scope. Drafted designs awaiting review (T1-D, I4-D1) are
  reviewed in their milestone's design round.
- **`traceability.md` is updated once per milestone**, at acceptance, not per slice.

| Milestone | Slices                                | Design read first | Reviewers                   |
| --------- | ------------------------------------- | ----------------- | --------------------------- |
| M1        | I3-2c1, I3-2c2, T1                    | T1-D              | ML (named items), DIST, SEC |
| M2        | I3-3a to I3-3d                        | I3-3D             | SENS, SEC, ML               |
| M3        | I4-1a, I4-1b, I4-2, I5-1a, I4-3, I4-4 | I4-D1, I4-D2      | ML, NUM, EPI                |
| M4        | I4-5, I4-6, I4-7a, I4-7b, W1          | none              | VM, ARCH, ML                |
| M5        | I3-4a to I3-4c, I5-1b, W2             | V-D               | DIST, SEC, ML               |
| M6        | I4-8 to I4-11                         | none              | ML, NUM, SENS               |
| M7        | I5-2 to I5-5                          | I5-D              | ML, NUM, DIST               |
| M8        | I6-1, I6-2, T2                        | I6-D              | ML, DIST, SEC               |
| Final     | F1, F2                                | none              | ML (whole system), ARCH     |

M1's round also carries the owed I3-2 confirmations: the lead lists the open round-3 findings from
ML, epistemology, hardware and security, and the M1 reviewers check them against the code. Those
personas do not each run a separate round. This is about 30 reports for the remaining work, where
the section 5 table implied more than 200.

### 0.2 Records under the milestone rule

- 2026-11-04: T1-D round 1 (ARCH FAIL 3 MAJOR, DIST FAIL 4 MAJOR; [reports](t1-review/)). Revision 2
  resolves all seven; the lead verified the two code claims it rests on (`RemovedPredicate` absent
  from `upgradeChangeRequiresRefusal`, two ingress scopes in `observe`). T1-D revision 2 is
  accepted; T1 lands as T1a, T1b1, T1b2 and T1c (about 3,400 lines).

- 2026-11-04: I4-D1 round 1 (ML FAIL 2 MAJOR, NUM FAIL 3 MAJOR; [reports](i4-d1-review/)). Revision
  2 resolves all five. The ML expert confirmed ML-1 and ML-2 closed on re-run simulations and the
  vector checker, with no new MAJOR; the lead accepts the three NUM fixes (NUM-M3 in part: the host
  identity is printed by the checker, not stored). I4-D1 revision 2 is accepted. Open for the owner:
  the latch latency the slow fraction costs (about 2,515 events, 25 s at 100 Hz) against spec 5.1's
  five consecutive rejections (ML MINOR N-1, design Q1).

- 2026-11-05: I3-2a-r (I4-D1 section 12: parameterised fraction cells, `b` by series, burst tests)
  lands after T1 and before I3-3.

- 2026-11-06: the owner accepted the
  [latch-latency investigation](i4-d1-review/latch-latency-investigation.md) recommendation,
  answering I4-D1 Q1 and ML MINOR N-1. The slow-pair latch stays as designed (A12 ratified, knowing
  that a source rejected at a rate below about 0.7 no longer latches FAULTED and stays SUSPECT;
  latch latency 2,515 events from clean, 6,812 from a long clean run). A17 adds a derived,
  non-latching exclusion from the fast pair. Owed, without ratification: the user-guide line that
  gate FAULTED is an event-count latch and a sensor below 1 Hz is isolated by the health-timer or
  operator path, the 0.1 Hz figure, and a wall-clock floor as a follow-up. I4-8 implements A17.

- 2026-11-06: owner ruling on text and type. Type inference from canonical text on cell minting and
  value update stays; the cell's text is the user's and stays comparable whatever its value bits.
  The owner approved the core change of
  [the text-preservation study](m1-review/text-preservation-blast-radius.md): `promote` and
  `promoteOverlay` in `arena_manifold.cpp` keep a typed cell's own text (its span or scratch text)
  when inference from it gives the same kind and bits, and render canonically otherwise, as R6
  already requires. VQL `exportToStore` gets the same guard; the slice DTO is a follow-up.

## 1. I3-2 round-2 decisions

The items marked `DECISION` in [findings-r2.md](i3-2-review/findings-r2.md):

| Item | Decision                                                                                                                                                                                                                                                    |
| ---- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| F1   | (b) Relayed outcomes count in both `R` and `N`, once per (incarnation, event ID), first committed copy. Test at 0, 50 and 100% relay share.                                                                                                                 |
| F5   | (A) Classify at the check site. Non-finite or mistyped values and clock-epoch regressions are health refusals. `UncertainAcquisition` and mapping-version regressions join the host time class, which stays out of `f` until I3-3.                          |
| F6   | (a) One `health` count on the charge and its note, one ABI constant, a versioned list of counted codes. C++ is justified because the refusal is decided in `observe`, before the arena sees the event; the list is published and pinned by a test.          |
| F9   | (A) One settle turn per pending event. A failing event is discarded alone; the outcome carries settled and discarded counts and the bounded names of discarded predicates.                                                                                  |
| F11  | (a) Accepted residual: ingestion stays all-or-nothing with its own handlers (the I3-1 contract). An operator isolate or reset unwedges the source. Recorded as a known limit.                                                                               |
| F14  | Enforce the self-certification ban at `grantHealth`, at `bindSource` and revive, and at turn time, host-wide (any principal bound on any actor of the executor, tombstones included), on canonical principal tokens. Delegated device keys: recorded limit. |
| F17  | As recommended: `Reset` names the `reset_epoch` it clears, a mismatch refuses `StaleHealthState` and commits nothing; no-op actions return `NoChange` without a turn; tickets are (instance, counter); `healthOutcome` is pending, known or unknown.        |
| F20  | (A) A committed reset raises each of the principal's stream sequence floors to the highest sequence admitted before the reset, so a resend refuses `StaleSequence` on any host.                                                                             |
| F22  | (A) Delete the reflash runbook. A source needs a durable or fresh incarnation (persistent counter, trusted clock, host handshake); a device with none cannot be an arbitrated source.                                                                       |
| F23  | (A) No host latch mirror in I3-2. The arena refusal allocates nothing and the per-source queue share bounds a flood. Revisit only with a measured flood case.                                                                                               |
| F27  | (A) A fixed per-source health reserve inside the cell ceiling; `validate()` keeps the total under the checkpoint limit; exempt paths create at most `2 × max_sources` profiles.                                                                             |
| F28  | Evict audit entries by class (isolate and reset entries only by later isolate or reset entries), count repeats of (kind, code, issuer), keep `actions_evicted`, copy the removed fault's provenance into the reset entry, and add a host audit callback.    |
| F30  | (A) Keep today's whole-block fallback for invalid settings, with a WARN naming the field.                                                                                                                                                                   |
| F33  | Record every departure (section 3 below). On resume `R` and `N` are read under the settings in force; `N` is clamped to the new bound at the next update.                                                                                                   |

The merged owner questions in findings-r2.md section 4 take their recommended answers.

Revision 3's author asked for confirmation of four choices; the lead confirms them, subject to the
round-3 reviews: the F27 reserve is charged per existing profile, with `validate()` bounding the
worst case at `max_sources` profiles under the checkpoint limit; 2c1 ships a stub health turn that
answers `HealthUnavailable` and 2c2 follows it before any release; a failed settle event is reported
on the committed change (`SettleReport`), and the executor-side discard is justified only as the
recovery path when the settle turn itself cannot run; cycle prices are estimates that each slice
measures before acceptance.

### Round-3 rulings

From the five round-3 reports that finished (`i3-2-review/review-*-r3.md`); the ML and hardware
reports are still owed.

| Item  | Ruling                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| ----- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| R3-1  | The health reserve is charged to live profiles only. A tombstone is charged the cells it holds. Health actions on a tombstone, and isolate-created profiles past `max_sources`, draw on the exempt headroom (`cells / 16`) and refuse `EpistemicBudget` past it; the `2 × max_sources` cap is removed. Tombstones stay bounded only by the cell ceiling, as at HEAD, and the design gives the figure. (architect M1, codebase M3, epistemology M2, security M3)                                                                                                                                                                                                                     |
| R3-2  | `validate()` keeps per-field domains and relations that no legitimate small configuration violates. The capacity relations become a construction-time WARN plus the admission arithmetic. The design states the capacity at default settings plainly (live sources with full health bounds, and records that still fit); O7 revisits the defaults. (codebase M4, security M3)                                                                                                                                                                                                                                                                                                       |
| R3-3  | A new `firstCount` gates only the estimator's increments; `firstCharge` keeps its meaning for the lifetime counters and contact. (codebase M1)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| R3-4  | A closed charge moves into its note (`PostBox::Item` carries the `Charge`); the map holds only the open charge per (actor, principal). (codebase M2, distributed M3)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| R3-5  | Replay (revised after ML and hardware round 3): a host-side high-water mark per (actor, principal, incarnation, predicate), raised only by a commit or a pass credit, never by a refusal. A refusal moves the estimator only above the mark and only once, through a bounded ring of counted refusal sequences above the mark (its size a setting, default the per-source queue share). Below the mark nothing counts. A commit at or below a credited sequence does not count again. The mark is host memory, not checkpointed; after an executor restart one replay pass can count again, which the design states as the bound. (distributed M4, security M1, hardware M1, ML r3) |
| R3-6  | The reset floor comes from readings in flight: a bounded per-(principal, predicate) multiset of queued sequences whose maximum the reset call snapshots. `wouldChange(Reset)` is true while any reading is in flight. (distributed M1)                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| R3-7  | Strict control priority. The control cap yields only to other actors, counts every turn of a change (settle turns included) before the change starts, and applies to source changes too. The interleave claim and its test are deleted. (distributed M2)                                                                                                                                                                                                                                                                                                                                                                                                                            |
| R3-8  | A4 is reworded (section 3) and 2b adds the wedge test. F11 (a) stands; settle after commit for ingestion is an open owner question. (epistemology M1)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| R3-9  | A shrunken `H` rescales `R` and `N` together, preserving `f`, before the update is applied, so a batch equals the sequential updates. (epistemology M3, ML r3 M2)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| R3-10 | Audit sink contract: called with no executor lock held; may call the read-only host API and `revokeHealthGrant`; must not block (queue handoff); installation serialized with delivery, a null sink allowed with a WARN; at most once, in commit order per actor; call and throw counts in `stat()` and `SourceHealth`; no secret in `HealthAudit`. The sink runs after commit; "no durable record, no isolate" is an open owner question. (security M2)                                                                                                                                                                                                                            |
| R3-11 | No shape-version or state-version cell in I3-2. The shape version moves to T1, with one guard function `hasTranslatableEpistemicState` (plan review architect M3, items 1-4); the epistemic-state version moves to I4 with its first consumer. I3-2 records that every health change, supersession and policy change must increment it once it exists. A "health change" there means a change of the derived state class, the latch or the reasons, not every `R`/`N` update (ML r3).                                                                                                                                                                                               |
| R3-12 | A note applies its refusal and credit counts as an evenly interleaved mixture (`R <- b^n R + (k/n)(1-b^n)/(1-b)`, `N <- b^n N + (1-b^n)/(1-b)` for `k` refusals in `n` events), not credits first. Notes still close on a queued accept. (ML r3 M1)                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| R3-13 | F23 revisited, its condition met by a measured flood (hardware r3 M2: eight isolated sources at 100 Hz leave healthy sources 29% acceptance). A host negative cache of the arena latch, never an authority: set by an arena `SourceFaulted` refusal or a committed isolate; cleared by a committed reset, actor stop, upgrade or resume; while set, `observe` refuses `SourceFaulted` without queueing or charging. One test per transition.                                                                                                                                                                                                                                        |

## 2. Plan decisions

For [remaining-plan.md](remaining-plan.md) section 7 and the plan reviews in
[plan-review/](plan-review/):

| Item | Decision                                                                                                                                                                                                                                                                                                                                                                                  |
| ---- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| O1   | (b) Translate P19 state for supported agents and refuse an active intention with a typed, tested error. This claims P19 upgrade support before the full P17 pipeline, so it needs proposed amendment A5 (plan review architect M2).                                                                                                                                                       |
| O2   | Follow the spec's P19.2/P19.3 file list (graph construction in `vortex_stdlib`), since every actor's worker context already bootstraps `std:math` (architect plan-review M7). I4-D settles the mechanism and departs only with evidence, as a proposed amendment.                                                                                                                         |
| O3   | I4-D settles. Leaning: ranked continuations live in the activation's scope; verification query tokens are committed at the commit point under the agent's `d.epistemic` with a retention bound. A proposed amendment to 2.1 ("an intention heads query tokens") until intentions exist.                                                                                                   |
| O4   | I3-3D settles the scope with the security and distributed reviewers. Leaning: an in-tree reference adapter that verifies an origin signature over canonical observation bytes, with audience binding and domain separation, so authentication holds outside tests.                                                                                                                        |
| O5   | Amendments accumulate in section 3; F1 writes spec v1.6 with them marked pending ratification. E0 shrinks to this record.                                                                                                                                                                                                                                                                 |
| O6   | I4-D settles with the ML, numerics and epistemology reviewers.                                                                                                                                                                                                                                                                                                                            |
| O7   | I4-D prices every structure and sets the label and ledger caps (ML plan-review M3); the ceiling is re-measured at each slice.                                                                                                                                                                                                                                                             |
| O8   | Per F28.                                                                                                                                                                                                                                                                                                                                                                                  |
| O9   | (New, ML plan-review M7: `lcb` at an uninformative prior makes a fresh source vote against its own claim.) I4-D settles with the ML reviewer; a change to the spec's formula is a proposed amendment.                                                                                                                                                                                     |
| O10  | (New, ML plan-review M4: five consecutive gate rejections latch FAULTED on noise.) I4-D settles with the ML and hardware reviewers; a change to the spec default is a proposed amendment.                                                                                                                                                                                                 |
| E1   | Minimal: a private header for the code generator's declaration so new epistemic code lives in new files; no bulk move of existing bodies. The plan revision states the boundary.                                                                                                                                                                                                          |
| T0   | Share the upgrade guards across both entry points now. Collapsing the two state copiers is the translator's design (T1), not T0.                                                                                                                                                                                                                                                          |
| D1   | (Plan draft 2, 7.2 item 1.) Replay is bit-equal on one host; across hosts results agree within a stated tolerance, and decisions near a threshold carry a declared margin.                                                                                                                                                                                                                |
| D2   | (Plan draft 2, 7.2 item 2, revised after plan review distributed M2.) I3-4c refuses a second live resume of one checkpoint in one executor with a typed error. Cross-host fork prevention is an operator precondition, stated in the user guide and the final limits, not claimed as implemented. An issuer restored from a checkpoint takes a fresh incarnation (A3 applied to issuers). |
| D3   | (T0 codebase review.) Source bindings keep the policy they were bound under after an upgrade. T1-D decides whether a policy change rebinds, refuses or migrates them.                                                                                                                                                                                                                     |
| P2-1 | Typed refusals for resume and upgrade: T1-D designs one channel (a checked pre-flight that returns the typed reason and the offending numbers before `runtime_.resume`), and T1 implements it, fixing today's resume of a changed policy that looks successful and then dies (X-9). Later slices cite it. (plan review architect M1)                                                      |
| P2-2 | Verification replay floor: an issuer-declared low-watermark; a bounded window with a retryable `VerificationWindowFull`; a monotone issuer incarnation; floor state created only when intake commits; the signed header common to every message kind fixed by I3-3D. (plan review distributed M1)                                                                                         |
| P2-3 | V-D defines two fault models (in-turn abort by fault injection; restart from the last checkpoint) and the expected state per crash point and per D2 option; upgrade rows are refused-not-quiescent or state-carried. (plan review distributed M3)                                                                                                                                         |
| P2-4 | Reset and the label ledger: I4-D2 states the rule. A reset opens a new calibration epoch in O(1); the recompute reads only current-epoch entries; older entries leave the cap; a pending label for the old epoch gets `ExpiredLabel`. I5-1a tests it, and row 5.1-13 closes there. (plan review ML M1)                                                                                    |
| P2-5 | The ledger cap has one owner, I4-D2: the cap per (source, domain, epoch), its default, the cell price of one entry computed from its fields and confirmed by a throwaway fixture, the minimum labels per source, and the checkpoint interaction. (plan review ML M2)                                                                                                                      |
| P2-6 | Plan draft 3, 7.3 items 1-3, confirmed: I3-2c1 is the host side with a stub turn and I3-2c2 the health turn, as in the I3-2 design; the epistemic-state version is born in I4-1b with its first writer; D2's rollback case is option (c), a stated limit (rollback past an acknowledged verification loses it), with the operator procedure in the guide.                                 |

## 3. Proposed amendments (pending owner ratification)

| #   | Spec section | Amendment                                                                                                                                                                                                                                                                                                                                        | Source                             |
| --- | ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ---------------------------------- |
| A1  | 5.1          | `clear` removes operator-placed degradation reasons; it never unlatches FAULTED and never touches calibration.                                                                                                                                                                                                                                   | owner, I3-2 rev 1                  |
| A2  | 5.1          | Health thresholds are actor-wide settings, not policy fields; SUSPECT from content refusals is an event-indexed decayed fraction (owner Q2); gate rejections are I4-D's (O10).                                                                                                                                                                   | owner Q2, F33                      |
| A3  | 2.1, 5.1     | A source needs a durable or fresh incarnation; one without cannot be an arbitrated source.                                                                                                                                                                                                                                                       | F22                                |
| A4  | 5.1          | A program whose own handler for an arbitrated predicate always fails cannot update that predicate's keys until the handler is fixed by an upgrade. An operator isolate or reset admits at most one non-replacing reading per key and is not a remedy.                                                                                            | F11, epistemology r3 M1            |
| A5  | 8.2          | Gate 0: P19 upgrade support is claimed for agents without an active intention, by quiescent upgrade only; full vagent upgrade support remains owed to P17.                                                                                                                                                                                       | O1, plan review architect M2       |
| A6  | 5.1          | reasons carry an `origin`; `Clear {}` removes operator-origin reasons only and `Clear {code}` refuses another origin; one reasons slot is reserved for evidence-driven reasons (refines A1; F33 item 4)                                                                                                                                          | I3-2 rev 4, section 11             |
| A7  | 2.1, 5.1     | a committed reset names and bumps a `reset_epoch` (a retry of a stale epoch refuses), stamps it into new records' `calibration_epoch`, and raises each stream's sequence floor to the highest sequence **in flight** at the call, capped at the stream's last sequence plus a setting                                                            | I3-2 rev 4, section 11             |
| A8  | 5.1          | isolate withdraws the source's supports at once and refuses its later readings, instead of only excluding it from I4's eligibility                                                                                                                                                                                                               | I3-2 rev 4, section 11             |
| A9  | 5.1          | relayed outcomes count in `R` and `N` once per stream sequence, by a high-water mark that only commits and pass credits raise; a reading that passed every check but was dropped under load counts as accepted (a reset clock epoch dropped under load is credited, not charged); a note applies its counts as an evenly interleaved mixture     | I3-2 rev 4, section 11             |
| A10 | 5.1          | belief events raised by a source change or health action run in their own turns after the change commits, one top-level event per turn; a failing event is discarded alone (for every handler of that event) and reported; nothing re-drives it                                                                                                  | I3-2 rev 4, section 11             |
| A11 | 5.1          | I4 may score with a smooth penalty computed from `(R, N)` as one of P19's "configurable heuristics", combined with the discrete inflation by maximum                                                                                                                                                                                             | I3-2 rev 4, section 11             |
| A12 | 5.1          | Gate rejections drive event-indexed fractions in place of the streaks of three and five: SUSPECT at a fast (64) fraction of 0.30; FAULTED only when a fast (64, 0.70) and a slow (4,096, 0.70) fraction both hold; the gate tests the nominal variance. Ratified 2026-11-06 with its stated price: rejection rates below about 0.7 do not latch. | I4-D1 section 7, owner             |
| A17 | 5.1          | While the fast gate fraction (64, 0.70, at least 48 events) holds, the source's accepted readings are excluded from fusion, non-latching, with reason `gate_excluded` and displayed SUSPECT; the source returns by itself when the fraction falls. Spec 5.1 excluded only an active fault status. Ratified 2026-11-06.                           | latch-latency investigation, owner |

Later decisions append rows here.

# P19 completion: lead decisions and proposed spec amendments

Authority: the owner's instruction of 2026-11-02 to complete P19 in iterations with worker agents,
gated by expert reviewers. A lead decision chooses between options within approved P19 v1.5 and the
owner's earlier decisions. A decision that changes what the spec says is also listed under "Proposed
amendments". Those are folded into spec v1.6 at F1, marked pending, and take effect as spec text
only when the owner ratifies them. Reviewers may contest any decision with evidence.

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

## 2. Plan decisions

For [remaining-plan.md](remaining-plan.md) section 7 and the plan reviews in
[plan-review/](plan-review/):

| Item | Decision                                                                                                                                                                                                                                                                                |
| ---- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| O1   | (b) Translate P19 state for supported agents and refuse an active intention with a typed, tested error. The final record does not claim full vagent upgrade support, which spec 8.2 reserves for after the P17 prerequisite; no amendment is needed.                                    |
| O2   | Follow the spec's P19.2/P19.3 file list (graph construction in `vortex_stdlib`), since every actor's worker context already bootstraps `std:math` (architect plan-review M7). I4-D settles the mechanism and departs only with evidence, as a proposed amendment.                       |
| O3   | I4-D settles. Leaning: ranked continuations live in the activation's scope; verification query tokens are committed at the commit point under the agent's `d.epistemic` with a retention bound. A proposed amendment to 2.1 ("an intention heads query tokens") until intentions exist. |
| O4   | I3-3D settles the scope with the security and distributed reviewers. Leaning: an in-tree reference adapter that verifies an origin signature over canonical observation bytes, with audience binding and domain separation, so authentication holds outside tests.                      |
| O5   | Amendments accumulate in section 3; F1 writes spec v1.6 with them marked pending ratification. E0 shrinks to this record.                                                                                                                                                               |
| O6   | I4-D settles with the ML, numerics and epistemology reviewers.                                                                                                                                                                                                                          |
| O7   | I4-D prices every structure and sets the label and ledger caps (ML plan-review M3); the ceiling is re-measured at each slice.                                                                                                                                                           |
| O8   | Per F28.                                                                                                                                                                                                                                                                                |
| O9   | (New, ML plan-review M7: `lcb` at an uninformative prior makes a fresh source vote against its own claim.) I4-D settles with the ML reviewer; a change to the spec's formula is a proposed amendment.                                                                                   |
| O10  | (New, ML plan-review M4: five consecutive gate rejections latch FAULTED on noise.) I4-D settles with the ML and hardware reviewers; a change to the spec default is a proposed amendment.                                                                                               |
| E1   | Minimal: a private header for the code generator's declaration so new epistemic code lives in new files; no bulk move of existing bodies. The plan revision states the boundary.                                                                                                        |
| T0   | Share the upgrade guards across both entry points now. Collapsing the two state copiers is the translator's design (T1), not T0.                                                                                                                                                        |

## 3. Proposed amendments (pending owner ratification)

| #   | Spec section | Amendment                                                                                                                                                                                  | Source            |
| --- | ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ----------------- |
| A1  | 5.1          | `clear` removes operator-placed degradation reasons; it never unlatches FAULTED and never touches calibration.                                                                             | owner, I3-2 rev 1 |
| A2  | 5.1          | Health thresholds are actor-wide settings, not policy fields; SUSPECT from content refusals is an event-indexed decayed fraction (owner Q2); consecutive streaks stay for gate rejections. | owner Q2, F33     |
| A3  | 2.1, 5.1     | A source needs a durable or fresh incarnation; one without cannot be an arbitrated source.                                                                                                 | F22               |
| A4  | 5.1          | A program whose own handler for an arbitrated predicate always fails cannot ingest that predicate; an operator isolate or reset unwedges it.                                               | F11               |

Later decisions append rows here.

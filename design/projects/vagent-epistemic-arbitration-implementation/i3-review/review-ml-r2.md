# ML gate review, round 2: I3-1 design revision 2

Read in full: i3-design.md (rev 2, 288 lines) against ml-gate-plan I3-I6, spec 2.1/2.2/5.1/8.1, and
the round-1 report. Code facts from round 1 stand (rtBSet's replace path calls rtBDrop, which
unlinks the source's support; rev 2 now adds a separate rtObserveSet that splices history before the
drop). No files edited in the repository.

## Round-1 findings: status

- ML B1 (changed value destroys history): RESOLVED. Section 5.4 splices the old support's
  `d.epistemic` history onto the new support before the old support is dropped, so one owner holds
  it throughout and there is exactly one support, hence one current record, per (identity, key).
  Test: "changed value transfers history and keeps one current record". Satisfies spec 2.1
  (own-source case).
- M2 (current per identity/key): RESOLVED by the same construction plus identity-keyed supports.
- M3 (calibration key): RESOLVED. Predicate signature, domain/schema ID, model/model epoch,
  calibration epoch, feature schema, observation version, health at ingest are all on the record.
- M4 (feature snapshot): RESOLVED. Feature schema 0 with an empty owned list; I5 can distinguish
  "pre-feature" records from real snapshots.
- M5 (verdict): RESOLVED in substance: immutable verdict, verdict epoch 0, "unscored, not endorsed",
  I4 must treat no-calibration-record as ineligible or refuse. See finding 1 for the one loose end.
- M6 (idempotency): RESOLVED. Per-profile (event ID, digest) window, `Duplicate`,
  `ConflictingEvent`, replay horizon = window, then the per-stream sequence rule. See finding 4 for
  digest provenance.
- M7 (priors): RESOLVED. `prior_alpha`, `prior_beta` required and finite >0; no implicit Beta(1,1).
- Minors 8-12: RESOLVED (uint64 range check, host-stamped receipt, discrete variance refused, pinned
  flag, incarnation start at sequence 1, queued events drain before checkpoint). Refusal of
  retract/revise is now a static `EpistemicIngressOnly`, acceptable and consistent with spec (user
  revision deferred to I3-2).

## New findings (none blocking)

1. MINOR - Where does a later verdict revision live? Section 5.5 says a gate "appends a verdict
   revision with a higher verdict epoch", but the record has no revision list head. Fix: add an
   empty owned `verdict_revisions` list head to the record now (consistent with the empty feature
   list), and state that readers take the highest verdict epoch; old records without it read as
   epoch 0. This is what lets a later gate re-verdict without rewriting history.

1. MINOR - Cell arithmetic in section 8 undercounts. Supports are per (identity, key), not per
   principal: cost is principals x keys x history x ~60 cells, not principals x history x 60. Dedup
   windows (64 pairs per profile), stream lists, profiles and clause cells are also not counted, so
   the stated ~61,000 is a floor and the 65,536 checkpoint budget is reached with few keys. The
   admission guard keeps this safe (explicit `EpistemicBudget`), but state the formula, count the
   dedup/profile cells in the guard, and reconsider the dedup default (64) and whether a new source
   should be refused while retired records hold budget (excise retired history before refusing
   admission).

1. MINOR - Protocol refusals versus gate rejections. Section 5's failure bookkeeping increments the
   profile "reject count". Spec 5.1/I3 gate requires health warning versus competence separation,
   and I3-3 turns reject streaks into SUSPECT/FAULT. Fix: use a distinct
   `refused_count`/`last_refusal_code` for protocol/ingress refusals and leave the gate
   `reject_count` and rejection streak untouched until gates exist. Also state the bookkeeping write
   is best-effort: if the failure was the cell budget, it may itself be refused without corrupting
   state.

1. MINOR - `payloadDigest` is host-supplied. `ConflictingEvent` depends on it matching the stored
   fields. Define it as SHA-256 over a canonical encoding covering principal, incarnation, event ID,
   sequence, predicate, tuple, times, uncertainty, frame, units, variance (not `relayedBy`), and
   either have the executor recompute it from the checked fields or document that the
   origin-signature verification binds exactly those fields. Test: tuple altered with the same
   digest is refused.

1. MINOR - New-incarnation retirement scope: "the predecessor's current records are retired by that
   ingestion" must be bounded (all keys? only this key?) and cost-counted in the turn's fuel; if
   only the reported key, other predecessor readings stay current for a reset device, which
   conflicts with "reset invalidates old observations" (spec 5.1). Pick all keys and test it, or
   defer to I3-2 explicitly.

1. MINOR - Define `domain/schema ID` (e.g. predicate signature plus policy hash) so I5's exact-match
   derivation is mechanical, and note that adding policy keys in I4 changes the policy hash (typed
   upgrade change, acceptable with the typed restore mismatch already designed).

## Assessment

Numeric types match the contract (Double times/variance, Int64 sequences/epochs, normal-positive
variance, declared priors). Epoch fields are complete for I4/I5 keying without positional
reinterpretation. A later gate can re-verdict without corrupting history if finding 1 is applied;
the UNTESTED deferral is otherwise sound. I3-1a, 1b and 1c may proceed to implementation; findings
1-6 should be folded into the design/implementation (1, 3, 4 before 1b lands; 2 and 5 before 1c).

Tally: BLOCKING 0, MAJOR 0, MINOR 6.

VERDICT: PASS

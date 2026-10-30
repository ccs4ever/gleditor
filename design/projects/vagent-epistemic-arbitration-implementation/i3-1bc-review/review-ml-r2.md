# ML gate review, round 2: P19 I3-1b and I3-1c

Scope: head `d48e85f8` (fix `e91e8fe8` plus `0124b0d2`, which adds the `max_uncertainty` key and
I3-1d epistemic resume, and `d48e85f8`, which removes stray corpus files), judged as the data
pipeline for I4/I5 scoring, calibration and fusion. Compared against my round-1 report
(`review-ml.md`) and design section 14. No repository file was changed; probes and mutants are in
`/tmp/i3-1bc-ml/r2/`.

## Probe results

Existing suites at this head (headless): `VagentEpistemic*` 92 tests pass (ingress, noise, policy,
resume). Probes (`/tmp/i3-1bc-ml/r2/probe.cpp`, linked to the current objects):

| round-1 finding | probe | result now |
| --- | --- | --- |
| M1 keyless freezes source | keyless `pos(x: Float)`, 8 readings | all 8 commit; each supersedes the last; one `current`, earlier ones `retired`, history trimmed to 4 |
| M2 retry after `max_age` | exact resend of committed event after 1.5 s with `max_age: 1s` | outcome stays `Committed`; `content_refusals` 0, `refused_count` 0 |
| M2 relayed/direct conflicting copy | same event ID, other tuple, relayed then direct | original stays `Committed`; counters 0 |
| M2 cross-source eviction | 300 refusals from P1, P2's committed outcome | intact (`Committed`) |
| M3 record identity | dump of committed and relayed records | `observation_version=1`, `domain=at/2@<policy hash>`, `model`, `model_epoch=0`, `calibration_epoch=0`, `feature_schema=0`, `clock_instance=<16 hex>` (differs per executor), `health=NOMINAL`, `relayed_by` present on relayed copy |
| M4 epoch/mapping | epoch 5, then epoch 4 (older acquisition), then epoch 5 with lower mapping, then epoch 6 (older acquisition), then epoch 5 again | epoch 4: `ObservationTimeMismatch`; lower mapping: `ObservationTimeMismatch`; epoch 6 commits and retires the epoch-5 record; epoch 5 after 6: `ObservationTimeMismatch`. The older reading no longer retires the newer one |
| hardware M3 `max_uncertainty` (now judged) | uncertainty 0.5, 9.9, 10, 11, 3600 s at `max_age: 10s` | 0.5, 9.9, 10 commit; 11 and 3600 refuse `UncertainAcquisition`; `content_refusals` 2 |
| early reading refused, resent | acquired 7 s before the current record, sent twice | refused `ObservationTimeMismatch` both times, no record written; `content_refusals` 2 |

Mutation check against the ingress and noise tests (the tests carry the evidence; the fuzz column is
not reported because my stand-in harness object predates the new fuzzer):

| mutant | killed |
| --- | --- |
| outcome log overwrites `Committed` | yes (2 tests) |
| epoch regression check off; mapping check inverted | yes (`ClockEpochsAndMappingsNeverRegress`) |
| `max_uncertainty` check off | yes |
| keyless predicate stops replacing | yes (2 tests) |
| `clock_instance` constant; `domain` without policy hash | yes (`ARecordCarriesWhatProducedIt`) |
| **FAULTED health never stamped on the record** | **survives** |
| retry charge in observe-time refusal | not evaluated (my mutant did not compile: unused variable) |

## BLOCKING

None.

## MAJOR

None. Each of M1-M4 is fixed and a regression test fails the matching mutant. Hardware M3 is fixed
by `max_uncertainty` (`agent_executor.cpp` observe, before the age and skew windows); nothing in
the data pipeline depends on anything else being decided about it.

## MINOR (new or remaining)

1. **Health at ingest is only the FAULTED latch.** `compiler.cpp` writes `FAULTED` or `NOMINAL`
   from `profile.faulted`; no test sets the latch and ingests (mutant `healthNever` survives). I4
   will read this field for calibration cohorts. Add a test that ingests after a latch and expects
   `FAULTED`, and say in the design that finer states arrive with I3-2.
1. **A source's own flood evicts its committed outcome.** Probe: one committed event, then 200
   refusals from the same principal; `ingestOutcome` returns `nullopt`. The ring is per source
   (64) and evicts oldest first regardless of kind (`Ingress::log`, `agent_executor.cpp` ~:658). The
   sticky `Committed` guard is then gone, so a late resend of that event is charged as a fresh
   content refusal. Evict refused entries before committed ones, or keep committed entries in a
   separate sub-ring.
1. **Identical resends of a refused event are each charged.** Probe: one bad-units event sent five
   times after a good one gives `content_refusals` 5; the early-reading probe gives 2 for one event.
   The refusal class is Terminal ("never resend"), but a store-and-forward transport does. In
   `refuse()` and `finishIngest`, treat a key already logged `Refused` with the same code as a
   retry and do not charge it again. The fix for committed events (M2) shows the shape.
1. **A refused retry of a pending event rewrites its logged outcome.** Probe: event queued, a
   same-ID bad copy arrives before the pump; the log shows `ObservationFrameMismatch` until the pump
   then flips to `Committed`, and `content_refusals` is 1. `log()` guards only `Committed` and
   `Duplicate`. Guard `Pending` too, and do not charge a copy whose key is pending.
1. **Resume loses the outcome log and host floors, so post-resume retries are charged.** After a
   resume the executor log is empty. A resend of an already committed event inside the dedup window
   is `Duplicate` (uncharged); one outside it meets `StaleSequence` and lands in `refused_count`.
   Diagnostic only, but state it in section 14 so I3-2 does not read `refused_count` across a
   resume. The resume test file covers dedup carry-over, not this.
1. **Counters remain lifetime-cumulative across incarnations, revivals and now resumes.** Open in
   section 14 for I3-2; no change from round 1. First-contact refusals still leave no counters
   (recorded open).
1. **Noise test and simulator** (round-1 minors 8 and 9, untouched in this fix): wall-clock
   dependence (`wallSeconds() - 5.0`, `max_age: 10s`), no assertion of counters against tallied
   outcomes, no ground-truth label on reports, single key, relayed copies emitted immediately so
   no pre-reboot replay arrives after a rebind. Cheapest strong addition remains asserting
   per-principal `content_refusals` and `refused_count` against what the test saw.
1. **Fuzzer**: still no record-content or identity invariant (the epistemic invariants file only
   gained keyless handling), so M3 and M4 are guarded by unit tests alone; fresh actor per input
   remains. Section 14 says the corpus now reaches 17 outcome codes; I did not re-count it.
1. Round-1 minors not repeated here: host-caused refusals reaching `refused_count` is fixed
   (`chargeable` now excludes `UnboundIncarnation`, `EpistemicBudget`, `Undelivered`,
   `TurnExhausted`, `ArenaExhausted`); `last_contact` monotonicity is fixed by taking the max when
   charging; the relay-first-then-direct contact gap is still by design for I3-3.

## Resume (I3-1d) from the data side

Profiles, streams, dedup windows, tombstones and every support history are restored with one copy
map so clone occurrences are shared (test `ProfilesHistoriesAndCloneRanksSurvive`); steady liveness
stamps on profiles are dropped, records keep theirs together with the old `clock_instance`, which
differs from the new executor's, so a scorer can tell whose steady clock a stamp belongs to. A
changed policy hash refuses the resume, so history is never scored under rules it was not checked
against. The arena-floor refusal after resume is tested for a live profile and a tombstone. I found
no leak of replays into the data; the only gap is minor 4.

Tally: BLOCKING 0, MAJOR 0, MINOR 7.

Verdict: PASS

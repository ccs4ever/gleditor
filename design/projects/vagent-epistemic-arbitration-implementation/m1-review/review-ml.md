# M1 review: ML (named items and owed confirmations)

Verdict: **PASS** (no ML BLOCKING or MAJOR finding; three MINOR).

Range read: `git diff 9bd27a03..61e57eae -- apps tests tools Makefile docs`, plus the I3-2a
estimator IR (`epistemic_emit_health.cpp` `emitHealthRescaled`, `emitRtHealthUpdate`) where the
confirmations needed it. Ran `vagent_actor_test` with the filter
`VagentEpistemicHealth*:Translate.*:VagentEpistemicResume*:VagentEpistemicSettle*`, headless and
with private XDG dirs: 112 passed.

## Findings

### ML-1 MINOR: the R3-12 note mixture is tested only to `n` = 64 in the VM, not to 1,000

- Where: `tests/xuzz/vagent_epistemic_health_test.cpp:390` (VM, one note of 8 queued and 56 offered,
  so `n` is at most 64), `:852` (model only, `k, c <= 40`). The design's test row
  (`i3-2-design.md:553`) and the resolution of ml M1 (`:1612`) say `n` = 64, 128, 256, 512 and 1,000
  at 5% bad, with `f` kept under `suspect_fraction`.
- Failure scenario: none in the shipped code. The update at `epistemic_emit_health.cpp:700-716` is
  the closed form, so it does not depend on `n`, and `b^n` going to 0 at large `n` is benign. What
  is missing is the evidence the ruling cited. A later change to the note path (I4-1b's version
  bump, I3-3 reasons) could break the long-note case without a red test.
- Fix: parameterise `ANoteIsAnEvenlyInterleavedMixture` over the five `n` with a per-source queue
  big enough to coalesce them (or drive `rtHealthUpdate` directly). Assert `f < suspect_fraction`
  and agreement with `Model::note` to 1e-9.

### ML-2 MINOR: the fuzz invariant that R3-9 relies on is not in the fuzzer

- Where: `tests/fuzz/fuzz_epistemic_ingress.cpp` and `tests/xuzz/epistemic_invariants.hpp` (no `R`,
  `N`, cap or rescale check anywhere; the fuzzer never changes the half-life). Design 6.8
  (`i3-2-design.md:1396-1399`) promises `0 <= R <= N <= 1/(1-b)` under the largest bound in force,
  and that a rescale leaves `f` unchanged. The resolution row for epi M3 (`:1609`) says the fuzz
  invariant carries R3-9.
- Failure scenario: none today. The R3-9 rescale is correct (`epistemic_emit_health.cpp:647-656`,
  before the update) and the unit tests at `vagent_epistemic_health_test.cpp:310`, `:335` and `:840`
  cover it. The fuzzer would catch a future clamp-after-update regression on paths the unit tests do
  not reach (health actions interleaved with notes); it does not.
- Fix: extend `ProfileHealth` in `epistemic_invariants.hpp` with `R` and `N` as the design says, and
  check `0 <= R <= N <= cap(H_max)` after every step. Add a fuzz step that restarts the executor
  under a smaller `health_half_life` (or sets the setting live), and check `|f_after - f_before|` at
  most 1e-12 across the first update.

### ML-3 MINOR: the checkpoint road re-types numeric text, so dedup misses after a resume

It does not move the estimator; it is for DIST.

- Where: `apps/common/xanadu/store.cpp:767` (`Store::makeCell(text)` infers a scalar kind), reached
  from `zigzag/arena_manifold.cpp:1991` when the checkpoint promotes scratch text. The kind then
  comes back on resume (`vortex/mesh_value.cpp:480-490`) and the translator moves it unchanged
  (`epistemic_translate.cpp:483-491`). The dedup check compares typed values (`compiler.cpp:4291`,
  `vortex_vm.cpp:569`, `vortex_core.cpp:296`). `t1-validation.md:267` records the re-typing, but
  only for field names and `-0.0`.
- Failure scenario: a device that uses numeric event IDs (`"6"`) commits event 6, and the agent
  checkpoints and resumes. The exact retry of event 6 now compares Int64 6 against text `"6"`,
  misses the dedup window and answers `StaleSequence` instead of `Duplicate`. A reused ID with other
  bytes answers `StaleSequence` instead of `ConflictingEvent`. The lifetime `refused_count` grows.
  The host-state row "a resend meets the arena's dedup window and answers Duplicate"
  (`epistemic_translate_registry.cpp:419`) is therefore false for such IDs. `R`/`N` do not move:
  neither code is in `kHealthRefusalCodes` (`epistemic_health.hpp:85`). That is why this is not
  ML-gating. The same mechanism re-types clause tuple text keys (`"17"`) for every agent, which
  predates the range.
- Fix: compare the dedup `event_id` and `digest` by text, not by typed value. Alternatively, on
  `Disposition::Resume`, have the translator reset text-only registry fields (event ID, digest,
  status, `retired_by`, tokens) to `ValueKind::None`. Add a resume test with a numeric event ID to
  `DedupWindowsAndStreamsCarryOver` (`vagent_epistemic_resume_test.cpp:222`). The general fix (a
  promote that preserves `ValueKind::None`) is a core change and needs its own approval.

### Checked and clean (no finding)

- **Reset epoch (P2-4).** A committed reset increments `reset_epoch` in O(1)
  (`epistemic_emit_health.cpp:393-394`). It zeroes `R`, `N` and the streak, and stamps the withdrawn
  readings `retired_by = reset`. Every new record takes the profile's epoch as `calibration_epoch`
  (`compiler.cpp:4483-4494`). A future label keyed by that field can be expired by one integer
  compare against the profile, which is all P2-4 needs.
- **Retirement keeps the epoch.** Retirement clears only `dedup` and `streams`
  (`compiler.cpp:4074-4076`), so a re-bound principal cannot reuse an old epoch (no ABA). Tombstones
  are never evicted (R3-1).
- **Pre-reset counts stay out of the new epoch.** A note whose charge predates a committed reset has
  its `health` and `passed` zeroed (`agent_executor.cpp:1479-1495`). A reading queued before the
  commit answers `SupersededByReset` without a turn (`:1460-1475`). Replay marks are not cleared by
  a reset (`agent_executor_internal.hpp:546-570`, only `forget`), so old signed events stay
  uncountable afterwards.
- **The translator carries the estimator state bit for bit on both roads.** `reset_epoch`,
  `action_seq`, `fault_seq`, `R`, `N`, `calibration_epoch` and `retired_by` are all `Carried`
  (`epistemic_translate_registry.cpp:213-262`, `:370-388`). `copyCell` moves the raw `valueBits`.
  The checkpoint road loses only `-0.0` and NaN payloads (`scalar.cpp:87-98`), and
  `to_chars`/`from_chars` round-trip every finite double exactly. `R` and `N` can be neither:
  non-finite values are refused at ingress, and every update is a sum of non-negative terms starting
  from +0.0.
- **Host reset generations restart at 0 on resume.** This is safe: nothing is queued at quiescence,
  and the arena epoch is authoritative.
- **`IngestionFault` (the wedge) is not chargeable** (`agent_executor_internal.hpp:617-619`). A
  program's own failing handler therefore never moves its sources' `R`/`N`.

## Confirmations

- ml M1 (R3-12, interleaved mixture): CONFIRMED in code (`epistemic_emit_health.cpp:686-716`, closed
  form, applied after the rescale). The "tested to `n` = 1,000" part is not met; see ML-1.
- ml M2 (R3-9, `R` and `N` rescale together, before the update): CONFIRMED
  (`epistemic_emit_health.cpp:647-656`; tests `vagent_epistemic_health_test.cpp:310`, `:335`,
  `:840`).
- epi M3 (R3-9; the fuzz invariant carries it): the code is CONFIRMED as for ml M2; the fuzz
  invariant is absent, see ML-2.
- epi M1 (R3-8, A4 reworded; wedge test): CONFIRMED. A4 is reworded at `i3-2-design.md:1568`. The
  wedge tests are at `vagent_epistemic_settle_test.cpp:484` and `:503`, the second half included
  (after a reset, one non-replacing reading per key commits and the next replacing one refuses).
- I3-2c2 named items (reset epoch and `retired_by`): CONFIRMED against P2-4 and carried by T1c on
  resume and upgrade (see "Checked and clean").

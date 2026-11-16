# M1 release PGO pipeline review — iteration 01

## Verdict: needs fixes

The generator/trainer/merger/release path is connected, and the focused unit suite passes. I found
one provenance hole in the release gate and a lifecycle-test gap that leaves the actual stage
orchestration unprotected.

## Findings

### P2 — Release accepts a profile after a declared runtime input changes

[`source_identity`](../../../tools/release_profile.py#L25) explicitly skips every path below
`tests/samples/`. The training report does record hashes for manifest `inputs` and `dependencies` in
[`performance-report.py`](../../../tools/performance-report.py#L530), and merge hashes the report
file. But merge only checks that the report still matches its previously recorded hash; it never
compares the report's input digests to the current files. Release does not reopen the report at all:
it validates the merged profile digest and source/compiler identity in
[`validate_profile_manifest`](../../../tools/release_profile.py#L180), then builds and validates
using the unchanged source identity.

Concrete counterexample: add `tests/samples/foo` as a manifest input, train and merge, then change
`tests/samples/foo` without changing the manifest. The current source identity remains unchanged
because the sample tree is excluded, and `release` proceeds using counters collected from the old
fixture. This violates the plan's requirement that profile provenance bind the training recipe/input
digest through profile use. At merge and release, reopen the recorded training report; check its
digest and manifest digest, then recompute and compare every declared input/dependency digest (and
the relevant workload command/weight/repetition recipe). Add a regression that mutates a declared
fixture between training and merge/release and proves refusal.

### P2 — Tests do not exercise a stage transition or final release gate

[`test_release_profile.py`](../../../tests/performance/test_release_profile.py) has seven tests, but
its lifecycle test calls `validate_stage_state` with hand-constructed dictionaries. The profile
tests likewise call isolated validation helpers. None execute `main()` against mocked Make,
compiler, workload runner, profile merger, or final validation, so regressions in ordering and state
writes can pass. For example, these tests would not catch the release path ceasing to invoke
held-out validation or final artifact recording, nor a merge that records a profile without binding
the training report.

Add a lightweight temporary-root/command-mock integration test that drives generate → train → merge
→ release and asserts calls, state transitions, report/profile/artifact digests, and refusal after a
failed or interrupted stage. Include stale-input refusal from the prior finding. Keep it independent
of project compilation.

## Verification

- `python3 -m unittest tests/performance/test_release_profile.py`: 7 tests passed.
- `make --no-print-directory --silent perf-print-flags PERF_MODE=control`: returned the control
  compile/link flags without remaking included dependency files.
- A profile-use flag query with a missing profile failed at Make parse time as intended.
- `git diff --check` passed for the inspected M1 files.
- No project build or runtime workload was run.

# M1 release PGO pipeline review — iteration 01, pass 2

## Verdict: implementation ready; full release gate still needs project build evidence

The two findings from pass 1 are fixed. The driver now reopens and validates the training report at
merge and at both sides of profile-use validation, binds it to the current manifest and current
declared input/dependency hashes, and records the actual release artifacts only after validating
they remain present and unchanged. The new mocked lifecycle tests exercise the stage entry point and
refusal paths rather than only state helpers.

## Review

- `validate_training_report` checks the report hash, manifest version and hash, complete training
  workload ID set, weights and repetition recipe, command argv, scenario, and current hashes/sizes
  for declared inputs and dependencies.
- Merge and release recheck the report and current inputs. Generation, training, merge, release
  build and held-out validation also compare the source identity at their stage boundaries.
- The mocked `main()` lifecycle covers generate → train → merge → release, stage-state writes,
  manifest/profile/report/artifact links, fixture and dependency mutation after training and merge,
  source mutation during generation, stale lock, incorrect stage order, failed validation, and
  missing final output.
- Final release artifacts are hashed before validation and compared again afterward. Missing
  expected outputs fail before the release artifact manifest is written.
- The current M0 manifest/report schema is compatible with these checks. It now has distinct train
  and validation CLI corpus files and probe seeds, separate test-source dependencies,
  generated-probe `scenario_parameters`, and an explicit no-held-out-data claim for source-only
  GoogleTest workloads. Timing and allocation probes use distinct replays. The report retains
  per-workload counters and labels unavailable system counters rather than inventing values.

## Verification

- `python3 -m unittest tests/performance/test_release_profile.py -v`: 14 tests passed, including the
  seven mocked pipeline tests.
- Inspected the current manifest: 12 training workloads and 16 validation workloads, with three CLI
  corpus inputs per split and disjoint source-only test dependencies.
- `git diff --check` passed for the reviewed M1 files.
- No project build, runtime workload, or real profile-use release was run in this review. The M1
  acceptance gate still needs the actual instrumented project pipeline, real held-out validation,
  and backend comparison evidence before the release mode is evidenced end to end.

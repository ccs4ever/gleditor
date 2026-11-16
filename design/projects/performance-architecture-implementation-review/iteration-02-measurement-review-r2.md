# Independent review: M2 measurement integration follow-up

## Verdict

**No remaining measurement-integration blocker found in the revised code and manifest.** The parser
now refuses malformed counter numbers as counted values; the performance runner selects and
validates the declared P3 traversal metric; and the manifest supplies paired timing/count commands
for the five deterministic cohorts in both splits. This review is static only. The baseline
scenarios have not been run, so it makes no runtime-success, P3 speedup, or observed eligibility
claim.

## Checks completed

- Hardware CSV validation now requires nonnegative integer counts and runtime nanoseconds and a
  finite runtime percentage in `[0, 100]`. Invalid values become `None` with an explanatory status;
  raw CSV text/fields remain strings. The tests cover NaN, positive/negative infinity, percentages
  above/below range, negative count/runtime, valid zero, unsupported values, and multiplexing. No
  invalid numeric token is copied into parsed numeric fields, so rejected samples remain
  JSON-serializable without NaN/Infinity numeric values.
- `performance-report.py` accepts a per-workload `timing_metric`, requires its declared scope,
  requires the selected timing value to be positive and finite, records source/scope, and uses that
  metric for `wall_seconds` and summaries. Its synthetic test proves that the isolated P3 duration
  is summarized instead of setup-inclusive `wall_time_ns`, and rejects zero, NaN, and infinity by
  discarding the malformed probe metrics.
- The manifest has 30 train and 21 validation entries, including all five P3 cohorts in each split.
  Train/validation use different sizes and seed tags; the manifest explicitly identifies those tags
  as checksum labels and makes no random-topology or global-eligibility claim. Each P3 timing
  workload declares `p3_link_traversal_ns` and a matching scope, plus a separate
  `--measure allocations` counter command.
- Counter commands run only under `--collect-counters`, after the timing samples, and with
  `measure=allocations`. Their `wall_seconds` is `None`, their timing source is marked
  `not_measured`, and they are excluded from the primary timing summary. Existing workloads without
  a selected metric continue to use the original `wall_time_ns` behavior.
- The hardware collector retains whole-process user-counter scope and separate diagnostic wrapper
  time, verifies source and artifact identities before/after collection, keeps raw CSV and
  unavailable/multiplexed values, and has a source-mismatch-specific diagnostic now.
- The performance-counter documentation describes the primary timing metric, separate count pass,
  cohort-only eligibility scope, checksum-only seeds, and run command.

The author reports that 51 focused tests pass; I did not run them. The production
`ArenaManifold::linked` guard remains unchanged, and the cohorts have not yet been smoke-run against
the baseline. Those runtime checks and post-change correctness/performance measurements remain
required before drawing conclusions about P3.

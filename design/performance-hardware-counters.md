# Supplemental hardware counters

Use `tools/performance-hardware-report.py` to collect user-mode `cycles` and `instructions`
alongside the validated workload report. Supply the exact artifact manifest for the build being
measured and a label that identifies the variant:

```sh
python3 tools/performance-hardware-report.py \
  --artifact-manifest build/performance/release-artifacts.json \
  --variant thinlto-pgo \
  --split validation \
  --output build/performance/thinlto-pgo-hardware.json
```

The default warmup and repetition counts come from `tests/performance/workloads.json`; `--warmups`
and `--repetitions` override them. The collector records each raw `perf stat -x,` CSV, parsed event
values, runtime percentages, process return codes, command/library digests, XDG paths, and the
existing gtest, probe, stdout, and artifact validation results. It rejects artifact/source digest
mismatches and refuses to overwrite the report or raw CSV directory.

Hardware counts cover the command process and inherited user-space subprocess work, including
startup and setup. They do not isolate a probe's internal kernel timer. Missing or unsupported
events and multiplexed counts are stored as null with their status and runtime percentage.
Counter-run wall times are diagnostic; use the matched runs from `tools/performance-report.py` for
timing comparisons.

The parser and fake-perf integration tests are in `tests/performance/test_hardware_report.py`. Run
them without PMU access using:

```sh
python3 tests/performance/test_hardware_report.py
```

## P3 link cohorts

The workload manifest includes paired training (128 cells) and validation (144 cells) scenarios for
arena-local links, base fallback, local quote occurrences, ExternRef quote occurrences, and
canonical proxies. Each timing record selects `p3_link_traversal_ns`; its summary excludes
generated-store and arena setup, semantic validation, and post-traversal inspection. Samples also
record the selected `timing_source` and declared `timing_scope`. The existing `arena-links` workload
remains the general control.

The paired `counter_command` uses the allocation/count executable to report actual linked-call
counts and eligibility classes for the constructed scenario only. Those values are not estimates of
global runtime eligibility. Scenario seed values are distinct checksum tags; the P3 graph topology
is deterministic and is not randomized by those tags.

For the P3 primary timing and its separate scenario-classification pass, run the normal reporter
with `--collect-counters` (after building both probe binaries for the same source/build variant):

```sh
python3 tools/performance-report.py run --split validation \
  --output build/performance/p3-validation.json \
  --variant p3-validation --build-label current-release \
  --pairing-id p3-current-release --collect-counters
```

The manifest pairs each timing command with its count command. The primary sample's `wall_seconds`
and summary use the selected `p3_link_traversal_ns`; `wall_time_ns` remains recorded inside
`probe_metrics` as setup-inclusive whole-scenario duration. Count-command timing is not substituted
into the primary timing summary. Eligibility counts are scoped to the constructed cohort, as
indicated by `p3_eligibility_scope: scenario_only`.

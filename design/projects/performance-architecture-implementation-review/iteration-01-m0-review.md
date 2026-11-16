# M0 harness review — iteration 01

## Verdict: needs fixes before the M0 harness is ready

The focused report tests pass, but they exercise manifest shape and median arithmetic only. The
runner still has correctness gaps that can make training/validation evidence weaker than the
accepted plan requires.

## Findings

### P1 — Workload weights are recorded but never applied

[`performance-report.py`](../../../tools/performance-report.py#L212) runs each workload exactly
`repetitions` times and records its `weight` at line 257. Nothing consumes the weight to
repeat/weight a workload, and it is not applied to an aggregate. The manifest currently sets every
workload to `1.0`, so the declared weights have no training effect. This does not satisfy the plan's
explicit corpus/iteration weighting for PGO training. Define whether the weight controls training
iterations or a documented sampling schedule, validate it, and have the training runner apply it;
report the realized count/weight. If weights are intentionally deferred to M1, make that explicit
and do not describe this M0 manifest as weighted training input.

### P1 — Source-file disjointness does not prove disjoint workload data

[`workloads.json`](../../../tests/performance/workloads.json) lists C++ test source files as inputs.
The overlap check in [`load_manifest`](../../../tools/performance-report.py#L201) compares only
those path strings. The workloads are test filters, and the listed sources may load shared fixtures,
embed common data, or synthesize their own inputs. Consequently the check cannot establish that
training and held-out workload inputs are actually disjoint. Model/hash the concrete corpus or
generated scenario data consumed by each workload (and include any shared data dependency in the
identity), or accurately label this check as source-file disjointness and defer the held-out-data
claim until the corpus is modeled.

### P1 — Successful GoogleTest exit does not prove the workload executed tests

The manifest passes `--gtest_fail_if_no_test_selected`; this catches an empty filter selection, but
a selected suite whose tests are all skipped can still exit successfully.
[`run_one`](../../../tools/performance-report.py#L127) trusts only the process return code.
Parse/emit a machine-readable test count and require at least one completed test (and record skipped
count), or use a small wrapper that verifies the GoogleTest result summary. Add a regression test
for zero executed tests.

### P2 — Peak RSS conversion is Linux-specific despite a Unix/macOS-compatible path

[`_measure_child`](../../../tools/performance-report.py#L284) multiplies `ru_maxrss` by 1024
unconditionally. Linux reports this field in KiB, while macOS reports bytes, so the reported macOS
peak RSS is 1024 times too large. Convert according to `sys.platform`/documented platform units, and
add unit tests for both conventions. `ldd` in `command_identity` is Linux-specific too, but its
`OSError` is caught and recorded as `null`; it is an honest missing identity rather than a false
value. Add a macOS equivalent (such as `otool -L`) if the harness claims library identity on macOS.

### P2 — Reports lack complete invocation and paired-comparison identity

The report records the workload argv and selected split, but not the top-level invocation (including
overrides for repetitions/warmups/output), an explicit build/variant label, or a pairing key. Each
workload is run seven times consecutively for one split; there is no baseline/candidate interleaving
or pairing mechanism. The accepted plan calls for raw commands and paired runs so comparison claims
can be reproduced and attributed. Record the full normalized invocation and variant/build identity,
and define/implement pairing before using these reports for comparisons.

### P2 — Dirty submodule contents are not identified by digest

[`source_identity`](../../../tools/performance-report.py#L48) hashes dirty paths only when
`ROOT / name` is a file. Git status reports a modified submodule as a directory entry, so its dirty
files get no digests; `git submodule status` records the pinned commit and dirty marker only. This
does not meet the plan's source/submodule revision plus dirty-file digest identity. Collect status
and hashes inside each dirty submodule, or fail closed when a submodule is dirty and cannot be
enumerated.

### P3 — Log directory names can collide for two runs started in the same second

[`run`](../../../tools/performance-report.py#L230) uses second-resolution timestamps and `mkdir()`
without `exist_ok`; concurrent or rapid same-split invocations can fail before producing a report.
Use a collision-resistant run ID (UTC microseconds plus PID/random suffix) and store it in the
report. Also ensure output/report writes are atomic so interruption cannot leave a truncated file at
the requested output path.

## Required coverage gaps (not false passes)

The report honestly marks allocation counts/bytes, hardware cycles/instructions, and subsystem
counters unavailable at [`performance-report.py`](../../../tools/performance-report.py#L249). The
listed suite does not yet demonstrate backend rendering coverage. These are unfinished M0 coverage
items under the accepted plan; the report does not claim they were measured, so I am treating them
as gaps rather than false successes. Add/associate the dedicated probe and backend workload before
claiming full M0 observability.

## Verification

Ran `PYTHONDONTWRITEBYTECODE=1 python3 -m unittest tests/performance/test_performance_report.py -v`:
3 tests passed. No build or runtime workload was run.

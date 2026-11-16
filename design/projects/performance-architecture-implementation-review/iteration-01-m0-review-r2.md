# M0 harness rereview — iteration 01, pass 2

## Verdict: remaining fixes and evidence gaps

The previous seven findings are substantially addressed: training weights now affect the measured
repetition count, the manifest labels its source-file-only disjointness honestly, gtest JSON gates
on a nonzero executed count, Linux/macOS RSS units differ, reports and sample XDG state use unique
run IDs, invocation/variant/pairing labels are retained, and dirty submodule contents are
enumerated. Focused tests pass (8/8).

## Findings

### P2 — Structurally malformed but valid GoogleTest JSON aborts the whole report

[`parse_gtest_result`](../../../tools/performance-report.py#L182) assumes the root and every nested
value are dictionaries/lists of the expected shape. For example, valid JSON `[]` raises
`AttributeError` at `node.get`; `run_one` catches `OSError`, `ValueError`, and `JSONDecodeError`
only at lines 286–290, and the top-level handler also does not catch `AttributeError`. I reproduced
this with `parse_gtest_result` on a temporary file containing `[]`. The runner exits without writing
the report instead of retaining a failed sample and continuing to emit partial results. Validate the
schema and raise `ValueError` (or return an invalid-result error) for malformed shapes; make the
sample unsuccessful while preserving the report. Add tests for a non-object root, malformed suite
entries, and a structurally valid all-skipped result.

### P2 — Dirty top-level rename/quoted paths can still lack content digests

[`source_identity`](../../../tools/performance-report.py#L49) parses human-readable porcelain output
by splitting lines and taking `line[3:]`. Git porcelain v1 quotes unusual pathnames and represents
renames as `old -> new`; those strings are not necessarily real paths, so the code can record a
dirty entry without hashing the changed file(s). This leaves source identity incomplete for the
working-tree changes M0 is meant to fingerprint. Use NUL-delimited porcelain output and handle the
second path of rename/copy entries explicitly; test a staged rename and a filename requiring
quoting.

### P3 — Weight tests validate the helper, not the actual runner's sample count

The implementation does apply `effective_repetitions()` at
[`run`](../../../tools/performance-report.py#L423), and its report records realized counts. The test
at [`test_performance_report.py`](../../../tests/performance/test_performance_report.py#L35) only
checks the helper's return values, however; it would still pass if the run loop stopped using that
helper. Add a lightweight integration/unit seam that runs a tiny weighted manifest workload and
asserts the actual training sample count/report `realized_measured_iterations`, while validation
remains unweighted. This is a regression-test gap rather than a current weight-application defect.

## M0 gate still open (coverage gap, not false pass)

The manifest explicitly limits its identity guarantee to listed test source files and says it does
not establish held-out corpus-data separation. That is accurate. The current report also marks
allocation counts/bytes, hardware cycles/instructions, and subsystem counters unavailable; the new
probe is separate from the workload/report flow. The workload set does not exercise backend
rendering. These are still M0 corpus/backend/probe-integration coverage gates; do not mark M0 as
complete until the chosen concrete workload inputs and required probe/backend evidence are attached
to the same report. None of these are presented as measured successes.

## Verification

Ran `PYTHONDONTWRITEBYTECODE=1 python3 -m unittest tests/performance/test_performance_report.py -v`:
8 tests passed. Probed `parse_gtest_result` with valid JSON `[]`; it raises uncaught
`AttributeError`. No build or runtime benchmark workload was run.

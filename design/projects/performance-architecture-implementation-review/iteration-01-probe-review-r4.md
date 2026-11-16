# M0 performance probe split review — iteration 01, pass 4

## Verdict: ready

The timing and allocation probes are now separate binaries with the intended instrumentation
boundary. The ordinary `performance-probe` translation unit is compiled without
`GLEDITOR_PERF_ALLOC_PROBE`; all global `operator new`/`delete` replacements and counter state are
inside that macro guard in `tools/performance-probe.cpp`. The `performance-probe-counts` target uses
a separate object compiled with the macro. The schema note identifies its `both` timing as
diagnostic only because its allocator replacement remains interposed even while the count flag is
off.

## Evidence

- [`split-probe-validation/results.json`](../../../build/performance/iterations/split-probe-validation/results.json)
  records `nm --defined-only` results: the timing executable has no defined global allocation
  provider; the counts executable has the expected provider. Direct `nm -C --defined-only` review
  confirmed no `operator new`/`delete` definitions in `build/performance-probe` and the replacement
  forms in `build/performance-probe-counts`.
- The same artifact records 36 successful scenario/mode/size runs and three expected mode refusals:
  timing binary rejects `allocations` and `both`, and counts binary rejects `timing`.
- The fresh M0 reports, `split-m0-train.json` and `split-m0-validation.json`, both pass. They cover
  12 training and 16 validation workloads, with six separate allocation-counter runs in each report.
  A spot check confirms timing samples use `build/performance-probe` with `measure: timing` and null
  allocation metrics; counter samples use `build/performance-probe-counts` with
  `measure: allocations` and populated counts.
- `performance-report.py` only launches a workload's `counter_command` when `--collect-counters` is
  requested. The PGO training driver calls the runner without that option, so the allocation
  executable is not part of the training workload. The pipeline nevertheless builds and hashes both
  probe binaries through `TARGETS` and `build_artifacts()`.

## Build-rule review

`PERF_PROBE_COUNT_OBJ` has a distinct object path. Its compile rule depends on the source, Makefile,
and shared flag stamp; the matching `.dep` and `.j` rules use the same filtered count flags and
macro. It is included in the aggregate object/dependency/compile-command lists, so transitive
headers and compile-command generation remain covered. Its executable links that object with the
selected-mode engine objects, as intended for diagnostic counts against the chosen engine build. The
shared flag stamp includes mode and profile digest, so mode/profile changes rebuild it even though
its own compile flags strip PGO generation/use options.

No incorrect phase fields were found in the JSON paths. The timing binary refuses allocation
measurement requests; the counts binary labels whole-scenario allocation totals separately from the
fold-only allocation window. M0 counter runs use the counts executable as a separate pass, and the
runner does not use that pass as timing evidence.

No findings remain in this split-specific review. This verifies the executable/instrumentation
boundary and phase labeling; it does not add evidence for still-unavailable runtime counters listed
in the prior M0 reviews.

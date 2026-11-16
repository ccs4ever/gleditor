# Independent review: hardware collector and M2 P3 measurement patches

## Verdict

**Do not collect the P3 baseline yet.** The scenario construction and the hardware-counter wrapper
are close to the intended model, but two concrete measurement defects need correction before
baseline results are interpreted:

1. The P3 timing probe's primary `wall_time_ns` is still the outer duration of `runScenario`, which
   includes store/arena construction, validation and structure inspection. The probe emits
   `p3_link_traversal_ns` for the isolated read loop, but `performance-report.py` chooses only
   `wall_time_ns` for `wall_seconds` and summary ratios. Therefore the documented/standard timing
   report will compare setup-inclusive samples, not the P3 linked() loop. Wire the P3 traversal
   duration into the primary probe timing field for these scenarios, or teach the workload reporter
   to select and summarize the dedicated field with explicit schema validation. Do not use the
   existing wall-time summary for P3 speed claims.
1. `parse_perf_csv` accepts non-finite runtime percentages. `float("nan")` parses successfully;
   because `nan < 100.0` is false, that event is labeled `counted` and its value is retained.
   Positive infinity is also counted. Negative counts and negative runtime nanoseconds can likewise
   be labeled counted when percentage is 100. Require finite, in-range percentage values and
   nonnegative count/runtime values before marking an event counted; add fake CSV cases for NaN,
   infinities, negatives, and out-of-range percentages.

The P3 scenarios are also not connected to the checked-in workload manifest in these patches. If
baseline collection will use `performance-report.py`, add separate timing and allocation-count
workload entries for the scenario classes (and record them in the comparison protocol); otherwise
specify and validate an equivalent raw-record procedure before running scenarios. This is a
measurement integration gap, not a C++ semantic defect.

## What checks out

- Hardware collection reuses `performance-report.py`'s `run_one`, placeholder expansion, headless
  environment, output validation and fresh per-sample XDG directories. Wrapping the command with
  `perf stat ... -- <original command>` preserves probe/gtest validation, and the fake subprocess
  test exercises both warmup and measured runs.
- Counter output is stored separately from timing runs. The collector marks missing/unsupported and
  sub-100% multiplexed event values null, retains raw CSV/runtime metadata, identifies whole-process
  user counter scope, and explicitly treats wrapper wall time as diagnostic. It verifies artifact
  hashes and source identity both before and after the set of samples and records the perf version
  and command metadata.
- The artifact manifest verifier checks every listed binary digest and the optional source content
  identity. One minor diagnostic issue: if only source differs, the thrown message says
  `artifact/source identity mismatch: []`; include the source mismatch explicitly in that error.
- P3 generated cohorts exercise local arena links, base fallback, quote occurrences with
  `ValueKind::None`, quote occurrences marked `ExternRef`, and canonical proxies. Setup checks
  topology/identity and reads both directions. The traversal count is calculated from actual loop
  bounds; `ceil(32768 / (2 * (size - 1)))` repetitions guarantees at least 32,768 actual `linked()`
  calls for valid `size >= 2`. Calls and eligibility counts are emitted only by the separate
  allocation/count build; eligibility is explicitly labeled `scenario_only`, with only the
  locally-owned non-ExternRef and quote-None cohorts designated eligible. That avoids claiming a
  global workload eligibility rate.
- The timing loop itself is cleanly separated from cohort construction and post-loop inspection in
  `p3_link_traversal_ns`; it performs the link lookup, result check and checksum. Allocation
  telemetry is in the separately built count executable, so it need not pollute the primary loop.
- The aligned-allocation adjustment raises small POSIX alignment requests to at least pointer
  alignment and preserves the requested byte accounting. It is appropriate for normal
  aligned-new/PMR inputs; invalid non-power-of-two alignment is still not normalized, which is
  correct because such alignment is not a valid allocator request.

## Conditions before baseline collection

1. Fix the primary P3 timing field/report selection so the comparison uses the isolated traversal
   duration, not full setup-inclusive process duration.
1. Reject malformed/non-finite counter percentages and impossible negative values; expand the
   synthetic parser cases.
1. Add the P3 cohorts to the workload/report path or document a directly reproducible alternative
   that records source/binary identity, class parameters, timing binary output, count binary output,
   and per-run results separately.
1. After applying the patches, run the existing 41 root checks and the six synthetic tests as
   specified. Then build the baseline and execute P3 scenarios; the scenario timings have not yet
   been run, so no timing or eligibility result can currently be claimed.

No production `ArenaManifold::linked` fast-path change is included in the reviewed P3 probe patch.
The generated scenarios can provide a before/after harness once the actual-source implementation is
applied and retested, but their pre-change behavior alone does not demonstrate the optimization.

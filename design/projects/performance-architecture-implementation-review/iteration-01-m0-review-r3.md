# M0 core and corpus review — iteration 01, pass 3

## Verdict

The runner, manifest, and corpus are now credible enough to freeze as the workload recipe for the M1
PGO pipeline. The focused runner tests pass (12/12), and the saved baseline reports are complete for
the declared workloads. This does not close every M0 observability item or validate a PGO release
comparison: the probe timing executable still has allocator-hook overhead, selected linker identity
is not reliably reported, and several planned fine-grained counters remain unavailable.

## Verified evidence

- `PYTHONDONTWRITEBYTECODE=1 python3 -m unittest tests/performance/test_performance_report.py -v`:
  12 passed. The suite now exercises real `run()` sample counts for weighted training versus
  unweighted validation, malformed GoogleTest output as a failed report, NUL-delimited
  rename/quoted-path hashing, and counter-pass separation.
- `build/performance/iterations/resume-m0-train.json`: report schema 1, successful, 12 workloads, 13
  primary measured samples (the VPL workload's weight is 2), six successful separate
  allocation-counter samples, and schema 2 for all six timing probes.
- `build/performance/iterations/resume-m0-validation.json`: report schema 1, successful, 16
  workloads and 16 primary measured samples, six successful separate allocation-counter samples, and
  schema 2 for all six timing probes.
- Both reports contain successful nonempty GoogleTest counts, matched VPL/VProlog CLI output checks,
  and nonempty headless render artifacts of 1,440,015 bytes each. Train and validation CLI corpus
  files are distinct and hashed. Probe scenario/size/seed and probe source dependencies are
  recorded; train and validation probe seeds differ. The manifest correctly makes no held-out corpus
  claim for source-only GoogleTest workloads.
- Counter collection is explicitly opt-in, runs after primary timing in a separate process, and its
  runtime is not substituted for the primary sample time. The report accurately labels the probe's
  wall-time scope as the full generated scenario and separately records fold-kernel and public byRef
  scan timings where available.

## Findings before interpreting timing or freezing a comparison recipe

### P1 — The probe's timing mode still executes the allocation hook on every C++ allocation

[`performance-probe.cpp`](../../../tools/performance-probe.cpp#L31) calls `noteAllocation()` from
every replacement `operator new` path, including timing-only runs. Even with
`countAllocations == false`, `noteAllocation()` performs an atomic load for each allocation. The two
run modes are separate and counts are not incremented during timing, but the timing executable still
carries allocation-hook overhead. This violates the plan's requirement that timing runs not include
allocation instrumentation and can bias allocator-heavy scenarios. Build/use a timing probe without
the replacement hooks and a separate allocation-counting probe, or otherwise make the timing path's
`operator new` a normal uninstrumented implementation. The test that checks separate child timing
validates phase ordering, not the absence of this per-allocation hook cost.

### P2 — The report's linker version can name a linker that was not used

[`run()`](../../../tools/performance-report.py#L569) records `ld --version` unconditionally. The
Makefile's ThinLTO path adds `-fuse-ld=lld`; under that mode this field will still report the system
`ld` (GNU ld here), not the selected `ld.lld`. The buildflags show the flag but do not make the
recorded linker-version field accurate. Resolve and record the linker selected by the actual
compiler/link command, or extract it from a recorded verbose link command. This matters for
reproducible ThinLTO comparisons.

## M0 telemetry still unavailable or narrower than the plan

These are explicit gaps in the report, not false passes:

- Hardware cycles and instructions are always listed as unavailable; the runner does not probe
  whether a usable hardware-counter tool is present or distinguish “unsupported/unavailable” from
  “not collected.”
- Allocation metrics cover C++ `new` calls and requested bytes in the probe, plus fold-window
  counts. They exclude C allocation APIs, allocator overhead, and runtime-wide allocations.
- The probe reports byRef density but not lookup-table initialization/page-fault or
  reserved/committed capacity work.
- Operand distribution is limited to arities 0, 8, and 9. It captures the first spill boundary, not
  a broader large-arity/spill distribution.
- Snapshot reporting counts selected top-level map entries and choice points; it does not measure
  actual snapshot-copy counts/bytes or nested/omitted state. The probe itself documents exclusions
  for slot sets, Dewey paths, and nested payloads.
- Occurrence fast-path eligibility, mutable-payload copied elements, active boundary/table
  capacities, and root-steal/rebase/move work remain unavailable, as the report states.
- Rendering is demonstrated by a nonempty screenshot on the configured headless path, but the report
  does not record the backend actually selected or provide per-backend results.
- VQL is represented by a held-out GoogleTest workload, not by a train-side CLI workload. This is
  transparent in the manifest, but VQL behavior will be cold in a profile trained only from the
  train split unless another train workload is added.

The corpus and harness can be frozen for M1 pipeline development, with these coverage gaps kept
visible. Do not use the current probe timing numbers as uninstrumented timing baselines, and do not
label M0 observability complete until the listed counters/backend identity are either collected or
explicitly dispositioned.

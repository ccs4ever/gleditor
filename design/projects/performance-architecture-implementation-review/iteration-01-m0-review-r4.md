# M0 core and corpus review — iteration 01, pass 4

## Verdict

The frozen runner, corpus, and probe split are ready to source-freeze for M1 PGO generation. The
earlier linker identity defect is fixed and verified against the host Clang/LLD pair. The saved M0
reports are successful and cover the declared training and validation scenarios. M0 observability
remains partial; the specific counter gaps below should stay visible in later performance reports.
No PGO release comparison was run in this review.

## Verification

- Ran
  `PYTHONDONTWRITEBYTECODE=1 python3 -m unittest tests/performance/test_performance_report.py -v`:
  16 tests passed.
- Fresh report [`split-m0-train.json`](../../../build/performance/iterations/split-m0-train.json) is
  successful with 12 workloads and 13 measured primary samples (the VPL workload has weight 2); all
  six separate counter passes succeeded.
  [`split-m0-validation.json`](../../../build/performance/iterations/split-m0-validation.json) is
  successful with 16 workloads, 16 measured primary samples, and six successful counter passes. The
  reports are schema 1; each of the 12 timing probes returned probe schema 2.
- Both report sets contain successful nonempty GoogleTest counts, matched VPL/VProlog CLI output
  checks, and nonempty headless screenshots of 1,440,015 bytes. CLI fixture files are distinct and
  content-hashed. Probe source dependencies and scenario parameters are recorded; corresponding
  train/validation probes use distinct seeds and sizes. GoogleTest inputs are explicitly source-only
  and make no held-out corpus claim.
- [`results.json`](../../../build/performance/iterations/split-probe-validation/results.json)
  records 36 successful probe runs, three expected wrong-mode refusals, no defined `operator new` in
  `performance-probe`, and one defined `operator new` in `performance-probe-counts`.
- The control reports identify GNU ld 2.47. I also called
  `configured_linker_identity("-O3 -fuse-ld=lld")` on the host; it resolves `/usr/bin/ld.lld` and
  reports LLD 23.1.4 with return code 0.

## Linker identity defect found and fixed during this review

The first revised selector asked Clang for `lld`, which returned `/usr/bin/lld`, the generic
dispatcher. Running that path with `--version` failed, even though the report initially called the
path resolved. The final code maps `-fuse-ld=lld` to the platform linker program (`ld.lld` here),
preserves the reported symlink basename, and treats a nonzero version command as unknown. The live
host check and new tests now pass. The report records the exact linker selected by the compiler
driver for the actual build flags.

## Remaining M0 telemetry and training-coverage gaps

These remain explicit gaps rather than false passes:

- Hardware cycles and instructions are listed unavailable, but the runner does not test for a usable
  hardware-counter facility or distinguish unsupported hardware from counters not collected.
- Allocation probes count C++ `new` calls and requested bytes, including a fold-window subset. They
  do not count C allocation APIs, allocator overhead, or runtime-wide allocation totals.
- byRef density is recorded, but lookup initialization/page-fault work and reserved/committed table
  capacity are not.
- Operand arity coverage is 0, 8, and 9, which checks the first spill boundary but not a broader
  large-arity spill distribution.
- Snapshot metrics record choice points and selected top-level map entries. They do not count all
  snapshot copies/bytes or deep state; the probe documents exclusions for slot sets, Dewey paths,
  and nested payloads.
- The arena-links scenario traverses a direct local rank only; it does not measure quote or proxy
  traversal. Occurrence fast-path eligibility, mutable-payload copied elements, active
  boundary/table capacity, and root-steal/rebase/move work are also unavailable.
- The report validates a headless screenshot but does not name the backend selected for that sample.
  The separate backend comparison is not embedded as per-backend evidence in the M0 JSON report.
- VQL is present only in a validation GoogleTest workload. A training-only PGO profile will leave
  VQL behavior cold unless a representative VQL training workload is added or the M1 coverage report
  records that gap.

During PGO profile generation, keep `--collect-counters` disabled (its default) unless counter-pass
profiles are excluded from the merged training set. The count executable has a dedicated unprofiled
probe translation unit, but it links the selected-mode engine objects and inherits
`LLVM_PROFILE_FILE`; in profile-generation mode those engine objects can still emit raw profiles
from the allocation pass.

M1 profile generation can proceed with this frozen workload recipe. Do not use the current
timing-probe values as uninstrumented application timings or mark M0 cost observability complete
until the remaining fields are collected or dispositioned.

# M4: operand list (P5) measurement review (self-review, no Luna available)

Baseline is the M3 commit `d2ec4f4d` (source identity `b413cd25…`). Candidate is the M3 tree plus
`restart-m4-operand-list.patch` (patch SHA-256 `50cbbd10…1d8a`, source identity `8e740ca1…`). Both
are ThinLTO probe pairs built with the same compiler, flags and libraries. This review is by the
author, not independent.

## Process notes (kept so the evidence can be trusted)

- A first candidate variant was **invalid**: `make build/performance-probe` matched no rule, so make
  did nothing and the script archived the stale C2 PGO probe. It is quarantined as
  `m4-INVALID-stale-pgo-probe-not-rebuilt/`. The build script now builds the real phony targets and
  asserts each probe was rebuilt and differs from the pre-build binary.
- The first baseline attempt was refused by the identity guard. `git apply -R` leaves the patch's
  two new files behind as untracked, so the tree was not M3. The script now removes them, requires a
  clean tree, and asserts identity `b413cd25…`. The baseline then built at exactly that identity.
- A first comparison run was started while the host load average was 9.1 (left over from the
  baseline build) and was aborted before producing evidence. It is kept as
  `m4-ABORTED-load-contaminated-start/`. The accepted run started at load 0.08.

## Correctness

| Gate                                | Result                                           |
| ----------------------------------- | ------------------------------------------------ |
| Full `make -j$(nproc)` linked build | passed                                           |
| Full Xuzz                           | 4475 tests, 0 failures, 12 disabled (M3: 4469)   |
| Full ZigZag                         | 268 tests, 0 failures                            |
| clang-format 19 on touched files    | clean                                            |
| Integrated ASan/UBSan engine run    | **pending** (`make sanitize/engine-address/run`) |

The +6 Xuzz tests are the new `operand_list_test.cpp` cases plus the added `vortex_core_test.cpp`
case. Helper tests passed earlier on native and forced Beman builds, normal and ASan/UBSan.

## Allocation counts (deterministic, `operands` scenario, whole-scenario C++ `new` calls)

| Size   | M3 baseline | M4 operand list | Change |
| ------ | ----------- | --------------- | ------ |
| 144    | 130         | 82              | -37%   |
| 1,000  | 705         | 372             | -47%   |
| 10,000 | 6,712       | 3,379           | -50%   |

The saving at 10,000 is 3,333 calls, which is the number of eight-operand operations: they now fit
inline. Nine-operand operations still spill, as designed. Total requested bytes fall by only about
1%, because the arena dominates the scenario. `vm-snapshots` saves a fixed 4 calls. Checksums match
in every case.

## Timing (ThinLTO, CPU-pinned interleaved blocks, 7 runs plus warm-up, unadjusted)

Candidate over baseline, median ratio with temporal-block bootstrap 95% interval:

| Scenario             | 144                | 1,000              | 10,000             |
| -------------------- | ------------------ | ------------------ | ------------------ |
| operands (whole)     | 0.989 [0.88, 1.15] | 1.067 [0.75, 1.13] | 0.962 [0.93, 0.99] |
| vm-snapshots (whole) | 1.025 [1.00, 1.13] | 0.988 [0.95, 1.03] | 1.014 [1.00, 1.04] |
| fold-text (control)  | 0.981 [0.90, 1.04] | 0.984 [0.91, 1.02] | 1.047 [0.90, 1.11] |

- `fold-text` is unaffected by M4 and shows the noise floor: ratios of 0.98 to 1.05 with intervals
  of roughly ±10%.
- Only `operands` at 10,000 has an interval that excludes 1: about 4% faster. Every other interval
  crosses 1.
- `operands` and `vm-snapshots` only have a whole-scenario timer, which includes building the arena
  and cells. A halved allocation count is therefore diluted, and a gain of a few percent is
  consistent with that. No per-call kernel was timed.
- No regression is detected. The evidence does not show a throughput improvement beyond the 10,000
  operands case. It does show the intended allocation reduction.

## Not established

- Real application weights. The probe uses synthetic 0/8/9 operand cohorts, so no application-level
  speedup is claimed.
- A per-call timing of `inputsOf`, and the cost of the 9+ spill path versus the old vector.
- Integrated ASan/UBSan results (pending above).

## Recommendation

Retain M4 if the integrated sanitizer run is clean. It removes about one in three of the operand
vector allocations in this scenario at no detected timing cost, and keeps the 64-byte host object
and the spill path. If the sanitizer run finds a defect, fix it or fall back to the vector.

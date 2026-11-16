# Iteration 03: C2 final PGO review (self-review, no Luna available)

Reviewer: the resuming agent acting as the C++ performance reviewer, because Luna agents were
unavailable. This is not an independent review and should be labelled that way in the journal.

Source `b413cd256f60ec0549d0a8fa3b181026272a059357391ac8e9463f8689f5cf65`, profile `ef269ceb…2bd7`.
Baseline: accepted P3 PGO, source `09da6e1b…0873`, profile `d7e367d8…a6a5`.

## Gates

| Gate                                            | Result                                           |
| ----------------------------------------------- | ------------------------------------------------ |
| PGO generate/train/merge/release, 26 artifacts  | passed                                           |
| 21-case held-out validation, timing, 7 reps     | passed                                           |
| Hardware collection, 168 samples, 21 workloads  | passed on rerun (see identity-drift note)        |
| Full Xuzz                                       | 4469 tests, 0 failures, 12 disabled              |
| Full ZigZag                                     | 268 tests, 0 failures                            |
| 252-sample paired probe comparison and analysis | ran; mixed result below                          |
| `fold-dispatch-inspection.json`                 | no stack subtraction in `applyStructure` (181 B) |

### Identity-drift note

The first hardware report failed `identity_unchanged`. The tool's identity is
`git status --untracked-files=all` plus file hashes, so it includes Markdown and patch files. The
handoff archives (`restart-*`) were written into `design/` during collection, adding 7 dirty
entries. The 20 original dirty-source entries and both artifact manifests were identical, and the
code fingerprint (`tools/release_profile.py::source_identity`, which hashes only code suffixes)
stayed `b413cd25…`. The failed report is preserved as `hardware.failed-identity-drift*.json`. The
guard was not changed. Only the hardware step was repeated, with no repo writes during collection.

## Paired C2/P3 PGO ratios (kernel metric, temporal-block bootstrap, unadjusted)

| Scenario       | 144                | 1k                 | 10k                |
| -------------- | ------------------ | ------------------ | ------------------ |
| fold-text      | 1.205 [1.13, 1.45] | 0.947 [0.82, 1.08] | 0.853 [0.74, 0.88] |
| fold-structure | 0.946 [0.93, 0.96] | 1.007 [0.89, 1.08] | 0.976 [0.95, 1.02] |
| fold-branch    | 1.630 [1.32, 1.75] | 1.294 [1.19, 1.60] | 1.495 [1.26, 2.35] |

The five arena-link scenarios have intervals crossing or touching 1.

## Absolute kernel medians, ns (P3 → C2)

| Case               | P3     | C2     | Delta  | Shape (cells, work items, byRef) |
| ------------------ | ------ | ------ | ------ | -------------------------------- |
| fold-branch 144    | 4,869  | 7,935  | +3,066 | 3, 1, 3                          |
| fold-branch 1k     | 6,302  | 8,155  | +1,853 | 3, 1, 3                          |
| fold-branch 10k    | 7,494  | 11,201 | +3,707 | 3, 1, 3                          |
| fold-text 144      | 3,226  | 3,887  | +661   | 0, 144, 0                        |
| fold-structure 144 | 43,723 | 41,379 | -2,344 | 147, 144, 291                    |

## Findings

1. **The `fold-branch` regression is a fixed cost on a tiny single-shot kernel.** Every size folds 3
   cells from one work item. The kernel is 5–11 µs, one fold per process, and the size parameter
   only changes the operation range. The delta is +1.9 to +3.7 µs and does not scale with size. The
   ratios are large because the denominator is tiny. They do not show a per-operation slowdown.
1. **Where the fixed cost comes from is not diagnosed.** Plausible sources are the larger `Manifold`
   (+192 B), first-use allocation of the dense/presence/overflow tables and the PMR births map, and
   the prepared-insert tokens. None of these was isolated by measurement. Do not cite any one of
   them as the cause.
1. **Static dispatch is not the cause.** `applyStructure` is 181 bytes with no stack subtraction,
   and the ignored-context path no longer builds a mutation frame.
1. **C2 wins where there is real work.** `fold-structure` at 144 is 5% faster in absolute terms
   (-2.3 µs) with a tight interval. `fold-text` at 10k is 15% faster, and held-out `probe_fold_text`
   is 0.741 [0.659, 0.991].
1. **Whole-scenario held-out timings do not decide this.** Most rows sit at or below 1.0, but they
   include process startup and input generation and mask kernel costs. They are serial blocks with
   independent bootstrap, not paired.
1. **ThinLTO showed the same direction with wider intervals.** `fold-branch` was 1.282 / 0.944 /
   1.121, with intervals crossing 1. PGO narrowed the intervals, which makes the penalty look more
   consistent. It did not shrink it.

## Bounded fixed-cost diagnostic (`fold-branch`, PGO binaries)

Run after the owner chose to retain C2 after one diagnostic. Not part of the frozen measurement set.

- **Allocations are not the cause.** Fold-window C++ `new` calls: P3 21 (1,028 B), C2 20 (1,190 B),
  identical at sizes 144 and 10k.
- **Whole-process counters, pinned, 300 runs, two interleaved rounds.** Instructions: +30k to +35k
  (+0.03%). Cycles: +0.05% to +0.3%, within noise. L1-icache misses: flat. Page faults: **+2** (1252
  → 1254) in both rounds. The kernel is about 8 µs inside a roughly 25 ms process, so these counters
  cannot isolate the kernel and the instruction delta is larger than the kernel delta.
- **Page-fault attribution (`perf record -e page-faults -c 1`, one run each).** About 750 of the
  ~1250 faults are in the dynamic loader, which is the same for both. In engine code the C2 run has
  one fault each in `operator new` and `std::pmr::get_default_resource()` that the P3 run lacks.
  These are libstdc++ code pages, so they are first-touch faults on code, not heap growth.
- **Reading.** The evidence is consistent with a first-touch cost. C2 constructs a PMR-backed
  `Manifold` and calls `get_default_resource()` on a path a fresh probe process has never executed.
  Two minor faults at roughly 0.5–1 µs each match the observed +1.9 to +3.7 µs. **This is supported,
  not proven.** It is one attribution sample, whole-process counters, and a hypothesis about page
  residency. A resident editor has already touched these pages, so the penalty is probably specific
  to fresh single-fold processes. That has not been measured in the application.
- **No cheap fix.** Lazily allocating the births map would not remove the `get_default_resource()`
  call or the `operator new` first touch. No fix was attempted, to avoid the unbounded fourth
  experiment.

## Limitations

- Shared host, unadjusted intervals, seven serial repetitions, and no application workload weights.
  No aggregate or universal speedup is claimed.
- Hardware counters are whole-process, including startup, and are diagnostic only.
- This review is by the author, not independent.

## Decision (recommendation, owner to confirm)

Retain C2 conditionally. It improves the work-dominated kernels and removes the per-fold mutation
frame. The cost is a bounded, non-scaling +2–4 µs on folds that already take under 12 µs, plus +192
B per `Manifold`. Both costs are recorded here and should be carried into the proposal prose. The
original P3 fallback stays available as `restart-c2-final-candidate.patch` (reversible) and the
accepted P3 archive.

Before committing M3:

1. Run the global format check and touched-Markdown lint.
1. Update the proposal and implementation-policy prose (known counts use tight bounds, unknown
   retains the 4096 floor, unordered births chosen on measured cost, capacity slack and PMR costs).
1. Do not run another storage experiment. Optionally bound the fixed-cost question with one
   diagnostic (perf-record or a first-use allocation count on `fold-branch`), but do not block M3 on
   it.

# M0 performance probe review — iteration 01

## Verdict: needs fixes before relying on the reported metrics

The six scenarios are real engine calls and the workload builders do useful work. The smoke run
recorded in `build/performance/iterations/probe-smoke/results.json` built the control probe and ran
all six scenarios with `timing`, `allocations`, and `both` at size 16 / seed 7: all 18 commands
returned 0 and produced parseable JSON. That is useful execution evidence, but it does not close the
metric-definition and self-check gaps below. No heavy build or test suite was run for this review.

## Findings

### P1 — Unmeasured common metrics are serialized as factual zeroes

`WorkResult` initializes every count/checksum to zero
([`performance-probe.cpp:110-127`](../../../tools/performance-probe.cpp#L110)), and JSON always
prints `dimension_run_count`, `empty_dimension_run_cells`, and `checksum`
([`performance-probe.cpp:439-442`](../../../tools/performance-probe.cpp#L439),
[`performance-probe.cpp:501-502`](../../../tools/performance-probe.cpp#L501)). Several scenarios
never populate those fields. For example, `runVmSnapshots()` creates a spin link at
[`performance-probe.cpp:326`](../../../tools/performance-probe.cpp#L326), but its smoke JSON reports
`dimension_run_count: 0`; it also reports `empty_dimension_run_cells: 0` and `checksum: 0` without
measuring those quantities. `fold-branch` similarly leaves the empty-cell count unset. These zeroes
look like measured structural facts and can poison comparisons or aggregate reports.

Make not-collected fields optional and emit JSON `null`; where a scenario claims the metric, compute
it and self-check it. Apply the same rule to any other common field whose value is currently just a
default initializer. Preserve genuine zeroes, such as the text-only fold's zero by-ref entries, when
the probe actually measured them.

### P1 — `by_ref_per_dense_slot` divides by cell count, not dense-slot capacity

At [`performance-probe.cpp:458-462`](../../../tools/performance-probe.cpp#L458), the numerator is
the number of operation references found by `slot()` while the denominator is `metrics.cells`, which
comes from `Manifold::cellCount()`. The manifold's dense slots are cells, but `byRef` has an entry
for every operation in each cell's chain; its entry count can therefore exceed the cell count. The
smoke output makes the mismatch concrete: `fold-structure` reports 51 by-ref entries for 35 cells,
so this field would report about 1.46 “per dense slot.” That is not a density or capacity ratio.
`by_ref_per_op_range` is the useful-entry density against the full operation-index range and is
already emitted separately.

Remove `by_ref_per_dense_slot`, or rename it to the literal entries-per-cell ratio and do not use it
as a density measure. If the intended denominator is allocated lookup capacity, expose and report
that capacity directly. Keep the operation-range ratio for sparse-domain density.

### P2 — Fold allocation counts include construction and inspection

The allocation counter is enabled around the entire `runScenario()` call
([`performance-probe.cpp:408-414`](../../../tools/performance-probe.cpp#L408)). For each fold
scenario this counts Store and input construction, `rebuildManifold()`, `scanByRef()`, post-fold
inspection, and destruction. In contrast, `fold_kernel_ns` times only `rebuildManifold()` at
[`performance-probe.cpp:201-205`](../../../tools/performance-probe.cpp#L201) and corresponding
text/structure sites. Thus the allocation and time values do not describe the same fold kernel; they
are not suitable as fold-allocation evidence without a scope label or a second counter window around
the rebuild call. The JSON currently names the generic values `cpp_new_calls` and
`cpp_new_requested_bytes`, and its coverage note does not say that they are whole-scenario totals.

Either scope a second allocation counter around the fold call, or label these as scenario-total
allocation counts/bytes and explicitly mark fold-only allocations unavailable. Keep the instrumented
allocation replay separate from the timing replay, as the implementation already does.

### P2 — VM snapshot self-check does not verify the state being copied

`runVmSnapshots()` seeds a real reference mapping and checks it once before the choice-point loop
([`performance-probe.cpp:328-331`](../../../tools/performance-probe.cpp#L328)). It then pushes
`size` choice points and counts their `cellReferences.size()` among a sum of selected fields
([`performance-probe.cpp:332-346`](../../../tools/performance-probe.cpp#L332)). Its only
postcondition is stack depth. A regression that stopped copying the target-to-source mapping into
ChoicePoints would still exit successfully and emit zero snapshot entries; there is no asserted
expected snapshot-entry count or per-snapshot reference value. The probe also sets
`activeBoundaries = choiceStack().size()` instead of independently checking mark/boundary state.

For this scenario, assert every choice snapshot contains the seeded target-to-source reference and
that its total expected copied-entry count matches the generated depth. Name the boundary metric as
a choice-point/mark count unless the arena boundary count is independently observable. This scenario
currently exercises ordinary, unscoped `pushChoicePoint()` calls; it does not exercise scoped
choice-budget refusal or unwind/backtrack behavior. Keep those as separate explicit checks if this
workload is intended to claim budget/unwind coverage. `ScopedChoiceLimits` is enforced only for
scoped choices in `VortexVM::pushChoicePointImpl()`.

### P2 — The global aligned-allocation hook is POSIX-only

`allocateAligned()` calls `posix_memalign()` directly at
[`performance-probe.cpp:42-47`](../../../tools/performance-probe.cpp#L42). This probe is part of the
normal Makefile source set and the repository has Windows packaging; `posix_memalign` is not a
portable C++ allocation API and is unavailable in some Windows toolchains. Use a supported
cross-platform aligned-allocation wrapper with the matching deallocator, or conditionally provide
the platform implementation. The current JSON note correctly admits the POSIX dependency, but that
note does not make the target build portable.

### P3 — Snapshot-entry count is a partial inventory of ChoicePoint state

The manual sum at [`performance-probe.cpp:337-341`](../../../tools/performance-probe.cpp#L337)
counts eight map-like fields, but omits `deweyPath`, `streamSlots`, `iteratorSlots`, and nested
payload elements. The VM's existing `countSnapshotEntries()` includes those fields and nested
contents (`vortex_vm.cpp:59-143`). The current seeded case has only the one cell-reference entry, so
the smoke result of 16 entries at depth 16 is meaningful for that narrow case; the field name
suggests a general snapshot-size count that it does not provide. Either constrain/document this as
`selected_snapshot_map_entries` or keep the probe's inventory aligned with the VM's defined entry
count, and add non-empty state to the workload before using it to compare broad snapshot costs.

## Other coverage gaps (not false passes)

- `fold_kernel_ns` is correctly timed around `rebuildManifold()` alone. `wall_time_ns` wraps the
  entire scenario, including generated input setup and post-fold `scanByRef()`/inspection; do not
  present it as fold-only time. The JSON note identifies the kernel and scan separately, but an
  explicit wall-time scope would make the distinction harder to miss.
- `fold-branch` constructs `size` sibling versions from the same root, then folds only the final
  sibling. The size-16 smoke run's `by_ref_entries: 3` confirms this is a sparse absolute-index
  lookup case, not a fold of all sibling branches. This can be useful for by-ref density; report the
  generated sibling count separately from folded-path work instead of overloading `work_items`
  (currently 1).
- `fold-structure` first creates `size` unlinked cells and then creates another `size` member cells
  before building the rank. The shape may be intentional, but `--size` therefore means two batches
  of cells plus genesis/dimension work; document that workload mix or simplify it so the size
  parameter has a clearer interpretation.
- The probe gives useful arity counts for 0/8/9, arena link traversal, three fold shapes, and
  choice-point snapshots. It does not provide the M0 counters for occurrence fast-path eligibility,
  mutable-payload copied elements, trail/table capacities, or root-steal/rebase/move work. Those
  remain unavailable here and must not be inferred from the six scenario totals.
- Allocation output is explicitly limited to requested C++ `new` sizes; C allocation APIs and
  allocator overhead are excluded. Keep that scope in report interpretation. The requested-byte
  counter is not peak or committed memory.

## Small validation commands

After building the target with the normal Makefile flags, run these headlessly; they cover a
zero/one-boundary case and a small representative pass without invoking the full suite:

```sh
make -j"$(nproc)" PERF_MODE=control performance-probe
for scenario in arena-links fold-text fold-structure fold-branch operands vm-snapshots; do
  SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
    XDG_DATA_HOME="$PWD/build/xdg/perf-probe/data" \
    XDG_CONFIG_HOME="$PWD/build/xdg/perf-probe/config" \
    XDG_CACHE_HOME="$PWD/build/xdg/perf-probe/cache" \
    build/performance-probe --scenario "$scenario" --size 1 --seed 7 --measure both
done
```

The existing size-16 smoke commands are recorded in the report artifact cited above. A corrected
probe should additionally assert the per-choice reference mapping and expected snapshot-entry count;
merely observing successful JSON at sizes 1 and 16 is not that assertion.

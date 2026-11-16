# M0 performance probe rereview — iteration 01, pass 2

## Verdict: one P1 self-check regression remains

The schema-2 revisions address the prior metric-scope and self-check findings: uncollected structure
counts become `null`, the per-cell ratio is named as such, scenario-total and fold-only allocation
counts are separate, VM snapshots verify the seeded reference, selected snapshot inventory is
explicitly scoped, and aligned allocation is paired by platform. The branch and structure workload
sizes are clearer, and wall-time scope is in the JSON.

The parent's control build and headless matrix in
`build/performance/iterations/probe-smoke-v2/results.json` ran all six scenarios at sizes 1 and 16
in all three modes. Thirty commands passed with valid output; the six failures are all `fold-text`
(both sizes, all modes). My own small timing pass reproduced those two failures.

### P1 — Text-fold self-check expects a cell the generated Xanadoc does not mint

[`performance-probe.cpp:291`](../../../tools/performance-probe.cpp#L291) now rejects the workload
unless `manifold.cellCount() == 1`. The workload still starts with only `store.makeXanadoc(...)` at
lines 243-245 and appends text edits; it creates no slice cell. This matches the prior smoke
evidence, where the same generated text store folded to zero cells. The schema-2 matrix confirms the
regression: all six `fold-text` commands exit 2 with
`performance-probe: text fold failed validation` (sizes 1 and 16; timing, allocations, and both).
The other 30 commands pass.

Keep this as a text-only store and validate its actual expected structure (zero manifold cells), or
intentionally add a slice/cell and update the workload description and structural expectations. Do
not keep the `== 1` assertion for the existing input.

## Prior findings status

- **False zeroes for unmeasured common fields — fixed.** `hasStructureCounts` controls whether
  structure fields are emitted; VM and operand scenarios now inspect the arena, and fold scenarios
  inspect the manifold. `checksum` is optional. The matrix confirms concrete VM link counts (29 at
  both tested sizes) instead of an unmeasured zero.
- **Cell ratio mislabeled as dense-slot density — fixed.** The key is now `by_ref_entries_per_cell`
  at lines 549-554. It describes entries per cell; it is not presented as a capacity density.
  `by_ref_per_op_range` remains the useful-entry ratio over the operation domain.
- **Fold allocation scope ambiguous — fixed.** Scenario totals are named `scenario_total_cpp_new_*`;
  separate `fold_cpp_new_*` deltas are collected around `rebuildManifold()`. The JSON says wall time
  covers the entire scenario, and its coverage note describes setup/inspection allocation scope.
- **VM reference snapshots unverified — fixed for the selected state.** Every ChoicePoint is checked
  for exactly the seeded target-to-source reference, and the selected top-level map-entry count is
  asserted equal to depth. The metric and coverage note explicitly limit its inventory to selected
  maps and exclude nested payloads, Dewey-path entries, and slot sets.
- **POSIX-only aligned allocator — fixed for Windows.** `_aligned_malloc` is paired with
  `_aligned_free`; POSIX uses `posix_memalign` with `free`.
- **Branch work hidden by a one-item count — clarified.** The output now distinguishes generated
  siblings from the folded-path operation count. It still folds only the final sibling, which is a
  valid sparse-index case and is identified by those separate fields.
- **Extra unlinked structure batch — fixed.** `fold-structure` now creates one member batch and its
  rank instead of an additional unlinked batch.
- **Invented active-boundary metric — removed.** The output no longer aliases choice-stack depth to
  an arena-boundary count.

## Remaining coverage limits

`vm-snapshots` now proves that the selected ordinary choice snapshots retain actual reference state.
It does not exercise scoped-choice budget refusal or choice unwinding/backtracking; its choices are
pushed with `std::nullopt` cursor through the ordinary, unscoped API. No budget or unwind metric is
emitted, so do not treat this scenario as evidence for those behaviors. If M0's “nested choices”
workload is intended to cover execution/unwind behavior, add a separate focused case.

The probe also does not expose by-ref initialization work, occurrence fast-path eligibility,
mutable-payload copied elements, trail/table capacities, or root-steal/rebase/move counts. Those
remain unavailable and are not implied by these scenario fields; the overall M0 observability gate
must track them separately or report them unavailable. Allocation totals cover requested C++ `new`
sizes only, not C allocation calls, allocator overhead, peak memory, or committed capacity.

## Small validation commands

The schema-2 full light matrix is already recorded in the artifact above. To reproduce the current
failure without a heavy test run, use the built control binary headlessly:

```sh
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  XDG_DATA_HOME="$PWD/build/xdg/probe-review/data" \
  XDG_CONFIG_HOME="$PWD/build/xdg/probe-review/config" \
  XDG_CACHE_HOME="$PWD/build/xdg/probe-review/cache" \
  build/performance-probe --scenario fold-text --size 1 --seed 7 --measure timing
```

After correcting the expected text-store cell count, rerun that command and the six scenarios at
sizes 1 and 16 with `--measure both`. The expected result is valid schema-2 JSON for each run;
verify that `fold-text` reports the measured zero-cell manifold rather than failing its assertion.

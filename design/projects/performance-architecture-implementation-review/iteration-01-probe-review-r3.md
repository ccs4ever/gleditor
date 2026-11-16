# M0 performance probe rereview — iteration 01, pass 3

## Verdict: ready for the six probe scenarios

The remaining P1 from pass 2 is resolved. The text scenario now independently verifies that the
generated version replays to exactly `size` `x` characters and has the Xanadoc birth plus `size`
INSERT operations, then correctly expects the text-only manifold to have zero cells
([`performance-probe.cpp:280-300`](../../../tools/performance-probe.cpp#L280)). This keeps the
intended workload and validates its actual structure.

The parent's schema-2 smoke artifact, `build/performance/iterations/probe-smoke-v3/results.json`,
contains 36 runs: all six scenarios at sizes 1 and 16 in `timing`, `allocations`, and `both` modes.
All 36 returned 0 with schema-version-2 metrics. The text-fold outputs report zero cells, zero
by-ref entries, and the expected operation ranges of 2 and 17 at sizes 1 and 16. Their text and
operation-path checks passed before the timed rebuild.

I spot-checked the phase fields: timing runs leave allocation fields null; allocation runs leave
wall time null; `both` emits scenario-total and fold-only allocation counts separately. For example,
size-16 `fold-branch` reports 84 scenario `new` calls and 21 during `rebuildManifold()`; its
generated sibling count is 16 and its folded path contains 5 operations. The wall-time scope and
selected snapshot inventory limits are explicit. Earlier corrections also remain in place:
unmeasured structure fields are null, per-cell entries are not called a capacity density, VM
snapshots assert the target-to-source state at every choice point, and Windows aligned allocation
uses its matching free function.

No remaining defect was found in the scoped probe implementation. This verdict does not mark the
overall M0 observability gate complete: the probe does not report by-ref initialization work,
occurrence fast-path eligibility, mutable-payload copied elements, trail/table capacities, or
root-steal/rebase/move counts. It also does not test scoped-choice budget refusal or backtracking
unwind. Those remain explicitly unmeasured; do not infer them from this six-scenario matrix.

## Small rerun command

The complete light matrix is already recorded in the artifact above. For a focused check after a
future probe edit, run the corrected text scenario headlessly:

```sh
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  XDG_DATA_HOME="$PWD/build/xdg/probe-review-r3/data" \
  XDG_CONFIG_HOME="$PWD/build/xdg/probe-review-r3/config" \
  XDG_CACHE_HOME="$PWD/build/xdg/probe-review-r3/cache" \
  build/performance-probe --scenario fold-text --size 16 --seed 7 --measure both
```

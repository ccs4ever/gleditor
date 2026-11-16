# M3 PGO fold regression investigation

## Evidence and scope

Read-only comparison of the exact archived P3 baseline (`09da6e1b…`, probe SHA `2d096b73…`) and C
(`e1ad7a0d…`, probe SHA `f272819c…`) PGO probes. Source was not changed and no build was run.
Primary paired results remain `build/performance/comparisons/m3-pgo-interleaved/analysis.json`; the
extra `LD_BIND_NOW` run below is diagnostic only, not acceptance data.

The archived comparison reports candidate/P3 fold-kernel ratios and 95% temporal-block intervals:

| Workload                       |              Ratio |                      95% interval |
| ------------------------------ | -----------------: | --------------------------------: |
| Text 144 / 1,000 / 10,000      | 4.78 / 2.25 / 1.28 | 3.24–5.25 / 2.15–2.29 / 1.14–1.35 |
| Branch 144 / 1,000 / 10,000    | 2.69 / 2.36 / 2.58 | 2.39–2.87 / 2.05–2.65 / 2.41–2.92 |
| Structure 144 / 1,000 / 10,000 | 1.01 / 0.89 / 0.91 | 0.94–1.20 / 0.87–0.99 / 0.78–0.97 |

The whole-scenario timings are dominated by setup for these small fold kernels. The fold-kernel
field is the relevant comparison here; no workload weights are inferred.

## Text-fold cause and safe correction

The generated text folds have `by_ref_entries=0` at all sizes, while `op_range` is 145, 1,001 and
10,001. Their count runs show only one additional C++ `new` call for C at each size and 72 fewer
requested bytes at 10,000, so extra allocation does not explain the kernel regression. The paths are
indexed Store rebuilds, not the fallback `version.path()` route.

In P3, `applyStructure` advances `foldedThrough_` at entry, then the existing non-Structure branch
resolves `contextOf(node)` through `denseOf()` and returns if the context has no cell. C adds a
local `FoldPositionGuard` whose destructor writes the max fold position on every return, an
operation-index-zero check, and the new lookup/preflight logic in the same function. The C
`applyStructure` symbol is 7,441 bytes versus 5,194 bytes in P3 (+43%);
`Store::rebuildManifoldFromIndex` is 1,667 versus 1,579 bytes (+6%). These sizes are evidence of a
much larger hot function, not a disassembly-level attribution.

C's “ignored non-Structure” behavior is not an early fast path: it still builds the context and asks
the by-ref map whether the context names a cell. With an empty by-ref map this lookup cannot
succeed. A correctness-safe targeted correction is to handle `opIndex == 0` first, then, for
`node.kind != Structure && byRef.size() == 0`, directly advance `foldedThrough_` with the existing
max rule and return success before constructing the guard or resolving context. Empty by-ref means
no Cell birth or cell-chain entry is present; Slice/Xanadoc birth records alone cannot make a text
context resolve to a Cell. This preserves fold position and refusal counts while removing the
empty-map lookup for the text workload. Add a focused test for a Xanadoc birth followed by text
operations, asserting empty cells, zero refusals and the final folded-through index. It will not fix
branch folds, where by-ref is nonempty.

The guard is serving a real invariant: lookup-capacity refusal must leave the logical position
unchanged, while semantic refusals and successful no-ops still advance it. Do not remove that
invariant or the preflight merely to shorten the common function. Special-case the semantically
proven empty-map path, then remeasure the same PGO pair.

## Branch-fold cause and safe correction

The benchmark creates `size` cells as siblings of the chosen root and folds the last sibling. The
actual selected path has only five operations and three by-ref entries at all three sizes;
`op_range` grows from 148 to 1,004 to 10,004. C is therefore exercising a sparse
high-operation-index path, not a long ancestry path.

The hybrid dense map has `minimumDenseOpCountHint = 4096`; `denseBound()` is twice the max of that
floor and the selected path count. That gives a dense cutoff of 8,192 even for this five-op path.
For 1,000 siblings, the terminal operation index is about 1,004, so the selected insertion takes the
dense branch and grows dense values/presence storage through that index for only three live entries.
The allocation pass requests 5,234 fold bytes for C versus 1,028 for P3. At 10,000 siblings the
terminal index is 10,004, above the 8,192 cutoff; the same entries route to sparse overflow and C
requests 1,158 fold bytes versus 1,028 for P3. This matches the discontinuity in the branch
allocation record and identifies an avoidable density policy cost around the fixed cutoff.

A correctness-safe candidate correction is to make dense eligibility follow the exact path count
rather than a 4,096-operation floor (while retaining checked overflow lookup for larger operation
indices). For the measured five-op branch this routes the high-index entries through the existing
sparse overflow map; for ordinary sequential paths the exact hint still permits dense storage.
Preserve R7 mapping semantics and the typed preflight contract. Add a regression test for a short
selected path with at least 1,000 preceding siblings, checking correct lookup/equivalence and
bounded dense capacity. This is a policy change in the existing representation, not evidence that
the timing problem is solved.

The branch slowdown remains about 2.58x even at 10,000 siblings, where allocation bytes are nearly
the same and the index is already routed to overflow. Thus the dense cutoff explains the
1,000-sibling memory/capacity spike, but it cannot explain the full branch timing regression.
Remaining candidates include the per-entry `contains`/prepare/commit machinery and the larger
`applyStructure` body. The count and fold-kernel reports do not isolate their instruction cost.
Removing structural preflight would weaken the required allocation-failure guarantee; if a measured
correction cannot preserve that gate, fall back to P3 rather than relaxing it.

## Walker and preflight findings

`walkAncestralWithCount` records the chosen path once, calls the hint callback once, then visits
that same buffer. It does not perform a second ancestry walk in the indexed path used by these
probes. It adds a one-time callback/metadata step, not a per-op second traversal. The fallback
`Store::rebuildManifold(version)` does count and then iterate its `version.path()` separately, but
that route is not used for these indexed benchmark versions.

`prepareRef` is not invoked for the text workload because its `byRef` is empty. When a non-Structure
operation resolves to an existing cell, adding its op to `byRef` is required by the existing R7
chain semantics; the same mapping was inserted by P3 after applying the edit. For Cell births and
Structure edits, preflight protects a real map write before logical mutation. The evidence does not
support labeling this as preflight on the empty text no-op; the missed opportunity is the earlier
empty-map return.

## Bounded lazy-binding diagnostic

Ran the archived probes only, pinned to CPU 0, with one warm-up and seven measured runs per arm for
text-144, branch-144 and branch-10,000. Arms were interleaved with randomized order. The
`LD_BIND_NOW=1` runs resolved dynamic symbols before `main`; the primary metric remained internal
`fold_kernel_ns`. All commands, JSON samples, checksums, work counts and environment values are in
`/tmp/perf-m3-bind-now/results.json`; runner: `/tmp/perf-m3-bind-now-diagnostic.py`.

| Workload      |   Candidate/P3, default loader |  Candidate/P3, `LD_BIND_NOW=1` |
| ------------- | -----------------------------: | -----------------------------: |
| Text 144      | 4.88x (P3 2.43 us, C 11.79 us) |  3.58x (P3 2.78 us, C 9.94 us) |
| Branch 144    | 2.47x (P3 5.49 us, C 14.26 us) | 2.19x (P3 5.42 us, C 12.09 us) |
| Branch 10,000 | 2.75x (P3 7.97 us, C 21.94 us) | 2.26x (P3 8.10 us, C 18.07 us) |

Prebinding reduced C's measured kernel medians by roughly 1.9 us on text-144 and 2.2–3.9 us on
branch, while P3 was roughly unchanged; a first-use dynamic binding cost contributes to C's fixed
penalty. It is not the whole cause: substantial text and branch penalties remain with prebinding,
and branch's sparse-path representation policy is independently exposed by allocation counts. This
short diagnostic is descriptive and not a replacement for the accepted paired report.

## Recommendation

First measure the correctness-preserving empty-byRef non-Structure fast path and the selected-path
dense cutoff as isolated corrections, with failure-injection/equivalence tests retained. Do not
claim either fix addresses the persistent branch kernel gap until a fresh source-bound PGO
comparison confirms it. If the branch regression remains large or the plan supplies no accepted
workload weighting that offsets it, retain the accepted P3 baseline rather than treating broad
whole-workload setup as evidence that the fold kernel is acceptable.

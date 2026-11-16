# M3 architecture and evidence review

## Verdict

No correctness blocker is apparent in the reserved-unordered implementation’s reviewed architecture.
There is a selection blocker: do not promote it as the M3 default from the current two-arm evidence.
The paired data shows a substantial tradeoff, and the third dense-byRef/unordered-birth arm is
needed to choose against the hybrid and reserved-unordered options.

## Architecture findings

- **Layering:** the lookup helpers and `Manifold`/`Store` changes live under `apps/common/xanadu`;
  no generic `src/` library dependency was introduced. That is the right layer for
  VM/manifold-specific indexes.
- **Capacity policy:** the fold path count is gathered per selected ancestral branch, then used to
  pre-reserve lookup capacity before folding. This avoids using a global maximum operation index or
  sibling history as the estimate. `rebuildManifold(version)` currently scans the path once to
  estimate and again to fold; this is extra work but keeps the same preflight behavior.
- **Failure atomicity:** `applyStructure` prepares the ref and birth insertions before logical
  cell/birth mutation. Lookup allocation refusal returns a typed refusal and the fold-position guard
  leaves the operation available for retry. `advance` preserves/restores its hint on refusal. The
  post-preflight commits are treated as invariant-preserving; a failed commit terminates rather than
  returning a partially mutated manifold. This is coherent if the prepared-map no-allocation commit
  contract remains enforced by tests.
- **Non-blocking capacity detail:** `prepareInsert` can enlarge retained bucket capacity before a
  later staging-node allocation fails. That changes allocation/capacity state, but not logical map
  contents or fold state. Document this as logical failure atomicity; no need to promise capacity
  rollback.
- **Memory-policy concern:** the path count reserves both by-ref and birth maps, though only MAKE
  operations populate births. This can over-reserve on paths with little structure. It is a tunable
  policy question, not a correctness defect; the C arm may clarify whether keeping a flat birth
  table reduces this cost.
- **Resource/token contract:** the prepared PMR node/token design depends on the resource outliving
  the maps and outstanding tokens. Tokens are operation-local in the reviewed call paths. Preserve
  that lifetime precondition in the helper contract; it is not a blocker for current use.
- **Walker hint:** `walkAncestralWithCount` supplies a branch-local count before visiting, so the
  hint does not leak across sibling branches. The hint is not itself a capacity guarantee;
  `reserveFor` is the actual capacity mechanism. Keep these concepts distinct in comments/API
  naming.

## Paired measurements

`build/performance/comparisons/m3-interleaved/analysis.json` reports 20,000 temporal-block bootstrap
draws, seven repetitions per side and fixed case, alternating order, CPU pinning, and median
`reserved-unordered / hybrid-flat` ratios. The scope is conditional to these scenarios, not an
application-wide claim.

- P3 local traversal favored reserved-unordered: ratio 0.760 (95% interval 0.674–0.864); local quote
  0.764 (0.754–0.862). Base and ExternRef quote were effectively tied (1.006 and 1.004); proxy was
  0.914 with an interval spanning 1.
- Fold costs favored hybrid-flat materially: branch ratios were 2.285, 1.879, and 3.051 at sizes
  144/1k/10k; structure ratios 1.273, 1.325, and 1.389; text ratios 2.789, 1.689, and 1.001. Ratios
  above 1 mean reserved-unordered took longer.
- In the structure-10k allocation pass, scenario-wide C++ `new` calls were 30,858 for
  reserved-unordered versus 882 for hybrid-flat. These are whole-scenario requested allocations, not
  isolated table allocations; requested bytes and RSS were similar enough that call count alone must
  not be read as memory consumption.

The source identity is represented by the variant timing reports, artifact manifests, and
`source-backup.json` snapshots; the two variants intentionally have different source fingerprints
while sharing the recorded build/toolchain identity. The analysis names a source-report SHA, but the
report itself should be retained alongside the analysis in the final evidence archive so that digest
can be independently checked. No measured data is inferred for the pending C arm.

## Selection guidance

Keep all three strategies as experimental arms until the dense-byRef/unordered-birth result is
available. The current evidence does not support one unconditional winner: local traversal favors
reserved-unordered, while branch and structure folds favor hybrid-flat. Select using the workload
weights stated for M3, and preserve scenario-level results and source/artifact identities in the
decision record.

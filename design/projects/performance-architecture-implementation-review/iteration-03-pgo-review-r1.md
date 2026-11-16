# M3 PGO selection review

## Recommendation

Do not accept the current C candidate as converged. Keep the measured M3 storage design and make one
bounded hot-path correction before another source-bound PGO run: split `Manifold::applyStructure`
into a small public dispatch wrapper and the existing lookup-heavy body, and return directly for
non-Structure operations whose context does not resolve to a cell. Preserve the existing
cell-context text handling, scratch/page-break refusals, and zero-index refusal. This is a
dispatch/code-size correction within C, not a fourth storage representation.

The correction is justified by the held-out PGO evidence and compiled code. Current C's
`applyStructure` is 0x1d11 bytes (7,441) with a 408-byte stack allocation; P3's archived function is
0x144a bytes (5,194) with a 216-byte stack allocation. This 43% function growth and larger per-call
frame are consistent with the regressions in workloads dominated by non-Structure operations,
especially short folds. The small wrapper should let those operations bypass the large body and its
stack-frame setup. Confirm the mechanism with post-change disassembly and held-out measurements; the
disassembly alone is not proof that the refactor fixes the ratios.

## Evidence and tradeoff

The PGO pair is source/profile-bound and interleaved; its confidence intervals identify meaningful
tradeoffs, not a universal aggregate. C has clear fold-structure gains at 1k (ratio 0.887, 95% CI
0.872–0.987) and 10k (0.912, CI 0.780–0.967), but is indistinguishable at 144 (1.011, CI
0.938–1.197). The stronger per-cell improvements are priced by material small-kernel regressions:
branch folds are 2.36–2.69x P3 (all CIs exclude 1), and text folds are 4.78x at 144, 2.25x at 1k,
and 1.285x at 10k (all CIs exclude 1). The arena link-local/quote-local kernels improve modestly
(0.987 and 0.956); base is inconclusive (1.116, CI 0.999–1.122), and other link cases are
inconclusive.

The memory tradeoff is substantial and real: `Manifold` grows from 296 to 544 bytes; at 1M
sequential byRef keys the recorded allocator current is 4.33 MB for C versus 24.45 MB for
pre-reserved unordered; sequential births are 64.45 MB for staged unordered versus 101.19 MB for
flat storage. Those gains justify preserving C as the candidate while repairing the avoidable
dispatch cost. They do not justify shipping the measured regressions without an application workload
weighting decision, which is absent.

## Verification gates

After the narrow refactor, retain all existing correctness and memory evidence, inspect the
wrapper/body disassembly to confirm the ignored-text path avoids the large frame, then rerun the
same interleaved held-out PGO comparisons against the already verified P3 PGO baseline using a new
source-bound candidate profile. Require the branch and text regressions to disappear or become
statistically inconclusive without losing the structure or memory gains. If they remain, the correct
decision is P3 fallback unless an explicit, evidence-backed workload weighting accepts the cost.

## Cold-PMR diagnostic and bounded C2 refinement

The text fixture contains one Xanadoc genesis Structure birth even though it has zero cells/byRef
entries, so its first-allocation path does exercise the birth map. The branch fixture folds the
genesis plus only a few MakeCell births. Archived `LD_BIND_NOW` diagnostic results show prebinding
reduces, but does not erase, candidate/P3 ratios: text-144 about 4.88x to 3.58x; branch-144 2.47x to
2.19x; branch-10k 2.75x to 2.26x. Lazy binding is a contributor, not a sufficient explanation or
fix.

I endorse one narrow preflight correction for C's birth map: reserve capacity for the prospective
final size in `entries_`, `try_emplace` the node there, then immediately extract it into the
prepared token. For these private trivial birth values there is no callback or externally observable
point between insertion and extraction. The successful prepare returns with no logical key present;
single-element insertion has the no-effects guarantee on allocation failure; extracting does not
shrink/re-hash the table; and the reserved bucket bound suffices to reinsert the node on commit
without allocating. Keep the generation check and verify temporary absence, each PMR
allocation-failure ordinal, and allocation-disabled commit. This removes the second staging map and
its bucket/node path without weakening the preflight contract.

Pair that with a thin early dispatch for irrelevant no-cell non-Structure operations and the
corrected branch-local hint cutoff. It is justified as a bounded C2 correction, not acceptance of
current C. Re-run focused branch and text measurements first; only proceed to fresh full PGO if the
fixed costs shrink and structure/memory benefits remain. If not, retain the verified P3 fallback
rather than attribute the residual to PMR binding alone.

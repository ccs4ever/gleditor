# M3 representation selection review

## Recommendation

Do not select either A (`hybrid-flat`) or B (`reserved-unordered`) as the final M3 policy from this
comparison. Run one narrowly scoped C variant: dense/presence `byRef` for operation-index lookup,
with the reserved unordered structure-birth table. This is justified as an attribution experiment,
not as an assumed winner. Retain B as the fallback until C satisfies correctness, memory, ThinLTO
and PGO gates.

The plan calls for dense lookup with overflow handling and specifically says to retain reserved
unordered maps where flat costs do not justify replacement. A hybrid separates the two roles and
directly tests that policy. Keep the existing typed preflight/commit contract: reserve both maps
before mutating logical manifold state; a staged birth insert must not publish before `byRef`
preparation succeeds. Any failed preparation must leave `foldedThrough`, slots, content, links,
mappings and semantic refusal counters unchanged, and retry must remain possible.

## What the archived A/B evidence says

The interleaved report is internally consistent for this comparison: 252 total samples (28 warm-ups,
196 measured timings and 28 counter runs), seven measured repetitions per side, alternating order,
CPU 0 pinned, shared host, matched Clang 23.1.4 / ThinLTO flags, checksums equal within every case,
and `artifacts_unchanged: true`. The bootstrap is explicitly per fixed case; it does not establish a
universal speedup or an application-wide aggregate.

- B is faster for the selected P3 timer in `arena-link-local` (unordered/flat 0.760, CI 0.674–0.864)
  and `arena-link-quote-local` (0.764, CI 0.754–0.862). These are the two eligible local scenarios
  (32,890 eligible linked calls each). The base and extern-quote cases are effectively
  tied/inconclusive; proxy is inconclusive (0.914, CI 0.722–1.048).
- A is faster on all three measured `fold-structure` sizes (B/A ratios 1.273, 1.325, 1.389;
  intervals exclude 1), all three `fold-branch` sizes (2.285, 1.879, 3.051; intervals exclude 1),
  and the 144/1000 `fold-text` cases (2.789 and 1.689). The 10,000 text case is tied (1.001, CI
  0.817–1.124). The 10,000-branch probe has only three cells, so it should not be generalized to
  large realistic branch trees.
- Timing and allocation evidence were collected in separate runs. Scenario-wide allocation counts
  include input generation and inspection, while fold-only counts are separately scoped. B often has
  many more C++ `new` calls on structure-heavy folds (476 vs 52 at size 144; 3,052 vs 70 at 1,000;
  30,068 vs 100 at 10,000), but requested C++ bytes are slightly lower for B. Thus the flat
  representation reduces allocation-call count substantially; it does not demonstrate lower
  requested bytes. Counts omit C allocation APIs and allocator overhead.
- The probe RSS is close and noisy (for example at 10,000 structure ops, 22,668 KiB A vs 23,120 KiB
  B). It is not a dedicated capacity/peak-memory measurement. Do not infer that flat storage meets
  the plan's dense-capacity or rehash-peak target from these RSS values.

The pair changes both byRef and structureBirths representation, so the fold gains and
linked-traversal gains cannot be causally assigned to just one map. C is the minimum useful
follow-up to separate them. Preserve the current seed, scenarios, count/timing separation,
checksums, matching build flags, and interleaving. Add per-map live/capacity/peak counters to
quantify dense slack and birth-table capacity; report virtual reservation, committed/page-faulted
bytes, and transient old-plus-new peak separately. The current allocation counters alone cannot
answer the plan's memory target.

## PGO and acceptance gates

The archived evidence is ThinLTO-only; it does not accept an M3 candidate for the release objective.
First compare C with the same ThinLTO experiment and require structural-equality/failure-injection
gates independently of timing. If C is selected for further study, train a fresh profile from C's
exact source/build identity and held-out corpus; do not reuse the P3 candidate's profile for changed
M3 source.

The accepted P3 baseline's archived PGO binaries/profile can serve as the baseline side of an
interleaved comparison if their complete source, compiler/target/flags/profile provenance and
binary/library digests are verified and frozen. Rebuilding or retraining that unchanged baseline is
unnecessary. Build C with its own source-bound profile, validate it, and compare per fixed workload
with confidence intervals. Include M3 structure/branch/text and local/quote/proxy link workloads
plus held-out application validation. Keep each result scoped to its scenario; report every
regression and uncertainty. Retain the existing reserved-unordered fallback if C's measured benefit
is not distinguishable from variation or its memory/capacity costs are not justified.

## Limits

This is an independent static review of archived M3 evidence and source provenance. It does not
claim C correctness, C runtime improvement, PGO benefit, or final memory suitability; those require
the isolated C implementation and its gated measurements.

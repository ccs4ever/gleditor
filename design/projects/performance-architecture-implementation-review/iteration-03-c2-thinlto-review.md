# M3 C2 paired ThinLTO selection review

## Decision

Go for one source-bound C2 PGO experiment after applying the already reviewed R3 token
move-assignment hardening and freezing the exact source. This is a bounded release-candidate
experiment, not a decision to retain C2. The ThinLTO evidence now supports testing the same hybrid
representation under the accepted release mode: the prior dominant branch/text regressions no longer
have a consistent large-kernel signature, while structure folds improve and the memory tradeoff
remains substantial.

## Evidence and remaining limits

The paired, pinned, interleaved ThinLTO report uses seven measured samples per side and 20,000
paired temporal-block bootstrap draws. At structure-fold sizes 144/1k/10k the candidate ratios are
0.874/0.948/0.928; only 1k excludes 1 (CI 0.930–0.968). Text folds are 1.164 at 144 (CI
1.080–1.493), then 0.910 at 1k (CI 0.818–0.965) and 0.853 at 10k (CI 0.757–0.964). The small-text
cold penalty remains and should stay visible in the final report. Branch folds are inconclusive at
all three sizes (ratios 1.282/0.944/1.121, all intervals include 1), as are the five arena-link
cases. There is no additive application-wide speedup claim.

The accepted memory evidence remains a meaningful reason to continue: at 1M sequential byRef keys,
C2 used about 4.33 MB current PMR bytes versus 24.45 MB for pre-reserved unordered; sequential
births used about 64.45 MB versus 101.19 MB for flat storage. `Manifold` grows from 296 to 544
bytes. These are allocator-accounted table measurements; they do not include allocator metadata or
establish whole-process RSS. The standalone UI test rerun passed after an isolated prior failure,
but it supplies no causal evidence about C2.

The optimized wrapper/helper disassembly confirms the ignored-text path no longer pays the heavy
helper's 408-byte frame: the public wrapper is 164 bytes with three pushes, while the helper is
4,053 bytes. The text-144 regression therefore remains a cold one-birth/helper cost rather than
accidental inlining of the whole implementation. R3's custom token move assignment is API hardening;
it must be included before capturing the PGO source identity.

## PGO gate

Build/train C2 against the exact frozen post-R3 source and compare against the verified P3 PGO
baseline with the same paired held-out cohorts and provenance checks. If PGO reintroduces
statistically clear sustained branch/text regressions, keep P3 fallback absent a documented workload
weighting. Otherwise retain the memory gains and report the small cold-text cost and unresolved
cases explicitly. Do not infer a universal win from the structure cohorts or collapse distinct
workload cases into a weighted total.

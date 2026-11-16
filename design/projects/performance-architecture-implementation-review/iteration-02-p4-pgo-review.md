# Independent performance review: M2 P4 ThinLTO+PGO

## Recommendation

**Use the existing out-of-line accessors for the release implementation; keep the ThinLTO+PGO
pipeline and the measurement artifacts.** The P4 candidate has demonstrated that inlining cuts
direct slot accessor call sites in the inspected probe, but the end-to-end evidence does not
establish a broad performance win: the 21-workload geomean is 1.011 candidate/baseline (95% CI
0.979–1.063), while two representative workloads regress significantly and one workload improves
significantly. Given §6.2’s explicit keep-the-out-of-line alternative when it is preferable, the
measurements do not justify taking the accessor visibility change into the next milestone as an
optimization. This is a performance recommendation, not a correctness defect in the accessor
implementation.

## Provenance and comparison validity

- The archived analysis is `complete_comparison`. Baseline is the P3 candidate source/profile and P4
  is the same P3 source plus exactly the six accessor-move files. The exact accessor transformation
  proof is true. Compiler identities match (Clang 23.1.4), machine identity matches, and each side
  has 26 verified artifact records.
- The baseline and candidate each trained a source-matched profile. Their source fingerprints are
  respectively `09da6e1b…` and `ede83f51…`, so different profile hashes are expected. Both training
  generations have the same buildflags hash and LLVM profdata tool/version. Comparison normalization
  is restricted to the exact profile-use path and the one exact profile digest; the recorded two
  `-fprofile-instr-use` tokens are identical duplicates from CXXFLAGS/LDFLAGS, and the digest
  identity is checked. `buildflags_match_after_profile_input_normalization` is true. This is not an
  unprofiled comparison or a profile carried across source revisions.
- The parent reports complete fresh PGO builds, 30 training workloads, 21 validation workloads,
  seven timing repetitions per validation workload, 168 hardware samples per side, and 161 focused
  tests passing. The archived analysis independently records 21 timing blocks, seven successful
  samples each, 20,000 independent bootstrap iterations, no run-index pairing, and matching
  artifact/dependency identities. It records all hardware counters as counted: 21 workloads × 2
  events × 7 samples on both sides. Counter scope is the entire user-space workload process
  including startup/setup; do not attribute those counters to an inner timing region.
- Corpus identity is described honestly: CLI training and validation files and generated-probe seeds
  are distinct; source-only tests have no held-out corpus claim; P3 cohorts are deterministic
  topology classes, and their seed tags only label checksums, not randomized graph inputs.

## Runtime results

- Overall geomean across 21 fixed workload blocks: **1.011 [0.979, 1.063]**, inconclusive and
  compatible with small overall improvement or regression.
- Five P3 cohorts geomean: **0.987 [0.952, 1.022]**, also inconclusive. Every individual P3 linked
  cohort interval crosses parity; the PGO data do not establish a broad linked fast-path gain or
  regression.
- `probe_fold_structure_validation` improves: **0.858 [0.699, 0.964]**. This is one clear
  workload-specific gain.
- `store_projection_validation` regresses: **1.070 [1.028, 1.096]**; `vicon_validation` regresses:
  **1.084 [1.002, 1.155]**. Both intervals exclude parity. These are separate from the P3 cohorts
  and should not be obscured by their aggregate.
- Other cohort intervals mostly cross parity: base 0.966 [0.900, 1.071], local 0.972 [0.930, 1.090],
  proxy 0.989 [0.872, 1.002], quote-ExternRef 1.003 [0.979, 1.011], quote-local 1.005 \[0.962,
  1.018\]. The P4 PGO comparison removes the ThinLTO-only quote-local regression from the
  significant set. Fold-text is 1.081 [0.705, 1.506], too noisy to decide; its selected timer
  includes generated setup and post-fold inspection, not only `rebuildManifold`.
- ThinLTO-only results remain relevant but are a separate mode: quote-local 1.164 [1.010, 1.182] and
  fold-text 1.470 [1.304, 1.557] were scenario regressions, while proxy/snapshot improved. Do not
  combine those numbers with the fresh PGO run or claim they persist under PGO.

These results support neither an additive nor universal speedup statement. The clear structure-fold
gain is real for that scenario; the two representative regressions are also clear in this validation
set. The geomean’s interval crosses one, and the P3 subset has no established aggregate change.

## Inlining and code size

The PGO artifact-bound `accessor-inspection.json` is hashed to the exact P4 candidate
`build/performance-probe`. Compared with the archived P3 PGO baseline probe, named direct
call/tail-jump references are:

- `Manifold::slot(uint32_t) const`: 55 → 5.
- `ArenaManifold::slot(uint32_t) const`: 117 → 12.
- `SegmentedOpsSpool::get(uint32_t)`: zero in both builds, so ThinLTO/PGO already removed direct
  calls there in the baseline.

This confirms a substantial static call-site reduction at those inspected probe sites. It is not a
dynamic per-call inlining ratio, nor evidence that all production call sites inline.

For linked code size, the archive records 26 paths but aliases must not be counted as separate
binaries. Deduplicating paired baseline/candidate SHA-256 identities yields 21 distinct artifact
pairs and `.text` totals of **73,620,249 → 73,648,185 bytes**, a net increase of **27,936 bytes
(~0.038%)**. The aliases collapsed include `libgleditor.so`/`.so.0`, `xudu`/`xuzz`/`zigzag`, and
`xudu_test`/`xuzz_test`. The unique xuzz binary adds 2,224 `.text` bytes; xuzz_test adds 5,936; the
two performance executables add 320 and 704. Other applications mostly gain roughly 0.4–3.3 KB each,
and the xudu swarm peer shrinks 320 bytes. PGO keeps the size cost small; it does not resolve the
two measured regressions.

## Gate disposition

P4’s source and tests appear valid, PGO is trained and compared source-by-source, and the expected
static call reduction is proven. However, the release objective is runtime performance, and PGO does
not provide an aggregate or P3-cohort speedup while it establishes regressions in store projection
and Vicon. I recommend restoring the four accessor definitions out of line and retaining the
already-out-of-line `get(MicroversionId)`; this follows the plan’s explicit fallback. Preserve the
P4 report as a measured rejected alternative. No further P4 measurement blocker is evident from the
saved analysis; the remaining decision is whether to accept a local structure-fold win despite the
broader measured tradeoff, for which this review recommends the measured fallback.

# Independent C++ performance review: M1 four-mode comparison

## Verdict

**M1 is a credible foundation for proceeding to M2, with scoped evidence and archive caveats.** The
accepted release objective remains ThinLTO+PGO. The evidence validates the pipeline and measures
effects on the current source; it does not establish a general speedup, and it cannot be carried
forward as the M2 release comparison. M2 must retrain and compare its own source revision as the
plan requires.

I found no cross-mode compiler, target, C++ standard, ABI, SDL/backend, or feature-selection
mismatch in the archived artifact manifests for control, ThinLTO, and PGO. The combined ThinLTO+PGO
validation report independently records the matching source/build identity and its binary-size
report records artifact hashes, but its standard `artifact-manifest.json` is absent. That is a small
provenance/archive consistency defect: add or regenerate that manifest before calling the four-mode
archive fully uniform. It does not invalidate the measured report because the combined mode's other
records retain the relevant identity and hashes.

## Evidence checked

- Read the M1 gate in `design/performance-architecture-implementation-plan.md`, the four-mode
  JSON/Markdown comparison, per-mode validation and binary-size reports, artifact manifests where
  present, relevant Makefile mode flags, and profile semantic-identity normalization.
- Four modes are available and each validation report succeeds on 16 workloads with seven measured
  runs per workload. Workload identities and manifest digest match across modes; the report
  correctly says the serial blocks are not paired. The report's independent-block bootstrap
  intervals and medians/ranges are consistent with the stated method.
- All four modes use Clang 23.1.4, x86_64 Linux, C++2c probe output, and matching SDL3, Vulkan, and
  other feature defines. Control and ThinLTO have the same non-mode flags. PGO adds profile-use and
  the documented cold-TU warning classification; ThinLTO adds `-flto=thin`/LLD. Repeated `-O3 -g`
  entries are redundant and harmless. Profile generation/use flag differences are normalized as
  expected; compiler/target identity remains checked separately.
- Source content identity is the same `0b39e41f…b5` across variants. The pipeline stage logs show
  generate/train/merge/use completed. Cold/unprofiled translation units are permitted by the gate
  and are recorded in the release metadata; the warning exception is scoped to the unprofiled-TU
  warning, not global `-Werror` removal.
- The combined optimized build's reported full correctness run passed 842 library, 4,380 Xuzz, 268
  ZigZag tests plus 12 and 5 network checks; focused tests passed; OpenGL, GLES and Vulkan backend
  comparisons passed with exact artifacts unchanged. These results are supplied in the task evidence
  and stage logs; I did not rerun them.

## Performance interpretation

There is no additive or universal speedup. The combined ThinLTO+PGO mode has clear held-out
improvements on some synthetic/probe scenarios: arena links ratio 0.5885 (95% interval
0.4713–0.9583), branch fold 0.8325 (0.6996–0.9381), structure fold 0.6997 (0.5938–0.7846), operands
0.5471 (0.4560–0.6733), and VQL destructure 0.9392 (0.8714–0.9988). Other mode/workload intervals
cross 1, and those should remain inconclusive.

The plain editor render workload is a material caveat: it runs `--profile` until settled, not a
fixed frame count. Runtime ratios are near 1, but whole-process user counters include startup and
subprocesses, and instruction counts can change with the amount of rendered work. The report
correctly avoids treating those counters as kernel instruction counts or making a render-work claim.
Probe internal scenario timing is distinct from those process-wide counters.

A concrete observed regression exists for PGO-only editor rendering (ratio 1.0167, interval
1.0120–1.0483), but it is not replicated by the combined mode (1.0042, 0.9968–1.0249); variable
render work and short runs limit attribution. Other isolated regressions include PGO-only VPL-file
and VProlog-file runs whose intervals sit above 1. They justify retaining per-workload uncertainty
and watching these workloads after M2; they do not show a broad release regression.

Binary text sizes are recorded per artifact and aliases are not double-counted. The combined mode
generally reduces `.text` for the large xuzz/test/probe artifacts versus control, though sizes alone
do not establish runtime benefit. No cold build-cost conclusion is supportable: ccache warmth
differs and stage measurements do not isolate comparable clean builds.

The warm-object xuzz relink evidence is tightly scoped. It has one record for ThinLTO and one for
PGO-only, no object-compile records, and the supplied evidence says object and binary/stamp hashes
stayed unchanged. The large elapsed/RSS differences between those records are consistent with
different amounts of linker work/cache state; these records do not establish a mode-to-mode
link-speed win. They are not ThinLTO+PGO evidence and should not be presented as such.

## Corrections and remaining limits

1. Produce the missing `build/performance/variants/m1-thinlto-pgo/artifact-manifest.json` from the
   existing combined-mode identity/artifact evidence, or document why that archive intentionally
   uses a different schema. Keep the shared comparison's “all four archives” claim aligned with the
   on-disk files.
1. Keep the report's current scoped interpretation: no universal speedup, no clean build-time claim,
   and no fixed-work rendering conclusion. Preserve the observed PGO-only render and CLI regressions
   as regressions with their uncertainty.
1. Retrain PGO on the exact M2 source before any M2 release comparison; no M1 profile or timing
   claim should be carried across a source revision.

No further M1 performance or correctness blocker is evident from the inspected records. The missing
combined artifact manifest is an archive completeness fix, not an architectural blocker to beginning
M2; it should be closed as part of M1's documentation/evidence cleanup.

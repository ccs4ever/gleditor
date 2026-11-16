# M1 four-mode review, follow-up

## Result

**The archive correction closes the remaining M1-scoped evidence gap. I found no remaining M1
blocker to proceeding with M2.** The combined ThinLTO+PGO manifest is now present at
`build/performance/variants/m1-thinlto-pgo/artifact-manifest.json`, and the regenerated comparison
report is complete with all four modes, no missing modes, equal input/scenario identity, successful
validation reports, and seven measured runs per workload.

## Correction verified

- The new manifest has 26 artifact entries. Its path set and every SHA-256 match both
  `release-artifacts.json` and `binary-sizes.json` exactly.
- Manifest `source` matches the retained combined release-artifacts source identity (`0b39e41f…b5`);
  its build flags match `validation-comparison-report.json`'s recorded source/build identity. The
  archived release record also matches the manifest's artifact hashes.
- `archive_origin` records hashes for the source release-artifact and validation-comparison records
  and explicitly says it was reconstructed from archived evidence, not from current other-mode
  binaries. This is the right provenance distinction for a post hoc archive repair.
- The combined mode still has its own saved binary-size evidence, so no artifact or text-size claims
  need be reconstructed from a different mode.

## Remaining interpretation

This closes the manifest completeness issue only. Preserve the original review's evidence
boundaries: the results support per-workload observations, not a universal speedup; no cold
build-cost conclusion; editor rendering is variable work because `--profile` runs until settled;
process-wide hardware counters are not probe-kernel counters. Retain the observed PGO-only
editor-render regression (ratio 1.0167, interval 1.0120–1.0483) and PGO-only VPL-file/VProlog-file
regressions as uncertainty-qualified results. The combined-mode render interval crosses 1, so it is
inconclusive.

The accepted release target remains ThinLTO+PGO. Retrain profiles on the exact M2 source revision
and repeat the relevant comparisons there; M1 timing/profile results do not transfer across source
changes. No new build or workload was run for this follow-up.

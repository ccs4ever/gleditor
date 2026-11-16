# Independent review: fresh P3 ThinLTO+PGO comparison

Read-only audit of `m2-p3-pgo-analysis.json`, both raw PGO variant directories, profile
manifests/reports, binary sizes, hardware records, and the M2 P3 plan gate. No source changes,
builds, or workload runs were performed.

## Verdict

**The P3 measurement gate has converged; proceed to P4 as a separate experiment.** The fresh-profile
pair is provenance-valid, has complete paired validation and hardware samples, and shows the
expected reductions on constructed eligible cohorts without an established validation-workload
regression. Keep the result scoped: these five constructed classes do not estimate real application
eligibility or prevalence, and the profile-use pair measures P3 source plus its matching newly
trained profile as a release configuration.

The parent’s planned final restoration check is still appropriate: restore the committed P3 source,
validate the archived candidate profile manifest against its exact source/compiler/hash, rebuild the
targeted PGO artifacts, verify their archived hashes, and run the focused PGO candidate tests. That
is an artifact reproducibility/correctness check, not a reason to rerun primary timing.

## Provenance and completeness

- Baseline artifact source fingerprint `9e170b…` is the old `arena_manifold.cpp` content; the
  recorded override hash equals the file at baseline parent revision `387eff44…`. Candidate
  fingerprint `09da6e…` is clean at commit `0c029b…`; no dirty files are recorded. Both timing
  reports were run at the same head revision.
- Both use Clang 23.1.4 and match build flags after only the exact profile-use path/flag and
  recorded profile digest are normalized. Profile generation flags, compiler profile tool
  path/hash/version, workload manifest, and validation commands match.
- Training reports each contain 30/30 successful train workloads and 279 profile-output records. The
  training reports, validation reports, and archived `release.profdata` files independently hash to
  the analysis provenance values. Baseline/candidate profiles are distinct (`975388…` / `d7e367…`)
  and each profile manifest names its own source fingerprint.
- Both validation reports use the same manifest hash and have 21 workloads, seven successful
  measured runs each, and a separate warmup. The hardware reports each contain 168 samples (21 × 8),
  no failures, unchanged artifact/source identity, and all 336 event rows counted at 100% runtime.
  Hardware counters cover whole user-space processes and inherited subprocesses, including
  startup/setup; they do not attribute counts to the P3 timed loop.
- Artifact identity evidence verifies the 26 release artifacts per variant and recorded command
  executables/size records. The candidate source/probe binary and profile hashes are archived; the
  baseline and candidate artifacts are not reconstructed from the current checkout.

## P3 measurements and uncertainty

P3 timing uses the declared isolated `p3_link_traversal_ns` metric; every generated cohort performs
32,890 actual forward/reverse lookups with result checks and a checksum. A separate counter run
reports 32,890 calls per class, and exactly 32,890 eligible calls for the arena-owned non-ExternRef
local and quote-local classes. Those counts describe the constructed cohorts only.

| Constructed cohort                 | Candidate / baseline median | 95% bootstrap interval | Result                                                           |
| ---------------------------------- | --------------------------: | ---------------------: | ---------------------------------------------------------------- |
| Eligible arena-local non-ExternRef |                       0.836 |            0.767–0.866 | Lower measured traversal time                                    |
| Quote-local non-ExternRef          |                       0.748 |            0.745–0.779 | Lower measured traversal time                                    |
| Canonical proxy                    |                       0.826 |            0.806–0.939 | Lower measured traversal time; this path is not counted eligible |
| Persistent base fallback           |                       1.017 |            0.959–1.046 | Inconclusive, near unchanged                                     |
| Quote ExternRef                    |                       0.984 |            0.411–1.027 | Inconclusive; broad interval                                     |

Checksums and actual call/eligibility counts match across variants. The P3 five-class equal-weight
geometric mean is 0.877 (95% interval 0.731–0.906). Treat that as a summary of this fixed scenario
set, not a global or additive speedup.

Across all 21 validation blocks, the equal-weight geometric-mean ratio is 0.925 (95% interval
0.871–0.948). No individual block has a 95% interval wholly above 1. Several non-P3 blocks are
inconclusive; preserve them as such. The P3 proxy result is a measured effect of the P3
build/profile pair but does not imply proxy eligibility or a universal proxy improvement. Editor
rendering remains effectively unchanged (ratio 1.001, interval 0.985–1.011).

The PGO comparison is stronger release evidence than the ThinLTO-only run, while the ThinLTO-only
comparison remains useful to separate P3 source behavior from profile-use mode. Do not combine the
two into one estimate. PGO training was correctly repeated for each source fingerprint; the result
therefore includes both the P3 source and its corresponding retrained profile, as a release build
would.

## Size and remaining scope

Text-size records define text as ELF `.text`/`.text.*`, excluding debug and read-only data. Main
`xuzz` text increases from 7,350,481 to 7,405,505 bytes (+55,024, about 0.75%); `xuzz_test` changes
by the same +55,024 bytes. `libgleditor.so` text falls by 224 bytes. This is a modest but real
code-size cost to retain in the P3 record and monitor in P4. Probe binaries also grow by about 55 KB
of text; they include the P3 probe work and should not be treated as product size.

## Decision and next work

P3 can be recorded as accepted for M2: semantics and focused ThinLTO tests were reviewed previously,
this fresh ThinLTO+PGO comparison is complete, and no concrete correctness, provenance, or
measured-regression blocker remains. Complete the parent’s exact archived-profile restoration/test
check, then start P4 with a separate accessor-only source delta. P4 baseline and candidate need
their own matched ThinLTO and ThinLTO+PGO comparisons and fresh profiles for each changed source; do
not carry P3 profiles across the P4 source revision. Keep text size, uncertainty, and the
scenario-only eligibility scope beside all timing claims.

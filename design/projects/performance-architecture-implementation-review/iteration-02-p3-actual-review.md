# Independent review: M2 P3 candidate and four-way measurement evidence

Static source/evidence review only. I inspected the P3 `ArenaManifold::linked` diff and regression,
saved baseline/candidate performance and hardware reports, full analysis JSON, artifact manifests,
and plan §6.1–6.2. I did not compile or rerun workloads.

## Verdict

**No semantic blocker found for the P3 change.** Under identical ThinLTO flags, the paired artifacts
and measurements support committing this narrowly scoped change with a ThinLTO-only,
workload-specific interpretation. The accepted release target remains ThinLTO+PGO; fresh PGO
evidence for this source is still required before describing P3 as validated for that release
configuration. Parent reports that this follow-up run is underway.

## Semantic review

The candidate preserves the no-dense/base fallback. Its new decision is equivalent to the former
slow classification on dense local slots: only an ephemeral, non-quote `ExternRef` enters
shadow/foreign delegation. `isProxy()` itself is exactly the conjunction of ephemeral, not quote,
valid dense slot, and `ExternRef`; persistent-shadow `ExternRef` cells remain on the local scan.
Quotes remain local even after their mutable value changes to `ExternRef`. The shared local-edge
scan is unchanged.

The added regression checks both directions for an initially unlinked quote occurrence, local
presentation edges after mutation to `ExternRef`, local behavior through `Int64`/`ExternRef`
changes, release of temporary links/values, canonical proxy delegation restoration, and persistent
base-cell local edges. The saved focused run reports 158/158 tests passed; the separate final unit
log should be retained with the eventual change record.

## Identity and measurement integrity

- Baseline and candidate are the same source revision with only the P3 implementation/test delta;
  the only other source fingerprint delta is `tools/performance-probe.cpp`, for which the analysis
  records a byte-for-byte archived-source comparison proving the candidate is exactly the
  clang-format-19 output. Instrumentation/workload/report tooling is common to both.
- Both builds use Clang 23.1.4, ThinLTO/LLD, and matching build flags. Each has a release-artifact
  manifest for 26 artifacts; analysis verifies every recorded command executable and size record.
  Candidate binary hashes differ as expected.
- Both validation timing reports have 21 workloads × 7 successful measured runs, with shared
  manifest hash and pairing scheme. P3 primary time selects `p3_link_traversal_ns`: only
  forward/reverse `linked()` calls, result checks, checksum; excludes graph setup, validation, and
  inspection. Each primary P3 run exercised 32,890 calls (above 32,768 target). A separate
  allocation/count command reports actual constructed-cohort calls and eligibility. Those counters
  are explicitly scenario-only, not a global frequency estimate.
- Hardware reports have 168/168 samples counted in each variant; both `cycles:u` and
  `instructions:u` rows are counted with 100% runtime and no reported collection failures. These
  counters cover the whole user-space process and inherited children, including setup/startup. They
  are diagnostic and do not attribute work to the timed inner loop.
- No runtime-speedup conclusion should be drawn from the global equal-weight aggregate. The analysis
  reports 0.873 candidate/baseline for the full 21-workload geometric mean (95% bootstrap interval
  0.850–0.962) and 0.628 for the equal-weight five-cohort P3 geometric mean (0.604–0.882). The
  latter is a constructed-cohort summary, not an expected application mix. Broader non-P3 workload
  ratios mostly have intervals crossing 1; keep their observed ups and downs visible rather than
  calling a universal gain.

## Scoped P3 results and uncertainty

The five constructed cohorts show the intended boundary clearly:

| Cohort                     | Candidate / baseline median | 95% bootstrap interval | Interpretation                                                           |
| -------------------------- | --------------------------: | ---------------------: | ------------------------------------------------------------------------ |
| Eligible arena-owned local |                       0.352 |            0.292–0.557 | Lower measured traversal time                                            |
| Quote-local, non-ExternRef |                       0.656 |            0.623–0.708 | Lower measured traversal time                                            |
| Base-manifold fallback     |                       1.006 |            0.970–1.772 | No resolved change; very uncertain interval                              |
| Quote ExternRef            |                       1.000 |            0.978–1.036 | No measurable change                                                     |
| Canonical proxy slow class |                       0.420 |            0.409–1.131 | Median lower, but interval is inconclusive due to high baseline variance |

Checksums/counts match between variants. This supports the optimization for eligible local paths and
the claim that explicitly excluded paths retain their behavior. It does not support a general
ArenaManifold speedup or a real-world eligible-call fraction. The unchanged legacy `arena-links`
scenario is also inconclusive (ratio 0.901; interval 0.575–1.115).

The analysis also records apparent candidate increases in other probe/CLI workloads (for example
fold-structure median ratio 1.204, interval 0.894–1.439; VProlog file ratio 1.057, interval
0.967–1.136). Those are uncertain and not attributable to P3; report them as observed uncertainty,
not as established regressions. Editor render is effectively unchanged (1.004, interval
0.986–1.017).

## Required next gates

1. Complete the fresh P3 ThinLTO+PGO run and compare it with a baseline freshly profiled for the
   same source/tooling and workload recipe. Do not reuse the M1 profile across P3 source changes.
   Keep ThinLTO-only and ThinLTO+PGO comparisons separate.
1. For P4, build paired baseline/accessor variants from identical source except the accessor change,
   and train fresh profiles for each source variant. This isolates P4 from P3 and tests the accepted
   ThinLTO+PGO release configuration. Report text size and runtime/build evidence per plan; inspect
   actual inlining.
1. Preserve the full 21-workload results and counter scope in the final decision artifact. No
   additive predicted gains, no global eligibility extrapolation, and no clean-build claim.

**Commit guidance:** the P3 code/test delta is semantically ready on current static evidence, and
its ThinLTO results are scoped and reproducible enough to record. Treat P3 validation against the
supported ThinLTO+PGO release mode as pending until the fresh-profile comparison finishes; do not
close the M2 release gate before it does.

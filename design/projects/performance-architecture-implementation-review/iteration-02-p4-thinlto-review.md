# Independent interim review: M2 P4 ThinLTO

## Verdict

The saved P4 ThinLTO evidence is identity-consistent and adequate as an interim comparison, but it
does not support accepting the accessor move on performance grounds. Results are mixed, including
two scenario-level regressions with confidence intervals excluding 1.0. Keep the P4 decision open
pending the fresh ThinLTO+PGO comparison already in progress. This is not a final P4 gate verdict.

## Evidence and scope

- `m2-p4-thinlto-analysis.json` reports a complete 21-workload comparison, seven successful timing
  samples per workload, 20,000 bootstrap iterations, matching compiler/buildflags, and verified
  executable/artifact identities (26 artifacts per side). The source delta is the six-file accessor
  move only; the analysis records an exact transformation proof. The baseline is the P3 candidate
  source and the candidate is the committed P3 source plus P4 accessor visibility.
- Hardware report metadata labels counters as whole-process user-space counters, including setup and
  inherited subprocesses, and explicitly says they are not attributed to the timed P3 traversal. The
  reported workload medians have seven counted samples for both cycles and instructions; do not use
  them as traversal-specific evidence.
- P3 eligibility/count records are explicitly `scenario_only`. Local and quote-local report 32,890
  linked calls and 32,890 eligible calls; base, proxy and ExternRef cohorts report zero eligible
  calls. Construction/inspection allocation counters are separate from primary timing. All cohort
  checksums match across the paired builds.
- The P4 analysis reports a 21-workload geomean ratio of 1.018 (95% CI 0.969–1.053), inconclusive.
  Paired results include quote-local 1.164 (1.010–1.182) and text-fold 1.470 (1.304–1.557), both
  regressions; proxy 0.834 (0.830–0.988) and snapshots 0.669 (0.585–0.777), improvements. Operands
  are 0.970 (0.757–1.000), borderline. Base/local and several other workloads have broad intervals.
  This is not an additive or universal speedup.
- The archive records 26 artifact paths, but some are aliases to the same physical binary and must
  not be summed as independent outputs. Deduplicating the baseline/candidate path records by their
  paired SHA-256 identities yields 21 distinct artifact pairs and `.text` totals of 94,735,706 bytes
  at baseline versus 95,266,442 for P4: +530,736 bytes (~0.56%). This deduplication collapses
  `libgleditor.so` with `.so.0`, `store-dump` with `xudu-dump`, `xudu` and `zigzag` with `xuzz`, and
  `xudu_test` with `xuzz_test`; the current compatibility links confirm those alias relationships.
  The actual unique `xuzz` binary adds 48,016 `.text` bytes, `xuzz_test` adds 90,704 bytes, and the
  two performance binaries add 28,416 and 28,512 bytes. `gleditor` is unchanged. These linked-code
  changes matter for the inline tradeoff, but are not evidence by themselves of a regression
  mechanism. I exclude aggregate whole-file bytes because alias paths would inflate that total too.

## Timer interpretation

The text-fold sample explicitly selects `probe_metrics.wall_time_ns`, whose scope is “whole scenario
including generated input and post-fold inspection.” The same probe also emits `fold_kernel_ns`
(only `rebuildManifold`) and `by_ref_index_scan_ns` (a separate public-API inspection timer), but
neither is the selected primary metric. Therefore the 1.470 text-fold result is a statistically
clear regression for that complete scenario, not a clean measurement of only the fold loop. It
includes setup and inspection whose costs may swamp the kernel; the P4 inline changes can affect
both fold and slot inspection. The reported P3 link metric is different: `p3_link_traversal_ns`
times forward/reverse `ArenaManifold::linked` calls with result checks and checksum, excluding
cohort construction, validation, and structure inspection. Do not compare these absolute durations
or imply that the text-fold result is the P3 inner-loop result.

As a diagnostic only, seven saved P4 candidate samples show a median `fold_kernel_ns` of 4,780 ns,
while the candidate’s selected scenario median is about 3.08 ms. That inner value was not analyzed
as a paired primary metric here, so it must not replace the reported scenario result or be promoted
to an acceptance claim. If fold-kernel attribution is needed for the P4 decision, analyze that
metric separately with paired baseline/candidate samples and an explicit scope.

## Remaining gate

Await the source-matched fresh ThinLTO+PGO baseline/candidate analysis. Review its source/profile
identity, all-workload success and timing scopes, artifact hashes, and text sizes before deciding
P4. If the PGO comparison does not establish an acceptable tradeoff for the stated release
objective, retain the out-of-line accessors or document the scoped reason for keeping them; do not
use the proxy/snapshot wins to erase the quote-local/text-fold regressions.

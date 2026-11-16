# M2 convergence review

**M2 is complete with P3 retained and P4 restored to its out-of-line fallback.** This follows the
accepted plan’s §6.2 instruction to keep the out-of-line definitions when measured results favor
that alternative. The ThinLTO+PGO pipeline remains in place.

P3’s guarded `ArenaManifold::linked` optimization is retained based on its accepted, source-matched
ThinLTO+PGO evidence. P4 was a separate six-accessor visibility experiment; it achieved the expected
static call-site reductions, but the fresh PGO comparison showed no clear aggregate benefit
(21-workload geomean 1.011, 95% CI 0.979–1.063) and established regressions in store projection
(1.070, CI 1.028–1.096) and Vicon (1.084, CI 1.002–1.155). Structure folding improved (0.858, CI
0.699–0.964), while all five P3 cohorts were inconclusive as a group. ThinLTO-only results were also
mixed. See `/tmp/perf-m2-p4-thinlto-review.md` and `/tmp/perf-m2-p4-pgo-review.md` for scopes,
confidence intervals, provenance, and deduplicated code sizes.

Restoration verification is complete: source is clean at HEAD `7ba0513e`; its source fingerprint
matches the accepted P3 baseline (`09da6e1b…`). The restored `build/xuzz_test`,
`build/performance-probe`, and `build/performance-probe-counts` hashes match their accepted P3
archived artifacts exactly. All 161 focused PGO tests passed. Reports:
`build/performance/iterations/m2-p4-fallback-restored-artifacts.json` and
`build/performance/iterations/m2-final-focused-tests.json`.

No M2 acceptance blocker remains. M3 may proceed from the accepted P3 source state; do not carry P4
accessor inlining into that baseline.

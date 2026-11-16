# Independent M1 release coverage/provenance re-review

Reviewed the current `Makefile`, `tools/release_profile.py`, `tests/performance/workloads.json`,
`tests/performance/test_release_profile.py`, `docs/testing.md`, the prior review at
`/tmp/perf-m1-release-coverage-independent-review.md`, and the existing smoke log. I made no
repository edits and ran no builds or focused test suite while R5 generation was active.

## Disposition

The previously identified source-level gaps appear corrected. I found no remaining source blocker in
the requested M1 scope. **Ready for runtime/performance gates only; the full profile-use release and
held-out validation remain unrun.** R5 generation is the first actual Make-graph confirmation of the
new `shader_assemble` goal alias; the earlier unit assertion only checked inventory membership and
would not have caught the missing public target.

## Verified corrections

- `tools/release_profile.py` now pins `llvm-profdata` by absolute invocation path, `--version`
  output, and executable SHA-256. It records that identity during generation, verifies it before
  train/merge/release, and invokes that recorded path for merges. The tests cover replacement and
  changed tool identity at each later stage.
- `TARGETS` explicitly asks Make for `all`, `shader_assemble`, `xudu-swarm-peer`, and both probes.
  `BUILD_ARTIFACTS` includes each corresponding executable, the programs and compatibility links
  from `all`, test executables, and every `libgleditor.*` path found in `build/`.
  `build_artifacts()` fails on missing expected files. This covers the stated release target
  inventory, including the helper and test peer outside `all`.
- The Makefile now defines a phony public `shader_assemble` goal for `$(OBJDIR)/shader_assemble`.
  The helper’s link recipe consumes `$(CXXFLAGS)` and `$(LDFLAGS)` and depends on `$(FLAGSTAMP)`, so
  the explicit target participates in the active PGO/LTO mode and flag invalidation.
- `swarm_peer_entrypoint_train` exercises `build/xudu-swarm-peer --help`; current source returns
  success for this path. The old pre-R5 binary I smoke-ran printed usage but returned 2, so that
  binary is stale relative to the current source and is not valid confirmation. The newly rebuilt R5
  binary should be included in the next entrypoint smoke before this workload is considered
  runtime-verified.
- The warning policy remains narrow: only profile-use adds `-Wno-error=profile-instr-unprofiled`,
  and semantic identity normalizes exactly that flag only in PGO-use modes. The parser records
  actual cold-TU warning paths from the captured build output; the release report includes the list,
  build-log path and digest, and an explicit statement that out-of-date profile diagnostics remain
  fatal. Documentation distinguishes entrypoint coverage from representative workload evidence and
  says cold-TU warnings do not count as coverage.

## Evidence limits

The prior report documents an earlier 32-test focused run; parent reports a newer 41-test run, but I
did not independently rerun it. My peer smoke was against the existing stale binary and failed with
status 2; no source conclusion follows from that run. The active R5 generation has not yet
established successful end-to-end target resolution, and no training, merge, profile-use build,
held-out validation, or performance comparison is claimed here.

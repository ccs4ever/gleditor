# Independent M1 release coverage/provenance audit

Reviewed the frozen source's Make profile-use flags, `tests/performance/workloads.json`,
`tools/release_profile.py`, `/tmp/perf-m1-release-mismatch-review.md`, and
`/tmp/perf-m1-focused-tests.log`. No source edits, builds, profile stages, or workload runs were
made. The source was treated as frozen during generation.

## Findings

1. **Conditional build blocker: `shader_assemble` is required unconditionally but is not built
   unconditionally.** The new workload invokes `build/shader_assemble`, and `BUILD_ARTIFACTS`
   requires it. `release_profile.py` builds `all`, the two performance probes, but not the
   `shader_assemble` goal. In Make, `$(OBJDIR)/shader_assemble` is only a prerequisite of Vulkan
   SPIR-V generation; `all: shaders` is itself conditional on `GLEDITOR_ENABLE_VULKAN`. Therefore a
   valid no-Vulkan build (including `GLEDITOR_DISABLE_VULKAN=1`) reaches `build_artifacts()` without
   this binary and fails before training. Fix by adding `shader_assemble` explicitly to the release
   targets (and confirming its dependencies work in no-Vulkan mode), or conditionally excluding the
   workload and inventory entry when the target is intentionally unavailable. This is separate from
   the pending warning-flag normalization.

1. **Profile-tool provenance can drift between stages.** The generation probe checks that
   `llvm-profdata` is on PATH, runs a tiny merge, and stores only its resolved path in
   `toolchain.profile_tool`. The actual merge stage resolves `llvm-profdata` from PATH again and
   does not compare its version or file identity to the generation record. A tool replacement/PATH
   change between generation and merge can therefore consume the collected raw profiles with a
   different LLVM profile tool while compiler/source checks still pass. §5's
   matching-profile-tool/provenance gate is stronger than presence-at-generation. Store the profile
   tool version (and preferably resolved executable identity) in the generation manifest and refuse
   to merge/use if it changes.

1. **Pending known blocker remains: semantic identity differs solely by the Clang cold-TU warning
   policy.** As the supplied mismatch review says, `semantic_build_identity()` strips profile
   generation/use paths and LTO flags but not `-Wno-error=profile-instr-unprofiled`, which is added
   only in PGO-use `CXXFLAGS`. It rejects the intended generate→use transition. The focused test log
   shows 32 tests passed but does not establish that this identity comparison is fixed. Keep this
   marked fix-pending until the worker patch normalizes only this use-only warning flag and tests
   generation/use semantic equivalence while preserving other warning/build-flag mismatches.

## Coverage/provenance observations

- The added training entries cover the distinct main functions omitted by the previous corpus: xuzz,
  VQL/VQL compiler, VPL compiler, Vlace/Vlace compiler, Vicon/Vicon compiler, Vagent, store-dump,
  shader assembler, and `gleditor_test`. `vpl` and `vprolog` have real file/eval invocations;
  `gleditor` has a render workload. Existing `xuzz_test` and `zigzag_test` filtered suites cover
  those test executable mains. `xudu`, `zigzag`, `xudu-dump`, and `xudu_test` are compatibility
  aliases of already-covered binaries, so they do not need distinct CFGs; their paths are explicitly
  present in `BUILD_ARTIFACTS`.
- Artifact coverage includes the `all` binaries/aliases and shared library, plus `performance-probe`
  and `performance-probe-counts`. It does not include `xudu-swarm-peer`, but that target is not a
  prerequisite of `all` or the release driver's target set, so this is not an omission from the
  stated all-output inventory.
- The report labels help/startup workloads as entrypoint coverage, not representative performance
  evidence. The manifest SHA, command argv, input/dependency hashes, training repetitions/weights,
  raw profile records, and output artifact digests are checked across stages. The warning policy
  remains narrow: `profile-instr-unprofiled` becomes a visible nonfatal warning in Clang PGO-use
  CXXFLAGS; out-of-date profile diagnostics remain errors. No evidence supports suppressing profile
  mismatch diagnostics, and none should be added.

## Evidence status

The prior mismatch review reports the entrypoint smoke commands succeeded and the focused Python
suite passed 32 tests, including the Clang regression for distinct `main` CFG hashes, missing-hash
failure, and visible cold-TU warning. This independent audit did not rerun them. No fresh
generation/train/merge/profile-use release or full-release validation was performed. The source
freeze and generation-in-progress status therefore remain: **fix pending; full release unrun**.

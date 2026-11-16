# Performance architecture implementation progress

Execution source:
[accepted implementation plan](../performance-architecture-implementation-plan.md). All P1-P5 and
release ThinLTO+PGO are approved. Work uses Luna implementation workers and independent Luna
reviewers; acceptance is based on current code and recorded checks.

## Milestones

| Milestone                 | Status      | Evidence / remaining work                                                                                                                                                                                                                                                                                                                                                                      |
| ------------------------- | ----------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| M0 baseline/observability | In progress | Ordinary `-O3 -g` full build passed. Complete control `make test` passed, including 842 library, 4380 Xuzz, 268 ZigZag, 12 additional tests and 5 network tests. Independent M0/probe reviews converged for the frozen corpus recipe. All 36 split probe cases and both corpus smoke reports passed; full control backend comparison passed. Fine-grained telemetry listed below remains open. |
| M1 release ThinLTO+PGO    | Complete    | Four Clang modes built and passed16 held-out timing blocks each;512 separate perf samples usable. Combined full tests/backends passed;41 focused tests and formatting passed. Independent C++ performance review converged; scoped results and pre-existing lint failure recorded below.                                                                                                       |
| M2 P3/P4                  | Pending     | Depends on baseline and release pipeline evidence                                                                                                                                                                                                                                                                                                                                              |
| M3 P2                     | Pending     | No runtime lookup changes yet                                                                                                                                                                                                                                                                                                                                                                  |
| M4 P5                     | Pending     | No operand container changes yet                                                                                                                                                                                                                                                                                                                                                               |
| M5a-c P1                  | Pending     | No VM trail/container changes yet                                                                                                                                                                                                                                                                                                                                                              |
| M6 final release          | Pending     | Requires all preceding correctness and performance gates                                                                                                                                                                                                                                                                                                                                       |

## Iteration 1: baseline and release infrastructure

The starting tree was commit `82edc2d0` plus the accepted proposal/plan/review documentation from
this conversation. No runtime source changes were present. Submodules were initialized. The build
stamp showed ordinary `-O3 -g`, SDL3 and Vulkan enabled. Installed Clang, LLD and llvm-profdata
reported version 23.1.4.

- `make -j$(nproc) all`: passed. Log: `build/performance/iterations/m0-control-build.log`.
- `make -j$(nproc) test`: passed with exit status 0, including rootless network checks. Log:
  `build/performance/iterations/m0-control-tests.log`.
- Worker ownership: M0 owns corpus/report runner and its focused tests. M1 owns Makefile,
  release-profile driver, documentation and its focused tests. Parent serializes heavy builds.

Build/profile artifacts remain under ignored `build/performance/`; committed evidence summaries will
identify commands, source/configuration, failures and limitations. A script's presence or unit test
pass is not proof that representative PGO training or a final optimized release succeeded.

### Infrastructure checks recorded so far

- Standalone `make -j$(nproc) PERF_MODE=control performance-probe`: passed. This target did not
  replace the application/shared-library binaries being exercised by the baseline network tests.
- All six probe scenarios at size 16/seed 7 in timing, allocations and both modes: 18/18 passed,
  with valid JSON and workload self-checks. Raw results:
  `build/performance/iterations/probe-smoke/results.json`. These are smoke/structural checks, not
  performance comparisons. Text-fold mapping count was 0; structure-fold 51; sparse branch-fold 3.
- Parent-run M1 focused Python tests: 7/7 passed. Independent review still found stale declared
  input validation and main-stage integration-test gaps; a green helper suite is not release proof.
- M0 independent reviews 1 and 2 identified and drove corrections in weights, executed-test counts,
  RSS units, isolation, run identity and malformed-result/provenance handling. Reports are under
  `design/projects/performance-architecture-implementation-review/`; remaining issues are open until
  reviewer verification, with corpus/backend coverage explicitly incomplete.

No optimization milestone M2-M6 is complete. No release speedup or fully trained project release has
been demonstrated. The active objective remains implementation of the complete accepted plan.

### Completed control run

The full control test target exited 0. Main summaries were 842 library tests, 4380 Xuzz tests and
268 ZigZag tests; an additional 12-test group passed, and the rootless publication/checkpoint group
passed all 5 tests. The network group took about 914 seconds, including deliberate UI/network waits.
Its quiet intervals were verified against the live process and test code rather than treated as
terminal failures. The test log remains `build/performance/iterations/m0-control-tests.log`.

Parent independently exercised the real tiny compiler-profile workflows: Clang ThinLTO+PGO, GCC
LTO+PGO and GCC PGO-only all passed. Results and log are
`build/performance/iterations/m1-toolchain-probes.{json,log}`. This establishes tool availability,
not that the entire project has been trained or validated in those modes.

### Resumed infrastructure verification

Accepted design and plan/review documentation was committed as `73152de9`. Infrastructure code
remains a separate uncommitted iteration while its real pipeline gates run.

- Parent-run focused Python suite: 31/31 passed, recorded in
  `build/performance/iterations/iteration01-final-unit-tests.log`.
- Full repository `make -j$(nproc) format-check`: passed before this journal update. New review
  documents also receive targeted Markdown formatting/lint checks.
- Full headless control backend comparison passed OpenGL, GLES and Vulkan, including E2E scenarios.
  Log: `build/performance/iterations/control-backends.log`.
- Frozen corpus smoke reports passed 12/12 training and 16/16 validation workloads, each with six
  separately collected counter runs: `split-m0-train.json` and `split-m0-validation.json` under
  `build/performance/iterations/`. Source-only gtest workloads make no held-out-data claim.
- Timing and allocation probes are separate executables. Symbol inspection confirmed no replacement
  global allocation providers in the timing executable. All 36 scenario/mode/size checks passed and
  three incorrect executable/mode combinations were refused; evidence is in
  `build/performance/iterations/split-probe-validation/results.json`.
- Clang profile merging retains cold function records by using the default full profile rather than
  sparse merging. The tiny compiler test and main-stage argument regression both pass.
- `make -j$(nproc) perf-pgo-generate` is compiling the full frozen project with ThinLTO and
  instrumentation; log: `build/performance/iterations/m1-generate.log`. Training, merge, profile-use
  build and optimized release validation remain pending.

Remaining M0 observability includes occurrence/proxy eligibility distributions, mutable payload
copied elements, trail/table capacities, and root steal/rebase/move work. These must be supplied for
relevant candidate comparisons; existing nullable fields are not evidence of measured zero cost. M2
drafts are prepared outside the repository while generation keeps source identities frozen.

### Actual pipeline integration and target correction

The full instrumented generation passed, followed by successful training of all 12 workloads and
merge of 119 raw profile files. The first public `make perf-release` invocation then refused the
pipeline before building: its recipe omitted `--mode thinlto-pgo`, so the driver's PGO-only default
disagreed with the preceding three Make targets. This was a real integration defect missed by the
mocked lifecycle tests. The Make recipe now passes the explicit accepted mode. Independent Luna
review verified the correction; it does not claim a release pass.

Generation provenance was refreshed for the changed Makefile and passed (one counter-probe object
recompiled). Training is being repeated under that source identity before another merge/release.
Logs carry `-r2` suffixes under `build/performance/iterations/`. The earlier release refusal remains
recorded in `m1-release.log`; it must not be presented as a successful release build.

The corrected public release target entered actual profile-use compilation, then failed on training
coverage: unprofiled Xuzz translation units were promoted to errors by global `-Werror`, and the
untrained Xuzz `main` encountered other executable entry-point hashes. The log is
`build/performance/iterations/m1-release-r2.log`. The next Luna correction audits all built
executable entry points, separates allowed cold/untrained coverage diagnostics from genuine profile
mismatch errors, and expands training coverage. No profile-use build or held-out release pass is
claimed.

The Luna expert's real two-binary compiler experiment showed that LLVM retains multiple `main`
CFG-hash records in one merged profile and correctly selects matching records when both entry points
are trained. The correction therefore preserves `main` instrumentation and adds missing entry-point
coverage, rather than disabling PGO for entry points or relaxing mismatch diagnostics. Entirely
untrained translation units are allowed only as explicit visible coverage warnings. Hardware
cycles/instructions are currently unavailable: `perf stat -e cycles,instructions true` failed
because `perf` is not installed.

### Subsequent convergence checks

The expanded entry-point correction passed 32 focused tests and its new startup/help smoke commands.
The semantic-identity follow-up then passed all 34 focused tests, including actual Make-expanded
generation/use identity equality, preservation of unrelated warning flags, and actual cold-file
warning extraction into the artifact report. Its first test run found a missing Python test import;
the import was fixed and the full focused suite rerun successfully.

Independent review additionally identified a no-Vulkan shader-helper prerequisite gap and missing
cross-stage llvm-profdata identity checks. The direct shader-helper recipe also lacked the shared
flag-stamp and linker flags. These corrections are being made before the next full pipeline run. The
optimized full test target additionally requires the swarm peer; its distinct entry point must
receive training coverage rather than triggering another mismatched `main` hash during final tests.
The successful generation logs through `m1-generate-r3.log` are infrastructure evidence only; no
profile-use project build or release gate has passed yet.

### Corrected generation and expanded entry-point coverage

After 41 focused tests passed, real generation found a missing public `shader_assemble` Make alias;
the file target existed but the driver requested the public goal. The alias was added and the next
public generation passed. Log: `build/performance/iterations/m1-generate-r5.log`. The stage took
about 41.5 seconds with cached objects. Its resource JSON records Linux child-process RSS
high-water, explicitly not aggregate concurrent-tree memory, so this is not a cold-build cost.

The final narrow independent re-review found no remaining scoped source blocker and kept runtime and
performance gates pending. The current corpus has 25 training and 16 validation entries;
startup/help entries are coverage only. All 41 focused tests were rerun by the parent and passed;
touched Markdown lint and C++ formatting also passed. Training R5 is running under the corrected
source/toolchain identities. The rebuilt swarm-peer help behavior will be verified by that run.

### Successful combined release and current validation

R5 generation, all 25 training workloads (239 raw profiles), merge, and the complete profile-use
build succeeded. All 16 held-out validation workloads passed. The final artifact manifest binds 26
executable/compatibility/library paths to source identity
`0b39e41fd7453534fe899f42a8c5036bae8147d887b56f1e4dda2c9e24b735b5`. No unprofiled translation-unit
warnings were reported. The profile-use stage, including held-out validation, took about 862
seconds; maximum child RSS was 4,833,312 KiB, explicitly not aggregate concurrent process-tree
memory. This is an actual stage observation, not a cold-build prediction.

Reports, merged profile and binary text-section sizes are archived under
`build/performance/variants/m1-thinlto-pgo/`. No executable was moved into an incompatible runtime.
Full tests run with the absolute profile path and matching profile-use mode; a bare Make test would
switch back to control. Backend comparison uses the same binaries and its own six-variable headless
and XDG isolation. Logs: `m1-release-tests.log` and `m1-release-backends.log` under the iterations
directory. They are still running; the 842-test library suite has passed.

Repository `format-check` passed. Repository `lint` failed with 590 Markdown findings in 162 files;
all finding files are tracked and unchanged from HEAD, verified by Git. This is pre-existing
unrelated documentation debt, recorded in `m1-lint-baseline-analysis.json`. Touched performance
documents separately pass Markdown lint, and touched C++ passes clang-format checks. No skipped or
failed gate is counted as green. Full optimized correctness and four-mode comparison gates remain
open, as do the runtime optimization milestones.

### Optimized full correctness and newly available hardware counters

The optimized full test target exited 0: 842 library, 4380 Xuzz, 268 ZigZag, 12 additional, and all
5 network tests passed. The full headless backend comparison also exited 0 across OpenGL, GLES and
Vulkan E2E scenarios. Source and all 26 release artifact hashes were unchanged after these runs. The
full test target took about 1345 seconds, including deliberate network waits; backend checks about
616 seconds. These are correctness runs, not performance samples.

The owner installed `perf`; access to user-space cycles/instructions now succeeds. A separate
supplemental pass used the existing workload correctness validator for all 16 held-out cases with
one warmup and seven measurements: all 128 samples passed and both counters were counted at full
runtime with no multiplexing/unavailable events. Raw CSV and report are under
`build/performance/variants/m1-thinlto-pgo/hardware-r1/`. Counters cover whole user-space process
and subprocess startup/setup, not just internal probe kernels. Counter-run times are diagnostic
only. The separate matched primary timing block also passed all 16 cases. Measurement helper copies
are archived under `build/performance/measurement-tools/`.

Same-source ordinary control, ThinLTO-only, and PGO-only comparisons remain open. The control build
has started serially after the combined variant's measurements completed. M1 is not marked complete
until that comparison evidence is recorded; M2-M6 remain implementation work.

Ordinary control and ThinLTO-only builds and their matched timing/counter passes have now passed.
Each variant ran all 16 held-out timing cases and 128 separate perf samples with both events fully
counted. All retain the same driver source identity `0b39e41f...`. PGO-only remains pending; the
four-mode review stays open. Build resource observations include differing ccache warmth and must
not be presented as controlled cold-build comparisons.

Installed ccache is 4.14.1. Its official source explicitly hashes profile-use data before cache
lookup in both direct/preprocessor paths, consistent with the Make profile-content stamp:
[source](https://github.com/ccache/ccache/blob/v4.14.1/src/ccache/ccache.cpp). No extra cache bypass
or compiler-flag change was introduced for this check.

### M1 closure

All four mode archives are complete and share source identity `0b39e41f...`. Each passed all16
held-out timing workloads with seven measurements; all512 warmup/measured hardware samples had fully
counted user-space cycles/instructions and successful workload validation. The independent C++
expert found no target/ABI/feature mismatch. Its requested combined-mode manifest cleanup was
verified in a follow-up with no remaining M1-scoped blocker.

Reviewable results and raw measured samples are committed in
`performance-architecture-implementation-review/iteration-01-m1-results.md` and the adjacent JSONL
evidence. Whole-process counters, internal scenario timings, variable render work, serial-block
uncertainty and differing build-cache warmth remain explicitly separate. PGO-only regressions are
preserved. ThinLTO and PGO-only warm-object Xuzz relinks performed no object compilation and left
all26 binary hashes and flag stamps unchanged; these are scoped observations, not a universal link
or cold-build speed claim.

M0 fine-grained telemetry remains in progress for later candidate gates. M2 begins with isolated
probe and accessor drafts; no P1-P3/P5 runtime changes have landed. Every later PGO source variant
must be retrained. No whole-plan completion is claimed.

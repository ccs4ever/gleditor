# VM restoration: coordinator plan for the remaining work

Written against `feature/vql-completion` at `8b2d1ee4`. Policy is fixed by commit `e9090999`: every
frontend compiles to a Vortex graph and runs on the VM; there is no direct interpreter and an
unlowered form is an explicit error. The red tests below are that policy's debt, not regressions.

## 1. Measured baseline

Full build is clean (`make -j$(nproc)`, rc 0). `xuzz_test`: 3141 tests, 2930 passed, 28 skipped,
**183 failed**, all with the same cause: a frontend feature that has no lowering yet. The failures
were identical at HEAD before iteration 56 apart from two tests, which iteration 56 fixed. Counts
are by suite; the exact message is the work item's definition of done.

| Track | Refusal message                                                  | Failing suites (count)                                                                                                                                                 |
| ----- | ---------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| L     | `VM lowering is not implemented for VLace programs`              | VlaceIO 21, VlaceCompiler 20, VlacePrelude 17, VlaceE2E 15, VlaceXanadu 13, VlaceRecords 7, VlaceDriver 2, LensVlaceGate 2                                             |
| P     | `Vortex logic execution is unavailable: VM resolution lowering…` | VPrologE2E 9, VortexLogic 9, VPrologCompiler 7, LensVprologGate 5, VortexHost 2, SqlLens 1                                                                             |
| Q     | various `…requires Vortex graph lowering` / `not compiled yet`   | VQLHistory 9, VQLWatch 8, VQLSort 5, VQLMultiStore 5, VQLDynamicView 5, VQLPerspective 3, VQLShapeRuntime 2, VQLLambdaStep 2, VQLEngine 2, VQLValue 1, VQLGenerators 1 |
| M     | consumers of the above through mesh/CLI                          | MeshEvaluator 7, MeshJobs 1, MeshHost 1, MeshCli 1 (expected to clear when L and P land; re-triage afterwards)                                                         |

Q message detail, one work card each (section 3): `version pins require historical VM lowering`,
`watch and perspective statements require Vortex graph lowering`, `watch subscriptions require…`,
`VQL path function 'sort_into' | 'find' | 'count' is not compiled to Vortex yet`,
`this VQL path selector does not have Vortex graph lowering yet`,
`store generators and historical anchors require VM selector lowering`,
`value expression requires Vortex graph lowering`, `unknown compiled function: lens`, shape
projection of non-static rules (`VQLDynamicView`), `frame eligibility found a null AST node`, and
two expectation-only tests (`VQLMultiStore` `frob`, `VQLValue` `value("bad","offset")`) that need a
refusal-type check rather than new lowering.

E2E binary tests (`E2EBinaryOrchestrationTest`, `AnimationTransclusionTest`) shell out to
`build/xuzz`; they pass here only because the full `make` links it. The swarm namespace tests need
`veth` and are the user's to enable; they are outside this plan.

## 2. Coordinator operating rules

These exist to keep the expensive coordinator's context small and the cheap workers' context exact.

1. **The coordinator never reads implementation source.** It reads this file, each worker's one-page
   report, and test summaries. A worker that needs context reads the files named on its card.
1. **One card = one disjoint file set = one worker.** Cards below list the files the worker may
   edit. Two cards never edit the same file in one wave. Shared files (`vortex_vm.cpp/.hpp`,
   `compiler.hpp`) are owned by exactly one card per wave; others request changes through the
   coordinator rather than editing.
1. **Writer freeze, then one build.** Workers edit and run only a focused target
   (`make -j$(nproc) xuzz_test` is acceptable inside a private worktree). The coordinator merges the
   wave, builds once, runs the wave's test filters, then the full suite. No worker commits to the
   shared branch.
1. **Private worktrees.** Each worker runs with `isolation: "worktree"` from the wave's base commit;
   `thirdparty/` must be populated (copy it from the main worktree; `git submodule update` does not
   resolve in a detached temp path). Headless env and the three XDG redirects are mandatory.
1. **Test-first cards.** Each card names failing tests that already exist. The worker's contract is
   to turn exactly those green without weakening them. Changing an expectation is allowed only when
   the card says so (the two expectation-only items) or the old assertion contradicts a ruling in
   `vql-412-completion-gates.md`; the worker must say which.
1. **Reviewer is a second cheap agent with a different prompt**, given only the diff, the card, and
   the refusal rules: no sentinels outside IO (`std::optional`), setters chain, no naked magic
   numbers, no new C++ where Vortex stdlib would do, no `apps/common/xanadu` include of `ui/`. It
   returns `PASS` or a numbered defect list. One fix round, then escalate to the coordinator.
1. **Escalation triggers** (coordinator takes over or asks the owner): any change to
   `CompactOpNode`, the opcode enum order (append only), on-disk formats, `system://` documents; any
   card needing a new core/stdlib capability (each needs the owner's separate approval, see memory
   note on core changes); two failed review rounds; a worker proposing to reintroduce a direct
   interpreter.
1. **Report format** (worker to coordinator, under 40 lines): files changed, tests now green
   (names), tests still red (names and message), anything refused or deferred and why, measured
   numbers if any. No narrative.
1. **Gate after every wave:** `make -j$(nproc)` rc 0; wave filters green; full `xuzz_test` failures
   are a strict subset of the previous wave's; `make format-check` clean on touched files (use the
   clang-format 19 venv); one commit per card, then push.

## 3. Waves and cards

Order is by dependency and by how much red each unlocks. Q cards are small and independent, so they
run in parallel; L and P are large and each starts with a design spike that is cheap to review.

### Wave 0: groundwork (coordinator, one cheap worker, under an hour)

- **W0.1** Record the baseline: commit the failure inventory table above into the iterations log and
  save the failing-test name list as `design/projects/vm-restoration-baseline.txt`, so later waves
  diff names, not counts. Files: that file and `vortex-vm-only-iterations.md`.

### Wave 1: VQL gaps, six parallel cards (Q), cheapest tier

Each worker owns only the listed files plus its own test file. Spec for every card: the section of
`design/vql-query-language.md` named by the failing test; lowering emits ordinary Vortex ops and
reuses the existing scalar/stream/frame machinery; unsupported sub-forms keep an explicit refusal.

| Card | Scope                                                                                                                                       | Failing tests to green                                                 | Files owned                                                         |
| ---- | ------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------- | ------------------------------------------------------------------- |
| Q1   | Path functions `sort_into`, `find`, `count` as graph ops over streams                                                                       | VQLSort (5), VQLMultiStore `find` (2), VQLLambdaStep `count` (1)       | `vql/compile_call.cpp`, new `vql/compile_path_functions.cpp`        |
| Q2   | Version pins and ordering: historical anchors, `store generators`                                                                           | VQLHistory pins/ordering (5), VQLGenerators (1)                        | new `vql/compile_history.cpp`; read-only use of Store history API   |
| Q3   | `watch` and `perspective` statements and subscriptions                                                                                      | VQLWatch (8), VQLPerspective (3), VQLHistory watch (2)                 | new `vql/compile_watch.cpp`                                         |
| Q4   | Remaining path selectors and `value expression` lowering, `lens` as compiled function                                                       | VQLEngine selector (1), VQLShapeRuntime (1), VQLDynamicView `lens` (2) | `vql/compile_path.cpp`                                              |
| Q5   | Computed/non-static shape rules in projection (extends iteration 56's `hasStaticProjectionRuleShape`)                                       | VQLDynamicView shape (3)                                               | `vql/compile_projection.cpp`, `vql/frame_eligibility.cpp`           |
| Q6   | Expectation-only: null-AST guard in frame eligibility returns a typed refusal; `frob` and `value("bad",…)` assert the intended refusal type | VQLEngine null node (1), VQLMultiStore `frob` (1), VQLValue (1)        | `vql/frame_eligibility.cpp` (after Q5 merges), the three test files |

Q5 and Q6 both touch `frame_eligibility.cpp`: run Q6 after Q5's merge, in the same wave.
`compiler.hpp` additions are batched: each card lists the declarations it needs; one cheap worker
(the **integrator**) applies them first so cards never collide on it.

Parallel width: 6, then 1 (Q6). Expected size: small to medium each.

### Wave 2: VLace lowering (track L), staged

Spec: `design/vlace-functional-language.md` and `vlace-lens-bridge.md`; the pre-`e9090999` compiler
in git history (`git show e9090999^:apps/common/xanadu/vlace/compiler.cpp`) is the behavioural
oracle for what each test expects, never a code source to restore as an interpreter.

- **L0 spike (one worker, report only):** which Vlace constructs map onto existing Vortex closure
  and frame machinery (iterations 50 to 55), which need new opcodes. Output: a table construct to
  existing opcode or "needs core change", with the failing tests each construct blocks. The
  coordinator turns "needs core change" rows into owner approval requests; everything else is
  unblocked.
- **L1..Ln (parallel by construct, split by L0's table):** literals and arithmetic (VlaceCompiler
  basics), `let`/lambda/application (closures), `if`/boolean short circuit, pattern matching and
  non-exhaustive/non-linear errors, records and constructors (VlaceRecords), prelude (VlacePrelude),
  IO programs (VlaceIO), Xanadu bindings (VlaceXanadu), driver and type check toggle (VlaceDriver),
  lens gate (LensVlaceGate). Cards own distinct files under `vlace/` plus the matching test file.
- Order inside the wave: literals/arithmetic, then let/lambda/if, then patterns and records, then
  prelude, then IO and Xanadu. Later cards start from the earlier cards' merged commit.

Expected size: the largest track (97 tests). Budget the spike first; do not fan out before L0 is
accepted.

### Wave 3: logic lowering (track P), staged

Spec: `design/vlog-logic-extension.md` §4 to §7 and `vprolog-lens-bridge.md`. The VM already carries
`Unify`, `Choice`, `Fail`, `Cut`, `ChoicePoint` over `ArenaManifold` and `zigzag/vlog`.

- **P0 spike:** what `VM resolution lowering` must emit for clause selection and solutions
  retention; whether §6 (clause selection) fits existing opcodes or needs the Vortex-written solver
  that the doc gates on U1. Output as L0.
- **P1** `VortexLogic` solve entry (9) and `VortexHost::solveLogic`/`solveLogicBatchParallel` (2).
- **P2** `vprolog` compiler to graph (`VPrologCompiler` 7) then REPL/E2E (`VPrologE2E` 9, including
  the exit-code test).
- **P3** lens gates (`LensVprologGate` 5, `SqlLens` 1).

P is independent of L, so P0 and L0 run in parallel in the same wave; P1..P3 are sequential because
each depends on the solve entry.

### Wave 4: mesh, routes, and the §4.12 gates

- **M1** re-run Mesh suites; anything still red is a real mesh bug, triaged individually (expected
  under 5 tests once L and P land).
- **C-gates** from `vql-412-completion-gates.md`: C2 whole-formal defaults and variadic value-lambda
  formals; C4 non-closed static formal defaults and ordered projections (Q5 overlaps); C6 stream
  lifecycle and parallel/mesh behaviour; C7 execute the four documented examples' actual bodies (the
  iteration 56 plan covers example four); C8 the combined route matrix; C9 the measurement and the
  lowering decision. C8 and C9 are coordinator-run, not fanned out.
- Final gate: full `xuzz_test` with zero failures other than documented skips, `gleditor_test` and
  `zigzag_test` unchanged, `make lint` and `make format-check` clean, `make test` swarm tests run by
  the owner where `veth` is available.

## 4. Cost shaping

- Cheapest tier for implementation workers on Q cards and L1..Ln constructs after the spike; spike
  cards (L0, P0) and any card tagged by the coordinator as architecture-sensitive use a mid tier.
  Reviewers use the cheapest tier on a diff under 400 lines, mid tier above.
- A card's prompt is self-contained: the refusal message, the failing test names, the owned files,
  the spec section, the rules in section 2 items 4 to 6. Do not paste the repository guide; workers
  read `AGENTS.md` themselves once.
- Do not run the full suite per card. The full run takes about 280 s; run it once per wave. Per
  card, run `--gtest_filter` on the card's suites.
- Coordinator context stays under the plan, the reports, and the wave gate logs. Keep logs on disk
  (`/tmp/<wave>-*.log`) and quote only counts and test names.
- Fan-out cap: 6 concurrent workers. Beyond that, merge conflicts and build contention cost more
  than the wall-clock saved.

## 5. Open decisions for the owner

1. Whether L0/P0 findings that need new opcodes or stdlib are approved (each separately).
1. Whether the Mesh and CLI refusal text for unlowered forms should be unified into one typed error
   before Wave 1, so tests assert a type rather than a message.
1. Whether to keep `design/projects/vm-restoration-baseline.txt` in tree or only in the iterations
   log.

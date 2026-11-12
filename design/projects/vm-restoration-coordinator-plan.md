# VM restoration: coordinator plan for the remaining work

For a coordinator agent driving low-cost implementation and review subagents. Every claim here was
checked against branch `feature/vql-completion` at `8b2d1ee4` on 2026-11-11 unless marked
**unverified**. The per-test baseline is in
[`vm-restoration-baseline.tsv`](vm-restoration-baseline.tsv) (card, test, observed message).

Policy, fixed by `e9090999`: every frontend compiles to a Vortex graph and runs on the VM. There is
no direct interpreter, and an unlowered form must refuse with an explicit error.

## Status: W0 is done (2026-11-11)

Sections 0 and 1 describe the tree before W0 and are kept as the record. What is true now:

- **W0.1** landed on `main` as `5239d848`. The port covers five files, not three: two IRC evidence
  tests had the same break. A mid-tier security review passed it.

- **W0.2** merged `main` into this branch as `8462ef31` plus fix commit `d1a10256`. Opcodes: this
  branch keeps 47–80; `main`'s `Linked`, `Find`, `FieldSet`, `RunCall` and `RunResume` are 81–85. No
  stored artifact held opcode numbers. A mechanical check confirms that no test name from either
  parent was lost.

- **W0.3** re-measured the baseline at `d1a10256`; see the TSV, which now has a binary column:

  | Binary          | Ran  | Failed | Of which already failing on `main` |
  | --------------- | ---- | ------ | ---------------------------------- |
  | `xuzz_test`     | 3787 | 237    | 11                                 |
  | `gleditor_test` | 842  | 1      | 1                                  |
  | `zigzag_test`   | 268  | 2      | 1                                  |

  The 237 are 182 of the old baseline, 11 of `main`'s, and 44 of `main`'s VQL tests that meet the
  VM-only policy for the first time. The W1 card table below is corrected for them.

- **`main` is not green on its own** at `5239d848`: 15 `xuzz_test` failures (evidence, subscription,
  VPL compiler, one benchmark), one `gleditor_test`, one `zigzag_test`; `make format-check` fails on
  42 files and `make lint` on 2, all byte-identical to `main`. These carry the card `MAIN` in the
  TSV and are outside this plan. Until the owner rules otherwise, the wave gate for format and lint
  is "no new violation relative to `main`", and the final gate is "no failure outside `MAIN`".

- Evidence for all of the above is under `/data/git/gleditor-restore/.logs/w0/`:
  `classification.tsv` and `classification-notes.md` (the 45 new failures),
  `d3-expected-feasibility.md`, `out-of-plan-findings.md`, and the `main-*` and `merged-*` logs.

## 0. Blockers: nothing below can start until W0 is done

1. **The tree does not compile.** OpenSSL 4.1.0 was installed on this machine at 10:17 on
   2026-11-11. That changed the pkg-config flags, so `.buildflags` now forces a full rebuild, and
   the rebuild fails. `apps/common/xanadu/identity/standard_crypto_engine.cpp` has 9 errors:
   `X509_get_subject_name`, `X509_get_issuer_name` and `X509_REQ_get_subject_name` now return
   `const X509_NAME *` at lines 599, 700, 716, 861 and 1043, and `ASN1_BIT_STRING` is opaque at
   845–846 and 1012–1013. `tests/xuzz/standard_crypto_engine_test.cpp:334` has one more error.
   `main` has identical code and is equally broken.
1. **The branch is 155 commits behind `main`.** The merge base is `f0c55b9e`, and this branch has 62
   commits of its own. A trial `git merge main` gives 15 conflicted files and 61 hunks. The largest
   are `vql/vql_engine.cpp` (23), `vortex/vortex_vm.cpp` (13) and `design/vql-query-language.md`
   (6). The others are `Makefile`, `vortex_vm.hpp`, `mesh_evaluator.cpp`, `vpl/compiler.cpp`,
   `vql/compiler.cpp`, `vql/parser.cpp`, `zigzag/arena_manifold.cpp` and five test files.
1. **The opcode enum collides.** Both sides appended after `ChoiceDepth`, which is index 46.
   - `main` added `Linked`, `Find`, `FieldSet`, `RunCall` and `RunResume`, at 47–51. vagent's
     `compiler.cpp` and `meta.cpp` use them.
   - This branch added 34 opcodes, `MatchContract` through `CurrentTimestamp`, at 47–80.
   - Opcode integers are stored in exported programs and mesh artifacts, so this needs an owner
     decision (D1).
1. **`main` extended the direct evaluator that this branch removed.** Commit `afc99f6d` added
   `apply_shape`, `relations` (`vql/relations.cpp`) and `windows` (`vql/windows.cpp`) to the direct
   engine. It also changed the intended semantics of watch, since and delta, and added
   `vql_relations_test`, `vql_windows_test`, `vql_semantics_repair_test` (659 lines) and
   `vql_conformance_test`. After the merge, those features have to be either lowered or refused
   (D2), and Q1 and Q4 below must be re-scoped against `main`'s tests.

## 1. Measured baseline (before W0)

Measured at `8b2d1ee4` with the binary built before the OpenSSL change:

| Binary          | Tests | Passed | Failed | Notes                                                                      |
| --------------- | ----- | ------ | ------ | -------------------------------------------------------------------------- |
| `zigzag_test`   | 256   | 256    | 0      |                                                                            |
| `gleditor_test` | 830   | 830    | 0      | 37 s                                                                       |
| `xuzz_test`     | 3141  | 2930   | 183    | 276 s; 28 skipped (environment-gated `GTEST_SKIP`); 27 disabled, see below |

Of the 27 disabled tests, 13 are profilers. The other 14 are **`VPLEngineTest.DISABLED_*`**, and
they are VPL debt. "Zero failures" does not see them, so card V must re-enable them.

The 183 failures, by card. Mesh tests are split by the frontend they exercise:

| Card  | Tests | Cause                                                                                                                                          |
| ----- | ----- | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| L     | 99    | `VM lowering is not implemented for VLace …`, from `vlace/driver.cpp:50,85` and `vlace/compiler.cpp:764,771,1252`                              |
| P     | 35    | `… VM resolution lowering is not implemented`, from `vortex/vortex_stdlib.cpp:2992` (`VortexStdLib::solve`) and `vprolog/compiler.cpp:237,244` |
| Q1–Q7 | 42    | VQL forms with no lowering, plus 4 real bugs; see Wave 1                                                                                       |
| D3    | 1     | Exception-type contract (owner decision)                                                                                                       |
| V     | 2     | `unsupported VPL monadic/dyadic verb in VM compiler`, from `vpl/compiler.cpp:865,916,925,1004`                                                 |
| M     | 4     | Multi-frontend mesh tests; clear only when L, P and V all land                                                                                 |

These currently **pass** but deliberately assert the refusals, so they must be flipped on purpose:
`LanguageExecutionCutoffTest.*` in `tests/xuzz/language_vm_cutoff_test.cpp` (L and P), and the
`typed-handle VM lowering` assertion at `tests/xuzz/vpl_compiler_test.cpp:251` (V).

## 2. Standing rules for every subagent prompt

Paste this section into every worker and reviewer prompt verbatim; it is the whole context they need
besides their card.

**Setup.** Run these in the worker's worktree. `.claude/worktrees` is on `/data`; `/tmp` is a 16 GB
tmpfs and cannot hold a build.

```sh
git submodule update --init --recursive        # verified: about 12 s in a fresh worktree
export CCACHE_DIR=/data/git/gleditor-restore/.ccache CCACHE_NOHASHDIR=1 \
       CCACHE_BASEDIR=/data/git/gleditor-restore CCACHE_MAXSIZE=20G
make -j$JOBS xuzz_test                         # JOBS is set by the coordinator, see §3
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
       XDG_DATA_HOME=$PWD/build/xdg/data XDG_CONFIG_HOME=$PWD/build/xdg/config \
       XDG_CACHE_HOME=$PWD/build/xdg/cache
./build/xuzz_test --gtest_brief=1 --gtest_filter='<card filter>'
```

The `CCACHE_*` variables are required. The Makefile exports `XDG_CACHE_HOME=build/xdg/cache`, and
ccache follows it, so without them every worktree gets a private cache.

- **Time.** A private cache starts empty: a cold `xuzz_test` build costs about 6 minutes of wall
  time and about 9,300 CPU-seconds on 32 cores (observed). With the shared cache, a second worktree
  got a direct hit for an object the first one compiled (verified on one object).
- **Disk.** A private cache grows to its 5 GiB default limit. In `vql-completion`, 5.1 GB of the 7.2
  GB `build/` directory was `build/xdg/cache/ccache`.
- **Size of the shared cache.** One full build fills 5 GiB, so `CCACHE_MAXSIZE=20G` holds both sides
  of a merge plus a wave of edits without evicting.

**Hard rules.**

- Do not call the residual interpreter helpers left in `vql/vql_engine.cpp`, `vql/sort.cpp`,
  `vql/dynamic_view.cpp`, `vql/version_value.cpp` and `vql/shape_eval.cpp` from any execution path.
  These include `sortInto`, `findInDocuments`, `evaluatePerspectiveStep`, `traverseView`,
  `projectDynamic`, `versionValue`, `evaluatePredicate` and `evaluateShapePredicate`. They are the
  behavioural oracle; read them, do not call them. The same holds for pre-cutoff code read with
  `git show e9090999^:<path>`.
- Lowering emits ordinary Vortex graph operations through the existing compiler helpers (`emitOp`,
  `emitOutput`, streams, frames, mutation groups). It never stores an AST or a callback for the VM.
- The `OpcodeKind` enum is append-only. A new opcode, or a new C++ standard-library function, needs
  coordinator approval before any code is written.
- Do not weaken, delete or disable a test. Change an expectation only where your card says so, and
  say so in your report.
- Diagnostics use `GLEDITOR_LOG_*`, not `std::cout` or `std::cerr`. Use `std::optional` or
  `std::expected`, never sentinel values outside the wire and disk formats. No naked magic numbers.
  Comments explain why, not what.
- Format the files you touch with clang-format 19:
  `uv venv /tmp/cf19 && uv pip install --python /tmp/cf19/bin/python 'clang-format==19.*'`, then
  `/tmp/cf19/bin/clang-format -i --style=file <files>`.
- Commit on your own worktree branch. Never push and never touch the shared branch. Before you
  report, run `rm -rf build` to free disk.

**Report** (under 40 lines): branch and commit; files changed; tests turned green; tests still red,
each with its message; expectations you changed and why; anything you refused or deferred and why.

**Reviewer** (separate agent; it gets this section, the card and `git diff <base>..<branch>`): it
checks the hard rules, that every listed test is green, that no unrelated test changed, and that
each refusal names what is unsupported. It replies `PASS`, or with numbered defects citing
`file:line`. Allow one fix round; after a second failure, the coordinator decides.

## 3. Coordinator rules

1. Do not read source. Read this plan, the TSV, worker reports, reviewer verdicts and gate logs.
   Keep logs on disk under `/data/git/gleditor-restore/.logs/<wave>/`.

1. Within a wave, cards own disjoint files. Shared dispatch points are pre-stubbed by an integrator
   card (W1.0) so that workers only add new files.

1. **Wave gate.** Merge the reviewed branches into the shared branch in card order, then run
   `make -j$(nproc)`, then the full `xuzz_test` once (276 s), `gleditor_test` and `zigzag_test`. The
   failing set must be a strict subset of the previous wave's: compare names against the TSV, not
   counts. Then run `make format-check` (prefix `PATH=/tmp/cf19/bin:$PATH`), `make lint`, and push.

1. **Capacity is computed at the start of each wave, not fixed.** Fan out every card whose
   dependencies are met, up to the number of workers the machine can hold now:

   ```sh
   FREE_GB=$(df --output=avail -BG /data | tail -1 | tr -dc 0-9)
   DISK_SLOTS=$(( (FREE_GB - 25) / 2 ))   # 25 GB: shared cache (20G) plus the gate build
   CPU_SLOTS=$(( $(nproc) / 4 ))          # keep at least 4 compile jobs per building worker
   BUILDERS=$(( DISK_SLOTS < CPU_SLOTS ? DISK_SLOTS : CPU_SLOTS ))
   JOBS=$(( $(nproc) / (BUILDERS < READY ? BUILDERS : READY) ))   # READY = cards ready to run
   ```

   - **Per-worker disk** with the shared cache is about 1.1 GB, measured from the existing tree:
     xanadu and test objects 0.57 GB plus the 0.48 GB `xuzz_test` binary, both as stored on lz4 ZFS.
     The plan budgets 2 GB to allow for growth. Without the shared cache it is about 6 GB, which is
     why the cache matters more than any cap.
   - **Current values (2026-11-11, after `make clean` in every idle worktree):** 112 GB free and 32
     cores give 43 disk slots and 8 CPU slots. So CPU binds at 8 concurrent builders with `-j4`
     each.
   - Spikes and reviewers do not build and are not counted. Recompute before every wave, and after
     any change to free disk.
   - Every worktree except `.worktrees/metacircular-compiler`, which is under active development,
     was cleaned. So the first W0 build in `vql-completion` is cold and fills the shared cache. It
     is cold regardless, because the OpenSSL change forces a full rebuild.

1. **Escalate to the owner** for: anything in section 5; any change to `CompactOpNode`, to on-disk
   or wire formats, or to `system://` documents; a worker that proposes to reintroduce direct
   execution.

## 4. Waves

### W0: make the tree buildable and current (coordinator plus one mid-tier worker; serial)

- **W0.1 OpenSSL 4 port, on `main`.** Fix all 10 errors listed in §0.1.
  - Build each name with `X509_NAME_new` and `X509_NAME_add_entry_by_txt`, then install it with
    `X509_set_subject_name`, `X509_set_issuer_name` or `X509_REQ_set_subject_name`. Do not cast away
    `const`.
  - Read the signature bytes with `ASN1_STRING_length` and `ASN1_STRING_get0_data`.
  - Keep the canonical-scalar check at 845 and 1012 byte-for-byte equivalent.
  - Done when `StandardCrypto*` and the identity tests pass on a clean build. This is security code:
    use a mid-tier reviewer.
- **W0.2 Merge `main` into the branch.** Mid tier or the coordinator; not parallelisable. Resolve
  per D1 and D2.
  - **Unverified:** whether vicon, which holds a `VortexStdLib` (`vicon/compiler.hpp:160`), calls
    any removed solver; the merged build will show it.
  - Done when `make -j$(nproc)` passes and the full suite is run.
- **W0.3 Re-baseline.** Regenerate `vm-restoration-baseline.tsv` from the merged tree. Add `main`'s
  new VQL tests to Q cards or to the D2 card, and re-scope Q1 (`delta`) and Q4 (watch and since)
  against `main`'s versions of `vql_multi_store_test` and `vql_watch_test`. The card table in W1 is
  pre-merge and must be corrected here.

### W1: VQL (seven cards, fanned out per the §3 capacity rule)

**W1.0 integrator** (low tier, about 30 minutes, runs first). It adds dispatch stubs that keep
throwing today's messages, plus the declarations in `vql/compiler.hpp`, so that each card below only
fills in a function in its own new file:

- `vql/compile_selector.cpp:245` for named path functions (Q1).
- `vql/compile_selector.cpp:251` for selectors (Q2).
- `vql/compile_steps.cpp:39` and `vql/compiler.cpp:548,747` for history (Q3).
- `vql/compiler.cpp:514,529` and `vql/vql_engine.cpp:584,742,748` for watch and mount (Q4).
- `vql/compile_projection.cpp:50` and `vql/compiler.cpp:970` for shapes (Q5).
- `vql/compile_call.cpp:223` and `vql/compiler.cpp:931` for `delta`, `lens` and other unknown
  functions (Q1 and Q5).

New `.cpp` files are picked up automatically: the Makefile uses
`find apps/common/xanadu -name '*.cpp'`.

**Corrections from W0.3** (they override the table below where they differ; counts are tests in the
TSV that list the card, and a test may list several):

- **D3 card (mid, runs with W1.0, before every other card; 22 tests).** Boundary conversion to
  `std::expected`, per the owner's ruling and `d3-expected-feasibility.md`:
  - Add a typed category to `CompilationResult` and to the executor's error (`Malformed`,
    `Unsupported`, `Unknown`, `TypeContract`, `Runtime`, `Budget`).
  - Add `VQLEngine::tryExecute`, returning `std::expected<std::vector<CellRef>, QueryError>`, as the
    primary API.
  - Keep `execute` as a thin throwing wrapper: `QueryRefusal` (derived from `std::runtime_error`)
    for unsupported, unknown, runtime and budget failures; `std::invalid_argument` only for
    malformed source; `TypeContractError` unchanged.
  - Replace both mappings at `vql/vql_engine.cpp:174` and `:1032`.
  - Owns `vql/execution.hpp/.cpp`, `vql/vql_engine.hpp/.cpp`, one new header, and the catch in
    `vql/compiler.cpp`.
- **Q1 (17).** `delta` is not a lowering: `main` requires it to refuse, and the D3 card satisfies
  that. `count` and `contains` gain tests from `vql_semantics_repair_test`.
- **Q2 (5).** Also owns the macro-group selector `( )?*+`.
- **Q3 (10).** `current()` only among the generators; the rest wait on R-GEN.
- **Q4 (14).** Blocked on R-WATCH: the two parents' watch tests contradict each other.
- **Q6 (16 tests, 7 defects; mid; runs last in W1, as sole owner of the shared files).**
  - Compiled `%` append does not refuse cycles (`compile_selector.cpp:82-90`).
  - `compile(".")` and `compileVQL("/d.1/d.2")` succeed with no bound context
    (`compiler.cpp:572-581`; needs R-CTX).
  - Clone chains fan out from the master instead of chaining (`compiler.cpp:658`).
  - Stream comparisons use each operand's first cell only (`compiler.cpp:934,942,960`).
  - Excess positional arguments are accepted (`compile_call.cpp:235`, `compile_selector.cpp:126`).
  - `link(+d.edge, 0)` is accepted and mutates (`compile_selector.cpp:177-226`).
  - The `#EMPTY_CALL` fallback runs a function after a genuinely empty walk
    (`compile_steps.cpp:104-124`).
  - Plus the three defects listed under the table.
- **New Q8 (mid; 6).** Session-persistent `def`: `userDefs_` is never written, so a definition does
  not survive to the next `execute`. Owns `vql/execution.cpp` after the D3 card lands.
- **New Q9 (mid; 1).** Field weaving (`compile_block.cpp:115`).
- **New S1 (mid; 8).** Semantic builtins `project` and `apply_shape` (D2).
- **New S2 (mid; 7).** Semantic builtins `any`, `all`, `none`, `one`, `optional_one`, `first` and
  `same_cell` (D2).
- **R-OBS (7 tests).** Seven of `main`'s tests assert a refusal, or a compiler contract, that this
  branch deliberately replaced. No lowering can turn them green; they wait on the owner (section 5).

The cards below are fixed by this table; the test names are in the TSV under each card label.

| Card | Tier | Tests | Feature                                                                                                                                   | Spec                              | Oracle                              | Owns                                                |
| ---- | ---- | ----- | ----------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------- | ----------------------------------- | --------------------------------------------------- |
| Q1   | low  | 10    | Path functions `sort_into` (5), `find` (2), `contains` (1), `delta` (1), `count` (1)                                                      | §4.8.7; grep §4 for the rest      | `sort.cpp`, `findInDocuments`       | new `vql/compile_path_functions.cpp`                |
| Q2   | mid  | 3     | Root selectors `/$slice/##`, `/$slice/^`, `/$slice/^name`; perspective read `$p/@alice:+d.x` with fallback                                | §4.11.2 "Root Operators…", §4.9.4 | `evaluatePerspectiveStep`           | new `vql/compile_root_selectors.cpp`                |
| Q3   | mid  | 7     | Version pins and arithmetic (5), `@a < @b` ordering (1), `current()` / `local()` / `slices()` generators (1)                              | §4.11.1, §4.11.2, §4.11.4–4.11.6  | `versionValue`, `version_value.cpp` | new `vql/compile_history.cpp`                       |
| Q4   | mid  | 11    | `watch … since … until`, poll, cancel and callbacks (8 incl. 3 mount), watch-since (2), scoped `perspective` block (1)                    | §4.9.2, §4.9.4, §4.9.6            | `vql_engine.cpp:581–750`            | new `vql/compile_watch.cpp`                         |
| Q5   | mid  | 7     | Pivots and computed dimensions (3), put/get `lens` (2), shape returned by a function (1), unbound `$shape` raises `TypeContractError` (1) | §4.8.3, §4.8.8, §4.10.1–4.10.3    | `dynamic_view.cpp`                  | new `vql/compile_dynamic_view.cpp`                  |
| Q6   | mid  | 3     | **Real bugs, not lowering**; see below                                                                                                    | §4.8.5, §4.8.8                    |                                     | `vql/frame_eligibility.cpp`, `vql/compile_call.cpp` |
| Q7   | mid  | 1     | `##{ !*, +d.history-test }` on a store proxy: `VqlMaskLinks cannot remove an internal store-ref index link` at `vortex/vortex_vm.cpp:821` | §4.8.3                            |                                     | `vortex/vortex_vm.cpp` (sole owner in W1)           |

The three Q6 bugs:

- `VQLValueTest.InvalidOffsetsRefuseBeforeMutation`: `$item/value("bad", "offset")` returns
  silently. It must raise `std::runtime_error` before any mutation.
- `VQLEngineTest.SproutMorphism`: `$r/=> [ "Alpha", "Beta" ]` throws
  `frame eligibility found a null AST node` at `frame_eligibility.cpp:411`.
- `VQLLambdaStepTest.UserDefinedFunctionBlockBodyAndWeave`: a `def weave` with a block body returns
  an empty result instead of the cell.

Tier choice: low tier only where a direct oracle exists and the change is a single new function
(Q1). Everything that touches history, streams or the VM is mid tier. A failed low-tier round costs
more coordinator time than the price difference saves.

### W2 and W3: VLace (L) and logic (P), each spike-first; V can run alongside

Each track starts with a **spike**: mid tier, read-only, no build. It returns a table of each
construct (or predicate), the existing opcodes and helpers that lower it, any "needs new opcode or
stdlib" item with justification, and the TSV tests it unblocks. The coordinator sends the "needs"
rows to the owner (D4) and only then fans out.

- **L** (99 tests).
  - Spec: `design/vlace-functional-language.md` §4–§7 and §13 (`std:xanadu`), plus
    `vlace-lens-bridge.md`.
  - Oracle: `git show e9090999^:apps/common/xanadu/vlace/compiler.cpp`.
  - Reuse: the closure machinery (`CreateClosure`, `ApplyClosure`, `RestoreBindings`, iterations
    50–55).
  - Cards, in order: (1) literals, arithmetic, comparison and `if` (VlaceCompiler); (2) `let`,
    lambda, application and letrec (VlaceCompiler, VlaceDriver); (3) lists, ADTs and `case`,
    including `NonExhaustiveMatch` and `NonLinearPattern` (VlaceE2E); (4) records (VlaceRecords);
    (5) prelude (VlacePrelude); (6) IO (VlaceIO, LensVlaceGate); (7) `std:xanadu` (VlaceXanadu).
  - Each card owns separate new files under `vlace/`. `vlace/compiler.cpp` dispatch is pre-stubbed
    as in W1.0. Card (1) also flips `LanguageExecutionCutoffTest.VlaceCompilerRefusesAstExecution`
    and `…VlaceDriversReportCompileRefusalWithoutEffects` to assert success through the VM.
- **P** (35 tests).
  - Spec: `design/vlog-logic-extension.md` §3–§6 and `vprolog-lens-bridge.md`.
  - The VM already has `Unify`, `IsVar`, `MakeVar`, `MakeTerm`, `Deref`, `Choice`, `Fail`, `Cut`,
    `ChoiceDepth` and `ScopedChoice`. There is no `ChoicePoint` opcode, although AGENTS.md says
    otherwise.
  - Oracle: `git show e9090999^:apps/common/xanadu/vortex/vortex_stdlib.cpp`; the solver was about
    1,400 lines.
  - Cards, in order:
    - P1: `VortexStdLib::solve` lowers to a graph. This turns VortexLogic (9) and VortexHost (2)
      green, including the deterministic order of `OrParallelLogicSearchIsDeterministic`.
    - P2: `vprolog::Compiler::solve` and `solveOnce` (VPrologCompiler 7), then the `build/vprolog`
      CLI (VPrologE2E 9). `QueryFailureExitsFalse` expects `false.`.
    - P3: LensVprologGate (5) and SqlLens (1).
    - P1 flips `LanguageExecutionCutoffTest.PrologCompilerRefusesHostResolution`.
  - The spike must say whether §6 clause selection fits the existing opcodes or needs U1; U1 is an
    owner decision.
- **V** (2 mesh tests, 14 disabled tests).
  - Missing lowerings: the `each` adverb, custom verbs, monadic `⍳ , ⊂ ⊃ ⍴ ⍉ ⊖ ⌽ * × ⌊ ⌈`, dyadic
    `⍳ , ⍴ ↑ ↓` and power, and rank arithmetic (`vpl/compiler.cpp:840–1010`).
  - Spec: `design/vpl-array-language.md` §2–§4.
  - Candidate reuse: the `sys:array` stdlib, e.g. `VortexStdLib::arrayIota`. Whether it is callable
    from a graph is **unverified**; the spike answers it.
  - Done when both mesh VPL tests pass, the 14 `DISABLED_` tests are re-enabled and pass, and
    `vpl_compiler_test.cpp:251` is flipped.

### W4: mesh, routes and the §4.12 gates (coordinator-led)

- **M**: the 4 multi-frontend tests (`AllFrontendsUsePrivateOwnedContexts`,
  `ParsingIsBoundedBeforeRecursiveLanguageConsumers`, `MeshJobs…EveryFrontend…` and
  `MeshHost…TwoOwners…`) must pass once L, P and V land. Anything still red is a mesh bug and gets
  its own card.
- **§4.12** (`vql-412-completion-gates.md`): the remaining parts of C2 and C4, then C6 stream
  lifecycle and parallel/mesh. C7's four §4.12.2 examples already have tests in
  `vql_computed_targets_clock_test.cpp`; confirm they still pass after the merge. C8 (route matrix)
  and C9 (measurement and lowering decision) are run by the coordinator, not fanned out.
- **Final gate:** `xuzz_test` with 0 failures and 0 non-profiler `DISABLED_` tests; `gleditor_test`
  830/830 and `zigzag_test` 256/256, or higher after the merge; `make format-check` and `make lint`
  clean. `make test` swarm tests are run by the owner (they need `veth`).

## 5. Owner decisions

Decided on 2026-11-11:

- **D5:** the OpenSSL port landed on `main` first.
- **D1:** this branch keeps opcodes 47–80.
- **D2:** `main`'s direct-evaluator features are kept, refused, and given lowering cards (S1, S2).
- **D3:** `std::expected` where feasible, otherwise `QueryRefusal`. The spike found full conversion
  costly (259 throw sites, an estimated 2,000–3,500 lines) and boundary conversion cheap, so the D3
  card in W1 does the boundary conversion.

Ruled by the owner on 2026-11-11, after W0.3 (each ruling binds the cards that list it in the TSV):

- **R-WATCH:** a `watch` statement runs only with a `since` baseline backed by store history;
  otherwise it refuses. The poll API stays. `StreamingAndDeltaMetadata` and
  `MultiStatementScriptWithForAndWatch` are rewritten to expect the refusal, and the duplicate
  `RefusesSimulatedDeltaMetadata` (a merge artifact with the branch's body) is reconciled with them.
  Card Q4.
- **R-OBS:** rewrite the seven obsolete `main` tests as proposed per test in
  `.logs/w0/classification.tsv`: keep a refusal assertion only for forms that are still refused,
  with a message that names the feature, and assert success where the branch compiles the form. New
  card T1 (low tier, test files only, runs after the D3 card).
- **R-GEN:** tentatively, `local`, `topic`, `btpk`, `system` and `slices` refuse for a missing
  store, and `current()` is lowered. Card Q3. The owner also wants a way to select store
  alternatives, so that `local(x)` can fall back to or combine with `local(y)`. That is a language
  design item needing its own proposal and approval; nothing in this plan builds it, and the refusal
  must not foreclose it.
- **R-CTX:** compiling a context-relative path with no bound context is refused, except inside
  functor definitions. Card Q6.
- **R-NUM:** numeric comparison is exact. `ScalarComparisonCoercionBoundariesRemainStable` is
  rewritten to the exact rule. Card T1.
- **R-BARE:** bare `$s{...}` stays an in-place mask. `main`'s spec lines and the one assertion in
  `vql_semantics_repair_test` are corrected; `project` and `apply_shape` are still added. Card S1.
- **R-RERUN:** a compiled creation may run again and appends again; the "already used" assertion is
  dropped. Card T1.
- **MAIN:** `main`'s 13 failing tests and its format and lint debt stay out of scope. The gates are
  "no failure outside `MAIN`" and "no new format or lint violation relative to `main`".

Ruled by the owner on 2026-11-11, after the three spikes (D4). The spike reports, with the card
splits and the card-to-test mappings, are in `/data/git/gleditor-restore/.logs/w2/`
(`L0-vlace-spike.md`, `P0-logic-spike.md`, `V0-vpl-spike.md` and the three `*-cards.tsv`); they
replace the card lists under "W2 and W3" above.

- **Logic opcodes:** approved after a probe. A probe test first shows that `MakeVar`, `MakeTerm` and
  `Deref` outputs cannot be chained by a graph; if it fails as predicted, those three publish their
  outputs as cell references and `MakeTerm` builds through `Vlog::makeTerm`. If the probe passes,
  nothing changes.
- **`RelationNext`:** approved. A read-only accessor opcode plus a `setRelationProvider` hook.
- **`DimByName`:** approved. `DimByName(name, mintFlag)` for VLace `std:xanadu`.
- **`Hypertime`:** approved. No inputs, one Int64 output: the arena's operation high-water mark.
- **`Pow`:** approved now, with its own tests, although no existing test needs it.
- **VLace IO:** a host trampoline satisfies the policy. IO actions evaluate on the VM to pure
  description values; `runIO` performs the leaf effects and calls the VM for every force and
  application. No effect opcode.
- **`VPLEngine`:** becomes a thin facade over the VM executor; the dead interpreter is deleted; the
  three refusal tests are rewritten to prove that no AST is executed directly.
- **Untested VPL semantics** (key, empty reduce, copies of quoted cells): follow
  `design/vpl-array-language.md` where it speaks; otherwise refuse with a named message. Each card
  tests what it implements.

All four opcodes are appended by one serial card, O1, which owns `vortex/vortex_vm.hpp/.cpp` until
it lands, so that no two cards append to the enum. The workers' common instructions are in
`/data/git/gleditor-restore/.logs/card-brief.md`.

Ruled by the owner on 2026-11-11, after the first W1 batch. The full list of what that batch found,
including the shared-file hooks for a second integrator card, is in
`/data/git/gleditor-restore/.logs/w1/pending-owner-and-hooks.md`.

- **Cell-identity comparison:** approved. One read-only, pure opcode: two cell inputs, one Bool
  output. Needed by multi-subject `apply_shape`, `same_cell` and watch-since.
- **Detached topology copy for `project`:** approved after a read-only spike specifies it and
  confirms that nothing existing does it.
- **`PinHistorical`:** approved. A cell stream and a constant naming the (store, version) fold in;
  the matching historical proxies out; a cell absent from the fold yields nothing.
- **`ArenaManifold` undo of a space attach:** approved. `Mark` records the space count and
  `release()` truncates back to it, so a failed compile leaves no pinned history behind.
- **`find`:** no new opcode. It is lowered with `RelationNext` and a host-registered prose-hits
  provider (reviewer's verdict, adopted).

These go through a second serial opcode card, O2, after O1 lands.

Gate A (2026-11-11, shared branch at `98ecab03`, cards P0, V1.0, L0, Q1, Q3, Q4 and S1 merged):
`xuzz_test` 206 failures of 3892, nothing newly failing against the 225 baseline, 19 turned green;
`gleditor_test` and `zigzag_test` unchanged. The current failing list is
`/data/git/gleditor-restore/.logs/w1/gate-a-xuzz-failed.txt`.

**Model usage is a capacity limit too.** Eight mid-tier workers at once exhausted the five-hour
usage limit twice. Hold concurrent mid-tier agents (workers plus reviewers) to about four.

## 5a. Original decision table (before W0)

| Id  | Before | Decision                                                                                                                   | Recommendation                                                                                                                                                          |
| --- | ------ | -------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| D1  | W0.2   | Which side keeps opcodes 47–51                                                                                             | Keep this branch's 47–80, renumber `main`'s five to 81–85, and regenerate any artifact that stores opcodes. That moves 5 names instead of 34.                           |
| D2  | W0.2   | `main`'s direct-evaluator additions (`apply_shape`, relations, windows, watch/since/delta changes)                         | Keep the parsing and data structures, refuse execution until lowered, and add a W1 card per feature.                                                                    |
| D3  | W1     | Compile-time refusals reach callers as `std::invalid_argument` (`vql_engine.cpp:174`); 3 tests expect `std::runtime_error` | Throw a `QueryRefusal` that derives from `std::runtime_error` for "unsupported or unknown" (runtime-shaped) refusals, and keep `invalid_argument` for malformed source. |
| D4  | W2/W3  | New opcodes or stdlib found by the spikes                                                                                  | Approve each separately, as per standing practice.                                                                                                                      |
| D5  | W0.1   | Land the OpenSSL port on `main` first and merge it in, or on this branch                                                   | `main` first, since `main` is equally broken.                                                                                                                           |

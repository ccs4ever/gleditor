# vagent implementation and user-guide progress

The objective is a working initial-release vagent language as specified by
[vagent v3.9](vagent-agent-language.md), with a complete introductory
[user guide](../docs/vagent-user-guide.md). A frontend-only executable is an iteration, not the
finished language. Deferred U-A extensions stay deferred; they are not substitutes for initial
features or prerequisites for declaring the initial release complete.

## Iteration sequence

| Iteration                           | Deliverable                                                                                                                                                 | Required evidence                                                                                                                                        | State                                                                                 |
| ----------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------- |
| 1. Source frontend                  | Owned tokens, complete surface parser, static checker, `vagent check`, source diagnostics, sample corpus.                                                   | Positive and negative lexer/parser/checker tests; all 23 isolated specification units checked; CLI exit codes and stdin/file diagnostics.                | 108 tests pass; module and template checks remain open.                               |
| 2. Runtime prerequisites            | Approved P1, P12, P13; Vlog clause database and registered scanning functions required by vagent.                                                           | Dynamic cut barriers, caller-preserving cuts, nested mark undo, trail/Dewey lifecycle and scanning/logic regressions.                                    | P1/P12/P13 verified; numeric regressions pass. Other prerequisites open.              |
| 3. Compilation and artifacts        | Typed lowering to Vortex, activation frames and spills, bounded statements, modules, verified artifact loading, linked `std:agent` prelude.                 | Compile/load round trips, malformed artifact refusal, recursion, search/undo, contracts and opcode allowlist tests; no AST interpreter replacing Vortex. | Scalar kernel: 39 VM tests pass; remaining compilation open.                          |
| 4. Actor turns                      | One arena/cursor per actor, genesis/dimensions, initialize-once, budgets, recovery, staged outbox, deterministic harness.                                   | Whole-turn and per-statement rollback tests; no external effect before successful commit; same-frame restoration and fuel/cycle/settle failures.         | Open.                                                                                 |
| 5. Beliefs and BDI                  | Typed keyed/source-aware facts, queries/logical update view, goal kinds, plans, subgoals, conditions, waits and captured bindings.                          | Complete adoption/selection/reconsideration/maintain/back-off/failure and await race coverage through turns.                                             | Open.                                                                                 |
| 6. Effects and composition          | Capacities/skills/behaviors, native asynchronous effects, deadlines/cancel/preemption, scheduling, questions and typed performatives.                       | Multi-turn effects and late completion, swaps, handler merge/consume, question typing/fallback and scope/sender tests.                                   | Open.                                                                                 |
| 7. Long-lived mesh                  | Approved P2–P8 and P14, single-owner spaces/holons, issuer budgets, renewal/stopping, fair finite-service P4 arbitration, identities/checkpoints/migration. | Queue/retry/floor/lease/slot regressions, bounded service under load, checkpoint/resume and migration including pending-state restrictions.              | Open.                                                                                 |
| 8. Product integration              | `run`, `check`, REPL, launcher, settings/schema/notes P9, logging P10, build/tests/fuzz P11.                                                                | Real command-line/REPL journeys, failure exit codes, settings and binding diagnostics, fuzz inputs and normal Makefile targets.                          | `check` CLI verified; other integration open.                                         |
| 9. Guide and end-to-end conformance | Complete beginner-to-operation guide, checked examples and executable sample project.                                                                       | Guide examples pass appropriate syntax/static/runtime checks; final worked examples exercise actual actors; limitations match current capabilities.      | Complete teaching draft; 25 source units statically checked, runtime validation open. |

These tracks may overlap. Each iteration must update evidence rather than relabel an incomplete
feature as implemented. The final completion audit covers every initial feature in §§1–17, P1–P14,
initial boundaries in §18, all four worked source examples, and the 23 small examples. The bounded
Contract-Net transition contract remains a deferred library illustration with its stated
host-binding/source prerequisites; dedicated protocol syntax is not part of the release.

## Architecture and interfaces

The frontend lives in `apps/common/xanadu/vagent/`; the executable lives in `apps/vagent/`. Tokens
own their spellings and diagnostics retain line/column/offset. The parser returns a typed, move-only
AST through `std::expected`, with explicit limits and no partial success after an error. Its AST
describes the complete specified surface, including contexts in which checking may refuse a
construct. Static checking must establish types, purity, positions, signatures and scopes before
lowering. Unavailable module loading is a refusal, not permission to trust unknown imports.

Vortex remains the backend. Source checking does not execute agents. The first frontend target has
no graphics/device dependencies and can be tested independently, while the same frontend test
sources are included once in `xuzz_test`. Runtime tests stay beside the engine. The initial CLI
checks standalone units; cross-file module resolution, lowering, running and the REPL remain
separate tracked work until verified.

`ChoiceDepth` is appended after `Cut`; the existing pure-job artifact allowlist and range check
remain unchanged. Existing opcode numbers, `CompactOpNode`, the on-disk store and fixtures retain
their layout. New VM functionality needs runtime tests, not merely successful frontend parsing.

## First iteration commands and evidence

The Makefile exposes:

```sh
make -j$(nproc) vagent vagent_frontend_test
make -j$(nproc) test/vagent-frontend
make -j$(nproc) test/vagent-compiler test/vagent-runtime
build/vagent check tests/samples/vagent/mini_01.vagent
build/vagent check tests/samples/vagent/*.vagent
```

`check` reports static diagnostics and does not assert that a source has been lowered, linked or
executed. The extracted samples are exact separate source units from design §12.6; dedicated
negative and structural tests provide independent evidence beyond accepting the examples.

The current verification checkpoint is:

- The combined frontend suite passes 108 tests: 15 lexer, 33 parser, 59 checker, and one guide test
  that parses and checks all 25 complete source units in the guide. The 23 separate specification
  examples are also covered. These checks establish source acceptance, not agent execution.
- CLI checks cover all 23 sample files, stdin, help/version, malformed source, unknown names, type
  errors, missing files and usage exit codes.
- The shared runtime suite passes 91 tests covering arena undo, logic choices, numeric operations,
  OR-parallel path lifecycle and artifact admission. The independent codebase review found no
  actionable issue in these shared-runtime changes; see
  [runtime review](projects/vagent-implementation-reviews/runtime-r1.md).
- The first scalar compiler kernel passes 39 integration tests executing actual Vortex graphs. It
  lowers literals, typed scalar bindings, arithmetic, string concatenation, comparisons,
  conjunction, alternatives, negation, assignments, failure, blocks, lexical locals and
  conditionals, with separate value-producing and discarded-result contexts. It preserves caller
  choices and distinguishes a successful `false` value from failure. Unsupported forms are refused
  before graph allocation. This internal API is not yet exposed as an executable language runner.
  The [review checkpoint](projects/vagent-implementation-reviews/README.md) links the converged
  writer, language, Vortex and codebase reviews and states their scopes. The separate
  [user-guide review](projects/vagent-guide-review/README.md) records three rounds with those four
  Luna roles and a learner, all converged on the revised 24-unit guide. The later
  [educational review](projects/vagent-guide-education-review/README.md) adds a record lesson and
  transfer exercises; four Luna teaching/student roles converged on the current 25-unit guide after
  two rounds.
- The final full project build passes. The broader headless engine run passes 1,996 tests with no
  failures; 17 network tests are skipped without their dedicated swarm harnesses, and one test is
  disabled. That run preceded the last checker API/purity guard changes; the final focused frontend
  suite verifies those additions.
- Iterative inheritance checking accepts the formerly crashing 8,001-capacity reverse chain under a
  512 KiB process stack. Public-AST depth/node limits and separate purity-analysis and
  symbol-resolution work budgets return named diagnostics before recursive checking or repeated
  scans exhaust their work grants. Indexed member lookup and cached owner scopes also accept an
  8,000-method flat owner under the default budgets. Signature comparisons debit parameter/type-node
  weights; goal parameter and checked-goal lookup use indexes instead of repeated scans.
- The instrumented frontend fuzzer completes 5,000-run smoke checks with AddressSanitizer and
  UndefinedBehaviorSanitizer; the final run includes the checker safety guards.

Static module aliases/selectors and adopting-agent scope checks still need work. Cross-file module
loading, the graph call ABI and linked prelude, complete lowering, actor turns, BDI, effects,
scheduling, persistence and the REPL remain open. The guide explains the specified language; its
execution traces have not yet been validated against an agent runtime. The
[runtime grounding audit](vagent-runtime-grounding.md) records why existing `Call` and stdlib entry
points cannot simply be treated as the required language call ABI.

Repository-wide formatting and lint gates were attempted. Formatting reports 106 existing files
outside this change; lint stops on existing typography literals in `acme_presenter.hpp:28` and
`slice_view_presenter.hpp:46`. Those results are failures, not passing or skipped gates. Changed
files are checked separately without reformatting unrelated code.

## Completion audit checklist

- Every declared builtin/user type, declaration and expression form has parser, checker and lowering
  coverage, including all position and purity restrictions and ambiguous keyword cases.
- Agents execute through Vortex opcode graphs and the mesh actor adapter; no side interpreter is
  used to claim language conformance.
- Turn, statement, handler and plan failure follow the specified undo/staging rules; host effects
  are asynchronous and no failed turn drains its staged outbox.
- Beliefs, provenance, all goal kinds, intentions, suspension/captured waits, conditions and
  lifecycle work through real turns and persist only where the initial design permits.
- P1–P14 and their consumers have meaningful tests, including regression checks for existing
  VQL/VPL/Vlog/vicon/mesh behavior and pure artifact boundaries.
- Command-line and REPL operations, native bindings, module/artifact paths, system settings and
  failure diagnostics are usable through the advertised interfaces.
- Documentation introduces all language features and operational limits, and demonstrated source
  units and the guide's complete sample project are verified against actual supported behavior.
- Formatting, lint, relevant unit/integration tests and the headless initial-release journeys pass;
  environmental test failures are distinguished from passing evidence and remain visible.

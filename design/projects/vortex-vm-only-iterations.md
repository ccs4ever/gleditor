# Vortex-only frontend iterations

## Execution contract

Every executable language submission is compiled to a real Vortex instruction graph and run by the
Vortex VM. Text and ASTs are compiler inputs, never runtime programs. Native operations may provide
data operations and host effects; they must not hide AST evaluation or a recursive language solver.
Unsupported lowering refuses explicitly. Temporary loss of interpreter-only features is accepted.

## Iteration 1: execution cutoff

- VQL defaults to VM execution and rejects direct mode. Public engine query/path/step APIs compile
  and run the VM; mesh queries, quoted selectors and result lenses use the shared executor.
- VQL and VPL compilation report explicit cursor, terminal and result slots and roll back failed
  assembly. VQL compilation accepts coordinator services and bindings independently of the legacy
  engine object.
- VPL defaults to VM compilation. Direct engine entrypoints refuse; incomplete compiler operations
  refuse instead of emitting identity, null or placeholder operations. Host and mesh VPL execution
  currently refuse through those entrypoints until migrated to the shared compiled runner.
- Prolog's recursive C++ resolver is removed. Solve entrypoints refuse until resolution control is
  compiled to VM instructions. Clause graphs alone are data and do not satisfy this contract.
- VLace public evaluation and IO execution refuse after parsing/type checking until closures,
  thunks, patterns and effects have real graph lowering.
- VIcon retains its compiled generator/VM cursor route. VAgent remains check-only. No VWeave
  implementation exists in this checkout.

Legacy private VQL/VPL interpreter helpers and compatibility names remain removal debt; they must
not be reachable from public execution entrypoints. Historical semantic tests are reference cases
for restoring missing lowering, not evidence that disabled language features still work.

## Next iterations

1. Complete graph ownership, session bindings/definitions, activation cleanup and shared VPL runner.
   Remove remaining legacy evaluator implementation and compatibility execution options.
1. Complete VQL §4.12: recursive frames, default thunks, complete functors/streams and atomic
   mutation-boundary contracts, followed by the remaining VQL compiler capability matrix.
1. Restore VPL dynamic array/rank/view and effect semantics through genuine VM instructions.
1. Compile Prolog dispatch, recursion, freshening, unification, choices, cut and solution yields.
1. Compile VLace lexical closures, lazy memoized thunks, patterns and IO sequencing.

Each iteration requires independent Luna architecture and senior gates and profiler review. Validate
AST destruction before execution, all frontend routes, explicit refusal, session lifetime,
budgets/cancellation, failure isolation and exported fresh-VM execution where supported. Measure
parse, compile and VM costs separately before making optimization claims. Freeze all source writers
before builds; use `make -j$(nproc)` and headless tests with all three XDG directories isolated.

## Iteration 1 validation

- Full `make -j$(nproc)` and repository `format-check` pass.
- 112 focused cutoff/compiler/frontend tests pass, including AST destruction, failed assembly
  rollback and retry, host/mesh-related VQL routes and exported fresh-VM execution.
- An additional VM/parser/typed-call run passed 137 of 146 tests. The nine failures are legacy
  `VortexLogicTest` cases expecting the deliberately removed C++ resolver (equal, member, append,
  length and hypermedia predicates). They remain restoration cases; no full-suite pass is claimed.
- Headless CLI checks passed: direct flags refuse with exit 2, VPL scalar execution yields 3,
  consecutive REPL submissions yield 3 then 9, and Prolog/VLace report unavailable VM lowering
  cleanly with exit 1. All three XDG directories were isolated.
- Fifteen VPL interpreter semantic fixtures are preserved as disabled reference tests. Other
  interpreter-only semantic tests remain incompatible until their VM implementations are restored.

Logs: `/tmp/vortex-vm-only-build.log`, `/tmp/vortex-vm-only-focused.log`,
`/tmp/vortex-vm-only-core.log`, `/tmp/vortex-vm-only-format.log`.

## Iteration 2: shared VPL execution and portable graphs

- CLI, host and mesh VPL execution now use one compiler/VM executor. Persistent executor sessions
  retain scalar/rank bindings, directed axes and source dependencies; failed submissions do not
  publish binding updates. Rebinding the host manifold clears session references and provenance.
- Programs return their final expression; literal vectors/scans retain their complete rank. Scalar
  operations on ranks, unknown variables and untyped dimension handles refuse explicitly until
  corresponding VM lowering exists. VPL's retired engine is no longer used by these routes.
- VQL and VPL share the relocation exporter. A VPL arithmetic program is saved, reloaded and run in
  a fresh VM after the original arena/compiler/VM are destroyed. Full portable result-layout
  metadata remains future work, including singleton rank/scalar distinctions for artifact consumers.
- Missing VQL bindings now produce empty streams rather than fabricated false-valued cells.

Validation: full `make -j$(nproc)` passed. The combined cutoff, compiler, session/provenance, mesh,
CLI and exported-artifact matrix passed all 141 tests. Repository formatting passed before the final
test-input corrections; the modified tests were formatted with clang-format 19 afterwards. Luna
architecture and independent senior reviews found no remaining blocker for this bounded iteration.
No full-suite pass or full language/specification completion is claimed.

Logs: `/tmp/vpl-shared-vm-build.log`, `/tmp/vpl-shared-vm-focused.log`,
`/tmp/vpl-shared-vm-format.log`.

## Iteration 3: runtime VQL blocks and context contracts

- Nested block-level `for` loops, destructuring, per-member `where`, conditional actions and ordered
  multi-item returns now lower to runtime stream iterators and graph-referenced branches. Lexical
  shadowing restores outer bindings; untyped absent fields bind empty instead of dropping the
  iteration; literal field defaults mint fresh cells only when needed.
- Context contracts support `.: Type` and `.: $alias: Type`. Context captures drive function bodies,
  later parameter defaults and explicit dot parameters. A selected union arm without a context
  capture preserves the caller context through VM branches and reference copies.
- The portable descriptor ABI is now `VQL_CONTRACT/2`, including implicit context-capture slots.
  Version 1 is refused explicitly; old executable contract artifacts must be recompiled.
- Request-scoped execution restores the VM's prior contract coordinator on every exit, including
  restoring an unset coordinator, so returned mesh contexts retain no request-local service pointer.
- Export/reload tests execute both conditional branches and runtime destructuring/default/filter
  loops in a fresh VM. The same for-contract query passes CLI, host query/navigation/script/batch
  and mesh routes. Compiler and AST destruction before execution is tested directly.

Validation: full `make -j$(nproc)` passed; all 238 focused frontend, parser, contract, stream and VM
core tests passed. Formatting is checked independently. Luna architecture, senior engineering and
profiler reviews accepted the bounded design; no performance or full-specification claim follows.
Nonliteral contract-field defaults, recursive frames, field-weave results and nested effect loops
remain explicit limitations. Mixed-kind/numeric-text comparison policy is the next independent
semantic gap; same-kind comparisons are used in the exported filter fixtures.

Logs: `/tmp/vql-block-vm-build.log`, `/tmp/vql-block-vm-focused.log`,
`/tmp/vql-block-vm-format.log`.

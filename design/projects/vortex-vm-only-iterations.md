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

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

## Iteration 4: typed scalar comparisons

VQL comparison expressions now emit the graph-native `VqlCompare` instruction. Generic VM comparison
instructions keep their existing language-independent behavior. Operands resolve runtime references
and clone masters; missing operands compare false, including inequality. Int/Int comparisons are
exact, Float/Float comparisons follow IEEE rules, and bounded whole-token text inference uses the
shared scalar parser. Other comparisons use canonical scalar text and original nonnumeric text.
Typed value bits take precedence over custom display text. Mixed Int/Float comparisons explicitly
refuse pending the cross-type policy decision.

The instruction is included in persisted opcode decoding and the mesh artifact allowlist. It
bypasses value-only memoization and eager input snapshots to preserve missing references. Tests
cover integer bounds, IEEE special values, text fallback, clone values, missing operands, work
exhaustion, and actual exported graphs. The CLI/host/mesh filter fixture now compares text cells
against an integer literal. Text byte charging occurs after materialization; it does not bound the
preceding string allocation. Full `make -j$(nproc)` passed, and all 249 focused tests passed.
Independent Luna architecture, senior and profiler reviews accepted this bounded implementation. No
full-suite or performance claim follows. Logs: `/tmp/vql-comparison-build.log`,
`/tmp/vql-comparison-tests.log`, `/tmp/vql-comparison-format.log`.

## Iteration 5: lazy alias field defaults

Contract field defaults may capture a prior lexical variable or earlier parameter as a graph slot.
The matcher reads that slot only when the field is absent, preserving the original cell identity;
empty bindings remain empty. Existing fields skip fallback resolution entirely. Larger alias streams
refuse explicitly because the current capture representation does not preserve them in full. Path
and function-call defaults still require genuine VM thunks and refuse during compilation.

`VQL_CONTRACT/3` adds explicit subject counts and ordered alias slots to matching and return checks.
Older descriptors refuse. Exact operand and capture layouts are checked before matching. Descriptor
collection is shared by call guards, return checks and loop destructuring.

General matcher atomicity remains unfinished: literal-default cells can survive a later mismatch. A
whole-matcher arena rollback would currently leave minted dimension-cache entries stale, so this
iteration adds no such rollback. Alias resolution itself introduces no graph mutations.

Validation: full `make -j$(nproc)` passed; all 261 focused tests passed, including malformed ABI
refusal before default allocation and alias execution from a saved graph in a fresh VM. Independent
Luna architecture and profiler reviews accepted this bounded scope. No full-suite pass or general
lazy-thunk/atomicity completion is claimed. Logs: `/tmp/vql-alias-default-build.log`,
`/tmp/vql-alias-default-tests.log`, `/tmp/vql-alias-default-format.log`.

## Iteration 6: lossless mixed numeric comparisons

Mixed Int/Float operands compare their exact numeric values while preserving their stored types. No
Int-to-Float conversion is used. Integer boundaries are checked before converting a finite in-range
Float's integral part, after which exact integer ordering and its fractional part determine the
result. NaN stays unordered, signed zeros compare equal, and infinities lie outside the finite
integer range. Both operand orders use the same comparison rule.

Regression coverage includes all six operators in both operand orders, fractions, subnormals, IEEE
special values, adjacent integers beyond 2^53, and both Int64 boundaries. Exported graph and
CLI/host/mesh fixtures now exercise mixed numeric comparisons.

Validation: full `make -j$(nproc)` and repository formatting passed; all 263 focused tests passed.
The mixed matrix checks 18 numeric pairs across six operators in both operand orders (216 VM
comparisons), including unchanged operand kinds/bits. Independent Luna senior and profiler reviews
found no blocker; no performance improvement or full-specification completion is claimed. Logs:
`/tmp/vql-mixed-numeric-build.log`, `/tmp/vql-mixed-numeric-tests.log`,
`/tmp/vql-mixed-numeric-format.log`.

## Iteration 7: atomic matching and explicit resume state

Arena rollback now includes dimension-cache insertion journals and empty-run scalar headers. Nested
discard keeps undo records available to an outer mark; prefix hydration starts a clean local
journal. Branch-prefix payloads use `APF2` and refuse `APFX`. Taking a mark may allocate floor-stack
capacity; its API no longer promises `noexcept` or allocation-free operation.

Failed shape matches, union arms and exceptional default evaluation discard their arena changes.
Successful matches publish captures and their Boolean result together; return checks discard all
temporary validation defaults even on success. Work already spent remains spent after rollback.
Contract instructions refuse attached parameter pipelines and pre/postconditions in this bounded
implementation. General mutation-boundary contracts, recursive frames and path/call default thunks
remain future work.

### Goal-control regression uncovered by correct rollback

The corrected scalar trail exposed VIcon's reliance on compile-minted counter writes escaping
backtracking. `ResumeInit` and `ResumeSet` now represent that control state explicitly in the VM.
Activation token references roll back with choice points; their Int64/Bool values survive resume.
Cursor closure cleans up values, and restart/exhaustion closes the VIcon cursor. Limits and
condition flags compile to these graph instructions. Data cells retain normal rollback.

Nested/dynamic/nonpositive limits, bounded conditions, repeated loop activation and token ownership
are regression cases. Recursive VIcon call cycles refuse pending real call frames. Mesh artifacts
and OR-parallel spark export refuse resume state until transfer semantics are implemented. No
end-to-end latency or allocation improvement is claimed; state cleanup and ownership scans are not
constant-time. General bounded-body semantics for VIcon `while` and mutation-boundary enforcement
remain separate work. Stream element contracts intentionally validate without publishing per-element
captures, as specified in §4.12.5.

Validation: full `make -j$(nproc)` and repository `format-check` passed. All 395 affected tests from
33 suites passed, including overlaid-store preservation, allocation/fuel recovery, existing VQL
foreign/in-place paths, VIcon controls, native resume identity/ownership, goal helper cleanup, Vlog,
and OR-parallel prefix hydration. Independent Luna architecture, senior and profiler reviews gated
the bounded implementation. No full-suite pass or full §4.12 completion is claimed.

Logs: `/tmp/vql-contract-atomicity-build.log`, `/tmp/vql-contract-atomicity-tests.log`,
`/tmp/vql-contract-atomicity-format.log`.

## Iteration 8: stream contract capture scope

Stream element types and stream refinements validate each member without exporting its captures. The
compiler now omits those names from the match output layout, so an empty capture slot cannot shadow
an enclosing lexical binding or context. Shapes outside a stream still publish their normal
captures, including non-stream union arms. Invalid stream members still reject the entire call.

`VQL_CONTRACT/4` makes the corrected output layout explicit and refuses older descriptor versions.
No Store operation layout changes. The graph remains executable through native contract
instructions; no textual evaluator or process-local AST registry is introduced.

Validation: full `make -j$(nproc)` and repository `format-check` passed. All 402 affected tests from
34 suites passed, including seven capture-scope regressions, descriptor version refusal, retained
alias-default inputs, and saved-graph execution in a fresh VM for populated and empty streams.
Independent Luna architecture, senior and profiler reviews found no blocker. This bounded scope does
not complete §4.12 or introduce lazy default thunks; no runtime performance improvement is claimed.

Logs: `/tmp/vql-stream-capture-build.log`, `/tmp/vql-stream-capture-tests.log`,
`/tmp/vql-stream-capture-format.log`.

## Iteration 9: whole-step creation yields

Creation selectors emit their complete runtime result stream before the step's dereference,
predicates and clamp. Yield then selects once across the flattened stream: `!last` keeps its final
member, `!both` yields all original contexts followed by surviving creations, and `!keep` retains
the contexts while creation side effects still run. Source order and duplicate contexts are
preserved. Read steps keep their existing `!last`/`!both` behavior.

Existing-target `link()` fan-out mints a fresh clone on each runtime iteration through native graph
creation and link instructions, rather than reusing a compiler-allocated output cell. This preserves
distinct identities and each context's attachment. No interpreter fallback is introduced.

Validation is pending. This bounded change does not add general yield overrides for opaque functor
applications, shape predicates, lazy default thunks, recursive frames or mutation-boundary
contracts.

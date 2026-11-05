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

Validation: full `make -j$(nproc)` and repository `format-check` passed. All 412 affected tests from
36 suites passed, including eight new VM regressions, the previously failing contexts-before-clones
case, captured functors after rebinding, and persisted graphs executed in a fresh VM. Independent
Luna architecture, senior and profiler reviews gated the bounded change; no performance improvement
or full-suite pass is claimed.

This change does not add general yield overrides for opaque functor applications, shape predicates,
lazy default thunks, recursive frames or mutation-boundary contracts. Full §4.12 remains incomplete.

Logs: `/tmp/vql-yield-baseline.log`, `/tmp/vql-creation-yield-build.log`,
`/tmp/vql-creation-yield-tests.log`, `/tmp/vql-creation-yield-format.log`.

## Iteration 10: static dimensional shape predicates

Nonempty literal shape predicates with targetless dimension rules compile to native graph
instructions. Signed rules test the immediate neighbor, `~d.name` accepts either direction, and
`!d.name` requires absence in both directions. Rules combine with short-circuit conjunction. Shape
filtering preserves candidate identity, order and duplicates, and runs after scalar predicates but
before the range clamp. The same helper serves Boolean shape tests and captured functor bodies. The
AST keeps scalar and shape predicate groups separately; this stage does not preserve their original
bracket interleaving.

`ProbeLink` is an explicit, metered one-hop VM instruction. It resolves canonical foreign neighbors
and recognizes self-links without applying rank iteration's visited-cell suppression. Ordinary
`Link` behavior is unchanged. Compilation emits no runtime AST evaluator or shape callback.

Empty shapes, wildcards, value and nested constraints, shape references/functions, pivots and
virtual-dimension rules still refuse explicitly. They require further graph lowering. Validation:
full `make -j$(nproc)` and repository `format-check` passed. All 435 affected tests from 39 suites
passed, including seven shape tests, seven native probe tests, CLI/host query, navigation, script
and batch routes, source mesh execution, sealed mesh artifacts, and saved graphs executed in a fresh
VM. Independent Luna architecture, senior and profiler gates accepted the bounded stage. No runtime
performance improvement is claimed; mesh also charges its normal per-instruction fuel.

An expanded 446-test run passed 439 and failed seven broader mesh expectations on missing Prolog,
VLace, and VPL array/enclosure lowering. These failures are retained in the expanded-run log. No
full-suite pass or full §4.12 completion is claimed.

Logs: `/tmp/vql-shape-predicate-build.log`, `/tmp/vql-shape-predicate-focused.log`,
`/tmp/vql-shape-predicate-tests.log` (expanded run), `/tmp/vql-shape-predicate-format.log`.

## Iteration 11: scalar and nested static shape targets

Static shape rules may constrain an immediate neighbor to a scalar literal or another static shape.
Scalar constraints use native `VqlCompare` equality, preserving exact integer and lossless mixed
numeric comparisons. Nested shapes emit further native probes and Boolean graph branches. A
bidirectional rule succeeds when either selected neighbor matches; a missing outer neighbor never
satisfies a nested absence check. Negated dimension rules retain absence-in-both-directions
semantics and do not evaluate their static targets. Dynamic target expressions still refuse.

Compiler preflight detects cyclic programmatic shapes, limits nesting to 128, and caps expanded
shape work at 4096 units across a compilation. A separate budget of 4096 rule visits also bounds
validation of shared and ignored target subtrees. Its expansion count includes duplicated nested
bidirectional branches. These guards bound this lowering; they do not establish a general parser or
whole-query compilation budget. Shared acyclic shape subtrees remain supported.

Foreign comparisons now inspect the source manifold's scalar kind and read its typed payload,
instead of rejecting a proxy wrapper or relying on display text. Exact Int64, Double and Bool bits
survive misleading text and foreign clone masters; actual structural handles still refuse. The
shared, work-metered clone resolver now lives in the ZigZag layer for both consumers.

Validation: full `make -j$(nproc)` and repository `format-check` passed. All 449 affected tests from
42 suites passed, including seven target tests, six compiler-bound tests, the restored existing
nested-shape regression, CLI/host/mesh routes, and saved graphs executed in a fresh VM. Independent
Luna architecture, senior and profiler reviews accepted the bounded stage. No full-suite pass or
runtime performance improvement is claimed. The broader unsupported-language mesh failures recorded
in iteration 10 remain separate work.

Wildcard/exact-valence shapes, dynamic shape values, general field-default thunks, recursive VM
frames and mutation-boundary contracts remain open. Full §4.12 is incomplete.

Logs: `/tmp/vql-shape-target-build.log`, `/tmp/vql-shape-target-tests.log`,
`/tmp/vql-shape-target-format.log`.

## Iteration 12: shared scalar contract reads

Comparisons and structural contracts now use one query scalar reader in the ZigZag layer. It
resolves clone masters and foreign source kinds, preserving Int64/Double/Bool bits despite custom
display text. Primitive contracts no longer classify a typed foreign value from its display. Only
textual values satisfy String, including empty text; Timestamp keeps its existing textual fallback.
Untyped text uses the same bounded inference as comparison, so an oversized integer is not silently
admitted as a rounded Float. Float contracts still admit Int values without converting the stored
bits, as specified.

Missing cells and valid structural handles are ordinary primitive mismatches. Invalid foreign
sources and unreadable typed payloads fail loudly. Any/Cell keep their physical identity semantics.
Untyped text bytes are charged before materialization; the reader still traverses text twice and no
allocation or latency improvement is claimed.

Validation: full `make -j$(nproc)` and repository `format-check` passed. All 454 affected tests from
43 suites passed, including local/foreign contracts, foreign clone masters, empty text, lossless
inference, Timestamp fallback, invalid proxies and saved graphs. Independent Luna architecture,
engineering and profiler reviews accepted this bounded correction. §4.12 remains incomplete;
activation frames and resumable default thunks are the next dependencies.

Logs: `/tmp/vql-contract-scalar-build.log`, `/tmp/vql-contract-scalar-tests.log`,
`/tmp/vql-contract-scalar-format.log`.

## Iteration 13: native activation-frame foundation

Native `InvokeFrame` and `ReturnFrame` instructions use versioned graph descriptors with explicit
local, argument and result records. Each invocation maps logical local registers to fresh arena
backing; graph constants and external cell identities remain shared. Cell ports accept zero or one
cell, while Stream ports preserve order and duplicates and accept scalar singletons. Caller result
slots resolve before activation. Per-cursor activation metadata participates in choice snapshots,
and explicit cursor close clears its live frame maps while saved choices can restore their state.

Descriptor reference records carry tagged full-width Int64 targets. Executable export validates and
relocates those records while preserving their role text; ordinary integer data stays numeric data.
Live activation export, framed stdlib entry, and unsupported condition/pipeline composition refuse
explicitly. Mesh frame artifacts still refuse until their descriptor/lifecycle route is implemented.

This stage does not yet lower VQL definitions to reusable frames: the compiler still inlines calls
and refuses recursive expansion. The next iteration must migrate argument/default/context/capture
and result streams to reusable function graphs, followed by resumable contract-field default thunks.
Full §4.12 remains incomplete.

Return publication, frame removal and cursor changes commit under one arena savepoint and restore
all affected VM maps on failure. Frame depth, local count, descriptor records and cumulative backing
cells have configurable limits. Backing cells remain alive because returned values may refer to
them; the default cumulative backing-cell cap is 1,048,576 and successful allocations are not
refunded by return or backtracking. Stream frames retain the existing one-active-stream-cursor
policy. Malformed frame bodies that halt or fall off without returning fail explicitly. Descriptor
header/role bytes and span fragmentation are bounded before materialization and charged as work.

Validation: full `make -j$(nproc)` and repository formatting passed. The affected matrix passed all
477 tests from 45 suites, including real recursive graphs, nested results, identity, streams, choice
restoration, cancellation, budget refusal/retry, malformed metadata and fresh-VM export. The broader
VM/language sweep passed 242 of 269 tests; the remaining 27 exercise Prolog execution routes already
refused by committed code because VM resolution lowering is absent. Its stale serialized-opcode
boundary assertion was updated for the appended opcodes. No full-suite pass is claimed. Independent
Luna architecture and engineering/profiler gates reviewed this bounded stage.

An opt-in native-call probe ran headlessly in isolation on an AMD Ryzen Threadripper 1950X with the
repository's Clang `-O3 -g` build. Each sample invokes the same identity frame 32 times; setup is
outside the timed interval. The following are single observations, not speedup or latency promises.

| Locals | Unrelated reference registers | Elapsed (µs) | Arena cells allocated |
| ------ | ----------------------------- | ------------ | --------------------- |
| 2      | 0                             | 255          | 64                    |
| 2      | 100                           | 346          | 64                    |
| 32     | 0                             | 516          | 1,024                 |
| 32     | 100                           | 779          | 1,024                 |
| 256    | 0                             | 3,438        | 8,192                 |
| 256    | 100                           | 3,816        | 8,192                 |

Arena backing grows with locals times calls. Descriptor parsing and rollback-map snapshots remain
measurable costs; the probe does not count heap allocations and unrelated-register setup also grows
the arena. No optimization or fixed per-call cost is established. The probe remains disabled during
ordinary test runs and is available with `--gtest_also_run_disabled_tests` and
`--gtest_filter='*DISABLED_NativeInvokeScaling'`.

Logs: `/tmp/vql-frames-build.log`, `/tmp/vql-frames-tests.log`, `/tmp/vql-frames-vm-tests.log`,
`/tmp/vql-frames-profile.log`, `/tmp/vql-frames-format.log`.

## Iteration 14: reusable closed-function graphs

Eligible closed primitive/Stream calls now compile to `InvokeFrame` and one reusable body graph per
function in the query. Reserved descriptor roots support forward self/mutual references; finalizing
one is atomic and refuses duplicate finalization. The compiler prepares actual arguments in caller
scope before invoking, passes context and ordered formal streams explicitly, and distinguishes an
omitted default from an explicitly empty argument through presence ports. Defaults run lazily in the
callee and can call the same reusable function without expanding its AST recursively. Parameter
mismatch returns an empty stream; successful body results pass their return contract before return.

Function bodies have separate instruction chains, with only their mutable output registers and
explicit ports designated local. Constants and actual external cells stay shared. Export inspection
now follows descriptor entries and graph branch targets, and a self-recursive traversal survives
Store save/reload after the compiling VM and AST are destroyed. Host and mesh Vquery source routes
execute these real graphs. Sealed Vortex frame artifacts remain explicitly unsupported as in the
previous stage.

A bounded, query-wide AST analysis selects the reusable subset before recursive signature/type
consumers run. Cyclic/malformed ASTs and budget exhaustion are hard errors. Free captures, shape
contracts, context-dot targets, functors, variadics and other unlowered forms keep the existing
compiled inline path; this is a second graph lowering strategy, not direct execution. Ineligibility
propagates through user-call dependencies using a budgeted reverse-edge queue, preserving outer
bindings for an otherwise closed wrapper that calls an inline-only helper. Eligible recursive
components remain reusable. Recursive cycles involving excluded forms still refuse explicitly.

Validation: full `make -j$(nproc)` passed. All 493 affected tests from 47 suites passed, including
finite self/mutual recursion, identity, defaults, depth Budget errors, indirect outer captures,
forward descriptor rollback/retry, cyclic ASTs, host/mesh source execution, and fresh-VM recursive
export. A headless CLI query created a three-cell chain and recursively returned `third`. The
broader VM/language sweep passed 244 of 271 tests, retaining the same 27 missing-Prolog- resolution
failures recorded in iteration 13. No full-suite pass or speedup is claimed. The native backing
allocation policy and measured costs from iteration 13 still apply. Independent Luna architecture
and senior/profiler gates assess this bounded stage; full §4.12 remains incomplete.

Next: expand the formal/context/capture ABI, then structural matching continuations and real graph
field-default thunks, mutation boundaries, and the remaining functor/variadic/lifecycle semantics.

Logs: `/tmp/vql-recursive-build.log`, `/tmp/vql-recursive-tests.log`,
`/tmp/vql-recursive-vm-tests.log`, `/tmp/vql-recursive-cli.log`, `/tmp/vql-recursive-format.log`.

## Iteration 15: explicit context-formal frames

One explicit `.` formal now uses reusable frame lowering, including primitive/Stream annotations and
closed expression defaults. Its positional Stream port remains separate from the hidden caller
context port. Defaults and contracts execute before the formal rebinds `.`, so a default can read
the prior hidden context or an earlier named parameter; an explicitly empty argument still overrides
that hidden context. UFCS retains the existing implicit positional argument-zero rule. Nested calls
restore their caller context, and recursive dot calls preserve actual cell identities and order.

Validation: full `make -j$(nproc)` and all 501 affected tests from 48 suites passed. Eight new tests
cover scalar/multiple/empty dot values, omission and default presence, type filtering, prior/hidden
context defaults, UFCS, nested/recursive calls, host and mesh Vquery source routes, and fresh-VM
Store export/reload. Luna architecture and independent senior/profiler gates accepted the bounded
stage. No ABI or performance improvement is claimed; the previously measured frame costs remain. The
broader VM sweep's known missing Prolog resolution work is unchanged; no full-suite pass is claimed.
Multiple explicit dot formals, structural captures/destructuring, free captures, functors,
variadics, nonliteral field defaults and mutation contracts remain open for reusable lowering. Full
§4.12 remains incomplete.

Logs: `/tmp/vql-context-frames-build.log`, `/tmp/vql-context-frames-tests.log`,
`/tmp/vql-context-frames-format.log`.

## Iteration 16: closed structural-contract frames

Closed structural annotations and destructured formals now use reusable native frames. A declared
parameter type matches before its target pattern; parameter mismatch returns empty, and return
promises retain their error behavior. Match captures and context-fallback outputs are frame locals,
so recursive invocations keep independent bindings while external parent/field identities survive.

Eligibility resolves every field-default alias against the lexical scope before its whole match,
including nested types, base/refinement pairs and independent union arms. Completed parameter
captures become available to later parameters. Generic element/refinement captures stay private,
while their defaults and AST structure are still scanned. Nested field types retain pointer-cycle,
depth and query-wide work guards. Free aliases and transitive inline-only callees retain compiled
inline lowering. Nonliteral field defaults still require graph continuations and are not enabled.

Validation: full `make -j$(nproc)` passed. All 514 affected tests from 49 suites passed, including
thirteen new structural-frame tests, all four contract modes, literal and lexical defaults,
parameter filters and return errors, scope/cycle regressions, stream identity/order/duplicates,
context restoration, recursive fresh-VM Store export/reload, and host/mesh Vquery source routes.
Independent Luna architecture and senior/profiler reviews accepted this bounded stage. Repository
formatting passed with clang-format 19. No performance improvement or full-suite pass is claimed.
The broader sweep's known Prolog resolution gaps remain unchanged. Full §4.12 remains incomplete.

Logs: `/tmp/vql-structural-frames-build.log`, `/tmp/vql-structural-frames-tests.log`,
`/tmp/vql-structural-frames-format.log`.

## Iteration 17: destructured loops and lexical frame boundaries

Closed destructured `for` bindings now use reusable frames through the existing native loop and
contract graph lowering. Per-member captures, literal/earlier-lexical defaults, typed filtering and
untyped missing-field empty bindings retain their established behavior. Nested loops preserve order
and duplicate identities; recursive loop graphs survive Store export/reload into a fresh VM.

Eligibility now mirrors the compiler's scope restoration: prefix `let` bindings before the first
loop remain visible to later script statements, while loop bindings and subsequent `let` bindings
remain local to the iteration. This also fixes false closedness for named loops: a later reference
to a caller-only alias keeps compiled inline lowering rather than losing the caller's binding.
Patterns still resolve defaults against their pre-match scope and publish captures afterward.

Validation: full `make -j$(nproc)` and all 521 affected tests from 50 suites passed, including seven
new loop-frame tests and real host/mesh Vquery source results. Independent Luna architecture and
senior/profiler gates accepted the bounded stage. Repository formatting passed with clang-format 19.
No runtime ABI, performance improvement or full-suite pass is claimed. Full §4.12 remains
incomplete.

Logs: `/tmp/vql-loop-frames-build.log`, `/tmp/vql-loop-frames-tests.log`,
`/tmp/vql-loop-frames-format.log`.

## Iteration 18: graph-native scalar string conversion

`string(expr)` now compiles to the appended native `VqlString` instruction. One positional argument
is normalized as a complete runtime stream; zero or multiple values refuse rather than truncating.
The shared query scalar reader preserves typed Int64/Double/Bool bits over misleading text,
including foreign cells and clone masters. Text passes through unchanged; numeric and Boolean values
use canonical scalar formatting from their stored bits, including normalized zero. Structural
handles refuse explicitly. User-defined `string` functions retain precedence over the builtin.

The primitive stages its result before publication, validates output shape, and charges member
visits and shared scalar reads. Pipeline/condition/pre-postcondition/memo combinations and raw
StdLib calls with incompatible operands refuse explicitly. VM and sealed-artifact opcode bounds are
updated without renumbering existing instructions. Sealed scalar artifacts execute the primitive;
sealed stream/frame artifact limitations remain separate work. No runtime AST or evaluator fallback
is introduced. Lazy parameter-expression defaults may call `string`; nonliteral contract-field
defaults still require graph continuations and refuse.

Validation: full `make -j$(nproc)` and all 533 affected tests from 52 suites passed, including
eleven new scalar-string tests, serialized-opcode boundary validation, budget recovery, caller/frame
scope, source-AST destruction and Store reload into a fresh VM, host/mesh Vquery sources and sealed
Vortex scalar artifacts. Independent Luna architecture and senior/profiler reviews accepted this
bounded stage. Repository formatting passed with clang-format 19. No performance improvement or
full-suite pass is claimed. Additive scalar/string expressions and the remaining §4.12 work are
incomplete.

Logs: `/tmp/vql-string-build.log`, `/tmp/vql-string-tests.log`, `/tmp/vql-string-format.log`.

## Iteration 19: native additive value expressions

Existing value-expression positions accept left-associative `+`, including expression-bodied
functions, arguments and comparison operands. Both child ASTs participate in the query-wide pointer,
cycle, depth, node and lexical dependency analysis. Compilation prepares both complete operand
streams in order and emits the appended `VqlAdd` opcode; no first-cell truncation or runtime AST
evaluation is introduced. Legacy direct evaluator helpers explicitly refuse the new AST node.

The native leaf shares cardinality and typed scalar reading with `VqlString`. Two text values
concatenate without numeric inference. Two Int64 values use checked addition; two Double values use
IEEE arithmetic. Mixed numeric addition requires an exactly representable Int64-to-Double promotion
before IEEE arithmetic, using unsigned magnitude and bit widths rather than an unsafe cast back to
Int64. Booleans, structural handles and mixed text/numeric values refuse. Existing exact comparison
semantics are unchanged. Output is staged until validation succeeds; native VM guards and opcode
bounds include the new leaf.

Computed `string()` and `+` outputs immediately execute `VqlBoxScalar`, a narrow allocation
primitive that publishes a fresh typed scalar or text cell through a reference slot. This fixes a
real CLI failure where a loop returned `second`, `second` for `first`, `second` because both results
aliased one mutable register. Variable/path aliases and immutable constants retain their identities.
Allocation and reference publication share an arena savepoint; refusal restores the prior output
reference and value without refunding work. Existing arena allocation limits apply to retained value
cells. Numeric arena cells remain bits-only until rendering/promotion.

Validation: full `make -j$(nproc)` and all 547 affected tests from 53 suites passed, including
fourteen new additive and computed-value lifetime cases. The actual §4.12 `format_title` and
`process_video` destructuring bodies execute in native frames. Tests cover overflow, exact/inexact
mixed promotion, typed foreign/clone values, full cardinality, free bindings, lazy parameter
defaults, cyclic AST refusal, fresh-VM Store reload, host/mesh Vquery sources and a sealed scalar
artifact. A real headless `vquery` invocation returned `h264 @ 60fps`; dedicated CLI loops returned
`first`, `second` and `first!`, `second!`. Lifetime tests check distinct output identities,
unchanged earlier results, nested calls/defaults, work/allocation refusal with prior-reference
preservation, retry and sealed boxed-result dereference. Independent Luna architecture and
senior/profiler gates accepted the bounded compiler/VM scope. Repository formatting passed with
clang-format 19. No speedup or full-suite pass is claimed.

Support is bounded to existing value-expression positions: direct top-level arithmetic, return-item
grammar and some path-led bindings still need integration. Direct interpreter consumers in dynamic
views, sorting and store-anchor helpers remain routing debt and refuse this node rather than
evaluate it. Sealed artifacts still refuse normalized stream instructions; the sealed scalar-opcode
test does not establish compiler-generated graph artifact parity. Nonliteral field-default thunks,
mutation boundaries, free closure inputs and remaining §4.12 work stay incomplete.

Logs: `/tmp/vql-additive-build.log`, `/tmp/vql-additive-tests.log`, `/tmp/vql-additive-format.log`,
`/tmp/vql-additive-cli.log`.

## Iteration 20: caller-bound free capture ports

Reusable definitions now carry free scalar/stream bindings through sorted hidden Stream ports after
the inherited context port. Explicit actual arguments compile in caller scope first; capture streams
then preserve the current caller bindings, identities, order, duplicates and empty values. Frame
bodies seed hidden aliases before ordered formal defaults/matches, so completed parameter captures
shadow them in the same order as inline lowering. Wrapper formals can supply a callee's free name
without making that name an outer capture of the wrapper. Direct/mutual recursion uses real frames.

The analyzer records original lexical scope at each callsite and propagates missing free names and
required field-default bindings through a budgeted work queue. Scope copies are charged before
insertion; capture metadata is sorted. Free-name discovery never invents a lexical binding. Union,
generic-element, prematch field-default and loop scopes retain their established behavior. Return
promises scan against the body's final escaping scope, allowing body-prefix lets to supply defaults.

Missing ordinary free values can remain empty. Missing field-default aliases still compile-refuse,
even when a field is present; explicit empty bindings retain their old semantics. Static multi-cell,
functor and pipeline default-alias refusals remain. A required free field-default dependency inside
a lazy parameter-default call conservatively keeps the enclosing definition inline, preserving the
explicit-actual path that skips that default. Known captured functors or pipelined values also use
existing compiled inline graphs. No direct evaluator, closure callback, new VM opcode or frame
descriptor version is introduced.

Validation: full `make -j$(nproc)` and all 560 affected tests from 54 suites passed, including
thirteen new capture cases. Tests cover native scalar/ordered-stream capture, per-call rebinding,
shadowing, transitive/formal forwarding, prematch/default/return scopes, recursion/depth refusal,
conservative inline boundaries, and host/mesh Vquery source routes. A captured recursive query
exported from a pure arena, then ran after source VM/AST destruction and Store reload with captured
identity intact. Base-backed export still requires the separately unimplemented source binding. Real
headless CLI checks returned `kept` through recursion and `first`, `second` after same-query
rebinding. Independent Luna architecture and senior/profiler gates accepted this bounded stage.
Repository formatting passed with clang-format 19; no runtime speedup or full-suite pass is claimed.

This does not implement first-class functor/lambda capture ABI, native variadics, lazy field-default
continuations, mutation-boundary contracts or the remaining route/lifecycle work. Full §4.12 remains
incomplete.

Logs: `/tmp/vql-capture-frames-build.log`, `/tmp/vql-capture-frames-tests.log`,
`/tmp/vql-capture-frames-format.log`, `/tmp/vql-capture-frames-cli.log`,
`/tmp/vql-capture-frames-rebind-cli.log`.

## Iteration 21: isolated graph rollback scopes

The next lazy-matching prerequisite is a native `ScopedChoice` with a direct graph target. Unlike
ordinary logic choices, internal rollback scopes must neither consume OR-worker oracle directives
nor become stealable alternatives. Their snapshots retain the caller's Dewey path, and closing
internal scopes must not publish branch-pruning callbacks. Graphs can use two nested scopes with
`ChoiceDepth`, `Fail` and `Cut` to roll back a successful validation into a success continuation,
while a failed attempt rolls back into a failure continuation. Caller choices below the recorded
barrier remain available.

This is a rollback substrate, not completed lazy field-default lowering. The shared matcher still
requires compilation of its traversal and default continuations; path/call defaults, mutation
contracts, functor ABI and route/lifecycle parity remain unfinished. No runtime AST evaluator or
reentrant VM execution is introduced.

The configurable limits default to 128 total outstanding choices while a scope is open, including
caller-prefix choices and nested ordinary choices, and 1,048,576 aggregate saved-snapshot entries.
The entry cap includes the prospective new snapshot and already-saved caller snapshots; borrowed
views count them before copying maps or taking an arena mark. Shared stream/iterator/frame payloads
are counted conservatively, and counted entries spend work that rollback does not refund. This is an
entry policy, not a byte-memory or speed guarantee. Invalid scopes and exceeded limits refuse before
adding a snapshot. Explicit cursor closure retains existing snapshots for same-cursor backtracking;
running to Halt/falloff with an unclosed scope refuses. Cross-cursor execution, backtracking and new
choices refuse while a scope is active. Owned scopes permit first stream use, including when
ordinary choices are nested inside them.

A scoped alternative's direct opcode target and successor chain are never executed as a parameter
pipeline. Conditions, memoization, raw stdlib calls and pipeline invocation of the instruction
refuse. Export relocates the direct target through the existing reference wings; live scoped
snapshots refuse export before store writes. Sealed mesh artifacts still refuse scoped choices. Cuts
suppress pruning callbacks for the entire scope-owned region, including nested ordinary choices;
caller-prefix pruning outside that region retains its existing behavior.

Validation: full `make -j$(nproc)` and all 580 affected tests from 56 suites passed, including
twenty new scoped-choice cases. Regressions execute both validation continuations, preserve caller
alternatives, restore fresh scalar allocations, references, streams and frame state, preserve oracle
directives and parent Dewey paths, enforce depth/snapshot/work limits and cross-cursor refusals, and
run relocated graphs after source-VM destruction and Store reload. Independent Luna architecture and
senior/profiler gates accepted this bounded prerequisite. Repository formatting passed with
clang-format 19. No speedup or full-suite pass is claimed.

Logs: `/tmp/vql-scoped-choice-build.log`, `/tmp/vql-scoped-choice-tests.log`,
`/tmp/vql-scoped-choice-format.log`.

## Iteration 22: one backend-driven structural matcher

The shared matcher now uses an internal continuation driver. It owns ordered union, cardinality,
shape, refinement, field-probe, lazy-default and capture-publication decisions; a synchronous arena
backend supplies control scopes and domain leaves. Backend-associated subjects, contexts, bindings
and compile-time continuations allow a later graph backend to consume the same walk. Callbacks run
only during a matching/build call and never become VM runtime closures.

This is a parity refactor, not nonliteral field-default support. Existing VM instructions still use
versioned contract descriptors and the synchronous matcher; the graph backend and lazy thunk
continuations remain to be implemented. No direct evaluator fallback is added.

The arena backend borrows input spans and singleton views, retains each default result only through
its immediate matching callback, and uses borrowed `function_ref` continuations. It introduces no
whole-input copy before fuel checks. Its field/element/arm loops remain iterative; only structural
type nesting recurses. Typed fields check before publishing their own aliases; bidirectional fields
try the reverse only when the first neighbor is absent, and a present mismatched neighbor never uses
a default. Null-capture nested checks roll back before an outer refinement executes, while captured
nested checks retain their defaults. Generic validation rolls back its whole collection scope even
on success. The original lexical default environment remains stable until whole-match publication.

Validation: full `make -j$(nproc)` and all 588 affected tests from 56 suites passed, including eight
new shared-matcher parity cases. Tests cover union/field/default order, prematch aliases,
bidirectional presence versus type failure, ignored generic captures and successful rollback,
shape/refinement overwrite order, exception atomicity, nested null-capture rollback timing and an
8,192-field flat pattern. Existing VM, frame, export/reload, host/mesh and language regressions
remain in that matrix. No performance improvement or full-suite pass is claimed.

Logs: `/tmp/vql-contract-driver-build.log`, `/tmp/vql-contract-driver-tests.log`,
`/tmp/vql-contract-driver-format.log`.

## Iteration 23: lazy computed field defaults in real graphs

Contracts containing computed defaults now compile through the same structural driver into Vortex
control-flow graphs. Literal/alias-only contracts retain the atomic descriptor path. The graph
backend covers unions, optionals, nested shapes/refinements and generic streams, with exact
cardinality checks, staged stream captures and replacement on repeated aliases. Default expressions
compile in the original lexical environment with the actual nested owner. Present fields skip their
fallback entirely, including invalid runtime expressions. Path/call defaults preserve full ordered
results and duplicates; direct aliases keep their established zero/one constraint. Literal fields
inside graph contracts use lazy `VqlBoxScalar` so successive uses retain distinct identities.

`VqlFieldProbe` resolves dimension names at runtime under scoped rollback, then reads one hop.
Direction remains a full-width signed choice; names are bounded and charged before materialization.
A local savepoint covers dimension/proxy minting and reference publication. `Assert` accepts an
optional text diagnostic for dynamic alias-cardinality errors, preserving its one-input behavior.
Shared continuations, `ScopedChoice`, `ChoiceDepth`, `Cut`, `Fail` and direct graph branches
implement commit and rollback-on-success policies without nested VM runs, retained ASTs or matcher
replay. Return validation rolls temporary effects back on both outcomes and preserves named
postconditions.

Inline parameter annotations/targets, return promises and `for` destructuring use the helper. The
existing eligibility refusal for computed-default outer definitions deliberately remains: those
functions compile inline, and affected outer recursion still refuses explicitly. Eligible closed
helpers can execute and recurse through `InvokeFrame`. This stage does not establish native-frame
computed-default parity or sealed mesh-artifact support; mesh Vquery source compilation works.

A configurable query-wide budget defaults to 65,536 graph emission units. Every compiler opcode mint
passes one budget-aware helper, including labels emitted by ordinary path/expression helpers;
appending reserved labels also charges assembly work. Units conservatively count construction work,
not exact instructions. Default compilation borrows the unchanged lexical environment instead of
cloning every binding. Runtime request failure now aborts owned scopes before closing its cursor,
restoring arena and VM maps without running alternatives, refunding fuel or pruning caller choices.
Raw VM stops remain resumable.

Validation: full `make -j$(nproc)` and all 612 affected tests from 58 suites passed. Cases cover
lazy false-valued fields, nested owners, prematch aliases, fresh literal identities, union rollback,
generic success rollback, multi-result defaults, dynamic alias refusal only on absence, return
errors, request recovery, loop BindEmpty semantics, helper recursion, deliberate outer-recursion
refusal, pure-arena export/reload after source VM/AST destruction, and host/mesh Vquery sources.
Native tests cover signed probes, bounded/fragmented names, partial-work rollback, scope abort and
raw-cycle resumption. Earlier string/addition/path refusal tests now assert successful values and
identity. A real isolated headless CLI returned `lazy`, with `VQL_FIELD_PROBE` and `INVOKE_FRAME` in
its disassembly. Independent Luna architecture and senior/profiler gates accepted the bounded
implementation subject to these validations. No speedup or full-suite pass is claimed.

Logs: `/tmp/vql-lazy-contract-build.log`, `/tmp/vql-lazy-contract-tests.log`,
`/tmp/vql-lazy-contract-cli.log`, `/tmp/vql-lazy-contract-format.log`.

## Iteration 24: native frames for computed structural contracts

Eligible named definitions now compile computed field defaults inside reusable native frames. Frame
parameter matches and return promises use the shared contract graph helper; captures, iterators,
choice depths and nested-call results become activation locals through the existing output tracking.
Return promises validate the completed result stream. Parameter mismatches still return empty before
checking the return promise. The eligibility scanner removes only the blanket computed-default
refusal: prematch lexical scope, earlier completed parameter captures, transitive callee
eligibility, malformed-AST checks and query-wide budgets remain intact.

Computed defaults can call the same or mutually recursive definitions without static expansion.
Frame/backing, scoped-choice snapshot/depth, graph-emission and execution limits remain active;
scoped-choice limit failures report the structured Budget category. Request cleanup restores pending
scopes and closes activation maps. No new frame ABI, runtime AST evaluator or matcher was
introduced.

Validation: full `make -j$(nproc)` and all 623 affected tests from 59 suites passed. Eleven new
cases prove outer `InvokeFrame` instructions for path/call/addition defaults, prematch free
captures, earlier parameter aliases, generic success rollback, exact nested owners and
caller-context restoration, fresh literal identities, parameter filtering, self/mutual default
recursion, bounded failure/recovery, lazy named return-contract errors, recursive export/reload
after source destruction and host/mesh Vquery source routes. Independent Luna architecture and
senior/profiler source gates accepted this bounded stage subject to validation; their requested
return/mutual-recursion gaps now have passing cases. No speedup or full-suite pass is claimed. Full
§4.12 remains incomplete.

Logs: `/tmp/vql-lazy-frames-build.log`, `/tmp/vql-lazy-frames-tests.log`,
`/tmp/vql-lazy-frames-cli.log`, `/tmp/vql-lazy-frames-format.log`.

## Iteration 25: refuse oversized arena content runs before mutation

Arena content setters now return `ArenaRefusal::ContentRunTooLong` when an input run exceeds the
physical `CellSlot::spanCount` width. The check precedes copy-on-write shadowing and trailing, so
refused writes leave content, headers, counters and rollback state intact. Quote minting checks the
same bound before consuming work or allocating a slot and throws `std::length_error`, consistent
with its direct-return API. The private content writer also checks defensively. The shared physical
bound derives from the count member's type beside `CellSlot`; no wire or store format changes.

Boundary tests preserve all 65,535 entries and refuse 65,536 on owned/base cells, compare copied
header/content snapshots, retain later valid-write rollback and prove rejected quotes consume no
mint work. Independent Luna architecture and senior/profiler reviews accepted this count-only stage.
This is constant-time preflight, with no speedup claim. Persistent folding/writer checks, aggregate
32-bit offset capacity and prefix hydration remain follow-up work; this stage does not claim general
mutation-contract enforcement or full §4.12 completion.

Validation: full `make -j$(nproc)`, repository formatting with clang-format 19 and all 688 affected
tests from 60 suites passed, including the persistent Manifold baseline and four new arena boundary
cases. Logs: `/tmp/vql-span-count-build.log`, `/tmp/vql-span-count-tests.log`,
`/tmp/vql-span-count-format.log`.

## Iteration 26: persistent content-run preflight and fold refusal

Persistent content commits now return `FoldRefusal::ContentRunTooLong` before modifying a run when
its normalized span count cannot fit `CellSlot`. A single pure splice normalizer supplies both
folding and Store-side preflight. Its coalescing pass keeps ordered addresses and removes empty or
contiguous pieces with one scan. Insert/Delete/Rearrange/Splice/SetValue routes publish target
metadata only after successful content commit. Multi-span Transclude prepares its complete result in
temporary storage and commits once, avoiding partially published target content on refusal.

Local `spliceCellSpan` checks capacity before authoring an operation. `spliceCell` checks its exact
candidate address under the permascroll's existing append mutex before publishing bytes, through
`appendChecked`; the callback is borrowed and never retained. Parent-manifold preparation stays
outside that lock. A prepared operation helper preserves scalar inference without duplicating the
capacity check. Both splice writers reject offsets/removal lengths outside the operation's 32-bit
wire fields before append, so preflight and folding cannot disagree through truncation.

Preflight adds a normalized-run allocation for Store splices; no performance improvement is claimed.
Multi-span transclusion retains repeated normalization work. This stage concerns content count
refusal in folding and these two local writers. General mutation contracts, other writer failure
atomicity, aggregate arena offset capacity and prefix hydration remain open; no full §4.12
completion follows.

Validation: full `make -j$(nproc)`, repository formatting with clang-format 19 and all 725 affected
tests from 68 suites passed. Six new cases cover exact-limit normalization, long contiguous runs
with empty pieces, fold refusal without target/by-ref publication, Store span/text/offset refusals
without operations or bytes, multi-span transclusion with an earlier fitting fragment, and checked
append candidate identity/exception rollback. Existing permascroll suites join the prior VQL/VM and
persistent Manifold matrix. Independent Luna architecture and senior/profiler gates accepted the
bounded implementation and coverage; no full-suite or speedup claim is made.

Logs: `/tmp/vql-persistent-spans-build.log`, `/tmp/vql-persistent-spans-tests.log`,
`/tmp/vql-persistent-spans-format.log`.

## Iteration 27: validated prefix baselines and non-destructive transfer refusal

Arena prefix export and hydration now return typed failures. Export validates the source mark,
restores its slot headers into the copied prefix, and normalizes omitted undo journals to a fresh
baseline: `trailSize` and dimension-journal count are zero. A snapshot whose original mark had a
nonzero trail count can be hydrated, changed and released without synthesizing dummy undo entries.
Tracked quote state created after an earlier mark can be excluded; identity state required by the
prefix but absent from its schema refuses explicitly. Untracked federation/bound-dimension/
provenance context remains conservatively unsupported.

Hydration checks normalized floors, live counts, disjoint CSR runs, scratch bounds, local birth and
last-operation identities, base shadows, shadow-order uniqueness and dimension/link references. It
prepares validation maps, payload copies and the floor locally, then swaps them into the arena. A
malformed snapshot leaves an existing destination untouched; existing marks and incompatible
identity state refuse. Export, encoding, hydration and decoding share the existing APF2 count caps.
APF2 and spark readers reject trailing data, duplicate dimension names, unknown model/presence tags
and inconsistent model/prefix combinations.

A shared checked calculator accounts for the entire encoded APF2 payload, including headers, arrays
and names, before export copies or choice removal. That total must fit the enclosing spark's 32-bit
length field; fitting each section separately is insufficient. Encoders also check string and path
lengths before narrowing. Synthetic scalar counts test the exact framing boundary without allocating
a multi-gigabyte payload, and a small real encoding checks the size calculation.

The OR-parallel producer prepares a required shadow prefix before removing the selected choice.
Export or Dewey-path wire-limit refusal preserves that choice and its mark; it never converts a
failed shadow export into oracle recomputation. Workers reject missing or refused prefixes before
program binding or VM execution. The existing Core-before-hydration bootstrap is retained.

Validation performs range sorting, map construction and full prefix copying; this stage makes no
speedup, byte-memory bound or complete parallel-parity claim. General aggregate mutation capacity,
serialization of omitted federation/provenance state, worker Core bootstrap identity and remaining
frame/stream/mesh lifecycle parity remain open. Full §4.12 is incomplete.

Validation: full `make -j$(nproc)` and all 736 affected tests from 68 suites passed. Eleven new
cases cover populated-destination refusal, required versus post-mark quote state, base-shadow
round-trip and duplicate rejection, total framing limits and real encoded size, malformed transport
models, path-limit refusal before stealing, and worker callback suppression. The existing empty-run
prefix regression now starts from a genuinely nonzero source trail and checks nested release
restoration. Independent Luna architecture and senior/profiler gates accepted the final code and
coverage; a fresh Luna writer completed the base-backed case after the previous writer reached a
usage limit. No full-suite or speedup claim is made.

Logs: `/tmp/vql-prefix-integrity-build.log`, `/tmp/vql-prefix-integrity-tests.log`,
`/tmp/vql-prefix-integrity-format.log`.

## Iteration 28: reusable computed return validators

Computed return promises now compile a capture-free validation graph into a reusable native frame
per static check site. The frame receives original subjects, context and referenced lexical streams,
then returns a Boolean Cell port. The caller preserves its original result identities and raises the
existing named return-contract error on false. Noncomputed promises retain `RequireContract`;
attached parameter pipelines retain the existing inline graph route. No runtime AST execution or
second matcher is introduced, and the existing frame ABI/export machinery is reused.

The shared contract driver runs validators with rollback on success and failure. Present fields skip
defaults; absent fields evaluate their compiled defaults against the current owning cell. Frozen
lexical inputs preserve order, duplicates, explicit emptiness and unavailable aliases. They freeze
references rather than cell payloads: a later cursor rechecks changed owner links instead of reusing
a cached default result. Return captures never replace caller bindings. Native recursion and nested
default calls retain their isolated active-call state.

A first profile exposed eager copying of unused lexical streams. The compiler now discovers actual
input references in the completed validator graph, prunes unused ports/locals, and emits caller
normalizers only for retained inputs before invocation. A regression with 200 unused binding names
passes with a 128-local frame limit. Temporary candidate slots and compile-time environment copies
still occur before pruning; final frame limits are not a complete bound on that preparation cost.

The opt-in probe runs 32/128 checks with zero/1,000 unused references. Before pruning, 1,000 unused
references increased maximum stream-append arity to 1,000; after pruning every measured case has
arity one, two validator arguments and 54 locals. Validator backing grows by 1,728/6,912 cells for
32/128 calls. Post-pruning single observations were 7,378/6,994 microseconds at 32 calls and
29,409/25,734 at 128 calls (zero/1,000 unused references). Inline controls were 4,714/5,029 and
18,554/18,011 microseconds. These observations establish removal of the unrelated runtime copy
dependency, not a general speedup; validator invocation still adds overhead.

Validation: full `make -j$(nproc)`, repository formatting with clang-format 19 and all 745 affected
tests from 69 suites passed. Nine new cases cover Boolean/result identity, absence-only defaults,
optional empty and false results, lexical streams/capture isolation, fresh-cursor current-owner
checks, recursion/failure recovery, exported graph reload after source destruction, pruned frame
limits, and host/mesh source plus pipeline routes. A headless Vquery CLI returned `validator`
through an emitted `#INVOKE_CONTRACT_VALIDATOR` instruction. Independent Luna architecture and
senior/profiler gates review this bounded stage. Active mutation-contract registration, dispatch and
atomic enforcement remain unimplemented; §4.12 remains incomplete.

Logs: `/tmp/vql-validator-frames-build.log`, `/tmp/vql-validator-frames-tests.log`,
`/tmp/vql-validator-frames-format.log`, `/tmp/vql-validator-frames-cli.log`,
`/tmp/vql-validator-frames-profile-before.log`, `/tmp/vql-validator-frames-profile-after.log`.

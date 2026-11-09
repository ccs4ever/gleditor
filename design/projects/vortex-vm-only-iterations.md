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

## Iteration 29: cursor-owned guards and asynchronous graph validation

`PushGuard`, `PopGuard` and `CheckGuards` provide an explicit graph-level guard protocol using the
existing `VORTEX_FRAME/1` descriptor. Push validates port arity and a single Cell result port,
freezes resolved argument identities and returns a fresh opaque cell token. Cell ports retain
zero/one cardinality; Stream ports retain emptiness, order and duplicates. Pop accepts only the
current activation's top token. Native activations have unique identities that are not refunded by
return or backtracking; returning trims callee guards while preserving the caller baseline.

Check snapshots the current guard order and runs each validator through the same prepared native
frame-entry helper as ordinary InvokeFrame. ReturnFrame resumes the checker, which requires an
actual Bool cell and reports a named type-contract failure on false. No nested VM run, runtime AST,
second matcher or cached default-result witness is introduced. A failed later dispatch discards its
pending continuation; retry checks the current subjects from the first guard again. Recursive checks
through the same opcode refuse using activation ownership rather than consuming an incomplete
result.

Guard records and pending continuations use immutable shared payloads. Choice snapshots retain both,
include them in scoped snapshot accounting, and restore them with frame/register state. Cursor close
clears private result mappings and current guards; callers still own the existing choice-abort/cut
protocol before nonresumable shutdown. Halt/falloff with active or pending guards refuses.
Per-cursor limits cover guard count, captured references and diagnostic text, including
fragmentation. Capture accounting conservatively includes active and pending references; it is not a
byte-memory bound. Successful token, scratch and frame allocations retain the arena's cumulative
allocation semantics.

Export relocates PushGuard's frame descriptor records and refuses live or choice-retained guard
state before Store operations. Sealed Vortex mesh artifacts and raw stdlib guard calls refuse
explicitly. Conditions and parameter pipelines reject guards before effects; pipeline preflight
stops at Halt/Return like execution and shares its existing instruction cap. Vquery source and
existing ordinary frame routes retain their compiled execution paths.

This stage has a real consumer: tests compile a computed return validator, then install and execute
its descriptor through runtime guards. Compiler-generated parameter activation and atomic mutation
brackets are still absent. CheckGuards itself owns no semantic mutation transaction; temporary
matching effects roll back through the shared compiled contract driver. Automatic mutation
enforcement, nested effectful validation scheduling and the remaining §4.12 work stay open.

The disabled opt-in probe measures 1/16/64 guards and zero/1,000 captured references per guard on an
AMD Threadripper 1950X, Clang `-O3 -g`. Each row is one observation; the control still performs
Push/Pop and capture copying, and omits checking. At 64 guards, checking took 1,077/1,702
microseconds for zero/64,000 total captures versus 327/707 for the control. Check/control used
321/128 VM cycles and 384/64 arena cells: four frame locals, one result scratch cell and one token
per checked guard. VM cycles do not count every C++ traversal. These measurements describe this
fixture's overhead and allocation accounting, not a latency improvement or general benchmark.

Validation: full `make -j$(nproc)` and all 760 affected tests from 70 suites passed. Fifteen new
cases cover compiler-generated validators, frozen streams, named failure order, token ownership,
choice/pending restoration, cancellation, cap/fuel refusal and retry, strict result ABI, unsupported
contexts, callee cleanup, fresh-VM export, recursive checks, current-owner retry after
second-dispatch refusal, and pipeline termination. Repository formatting with clang-format 19 and
Markdown lint passed. A headless Vquery CLI returned `validator` through the shared frame path.
Fresh independent Luna architecture and senior/profiler gates accepted this bounded stage.

Logs: `/tmp/vql-guards-build.log`, `/tmp/vql-guards-tests.log`, `/tmp/vql-guards-profile.log`,
`/tmp/vql-guards-format.log`, `/tmp/vql-guards-cli.log`.

## Iteration 30: active parameter guards and atomic source mutations

Supported typed function parameters now install runtime guards after each completed successful
match. Validator preparation freezes that contract's original lexical streams, subjects and context
before matching publishes captures. A completed type match publishes captures before preparing the
synthetic destructuring match, preserving their existing ordering. Earlier guards are active while
later parameter defaults execute. Variadic contracts retain their per-actual checks. Parameter
mismatch still returns empty without executing the body or return promise.

Inline calls use owned guard scopes and close them on success and partial-filter exits. Native
frames retain recursive execution and validate their own participating guards before return. A guard
participates only when a source mutation runs while it is active. Scope exit validates those
obligations, removes them from all enclosing groups, then retires the guard; an expired helper's
type does not constrain later caller writes. Caller guards remain eligible for repair at an outer
weave boundary. No cached default result replaces revalidation of the current owner.

Source value replacement, complete creation lists with initialization/attachment, clone fanout and
clone tails, and explicit weave effect sequences now emit mutation groups. Nested groups defer their
checks to the enclosing group, allowing intermediate invalid state to be repaired. The outer end
runs the shared validator frames asynchronously. Contract failure restores the exact owned choice
checkpoint before reporting the named error, preserving caller choices and earlier committed groups.
Group checkpoints use monotonic identities; ordinary Cut/Fail cannot consume them or replay a group.
Snapshots include groups, scopes, participants, pending validation, registers and activation state;
rollback restores cursor, stream and branch-path state as well. Fuel and successful frame
allocations remain cumulative rather than being refunded.

Per-call cycle exhaustion remains resumable. A separate cumulative group-cycle cap, scope/group
depth and participant caps bound this protocol; guard captures and scoped snapshots retain their
existing logical limits. Scope/group limit failures are classified as frontend Budget errors.
Cancellation and terminal errors unwind owned groups, and canonical close clears scope/pending
state. These counts are not a whole-heap or every-allocation bound.

The VM serializes execution while another cursor has current guards, scopes, pending validation or
an active group, closing the scalar-only cursor alias bypass. Historical choice snapshots alone do
not extend an expired guard's lifetime. Mutating pre/postcondition clauses refuse before outer
instruction effects while guarded state is present; parameter pipeline preflight likewise checks
unsupported combinations before execution. Export refuses live/restorable scope/group state and
relocates compiled graph references. Sealed Vortex mesh artifacts and raw stdlib guard/group routes
remain explicitly unsupported; Vquery source routes execute the compiled mutation graphs.

User mutation during a pending guard validation still refuses before its first source effect. A
bounded scheduler for effectful nested validation remains unfinished. This stage also exposes an old
compiler defect by explicitly refusing `PathExpression::shapeProjection`, which was previously
silently ignored. Shape field weaving/projection, return-field construction, remaining typed binding
and functor/variadic/lambda work, session and route/lifecycle parity still prevent §4.12 completion.
There is no direct evaluator fallback or native-recursion-to-inline substitution.

The disabled smoke probe compiles once per fresh case and separates compilation from execution. It
performs 32/128 repeated typed or untyped `put` calls on one Int cell using `value(7)`; the control
still includes scopes and mutation groups. On the Threadripper 1950X with Clang `-O3 -g`, single
cold typed/untyped observations were 5,637/2,093 microseconds at 32 calls and 23,522/7,994 at 128.
Compile observations were 509/155 and 342/237 microseconds. VM cycles were 3,141/1,637 and
12,549/6,533; runtime arena growth was 736/64 and 2,944/256 cells. Emitted opcodes were 215/75.
These rows describe the whole fixture and added contract costs, not isolated per-call latency,
stable benchmarks or a speedup.

Validation: full `make -j$(nproc)` exited zero and all 789 affected tests from 73 suites passed.
Twenty-nine new enabled cases cover source mutation/filter/alias/default behavior, full creation and
clone groups, weave repair and rollback, native recursion, caller/callee lifetimes, exact choice
ownership, cancellation/resume/hard-cycle budgets, mutating condition refusal, scalar cursor
isolation, and source-free exported graph reload. Host and mesh Vquery source tests cover success,
named failure and later recovery. A real headless Vquery CLI returns `7` through emitted PushGuard
and BeginMutationGroup instructions, and rejects a string write with the parameter's named
TypeContractError. Independent fresh Luna architecture and senior/profiler gates accepted this
bounded implementation. Repository formatting with clang-format 19 passed.

Logs: `/tmp/vql-mutations-build.log`, `/tmp/vql-mutations-tests.log`,
`/tmp/vql-mutations-profile.log`, `/tmp/vql-mutations-cli.log`, `/tmp/vql-mutations-format.log`.

## Iteration 31: static shape projection graphs

Static path shape projections now preserve each reached owner while executing masks and scalar
literal assignments as real Vortex graphs. The compiler normalizes ordered wildcard resets and
per-direction overrides into final keep bits. Empty shapes sever every incident link, the universal
wildcard preserves unmentioned links, and explicit exclusions sever both directions. Signed and
bidirectional inclusions retain their specified directions. Assignment targets are freshly boxed on
every execution, preserving scalar kinds and preventing reciprocal-link theft between owners.

The native `VqlMaskLinks` primitive snapshots the runtime owner's dimension run and unlinks denied
edges through the arena's reciprocal-link API. It validates its graph operands before mutation,
preserves full-width dimension identities, charges arena work and uses an arena savepoint. Every
projection is enclosed in a source mutation group, so active parameter contracts see the completed
projection and failed validation restores the original edges. Export relocates each dimension
operand. Configurable rule and owner-width limits bound the primitive; query compilation also limits
projection rules across the compilation.

This stage supports local owners, static canonical dimensions, wildcard algebra and scalar literal
assignments. Foreign owners and quoted occurrences refuse explicitly. Dynamic shape rules,
path/call/alias targets, nested shapes, negated assignments and bidirectional assignments remain
unsupported. In particular, future nonliteral targets must read the original owner before masking
can remove their input links; no direct evaluator or AST fallback supplies those semantics. Function
frame eligibility remains conservative and uses existing inline graph lowering for projections.
Return-field construction remains a separate unfinished operation. Full §4.12 remains incomplete.

Value-expression parsing also now retains `$owner{...}` inside an expression-bodied function.
Previously it parsed the bare variable as the function body and left the projection outside the
function scope. An AST regression checks placement, and a typed projection regression verifies that
the resulting in-function mutation is rejected and rolled back when its contract fails.

Validation: full `make -j$(nproc)` exited zero; all 807 affected tests from 75 suites passed.
Eighteen new enabled cases cover runtime masks and exact scalar boxing, malformed graph operands,
resource refusal without partial unlinking, guard rollback, parser scope, source-free export/reload,
and host/mesh Vquery source routes. The real headless CLI returns `7` through emitted mask, box and
link instructions, and rejects a typed projection with the named parameter TypeContractError.

The opt-in mask profile records single cold observations, not a speedup or stable latency claim. At
32 owners and 4/16 incident dimensions, compile times were 156/48 microseconds and run times
408/373; both ran 229 VM cycles with 72 compile cells and 32 runtime cells. At 128 owners, compile
times were 118/138 microseconds and run times 1,290/1,496; both ran 901 cycles with 168 compile
cells and 128 runtime cells. Every case emitted 11 opcodes. These are whole-fixture counts,
including mutation groups; arena fuel also charges work within the native mask primitive.

Logs: `/tmp/vql-projections-build.log`, `/tmp/vql-projections-tests.log`,
`/tmp/vql-projections-profile.log`, `/tmp/vql-projections-cli.log`,
`/tmp/vql-projections-format.log`.

## Iteration 32: prepared singleton projection targets

Projection assignments now accept lexical cell aliases and a bounded read-only path subset. Each
nonliteral target compiles to normal graph traversal and a normalized stream; the shared required
`Cell` contract checks its complete cardinality before selecting its sole reference. Empty or
multi-cell results raise a named target TypeContract error rather than silently choosing an element.
Every nonliteral RHS for one owner occurrence is prepared under that owner before any masking or
assignment. The source mutation group begins before preparation and rolls back the current
occurrence on failure. No earlier listed literal assignment is published if a target check fails.

Existing-cell targets use §4.7's unconditional fresh clone per owner occurrence. Projection and
`link()` share the extracted graph clone-generator helper, preserving existing target backlinks,
master identity and exact typed content without rendering/reboxing it. Literal targets retain fresh
scalar boxing. Owner results preserve order and duplicates. Preparation is per occurrence, not a
whole-input snapshot: a repeated owner's later path reads see its graph after the preceding
committed occurrence. An explicit outer weave retains its normal wider rollback boundary.

The read-only path subset has static signed dimension steps, static clamps, dereference and
read-only placements. It excludes calls, arithmetic, nested projection, clone tails, predicates,
versions, captured functors and yield suffixes; known attached pipelines refuse before effects.
Dynamic shapes, nested targets, negated/bidirectional assignments and foreign/quoted owners remain
unsupported. Nonliteral assignment on `d.clone` refuses before effects because an ordinary
attachment would consume the generated clone's master backlink. The pre-existing
`link(d.clone, target)` collision remains separate unfinished work. Projection frame eligibility
remains conservative.

Testing also exposed a parser gap: a separator after a tail projection was interpreted as a new
statement and lost the projected stream context. The tail-only AST cannot encode that ordering, so
`/`, `.`, `=>` and `<~` continuations now refuse explicitly. Bind the projected result with `let`
before traversing it; native ordered projection/traversal remains a separate milestone. No AST
execution or direct evaluator fallback supplies an unsupported case.

Validation: full `make -j$(nproc)` exited zero and all 815 affected tests from 76 suites passed.
Eight new enabled cases cover per-owner pre-mask reads, multiple RHS preparation, alias/path
cardinality rollback, exact Int64/Double clone content and preserved target backlinks, duplicate
owners, guarded replacement, refusal before effects, source-free export/reload and host/mesh Vquery
source routes. Parser coverage checks the explicit continuation refusal. The real headless CLI
returns exact `9007199254740993` through a prepared path target and rejects an empty target with its
named required-type diagnostic and exit status 1.

The opt-in profile records single cold observations with no speedup claim. At 32 owners,
shared-alias/owner-path compile times were 294/106 microseconds, run times 5,590/2,900, VM cycles
2,757/1,797, compile arena cells 146/234 and runtime arena cells 64/64. At 128 owners, the
corresponding values were 150/178, 47,889/10,085, 35,589/7,173, 242/330 and 256/256. The fixtures
emitted 26/50 opcodes. These are whole-fixture counts, including group tokens; unique fresh target
clones are checked separately. The shared-alias fixture repeatedly walks a growing clone rank, an
observed cost to address after semantic correctness rather than an optimization claim. Full §4.12
remains incomplete.

Logs: `/tmp/vql-projection-targets-build.log`, `/tmp/vql-projection-targets-tests.log`,
`/tmp/vql-projection-targets-profile.log`, `/tmp/vql-projection-targets-cli.log`,
`/tmp/vql-projection-targets-format.log`.

## Iteration 33: reusable native projection frames

Eligible named functions now execute supported projections through reusable native frames, including
actual self-recursion, nested calls and caller-bound free aliases. The compiler and eligibility
scanner share a flat syntax classifier for static masks, literals, aliases and bounded read-only
paths. Binding, pipeline, arena and resolved-dimension checks stay in the compiler; target
preparation, exact-one contracts, clone generation and mutation groups keep their existing graph
lowering. No VM opcode, frame port layout or runtime ABI was added.

The scanner walks every projection operand through its existing capture and traversal machinery
before classifying eligibility. Cycles, depth and query-wide node exhaustion remain hard errors,
even when the enclosing syntax is unsupported. Call graph propagation and pre-match scopes remain
unchanged. Generic first-class shape values are explicitly refused after traversal, including empty
and wildcard-only shapes; they do not gain eligibility from the projection-specific path. Explicit
weave definitions, functors, variadics, lambdas and the other previously excluded forms remain
outside this native subset.

The Host recovery test exposed a separate dimension identity defect. Name lookup started at the
stable `d.dims` directory cell, but minting searched and appended from home. An exclusive home
projection could detach only that connector, leaving the directory intact; subsequent new names then
became undiscoverable and could receive multiple identities. Minting now uses the same stable
directory root as lookup while retaining base-dimension precedence. It does not restore any links
that the caller removed. A focused core regression and the unchanged Host recovery case cover the
correction; this is not a general repair of projected core-home topology.

Validation: full `make -j$(nproc)` exited zero and all 824 affected tests from 77 suites passed.
Nine new enabled cases prove named function InvokeFrame descriptors (separate from validators),
recursive mutation and distinct scalar cells, typed filter and rollback behavior, nested free
captures with exact Int64/Double clone content, explicit context/default alias handling, source-free
recursive export/reload, Host and mesh Vquery source recovery, conservative refusal propagation, and
hard AST budgets/cycles. A projection-specific cyclic RHS also fails before syntax refusal. The real
headless CLI builds first→second→third, recursively projects through the same native walk graph and
returns `third`; an invalid native projected parameter raises its named contract error and exits 1.

The four-point profile uses cold single observations, without a speedup claim. At 32 owners,
untyped/Cell-typed parameters compiled in 411/216 microseconds and ran in 3,054/6,994; VM cycles
were 2,021/3,973, compile arena growth 300/589 and runtime growth 448/1,440. At 128 owners, the
values were 223/304, 11,749/30,124, 8,069/15,877, 396/685 and 1,792/5,760. The function descriptors
contained 12/22 locals and generated 70/150 opcodes at either owner count. Whole-fixture arena
growth includes backing, guard/group and result cells; it is not an isolated allocation or
whole-heap bound.

Foreign projection owners, ordered projection/traversal, nested/computed projection targets,
effectful nested validation, remaining callable forms and full frontend/parallel lifecycle parity
remain unfinished. Sealed Vortex mesh artifacts retain their explicit descriptor refusal. Full §4.12
remains incomplete.

Logs: `/tmp/vql-projection-frames-build.log`, `/tmp/vql-projection-frames-tests.log`,
`/tmp/vql-projection-frames-profile.log`, `/tmp/vql-projection-frames-cli.log`,
`/tmp/vql-projection-frames-format.log`.

## Iteration 34: reciprocal proxy-link updates and capacity refusal

Arena link replacement now resolves inherited foreign occupants to canonical proxies before planning
reciprocal clears and new endpoints. A fixed four-side plan is shared with raw local proxy-index
bookkeeping. Capacity checks use cell references and authoritative local/base rows before creating
copy-on-write shadows; exceeding the CellSlot link-count field's 65,535-row bound throws a numbered
length error rather than silently writing only one end. Dimension references retain their full
width. Ordinary local updates add no savepoint; foreign resolution uses a nested savepoint that
restores proxy registration and graph changes while retaining spent work budget.

Proxy construction owns a savepoint across dimension creation, minting, index attachment and cache
publication. Its rollback journal precedes map publication. Raw index append avoids recursive
foreign resolution and supports a base-owned store representative, including home. Rollback's index
tail reconstruction is traversal-bounded; this is not a universal cycle or allocation-failure
guarantee. General heap-allocation exception safety of existing arena journals remains unaudited.

Internal `d.store-refs` can collide with authored source topology. Until proper axis isolation is
implemented, proxy creation conservatively refuses incident source edges on its currently mapped
index axis, or on a literal same-named source axis when unmapped. It also checks an unshadowed proxy
tail's inherited positive edge using that tail's own source space. Name lookup detects a collision
only; it creates no dimension binding. Arbitrary binding changes after proxy edits remain an open
lifecycle issue. Foreign projection masks still refuse: `dimensionsOf(proxy)` is not a complete
effective-link snapshot after partial shadowing. No privileged storage-axis semantics or
foreign-mask support follows from this kernel stage.

Validation: full `make -j$(nproc)` passed, and all 838 affected tests from 77 suites passed.
Fourteen new enabled tests cover inherited unlink/replacement, cross-space identity, partial
direction overrides, self-links, base-owned roots, capacity refusal without new shadows, proxy
factory rollback/retry, source-axis collisions and unrefunded work. Two existing foreign-stream
fixtures now assert the reciprocal rank split and reconcile a late-bound clone rank with the
coordinator's existing representative before constructing a cycle. The real headless CLI returns
exact `9007199254740993` through a native projection function.

A broader overlay/federation/quotation/promotion sweep ran 136 tests: 127 passed, one swarm case
skipped, and eight failed at unchanged unsupported frontend routes. Three VLace cases report
unimplemented VM lowering; five VQL cases exercise unavailable `find`, `contains`, `delta`, or the
unknown-function exception-class expectation. These remain unfinished specification work, not a
passing full suite.

The opt-in profile reports twelve cold single observations on the Threadripper 1950X with Clang
`-O3 -g`; setup and assertions are outside timing. For 32 link operations, local/existing-proxy/
missing-proxy times were 4/27/59 microseconds; unlink times were 2/18/57. At 128 operations, the
corresponding times were 16/96/255 and 11/70/206. Arena cell growth was zero for local and
already-materialized neighbors and exactly 32/128 for missing neighbors. These are slot counts, not
heap-allocation counts; local means no extra savepoint/proxy work, not unchanged latency. Source
immutability, reciprocal edges, canonical identity and zero outstanding marks are checked. No
speedup claim follows. Full §4.12 remains incomplete.

Logs: `/tmp/vql-proxy-links-build.log`, `/tmp/vql-proxy-links-tests.log`,
`/tmp/vql-proxy-links-profile.log`, `/tmp/vql-proxy-links-overlay.log`,
`/tmp/vql-proxy-links-cli.log`, `/tmp/vql-proxy-links-format.log`.

## Iteration 35: bounded effective-link snapshots

`ArenaManifold::snapshotLinks` now returns an owning view of effective local and foreign edges.
Local endpoints carry CellRef; inherited endpoints carry exact `(space, source CellRef)` identities,
even when their canonical proxies do not exist yet. Discovery mints no proxies and changes no cells,
shadows, caches or links. Proxy rows merge inherited CSR topology with each direction's explicit
local override, including empty overrides. Ordinary local/base rows and quotation-induced local rows
retain their own topology. Empty result rows are omitted and axes retain full-width identities in
deterministic order. This is an arena read primitive; no AST evaluation or second contract matcher
is involved.

Candidate discovery includes the base dimension rank, registered arena axes, valid bound keys,
locally addressable raw binding members and owner rows. It walks the base rank directly rather than
calling the lazy dimension cache. Mandatory positive limits bound candidate attempts, nonempty
directed endpoints and work, including deduplication, sorting, mapping, CSR scans and filtering.
Work also consumes active arena fuel, including failed requests; fuel exhaustion returns the typed
limit refusal. Owning result vectors still allocate. No universal heap or concurrency guarantee
follows.

The shared checked mapper gives an exact arena key authority over raw-member aliases, including an
exact key without a member for the requested space. Otherwise raw aliases must agree on one target
identity or report ambiguity. The existing `dimIn` wrapper retains its one-space name fallback after
checked absence; snapshots never invoke that unbounded lookup. A one-space candidate that would
require name lookup returns `NameResolutionRequired` conservatively, even when another axis already
covers the same source topology: accepting could omit another observable alias. Positive foreign
snapshots currently use explicit mappings in multiple spaces. Missing coverage of any nonempty
source axis refuses the whole request. Multiple view axes for one source axis remain distinct, with
independent directional overrides.

Validation: full `make -j$(nproc)` and all 849 affected tests from 78 suites passed. Eleven new
enabled tests cover no-mint discovery, partial overrides, aliases and equal numeric refs across
spaces, local/quote topology, missing/ambiguous mappings, exact-key precedence, full-width mapping,
bounded refusal and spent fuel. They also retain the foreign-owner VqlMaskLinks refusal. The real
headless CLI still returns exact `9007199254740993` through a native projection function. A broader
consumer sweep passed 128 of 137 tests, skipped one swarm case, and retained the same eight
unsupported VLace/VQL route and exception-expectation failures recorded in iteration 34.

Six cold profile observations on the Threadripper 1950X with Clang `-O3 -g` exclude fixture setup
and assertions. For 32 snapshots, local/unmaterialized-foreign/partially-overridden times were
8/31/42 microseconds, returning 32/96/96 rows. At 128 snapshots they were 22/128/191 microseconds
and 128/384/384 rows. Foreign rows include the filing rank and distinct axis aliases. Every case
grew the arena by zero cells. These are whole-fixture observations, not zero heap allocation or
speedup claims.

Foreign masks remain unavailable until snapshot consumption and mutation preflight are implemented.
Bounded shared name resolution, index-rank mutation/cache lifecycle and the earlier callable and
frontend gaps remain open. Full §4.12 is incomplete.

Logs: `/tmp/vql-visible-links-build.log`, `/tmp/vql-visible-links-tests.log`,
`/tmp/vql-visible-links-profile.log`, `/tmp/vql-visible-links-overlay.log`,
`/tmp/vql-visible-links-cli.log`, `/tmp/vql-visible-links-format.log`.

## Iteration 36: foreign proxy projection masks

VqlMaskLinks now consumes effective snapshots for canonical foreign proxies with explicit
multi-space mappings. Rules, complete source-axis coverage, removals and the owner's filing-axis
guard are checked before mutation. One outer arena savepoint owns all unlinks and any canonical
neighbor proxies they materialize; nested kernel savepoints retain their usual rollback behavior.
The local-owner path keeps its existing behavior. Quotation occurrences and name-dependent one-space
snapshots remain explicit refusals, with no evaluator or legacy name-binding fallback.

The existing 65,536-row cap remains a row cap; checked multiplication supplies twice that cap to the
snapshot's directed-endpoint limit. Configurable candidate and work caps bound snapshot and
index-preflight passes. A planned removal of the owner's exact `d.store-refs` axis refuses before
any unlink, because arbitrary index-rank edits still do not maintain the cached tail. Index edges
remain ordinary visible graph data; no axis is hidden and no promotion exception was added.

Each space now records the actual index-axis identity at attachment. Proxy construction and rollback
use it without rediscovering a label. The checked accessor is non-allocating and noexcept; release
permanently clears an ephemeral cached identity that truncation removed, so a later mint with the
same dense ref and name cannot revive a stale cache. Prefix hydration clears these identities and
federation still refuses prefix export. This does not journal space attachment or establish general
binding-change/attachment rollback parity.

Validation: full `make -j$(nproc)` and all 862 affected tests from 80 suites passed. Thirteen new
enabled cases cover index refusal before effects, ordinary-axis removal, aliases, partial overrides,
quote/name/limit/fuel refusals, and second-neighbor allocation failure after an earlier unlink.
Rollback assertions inspect the restored effective foreign rows, proxy maps, cells, shadows and
marks. Host execution uses a named InvokeFrame over two real attached stores, preserves repeated
owner contexts, and checks exact fresh Int64 assignments and unchanged source operation counts. A
mask graph exported before its foreign stores exist reloads in a fresh VM and receives owners
through ordinary runtime CopyCellRefs inputs; it neither captures nor copies foreign data.

The real headless CLI removes an axis in a native function and retains exact `9007199254740993`. The
broader consumer sweep ran 139 tests: 130 passed, one swarm case skipped, and the same eight
unfinished frontend/exception-expectation cases failed. This remains a partial suite result. Mesh
Vquery SOURCE retains its existing single-base-snapshot execution route, but its API cannot inject
this multi-space proxy context; that route was not simulated by copying data. Sealed Vortex mesh
artifacts still refuse effectful projections and frame descriptors.

Four cold single profile observations on the Threadripper 1950X with Clang `-O3 -g` exclude setup
and assertions. For 32 masks, missing/already-canonical neighbors took 226/126 microseconds and
added 32/0 arena cells; for 128 they took 695/490 and added 128/0. VM dispatch counters were 32/32
and 128/128, not processor-cycle measurements. The fixtures use one source dimension per owner; they
measure repeated fixed-size masks, not large candidate-set scaling. Slots are not heap/RSS
measurements, and these observations establish no speedup.

Whole-operation CPU work is not yet fully bounded by the snapshot work cap. When a missing neighbor
is minted, proxy construction's existing collision diagnostic can call legacy `dimIn` and
`dimensionNamed` and scan a source directory outside that cap. This checks index-axis collisions; it
is not evaluator fallback or implicit binding by the mask. A shared bounded name/collision lookup is
the next prerequisite. General index-rank mutation, remaining callable forms and full
frontend/lifecycle parity also remain open. Full §4.12 is incomplete.

Logs: `/tmp/vql-foreign-masks-build.log`, `/tmp/vql-foreign-masks-tests.log`,
`/tmp/vql-foreign-masks-profile.log`, `/tmp/vql-foreign-masks-overlay.log`,
`/tmp/vql-foreign-masks-cli.log`, `/tmp/vql-foreign-masks-format.log`.

## Iteration 37: shared budgeted dimension-name lookup

Manifold name lookup now has one shared matcher with a bounded, read-only entry point. Both routes
retain registry-first held identities, directory order, exact text and one-hop `d.alias` semantics.
Home is excluded from the dimension directory, and ordinary linear-rank termination remains valid.
The compatibility entry point retains its previous reader behavior and successful cache insertion;
bounded reads neither populate the directory cache nor register derived names. Expanded reader
payloads remain accepted by legacy lookup while the bounded entry point refuses unplanned payload
growth. No new configured-budget exception is introduced through the legacy noexcept arena reader.

The bounded API returns a typed refusal separately from a successful missing name. It checks request
bytes/work before registry hashing, span counts and aggregate declared bytes before reading or
reserving payload storage, and CSR widths before link probes. Work includes repeated alias and
matching passes. Mutable request counters are separate from an optional per-call fuel callback; the
non-owning callback is never retained. Cache hits can bypass directory/span reads but still consume
request-byte and work budget. No AST evaluation is involved.

Proxy construction now shares one budget across explicit dimension mapping, requested-source and
tail-origin name diagnostics, and their source-neighbor probes. Every charged operation also
consumes active arena fuel. Lookup refusal throws inside the existing proxy savepoint, preserving
rollback and spent fuel rather than pretending the name was absent. The new arena limits are
configurable: defaults are 4,096 directory-name visits, 4,096 content spans, 1 MiB of cumulative
request/payload bytes and 16,384 work units. Independent caps can be exhausted earlier than another
cap, and repeated reads count again. This closes iteration 36's legacy name-scan escape in proxy
collision diagnostics; it does not establish a universal whole-VM CPU/heap bound or merge these
limits into the mask's snapshot cap.

Validation: full `make -j$(nproc)` passed, as did all 877 affected tests from 81 suites and 92
additional ZigZag tests from 17 suites. Fifteen new enabled cases cover typed refusals, cache-hit
costs, shared budget depletion, UTF-8 byte accounting, fragmented pre-read refusal, alias hop depth,
duplicate-name order, home exclusion, legacy reader compatibility, read-only behavior and proxy
rollback/retry/fuel. The pre-existing fuel regression's successful retry now supplies enough fuel
for newly metered diagnostics; its one-unit exhaustion/no-refund checks remain intact. The real
headless CLI still removes the requested axis and preserves exact `9007199254740993`.

A 2,048-dimension probe used explicitly enlarged limits to finish a missing-name lookup: 4,098
directory and span visits, 55,149 budget bytes, 55,136 reader bytes, 4,098 reader calls and 71,545
work units. The 13-byte difference is the request key. With the directory cap reduced to 256, the
same probe returned a typed refusal after 256 visits/spans, 3,224 budget bytes, 3,211 reader bytes,
256 reader calls and 4,250 work units. Single raw observations were 200 and 11 microseconds; fixture
construction was outside timing. These are diagnostic counters and observations, not heap,
throughput or speedup claims. The broader sweep passed 132 of 141 tests, skipped one swarm case, and
retained the same eight unfinished frontend/exception-expectation failures.

Name-dependent one-space snapshots still refuse; integrating the shared bounded resolver is a
separate stage. Arbitrary index-rank mutation, general attachment/binding lifecycle, callable forms
and full frontend/parallel parity remain open. Full §4.12 is incomplete.

Logs: `/tmp/vql-bounded-names-build.log`, `/tmp/vql-bounded-names-tests.log`,
`/tmp/vql-bounded-names-zigzag.log`, `/tmp/vql-bounded-names-profile.log`,
`/tmp/vql-bounded-names-profile.xml`, `/tmp/vql-bounded-names-overlay.log`,
`/tmp/vql-bounded-names-cli.log`, `/tmp/vql-bounded-names-format.log`.

## Iteration 38: bounded one-space snapshot name resolution

Effective-link snapshots now attempt the established one-space name mapping after explicit and
shared identity mapping is absent. Arena dimension names come from their recorded name first, or
from bounded content extraction with the existing reader and scratch namespace rules. Source
matching reuses the shared registry-first, exact-text and one-hop alias matcher. Neither name
resolution nor snapshotting publishes bindings, caches, dimensions or neighbor proxies. Multiple
attached spaces retain their explicit mapping requirement.

One lookup budget covers name extraction and all name matches across the snapshot. Its callback
charges the snapshot work cap and active arena fuel once per charged operation. The explicit mapping
path retains its existing snapshot budget. Shared bounded span materialization preflights declared
span counts and byte totals before reading or reserving, while legacy lookup retains its prior
reader behavior. A resource refusal becomes a snapshot limit refusal; a successful missing name
leaves that candidate unmapped. The final source-axis coverage check still refuses an unrepresented
nonempty foreign axis rather than returning a partial view or minting missing candidates.

Vortex callers supply an optional directory view over the core's existing dimension graph. Its
stable `d.dims` root remains usable after home loses its connector. Snapshot traversal charges CSR
scans and candidate attempts before collecting those cells; it neither mirrors names into the arena
registry nor changes the graph. Invalid, foreign or quote directory cells refuse explicitly. Local
and quote-occurrence owners ignore this foreign-view input and retain their local snapshot behavior.

Validation: full `make -j$(nproc)` passed, as did all 890 affected tests from 81 suites and 92
selected ZigZag tests from 17 suites. Thirteen new enabled cases cover real name and alias mapping,
ordinary scratch-text dimension names, complete source coverage, cumulative limits, pre-read
refusal, spent fuel and retry, direct callback refusal, stable Core-directory traversal and genuine
VM/host function-frame routes. The fixtures use an unrelated arena base so base-identity mapping
cannot bypass the name path. Tests assert unchanged source operations, absent published bindings and
exact, distinct Int64 assignments with repeated caller contexts.

The real headless CLI removes an axis through a native function and returns exact
`9007199254740993`. The broader sweep passed 132 of 141 tests, skipped one swarm case and retained
the same eight unfinished frontend/exception-expectation failures. Full repository formatting with
clang-format 19 passed. No new performance or allocation improvement is claimed for this stage;
iteration 37's name probe is not evidence for the new directory traversal. Declared span lengths are
preflighted, while custom readers retain responsibility for their own internal allocations.

Multiple spaces still require explicit/shared identity mappings; missing candidate dimensions are
not minted to satisfy source coverage. Arbitrary filing-rank mutation, general attachment/binding
lifecycle, remaining callable forms and full frontend parity remain open. Full §4.12 is incomplete.

Logs: `/tmp/vql-one-space-build.log`, `/tmp/vql-one-space-focused.log`,
`/tmp/vql-one-space-tests.log`, `/tmp/vql-one-space-zigzag.log`, `/tmp/vql-one-space-overlay.log`,
`/tmp/vql-one-space-cli.log`, `/tmp/vql-one-space-format.log`.

## Iteration 39: reusable untyped variadic frames

Eligible named definitions with a final untyped rest formal now use reusable native frames,
including recursive calls. Actual expressions still compile in caller scope before activation. After
the existing UFCS positional injection, remaining actual streams are combined in argument order into
the existing final Stream port; no tail sends an empty stream. Runtime stream append preserves cell
identity, duplicates and order. Fixed-formal default presence is recorded before packing, so omitted
and explicit-empty arguments stay distinct. Fixed defaults and contract matches retain their callee
scope and formal order. Ordinary calls retain their existing preparation path. As with other native
frames, actual streams are fully prepared in the caller before callee guards run: a fixed-formal
mismatch filters the body, not argument traversal or its execution-budget cost. No exact
fuel-threshold parity with compiled inline calls is claimed. A nonempty positional tail fills all
preceding fixed formals; omitted fixed defaults therefore accompany an empty tail.

This stage changes neither VM opcodes nor the frame descriptor ABI. Eligibility still enforces
query-wide syntax budgets, captures and transitive inline-only callees. Typed rest continues to use
compiled inline graphs: it must check each original actual separately, with preceding callee
defaults, captures, context and guards in scope. Caller-side prevalidation or one check over a flat
rest stream would change those semantics. Defaulted, context-dot, destructured and functor variadic
forms remain unsupported.

Validation: full `make -j$(nproc)` and all 899 affected tests from 82 suites passed; the focused
variadic/recursive/eligibility set passed all 32 tests from four suites. Nine new enabled cases
identify the actual named InvokeFrame, exercise dynamic tails, empty rest, caller capture/default
ordering, nested arguments, UFCS, fixed filtering and return promises, depth refusal/retry, and
Store export/reload after the source compiler and VM are destroyed. Host and mesh Vquery SOURCE
routes preserve exact Int64 values and duplicates. A budget regression confirms a dynamic rest
stream is prepared before a fixed-type mismatch filters the body.

The real headless CLI also executes mutual variadic recursion and returns `7`, exact
`9007199254740993`, then `7`. The broader sweep passed 132 of 141 tests, skipped one swarm case and
retained the same eight unfinished frontend/exception-expectation failures. Repository formatting
with clang-format 19 passed. No performance, allocation or exact cross-route fuel improvement is
claimed. Typed recursive rest still needs a grouped-actual ABI and callee-side matching; functors,
lambdas and remaining §4.12 semantics stay incomplete.

Logs: `/tmp/vql-native-rest-build.log`, `/tmp/vql-native-rest-focused.log`,
`/tmp/vql-native-rest-tests.log`, `/tmp/vql-native-rest-overlay.log`,
`/tmp/vql-native-rest-cli.log`, `/tmp/vql-native-rest-format.log`.

## Iteration 40: grouped typed variadic frame graphs

Eligible typed final named rest parameters now preserve each original actual as its own Stream port.
A frame variant is keyed by the function name and static rest-group count after existing UFCS
injection. Repeated arities reuse their descriptor; recursive calls invoke reserved variant roots
through the VM rather than expanding a recursive body. Zero groups remains distinct from one empty
actual. Ordinary and untyped-rest frames keep their existing port scheme and labels.

Inside the callee, fixed defaults, contracts and bindings precede rest matching. Each group prepares
its own validator and guard in that lexical/context environment, matches with capture publication
disabled, and appends only after a successful match. A failed group returns empty without executing
the body or checking the return promise. The assembled rest name is bound after all groups pass.
Shape captures from variadic types are scanned without publishing them into the body's lexical
scope; caller aliases and pre-match field-default bindings retain their established meaning.
Required aliases used only by a typed rest contract are conditional on a positive group count,
including through wrapper calls and UFCS. Zero-group variants skip those unused requirements;
requirements from fixed parameters, returns and body contracts remain active.

All group normalizers are emitted before InvokeFrame. Normal-context and empty-context control
alternatives may contain separate call sites sharing the same descriptor; the named-variant ceiling
counts unique keys, while the opcode ceiling counts every minted instruction cell.

Variant identity is separate from the base function name used for inline-cycle checks. All variants
remain disconnected native instruction graphs included in disassembly and export. The existing
FrameDescriptor format and VM opcodes are unchanged; the ordered argument ports describe each
variant's groups. Native argument streams still prepare in caller scope before callee guards.

Compilation has independent configurable aggregate policy ceilings: 1,024 named frame variants,
100,000 minted VQL opcode cells and 100,000 finalized descriptor records. Variant reservation,
opcode minting and descriptor serialization charge their respective ceilings first. Descriptor
records count the entry, locals, argument ports and result ports for named and contract-validator
frames. Existing contract-emission and VM per-descriptor limits still apply. These counters do not
bound every arena allocation, operand link, CPU operation or total compile time.

Compiler failure handling now includes terminal Halt/library Return emission, linking and result
construction. Resource refusal returns a failed compilation and the assembly transaction rolls back
without publishing an entry or results; a later compile resets its counters.

Validation: full `make -j$(nproc)` passed, as did all 909 affected tests from 83 suites and all 42
focused variadic/recursion/context/eligibility tests from six suites. Ten new enabled cases cover
per-actual Int versus Stream cardinality, explicit empty groups, descriptor reuse across control
alternatives and arities, typed UFCS, exact identity/order/duplicates, callee and owning-cell field
defaults, conditional zero-group aliases through wrappers/lazy defaults/UFCS, capture isolation,
per-group mutation guards and rollback, changed-arity mutual recursion and source-free Store
export/reload. Host and mesh Vquery SOURCE routes execute the grouped graphs. Compiler-cap cases
cover aggregate records including validators, unique variants, terminal executable/library opcodes,
rollback and same-VM retry.

The real headless CLI runs typed mutual recursion and preserves ordered `7`, exact
`9007199254740993`, `7`. The broader sweep passed 132 of 141 tests, skipped one swarm case and
retained the same eight unfinished frontend/exception-expectation failures. Repository formatting
with clang-format 19 passed. No performance, total-memory or total-compilation-time improvement is
claimed.

Defaulted, context-dot, destructured and functor-valued variadic forms remain unsupported. Functors,
lambdas, ordered projection continuations, remaining mutation/lifecycle/session semantics and full
frontend parity still leave §4.12 incomplete.

Logs: `/tmp/vql-typed-rest-build.log`, `/tmp/vql-typed-rest-focused.log`,
`/tmp/vql-typed-rest-tests.log`, `/tmp/vql-typed-rest-overlay.log`, `/tmp/vql-typed-rest-cli.log`,
`/tmp/vql-typed-rest-format.log`.

## Iteration 41: ordered shape projection continuations

Supported shape projections can be followed immediately by another path operation. The AST retains
the existing terminal `shapeProjection` field and records continuing projections after their anchor
or preceding step. Parser lowering preserves source order and keeps clone tails terminal.
Consecutive projections and unsupported raw-dot continuations remain explicit refusals.

The compiler applies anchor projections after anchor predicates and dereferencing. A step projection
runs after that step's selector, empty-context call alternative, dereferencing, predicates, clamp
and yield selection, before the next step. All positions use the same owner-preserving VM projection
lowering and static-rule validator, including empty streams and nested path steps. No VM opcode or
frame descriptor format changes. Eligible named bodies retain native recursive graphs and exported
instructions execute without their source AST.

Projection target classification refuses paths carrying projections because those paths mutate
rather than read their target. Functor capture likewise refuses the new projection-bearing paths
until captured projection semantics are defined. Eligibility visits all projection positions with
the existing lexical scope, cycle checks and query-wide budgets. Existing projection masks,
singleton target requirements, atomic mutation behavior and contract guards remain in force.

Validation: full `make -j$(nproc)` passed, as did all 922 affected tests across 84 suites and all 50
focused projection/frame/parser tests across three suites. Thirteen new cases cover ordered reads,
anchor and post-step projections, owner order and duplicates, predicates/clamps, keep-yield parents,
pre-mask read-only targets and aliases, atomic failure, parser and legacy AST compatibility, functor
capture refusals, host/mesh SOURCE execution, native recursion and recursive export/reload after the
source AST and VM are destroyed. The real headless CLI returns exact `9007199254740993` through a
recursive projected graph. Repository formatting with clang-format 19 passed; architecture, senior
and resource gates accepted this bounded stage.

The broader sweep retained the same eight unfinished failures and one skip, with 132 passes. An
additional callable audit passed 28 of 40 tests: typed/anonymous path lambdas, missing-context value
lowering, a weave value assertion and `sort_into` account for the 12 remaining failures in that
selection. These are unfinished functionality, not a full-suite pass. Parser recursion has no
separate depth budget before AST analysis; this stage adds no universal parser or compilation-time
bound. No performance or total-memory improvement is claimed.

Recursive functor/closure handles, anonymous lambda lowering, remaining variadic forms, deferred
mutation/lifecycle/session semantics and full frontend parity still leave §4.12 incomplete.

Logs: `/tmp/vql-ordered-projection-build.log`, `/tmp/vql-ordered-projection-focused.log`,
`/tmp/vql-ordered-projection-tests.log`, `/tmp/vql-ordered-projection-cli.log`,
`/tmp/vql-ordered-projection-overlay.log`, `/tmp/vql-ordered-projection-callables.log`,
`/tmp/vql-ordered-projection-format.log`.

## Iteration 42: anonymous path-step lambda graphs

Anonymous path-step lambdas compile their expression or block body into inline Vortex instruction
graphs through the shared call-contract lowering. The incoming candidate supplies the lambda context
and its optional single formal. Parameter type matching precedes destructuring, and captures enter
scope after their complete match. Existing default preparation, mutation guards and result
publication remain shared with named calls. No lambda AST or evaluator callback is retained for
execution.

A private lexical-call mode preserves the enclosing named function's activation tracking rather than
adding a synthetic lambda activation name. Consequently a lambda inside an eligible named function
can call that function recursively through its existing InvokeFrame graph. Real inline-only named
helper boundaries retain their recursion refusal. Lambda temporaries remain frame-local under the
existing mutable-output collection. No VM opcode or frame descriptor format changes.

Eligibility scans selector-lambda bodies in lexical scope and records their call dependencies in the
enclosing definition. Type and destructuring captures, including nested fields and context, publish
after matching; default aliases retain their pre-match environment. Existing cycle, depth and
query-wide analysis budgets remain active. Signature preflight runs before stream mapping, including
empty inputs. The supported bound is zero or one nonvariadic formal.

Validation: full `make -j$(nproc)` passed, as did 965 affected tests across 87 suites and all 40
focused lambda/type tests across two suites. Seventeen new cases cover expression/block bodies,
zero-formal and named-formal contexts, direct and helper-mediated native-frame context, optional and
typed captures, nested lambdas, caller shadowing, lazy empty input, guarded mutation rollback,
native outer-function recursion, source-free recursive graph export/reload, host/mesh SOURCE
execution, invalid signatures on empty/nonempty streams, malformed ASTs, rollback and retry. The
real headless CLI preserves ordered `7`, exact `9007199254740993`, `7` through a typed lambda, and a
recursive lambda call returns scalar zero. Architecture, senior and resource gates accepted this
bounded stage; repository formatting with clang-format 19 passed.

The callable audit improved from 28/40 to 33/40. Its seven remaining failures are the value-lambda
route, a weave value assertion and five `sort_into` cases. The broader overlay/frontend sweep still
has the same eight unfinished failures and one skip, with 132 passes. Neither selection is a
full-suite pass. Lambda values, stored closure handles, recursive functor parameters, remaining
variadic forms and other unfinished frontend/lifecycle work still leave §4.12 incomplete. No
performance improvement or universal allocation/compile-time bound is claimed.

Logs: `/tmp/vql-lambda-graph-build.log`, `/tmp/vql-lambda-graph-focused.log`,
`/tmp/vql-lambda-graph-tests.log`, `/tmp/vql-lambda-graph-cli.log`,
`/tmp/vql-lambda-graph-recursive-cli.log`, `/tmp/vql-lambda-graph-callables.log`,
`/tmp/vql-lambda-graph-overlay.log`, `/tmp/vql-lambda-graph-format.log`.

## Iteration 43: closed traversal functors in recursive frame variants

Fixed untyped named formals without defaults can carry closed traversal templates into reusable
named function graphs. A supported template contains only literal signed-dimension steps with
default placement and yield; predicates, clamps, dereferencing, creation, projections, captures and
other selectors remain outside this native subset. Step order, complete dimension names and
directions enter the exact variant key together with formal positions and typed-rest group count.
The existing dimension resolver preserves full-width identities.

The caller recognizes templates without copying their bodies or traversing the source path as data.
It prepares ordinary arguments in caller scope and normalizes an empty stream for each template
formal before InvokeFrame. Callee compilation installs the template at that formal's position and
clears ordinary value bindings there. The existing Stream local and argument port remain in the
descriptor. Self and mutual calls forwarding the same template reuse reserved graph roots rather
than recursively expanding the AST. No VM opcode or descriptor format changes.

Eligibility propagates required functor positions through calls and preserves pre-match lexical
scope, lambda shadowing, free-capture boundaries and both generated UFCS mappings. Free caller
functors and unsupported templates retain finite compiled-inline lowering; unsupported recursive
boundaries refuse explicitly. Numeric ordinals distinguish variant labels without printing template
contents.

A configurable `maxClosedFunctorTemplateWork` ceiling defaults to 1,000,000 weighted units. Binding
entries, steps and dimension-name bytes charge template normalization and key/state copying, lookup
and rehydration work before materialization. Existing syntax/capture validation, variable-name
lookups and lexical binding snapshots retain their own behavior; this ceiling is not a bound on all
compilation CPU or memory. Unique variants, generated opcodes and descriptor records continue using
the existing aggregate caps.

Validation: full `make -j$(nproc)` passed, together with 982 affected tests across 88 suites and 64
focused functor/eligibility/lambda/type tests across four suites. Thirteen new runtime/graph cases
and four additional eligibility cases cover real native invocation, self/mutual forwarding,
content/formal/rest key distinctions, duplicate-template reuse, retained dummy ports, caller-scope
preparation, rebinding, order/duplicates, UFCS branches, finite inline compatibility, projection
refusal, weighted limits and atomic retry, recursive source-free export/reload, and actual host/mesh
SOURCE traversal output. The real headless CLI recursively follows a supplied template and returns
`leaf`. Architecture, senior and resource gates accepted this bounded stage. Repository formatting
with clang-format 19 passed.

The broader sweep retains the same eight unfinished failures and one skip, with 132 passes. The
callable audit remains 33/40, with the same seven failures. These are not full-suite passes. General
closures, decorated recursive functors, lambda values, remaining variadic forms and
frontend/lifecycle gaps still leave §4.12 incomplete. No performance improvement is claimed.

Logs: `/tmp/vql-closed-functor-build.log`, `/tmp/vql-closed-functor-focused.log`,
`/tmp/vql-closed-functor-tests.log`, `/tmp/vql-closed-functor-cli.log`,
`/tmp/vql-closed-functor-callables.log`, `/tmp/vql-closed-functor-overlay.log`,
`/tmp/vql-closed-functor-format.log`.

## Iteration 44: static clamps in recursive traversal functors

Closed signed-dimension templates now include static index/range clamps. Exact variant keys preserve
clamp presence, signed 64-bit start, optional end presence and signed 64-bit end. Callee compilation
restores that data on ordinary PathSteps, which use the existing VM clamp lowering. This permits
recursive one-hop routes such as `/+d.next[1]` without narrowing bounds or dimension identities. The
parser remains unchanged: `[n]` and `[n,n]` normalize to identical AST clamps and share a key.

The shared closed-traversal predicate still excludes predicates, captures, projections, calls,
creation, dereferencing, placement, bidirectionality, pins and yield overrides. Dummy ports, caller
preparation order, reusable descriptors and source-free graph execution retain their previous
semantics. No VM opcode, frame descriptor format or general closure ABI changes.

Weighted template-work accounting adds a presence unit, the start's `sizeof(int64_t)`, an
end-presence unit and the optional endpoint's width for each present clamp. Unclamped step costs
remain unchanged. These are specialization work units, not a universal CPU or heap bound.

Validation: full `make -j$(nproc)` passed, as did 988 affected tests across 89 suites and all 70
focused tests across five suites. Six new cases cover self/mutual native calls, combined-stream
versus explicit per-candidate clamp application, exact key reuse and distinctions, full-width
positive and negative runtime bounds/dimensions, weighted atomic refusal/retry, recursive
source-free export/reload, and actual host/mesh SOURCE output. Existing predicate and projection
recursion boundaries remain covered. Large bounds select the existing nearest-edge clamp result; the
tests distinguish that from narrowed 32-bit values. The real headless CLI uses a recursive
Cell-returning function and a `[1]` route to reach `second`. Architecture, senior and resource gates
accepted this bounded stage. Repository formatting with clang-format 19 passed.

The broader sweep retains the same eight failures and one skip, with 132 passes; the callable audit
retains the same seven failures, with 33 passes. These are not full-suite passes. Other
decorated/captured functors, general closures, lambda values and the remaining specification work
still leave §4.12 incomplete. No speedup is claimed.

Logs: `/tmp/vql-clamped-functor-build.log`, `/tmp/vql-clamped-functor-focused.log`,
`/tmp/vql-clamped-functor-tests.log`, `/tmp/vql-clamped-functor-cli.log`,
`/tmp/vql-clamped-functor-callables.log`, `/tmp/vql-clamped-functor-overlay.log`,
`/tmp/vql-clamped-functor-format.log`.

## Iteration 45: literal scalar predicates in recursive traversal functors

Closed traversal templates now admit separate scalar predicate brackets containing one non-negated
comparison between the exact current candidate and a direct primitive literal, in either operand
order. All six comparison operators use ordinary Vortex comparison lowering. Multiple such brackets
retain their order; predicates run before the existing clamp. Compound Boolean forms, captures,
computed operands, shape predicates and other excluded decorations remain outside this native
subset.

Variant keys retain operator, operand order and literal type. Int64 remains a signed 64-bit integer;
Double uses shared scalar validation and canonical bits, preserving its distinct type while giving
signed zero and quiet NaNs stable keys. Signaling NaNs are refused before canonicalization. Frame
compilation reconstructs ordinary Boolean AST as a compilation intermediate and emits VM graphs;
execution retains no predicate AST or evaluator callback. Candidate identity, ordering and
duplicates continue through the existing stream filter. No VM opcode or descriptor format changes.

Each predicate adds four weighted header units (record, operator, operand order and literal tag),
plus its primitive width or UTF-8 string bytes, to specialization work before copying. All key/state
passes and rehydration share that policy. The eligibility literal-argument shortcut also charges
each accepted comparison's fixed AST structure before recognition; unsupported shapes still receive
the full bounded AST walk. These are scoped work limits, not universal CPU or heap bounds.

Validation: full `make -j$(nproc)`, all 997 affected tests from 90 suites and all 79 focused tests
from six suites passed. Nine new cases cover real self/mutual recursion, exact typed comparison
execution, ordered keys, signed zero and quiet NaN canonicalization, signaling NaN refusal/retry,
predicate-before-clamp behavior, free-alias inline scope, AST/template work limits and source-free
recursive export/reload with actual host/mesh SOURCE output. A real headless `vquery` run returned
`keep` through a recursive literal-filter route. Repository formatting with clang-format 19 passed;
independent Luna architecture/resource and senior reviews accepted this bounded stage.

The broader 141-test audit retains the same eight failures and one skip (132 passes); the callable
40-test audit retains the same seven failures (33 passes). These are not full-suite passes.
Captured/computed predicates, compound Boolean forms, general closures, lambda values and remaining
frontend/lifecycle work still leave §4.12 incomplete. No performance improvement is claimed.

Logs: `/tmp/vql-literal-predicate-build.log`, `/tmp/vql-literal-predicate-focused.log`,
`/tmp/vql-literal-predicate-tests.log`, `/tmp/vql-literal-predicate-cli.log`,
`/tmp/vql-literal-predicate-callables.log`, `/tmp/vql-literal-predicate-overlay.log`,
`/tmp/vql-literal-predicate-format.log`.

## Iteration 46: Boolean literal predicates in recursive traversal functors

Closed candidate/literal predicate templates now retain the parser's flat Boolean structure: ordered
OR terms containing ordered AND factors, with factor-level negation. Every leaf remains an exact
current-candidate/direct-primitive comparison. Exact keys retain bracket, term and factor
boundaries, order and polarity; reconstruction uses the existing VM Boolean graph lowering without
algebraic negation rewrites. Captures, computed/truthiness leaves, nested Boolean values and other
decorations remain outside this native subset.

Specialization work charges one unit per predicate bracket and term, five per factor (record,
operator, operand order, polarity and type), plus primitive widths or UTF-8 literal bytes. All
normalization, key/state and rehydration passes use this policy before copying. Syntax accounting
charges each Boolean root, term and comparison-factor structure before the shared recognizer walks
it; unsupported forms retain the full scope/cycle-aware walk. These limits cover their recorded
work, not total CPU or heap use.

Validation: full `make -j$(nproc)`, all 1,005 affected tests across 91 suites and all 87 focused
tests across seven suites passed. Eight new cases cover native self/mutual recursion, AND/OR
precedence and negation, grouping/order/polarity variant identity and reuse, IEEE NaN negation,
unsupported and cyclic trees, node/template work limits with atomic refusal/retry, recursive
source-free export and actual host/mesh SOURCE output. Compound predicates also run before static
clamps. A real headless `vquery` run returned `keep` through the recursive Boolean route.
Independent Luna architecture, senior and resource reviews accepted the bounded stage. Repository
formatting uses clang-format 19.

The broader audit retains eight failures and one skip (132 passes); the callable audit retains the
same seven failures (33 passes). These are not full-suite passes. The initial audit launches during
the linker rewrite did not execute; the reported audits were rerun after the build finished.
Captured/computed predicates, nested Boolean values, general closures and the remaining frontend and
lifecycle work still leave §4.12 incomplete. No performance improvement is claimed.

Logs: `/tmp/vql-boolean-predicate-build.log`, `/tmp/vql-boolean-predicate-focused.log`,
`/tmp/vql-boolean-predicate-tests.log`, `/tmp/vql-boolean-predicate-cli.log`,
`/tmp/vql-boolean-predicate-callables.log`, `/tmp/vql-boolean-predicate-overlay.log`,
`/tmp/vql-boolean-predicate-format.log`.

## Iteration 47: read-only traversal modifiers in recursive functor frames

Closed templates retain bidirectional dimension traversal and step-level clone-master dereferencing.
Exact keys carry both flags, and ordinary selector/step graph lowering preserves
positive-rank-then-negative-rank ordering and dereference-before-predicate/clamp semantics. Two
named metadata work units per step cover the flags in source normalization and stored-template
passes. Yield modes, placements, creation, historical pins, shapes and projections remain outside
this stage.

Validation: full `make -j$(nproc)` and all 93 focused tests across eight suites passed. Six new
cases cover native self/mutual recursion and ordinary-graph parity, POS-before-NEG order and
duplicate inputs, exact modifier/direction keys, two-hop clone masters before filtering/clamping,
caller bindings, atomic budget refusal/retry and excluded decorations. Source-free recursive
export/reload and actual host/mesh SOURCE output pass with a bare clone whose master is `keep`. The
real headless CLI returns `keep` through the bidirectional dereference route. Initializing a clone
would write through to its master; the fixture deliberately creates a blank clone. Independent Luna
architecture, senior and resource gates accepted this bounded stage.

All 1,011 affected tests across 92 suites and repository formatting with clang-format 19 passed. The
broader audit retains eight failures and one skip (132 passes); the callable audit retains the same
seven failures (33 passes). These are not full-suite passes. Other decorated/captured forms, general
closures and remaining frontend/lifecycle work still leave §4.12 incomplete. No performance
improvement is claimed.

Logs: `/tmp/vql-read-modifiers-build.log`, `/tmp/vql-read-modifiers-focused.log`,
`/tmp/vql-read-modifiers-tests.log`, `/tmp/vql-read-modifiers-cli.log`,
`/tmp/vql-read-modifiers-callables.log`, `/tmp/vql-read-modifiers-overlay.log`,
`/tmp/vql-read-modifiers-format.log`.

## Iteration 48: closed literal defaults for required functor formals

Fixed untyped named formals required as traversal functors may obtain a closed literal template from
an omitted default. Supplied closed functors override that template; supplied ordinary or empty
values remain supplied and never trigger it. The current static ABI still refuses applying an
ordinary value as a functor. Defaults that are unused or read as ordinary values retain their
existing routing. Captured/computed, typed, variadic, dot and destructured defaults remain outside
this bounded native subset.

Default and actual literals share a non-owning recognizer and one precharged normalization path.
Requirement propagation reaches its fixed point before deciding whether a default is unused; pure
forwarding to another static formal does not count as an ordinary read. Ordinary ValueExpr/variable
syntax visits remain charged. A defaulted formal required both as a functor and an ordinary value
refuses native reuse rather than dropping its value. The existing descriptor's dummy Stream and
presence ports are preserved, and identical default/actual templates share variants. The old UFCS
injection rule still applies; recursive omission tests use value calls through `identity(...)` to
make that distinction explicit.

Ordinary-use summaries also propagate to a fixed point after required-functor discovery. A
static-capable callee formal may still consume a value; both UFCS mappings and direct or relay
consumers feed the default mixed-use check. Queries without closed functor defaults skip this
additional summary pass. Syntax-budget visits bound every iteration, callsite, argument and mapping.

Validation: full `make -j$(nproc)`, all 1,019 affected tests across 93 suites and all 102 focused
tests across ten suites passed. Eight new cases include omitted self/mutual defaults, pure
forwarding, exact override/reuse, explicit-empty precedence/refusal, UFCS/caller binding isolation,
ordinary inline defaults, captured/mixed/direct and relay value-use exclusion, atomic budgets/retry,
source-free recursive export/reload and actual host/mesh SOURCE output. A real headless `vquery` run
returned `leaf` through an omitted default and recursive VM graphs. Independent Luna gates accepted
the final bounded stage. Repository formatting uses clang-format 19.

Broader audits retain the same eight route failures and one skip (132 passes) and seven callable
failures (33 passes). These are not full-suite passes. Captured/computed defaults, general closures,
remaining decorated forms and frontend/lifecycle work still leave §4.12 incomplete. No speedup is
claimed.

Logs: `/tmp/vql-functor-default-build.log`, `/tmp/vql-functor-default-focused.log`,
`/tmp/vql-functor-default-tests.log`, `/tmp/vql-functor-default-cli.log`,
`/tmp/vql-functor-default-callables.log`, `/tmp/vql-functor-default-overlay.log`,
`/tmp/vql-functor-default-format.log`.

## Iteration 49: explicit value expressions in return items

An explicit parenthesized return item carries a `ReturnValue` compilation intermediate and reuses
ordinary VM value-expression lowering. Existing slash-led macro groups and empty groups retain their
path grammar, as do bare integer cell addresses, bare variable paths and function-path UFCS.
Parenthesized value calls use the existing value-call mapping. Unsupported value-to-path
continuations and first-class functor/closure returns refuse explicitly. The legacy direct consumer
also refuses the new item instead of interpreting it.

The parser captures the closing parenthesis and requires comma, semicolon, block end, `else` or EOF
on the same line; a later line preserves the existing statement separator. Unsupported suffixes
therefore cannot silently become another query. Functor refusal inspects syntax and bound names
without copying step vectors. The existing value visitor covers the new AST arm in whole-query
syntax budgets, capture analysis and cycle checks.

Validation: full `make -j$(nproc)`, all 1,028 affected tests across 94 suites and all 111 focused
tests across eleven suites passed. Nine new cases cover parser compatibility and boundaries, exact
Int64/Double/Bool/zero/text results, ordered duplicate identity streams, fresh loop computations,
native recursion with an omitted functor default, return promises, explicit direct-route and closure
refusals, actual cyclic compiled-body AST rollback/retry, and source-free recursive export/reload
plus host/mesh SOURCE output. The real headless CLI returned exact `9007199254740993` through
recursive value calls and VM `string()`. Independent Luna architecture, senior and resource gates
accepted the final bounded stage. Repository formatting uses clang-format 19.

Broader audits retain eight route failures and one skip (132 passes), and seven callable failures
(33 passes). These are not full-suite passes. Unparenthesized/top-level value grammar, general
closures, remaining decorated forms and frontend/lifecycle work still leave §4.12 incomplete. No
speedup is claimed.

Logs: `/tmp/vql-return-value-build.log`, `/tmp/vql-return-value-focused.log`,
`/tmp/vql-return-value-tests.log`, `/tmp/vql-return-value-cli.log`,
`/tmp/vql-return-value-callables.log`, `/tmp/vql-return-value-overlay.log`,
`/tmp/vql-return-value-format.log`.

## Iteration 50: graph-resident closure VM foundation

`CreateClosure` and `ApplyClosure` use versioned descriptor/environment graphs and the existing
native frame invocation path. Captures own resolved ordered cell-reference membership, including
duplicates and empty streams, while data stays shared. Fresh data created inside a frame can escape
through a closure. No runtime AST, callback, evaluator or closure registry implements application.
Existing named definitions and static functors retain their behavior. The three-persona design is in
[the closure runtime plan](vortex-closure-runtime-plan.md).

A shared structural marker distinguishes closure roots from ordinary scalar text. Typed scalar bits
keep precedence; matching probe text without a positive claim stays String. Descriptor/value parsers
bound records, label fragments/bytes and relation rows before materialization or linear link scans.
Source indices and counts remain ordinary integers. Stream arities, aggregate captures and mapped
fan-out are checked before copying; copied references consume fuel. Successful creation and physical
environment-cell counters remain cumulative across return and backtracking. Failed publication rolls
back arena, maps, cursor and counters. The existing single active stream cursor policy remains.
Closure input/output wings use the cell-reference records emitted by `bindInput`/`bindOutput`; port
caps, cycles and metadata relation limits are checked while reading, before vectorization. Actual
data-cell relation inspection charges work without applying metadata caps to that data.

Export preflights closure/frame reference records and disconnected body instructions before Store
writes. Its visited worklist follows declared frame entries, continuations, graph branches,
conditions and pipelines; it does not infer code from ordinary input values. Materialized closure
roots can be supplied directly and applied after reload in a fresh VM. Export of a runtime-bound
Apply handle slot refuses until binding initialization is persisted. Numeric `Choice`, `Jump`,
`Branch` and `Call` control targets also refuse. Counts, source ordinals and ordinary numeric data
are not relocated. Closure opcodes refuse unsupported conditions, parameter pipelines, raw stdlib
calls and sealed Vortex mesh artifacts before effects; VQL source lambda lowering remains future
work.

Validation: full `make -j$(nproc)` passed, all 1,050 affected tests across 96 suites passed, and all
22 focused closure tests across two suites passed. Cases include malformed descriptors/values/wings,
actual VM slot rebinding, live cell-content changes, ordered duplicates/empties, fresh invocation
locals, frame-local data escape, choices, cursor inactivity/compaction, quota rollback/retry, scalar
boundaries, pre-effect refusals, and source-free graph/value export with a disconnected branch
target. The real headless CLI preserved exact `9007199254740993` through native recursion and VM
`string()`. Independent Luna architecture, senior and resource reviews covered the bounded
foundation and export corrections. Repository formatting uses clang-format 19.

The opt-in application probe ran 32 calls per capture shape on an AMD Threadripper 1950X with Clang
`-O3 -g`. Zero, one and eight capture streams allocated respectively 0, 32 and 256 frame backing
cells. For 0/1/32 references per stream, observed times were 195/440/169, 385/389/555 and
1,715/1,791/3,293 microseconds. Factories and graph construction preceded the timer; these single
observations measure application only, not binding, creation or compilation, and establish no
speedup. Repeated descriptor/frame parsing remains an observed cost. Metadata collection is fuel
charged; additional bounded metadata-vector copies and slot mapping are not separately charged.
These logical counters do not bound total CPU or heap use. Enable the probe with
`GLEDITOR_CLOSURE_PROFILE=1` and `--gtest_filter='VortexClosureProfileTest.*'`.

The callable audit retains seven failures (33 passes); the broader route audit retains eight
failures and one skip (132 passes), with the same identities as iteration 49. General VQL
closure/lambda lowering, persisted runtime bindings and remaining frontend/lifecycle work still
leave §4.12 incomplete. This is not a full-suite pass.

Logs: `/tmp/vql-closure-runtime-build.log`, `/tmp/vql-closure-runtime-focused.log`,
`/tmp/vql-closure-runtime-tests.log`, `/tmp/vql-closure-runtime-profile.log`,
`/tmp/vql-closure-runtime-cli.log`, `/tmp/vql-closure-runtime-callables.log`,
`/tmp/vql-closure-runtime-overlay.log`, `/tmp/vql-closure-runtime-format.log`.

## Iteration 51: bounded VQL lambda values and stored application

A lambda value with one untyped named nonvariadic formal and an alias or supported read-only
variable-anchored path body compiles into a detached native frame, a closure descriptor and
`CreateClosure`. The frame is compiled immediately in isolated compiler state. Its hidden context
capture is explicitly empty; sorted free aliases become owned capture streams and the named formal
is an applied stream. Capture membership freezes at creation while referenced data stays shared.
Synthetic names are collision checked even against programmatic AST definitions, and closure arity,
frame variants, generated code, descriptor records and syntax walks retain their logical budgets.

A dynamic `/$f` step uses `ApplyClosure` per incoming candidate when no static functor binding wins.
The VM accepts exactly one Cell/Stream handle and refuses empty/multiple handles and numeric pointer
imitation. Empty incoming data skips application; results preserve identity, order and duplicates.
Existing outer predicates/clamps/master operations follow application. The shared syntax recognizer
keeps candidate predicate context distinct from captured aliases, and the full eligibility walker
still validates unsupported and cyclic ASTs before signature recursion or inline expansion.

First-class types/defaults/dot/destructuring/block bodies, nested creation, implicit context roots,
functor captures and broader dynamic/higher-order calling forms remain deferred. Existing inline
path-step lambdas retain their wider signature/body support. Session variable publication and
persisted VM bindings remain unfinished. Export now refuses runtime-bound Stream handle slots as
well as Cell slots before Store writes; direct closure graph values remain portable.

Validation: full `make -j$(nproc)`, all 1,063 affected tests across 97 suites and all 33 focused
tests across three suites passed. Twelve new source cases prove real Create/Apply/frame emission,
creation-time rebinding, duplicate/empty captures, live data contents, predicate scope, a recursive
native maker returning a closure holding frame-owned data, per-iteration closures after frame
return, exact numeric kinds, handle/scalar refusals, bounded failure/rollback/retry, genuine
compiled-body AST cycles, source-free export/reload and host/mesh SOURCE execution. An additional VM
export case checks the live Stream-handle refusal. The old unavailable-value test now verifies
native graph values and an `Any` return promise. The real headless CLI returned exact
`9007199254740993` through a recursively created closure. Independent Luna architecture, senior and
resource gates accepted the bounded stage; repeated analysis and per-candidate frame allocations
establish no speedup claim. Repository formatting uses clang-format 19.

Broader audits retain eight route failures/one skip (132 passes) and seven callable failures (33
passes). The legacy missing-current-cell test now passes its closure-value segment and exposes its
still-unfinished session update and `count()` checks; those assertions remain intact. This is not a
full-suite pass or §4.12 completion. The remaining acceptance packages are tracked in
[the completion gates](vql-412-completion-gates.md); their iteration estimate is provisional.

Logs: `/tmp/vql-lambda-values-build.log`, `/tmp/vql-lambda-values-focused.log`,
`/tmp/vql-lambda-values-tests.log`, `/tmp/vql-lambda-values-cli.log`,
`/tmp/vql-lambda-values-callables.log`, `/tmp/vql-lambda-values-overlay.log`,
`/tmp/vql-lambda-values-format.log`.

## Iteration 52: runtime binding persistence and usable closure sessions

This larger iteration delivers graph binding transport, portable bound closure application and
session publication together. `VORTEX_BINDINGS/1` records literal Cell register targets, including
missing results, ordered duplicate/empty Stream membership and unavailable/exhausted Iterator type
markers. `RestoreBindings` stages replacement maps and advances the managed cursor under an arena
savepoint. Failure restores maps, cursor ownership, physical cursor state and cumulative load
counters. Successful loads remain cumulative. Live frames, choices, scopes, resumable state and
unfinished iterators refuse; this is binding transport, not continuation migration.

Cell data and VM bindings are independent. Export now preserves physical scalar bits/text and
relocates every runtime binding through explicit descriptor records, including disconnected slots
and alias cycles. A stream member that is itself another register remains that literal member.
Ordinary integer data is never inferred to be a reference. Declared parameter, descriptor and cursor
reference fields still relocate. Destination metadata and one native initializer are written only
after snapshot/schema/reference preflight; the source arena stays unchanged. Descriptor work,
records, metadata bytes/relations, slots/references and cumulative restore quotas are bounded.

The VQL compiler reports the final top-level binding layout. Successful execution publishes actual
VM cell-reference results atomically into the session before cursor cleanup. Failed execution
publishes nothing. Static functor syntax remains a compilation intermediate across queries;
functor/value/lens shadowing removes the old kind. Callee locals stay scoped. The CLI now retains
one session engine across REPL lines while each query compiles and runs in its own VM.

Validation: full `make -j$(nproc)` passed, all 1,084 affected tests across 100 suites passed and all
26 focused tests across six suites passed. New cases cover literal alias cycles and data/register
overlap, numeric payloads equal to CellRefs, exact Int64/Double values, missing/open/empty bindings,
closed and unfinished iterators, local parser inspection limits, malformed descriptors, repeated
cursor initialization, pre-Store quota refusal and a fuel-failure sweep with quota-preserving retry.
A frame-created closure is applied directly after source-VM destruction and Store reload; its
factory is not rerun. Both Cell and Stream handle routes work. Session tests use a fresh VM per
query and cover recursive creator-frame lifetime, rebinding, live referenced contents, scope and
return-promise failure. The real headless REPL returns exact `9007199254740993` through a closure
created recursively on its previous line.

Independent architecture, senior and resource reviews found and resolved descriptor marker,
cumulative inspection and relation-limit issues. Session syntax/map copying is not optimized and no
speedup or total CPU/heap claim is made. Broader callable and route audits are recorded with the
validation logs; their unfinished functionality is not a full-suite pass. Broader closure
signatures/bodies, general dynamic functors, deferred variadics and remaining route/lifecycle work
still leave §4.12 incomplete.

Logs: `/tmp/vql-persistent-bindings-build.log`, `/tmp/vql-persistent-bindings-focused.log`,
`/tmp/vql-persistent-bindings-tests.log`, `/tmp/vql-persistent-bindings-cli.log`,
`/tmp/vql-persistent-bindings-callables.log`, `/tmp/vql-persistent-bindings-overlay.log`,
`/tmp/vql-persistent-bindings-format.log`.

## Iteration 53: typed structural closure bodies and nested currying

This larger iteration combines zero/unary closure formals, typed and destructured contracts,
compatible scalar/path/named-call/block bodies, nested creation and staged currying. The applied
candidate supplies hidden context and the optional formal through separate frame ports. Free alias
captures retain creation-time membership, while referenced contents stay shared. Contracts still
filter parameter mismatches; named-helper return promises remain errors. Structural field defaults
keep their pre-match scope and existing graph thunks/guards.

Closure analysis now delegates to the actual bounded frame scope walker, deleting the duplicate
syntax walker. It borrows AST nodes and validates cycles/depth before recursive type validation.
Scoped callsite summaries let closure capture expansion include transitive named-helper free/default
aliases without capturing names bound by a lambda formal or prior body-local let. Typed-rest helper
default requirements follow the existing actual-count and UFCS context-injection rule. Missing
required aliases refuse at creation; bound-empty aliases remain defined. A separate configurable
`maxClosureCaptureWork` budget counts post-analysis expansion callsites and callee/capture-name
bytes. The initial scope walk, repeated nested scans and total compile CPU/heap are outside that
counter. Distinct capture limits apply before helper alias insertion.

Full `make -j$(nproc)` passed, all 1,105 affected tests across 101 suites passed, and all 51 focused
tests across four suites passed. Twenty-one new cases cover candidate context/order/duplicates,
typed/union/optional/destructure formals, Stream element-capture isolation, a field default that
retains the original outer alias despite an earlier field capture and later rebinding, undefined
versus empty aliases, exact Int64/Double bodies, helper scope/timing, block locals, nested
shadowing, curried frame-owned captures, promises/rollback, work-budget refusal/retry and named
unknown types. The new typed nested graph executes after AST/source-VM destruction and Store reload,
and through host/mesh SOURCE routes. The real headless REPL creates a typed increment closure on one
line and returns exact `9007199254740994` on its next line.

Whole-formal defaults, multiple/variadic value-lambda formals, captured static functor values and
general runtime application inside reusable bodies remain deferred. Existing inline path-step lambda
defaults retain their wider behavior. The callable audit retains seven known failures (33 passes);
the broader route audit retains eight failures and one skip (132 passes). These are not full-suite
passes. No optimization or total CPU/heap claim is made, and §4.12 remains incomplete.

Logs: `/tmp/vql-closure-signatures-build.log`, `/tmp/vql-closure-signatures-focused.log`,
`/tmp/vql-closure-signatures-tests.log`, `/tmp/vql-closure-signatures-cli.log`,
`/tmp/vql-closure-signatures-callables.log`, `/tmp/vql-closure-signatures-overlay.log`,
`/tmp/vql-closure-signatures-format.log`.

## Iteration 54: runtime higher-order frames and deferred rest signatures

This combined iteration permits runtime closure handles in named function formals and body locals,
and in value-lambda formals, locals and captured aliases. Application emits `ApplyClosure`; named
forwarding and self/mutual recursion reuse native `InvokeFrame` graphs. Closed static path actuals
retain separate template variants. Complex static paths retain finite inline graph lowering and
explicit recursive refusals. Static predicate aliases still resolve in the callee/application scope;
value-lambda aliases retain creation-time capture. A named call can be wrapped in a value lambda
when a runtime handle is needed. Bare definition names are not function-reference syntax.

Final named variadics now accept whole-rest defaults, context targets and destructuring. Zero
original rest groups run the default once, after fixed-parameter checks; an explicitly empty group
skips it. The default is one group. Each original group matches its type and then its destructuring
shape before ordered concatenation. Rest matching publishes no per-element captures. Context rest
formals bind the aggregate, including empty. Typed or destructured groups receive distinct Stream
ports; a default-presence Cell port distinguishes omission. Descriptor/local preflight includes the
extra ports. Omitted-rest dependency summaries propagate separately from positive-rest contract
dependencies, including omitted fixed defaults and the existing UFCS injection rule.

Full `make -j$(nproc)` passed, all 1,133 affected tests across 103 suites passed, and all 28 new
tests passed. Cases include typed and local runtime handles, nested unary composition, defaults that
produce handles, static/runtime variants, recursive forwarding, duplicate input, static predicate
scope, whole-rest omission versus empty, ordered duplicate defaults, prior fixed bindings, context
and destructuring isolation, filtering versus return promises, retry and fresh-VM export results.
Host and mesh SOURCE routes preserve exact Int64 `9007199254740994`; the actual headless CLI
produces the same result from combined runtime application and omitted fixed/rest defaults.

Independent Luna architecture, senior and resource reviews found no stage blocker. The resource
review noted the existing general native-frame Stream-copy cardinality gap as separate VM work;
closure creation/application reference caps remain in place. There is no speedup or total CPU/heap
claim. The broader callable audit retains seven failures (33 passes), and the route audit retains
eight failures and one skip (132 passes). Whole-formal defaults and multiple/variadic value-lambda
formals, decorated recursive static functors and remaining route/lifecycle gates keep §4.12
incomplete. These results are not a full-suite pass.

Logs: `/tmp/vql-callables-rest-build.log`, `/tmp/vql-callables-rest-focused.log`,
`/tmp/vql-callables-rest-tests.log`, `/tmp/vql-callables-rest-cli.log`,
`/tmp/vql-callables-rest-callables.log`, `/tmp/vql-callables-rest-overlay.log`,
`/tmp/vql-callables-rest-format.log`.

## Iteration 55: symbolic decorated static functor frames

Named static functor variants now lower computed scalar operands, nested Boolean values, existing
literal/nested shape predicates and nested frozen static bodies into reusable native frame graphs.
Existing read placements and Keep lowering can also run in recursive variants. Closed literal
templates retain their borrowed normalization path. Generic variants use a bounded structural key
and compile-only step payload; keys contain symbolic names, exact scalar type/bits, ordered
predicate/shape structure, flags and body/decorator boundaries, never current alias values or AST
pointer identity. Execution and export contain ordinary Vortex graphs only.

Variant-specific Stream ports transport template-only aliases and transitive named-helper
dependencies. Formal and prior-local shadowing applies at each use, including forwarding-only
functions. Required helper field-default aliases retain missing-versus-bound-empty semantics through
forwarding. Static bodies freeze when bound while their ordinary aliases resolve in
callee/application scope; value-lambda creation captures retain their separate rules. The normalizer
owns its outer step vector and shares read-only nested AST inputs during synchronous compilation.
Queued bodies are fully lowered before compilation returns; exported execution needs no AST payload.

Scope analysis preserves the existing node/event and depth limits and adds a separate default
1,000,000-unit metadata-work limit. Static normalization debits that preflight before key/payload
work. Borrowed capture lookups and incremental inspection/copy checks cover owned vectors, strings,
groups, expansion and splices before materialization. Non-closed programs spend the existing
`maxClosedFunctorTemplateWork` ceiling on those phases. These are bounded work estimates; allocator
overhead, hashing and total compile CPU/heap are outside the claim. No speedup is established.

Full `make -j$(nproc)` passed. All 1,157 affected tests across 104 suites passed, including all 24
new cases, and the expanded focused run passed 52 tests across five suites. Cases prove native
descriptor reuse, 1/5/9 alias timing, before/after local shadowing, caller static aliases shadowed
by formals/locals, direct/self/mutual/forwarded applications, helper dependencies and required
defaults, Boolean order and nested-value execution, shape matching, frozen inner rebinding,
aggregate clamps and duplicates, exact Int64/Double key distinctions, cycles inside unsupported
syntax, repeated shared-tree budgets, byte-work limits and rollback/retry. A recursive
computed-predicate graph survives AST/source-VM destruction and Store reload, and runs through
host/mesh SOURCE. The real headless CLI returns `11` after two recursive applications.

The matrix caught and fixed a formal-index increment missed by the closed fast path and restored
hard refusal for malformed empty Boolean trees. Former unsupported-form tests now execute newly
supported predicates and read modifiers; creation still refuses at that recursive boundary.
Independent Luna architecture, senior and resource gates found no remaining stage blocker. The
broader callable audit retains seven failures (33 passes); the route audit retains eight failures
and one skip (132 passes). This is not a full-suite pass or §4.12 completion. Non-closed static
formal defaults, computed shape targets, ordered projections, historical pins, new value-lambda
static capture ABI and the remaining completion gates stay open.

Logs: `/tmp/vql-static-functor-build.log`, `/tmp/vql-static-functor-focused.log`,
`/tmp/vql-static-functor-tests.log`, `/tmp/vql-static-functor-cli.log`,
`/tmp/vql-static-functor-callables.log`, `/tmp/vql-static-functor-overlay.log`,
`/tmp/vql-static-functor-format.log`.

> Current execution policy: all language frontends must compile to Vortex graphs and run on the VM.
> Direct execution has been cut off; earlier direct/Vortex parity results below are historical.
> Missing VM lowering now refuses explicitly. See
> [VM-only iterations](projects/vortex-vm-only-iterations.md) for validation and remaining work.

# VQL implementation status

Executable VQL now uses a restricted compiler and the Vortex VM. Missing lowering refuses
explicitly. Syntax acceptance alone does not establish execution support.

## Historical foundation (interpreter-only entries currently refuse)

- **§4.2/§7.4, content access:** slice reads remain distinct from replacement writes;
  `value(replacement, offset, length)` snapshots its operands and returns the edited cell. Callback
  exceptions restore outer bindings.
- **§4.8, shapes:** arena-local first-class shape handles; named and function-returned shapes;
  expansion, nested predicates, wildcard masks, exclusive projection, and refusal of unresolved
  shape references. Projection changes the query arena, not a source Store.
- **§4.8.7, sorting:** `sort_into(rank, by: path)` and path lambda keys; stable numeric/string
  sorting of a contiguous rank section with reciprocal links and preserved surrounding cells.
  Integer keys retain their integer precision. Mixed numeric/string keys and NaN keys refuse.
- **§4.9.2, local mounts:** private session aliases, local anchor resolution, ordered mount/unmount
  declarations, remounting, and isolation between engines. Unsupported remote mounts refuse.
- **§4.9.6, local observation:** subscription, polling, cancellation, membership/value/topology
  deltas, callback-write suppression, and callback-safe cancellation. Store-backed `since` compares
  a historical snapshot with the current snapshot. It does not replay every intervening operation.
- **§4.10, dynamic views:** scoped dimension pivots and lazy computed dimensions with independent
  view identities; pivot traversal/creation maps to the physical dimension. `lens(get:, put:)` binds
  path lambda callbacks and routes reads/writes through them. Computed dimensions without a put
  transform refuse writes. Eager assignments mixed into dynamic projection rules refuse.
- **§4.11, local history:** version literals, sequential next/parent/branch arithmetic, causal
  comparisons of version expressions and bound version values, cell/home pins, and ancestor ranges
  over available local versions. Historical folds retain source readers and identities through
  cached arena Spaces and proxies. Reading a fold appends no Store operations.
- **§4.12.3, compiled functors:** point-free bindings, composition and bare application expand to
  ordinary Vortex traversals. Structural result slots retain cell identity between VM instructions;
  missing links clear old results, and backtracking restores reference mappings. Topology reads
  bypass value-only memoization. Unsupported declarations/selectors refuse explicitly.

## §4.12 VM iteration in progress

Runtime block loops, destructuring, `where`, conditional actions and ordered multi-return now use
actual VM instructions. Typed context fields and implicit context captures use descriptor ABI
`VQL_CONTRACT/4`; older descriptors refuse. Request execution restores its contract coordinator on
every exit. The initial bounded VM-only iteration recorded 238 passing focused tests. Later
iterations below add reusable recursive frames and free capture ports; path/call field-default
thunks and atomic mutation enforcement remain open. Variable-alias field defaults now capture prior
lexical VM slots lazily, preserving identity for zero or one fallback cell. Larger streams refuse
explicitly. This alias increment passes 261 focused tests; it does not establish general lazy-thunk
support.

Scalar comparisons now lower to a dedicated Vortex instruction with exact Int64 and IEEE Double
semantics, whole-token text inference and canonical text fallback. Typed bits override display text;
clone masters and missing operands are respected. Mixed Int/Float compares numeric values losslessly
without rounding integers through floating point; stored types remain distinct. The comparison
iteration passes 263 focused tests, including CLI/host/mesh filtering and actual persisted-graph
execution. The mixed numeric matrix preserves operand kinds and bits at Int64 and IEEE boundaries.

Contract matching now stages shape captures and rolls back failed union/default attempts, including
minted dimensions. Return validation releases temporary defaults on both success and failure. Arena
rollback restores empty-run scalar headers and dimension-cache journals; branch-prefix payloads use
`APF2`. The matching-atomicity and explicit VM resume-state iteration passes 395 affected tests; it
does not establish mutation-boundary enforcement. Recursive frames and default thunks remain
separate work. Stream element contracts intentionally publish no per-element captures (§4.12.5). The
subsequent capture-scope iteration prevents empty generic capture slots from shadowing lexical names
or context. Descriptor ABI `/4` excludes those slots and explicitly refuses older versions. All 402
affected tests, the full build, repository formatting, and independent Luna gates passed, including
saved graphs executed by a fresh VM.

The shared `ContractMatcher` is used by actual Vortex contract opcodes. Versioned, bounded
descriptors survive save/reload without a process-local AST registry. Nonrecursive positional typed
calls support filtering, captures, literal shape defaults, nested calls and return checks. Omitted
parameter defaults lower supported expressions to real VM instructions after earlier parameter
matches. Explicit arguments resolve in the caller scope, and untyped named parameters capture
functor steps before callee bindings are introduced. A shared `QueryExecutor` supplies structured
errors and context handling for CLI and host query/navigation/script/batch entry points.

Executable export now traverses full arena cell identities and relocates parameter slots, dimension
operands and cursor PCs. Tests execute the saved graph in a fresh VM, rather than merely checking
that files exist. Unsupported foreign/base-backed exports and numeric control targets refuse
explicitly.

Step predicates now produce guarded runtime references. AND/OR use real cell-target branches, skip
absent candidates, and preserve short-circuit error/effect behavior. Saved programs preserve both
original text and exact scalar kind/bits, including the home cell; only explicit reference operands
relocate. Unsupported handle payloads refuse before export mutation. User-defined named arguments
refuse consistently, while named builtin options remain supported.

Runtime rank iterators and eager sequence registers now support variable-length ranks, per-member
typed calls, whole-stream contract cardinality, predicates and checked integer index clamps. Order,
identity and duplicates survive loops and choice-point snapshots. Foreign ranks resolve canonical
proxies lazily through the arena; compiler home/named-store anchors use coordinator identities.
Bidirectional, endpoint and whole-rank reads are lowered to real VM loops. Negative whole-rank reads
reverse canonical forward order, preserving semantics across asymmetric foreign overlays. Default
creation appends at the tail and mints a distinct runtime cell per invocation. Unsupported pins and
cursor/generator/historical anchor kinds refuse explicitly.

Decorated functor calls now run their full captured path before applying call-site dereference,
predicates and clamps over the combined result. Shared capture freezes decorated dependencies across
rebinding while preserving empty-input function behavior. Query-time `>` walks canonical foreign
clone chains with per-probe fuel checks; ordinary cells retain identity and cycles return the
original cell. The same VM lowering handles anchor, variable-stream and step dereference.
Creation-step `!last` now selects globally after predicates/clamps, and `!both` yields contexts
before creations. Bound functors retain those creation-step semantics after capture and rebinding.
Read `!last`/`!both` remain no-ops. `link()` fan-out mints distinct clones per runtime iteration,
preserving each attachment. The iteration passes all 412 affected tests, the full build and
formatting, including saved-graph execution. General yield overrides on opaque functor applications
and pins remain open.

Nonempty static shape predicates now compile bare signed, bidirectional, and negated dimension rules
into native `ProbeLink` instructions with Boolean graph branches. These one-hop checks recognize
self-links and resolve foreign proxies, retain candidate identities and duplicates, and filter
before clamping. Boolean shape tests in anchors, `where` and `if`, captured functors, CLI/host/mesh
routes, and persisted graph execution share this lowering. Scalar literal constraints and nested
static shape targets now lower to native comparisons and recursive graph branches. Bidirectional
targets accept either matching neighbor; absent neighbors cannot satisfy nested absence constraints.
Negated rules retain absence-only semantics and skip target evaluation. Foreign comparisons read
underlying typed bits, preserving exact mixed numeric comparisons and structural-handle refusal.
Compiler preflight bounds depth, cyclic ASTs, repeated validation work and bidirectional graph
expansion. All 449 affected tests, the full build, formatting and Luna gates passed for this target
increment. Empty/wildcard shapes, dynamic shape references and projection still need lowering. The
full build, formatting and all 435 affected tests passed, with independent Luna gates. A separate
expanded mesh run retains seven failures on missing Prolog, VLace and VPL lowerings; no full-suite
or full §4.12 pass is claimed. See iteration 10 in the VM-only ledger for logs.

The stream foundation supports one active stream cursor per VM; overlapping cursors and stream
choice-point stealing refuse. Mesh stream transport, foreign/base-backed export, and general lazy
stream semantics remain incomplete. Exported local instruction plans rebuild their iterators in a
fresh VM. Request execution releases its cursor on errors as well as success.

This is an internal foundation milestone. Full §4.12 parity remains incomplete; see the
[iteration gates](projects/vql-parity-iterations.md). Fresh Luna architecture and senior reviews
accepted the bounded foundation, predicate filtering, omitted-default and argument/functor code
iterations. The argument parser still rejects some qualified functor arguments before compilation;
the compiler AST guard is tested separately.

Comparisons and primitive contracts now share a query scalar reader with exact foreign typed bits,
clone-master resolution and bounded text inference. Empty text remains String, Timestamp keeps its
text fallback, and malformed foreign values produce errors rather than silent contract mismatches.
All 454 affected tests, the full build, formatting and Luna gates passed for this correction. Full
§4.12 remains open; the remaining dependency sequence is tracked in the parity iteration plan.

## Remaining specification work

- **§4.1–§4.4:** the direct evaluator materializes vectors; universal lazy streaming, full
  topological return weaving, and complete compiler parity are unfinished.
- **§4.8:** full shape extraction, all dynamic shape/type interactions, general scalar arithmetic,
  effect inference/enforcement, and complete lambda/function compilation remain incomplete.
- **§4.9:** sovereign discovery providers, live ZZ9P watch transport, per-operation Chronofilade
  replay, causal attribution across remote writers, transclusion ripple delivery, distributed mount
  dispatch, and in-band transcopyright settlement are not implemented by this change.
- **§4.10:** dynamic handles allocate arena cells. The zero-allocation `DynamicLensMatrix`, SIMD/JIT
  evaluators and stated nanosecond guarantees remain proposals. General callback arithmetic and a
  universal registered projection-lens interface are not added here.
- **§4.11:** the complete first-class Store/Slice/Dim register model, filesystem-opening generators,
  discovery catalogue, universal projection dispatch and 16-byte `HyperHandle` lowering remain
  incomplete. Invalid/unavailable pins yield no matches. Local mounts resolve registered stores.
- **§4.12:** complete lazy/runtime stream semantics, recursive functor/variadic frame forms,
  variadic defaults/dot/destructuring, nonliteral shape-field default thunks, mutation-boundary
  enforcement, functor shape/pin/yield completeness, session-state/frontend parity, and specialized
  `CurriedMorphism`/`TRAVERSE_CURRIED_DIM` lowering remain incomplete. Qualified functor anchors
  refuse explicitly in compiled capture.
- **§5:** arena lifetime and choice-point rollback exist; a VQL tracing/sweeping garbage collector
  rooted in the proposed live-query root set is not implemented.

The specialized ABI sizes and latency/cycle claims in the design have not been benchmarked here.

## Regression coverage

The headless `tests/xuzz/` coverage includes the existing lexer, parser, compiler, types/functors,
dynamic-view, shape, sorting, history, generator, watch and value suites, plus the shared contract
matcher/codec, type parity, executable export, shared execution, host parity and argument-scope
suites. Export tests save, reload, and execute in a fresh VM.

## Validation

- The resumed full `make -j$(nproc)` build passed.
- `make -j$(nproc) format-check` passed with clang-format 19. The edited Markdown passes mdformat
  and mdl; `git diff --check` passes.
- Focused runs progressed through 387, 389 and 391 passing tests as the iterations above landed.
- The broad `make -j$(nproc) test` run passed all 830 library tests. The Xuzz run had 2,316 passes,
  10 dependency-dependent skips and one failure in a new test that reached the parser instead of the
  intended compiler guard. The test was corrected to supply a parsed qualified-path AST.
- All 396 focused language/runtime parity tests then passed, including the corrected compiler guard.
  All 256 ZigZag tests passed separately.
- `make -j$(nproc) test/swarm` passed all 12 transport/mutable-name cases and all five publication/
  checkpoint network cases in real network namespaces.
- Repository lint is blocked by unchanged `Sans 12` defaults in `acme_presenter.hpp:28` and
  `slice_view_presenter.hpp:46`. Both literals also occur in `HEAD`; these unrelated UI files were
  left untouched.

All executables ran headlessly with all three XDG directories isolated under `build/xdg`. No
specialized lowering performance claim is made. Profiling review identified environment snapshots as
a workload to measure before optimizing them. Full §4.12 remains incomplete.

Validation logs are `/tmp/vql-final-build.log`, `/tmp/vql-final-tests.log`,
`/tmp/vql-final-focused-tests.log`, `/tmp/vql-final-zigzag-tests.log`,
`/tmp/vql-final-swarm-tests.log`, `/tmp/vql-final-format.log`, and `/tmp/vql-final-quality.log`. The
broad test target itself exited at the original compiler-guard test failure; the corrected guard and
affected language suites, followed by the remaining ZigZag and swarm stages, passed in separate
runs. No clean rerun of that entire target is claimed.

### Runtime stream iteration validation

- `make -j$(nproc)` passed after the runtime stream, selector and foreign-anchor changes.
- All 429 focused language/runtime/frontend parity tests passed, including real-store foreign ranks,
  reverse sequence rollback, CLI named-store resolution, and exported fresh-VM execution.
- The subsequent full Xuzz binary completed with 2,350 passes and 27 skips (2,377 tests run, one
  additional disabled test). All 830 library tests and 256 ZigZag tests also passed. These binaries
  ran headlessly with separate isolated XDG state. The rootless swarm stage was not rerun in this
  iteration; its earlier successful run is recorded above.
- `make -j$(nproc) format-check` passed with clang-format 19. Repository lint remains blocked by the
  same two unchanged UI typography defaults recorded above.
- Independent Luna senior source reviews accepted clamps, bidirectional/endpoints, runtime tail
  append, foreign hops/anchors and canonical reverse-rank lowering. Architecture review accepted the
  bounded algorithms using inspected/supplied code and was conditional on the listed tests; those
  focused regressions passed. The profiler accepted correctness/cycle reporting, with no uniform
  speedup or allocation claim.

Logs: `/tmp/vql-stream-full-build.log`, `/tmp/vql-stream-final-focused.log`,
`/tmp/vql-stream-format.log`, `/tmp/vql-stream-lint.log`, `/tmp/vql-stream-full-xuzz.log`,
`/tmp/vql-stream-full-library.log`, `/tmp/vql-stream-full-zigzag.log`. Full §4.12 remains
incomplete.

### Annotated functor iteration validation

All 433 focused language/runtime/frontend tests passed after the shared capture and
query-dereference changes. This includes decorated-call aggregation, rebinding/shadow capture,
context-free calls on empty input, local/foreign master chains and cycles, zero-fuel no-mint
recovery, CLI/host routes, and typed fresh-VM exported execution. Independent Luna senior source and
architecture design gates accepted this bounded iteration. The profiler accepted measured
graph/cycle results with no speedup or allocation claim; see the iteration ledger for the probe and
remaining limits.

The subsequent full Xuzz binary completed with 2,354 passing tests and 27 skips (2,381 tests run,
plus one disabled test). Full `make -j$(nproc)` and `make -j$(nproc) format-check` passed, as did
all 256 ZigZag tests. Repository lint still stops at the same two untouched UI typography defaults.
No library or rootless swarm stage was rerun for this engine-only iteration; their preceding passing
runs are recorded above.

Logs: `/tmp/vql-annotated-final-build.log`, `/tmp/vql-annotated-focused.log`,
`/tmp/vql-annotated-probe.jsonl`, `/tmp/vql-annotated-full-build.log`,
`/tmp/vql-annotated-format.log`, `/tmp/vql-annotated-zigzag.log`, `/tmp/vql-annotated-lint.log`,
`/tmp/vql-annotated-full-xuzz.log`. Full §4.12 remains incomplete.

### Named variadic and nested-argument iteration

Named final variadics preserve source/member order and duplicates, type-check each actual argument
stream before concatenation, and leave shape captures out of the callee environment. Shared
signature validation rejects unsupported rest forms before registering any definitions. Finite
nested calls in argument expressions now compile before callee activation; true body/default
recursion still refuses. Luna implemented the binder and runtime/frontend/export regressions, with
separate architecture/source gates. All 446 focused tests, the full build and formatting checks
passed. General variadic forms and full §4.12 remain incomplete; contract traversal/copy accounting
is the next review item.

Logs: `/tmp/vql-variadic-focused.log`, `/tmp/vql-variadic-full-build.log`,
`/tmp/vql-variadic-format.log`.

### Contract accounting iteration

Luna implemented a borrowed VM result visitor, preserving scalar/typed-stream identity and order,
and changed contract subject collection to charge before each append without first copying a whole
result vector. Matcher input remains materialized. Shared matching now charges recursive type visits
and field probes before proxy/default work. Native tests isolate prebound subject traversal, default
suppression, capture isolation and recovery; frontend tests separately cover structured request
budget errors and recovery. Environment snapshot/preparation costs are not fully metered by this
change, and no universal allocation/performance guarantee is made.

All 452 focused tests, the full build and formatting check passed. Luna cross-reviews and the
architecture design gate accepted the bounded implementation. Full §4.12 remains incomplete. Logs:
`/tmp/vql-contract-budget-focused.log`, `/tmp/vql-contract-budget-full-build.log`,
`/tmp/vql-contract-budget-format.log`.

The combined full Xuzz run for both new iterations passed 2,373 tests with 27 skips (2,400 tests
run, plus one disabled test). All 256 ZigZag tests also passed. The full build and formatting gate
passed; lint remains blocked by the same two unchanged UI defaults. No library or rootless swarm
stage was rerun for these engine-only changes. Logs: `/tmp/vql-iterations-full-xuzz.log`,
`/tmp/vql-iterations-zigzag.log`, `/tmp/vql-iterations-lint.log`.

## Native activation-frame foundation

Vortex now has graph-native versioned call descriptors and cursor-owned activation stacks. Local
registers receive separate backing per invocation, and Cell/Stream arguments and results preserve
actual identities and ordering. Choice points restore frames, explicit close cleans their runtime
maps, return publication is atomic, and cumulative backing allocation is bounded. Export relocates
tagged descriptor references with their role text intact and runs in a fresh VM. The affected
477-test matrix passed; the broader sweep retains 27 existing missing-Prolog-lowering failures.

This foundation still leaves VQL definitions compiled inline, with recursive expansion refused.
Reusable recursive function graph lowering is next. Mesh frame artifacts explicitly refuse, and
stream frames retain the existing single active stream cursor policy. Lazy field-default
continuations, mutation boundaries and full functor/parameter semantics remain open. See
[iteration 13](projects/vortex-vm-only-iterations.md) for scope, gates and measured resource costs;
full §4.12 remains incomplete.

## Reusable closed-function graphs

Closed primitive/Stream functions now compile once per query to reusable native frame bodies,
including self/mutual recursion and lazy parameter-expression defaults. Presence ports distinguish
omitted from explicitly empty arguments. A bounded AST/call-graph analysis protects lexical scope
and rejects malformed cycles before signature validation. Indirect open helpers retain compiled
inline lowering, preserving caller bindings. A self-recursive saved graph runs in a fresh VM, and
host/mesh Vquery source routes pass.

Full build and 493 affected tests passed; the broader sweep retains the 27 existing missing-Prolog-
resolution failures. Shape contracts, context-dot targets, explicit free captures, functors and
variadics still need reusable frame lowering. Nonliteral contract-field defaults and atomic mutation
contracts remain incomplete. See [iteration 14](projects/vortex-vm-only-iterations.md); this is not
a full §4.12 pass.

## Context-formal frames

A single explicit `.` parameter now lowers to a native frame Stream port, including supported type
annotations and lazy expression defaults. It overrides the separate inherited context even when
explicitly empty; UFCS, nested calls and recursion preserve identity/order. Full build and all 501
affected tests passed, including host/mesh source and fresh-VM export. See
[iteration 15](projects/vortex-vm-only-iterations.md). Structural captures/destructuring, free
closure inputs, functors, variadics and nonliteral field-default continuations remain open; this
does not complete §4.12.

## Closed structural-contract frames

Closed structural annotations and destructured formals now use reusable native frame graphs,
including nested/optional/union matches, reference captures, context capture, literal defaults and
earlier lexical aliases. Every default alias resolves before its whole match; generic element and
refinement captures stay private. Open defaults retain compiled inline lowering. Recursive
structural graphs survive Store export/reload into a fresh VM. Full build and all 514 affected tests
passed, with independent Luna architecture and senior/profiler gates. See
[iteration 16](projects/vortex-vm-only-iterations.md). Free closure inputs, functors, variadics,
nonliteral field-default continuations and mutation contracts remain open; §4.12 is incomplete.

## Destructured loops in reusable frames

Closed destructured loops now lower inside reusable functions using existing native graph
instructions. The scanner preserves prefix `let` bindings and restores loop-local names at block
exit, keeping later caller-only aliases on compiled inline lowering. Nested and recursive loops,
missing fields/defaults, caller identity, fresh-VM export/reload and host/mesh source routes passed.
Full build and all 521 affected tests passed with independent Luna gates; see
[iteration 17](projects/vortex-vm-only-iterations.md). The documented scalar/string expression
bodies, free closures, variadics, lazy field-default continuations and mutation contracts still
prevent §4.12 completion.

## Native scalar string conversion

`string(expr)` emits a native Vortex scalar conversion with full argument cardinality checks and
shared typed/foreign/clone reads. Text stays unchanged; typed values use canonical scalar
formatting. Nested frames, lazy parameter defaults, saved graphs and source/host/mesh routes passed,
including a sealed scalar artifact. Full build and all 533 affected tests passed with independent
Luna gates; see [iteration 18](projects/vortex-vm-only-iterations.md). Additive expressions are
still required by the documented contract examples. Nonliteral field-default thunks, mutation
boundaries, free closures, complete functors/variadics and remaining lifecycle routes still prevent
§4.12 completion.

## Additive value-expression graphs

Left-associative `+` now emits `VqlAdd` in existing value-expression positions. Text concatenates;
typed Int64 addition checks overflow; Double arithmetic follows IEEE rules; mixed numeric operands
require exact integer promotion. Full operand streams are checked without truncation. Computed
`string()` and `+` results are materialized as fresh value cells so loop results cannot alias a
reused register; ordinary aliases retain their identity. Allocation/publication rollback and retry
preserve earlier results. The actual §4.12 formatting/destructuring bodies passed, together with
saved graphs, host/mesh routes and a real CLI formatting query. Full build and all 547 affected
tests passed with independent Luna gates; see [iteration 19](projects/vortex-vm-only-iterations.md).
Return-item/top-level grammar, legacy direct consumer routing, sealed normalized-stream artifacts,
lazy field-default continuations, mutation contracts, free closures and remaining
functor/variadic/lifecycle work still prevent §4.12 completion.

## Caller-bound free captures in native frames

Named definitions can receive free scalar/stream values through explicit hidden frame ports,
including recursive and mutually recursive graphs. Callsite scope analysis preserves wrapper
formals, prematch defaults, loop restoration, return-contract locals and caller-time rebinding.
Required field-default aliases retain their missing/empty distinction. Functor/pipeline captures and
lazy parameter defaults with unavailable required-field aliases remain on compiled inline paths.
Full build and all 560 affected tests passed with independent Luna gates; pure-arena export and
fresh-VM reload, real CLI recursion/rebinding and host/mesh source routes passed. See
[iteration 20](projects/vortex-vm-only-iterations.md). First-class functor/lambda captures, native
variadics, nonliteral field-default continuations, mutation contracts and remaining grammar/route/
lifecycle work still prevent §4.12 completion.

## Scoped graph rollback prerequisite

`ScopedChoice` introduces an internal graph-targeted rollback alternative, isolated from logic
oracle directives and work stealing. It is intended for compiled matching continuations: capturing
matches commit successful defaults, while validation-only checks roll temporary defaults back on
both outcomes. Full nonliteral field-default lowering still requires the shared matching driver and
compiled thunk continuations; this substrate does not complete §4.12. See
[iteration 21](projects/vortex-vm-only-iterations.md).

The scope cap includes caller-prefix choices: defaults are 128 total outstanding choices and
1,048,576 aggregate saved-snapshot entries, with per-entry work charging before copies. Internal
cuts also suppress callbacks from nested ordinary choices. Full build, all 580 affected tests from
56 suites and repository formatting passed with independent Luna architecture and senior/profiler
gates. Fresh-VM export/reload and both rollback continuations passed. This establishes neither a
byte-memory bound nor a runtime speedup; the shared driver and default thunk lowering remain next.

## Shared structural matcher driver

An internal continuation driver now owns the single structural matching walk, with a synchronous
arena backend preserving existing contract behavior. Borrowed subject spans and immediate callbacks
avoid copying incoming streams before work checks; flat fields remain iterative. Nested
validation/capture scopes, union order, lexical defaults and generic success rollback have dedicated
parity coverage. Full build, all 588 affected tests from 56 suites and repository formatting passed
with independent Luna architecture and senior/profiler gates. See
[iteration 22](projects/vortex-vm-only-iterations.md).

The graph backend has not yet been wired, so path/call field-default thunks still refuse in compiled
contracts. Mutation contracts, functor ABI and remaining grammar/route/lifecycle work also keep
§4.12 incomplete. This refactor adds no runtime interpreter fallback or measured speedup.

## Lazy computed field-default graphs

Computed field defaults now lower through the shared driver into scoped Vortex graphs for inline
calls, return checks and loop destructuring. Path/call defaults run only on absence with the exact
owner and prematch lexical environment, preserve full result streams, and roll back failed arms or
validation-only success. Direct aliases retain zero/one constraints; lazy literal boxing preserves
identity across loop iterations. Runtime named field probes and request-abort cleanup restore
arena/VM state without refunding fuel; raw stops stay resumable. See
[iteration 23](projects/vortex-vm-only-iterations.md).

Full build and all 612 affected tests from 58 suites passed, with fresh-VM export/reload, host/mesh
source routes and a real headless CLI returning `lazy`. Independent Luna architecture and
senior/profiler gates accepted this bounded slice. Outer functions with computed defaults remain
inline-only; their recursive/native-frame route is next. Sealed artifacts, mutation enforcement,
complete functor/variadic behavior and remaining route/lifecycle work still prevent §4.12
completion. No speedup or full-suite pass is claimed.

## Computed structural defaults in native frames

Eligible named functions now execute computed field defaults and return contracts through reusable
Vortex activation frames, including self/mutual recursion. The shared graph matcher retains prematch
scope, exact owner context, full result streams, staged captures and validation rollback. Matcher
registers are activation locals; existing compile/runtime bounds and request cleanup remain active.
Full build and all 623 affected tests from 59 suites passed with independent Luna architecture and
senior/profiler gates. Recursive graph export/reload, host/mesh source routes, named return failures
and recovery passed. See [iteration 24](projects/vortex-vm-only-iterations.md).

This supersedes iteration 23's inline-only computed-default boundary. Sealed artifacts, mutation
contracts, content-span overflow integrity, functor/variadic completion and remaining grammar/route/
lifecycle work still prevent §4.12 completion. No speedup or full-suite pass is claimed.

## Arena content-run count integrity

Arena setters now refuse runs larger than the physical 16-bit content count before shadowing or
trailing. Quote minting rejects them before consuming work or allocating cells; private content
writes retain a defensive check. Boundary regressions cover 65,535 accepted entries, 65,536 refused
entries, unchanged owned/base state, later rollback and preserved mint fuel. The shared bound
derives from `CellSlot` rather than a second policy constant. See
[iteration 25](projects/vortex-vm-only-iterations.md).

Persistent fold/writer defense, aggregate offset capacity and prefix hydration still need work. This
closes the arena count-narrowing defect recorded during lazy-contract validation; it does not
establish mutation-contract enforcement or complete §4.12.

The full build, repository formatting and all 688 affected tests from 60 suites passed, including
persistent Manifold baseline coverage. Independent Luna architecture and senior/profiler gates
accepted this bounded stage. No full-suite or speedup claim is made.

## Persistent content-run capacity enforcement

Persistent folds now refuse normalized runs larger than `CellSlot` can represent before publishing
target content, value, operation identity or last-operation metadata. Multi-span transclusion
prepares then commits its complete target run. Both local splice writers reuse the fold normalizer
for preflight; text edits check their exact candidate address under the permascroll append mutex
before publishing bytes. Wire-field offsets are checked before narrowing. See
[iteration 26](projects/vortex-vm-only-iterations.md).

The bounded guarantee covers count refusal in folding and these local splice APIs. It leaves general
mutation-contract enforcement, broader post-append failure atomicity, aggregate offset capacity and
prefix hydration open. No performance or full §4.12 completion claim follows.

Full build, repository formatting and all 725 affected tests from 68 suites passed with independent
Luna architecture and senior/profiler gates. Tests include exact-limit acceptance, unchanged bytes
and operations on refusal, and a multi-span transclusion whose early fragment fits but later ones
overflow. No full-suite pass or speedup is claimed.

## Validated parallel prefix baselines

Prefix export/hydration now refuse malformed floors, runs, identities and omitted required state
without publishing replacement arena data. Exported marks normalize journals that are absent from
the snapshot; nested rollback restores correctly. Shadow-prefix preparation and wire-limit checks
happen before stealing a victim choice, with no failed-prefix oracle fallback. Workers reject
invalid snapshots before program callbacks or execution. Checked total APF2 sizing prevents nested
32-bit frame-length truncation. See [iteration 27](projects/vortex-vm-only-iterations.md).

Full build and all 736 affected tests from 68 suites passed with independent Luna architecture and
senior/profiler gates. Tests include valid base-backed round-trip, duplicate-shadow refusal,
populated-destination preservation and real encoded-length agreement. Snapshot copying, maps and
range sorting remain costs; no speedup or full-suite pass is claimed. Omitted federation/provenance
serialization, compatible worker bootstrap, aggregate mutation capacity, active mutation contracts
and remaining functor/variadic/frame/stream/mesh lifecycle work still prevent §4.12 completion.

## Reusable computed return validators

Computed return promises use capture-free native validator frames built by the shared contract
driver. Validators return a Boolean while the original result cells retain their identities; false
raises the named return-contract error. Defaults remain lazy, observe the current owner and roll
back on both outcomes. Noncomputed return checks retain the existing `RequireContract` path and
attached pipelines retain compiled inline matching. See
[iteration 28](projects/vortex-vm-only-iterations.md).

Unused lexical inputs are pruned before frame finalization and caller normalization. The profile
confirms removal of unused-stream copying, while validator invocation remains slower than the inline
control in this small probe. Candidate environment preparation remains an open compile-time cost.
Full build, formatting and all 745 affected tests from 69 suites passed, including source-free
export/reload, recursion, lexical stream order and current-owner revalidation. Active mutation
guards, atomic enforcement and the previously listed §4.12 gaps remain open; this is a bounded
foundation.

## Explicit native guard checking

Runtime graph consumers can install cursor-owned guards, freeze Cell/Stream arguments and check
validator frames asynchronously through ordinary VM execution. Exact-token removal and unique
activation ownership preserve caller guards across returns. Choices retain guard and pending-check
snapshots; close and refusal paths clean private mappings. Export relocates descriptors and refuses
live/restorable state. See [iteration 29](projects/vortex-vm-only-iterations.md).

Full build, repository formatting and all 760 affected tests from 70 suites passed with independent
Luna architecture and senior/profiler gates. Tests include compiler-produced validators, frozen
stream rebinding, strict Boolean results, source-free export/reload, recursive check refusal and
retry against changed owner bits after a later dispatch fails. The opt-in profile records raw
overhead and allocation counts, with no speedup claim. Guard limits are logical counts, not
whole-heap bounds. Compiler parameter activation, semantic mutation transactions and nested
validation scheduling remain open; explicit CheckGuards does not establish mutation enforcement or
complete §4.12.

## Active function parameter contracts and mutation groups

Supported typed function calls now activate guards after successful matches, preserving pre-match
lexical defaults, capture ordering, variadic per-actual filtering and native recursion. Complete
source value/create/clone/weave operations use owned rollback groups. Scope and frame exits validate
their participating guards before retiring them, while caller guards remain active. Failed checks
roll back before reporting named contract errors; caller choices and prior committed groups remain.
See [iteration 30](projects/vortex-vm-only-iterations.md).

Full build and all 789 affected tests from 73 suites passed with fresh independent Luna architecture
and senior/profiler gates. The new cases include aliases, displaced endpoints, literal creation
chains, weave repair, later defaults, native recursion, guard expiry, cursor isolation, budgets,
source-free export/reload and host/mesh source recovery. CLI validation exercises both successful
and rejected mutations. The profile is a cold smoke measurement with no speedup claim; runtime
limits remain logical counts. Guarded execution permits one live guarded cursor at a time.

Effectful nested validation remains explicitly refused before source effects. The compiler now also
refuses unlowered shape projection instead of silently ignoring it. Field weaving/projection,
return-field construction, remaining typed binding/functor/variadic/lambda work, session state and
complete frontend/parallel lifecycle parity remain open. Full §4.12 is incomplete.

## Static shape projection lowering

Static path projections now compile signed dimension masks, wildcard resets and scalar literal
assignments into Vortex graphs. Each owner retains its identity, stream position and duplicates;
assignment cells are freshly boxed with their scalar types. A native mask primitive discovers
unmentioned runtime links and severs reciprocal edges atomically. Source mutation groups integrate
complete projections with active function parameter guards. See
[iteration 31](projects/vortex-vm-only-iterations.md).

The initial subset supports local owners and static canonical dimensions. Foreign and quoted owners,
dynamic rules, computed or nested targets, and negated/bidirectional assignments remain explicit
refusals. The singleton target stage below adds alias and simple-path preparation before masking;
return-field construction and the previously listed §4.12 gaps remain open. No direct evaluator
fallback or full completion claim follows.

The full build and all 807 affected tests from 75 suites passed. Coverage includes exact fresh
scalar boxing, malformed and bounded mask inputs, reciprocal rollback, parser placement in function
bodies, fresh-VM export and host/mesh source routes. A real headless CLI checks successful
projection and named contract rejection. The opt-in four-point profile reports whole-fixture costs
and makes no speedup claim. Function-body parsing now retains a variable's projection within its
guarded scope instead of detaching it into the surrounding script.

## Singleton cell targets in projections

Aliases and bounded read-only paths now prepare every RHS for one owner before masking. The shared
`Cell` contract rejects empty and multiple targets explicitly; selection happens only after that
check. Existing-cell attachments use a fresh clone per owner occurrence through the same graph
helper as `link()`, retaining exact scalar content and the target's other links. Preparation and
assignment remain in the owner's mutation group. See
[iteration 32](projects/vortex-vm-only-iterations.md).

The full build and all 815 affected tests from 76 suites passed. New cases cover real rank
cardinality, pre-mask source reads, exact clone masters and bits, duplicate occurrences, guard
rollback, export/reload and host/mesh source execution. The CLI preserves an Int64 above the Double
exact-integer range and explicitly rejects an empty target. Profile measurements expose growing
shared-clone-rank traversal cost; no speedup or general latency claim follows.

Calls/computed targets, predicates and other deferred path forms, nested shapes, general foreign
projections and ordered projection/traversal remain unfinished. The native-frame stage below extends
projection eligibility. The parser now refuses immediate continuation after projection rather than
interpreting it as another statement; an intermediate `let` works. Nonliteral `d.clone` assignments
refuse, and the existing `link(d.clone, target)` collision is still open. Full §4.12 remains
incomplete.

## Native functions containing projections

Supported projection bodies now reuse native function graphs, including recursion and free aliases.
Compiler and eligibility share the same syntax predicate, while the existing budgeted scanner
preserves capture and hard malformed-AST behavior before classification. First-class shape values
remain refused after traversal; explicit weave definitions and previously excluded callable forms
retain their current graph routes. See [iteration 33](projects/vortex-vm-only-iterations.md).

The full build and all 824 affected tests from 77 suites passed. Tests distinguish the named
function's InvokeFrame from validator calls and cover recursive export, typed rollback, exact clone
content, default/caller scopes and Host/mesh source recovery. The CLI executes an actual three-node
recursive projection graph. Four profile rows record whole-fixture native-frame costs and make no
speedup claim.

Recovery also revealed inconsistent dimension lookup/mint anchors after a home projection detached
its `d.dims` connector. Both now use the stable directory cell, so new names retain one identity
without restoring removed topology. A core regression isolates that behavior and the Host recovery
case remains intact. General foreign-owner projections, ordered projection/traversal, other deferred
projection targets, callable forms, nested effectful validation and complete frontend/lifecycle
parity still prevent full §4.12 completion.

## Reciprocal foreign-link kernel prerequisite

Arena link replacement now clears inherited foreign reciprocals through canonical proxy identities.
A shared bounded write plan preflights link-row capacity before copy-on-write shadows, and proxy
construction rolls back failed mint/index/cache publication. Ordinary local updates add no
savepoint. See [iteration 34](projects/vortex-vm-only-iterations.md).

The full build and all 838 affected tests from 77 suites passed. Fourteen new cases cover source
immutability, cross-space identity, reciprocal edits, base-backed bookkeeping, capacity and budget
failures, rollback/retry and namespace collisions. The CLI preserves an exact large Int64 through a
native projection function. Twelve opt-in profile rows record elapsed time and arena slots; they
establish neither speedup nor heap-allocation bounds.

The broader kernel-consumer sweep passed 127 of 136 tests, skipped one swarm case, and retained
eight failures in unchanged unsupported VLace/VQL routes and an exception-class expectation. This is
not a full-suite pass; those frontend gaps remain on the specification backlog.

Proxy creation temporarily refuses collisions between its index axis and authored source edges,
including an unmapped literal same-named source axis. This is collision detection, not implicit
identity binding. General binding changes after proxy edits and axis isolation remain open. The
following snapshot and foreign-mask stages add a bounded subset; the previously listed
callable/frontend/lifecycle gaps still leave full §4.12 incomplete.

## Bounded effective-link discovery

An owning arena snapshot now merges inherited proxy topology with per-direction local overrides
without minting neighbors. Local endpoints remain CellRef; inherited endpoints retain their source
space and cell identity. Candidate, directed-edge and work limits apply before unbounded discovery,
and failed work consumes arena fuel. See [iteration 35](projects/vortex-vm-only-iterations.md).

The checked dimension mapper prioritizes exact arena keys and rejects conflicting raw aliases.
Snapshots conservatively refuse name-dependent one-space mappings rather than invoke the legacy
unbounded name lookup; the existing read wrapper keeps that fallback. Missing nonempty source axes
refuse explicitly, and separate aliases retain independent directional overrides.

The full build and all 849 affected tests from 78 suites passed. Eleven new cases cover identities,
partial topology, limits and refusals; six profile observations show zero arena-cell growth without
claiming zero heap allocation or speedup. The broader sweep retained eight unfinished frontend
failures, with 128 passes and one skip. The foreign-mask stage below now consumes these snapshots.
Shared bounded name resolution, general index-rank mutation/cache lifecycle, callable forms and full
frontend parity remain unfinished; full §4.12 is incomplete.

## Masks over explicitly bound foreign proxies

Foreign proxy masks now execute through the same VM primitive using effective snapshots, complete
removal planning and one outer rollback scope. The local path remains unchanged. Masks must preserve
the owner's actual filing-axis links; quote owners and name-dependent one-space mappings still
refuse before effects. See [iteration 36](projects/vortex-vm-only-iterations.md).

The index-axis identity is captured per space and invalidated permanently when release truncates it,
including same-name/reference reuse. This protects that cache without claiming general space
attachment rollback. Full build and all 862 affected tests from 80 suites passed. Coverage includes
late-mint rollback of earlier overlays, host native-frame calls with duplicate foreign contexts,
exact fresh Int64 assignments and source-free artifact reload with runtime foreign inputs. Source
stores remain unchanged. Four raw profile observations report VM dispatch counts and arena slots,
with no speedup/heap claims. The broader sweep passed 130 of 139 tests, skipped one, and retained
the same eight unfinished frontend failures.

The following shared lookup stage now budgets proxy collision-name diagnostics separately from
snapshot/planning work. General filing-rank mutation, federated mesh input, remaining callable forms
and complete frontend/lifecycle parity remain open. Full §4.12 is incomplete.

## Shared bounded dimension-name diagnostics

One matcher now serves legacy and bounded dimension-name lookup, preserving registry priority,
directory order, exact text and one-hop aliases. Bounded lookup separates typed resource refusal
from a missing name, preflights bytes/spans before reads, charges CSR probes and work, and leaves
derived caches unchanged. Proxy construction shares one configurable request budget and active arena
fuel across mapping, source/tail lookup and collision probes. Legacy reader compatibility is
retained. See [iteration 37](projects/vortex-vm-only-iterations.md).

The full build, all 877 affected tests from 81 suites and 92 additional ZigZag tests passed. New
cases cover cache, alias, duplicate/home, UTF-8, fragment, reader and rollback/fuel behavior. The
large-directory probe records an actual miss and a typed refusal at the configured visit cap; budget
bytes and reader bytes are reported separately, with no speedup or memory-usage claim. The broader
sweep retained eight unfinished failures, with 132 passes and one skip.

This closes the unmetered collision-name scan in the new proxy path, not every legacy reader or a
universal VM CPU bound. Name-dependent one-space snapshots still require integration with this
shared bounded resolver. Index-rank mutation, general lifecycle, callable forms and full frontend
parity still leave §4.12 incomplete.

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
every exit. The VM-only ledger records 238 passing focused tests for this bounded iteration;
recursive frames, path/call field-default thunks and atomic mutation enforcement remain open.
Variable-alias field defaults now capture prior lexical VM slots lazily, preserving identity for
zero or one fallback cell. Larger streams refuse explicitly. This alias increment passes 261 focused
tests; it does not establish general lazy-thunk support.

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
- **§4.12:** complete lazy/runtime stream semantics, recursive frames, variadic
  defaults/dot/destructuring, nonliteral shape-field default thunks, mutation-boundary enforcement,
  functor shape/pin/yield completeness, session-state/frontend parity, and specialized
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

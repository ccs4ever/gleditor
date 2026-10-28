# VQL parity implementation iterations

## Objective and release policy

Implement §4.12 of `design/vql-query-language.md` first, then the remaining specification work.
Direct and Vortex execution must have complete feature parity across the applicable frontend paths.
No new feature is accepted on the strength of direct-mode tests alone. Existing parity gaps are
blockers, not supported alternatives. Internal milestones do not establish §4.12 completion.

Each iteration requires independent Luna architecture and senior-engineering gates for quality,
correctness, and fitness for purpose. Profiling review rejects unmeasured ABI, allocation, and
timing claims. Builds run only after all source writers freeze their files.

## §4.12 priority iteration

### Foundation milestone: bounded gate accepted

- Extract one structural matcher from the direct engine, shared with actual VM instructions.
- Encode portable, versioned type descriptors; no process-local AST registry or direct fallback.
- Execute nonrecursive typed calls through VM instructions with parameter filtering, reference
  captures, guarded result slots, lexical scope restoration, and return-contract errors.
- Differential tests compare actual results and failure behavior, not compilation success alone.

This milestone is intentionally narrower than §4.12. The full feature gate remains closed.

### Required before the §4.12 gate can pass

- Complete runtime stream semantics, deferred variadic forms, recursive frames, and parallel/mesh
  stream lifecycle. The eager rank foundation below is implemented.
- All four unified contract modes over runtime streams, aliases, and nonliteral contract-field
  default thunks. Bounded optional/union contracts, nested calls and parameter expression defaults
  are implemented.
- Atomic mutation-boundary enforcement and identical failure semantics.
- Complete functor composition/application/capture semantics, including predicates and clamps.
- CLI, host/palette, mesh, selector, and exported/reloaded-program parity and error propagation.
- Measured evaluation of specialized functor lowering with full-width dimension references and an
  arbitrary-length fallback. Unsupported performance/ABI assertions must be corrected.

## Subsequent iterations

Work through the remaining entries in `design/vql-implementation-status.md`, implementing each
feature in both execution modes and running the frontend parity matrix before acceptance. This
includes streaming/materialization, shapes/functions, distributed discovery and watches, dynamic
lenses, complete handles/projections/history, and reachability collection. These remain unfinished
until their respective implementation and review gates pass.

## Validation log

The starting worktree passed 360 focused tests. The resumed blocker fixes passed 387 tests;
predicate control flow passed 389; omitted expression defaults and mixed-candidate filtering passed
391 tests. Current validation is recorded in
[implementation status](../vql-implementation-status.md).

## Current gate state

Fresh Luna architecture and senior reviews accepted the bounded foundation after exact exporter
content/value preservation and consistent positional-only user-call validation. The resumed focused
run passed 387 tests, including CLI, host entry points and actual exported/reloaded VM execution.

The next predicate iteration passed 389 tests. Step predicates retain or clear identity-bearing
runtime slots; missing candidates skip predicate work; AND/OR short-circuit through actual VM
branches. Both Luna reviews accepted the control flow. The omitted-parameter expression-default
iteration then passed 391 tests, including mixed-candidate identity and lazy default errors; both
reviews accepted it.

The argument/functor iteration prepares explicit arguments in caller scope, retains untyped functor
captures and lowers captured step vectors in the VM. Qualified functor anchors refuse explicitly
instead of discarding their modifiers. Both Luna code gates and the full build passed; validation
also covers the compiler AST guard independently of the remaining argument-parser limitation. These
iterations did not establish runtime variable-width stream/rank traversal, general recursive frames,
nonliteral contract-field default thunks or mutation-boundary enforcement. The final focused run
passed all 396 tests, including the qualified-anchor AST regression. The full §4.12 gate remains
closed. The full build, formatting gate, 830 library tests, 256 ZigZag tests and all 17 swarm/
publication network tests passed. The broad Xuzz run and corrected-test rerun are documented in the
implementation status; unrelated existing typography-policy failures still block repository lint.

## Runtime rank and eager stream foundation

Actual VM opcodes now own stream accumulators and forward/reverse sequence and rank iterators.
Runtime loops walk ranks, map user calls over each member, filter parameter contracts, check
whole-stream cardinality, and flatten results without losing identity, order or duplicates. Step
predicates and index clamps run after traversal; clamps use checked integer arithmetic and count the
filtered stream. Bidirectional reads run POS then NEG; endpoint selectors preserve the input when no
neighbor exists. Read-side `!keep` returns the original input after selector/filter/clamp work.

Whole-rank reads first find the negward extreme and walk POS including that head. Negative `::rank`
reverses that canonical sequence using a reverse chunk iterator with work charged per member. A
foreign overlay can expose asymmetric edges before all proxies exist; a backward topology walk is
not equivalent to reversing the forward result. A targeted regression caught that distinction.

Default creation seeks the current tail at runtime. An outputless `New` mints a distinct cell on
each execution, and a following `Link` captures its identity. Existing neighbors survive later
appends. Unsupported placed creation and historical pins refuse, including pins embedded in captured
functors.

Stream and iterator state uses copy-on-write snapshots, participates in choice-point rollback, and
charges work per member. Rank iteration uses the arena's `linkedMinting` API to resolve foreign
neighbors with translated dimensions and canonical proxy identity. A fixed initial arena bound is
not valid while proxy cells are being created; absent edges and seen identities terminate traversal.
Tests cover real-store ranks, cycles, local overlays and proxy-mint rollback/retry. Work exhaustion
before a hop mints no proxy. Compiler home/named-store anchors use the coordinator's actual cells;
unsupported cursor/generator/historical anchor kinds refuse instead of returning a placeholder home.

This remains an eager sequence foundation. One stream cursor may own a VM at a time. Overlapping
live cursors, ownership changes across outstanding choice points, and stealing stream choice points
refuse explicitly. Finished payload maps expire when a new owner starts, while typed markers prevent
expired registers from becoming scalar values. Low-level VM runs may resume after a cycle limit;
`QueryExecutor` abandons its request cursor on every exit because it exposes no resume handle.

Exports serialize instruction plans and graph-referenced loop targets, reconstructing streams in a
fresh VM. Foreign/base-backed export remains refused. Mesh rejects stream opcodes until transport
and worker lifecycle are supported. No direct-evaluator fallback is used.

Luna architecture and senior reviews accepted these bounded algorithms; source inspection by the
architect was partly limited by tool quota, with exact source supplied for the remaining transport
and foreign-hop review. The current full build and formatting gate passed; the full Xuzz binary
passed 2,350 tests with 27 skips, and separate library (830) and ZigZag (256) runs passed. The
profiler rejected a uniform speedup claim. Full §4.12 stays incomplete.

### Measurements

`make -j$(nproc) vql-rank-benchmark` builds `tools/vql-rank-benchmark.cpp`. With the six
headless/XDG environment variables set, run `build/vql-rank-benchmark --rank-size N --repeats 10`
for `N = 0, 1, 16, 256, 4096`. Construction is outside timing; five warmups precede measured runs;
every run checks exact result identity and order. Build: Clang 23.1.1, `-O3 -g -std=c++2c`,
SDL3/Vulkan; host: AMD Ryzen Threadripper 1950X, 32 logical CPUs.

The final transport graph has 20 opcodes and uses `17 + 8N` VM steps for this query, stable across
ten repeats. At N=256, median/p95 execution was 1,552.89/1,944.34 microseconds; at N=4096 it was
26,319.2/27,726.2 microseconds. These are one-run observations, not latency guarantees or an
allocation measurement. Earlier 15-opcode graphs are different workloads; their timings cannot
establish a same-graph optimization. Copy-on-write measurements on the earlier graph were mixed.
Artifacts: `/tmp/vql-stream-final.jsonl`, `/tmp/vql-stream-profile-metadata.txt`, and
`/tmp/vql-stream-profile-comparison.txt`.

A second final-graph run after the foreign/reverse corrections retained exact output and the same
cycle totals at every size (`/tmp/vql-stream-final-repeat.jsonl`). It overlapped the full Xuzz test
process; its timings are not isolated and establish no performance improvement or regression.

### Still required

Lazy universal streaming, recursive frames, variadic defaults/dot/destructuring, nonliteral
contract-field default thunks, atomic mutation-boundary checks, functor shape/pin support, complete
session/selector semantics, and parallel/mesh/export parity remain release blockers. No specialized
functor ABI or performance claim has been established.

## Annotated functor and query-dereference iteration

Bound functors now dispatch independently of their call-site modifiers. The shared compiler step
pipeline executes the captured body over the whole input stream, then applies dereference,
predicates, clamp and `!keep` once to the combined output. An outer clamp therefore does not restart
for each input member or move onto an individual captured hop. Unknown functors refuse rather than
minting a dimension named after the variable. Shape predicates and historical pins still refuse;
this iteration preserves existing direct behavior for functor `!last` and `!both` rather than
claiming their complete specification semantics.

One capture helper is shared by direct and compiled binding. Bare references splice captured steps;
decorated references retain their signed-dollar selector and attach an owned captured step sequence.
Keeping that selector preserves empty-input dispatch: a captured function can still produce a value
without an incoming cell. Later rebinding or lens shadowing does not change the already captured
functor. Nested AST nodes are read-only after parsing; capture does not mutate the source AST.
Runtime capture metadata lowers to VM instructions and is not serialized in executable artifacts.

Query-time master dereference now probes canonical foreign neighbors with work charged before each
probe. Ordinary cells retain their identity, chains yield their far master, and any repeated
identity returns the original cell, including asymmetric foreign overlay lassos. The compiler lowers
the same relation to rank iterations, a terminal link probe and reference-copy branches. The
coordinator's raw noexcept lookup used for store registration remains separate. Anchor dereference
runs after every anchor-resolution route, including multi-cell variable/context bindings. Exact
total fuel thresholds are not comparable across backends because their iterator/transport work
differs; zero-fuel refusal, no proxy mint after exhaustion, and recovery are tested in both modes.

The focused run passed 433 tests, including CLI, host query/navigation/script/batch, and typed
save/reload/fresh-VM execution of decorated captures. Luna senior source review accepted the final
implementation; architecture review accepted the supplied design with the listed passing identity,
empty-input and budget regressions. The full build/format gate passed; the full Xuzz binary passed
2,354 tests with 27 skips, and all 256 ZigZag tests passed. Lint remains blocked by the two
unchanged UI typography defaults. Full §4.12 remains incomplete.

### Annotated probe

The rank probe accepts `--functor` and `--annotated`; the latter preserves the full rank using
`[true][1,-1]`. Fifteen cases (plain/captured/annotated at N=0,1,16,256,4096) each passed exact
identity/order checks with five warmups and 30 measured repeats. No own build or test process ran
concurrently. Plain/captured plans both used 20 opcodes, 74 additional compile-time arena cells and
`17 + 8N` VM cycles. Annotated plans used 65 opcodes and 240 additional cells; empty input used 31
cycles and nonempty input used `40 + 35N`. These counts describe the measured queries, not a
specialized functor ABI or general per-hop guarantee.

At N=4096 the observed median was 26,284.7 microseconds plain, 28,756.8 captured and 110,565
annotated. The annotated graph does more work; this is not an optimization comparison. Compilation
timings include parsing, first-process rows have cold outliers, and allocations were not measured.
The Luna profiler accepted these reporting limits. Artifact: `/tmp/vql-annotated-probe.jsonl`; build
flags and host metadata match the earlier rank probe.

## Named variadic calls and caller argument activation

Named final variadic parameters now have a differential runtime/frontend/export gate. Each actual
argument is checked against the rest parameter's type before its result stream is concatenated in
source/member order. `...$xs: Int` therefore rejects one multi-cell actual; `...$xs: Stream<Int>`
can accept that actual. Empty rest streams, duplicate identities, fixed defaults, UFCS and repeated
calls are covered. Variadic shape checks do not introduce capture bindings, matching direct
evaluation; matcher output slots still exist in the VM instruction graph.

One shared signature validator checks all declarations before registration or lowering. Non-final or
multiple variadics and the currently unsupported defaulted/dot/destructured rest forms fail
explicitly in both modes. Nonvariadic defaults and existing extra-argument behavior are unchanged.
These deferred forms still prevent a claim of complete variadic specification support.

Explicit arguments now compile in caller scope before the callee is marked active. Finite nested
calls such as `identity(identity(7))` and `collect(collect(1,2),3)` compile and execute, while true
recursive bodies and recursive defaults remain refused pending VM call frames. Guarded scope cleanup
also handles argument-preparation failures.

Luna implemented the binder changes, runtime regressions and CLI/host/fresh-VM export cases.
Independent architecture and source reviews accepted the bounded design. All 446 focused tests
passed; the full build and formatting gate passed. Logs: `/tmp/vql-variadic-focused.log`,
`/tmp/vql-variadic-full-build.log`, `/tmp/vql-variadic-format.log`.

Profiling review identified an existing accounting edge: VM contract matching copies a whole result
stream before charging its individual members, and direct matching can inspect prebound subjects
without its own per-member fuel charge. The accounting iteration below addresses that boundary; no
performance or allocation improvement is claimed here. Full §4.12 remains incomplete.

## Contract traversal and collection accounting

A borrowed `forEachResultCell` visitor now supplies the same scalar/stream/iterator resolution as
`resultCells`. It retains no callback, and callbacks must not mutate VM register maps during the
visit. `resultCells` still returns a materialized vector and reserves known live stream capacity. VM
contract collection uses the visitor and charges each member before appending it to its matcher
input buffer, avoiding the prior extra per-input result-vector copy. The matcher buffer remains a
vector; this is not zero-copy execution or an allocation/speed guarantee.

The shared matcher charges each recursive type visit and each destructured field before its
lookup/proxy/default work. Tests supply prebound subjects independently of argument generation,
exercise exhausted fuel before default callbacks and cell minting, verify no partial caller captures
from a failed wrapped match, and recover after resetting limits. VM tests prepare a stream before
limiting work, exhaust during subject collection, and retry the same instruction successfully with
unchanged identities/order. Frontend zero-fuel tests separately verify structured budget errors and
later request recovery; they may fail at request entry rather than at the matcher probe.

All 452 focused tests passed, as did the full build and formatting gate. Luna implemented both the
VM visitor and matcher charging, then cross-reviewed each other's changes. Architecture review
accepted the bounded algorithm. No persisted opcode/format changed and no direct evaluator fallback
was introduced.

These are logical fuel boundaries when arena execution limits are enabled, not a bound on every
allocation or CPU instruction. Environment snapshot/preparation copies, general lazy streaming,
atomic mutation contracts and the remaining §4.12 work stay open. Exact total fuel thresholds are
not cross-backend equivalence guarantees. Logs: `/tmp/vql-contract-budget-focused.log`,
`/tmp/vql-contract-budget-full-build.log`, `/tmp/vql-contract-budget-format.log`.

The combined regression run for the variadic and accounting iterations completed with 2,373 Xuzz
passes and 27 skips (2,400 tests run, plus one disabled test), and all 256 ZigZag tests passed. Lint
still stops at the same two unchanged UI typography defaults. The library and rootless swarm stages
were not rerun for these engine-only changes; their preceding successful runs are recorded above.
Logs: `/tmp/vql-iterations-full-xuzz.log`, `/tmp/vql-iterations-zigzag.log`,
`/tmp/vql-iterations-lint.log`. Full §4.12 remains incomplete.

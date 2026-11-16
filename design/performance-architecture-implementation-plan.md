# Performance architecture implementation plan (v2)

## 1. Authority, scope and invariants

The owner accepted P1-P5 in
[performance-architecture-proposals.md](performance-architecture-proposals.md), including release
ThinLTO and PGO. This is the execution plan for those accepted proposals. It creates no second
approval gate for the same work. The measurement gates decide whether a candidate implementation or
its documented fallback is retained; acceptance does not assert an unmeasured speedup.

This document plans work; no milestone is implemented or measured by writing it. The original
proposal's gprof samples are hotspot leads, not release measurements. The independent C++ review and
its fixes are recorded in
[round 8](projects/performance-architecture-proposals-review/r8-cpp-performance.md).

Preserve C++23 and the Makefile's compiler/standard probes; use no CMake and edit no submodule
contents. Keep the library and plain editor free of Xanadu dependencies. All lookup/trail structures
are ephemeral host implementation state, not new persisted semantic metadata. Keep the 64-byte
CompactOpNode layout/alignment, immutable stored operations, Merkle piece size, R7 cell identity, R8
view isolation, R9 replay honesty and R12 dimension uniformity unchanged. Keep optional absence and
typed refusal at API boundaries. No fixture-format migration is planned.

Relevant guidance: [README](../README.md#library-and-rendering), [testing](../docs/testing.md),
[store/slice rulings](store-slice-convergence.md), and
[Vortex runtime](vortex-hyperstructural-runtime.md).

## 2. Dependency and commit order

Implement one reviewable milestone at a time in this order. M3 and M4 have separate experiments;
neither depends on the other beyond the baseline/harness. M5 consumes their evidence and helpers.

| Milestone | Accepted proposal | Deliverable                                                            | Depends on             |
| --------- | ----------------- | ---------------------------------------------------------------------- | ---------------------- |
| M0        | P1-P5 support     | Reproducible release controls, corpus manifests and cost counters      | None                   |
| M1        | P4                | ThinLTO+PGO build/train/use pipeline with controls                     | M0                     |
| M2        | P3, P4            | Guarded link fast path and separately measured accessor inlining       | M1                     |
| M3        | P2                | Dense/presence and sparse lookup representations                       | M2                     |
| M4        | P5                | Inline/spill operand container and complete consumer migration         | M2; scheduled after M3 |
| M5a       | P1                | Reversible containers first, then savepoint/trail prototype and oracle | M3, M4                 |
| M5b       | P1                | Boundary-aware mutable payload undo, scoped lifecycle and limits       | M5a                    |
| M5c       | P1                | Table tuning and retirement of production snapshot copies              | M5b                    |
| M6        | P1-P5, P4         | Retrained final release, full correctness/performance report           | All preceding gates    |

Each milestone's commit includes code, meaningful regression checks, documentation and measured
results or the explicit reason a proposed fallback was retained. Keep accessor inlining and P3
separate commits inside M2 so their effects remain attributable. Do not mix P1's data-structure
migration with its initial undo-algorithm change.

## 3. Cross-cutting execution and evidence contract

### 3.1 Builds and isolation

In a fresh worktree initialize submodules before building. All builds use `make -j$(nproc)`; all
tests/programs run headlessly with all six environment variables from docs/testing.md. The Makefile
supplies them for Make children. A direct script must also export isolated XDG data, config and
cache plus offscreen video, dummy audio and software GL. Reuse source fixtures only with their
associated permascroll; use disposable stores and author state for generated workloads.

Current flags are appended at `Makefile:498,555`; `.buildflags` currently records compile/link flags
at `Makefile:893-902`. M1 extends that stamp rather than introducing concurrent variants in the
shared build tree. Run variants serially. Include compiler identity, target/ABI, SDL/backend
features, optimization mode and profile digest in the stamp, and make object and link outputs
respond to it. A changed profile at the same pathname must rebuild profile-use outputs.

The stage driver refuses concurrent profile pipelines in one tree and records interrupted-stage
state; it must never reuse a partially completed profile or release. Stage artifacts live under
`build/performance/`; normal application outputs stay in `build/` for existing orchestration tests.
Do not copy an executable alone and accidentally run it against a different shared library. Record
binary/library digests and run every variant before changing the build mode.

### 3.2 Workloads, counters and decision rule

M0 adds a versioned manifest with disjoint training and held-out input sets. Cover text-heavy,
branch-heavy and structure-heavy stores; local/quote/proxy traversal; VQL/VPL/VProlog plus other
built surface-language calls; stream/rank iteration and nested choices; and plain-editor layout and
backend rendering. Use existing probes where they fit (`tools/layout-latency-probe.cpp`,
`tools/benchmark-kjv-load.py`, `tools/vql-rank-benchmark.cpp`), extending rather than duplicating
semantics. Record which executable and scenario exercises each target subsystem. Training weights
are explicit corpus/iteration weights, not incidental unit-test execution counts.

Configure warmups and repeat counts in the manifest (initially one warmup and seven paired runs,
increase when variation prevents a conclusion). Record per-run wall time, distribution/spread, peak
RSS, allocations/bytes and subsystem counters; collect hardware cycles/instructions when available,
and report their absence rather than substituting instrumented gprof time. Fix the compiler,
machine, driver, feature flags, cache conditions and corpus for a comparison. Timing runs must not
include allocation instrumentation; collect that evidence in separate runs of the same workload.
Gate structural equality and deterministic work-count properties independently of time.

Require a benefit distinguishable from baseline variation on the intended workloads, with all
regressions and confidence/uncertainty reported. A noisy result is inconclusive, not a pass. Choose
the existing representation/fallback if the candidate's complexity/cost is not justified.
ThinLTO+PGO remains the accepted release objective: investigate release regressions and repair the
training/build pipeline rather than silently relabeling an unprofiled build as optimized release.
Performance acceptance is not a hard wall-clock unit-test assertion.

## 4. M0: Baseline and observability

1. Record source/submodule revisions and dirty-file digests, toolchain/linker versions, machine,
   build flags and corpus identities. Build the current ordinary `-O3 -g` control after `5d604feb`;
   do not include `-pg`, coverage or sanitizers in release timing evidence.
1. Run existing focused tests and baseline workloads. Add counters only behind test/probe
   instrumentation or named diagnostic categories; keep document text and credentials out of logs.
   Preserve stdout protocol/benchmark output where it already exists.
1. Capture byRef useful-entry density and initialization work, wing arity/empty/spill distribution,
   occurrence fast-path eligibility, snapshot copies, mutable-payload copied elements, active
   boundary counts, trail/table capacities, and root-steal/rebase/move work.
1. Add a machine-readable report with commands, raw results, profile/build identity and decision
   fields. Results are per variant and workload, not sums of old gprof percentages.

**Gate:** reproducible corpus and control commands; current correctness passes or pre-existing
failures are explicitly separated; no claimed optimization result yet. P4's scripts/report files in
§11 own this infrastructure because it orchestrates C++ builds rather than Vortex computation.

## 5. M1: Release ThinLTO and PGO pipeline (P4)

Planned Make targets below are deliverables, not commands available today. Provide explicit control,
ThinLTO-only, PGO-only and ThinLTO+PGO modes; release uses the last one for the supported Clang
configuration. Existing compiler selection stays intact. Probe compiler, linker and matching profile
tools with a small compile/link/profile-use check; an unsupported toolchain fails with an
explanation. GCC keeps its own LTO/PGO path, labeled as a portability alternative, not ThinLTO.

1. `perf-pgo-generate`: build instrumented training executables and the matching shared library. Use
   compiler-specific optimization profiling without coverage mapping or `-pg`. Keep ordinary
   `make profile` coverage behavior separate (`Makefile:1472-1515`).
1. `perf-pgo-train`: run each manifest training workload headlessly. Use collision-free raw profile
   names for subprocesses/modules; refuse missing expected workload outputs or unsuccessful runs.
   Retain raw profiles and provenance until a successful merge is verified.
1. `perf-pgo-merge`: merge only this training generation's profiles with matching tools. Write the
   merged profile and manifest atomically. Record profile digest, source/target/toolchain identity,
   semantic flags and training recipe/input digest. Expected generation/use optimization-flag
   differences are normalized explicitly; ABI or source differences are not normalized away.
1. `perf-release`: require that compatible manifest/profile, rebuild with profile use and ThinLTO,
   and verify final compile/link commands and output digests. Refuse missing/corrupt/stale profile
   data or mismatch diagnostics rather than falling back silently. Cold untrained functions are
   allowed and reported as coverage gaps; source/profile incompatibility is an error.
1. Exercise profile replacement at the same path, wrong source/target/compiler, interrupted merge,
   repeated training, subprocess collisions and switching between control/generate/use modes. Keep
   Make's jobs parallel inside each stage while stage transitions remain sequential.

**Gate:** all four modes build/link; generation output is never used as release; the final release
passes real tests and backend comparisons. Compare the four modes on held-out inputs, recording text
size, build/link/training time and peak memory as well as runtime. Do not remove the control mode.
Retrain for each later source revision used as a release candidate or measurement variant; PGO
profiles are never carried across incompatible M2-M5 revisions.

## 6. M2: Guarded lookup and accessor changes (P3/P4)

### 6.1 P3 linked fast path

Change only `ArenaManifold::linked` (`arena_manifold.cpp:1625-1682`). Preserve its no-dense/base
path. Non-ExternRef slots use the local scan immediately. ExternRef slots first exclude quote
occurrences; only actual proxies use shadow/foreign resolution. Quote values are mutable
(`setValueBits`, `arena_manifold.cpp:2135-2145`), and `foreignOf` canonicalizes occurrences, so
construction with None is not a proxy-classification invariant.

Extend `tests/xuzz/arena_manifold_test.cpp` once: canonical foreign edge plus an occurrence with
absent or different local edge, set ExternRef, query both directions, change value and roll back
marks. Check local/base/proxy paths against the previous implementation. Count qualifying calls and
measure release traversal before/after; do not assume 99 percent are eligible.

### 6.2 P4 accessor visibility

Inline only uint32-index spool accessors, `Manifold::slot` and `ArenaManifold::slot`; preserve
bounds/base/refusal behavior and keep `get(MicroversionId)` out-of-line. Sources are
`segmented_ops_spool.cpp:397-413`, `manifold.cpp:126-139`, and `arena_manifold.cpp:1451-1456`.
Retain all required complete-type dependencies in headers without bringing bencode/hash
implementation into the public path.

**Gate:** focused arena/spool/manifold tests, full compilation and release comparisons separating
P3, header inlining and LTO/PGO effects on identical source variants. Inspect actual inlining and
text size. Keep existing out-of-line order/definitions if their measured alternative is preferable;
this does not remove the accepted release pipeline.

## 7. M3: Manifold lookup representations (P2)

1. Introduce an engine-private flat map with explicit occupancy for arbitrary sparse uint32 keys; no
   dependency on the graphics library. Use it for sparse byRef overflow and structureBirths. Bound
   load occupancy and define growth/rehash, erase, copy/move, iteration and comparison semantics;
   allocation failures must preserve the prior logical map. Share this helper with P1 only if the
   VM's reversible-entry contract can be met without weakening replay behavior.
1. Preserve `applyStructure(...) noexcept` (`manifold.hpp:792-793`), but make **new lookup
   allocation failure explicit and pre-mutation**. Add planned
   `FoldRefusal::LookupCapacityUnavailable` and its `toString` mapping in manifold.hpp. Preflight
   all dense/presence/overflow and birth-table capacity needed by the operation before changing
   foldedThrough, slots, content, links, lastOp, mappings or semantic refusal counters. A
   nonthrowing preparation helper catches allocation and capacity-overflow failures; its commit
   inserts into prepared storage without allocation. On preparation failure return this resource
   refusal directly, not via `refuse()` (which counts invalid authored operations). The logical
   manifold and fold position remain unchanged; capacity may have grown during preparation. This
   guarantee covers new lookup storage, not a claim to repair all pre-existing noexcept allocation
   paths in the replay engine.
1. `advance` propagates this typed refusal without advancing. `advanceOrRefold` and both Store
   rebuild paths must distinguish it from semantic refusals: stop and throw a planned typed
   `ManifoldCapacityUnavailable` exception rather than skipping an operation, refolding endlessly,
   or returning a partial success. Store rebuild already permits exceptions; declare the new
   exception in manifold.hpp. Existing semantic-refusal counting/continue behavior remains intact.
   Audit actual callers and tests; this is an in-memory error/API addition, not an on-disk version
   change. Do not catch resource exhaustion as an unreadable user file or regenerate system config.
1. Add a Store fold-capacity hint (`store.cpp:529-542`) and dense mapping at `opIndex - 1` with a
   separate presence bitmap. Refuse operation zero before subtracting; sparse/ephemeral keys never
   trigger an unpaged direct allocation. Implement the accepted U-P2 threshold using widened,
   overflow-checked arithmetic; dense lookup consults sparse overflow when needed.
1. Cover every operation-chain insertion in `Manifold::applyStructure` (`manifold.cpp:321-623`), not
   just MakeCell. Preserve optional `denseOf` and valid dense slot zero. Preserve copy/move,
   equality/honesty, iteration and compact/rebuild semantics (`manifold.cpp:29-103,671,1424-1476`).
   Audit public `byRef` access (`manifold.hpp:903`) and callers before changing its type.
1. Price actual dense capacity at 4B/entry plus one presence bit, page-rounded; structureBirths
   targets 48B/slot with capacity 1.5-2 times live entries. Record growth peak old-plus-new
   capacity, initialization/page-fault work, virtual reservation and committed/RSS memory
   separately.

**Gate:** extend `manifold_test.cpp` and existing replay/honesty tests for missing/zero mappings,
chain ops, sparse high keys, copy/move, mixed overflow and reset/rebuild. Compare incremental/full
folds and refused-operation counts. Inject failure at each new lookup preparation allocation;
compare the logical pre-operation state, foldedThrough and counters, then retry the same operation
and compare to full rebuild. Verify Store never returns a partial fold on resource refusal and
semantic refusals retain their existing accounting. Run text/branch/structure-heavy measurements.
Retain reserved unordered maps where dense/flat costs do not justify replacement; publish the
selected policy. No persistent layout changes or sample regeneration should be needed.

## 8. M4: Operand buffers and consumers (P5)

1. Add engine-private `OperandList` in `vortex/operand_list.hpp` using the repository cpp26 wrapper:
   provisional eight-inline cells plus vector spill. Provide mutable contiguous access, indexing,
   size/empty/front/back and iterators/span. The ninth element moves the prefix in order into the
   spill; no implicit vector conversion and no operand-arity ceiling. Price the target 64B return
   object and verify actual ABI size; defaulted copy/move must preserve active storage.
1. Migrate core wing/input/output declarations and VM wrappers together. Preserve master/reference
   resolution and runtimeSlot rewriting (`vortex_core.cpp:382-404`, `vortex_frames.cpp:326-335`).
   Audit every caller and explicit vector boundary, including VM, stdlib, VQL/VPL compiler and
   `tests/xuzz/vortex_core_test.cpp`; use explicit owned conversion only where required.
1. Keep `getOpcodeKind` register exclusions, map lookup and numeric fallback in their current
   precedence (`vortex_vm.cpp:576-591`). U-P4's instruction-only shortcut stays deferred even though
   P1 later changes the map representation.

**Gate:** compile every app/test; compare arities 0,8,9 and large variadic wings, runtime-slot
mutation and copy/move/spill failure behavior. Test integer-valued register cells overlapping opcode
ordinals. Compare smaller inline capacities and visitor/span alternatives using M0's arity and
allocation data; choose the measured threshold or retain vector fallback. No language syntax or
arity limit changes are authorized by this representation migration.

## 9. M5: Conditional VM undo and flat state (P1)

### 9.1 M5a: Reversible containers, mutator inventory and local savepoints

Inventory all writes, clears, erases and swaps to the ten ChoicePoint containers, cursorPC and
streamCursor, including frames, streams, bindings, closures, guards/contracts, resume cleanup and
abort paths. Include `vortex_resume.cpp:62-99` and adapt the concrete map/set parameter boundary in
`vortex_comparison.cpp:24-45,197-200`. Resume-token IDs/values stay untrailed, but the output
reference and failed resume initialization must restore exactly as before. Existing snapshot state
is in `vortex_vm.hpp:520-543,871-885`; local rollback copies are in
`vortex_frames.cpp:401-408,625-632`, `vortex_closures.cpp:864-883` and
`vortex_bindings.cpp:238-258`. Resume values/token IDs remain untrailed control state.

Before enabling any trail rollback, deliver an engine-private `ReversibleCellMap`/set adapter in
`vortex/reversible_cell_map.hpp` using M3's flat-storage primitive where suitable. Erasure under a
live mark retains the key's explicit logically-absent slot; rehash preserves all keys needed for
undo, and find/iteration expose only live entries. Prepare capacity for live plus restorable keys
before mutation, never shrink/reclaim them while a mark needs restoration, and restore by key
without allocation. Bulk erase/clear prepares all undo/metadata capacity first. Copy, comparison and
snapshots preserve logical map semantics, including absent versus present-unbound values.

Wire the reversible containers in a separate commit **while retaining the existing snapshot
algorithm**, and pass existing regressions. Only then enable local/trail prototypes. Test capacity
failure, erase/clear, rehash and restoration with allocation disabled before any noexcept undo is
used. Bucket reserve on a std::unordered_map alone is insufficient because it does not retain nodes.
M5c tunes/migrates remaining non-rollback tables and retires snapshots; it is not the first
milestone providing reversible storage. Price actual slot/value/occupancy and retained-key capacity;
the target 24B undo record does not include or eliminate that table storage.

Branch routing remains separate boundary-owned metadata: retain `ChoicePoint::deweyPath`,
nextAltIndex, cutBarrier, choiceId, scoped/checkpoint identity, cursor/altOp/stackFrame and cycle
metadata when retiring map snapshots. Preserve `currentDeweyPath_` creation, conditional backtrack
restoration, scoped-abort swap and cut-callback prefix construction
(`vortex_vm.cpp:1393-1433, 1478,1528-1543,1575`), plus OR-parallel spark export
(`or_parallel.cpp:224-258`). Do not treat Dewey routing as an untrailed resume value or discard it
with a map snapshot. The oracle compares path state, spark metadata and callback output as well as
registers.

Create engine-private VMTrailEntry/VMSavepoint helpers inside the VM implementation and a test
oracle that runs the same actions against the existing snapshot behavior and candidate trail.
Introduce staged, noexcept-movable inverse records: scalar/membership, typed shared handles and
individual ActivationStack push/pop inverses. Keep absent, existing-unbound and bound references
distinct. Local savepoints protect cursorPC; ordinary choices retain existing altOp cursor routing.
Retain empty frame stacks/capacity and restorable erased buckets so unwind never allocates.

When both choice depth and savepoint depth are zero, bypass recording. A committed inner local
savepoint keeps records while any outer choice/savepoint exists. Clear only when both depths are
zero. Prepare trail/table/stack storage before mutation; allocation failure leaves a restorable
state and reports the executing failingClause. A local savepoint wraps multi-write operations.

**Gate:** nested commit/rollback, absent versus unbound, frame push/pop, allocation failure at each
preparation point, resume-initialization failure and comparison helper compatibility, and zero
allocations during unwind. Include nested ordinary/scoped choice backtracking, cut callbacks, scoped
abort and root-steal Dewey paths in the differential gate. Keep the snapshot oracle test-only; do
not ship dual state engines as the final P1 implementation.

### 9.2 M5b: Shared payloads and boundary lifecycle

Use the accepted first-write rule from proposal §2.1: the innermost live choice/local savepoint owns
a touched set keyed by container and CellRef for mutable streams and iterators. Retain its old
handle/presence before the first uniqueness check and record touched only after the undo record is
secured. Later writes under that boundary do not add a snapshot owner; normal external-alias COW
remains in force. Each nested boundary has a fresh touched set.

On child commit/cut discard the child set but retain its undo records for any outer boundary; do not
merge sets on a noexcept commit. On rollback unwind to the child mark and remove child tracking.
Test parents already touched and not yet touched after child commit. Whole-map clear, erasure, root
stealing and last-boundary removal follow the same ownership policy.

Unwind the VM before releasing reclaimed arena slots on backtrack/scoped abort/mutation-group abort
(`vortex_vm.cpp:468-509,1440-1477,1548-1579`). Inner cut preserves outer records. Root steal
transfers the existing spark metadata/prefix mechanism, not VM trail pointers. Destroy dead prefix
owners below the minimum of all live choice/savepoint watermarks before return; keep indices stable
until no local savepoints prevent compaction. Compaction rebases surviving choices, costs linear
moves/rebasing, and reuses capacity without implying it returns memory to the OS.

Add optional caller-supplied maxUndoBytes via chaining VM limits API. Charge trail, restorable
buckets, frame capacity, touched sets and boundary headers before growth. Use actual ABI sizes (24B
target record, 16B target touched slot plus load slack), with retained payload versions and existing
payload budgets reported separately. No arbitrary small cap; no eviction of live undo. Budget
refusal rolls back and returns a failed ExecutionResult with failingClause.

**Gate:** extend existing stream/frame/scoped-choice/mutation-group/guard and OR-parallel tests,
with one choice plus N appends/rank advances, nested boundaries, aliases, cuts, budget failure, root
steals and aborts. Compare final state, allocations, copied payload elements and retained bytes to
snapshots. No snapshot-induced payload copy per mutation under one unchanged boundary. Measure
repeated small-prefix compactions; implement a different policy only with evidence.

### 9.3 M5c: Table tuning, remaining maps and snapshot retirement

After reversible storage and trail behavior pass, tune their capacity policy and migrate remaining
non-rollback per-cell tables such as opcodeMap/cursorPC using P2's helper or the justified VM
specialization. CursorPC participates in local savepoints, so it must use the reversible adapter
already in M5a wherever that contract requires it. Keys are arbitrary CellRefs, never raw dense
indices. Preserve reversible absent entries across rehash and key-based restoration; no undo record
may point into movable buckets. Generation-only reset is allowed only for trivially destructible
values, with a full sweep before generation wrap. Owning tables promptly destroy payloads on reset.
Benchmark the extra touched-set and flat-map lookups rather than assuming map flattening is free.

Remove production whole-map snapshot fields only after differential coverage passes; retain all
branch-routing and boundary identity metadata listed in §9.1. Keep a test-only reference model.
Update every mutator and the exact Proposal Register file list before retiring the old path.
Deterministic workloads with no boundary must not allocate trail records; local savepoint calls
still may grow storage. Publish actual bytes and fallback choice.

**Gate:** owning reset/destructor and wrap/rehash stress, sparse/ephemeral keys, full differential
journeys, scoped limits compatibility and existing all-language/actor tests. If P1 cost is not
justified, retain current maps with reserve and scoped guards as the approved fallback rather than
leaving a partially wired trail in production.

## 10. M6: Final release and completion gates

Retrain PGO on the final source revision, rebuild the accepted ThinLTO+PGO release, and record a
fresh manifest. Run all apps/tests through that final build, not just a pre-optimization control.
Run `make -j$(nproc) test`, shaders, format-check and lint, focused analyzer checks on touched C++
and the headless backend comparison. Outside Make use all six environment variables; the backend
script only supplies offscreen video itself (`tools/compare-backends.sh:24`).

Read the three gtest summaries separately from swarm kernel failures. Missing veth is an
infrastructure failure requiring the user's module load, not a gtest regression or a green swarm
test. Record failed/skipped gates honestly and keep unaffected work moving. Check no fixture bytes
or authored operation counts changed due solely to view/execution optimizations.

Compare final held-out workloads with M0 and the same-source control; retrain each PGO variant
independently with the same versioned training corpus. Publish raw repeated runs, structural
correctness, CPU/memory/allocation/text-size/build/training costs and each retained candidate or
fallback. Mark milestones complete only with evidence. No additive predicted speedups.

## 11. Accepted Proposal Register and planned file ownership

All entries are already accepted. New implementation helper/tool filenames below are planned;
confirm consumer inventory in each milestone and keep this table current as work lands.

| ID  | Exact planned files (relative to repo root)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       | C++/host justification and cost                                                                                                         | Approved fallback                                                              |
| --- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------ |
| P1  | `apps/common/xanadu/vortex/vortex_vm.{hpp,cpp}`, `vortex_frames.cpp`, `vortex_streams.cpp`, `vortex_closures.cpp`, `vortex_bindings.cpp`, `vortex_contracts.cpp`, `vortex_resume.cpp`, `vortex_comparison.cpp`; new `apps/common/xanadu/vortex/reversible_cell_map.hpp`; `tests/xuzz/vortex_{frame,stream,scoped_choice,scoped_choice_routes,mutation_groups,guard,closure}_test.cpp`, `tests/xuzz/transport_and_or_parallel_test.cpp`, `tests/xuzz/vortex_resume_state_test.cpp`, `tests/xuzz/vql_comparison_vm_test.cpp`; new `tests/xuzz/vortex_undo_test.cpp` | Host lifetimes and atomicity cannot be delegated to bytecode; 24B target records plus all retained capacities/payloads and touched sets | Existing maps/reserve and scoped local guards                                  |
| P2  | `apps/common/xanadu/zigzag/manifold.{hpp,cpp}`, `apps/common/xanadu/store.cpp`; new `apps/common/xanadu/flat_cell_map.hpp`; `tests/xuzz/manifold_test.cpp`, new `tests/xuzz/flat_cell_map_test.cpp`                                                                                                                                                                                                                                                                                                                                                               | C++ replay lookup; 4B plus presence bit/dense capacity, 48B target birth slot plus growth peaks                                         | Reserved unordered maps                                                        |
| P3  | `apps/common/xanadu/zigzag/arena_manifold.cpp`, `tests/xuzz/arena_manifold_test.cpp`                                                                                                                                                                                                                                                                                                                                                                                                                                                                              | C++ edge traversal, no added slot bytes; qualifying fraction measured                                                                   | Existing occurrence-first path                                                 |
| P4  | `Makefile`, `apps/common/xanadu/segmented_ops_spool.{hpp,cpp}`, `apps/common/xanadu/zigzag/manifold.{hpp,cpp}`, `apps/common/xanadu/zigzag/arena_manifold.{hpp,cpp}`; new `tools/release-profile.sh`, `tools/release_profile.py`, `tools/performance-report.py`, `tools/performance-probe.cpp`, `tests/performance/workloads.json`, `tests/performance/test_performance_report.py`, `tests/performance/test_release_profile.py`; `docs/testing.md`                                                                                                                | Build/compiler/profile orchestration plus inline C++ visibility; text/I-cache, build/link/training/storage costs measured               | Explicit portability/control builds; release optimization diagnostics retained |
| P5  | `apps/common/xanadu/vortex/vortex_core.{hpp,cpp}`, `vortex_vm.{hpp,cpp}`, `vortex_frames.cpp`, `vortex_stdlib.cpp`; new `apps/common/xanadu/vortex/operand_list.hpp`; `apps/common/xanadu/vql/compiler.cpp`, `apps/common/xanadu/vpl/compiler.cpp`, `tests/xuzz/vortex_core_test.cpp`; new `tests/xuzz/operand_list_test.cpp`                                                                                                                                                                                                                                     | C++ operand fetching; 64B provisional return object with unbounded spill                                                                | Existing vector return                                                         |

Brace suffixes in the table expand within the last fully named directory. Do not introduce a generic
library dependency on engine helpers. Extend existing tests once, in the layer that owns the code;
new generic-library behavior belongs in tests/lib, not replicated in engine suites.

## 12. Scope boundaries and refused shortcuts

| Topic                  | Decision/default                                                                                                                                             |
| ---------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Acceptance             | P1-P5 accepted; no repeated permission request for those changes                                                                                             |
| Timing thresholds      | M0 records repeat policy/noise; retain baseline if benefit is inconclusive                                                                                   |
| U-P2 dense threshold   | Use the accepted overflow-checked default initially; revise only with measured density/cost evidence                                                         |
| U-P3 inline capacity   | Eight is a provisional spill threshold; M4 chooses by measurements, never an arity cap                                                                       |
| U-P4 dispatch shortcut | Deferred; preserve general register/opcode classification                                                                                                    |
| Shared trail template  | Keep VM-specific first; extract only if an additional real consumer justifies it                                                                             |
| Persistence            | No on-disk changes; if one becomes necessary, stop treating it as an optimization-only diff and apply the repository's explicit format/version/fixture rules |

Refuse ring-buffer overwrite, raw CellRef-indexed VM arrays, global graph-mutation opcode caches,
LMDB manifold checkpoints, private dimension ordinals, bit-packed CellSlot flags, profile reuse
across incompatible builds, and shipping the training executable. Reasons remain in proposal §4;
this plan does not reopen them. Workload manifests are build artifacts, not JSON packed into cells.

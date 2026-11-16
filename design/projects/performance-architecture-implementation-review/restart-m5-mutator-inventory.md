# M5a/M5b Vortex VM mutation and savepoint inventory

Static inventory only. Inspected accepted P1 (§2.1), implementation plan §9, vortex_vm.hpp, named VM
translation units, or_parallel.cpp, and ArenaManifold savepoint/mark behavior. No repository source
was changed and no build or benchmark was run. This is an implementation checklist; line anchors
refer to the inspected tree and will drift.

## 1. State ownership and snapshot boundary

ChoicePoint (vortex_vm.hpp:520-543) snapshots ten containers plus streamCursor and
mutationCheckpointId:

| VM state             | Type                                                                | Undo character / mutators                                                                                                                            |
| -------------------- | ------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- |
| cellReferences\_     | unordered_map\<CellRef, optional<CellRef>>                          | Scalar membership/value; distinguish absent, present-unbound, and bound. Written by output, typed-register, frame, guard, resume, and binding paths. |
| streams\_            | unordered_map\<CellRef, shared_ptr\<vector<CellRef>>>               | Shared mutable payload handle and contents; snapshot before uniqueness/COW and append/clear.                                                         |
| iterators\_          | unordered_map\<CellRef, shared_ptr<IteratorState>>                  | Shared mutable variant; IteratorNext changes sequence cursor/index or rank current/done/seen.                                                        |
| streamSlots\_        | unordered_set<CellRef>                                              | Typed-register membership.                                                                                                                           |
| iteratorSlots\_      | unordered_set<CellRef>                                              | Typed-register membership.                                                                                                                           |
| streamCursor\_       | optional<CellRef>                                                   | Scalar cursor owner; ownership handoff can clear both payload maps.                                                                                  |
| activationFrames\_   | unordered_map\<CellRef, vector\<shared_ptr<const ActivationFrame>>> | Stack push/pop and owner-key removal; needs per-frame inverse and retained empty vector/capacity while marked.                                       |
| guardStacks\_        | unordered_map\<CellRef, shared_ptr\<const vector<RuntimeGuardRef>>> | Immutable shared stack-handle replacement/erase; retain old exact handle.                                                                            |
| pendingGuardChecks\_ | unordered_map\<CellRef, shared_ptr<const PendingGuardCheck>>        | Immutable handle insert/replace/erase; progression replaces pending object.                                                                          |
| guardScopes\_        | unordered_map\<CellRef, shared_ptr\<const vector<GuardScope>>>      | Immutable handle replacement/erase.                                                                                                                  |
| mutationGroups\_     | unordered_map\<CellRef, shared_ptr\<const vector<MutationGroup>>>   | Immutable handle replacement/erase; checkpoint identity separately tracked.                                                                          |
| mutationCheckpointId | optional in ChoicePoint                                             | Snapshot scalar; trace writes/restores together with mutation group stack.                                                                           |

ChoicePoint also owns boundary/routing state: cursor, alternative opcode, zigzag Mark, stack-frame
link, cut barrier, Dewey branch path/index, cycle baseline, choice identity, scoped bit, and
mutation checkpoint marker. currentDeweyPath\_ is conditionally restored on backtrack/scoped abort
and must remain boundary metadata. opcodeMap\_ is dispatch identity outside choice rollback.
memoizedOps\_, relationProviders\_, limits/counters and callbacks are not choice snapshots.
cursorPC\_ is intentionally absent from ordinary snapshots: ordinary backtrack routes to altOp;
local transactional savepoints must restore cursorPC\_. resumeValues\_, resumeTokenIds\_,
nextResumeTokenId\_ are explicitly untrailed control state; only the output token reference in
cellReferences\_ backtracks.

Choice creation snapshots in pushChoicePointImpl (vortex_vm.cpp:1364-1433) before taking the arena
mark. Scoped snapshot budgeting is enforceScopedChoiceBudget (1323-1362). Snapshot copies can throw
before the mark; arena-mark failure pops the copied ChoicePoint.

## 2. Mutation inventory by subsystem

This includes staged whole-container replacement (copy-mutate-swap), which direct operator[]
searches miss.

### Core/register, lifecycle and guard/mutation routing — vortex_vm.cpp

- mintOpcode / setOpcodeKind (519, 572): opcodeMap\_[opCell] writes; outside choice trail.
- setCursorOpcode (623-654): writes cursorPC\_[cursor]; on close/terminal cleanup updates PC then
  erases guard stack/scope, pending checks, result refs, and every frame backing from refs,
  stream/iterator maps and typed sets; erases activation frame owner. This is a bulk multi-container
  transition.
- pruneCursors (688): audit whether it calls close/cleanup; do not assume it only prunes graph
  cursor links.
- writeCellReferences (713-739): assigns scalar bindings. clearTypedRegister
  (vortex_streams.cpp:33-38) erases typed sets and payload maps. Central funnel candidate, but many
  bypass sites remain.
- removeValidatedGuardObligations (427-451) and trimGuardScopes (453-466): clone mutationGroups\_ /
  guardScopes\_, edit or erase cursor entry, then swap. These are logical mutations despite staging.
- abortOwnedMutationGroups (468-509, noexcept): pops inner choices, removes checkpoint choice,
  releases its mark, swaps all ten snapshot maps/sets and currentDeweyPath\_ into live state, then
  prunes resume state. Trail unwind must precede mark release and allocate nothing.
- executeGuardOpcode (1922+): cursorPC writes; staged guardStacks\_ and cellReferences\_ copy/swap;
  guard token refs; guard stack insertion/replacement/removal. Immutable RuntimeGuardStack handle
  can be retained.
- executeMutationScopeOpcode (2142+): cursorPC continuation; staged guardScopes\_ and
  cellReferences\_ swaps; scope open/close and mutation-group state.
- Mutation-group completion/abort (2353-2442): staged mutationGroups\_ and reference swaps,
  nested-choice pops, checkpoint metadata and callback/routing effects.
- driveGuardValidation (2446+): replaces/erases pendingGuardChecks\_, erases result refs, stages
  pending-map replacement before swap. Suspended validation advances immutable pending object;
  failed validation clears it.
- prepareTypeContract (4814-4850) and executeTypeContract (vortex_contracts.cpp:31+):
  erase/reset/emplace cellReferences\_. Reset of optional means present-unbound, distinct from
  erased.
- Generic outputs, pipeline, step and run orchestration (stepImpl 3914-4810, runPipeline 1626+):
  writeCellReferences plus direct output erases, cursor transitions and staged mutation-group
  replacement. Audit path-level writes and error cleanup. Undo-budget failure must identify the
  executing opcode as failingClause.
- executeRelationNext and comparison mostly read tables, but output commit still mutates references.

### Streams and iterators — vortex_streams.cpp

- clearTypedRegister (33-38): removes both memberships and both payload maps; callers can also
  modify cellReferences\_.
- requireStreamCursor (40-66): ownership changes; may clear streams\_ and iterators\_ wholesale if
  no blocking choice; sets streamCursor\_. Whole-map clear must journal all affected entries first.
- executeCellRefCopy (98+): ClearCellRefs erases scalar refs and may replace typed stream with empty
  vector. Stream copy installs stream membership/handle and removes iterator/scalar state; iterator
  copy installs iterator membership/handle and removes stream/scalar state; scalar fallback clears
  typed state. Source snapshots precede writes for alias safety. Journal target before coupled
  changes.
- StreamClear: inserts stream membership, erases iterator membership/ref/iterator payload, replaces
  stream handle.
- StreamAppend: potentially creates or COW-copies streams\_[target], mutates vector by
  insert/self-prefix append, then sets stream membership and erases iterator/ref/iterator payload.
  Retain old handle before uniqueness/COW and append. Under one boundary later appends should not
  repeat the trail snapshot; external alias COW remains.
- StreamBegin / StreamReverseBegin / RankBegin: create IteratorState payload; set iterator
  membership, erase stream membership/handle/ref. RankBegin seeds seen with anchor.
- IteratorNext: clones if shared then mutates SequenceIteratorState chunkIndex/index or
  RankIteratorState current/done/seen. It then clears item output register state, writes scalar
  item, writes arena presence bool, and clears presence typed/ref/payload state. Snapshot iterator
  before .unique() and any mutation. A later arena write refusal cannot leave the advance committed.
- PinHistorical: extends/replaces stream payload, sets stream membership, erases iterator
  membership/scalar ref/iterator handle; same first-write rule.
- Map erase can change presence independently from typed membership. Do not assume sets alone encode
  exact map state.

### Frames — vortex_frames.cpp

- invokeFramePrepared (338-500): copies refs, streams, iterators, both typed sets and
  activationFrames\_; saves cursorPC\_ and streamCursor\_; requireStreamCursor may itself clear
  payload maps. Opens Arena mark, allocates local backing cells and present-unbound ref slots,
  initializes argument streams/cells, pushes ActivationFrame, updates PC, discards mark and
  increments frame counters. Catch swaps copies back and releases mark. New local VMSavepoint must
  enclose every VM write; VM undo before arena release. Frame push inverse should pop and retain
  vector capacity.
- returnFrame (502-715): copies guardScopes, guardStacks, refs, streams, iterators, typed sets and
  frames; saves cursorPC\_/streamCursor\_; marks arena. Writes result slots, trims scopes/replaces
  or erases guard stack, pops frame, erases backing state, removes empty frame owner, routes PC and
  writes graph cursor content, discards mark. Catch releases arena mark then restores copies. Trail
  result writes, backing cleanup, and one frame-pop inverse; preserve exact shared guard handles.
- Frame copying is costly because full activationFrames map and stack vectors clone. Avoid
  recreate-on-undo allocations.
- Existing tests include partial backing allocation failure/retry, nested/recursive stack behavior,
  backtrack restore, close cleanup and stream ports.

### Closures — vortex_closures.cpp

- executeClosureOpcodeBody (668+): CreateClosure already has ArenaSavepoint; capture and
  output-register writes still need host-state savepoint parity.
- ApplyClosure setup local clone/swap (757-778) copies all cellReferences\_ and saves cursor PC; its
  failure path restores both.
- CheckClosureCall clone/swap (864-883) similarly copies cellReferences\_, captures prior cursor PC
  and restores/erases it on failure. Both are local transaction targets.
- Persistent descriptor/capture metadata is arena state, not VM trail state; result registers, frame
  setup and cursor route are VM state.
- Tests cover closure backtracking, failed creation/retry, aliases/source rebind, repeated apply and
  frames; extend for staged write failure and nested local/choice boundaries.

### Bindings — vortex_bindings.cpp

- executeRestoreBindings (168-271): copies refs/streams/iterators/typed sets; saves streamCursor and
  restore counters; has ArenaSavepoint; calls requireStreamCursor and restoreBindings, writes graph
  cursor and cursorPC, commits. Catch restores maps/counters/PC and rethrows. Explicit local
  savepoint target; current preconditions exclude live choices/frames/guards/resume/groups.
- restoreBindings (628+): stages replacement maps/sets then swaps all five register containers;
  clears streamCursor if no active cursor. Any future live boundary requires undoable bulk replace;
  direct uses/callers must be audited.
- snapshotBindings/bindingsDescriptor mostly build temporary objects until restore. Preserve absent
  vs present-unbound state and comparison-helper parameter compatibility.
- Counters are restored on failed operation today; retain that behavior.

### Resume — vortex_resume.cpp

- executeResumeInit (~30-108): increments nextResumeTokenId\_, creates arena token, writes ExternRef
  and output through writeCellReferences, emplaces resumeValues\_ and resumeTokenIds\_, commits
  arena savepoint, prunes stale state. Catch erases token entries and restores or erases previous
  cellReferences\_ output then rolls back arena. Resume values/IDs remain untrailed; output ref is
  choice-visible. Current failed init consumes ID; preserve monotonicity absent accepted change.
- clearResumeState (~183+) / pruneResumeState (~193+): remove resume maps (untrailed) and sometimes
  token-cell references (trailed normally). Verify pruning after backtrack does not invalidate
  restored reference.
- Existing resume tests cover token reinit/backtrack, cursor isolation and closed/foreign tokens.
  Add stage-by-stage failure consistency.

### Comparison — vortex_comparison.cpp

comparisonOperand (24-45) takes concrete CellReferenceMap and set/map arguments; read-only but a
table representation migration must retain or intentionally adapt the concrete API. Comparison
output writes still use normal reference commit. Add an adapter compile/use case to
vql_comparison_vm_test.cpp.

## 3. Boundary transitions and order

- **Choice push:** pushChoicePointImpl validates cursor/alternative/scope/mutation owner, optionally
  counts snapshot budgets, captures stackFrame/Dewey metadata, copies all ten containers and
  streamCursor, then takes Arena mark. Add vmTrailMark and per-boundary touched metadata. Secure
  trail/container capacity before publishing a mark; current path advances Dewey only after mark
  succeeds.
- **Backtrack:** backtrack (1436-1490) may stop at mutation checkpoint/foreign cursor, then pops a
  ChoicePoint. Current code releases arena mark before moving snapshots into live maps. Trail must
  unwind to cp mark before arena release. Restore maps, prune resume state, conditionally restore
  Dewey, then route cursor to altOp and restore stackFrame link. Do not roll back all cursorPC on
  ordinary choice.
- **Cut:** cut (1493-1546) discards marks without restoring snapshots and invokes callbacks with
  prefix path. Committed child trail remains while outer choice/savepoint exists; never clear outer
  trail on inner cut. Clear only when both depths reach zero. Preserve callback prefix logic.
- **Scoped abort:** abortScopedChoices (1548-1579, noexcept) discards nested marks, pops outer
  scope, releases outer mark, swaps snapshot state/path, restores streamCursor and prunes resume
  state. Unwind before release, no allocation. Existing semantics do not invoke cut callbacks;
  differential oracle must verify.
- **Mutation-group abort:** abortOwnedMutationGroups (468-509, noexcept) can remove checkpoint below
  nested choices, releases mark, restores snapshots and Dewey. Reverse-unwind removed range,
  preserving survivor watermarks.
- **Root steal:** stealNearRootChoicePoint (1604-1623) only when no scopes, streams, iterators,
  active guards; moves oldest choice, erases vector front, calls ArenaManifold::evictOldestMark,
  decrements later cutBarrier. or_parallel.cpp:224-258 exports optional shadow prefix before steal.
  or_parallel.cpp:392-444 creates fresh worker VM with scalar spark metadata/path and optional
  prefix snapshot; never transfers VM trail pointers. Destroy trail payloads below minimum surviving
  choice/savepoint watermark, mark dead records inactive without shifting under pinned local
  savepoints, then compact and rebase all surviving choice/savepoint watermarks at safe boundary.
  Preserve Dewey, alt, cycle, stackFrame, cursor and cut metadata.
- **Arena precedent:** ArenaSavepoint and ArenaManifold::discard/release
  (arena_manifold.cpp:2361-2384, 2484-2498) implement nested LIFO lifetime: discard retains records
  for outer mark, clears when no marks remain. VM mirrors this but also needs local marks/touched
  keys and root-evicted prefixes.

## 4. Allocation, fuel and rollback hazards

1. Reserve trail, touched-set, reversible bucket and stack capacity before publishing undo
   record/touched bit and before the mutation. Failure must not leave a touched key that suppresses
   future snapshot. Multi-write paths need enclosing VMSavepoint.
1. Unwind must allocate nothing and be noexcept. unordered_map::reserve does not retain erased
   nodes. Restorable absence must survive rehash; restore by key, not bucket pointer. Keep frame
   stack capacity and empty entries while marked.
1. Payload first-write belongs to innermost choice/local savepoint. Save old handle/presence before
   .unique()/COW and only then mark touched. Each child begins untapped. Commit drops child touched
   set but retains records for outer rollback; no merging. Whole-map clear/erase journals every key
   first.
1. Partial-write risks: IteratorNext changes iterator before output writes; returnFrame writes
   outputs before cleanup; invokeFramePrepared allocates arena cells before later VM/table writes.
   On consumeWork, makeCell/write refusal, bad_alloc/length_error/table/vector growth failure,
   restore host and arena state and return failure with executing failingClause. Local rollback
   ordering must not inspect reclaimed arena slots.
1. Copy-mutate-swap stages in guard/scope/group/frame/closure/bindings bypass wrapper mutators.
   Journal affected logical keys before swap or route diff through reversible operations.
1. abortScopedChoices and abortOwnedMutationGroups are noexcept. Tagged shared_ptr trail union
   construction/destruction and callback/error semantics must preserve no-throw unwind.
1. Trail-retained shared_ptr changes current use_count tests: StreamAppend/PinHistorical use
   values.use_count()>2 because local lookup adds owner. Compute external alias status before trail
   owner or adjust accounting explicitly; otherwise snapshot ownership can trigger unwanted COW or
   mask external aliases.
1. Pair VM and arena order: unwind VM before arena.release. On success discard arena mark but retain
   VM records if an outer VM boundary exists. Nested local savepoints and choices must remain
   LIFO-aligned.
1. setCursorOpcode changes host cursorPC and arena cursor value; ordinary backtrack routes to altOp,
   local savepoint restores prior host and graph state. Keep representations consistent.
1. Fuel/cycles are not refunded by rollback (existing ScopeDoesNotRefundExecutionWork). maxUndoBytes
   is separate, optional, absence means allocator-limited. Refusal precedes mutation and yields
   failed ExecutionResult with executing opcode as failingClause.
1. Resume token IDs remain untrailed; failed ResumeInit consumes next ID today. Restore output/token
   arena state without accidentally rolling back the monotonic ID.
1. Root eviction is not ordinary pop: arena evicts oldest mark while victim continues. Local
   watermarks can pin indexes; promptly destroy dead owning payloads, but compact/rebase only when
   safe. Vector erase does not release capacity.
1. MutationCheckpointId and mutation-group stacks are coupled barriers. Search all writes before
   designing the trail to ensure no cut/backtrack crosses an active group.

## 5. Regression coverage and current tests

- vortex_frame_test.cpp: invocation/result routing, nested/recursive frames, choice inside callee,
  stream ports, PartialLocalAllocationFailureRollsBackAndCanRetry, close cleanup. Add every
  trail/table/stack prep failure; frame push/pop inverses and zero-allocation unwind.
- vortex_stream_test.cpp: append/self-append, COW aliases, work preflight, copy source snapshot,
  choice restoring stream/iterator, rank termination, typed-register reuse and cursor ownership. Add
  N appends/rank advances under one boundary (one boundary snapshot), nested touched/untouched child
  commit/rollback, clear/erase restore, failure after advance before outputs.
- vortex_scoped_choice_test.cpp and vortex_scoped_choice_routes_test.cpp: nested ordinary choices,
  frame restore, stream rollback, scoped callback isolation, stealing disabled in scope,
  budget/foreign cursor rejection. Differentially cover backtrack, cut, abort, Dewey, callbacks,
  stackFrame and route.
- vortex_mutation_groups_test.cpp: groups, guard scopes, ownership and cycle limits. Add abort
  through inner choices and verify no reclaimed-handle access.
- vortex_guard_test.cpp: choice restores stack/pending position, dispatch retry and ReturnFrame
  guard trim. Add replacement/erase and per-validation-step rollback.
- vortex_closure_test.cpp: backtracking closure results, failed creation/retry, alias freeze and
  frames. Add ApplyClosure/CheckClosureCall failure at each post-write point and nested
  local+choice.
- vortex_resume_state_test.cpp: output reinit backtrack, cursor isolation, foreign/closed tokens.
  Add each ResumeInit failure stage and assert exact output ref plus deliberately untrailed control
  maps.
- vql_comparison_vm_test.cpp: comparison behavior/budget; adapter signature compile/use case.
- transport_and_or_parallel_test.cpp: spark encoding/OR parallel. Add root steal with outer records,
  payload destruction, metadata and Oracle/ShadowDelta result equivalence.
- New tests/xuzz/vortex_undo_test.cpp is planned for snapshot oracle and failpoints. Oracle must
  compare all ten maps, cursor PC for local boundaries, streamCursor, arena state, cycle/fuel,
  resume invariants, Dewey/spark metadata and callback emissions.
- Additional required cases: absent vs present-unbound vs bound; erase/rehash/restore; bulk clear;
  copy/move/logical equality; no-allocation undo; trail/touched/table/stack allocation failure;
  nested savepoint commit/rollback; mutable payload coalescing; scoped and ordinary paths; mutation
  group abort; root-steal rebase; resume partial init; comparison API compatibility.

## 6. M3/M4 prerequisites and ordering

M3 is not integrated: artifacts include /tmp/perf-m3-flat-map.patch and
/tmp/perf-m3-implementation-inventory.md; review /tmp/perf-m3-flat-map-review.md flags mutable
iterator/proxy compilation, missing dense/presence and applyStructure preflight integration,
inadequate failure atomicity tests, and no cost evidence. Before sharing its primitive with P1,
prove explicitly absent slots retained through undo, key-based restoration across rehash, live-only
iteration, copy/move/equality, and allocation-free restore. Do not couple VM semantics to an
unproven helper.

M4 drafts under /tmp/perf-m4-operand-list\* are isolated only; follow plan dependency order. M5a:
(1) ReversibleCellMap/set adapter plus retained erase/re-hash/copy/move and injected failures while
snapshot rollback stays production; (2) only then add trail/savepoint prototype and test-only
snapshot oracle; (3) wire whole mutator inventory and local guards; (4) differential
routes/failures. M5b adds first-write payload ownership, root eviction lifecycle/reclamation and
maxUndoBytes. M5c tunes remaining tables and only then retires production map snapshots.

P1 planned files: vortex_vm.hpp/.cpp, vortex_frames.cpp, vortex_streams.cpp, vortex_closures.cpp,
vortex_bindings.cpp, vortex_contracts.cpp, vortex_resume.cpp, vortex_comparison.cpp, new
vortex/reversible_cell_map.hpp; tests listed above.

## 7. High-risk review holes

1. Direct cellReferences\_ writes in generic output, pipeline, contract and validation paths can
   bypass wrappers.
1. Stage-and-swap paths in guards/scopes/groups, frames, closure checks and binding restore bypass
   map mutators.
1. Iterator interior mutation after .unique and stream mutation after use_count require pre-mutation
   handle snapshot.
1. Trail shared_ptr ownership alters use_count>2 alias logic.
1. Restoring erase by allocation violates noexcept; unordered_map bucket reserve alone is
   insufficient.
1. activationFrames\_ erases empty key; recreation on undo may allocate.
1. Cursor cleanup spans all tables, frames, guard state and cursorPC; missing a table leaves stale
   typed state.
1. driveGuardValidation can mutate during ReturnFrame validation before its local map snapshot
   starts; determine current partial-validation semantics before moving transaction boundary.
1. RestoreBindings failure restores counters, streamCursor and cursorPC today; retain exact
   behavior.
1. Root steal survivor watermark rebasing and prompt dead-payload release need dedicated tests; not
   equivalent to ordinary choice pop.
1. Check all mutationCheckpointId writes paired with mutationGroups\_ and cut/backtrack barriers.
1. Scalar outputs, typed memberships and payload maps must restore as a coherent register despite
   repeated outputs.
1. Failure injection must target each preparation allocation deterministically and be thread/test
   local. Verify actual VMTrailEntry ABI size/noexcept union lifetimes and charged capacity.
1. Handling choice rollback alone does not satisfy M5a: four named local full-map snapshot paths
   must become VMSavepoints too.
1. Measurements must report table/touched lookup overhead, retained capacity/payload versions and
   prefix compaction separately; snapshot removal alone is not measured benefit.

## 9. Assembly pruning correction from independent review

`VortexVM::pruneAssemblyState()` (`apps/common/xanadu/vortex/vortex_vm.cpp:544-560`) directly erases
dead arena keys from `opcodeMap_`, `cursorPC_`, `cellReferences_`, `streams_`, `iterators_`,
`streamSlots_`, and `iteratorSlots_`; it clears `streamCursor_` when its owner is no longer in the
arena and calls `pruneResumeState()`. `pruneResumeState()` (`vortex_resume.cpp:193-206`) removes
untrailed `resumeTokenIds_` and `resumeValues_` control entries based on arena token validity.
`clearResumeState()` separately drops resume-control entries and then prunes.

Call order matters:

- `AssemblyTransaction::~AssemblyTransaction()` (`assembly_transaction.hpp:15-21`) on failure first
  releases its arena mark, then prunes VM assembly state. VQL and VPL compiler `compile()` create
  this transaction; compilation itself does not execute the just-built program before committing,
  though executable compilation may create a cursor before its final return.
- `SolutionDriver::Impl` (`logic_compile_core.cpp:813-830`) captures `baseDepth = vm.choiceDepth()`
  after taking its arena mark. `SolutionDriver::~SolutionDriver()` (`836-849`) cuts only to that
  base depth, closes its cursor, releases its mark, then prunes. Therefore pre-existing choices may
  remain active; an unconditional `choiceDepth()==0` precondition would change existing
  nested-driver semantics.
- `CompileArea::~CompileArea()` (`logic_compile_core.cpp:925-936`) releases the query graph mark,
  then prunes. In `compileAndRunQuery`, it outlives the `SolutionDriver`, so it also runs after
  driver cleanup while pre-existing VM choices can remain.

The corrected boundary argument under strict arena LIFO is:

1. A choice snapshot that survives release of an inner arena mark predates that mark. Any key it had
   in its snapshot refers to a cell that predates the released mark and therefore cannot be
   reclaimed by that release. Later choices that could mention the released cells must be discarded
   before mark release. Thus no surviving ChoicePoint snapshot can legitimately restore a key whose
   arena cell was created inside the released region.
1. That does **not** mean no trail record can name such a key. `SolutionDriver::next()` executes the
   query while pre-existing choices may remain, so outer trail entries can record post-boundary
   writes for query/frame/cursor keys. They all have absent baseline state at the surviving
   boundary, but records remain in the LIFO stream.
1. Therefore pruning after arena release must be a trail-aware logical erase, not physical removal:
   keep the key slot retained-absent and trail old handles/values for every reversible table/set and
   `streamCursor_` mutation while any choice/local savepoint is live. On rollback, prune records
   unwind first and can temporarily restore handles to now-dead cell ids; subsequent earlier per-key
   records unwind to the absent baseline. Undo must use only map lookup/move/reset and must not
   dereference the arena referent during this transient replay. Reclaim retained-absent slots only
   after the full rollback range and last boundary finish.
1. `opcodeMap_` is explicitly outside the accepted trail; it may be physically pruned after release.
   `cursorPC_` is local-savepoint state only, not ordinary choice state; if a local VMSavepoint can
   enclose this cleanup, journal its old presence/value through that local boundary.
   `resumeTokenIds_`/`resumeValues_` remain deliberately untrailed control state, but any
   `cellReferences_` erasure performed by resume pruning must use the regular register trail.

Required regression: create an outer VM choice, create/use stream/iterator/reference state keyed by
cells allocated after the choice, release an inner arena mark and call `pruneAssemblyState()`, then
backtrack the outer choice. Verify the temporary dead keys are not observable/restored at the final
baseline; all shared payloads are released at the right watermark; `streamCursor_` is restored to
the pre-choice state; and reverse replay never dereferences the arena. Also exercise resume-token
pruning and an in-progress local savepoint if that call path is supported. This proof replaces the
earlier assumption that all choices must be closed before pruning; the later correction is in
`/tmp/perf-m5-inventory-review.md`.

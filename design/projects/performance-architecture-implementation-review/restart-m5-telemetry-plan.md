# M0 telemetry plan for M5

Scope: measurement proposal only. No production files or builds changed. This follows accepted plan
§4 (snapshot copies, mutable-payload copied elements, active boundaries, trail/table capacity,
root-steal/rebase/move work) and §§9.1–9.2 (first-write ownership, charged capacity, retained
payload versions, no snapshot-induced copy per repeated mutation under one boundary).

## Current coverage and gap

`tools/performance-probe.cpp` has a `vm-snapshots` workload with 128 train / 144 validation choice
points. It verifies one seeded `cellReferences` entry per choice and reports `choice_points` plus
`selected_snapshot_map_entries`. The current count is only eight selected maps; it excludes both
typed slot sets, Dewey path entries, frame vector elements and nested payload elements. The scenario
does not create streams, iterators, frames, or local savepoints. It cannot establish mutable payload
copies, all-table capacity, or root-steal cost. Existing `null`/absent counters must continue to
mean unmeasured or not applicable, never measured zero.

The relevant current deep copies are explicit: `pushChoicePointImpl` copies ten tables; frame
invoke/return and local closure/binding paths copy/swap table state; `StreamAppend` and
`PinHistorical` deep-copy `vector<CellRef>` when the live handle is shared; `IteratorNext` copies
`IteratorState` when shared, including `RankIteratorState::seen` keys or
`SequenceIteratorState::chunks` handles; frame snapshot copies duplicate each `ActivationStack`
vector's shared-pointer elements. `ChoicePoint` map copies of streams/iterators copy shared handles,
not the pointed-to payloads.

## Instrumentation boundary and API

Keep the timing and counter executables separate as they are now. **Do not conditionally add fields,
bases, accessors, or layout-dependent inline definitions to `VortexVM` or another class shared by
the engine objects.** The current counts probe reuses engine objects; compiling only the probe with
extra class members would create an ODR/ABI mismatch.

Instead, add an external fixed-size `VmCostSink`/counter API in a small telemetry header and a
count-only provider object. The sink is owned by the count executable, not by the VM. A scoped
activation sets a `thread_local` pointer to the sink and an optional non-owning `VortexVM*` filter;
scope exit restores the prior pointer. It stores only fixed integer counters, takes no VM ownership,
allocates nothing, and adds no `shared_ptr` owner or COW pressure. Use one thread-local sink per
worker; report whether the scenario is single-threaded or which worker threads are covered. No
cross-thread atomics are needed for the current single-thread probe.

Compile guarded event call sites in count-only **replacement engine objects** for the affected
`.cpp` files (`vortex_vm.cpp`, `vortex_streams.cpp`, `vortex_frames.cpp`, and any actually
instrumented local-snapshot files). Link those objects in place of the corresponding ordinary engine
objects for `performance-probe-counts`; never link both definitions. Other engine objects may be
reused only when their class definitions and inline behavior are identical. Reuse the existing
allocation-provider count flags, and strip PGO instrumentation/use flags from the telemetry
replacement objects as for the allocation-provider object. Keep ordinary timing/release objects and
binaries unchanged. The Make link/object graph, source hashes, and diagnostic binary identity must
make the replacement boundary explicit. If Make cannot replace only the instrumented TUs without
another ODR hazard, use a fully separate count-engine object graph and account for that build cost;
do not fall back to conditional VM class fields.

The ordinary timing executable then has no telemetry fields, branch, atomic, allocation, or
iteration overhead. The count-only engine call sites check the active sink/owner and increment plain
integers only; no maps, strings, vectors, callbacks, or allocations in the event path. Reset the
sink immediately before the measured workload event window and serialize after; fixture construction
and correctness checks are excluded unless a field explicitly says otherwise. Count-run wall time is
diagnostic only. Existing allocation-run C++ `new` calls/bytes remain a separate cumulative
requested-allocation measure and are not table capacity or retained bytes.

For snapshot copies, instrument explicit copy sites rather than estimating from total VM size:

- `VortexVM::pushChoicePointImpl`: after the ten table copies occur, count one choice snapshot and
  source `size()` for each of `cellReferences`, `streams`, `iterators`, `streamSlots`,
  `iteratorSlots`, `activationFrames`, `guardStacks`, `pendingGuardChecks`, `guardScopes`, and
  `mutationGroups`. Also count the sum of activation-frame stack elements copied. Track regular and
  scoped choice snapshots separately.
- Existing frame invoke/return, closure Apply/Check, and binding restore copy/swap blocks: count
  local snapshot events and the same per-table entry counts at the point copies are made. These are
  distinct from the new M5 `VMSavepoint` counts; do not relabel existing snapshots as trail
  savepoints.
- Retain separate counts for shallow `shared_ptr` handles copied from
  stream/iterator/guard/pending/group tables, scalar optional values copied, and activation-frame
  vector elements. This keeps map-entry copies distinct from nested payload copies.

Instrument actual deep-copy sites, counting elements only when a copy is constructed:

- In `StreamAppend` and `PinHistorical`, immediately before `make_shared<vector<CellRef>>(*values)`
  when `sharedWithOtherState` is true: increment COW events and add `values->size()` to copied
  stream `CellRef` elements. Do not count `insert()` growth or new elements appended as copied-old
  elements.
- In `IteratorNext`, immediately before cloning a shared `IteratorState`: record one clone and
  discriminate the variant. For rank iterators add `seen.size()` to copied rank-seen keys; for
  sequence iterators add `chunks.size()` to copied chunk handles. Do not count the immutable
  `CellRef` vectors behind those shared chunk handles as copied.
- At snapshot-copy sites, count activation-frame vector elements (`sum(stack.size())`)
  independently; copying the `shared_ptr<const ActivationFrame>` handles does not copy frame
  objects.

In the M5b implementation, count first-write retention separately from COW using **handle-retain
events**: old stream/iterator handles stored in undo records, touched-set insert attempts/successes,
and touched hits. These event totals count references/records, not unique physical payload versions;
multiple boundaries or aliases may retain the same `shared_ptr`. A touched hit is not a new payload
snapshot. Count table/touched/trail capacities after successful growth and at the operation
boundary; count active and maximum choice, scoped-choice, and local-savepoint depths. A baseline
with no `VMSavepoint` implementation reports local-savepoint metrics as `null`/`unsupported`, not
zero.

For exact unique payload versions and their current capacity bytes, inspect live tables plus
retained trail records after the measured window, collect raw pointee identities
(`shared_ptr::get()`), and deduplicate without copying/retaining any `shared_ptr`. This post-window
observer may allocate, but runs outside both timing and allocation-counter windows and adds no
strong owner, so it cannot trigger or mask COW. Name these fields
`unique_payload_versions_at_window_end` and `unique_payload_capacity_bytes_at_window_end`; include
payload kind and use the contained vector's actual `capacity() * sizeof(element)`. Do not infer this
state from scalar retain-event counts. Exact high-water unique payload versions/bytes require
event/lifetime tracking that observes destruction without retaining objects; unless that tracking is
independently implemented and validated, report the high-water unique fields as `null`/unsupported.
Do not hold `weak_ptr`s to make identities stable: they can prolong control-block/allocation
retention and contaminate physical-memory observations.

At `stealNearRootChoicePoint` and M5b prefix collection/compaction, record root steals, ChoicePoints
shifted by front erase, records retired/destroyed, record bytes released, records and boundary
watermarks rebased, entries/slots physically moved, and bytes moved using the helper's actual
element size. Keep per-event deltas and workload total/high-water values; do not infer this work
from wall time.

## Capacity and byte accounting

Report capacities in native units as well as bytes, with the accounting scope explicit:

- For the reversible flat map/set, expose count-build stats
  `{live, capacitySlots, occupiedSlots, retainedAbsentSlots}`. Report exact owned slot-buffer
  capacity bytes as `capacitySlots * sizeof(Slot)` and retained-absent bytes as
  `retainedAbsentSlots * sizeof(Slot)`. Report any separate vector buffers by
  `capacity * sizeof(element)`.
- For trail records, boundary headers, and touched sets, report current and peak `size`, `capacity`,
  and exact charged bytes from their actual `sizeof` values and capacity slots. Include retained
  undo records and active touched capacity. Emit `sizeof(VMTrailEntry)`, `sizeof(VMSavepoint)`, and
  touched-slot size in the report so changes in ABI are visible.
- For mutable payload buffers, report current/retained versions, element count and vector capacity
  bytes where the concrete capacity is available. Keep retained payload bytes/counts separate from
  the trail/table charge, as §9.2 requires. Report the existing stream/snapshot budget separately.
- For baseline `std::unordered_map/set`, report `size` and `bucket_count`; these are capacities, not
  exact allocated bytes. The existing global-new counter can report cumulative requested bytes for
  the isolated count run. Leave exact retained table bytes null unless measured through a separately
  identified allocator/PMR resource; do not estimate node or allocator overhead from `bucket_count`.
- For process memory, keep the existing whole-process peak RSS label. It includes startup and
  fixture construction and is not table/trail memory.

This distinguishes (a) live logical entries, (b) retained-absent/restorable storage, (c) reserved
capacity, (d) requested bytes over an interval, (e) handle-retain events, (f) unique
pointees/current capacity observed after the window, and (g) process RSS. Do not combine them into a
guessed `total_bytes` field. The M5 `maxUndoBytes` charge must match its documented capacity
formula, while separately publishing unique retained payload versions/elements and the pre-existing
payload budget. Never label event references as unique versions or as a physical-byte high-water.

## Workload and report shape

Keep the existing `vm-snapshots` case as the small snapshot-copy baseline, but enrich its count JSON
with the full ten-table entry counts and frame-element counts. Add one dedicated count/timing pair
for mutable payload behavior using real public VM operations: seed a stream and a rank iterator with
a known length, take a choice boundary, then perform a fixed number of appends/advances; validate
final contents and iterator position. Use the same operation trace for the timing and count
binaries. Include a nested-boundary cohort so a parent's already-touched payload and an untouched
child payload are both represented. The timing pass reports elapsed work/checksum only; the count
pass emits copied elements and table/trail/touched telemetry. Do not synthesize direct map writes
that bypass the VM mutators.

Add a root-steal case with a fixed choice depth and no active stream/iterator/guard (the current
steal preconditions), verify the returned root metadata and surviving branch state, then report
moved choices. The M5 candidate adds trail-prefix/rebase metrics to the same case. A root-steal case
that is disallowed by live mutable payloads is not counted as a successful steal.

Use deterministic train/validation sizes and commands with distinct identifiers. Record constructed
structure and actual operation counts/checksum; a different seed label alone is not evidence that
the graph or operation trace changed. Pair timing and counter commands in
`tests/performance/workloads.json`, and retain source/compiler/build identity through the existing
report runner. Update the probe schema and validator so the new fields are required for these cases,
nullable with explicit reason elsewhere, and zero is accepted only when the scenario semantically
measured zero.

## Gate before M5b

1. Add focused count-only counter tests for exact known values: one choice copy, a known number of
   stream cells, rank-seen keys, sequence chunk handles, activation-frame elements, nested
   boundaries/touched hits, and a root steal with a known number of shifted choices.
1. Verify the release/timing executable has no telemetry symbols or counter field overhead, just as
   the existing probe separates global allocation replacement. Verify the counts executable emits
   valid JSON and matches semantic work/checksum with the timing executable.
1. Preserve raw counter JSON and capacity units. Report exact bytes only for instrumented owned
   buffers; do not present generic allocator calls or RSS as table/trail bytes.
1. During M5b compare the same trace on the snapshot control and trail candidate: no repeated
   snapshot-induced payload copy under one unchanged boundary; one first-write handle retention per
   key/boundary; child-boundary touch isolation; root-steal destruction/rebase counts; and identical
   final VM/arena/oracle state.

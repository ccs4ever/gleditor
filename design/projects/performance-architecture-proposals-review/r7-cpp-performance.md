# Round 7 critique: C++ Performance Expert (performance-architecture-proposals.md)

Paths below are relative to the repository root. This independent pass reviews v6 against live
source. It does not implement the proposals or report new benchmark measurements.

Tally: 1 BLOCKING, 2 MAJOR, 3 MINOR.

## BLOCKING

### 1. (§2.3, line 224) Quote construction does not prove an immutable value kind

Full title: Value-kind-first proxy dispatch can change a quote occurrence's local-only links

**Problem:** P3 removes the occurrence lookup on the premise that occurrences are minted with
`ValueKind::None` and therefore never carry `ExternRef`. Construction does not establish that
invariant. `setValueBits` can subsequently assign `ExternRef` to an occurrence. Today, `linked`
handles that occurrence locally before considering proxies, and `isProxy` explicitly excludes it.
The proposed value-only test would enter proxy handling instead. `foreignOf` resolves an occurrence
through its canonical proxy, so this can return a foreign edge instead of the occurrence's local
edge or absence. This is a concrete behavior change, not an unmeasured speed hypothesis.

**Evidence:** `design/performance-architecture-proposals.md:220-233` uses the construction-time
value to eliminate the occurrence guard. In `apps/common/xanadu/zigzag/arena_manifold.cpp`,
`quoteOccurrence` mints `None` at line 570; `setValueBits` writes the caller's kind without an
occurrence restriction at lines 2135-2145; `shadow` returns an existing dense slot unchanged at
lines 2320-2322. `isProxy` rejects occurrences at lines 544-550, while `foreignOf` canonicalizes
them at lines 601-609. `linked` currently performs the local-only occurrence scan at lines
1632-1641. A counterexample is an occurrence with a canonical proxy having a foreign edge, no
corresponding local occurrence edge, and an accepted `setValueBits(occurrence, ExternRef, bits)`
call.

**Fix:** Use the value-kind test to bypass occurrence lookup only for non-`ExternRef` slots. Inside
the `ExternRef` branch, retain the occurrence exclusion before proxy delegation; occurrences still
use the local scan. Alternatively, formally prohibit this value mutation throughout all APIs and
price that separately, but that is a larger behavior change. Add a regression case for an
`ExternRef`-valued occurrence and remeasure the actual skipped-lookup fraction rather than assuming
more than 99 percent of calls qualify.

## MAJOR

### 1. (§2.1, line 125) Shared-handle trailing needs boundary-aware COW

Full title: Per-mutation shared-handle undo can turn linear stream iteration into quadratic copying

**Problem:** P1 retains old `shared_ptr` handles and says this preserves snapshot/COW semantics, but
does not specify when to retain them relative to mutation or how to avoid retaining a fresh version
on every write. The old handle must be saved before the uniqueness check; saving it after an
in-place payload mutation cannot undo that mutation. Saving a handle before every write makes the
payload shared before every write, however, forcing COW on every append or iterator advance. A
single long-lived choice followed by N single-element appends can then retain and copy vectors of
lengths 0 through N-1: quadratic payload work and retention, despite only O(N) trail records. Rank
iteration can similarly copy the growing `seen` set every step. Existing choice snapshots share the
initial payload once; after its first COW, subsequent updates can remain unique until a new rollback
boundary appears. P1 needs an explicit rule preserving that property.

**Evidence:** `design/performance-architecture-proposals.md:125-128` specifies retaining exact
handles without a first-write or boundary rule. `apps/common/xanadu/vortex/vortex_streams.cpp` lines
243-250 copy a stream when its map/local handle has additional owners (`use_count() > 2`), then
append and publish it at lines 252-266. Lines 349-353 clone `IteratorState` whenever its handle is
not unique; the rank branch grows `rank.seen` at lines 379-390. Existing snapshots copy map handles
in `apps/common/xanadu/vortex/vortex_vm.cpp:1412-1421`. These facts establish the regression for
naive per-write trailing; no claim is made that an implementation must choose it.

**Fix:** Specify first-write snapshotting per payload and rollback boundary, or a priced payload
mutation undo scheme. New nested choices/savepoints must establish their own restoration state;
commit, cut, rollback and root eviction must maintain or invalidate the tracking correctly. Retain
the old handle before COW only when that boundary needs a new snapshot. Price the tracking data and
retained payload versions. Add an acceptance workload with one outstanding choice and many
appends/rank advances, then nested boundaries, recording allocations, copied elements and peak
retained payload bytes; compare with the current snapshot implementation.

### 2. (§2.4, line 248) Instrumented self time is presented as a release speedup

Full title: P4's promised 1.4-second saving is unsupported by the supplied gprof evidence

**Problem:** The document attributes all sampled self time in `get` to cross-TU call overhead and
then promises approximately that entire time back through header inlining. Self time includes the
actual bounds checks, pointer arithmetic, instruction and data effects, and effects of the
instrumented build. `-pg` adds function-entry profiling work and can change optimization decisions;
it is not the same execution path as the ordinary release binary. Header visibility permits inlining
but does not guarantee every dynamic call disappears. Consequently neither the supplied call count
nor its flat self time establishes a 1.4-second release benefit. The zero-memory-cost claim also
omits possible executable-text growth and instruction-cache pressure. Section 6's later warning does
not reconcile these unconditional P4 and execution-order claims.

**Evidence:** `design/performance-architecture-proposals.md:17` identifies the `-O3 -g -pg` build;
lines 247-264 attribute the time and promise the saving; lines 385-386 assert that all calls are
removed. `Makefile:498` appends `DEBUG_OPTS` to compilation and line 555 also appends it to linking,
so this setting instruments both stages. `apps/common/xanadu/segmented_ops_spool.cpp:397-408`
contains real validation and address work, not an empty forwarding call. The default release options
are `-O3 -g` at `Makefile:483`.

**Fix:** Keep gprof as a hotspot lead and label the saving unmeasured. Require an ordinary release
baseline after `5d604feb`, then compare out-of-line, header-inline and LTO variants on the same
headless workload. Inspect optimization remarks or disassembly to establish which calls inline;
measure wall time, cycles/instructions when available, and executable-text size. Report repeated
runs and build/compiler details. Price possible code-size/I-cache costs and link/build time; make
P4's recommendation conditional on those measurements rather than subtracting flat self time.

## MINOR

### 1. (§2.5, line 273) Wing call count is not an allocation count

Full title: Empty wings allocate no operand vector and the inline capacity lacks an arity histogram

**Problem:** The current-structure text says every `inputsOf`/`outputsOf` call heap-allocates. Empty
wings return an empty vector without traversing/materializing a rank. A 7.4-million call count
therefore cannot determine allocations avoided. The inline capacity of eight may be useful, but the
document supplies no workload distribution to justify its 64-byte return object and copy cost
against smaller buffers or a visitor. This is a measurement gap rather than a correctness failure.

**Evidence:** `apps/common/xanadu/vortex/vortex_core.cpp:394-397` returns `{}` when no first operand
exists. `design/performance-architecture-proposals.md:273-274,313-314` makes the allocation claim
and prices the inline-plus-spill object.

**Fix:** Say nonempty materialized wings allocate; collect arities, spill frequency and allocation
counts. Compare inline capacities and visitor/span boundaries on representative execution before
fixing eight as a performance choice. Keep it a spill threshold rather than a language ceiling.

### 2. (§2.1, line 163) Prefix compaction needs an amortization measurement

Full title: Frequent root stealing can introduce repeated full surviving-trail moves

**Problem:** Compacting an inactive vector prefix and rebasing choice watermarks is linear in the
surviving trail and choice counts. If a small dead prefix is reclaimed before repeated growth, long
surviving trails may be moved repeatedly. This is a plausible cost to measure, not a proven
regression: no steal/growth distribution is supplied. Vector compaction also reuses its capacity; it
does not itself reduce the capacity charged to `maxUndoBytes`.

**Evidence:** `design/performance-architecture-proposals.md:101-105,152-166` mandates prompt payload
release, compaction before growth/refusal, watermark rebasing, and retained-capacity accounting.

**Fix:** State compaction's O(live records + choices) work and distinguish reusable slots from
released capacity. Measure a repeated-root-steal workload. Consider an amortized threshold or
chunked trail only if measurements show this is material; never delay dead owning-payload release or
invalidate live rollback marks.

### 3. (§2.2, line 182) Dense IDs do not establish dense useful mappings

Full title: P2 should measure mapping density and initialization work on text and branching stores

**Problem:** Sequential spool indices establish the possible key range, not how much of it maps to
cells in the selected fold. A text-heavy store or a branch selecting a small ancestral path can
allocate a dense table for many unused IDs. The proposal prices index bytes but does not compare
this distribution, zero-initialization/page-fault work, or the alternative's actual bytes per useful
mapping. This does not invalidate the representation; it limits the performance conclusion.

**Evidence:** `apps/common/xanadu/store.cpp:529-537` folds an ancestral walk, not every spool
operation. `apps/common/xanadu/zigzag/manifold.cpp:325-405` maps non-Structure operations only when
their context resolves to a cell. P2 selects and initially sizes dense storage by `opCount` at
`design/performance-architecture-proposals.md:182-208`.

**Fix:** Include text-heavy, branch-heavy and structure-heavy folds in P2's acceptance measurements.
Report total op capacity, mapped entries, committed/RSS bytes and initialization time. Use those
results to justify dense versus sparse selection; reserve-only virtual address space and committed
physical pages must be reported separately.

VERDICT: NOT CONVERGED

# Round 8 Disposition

Target: `design/vortex-stdlib-prebuilt-store.md`, revision 7 -> revision 8. All six critics verified
the correct absolute worktree and 21-module baseline. Raw tally: 0 BLOCKING, 7 MAJOR, 23 MINOR; the
seven majors form six clusters.

Writer returned CONVERGED with no majors. Architect and ergonomics returned the same verdict despite
reporting one and three majors respectively. Their finding lists take precedence: the workflow
requires zero majors, so those reports are not counted as converged. No implementation is made or
implied.

## R8-1. Status Is Arena-Local; Operation Refusals Are Host-Level

Sources: codebase M1, ergonomics M3, vortex minor 3. Accepted by narrowing the contract rather than
introducing host callbacks into every stdlib instance.

`VortexStdLib` has no host back-pointer; private worker contexts belong to the host. Revision 8
names `arenaLiveMarks`, an instantaneous count from its own arena. `lastRefusal` records only that
instance's image-initialization refusal. `ChoiceStackLive` instead returns via the host/visualizer
operation and visible feedback, carrying the host aggregate. It does not overwrite healthy loader
status or poison a retained image. Tests cover worker-only marks with primary count zero.

Codebase minor 2 is also accepted: a concrete result table distinguishes expected visualizer
pointers, host pointers, and the external lambda's expected-void result.

## R8-2. Name the Startup Reader and UI Deduplication Lifetime

Source: ergonomics M1. Accepted.

After successful host construction, `ensureVortexHost` reads primary stdlib status. The visualizer
owns `lastSurfacedImageRefusal_`, comparing structured diagnostic fields rather than recreated host
identity. A changed diagnostic writes feedback and opens the command bar; identical per-command
rebuilds do not reopen it. Successful initialization clears the latch for a later failure episode.
Tests use a fresh visualizer for fresh-startup assertions; the headless engine's reset hook does not
reach into UI state.

Writer minor 2 is accepted too: command refusals explicitly open the command bar, even outside
bar-driven invocation.

## R8-3. Do Not Promise an Unimplemented Cancel Operation

Source: ergonomics M2. Accepted by removing "cancel" rather than adding a cursor-lifecycle API to
this migration.

Recovery names resumption to completion through the owning runner using the existing embedding API.
Runners must retain their cursor token. There is no v1 abandon, cursor enumeration, or bounded-time
recovery promise for unbounded flows; token loss is an embedding error, not a secretly implemented
cancel action.

Writer minor 5, architect minor 3, and vortex minor 1 are accepted: direct attach checks the
outgoing host; the incoming host's base/store must remain valid and coherent with installation.
Incoming marks survive and block later replacement until their owner drains them.

## R8-4. Engine Reset APIs and Files Are Part of P30

Source: vortex M1. Accepted.

The VPL/VQL engines and VQL compiler hold private arena-keyed state; the host cannot clear it
directly. P30 now includes their three header/implementation pairs and public chainable
`onArenaSwapped()` methods, called after core/VM/stdlib resync and before host services are
reinstalled. The exact caches cleared are named, including compiler entry/current/result state and
VQL active perspective. VPL resets its optional dimension cache; existing `defaultDim()` lazily
resolves the new arena's dimension.

Vortex minor 2 and zigzag minor 3 are accepted: remove obsolete `guardInstalled_` and its
single-slot comment under owner-keyed lifetime registration. Vortex minor 4 is accepted: the Store
load flag defaults writable for existing callers; image loads explicitly opt into read-only.

## R8-5. Hydration Rejects Marks Before Mutation; Name Actual Lifecycle Operations

Source: architect M1; writer minor 1; zigzag minors 1/2. Accepted mechanism, corrected evidence.

The current method clears `floors_` before seeding (`arena_manifold.cpp:2186`), so the architect's
proposed `[real, synthetic]` vector is not the literal current behavior. The underlying defect is
real: calling hydration under outstanding marks invalidates their external snapshots and overwrites
the raw count.

P30 changes `hydratePrefix` to `ArenaResult`, refusing `ArenaRefusal::MarksOutstanding` at
raw-count-nonzero entry, before any mutation. The single production OR-parallel caller consumes that
result. The synthetic floor is then guaranteed oldest. Real-mark pop-back does not remove it; trying
to release/discard when only the seed remains is invalid/stale-Mark use and asserts before mutation.

There is no arena clear/reset API. The document now names hydration reseeding, whole-state
assignment, and destruction. Moved-from arenas are destruction/assignment only until reinitialized;
scalar flag/count access on moved-from state is not promised. Tests cover refusal atomicity,
eviction, real-mark nesting, and destination move state.

## R8-6. Scope Image Initialization and Gate the Known Compilation Bypass

Source: zigzag M1; ergonomics minor 1. Accepted.

The no-live-marks invariant explicitly governs ensure/graft/rebind and their image registration, not
arbitrary runtime cell allocation. It is checked before presence probing even for a second instance
over already-present cells.

P30 also gates `ZigzagVisualizer::compileVQL` before compiler minting/registration on a retained
host, even when no ensure runs, using its existing CompilationResult and visible feedback. Direct
clients of VM `mintOpcode` still have the pre-existing untrailed-registration exposure under marks;
a general runtime registration/trailing redesign is not claimed by this image migration.

## Remaining Minor Dispositions

- Architect minor 1: remove `makeWorkerContext`'s explicit bootstrap; constructor ensure already
  covers it. No unresolved ensure-or-drop alternative remains.
- Architect minor 2: internal tail ensures check/propagate unexpected results; null-host
  constructors/lazy/compile callers explicitly check too. Recorded image status is handled through
  the named reader, not an expected-constructor fiction.
- Architect minor 4 and ergonomics minor 2: reload prechecks before its own
  `store_->setCurrentVersions` mutation at `zigzag_visualizer.cpp:415`.
- Architect minor 5: direct adoption sinks and constructor fallback are separately named alongside
  API wrappers.
- Ergonomics minor 3: say "reorder" the pane leave after successful bind.
- Ergonomics minor 4: the claimed tenth bind call at `xuzz_app.cpp:3979` is not reproduced; that
  line parses a foreign cell in a callback. Target search finds nine direct calls. Avoid a brittle
  count in normative wording: all calls consume the result, whatever later edits add.
- Ergonomics minor 5: wrapper checks precede projection, avoiding wasted projection on a refused
  adoption.
- Writer minor 3: base pointer at host line 154 was already correct; arena assignment is separately
  identified. Destructor range drift is non-blocking.
- Writer minor 4: key-record operation order now matches the fragment precedent.

## Next Gate

Revision 8 requires focused follow-up of changed status/notification, hydration API, cache-reset
accounting, and compilation-gate contracts. Keep the correct absolute worktree baseline and
zero-BLOCKING/zero-MAJOR stopping rule.

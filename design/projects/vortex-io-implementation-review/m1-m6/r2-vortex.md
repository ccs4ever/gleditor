# Round 2 critique: Vortex Expert (Vortex I/O M1–M6 code)

Tally: 0 BLOCKING, 0 MAJOR, 3 MINOR. Target `dbe8e40c` (diff `757cd55b..dbe8e40c`).

## Status of round-1 findings

- **B1 RESOLVED.** `ArenaManifold::shadow()` now takes `nextAllocationIncarnation()` and pushes it
  after `slots_.push_back`, popping the slot on throw (`arena_manifold.cpp:2455-2468`). Both slot
  growth sites are covered: `mintSlot` (`:2097`) and `shadow` (`:2462`) are the only
  `slots_.push_back`. `mark()` records `cellCount = slots_.size()` and `release()` truncates
  `slots_` and `incarnations_` to it (`:2771-2772`), so shadows and mints truncate together.
  Incarnations stay ascending (a shadow takes the next one), so the `truncated` lambda in
  `release()` (`:2776`) holds for leases, runtime-only flags and tags. Snapshot restore rebuilds
  `incarnations_` from `slots_` (`:3630`). `compact()` touches links and content only. Tests
  `shadowedBaseCellsKeepIncarnationsParallelToSlots` and `runtimeOnlyGuardHoldsOverAShadowedBase`
  assert the failure shape from round 1 (mint after shadow, no base/runtime aliasing, promote and
  export refused); `IoCellsTest.requestGraphMintsOverAnArenaWithAShadowedBase` covers the I/O path.
- **M1 RESOLVED** (out of my lane's re-review; the shared `FramedLog` directory sync is described in
  disposition A and I saw nothing contradicting it in the capture-journal paths I read).
- **M2 RESOLVED.** `IoHost::awaitForVm` (`io_host.cpp:66-89`) is the handler the VM calls. It
  catches `std::exception` from `awaitCell`, tries a typed `Rejected(BackendFailure)` outcome, and
  if that mint also throws (the exhausted-arena case) returns the request as the "outcome" so
  `stepAwait` fails by value (`vortex_vm.cpp:4009-4011`). `awaitCell` retires the issued
  continuation when `service_.await` throws (`io_host.cpp:118-127`), and every later throw path
  there already retires waiter and continuation. The test sets the arena at its limit, asserts
  `step` does not throw, returns `Failed`, and that `parkAwait` for the issued continuation is
  refused as "does not belong", which proves retirement. It exercises the fallback branch (both
  mints fail); the typed `Rejected` branch is not isolated, but both return by value.

## Review of the round-1 fix diff

No defect found in the following, each checked against a failure scenario.

- **`CaptureAppendAdapter` deferred `complete()`.** `running_` is inserted under the same lock hold
  that takes the spec; `endTurn()` runs once on every path that reaches the end of `execute` (there
  is no early return between insert and `endTurn`), before `terminalCandidate`. A `complete()`
  during the turn erases `staged_` and defers the receipt/executed erase to `endTurn`, so no entry
  is re-created after the erase. Owner reads of `receipts_` for a live capture are unaffected (a
  deferral exists only after `complete()`).
- **Clone-view waiter topology.** `mintWaiter` attaches the request through `attachClonedField` and
  `retireWaiter` detaches the view and relinks neighbours (`detachFromRank`), so the request keeps a
  single inbound `d.io.request`-style neighbour per owner and the capture's append view survives
  waiter retirement. The capture's view and each waiter's view sit in the request's clone rank;
  mid-rank removal relinks prev to next.
- **Reaping vs live waiters and resumes.** `eraseRequestState` returns while `waiters` is non-empty
  (`io_service.cpp:359`) and only after it proves a linked outcome (`:371-375`). `reapIoRequest`
  unlinks the Queue and the Endpoint/Capability clone views but leaves the request-to-outcome link,
  which is all `await`/`poll` use for a reaped request (`io_service.cpp:462-469`). A reaped request
  therefore cannot be parked on (`await` returns `Ready(outcome)`), and a parked resume holds the
  waiter, which blocks the reap. The capture commit path reads the request through its own clone
  view, which reap does not touch.
- **Capture publication.** The key is `(journalId, captureId)`; the receipt chain reads id then
  journal id; the seal link is last, so a partial publish leaves unreachable cells only.
  `receiptRank`/`readPublication` refuse a damaged chain by number. `compact()` keeps
  `maxCaptureId_` in the Identity record (`capture_journal.cpp:365`), so compaction cannot let a
  restarted journal reuse the id of a dropped, published capture (which would otherwise hit the new
  `kDetailMismatch` refusal on every later capture).

## MINOR

1. **Wrong-thread await gets a refusal minted off the owner.** `awaitForVm` catches the
   `std::logic_error`-style failure of `requireOwner()` (via `awaitCell`) and then calls
   `service_.refuse(...)`, which mints arena cells on the thread that violated owner discipline
   (`io_host.cpp:70-82`). Call `requireOwner()` before the `try` so a worker-thread call still
   throws and mints nothing.
1. **Durable payload ranges now expire with their leases while the header says they do not.**
   `retain()` on a durable origin returns `Unresolved` once no lease or owner token holds the chunk
   (`payload_resolver.cpp:113-118`), whereas `sourceReachable` answers `true` for every durable
   origin (`:441`) and the `liveSources` comment at `payload_resolver.hpp:136` still says "every
   durable origin". A caller that keeps a `PayloadRange` from `resolveDurable` after releasing its
   lease and later hands it to `CaptureCoordinator::prepare` (`:931`) is refused where round 1
   resolved it. Either document that a durable range is only valid while leased, or make
   `sourceReachable`/the header say so; the capture map's slots for such origins are then kept for
   nothing.
1. **A throw after a successful park leaves VM and service state behind.** In `awaitCell`, if
   `parked_.emplace` throws (`bad_alloc`) after `vm_.parkAwait` succeeded and the waiter is
   registered (`io_host.cpp:150-153`), `awaitForVm` returns a refusal while the VM keeps a parked
   entry and the service keeps the waiter. Retire both in a catch around the emplace, as the
   `nextId_ == 0` branch does.

VERDICT: CONVERGED

# Round 2 critique: Software Engineer (M7-VQL-c code)

Target: `cb194c92` (round 1 fixes `dcce529b..cb194c92`). Tally: 0 BLOCKING, 0 MAJOR, 3 MINOR.

Method: read of the fix diff and the surrounding code. The suites `VqlIo*`, `FileWriteHostTest.*`,
`CrashPointTest.*` and the `VQueryE2ETest` reconcile, stale and exposure cases (239 tests) were run
headless against `build/xuzz_test` and pass. No sanitizer build exists here, so lifetime claims are
established by reading.

## Status of round 1 BLOCKING and MAJOR

- M1 (`~CaptureHost` calls into a freed `IoService`): RESOLVED. `~FileReadHost` calls
  `capture_->abandonAll()` after `requestShutdown` and while `service_` is alive (members die after
  the body), and `~CaptureHost` frees any leftover run with `serviceAlive=false`, which never touches
  the service. `abandonAll` snapshots the handles before releasing, so it does not mutate `runs_`
  while iterating. `ADroppedParkedTokenStillPinsAtRunnerDestruction` has a real fail-before.
  `DestroyingTheHostWithALiveCaptureIsSafe` proves nothing without a sanitizer (the author says so).
  That is acceptable only because the destructor no longer has a path to the service; see m3.
- M2 (`:reconcile ID promote N` promotes the journal span): RESOLVED. `parseReconcile` takes whole
  tokens, `from_chars` with full consumption, and accepts exactly `release`, `promote`, or
  `promote START LEN`; a half pair or trailing token returns nullopt and prints usage.
  `ReconcileRefusesAHalfTypedOrOverlongDecision` covers it.

## Other round 1 fixes checked

- `~ScriptRunner`: the `alive` vector is built from `live_` before `parked_.clear()`, and every
  later loop runs over it. Correct. The activations are released at function exit, after the arena
  work. Fixes m3.
- Trailing-slash root: `has_filename()` strip is right, the test builds the grant before the
  directory exists (a real fail-before).
- Capture-directory fsync: the `created` list is deepest-first and `syncParentDirectory(made)` syncs
  each new entry's parent; a failure only warns, which fits a diagnostic-only loss.
- `sweepStepGraphs`: freeing a never-admitted step is safe. Both push sites set `pending.spec` or
  `pending.durable` before `service_.submit`, nothing sweeps between push and that link, and
  `specInUse` protects the window inside `submit`. The `request` assignment follows on the owner
  thread.
- `owned_` collision: the old entry's budget and lease are now returned before `emplace`. Untested
  (needs a VM rollback that reuses a slot), but the code is straightforward.

## Deferred items

- `openat2(RESOLVE_BENEATH)` / grant-race test: acceptable as deferred, not MAJOR. The grant is an
  authorisation by the author who also passes `--io-write-root`; the escape needs a second local
  process with write access under the root to swap a directory in a microsecond window, the final
  component is `O_NOFOLLOW`, and the residual is written down (contract lines 876 and 972). It must
  be revisited before any mode where the root is shared with an untrusted writer.
- Refused close at destruction: acceptable. At destruction the process or runner is ending, the
  descriptor cost is one fd per refused close, and the stated reason (no admitted close to hand to
  `reapWhenEnded`) is true. Record it as a known leak in the library API.

## BLOCKING

None.

## MAJOR

None.

## MINOR

### m1. Stale-binding reconciliation runs only at runner entry points, not when a name is read

**Problem.** `reconcileBindings()` runs at `run`, `resume` and after an admission's reaping. The
host's `pump` inside a running script can free a retired request's cells (the service reaps a
retired request at its terminal) without a reconcile after it. A variable from an earlier script
that still names that request keeps its old `CellRef` until the next entry point.

**Failure scenario.** Script 1 ends with `$h` naming an in-flight request. Script 2 retires it, then
a later pump frees its graph; a following admission or a query segment in script 2 reuses the slot,
and `io_tag($h)` answers for the new cell. The window is narrow (free and reuse inside one script
before the next reconcile), which is why this is MINOR.

**Fix.** Check in the provider wrapper that each tracked argument still `matches` its remembered
incarnation (the wrapper already inspects `query.arguments`), instead of relying on a prior pass.

### m2. `exposures_` is session-long, linear to search, and never pruned

**Problem.** The vector gains one entry per distinct text-exposed range for the life of the runner,
deduplicated by `std::ranges::find`, and `assess()` calls `spansFor` for each entry per persistence
check. Growth and cost are O(distinct exposed ranges). The poison consequence (an exposure whose read
was retired before capture can never be covered) is a documented owner rule (Q2) and not a finding.

**Failure scenario.** A long REPL or a script that reads a large file in small pieces and prints
each piece accumulates entries and makes each `expose` and `:save` linear in them. No bound or price
is stated.

**Fix.** State the bound in the contract §4.7, or coalesce adjacent ranges per source into one
interval set.

### m3. The stale marker is minted before the constructor can throw and is freed while variables may still name it

**Problem.** `staleMarker_` is initialised in the member list, so the `logic_error` for a second
Submit hook leaks the cell. Separately, at `~ScriptRunner` the marker is freed while session
variables rebound to it remain in the `VQLEngine`, which outlives a runner that a REPL swaps; a later
reader of such a variable gets whatever reuses the slot, the failure the marker exists to prevent.

**Fix.** Move the hook check before minting the marker, and at destruction rebind any variable still
naming the marker to nothing (or keep the marker for the session's life).

## VERDICT: CONVERGED

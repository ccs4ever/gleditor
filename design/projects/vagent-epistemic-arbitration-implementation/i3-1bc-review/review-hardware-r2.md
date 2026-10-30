# Hardware and fault-tolerance review, round 2: I3-1b / I3-1c / I3-1d

Scope: head `d48e85f8` (`git diff 00db472b..d48e85f8`, with `e91e8fe8` the round-1 fix and `0124b0d2`
adding `max_uncertainty` and I3-1d resume). Read-only. Probes in `/tmp/i3-1bc-hardware/probe2.cpp`
and `probe3.cpp`, compiled against the objects in `build/` (built at `e91e8fe8`; the later diff
touches only the uncertainty check and `restoreState`, which I read and exercised through
`build/vagent_actor_test`, headless: `VagentEpistemic*` 92 tests pass at `d48e85f8`).

## Probe results

| probe | result |
| --- | --- |
| 256 unbound principals x 4 MiB eventId | all `InvalidObservation`, RSS +12 MB (was +1 GiB). Fixed |
| 256 bound x 4 MiB eventId; 100 x 1 MiB tuple text | all `InvalidObservation`, RSS +0 |
| 20000 observes naming a bogus pid | RSS +0, nothing logged. Fixed |
| bound neighbour flood (300) and unbound flood (300) vs a committed event of another source | outcome still present. Per-source log fixed |
| 16 sources x 20 events, queue 64 | exactly 64 accepted; a 17th source refused `IngressBackpressure` |
| 100 sources, one event each, queue 64 | 64 accepted (see MINOR 2) |
| 16 sources fill 64 slots, `pump(1)` | 4 slots free afterwards, 4 committed, 60 pending: slots held to turn end, 4 per wake. Fixed |
| bind 5, retire, bind 1 / 5 / 6 | `StaleIncarnation` / `StaleIncarnation` / ok, synchronously |
| `bindSource` with lifetime `nanoseconds::max()` / `min()` | ok, observe queued / `UnknownSource` (saturating add works) |
| wall predicate, flag never set | `ClockUnsynchronised`; after `setWallClockSynchronised(true)` accepted |
| `acquiredAt` 1e300 / uncertainty 1e308 (at e91e8fe8) | `ObservationTimeMismatch` / queued (MAJOR 3 before the key) |
| retire(5), then bind(7) before any pump (N1) | host accepts; arena runs bind(7) then retire; **every inc-7 event ends `StaleIncarnation`**, `sourceChangeOutcome` says `Committed` |
| same with a pump between (N1b) | works |
| actor stops with 5 queued events | `ingestOutcome` is nullopt for all five (see MINOR 3) |

## Round-1 findings

- MAJOR 1 (unbounded unauthenticated memory): fixed. Name/id/signed-field caps are checked before
  any lock (agent_executor.cpp `observe`, 3347-3360), the log is written only for a bound source,
  and an unknown pid touches nothing.
- MAJOR 2 (fairness): fixed. Log is per (pid, principal), sticky for `Committed`/`Duplicate`;
  queue share is `queue / bound sources` clamped to [1, per-source], applied to origin and relay.
- MAJOR 3 (`acquiredUncertainty`): fixed by `max_uncertainty` (below). Nothing else depends on it.
- MAJOR 4 (retire/rebind): the false claim is gone and `bindSource` refuses synchronously from the
  host-kept floor. The RAM-counter device needs a host-issued higher incarnation, now said in the
  design. But the retire-then-rebind path has a new failure, MAJOR 1 below.
- MAJOR 5 (queue release and unbounded settle): fixed. `release` runs in `finishIngest`/`abandon`;
  `settle` caps at `ingestPerWake` and reinserts the rest at the front; probe above.

## BLOCKING

None.

## MAJOR

1. **Source changes posted in the same drain run in reverse order (round-1 MINOR 4, now with a
   concrete consequence).** `bindSource` and `retireSource` both `putBack` to the front of the post
   box (agent_executor.cpp:3273, 3297; `PostBox::putBack` 584), so the later call is earlier in the
   box. `drainPosted` keeps box order and inserts each change after the previous one
   (3661-3664), so the order of execution is the reverse of the calls. Trace (probe N1): bind 5,
   pump; `retireSource`, then `bindSource(7)`, no pump. Host state: tombstone floor 5, live binding
   at 7. Arena runs advance(7), then retire(highest 5), which tombstones the live profile. Every
   inc-7 observation is accepted at ingress and committed to nothing: outcome `StaleIncarnation`
   (Terminal), while `sourceChangeOutcome` reports `Committed` for the last change, so the host
   has no signal. The recovery (bind 8) is not discoverable. This is the documented operator
   workflow. Remedy: give each source change a monotonic number under the ingress mutex and insert
   by it (or `push` instead of `putBack`, which is why `putBack` exists only for ordering ahead of
   work); have `finishSourceChange` of a retire that lands after a live bind revoke the host
   binding as the `StaleIncarnation` branch does. Add a test with both calls before one pump.

## MINOR

1. **`queuedVia` and `queuedBy` entries are never erased.** `release` decrements to zero and
   leaves the key; `observe` also creates `queuedVia[{pid, relay}]` by `operator[]` while testing
   `full`, even when the item is refused (3515-3520). A bound source whose events carry many
   distinct `relayedBy` tokens (the host sets the field) grows the map per token until actor stop.
   Erase at zero and test with `find`.
2. **Defaults oversubscribe the queue.** `maxSources` 128 over `ingressQueue` 64: `share` clamps to
   1, so 64 sources hold every slot and 64 starve while those turns are pending (probe: 100 sources
   -> 64 accepted). Transient, but the comment "every bound source keeps a slot" is false at the
   defaults. Validate `ingressQueue >= maxSources` or reword. `share()` also scans all bindings of
   all pids under the mutex on each observe; use `lower_bound` on the pid.
3. **Stop paths disagree about outcomes.** `orderlyStop` calls `hostEvents.clear()` then
   `forgetIngress`, so queued events of a stopping actor vanish from the log (`ingestOutcome`
   nullopt), contrary to section 14 ("logged `Undelivered`"); `fatal` leaves `hostEvents`, and
   `dropHostEvents` later logs `Undelivered` into the already forgotten map, which then lives
   forever. Pick one: log `Undelivered` before forgetting, and ignore posts for a forgotten pid.
4. **`bindSource` accepts any pid.** 20000 binds on bogus pids succeeded (+26 MB), and a bind
   after stop re-creates bindings that nothing will clear. Check that the pid is known and live
   (the pump-thread contract already allows reading `states_`).
5. **`max_uncertainty` details.** Default and cap are `max_age`, so an honest reading can be
   accepted up to `2 x max_age` old (`acquiredAt >= receipt - max_age - unc`); say so in the
   design, or subtract the uncertainty once. `UncertainAcquisition` is a content refusal charged to
   the source; a device whose clock lost sync is then charged for what is the host's time
   service's concern when the claim comes from a sync-loss. Acceptable, but record the choice.
   The policy hash version bump to `vagent-policy-2` correctly invalidates old checkpoints, and
   resume refuses a changed hash (4558-4572) by `std::runtime_error`, caught at 1267 as a failed
   prepare; make that a typed refusal so the host can tell it from a corrupt checkpoint.
6. **Orphaned doc comment** in agent_executor.hpp (~line 296): the "An event type the program does
   not declare..." sentence now precedes `observe`'s comment.

## I3-1d resume, as judged

- Liveness stamps: `last_receive` and `last_contact` are cleared on the restored profile
  (agent_executor.cpp 4518-4521); records keep theirs, tagged with the old `clock_instance`, which
  a new executor never equals, so no cross-clock comparison can happen. Sound. The field removal
  uses the optional-valued `setField`, not a sentinel. `LivenessStartsAgainFromTheFirstContact`
  passes.
- Default 24576: a checkpoint encodes about two operations per epistemic cell, so 24576 cells is
  about 49k of the 65536 `value_operations`, leaving room for the agent. The measured claim
  (24k checkpoint, 32k do not) is tested by `AFullEpistemicBudgetStillCheckpoints`. Fresh arena
  slots after resume also reset the `arenaSlots` allowance, which is its purpose. The risk that
  remains is that other state (large lists) shares the 65536; the margin is about 25 percent.
- The arena-floor revocation path is reachable after resume and tested
  (`TheArenaFloorRefusesABootTheNewHostAccepted`); `finishSourceChange` revokes under the ingress
  mutex, in the same order as everything else.
- Shared copy map: history clones meet their profile's rank; cloning is iterative and bounded by
  the epistemic ceiling. No new sentinel found in the diff; the executor's absence cases are
  `optional` throughout. Ingestion, bookkeeping and advance remain emitted IR run by the VM; the
  new C++ (`restoreState`) is state migration that must see two arenas at once, which is why it is
  not Vortex, and `epistemicCellsWithin` still wants its justification comment.

## Lock order and clocks (re-checked)

Ingress mutex is always taken before the post-box mutex (`observe`, `bindSource`, `retireSource`,
`finishIngest` via `postRefusals`, `abandon`); `take()` releases the post-box lock first. `refuse`
is called outside the enqueue critical section, so there is no self-deadlock. The record is built
and hashed outside the lock; the binding and wall flag are snapshotted, and the drain re-checks the
binding, so a revoke between the two critical sections is caught. `bindSource` reads `states_`
and is documented pump-thread only. Lifetime addition saturates in both directions. The wall flag
starts unknown.

Verdict: FAIL

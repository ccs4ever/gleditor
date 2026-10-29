# Hardware and fault-tolerance review: I3-1b / I3-1c

Scope: `git diff 00db472b..4ad3f53a`, chiefly `agent_executor.cpp` (`Ingress`, `observe`, `bindSource`,
`unbindSource`, `retireSource`, `drainPosted`, `deliverHost`, `forgetIngress`), `emitObserve` in
`compiler.cpp`, `epistemic_limits.hpp`, against i3-design sections 3-6, 9, 11. Read-only; the repo
was not changed. Probes live in `/tmp/i3-1bc-hardware/probe.cpp`, linked against the built objects
(headless env as briefed).

What is right: lock order is consistent (ingress mutex, then post-box mutex, in `observe`,
`bindSource`, `retireSource`, `deliverHost` and `drainPosted`; `pendingFor` takes only the post-box
lock, `take()` releases it before the ingress lock), so I found no deadlock and no inversion.
`states_` is never touched by `observe`. Binding lifetimes use `clock_.steady()`. `charges`
coalesces per (pid, principal) and is bound-principal only (`UnknownSource` is not chargeable), so
the coalescing map is bounded by bindings. Duplicate and conflicting-ID handling with the 16-entry
window is sound. Wrong-incarnation observations are refused at ingress and again at drain.

## Probe results

| probe                                                                       | result                                                           |
| --------------------------------------------------------------------------- | ---------------------------------------------------------------- |
| 256 refusals from 256 unbound principals, 4 MiB `eventId` each              | max RSS grew by 1,056,936 KB (about 1 GiB), no bound tripped     |
| wall predicate, `acquiredAt` = now - 100000 s, uncertainty 0.01             | `StaleObservation` at ingress (correct)                          |
| same, uncertainty 1e9                                                       | accepted, outcome `Committed`                                    |
| `acquiredAt` = now + 1 year, uncertainty 1e12                               | accepted, outcome `Committed`                                    |
| one `Committed` event, then 300 unbound-principal refusals                  | the committed event's outcome is gone (`ingestOutcome` nullopt)  |
| bind inc 5, `bindSource` inc 1                                              | refused synchronously `StaleIncarnation`                         |
| bind 5, `retireSource`, `bindSource` inc 1 (returns ok), then observe inc 1 | every observation `UnknownSource`; binding silently revoked      |
| same with inc 5 after retire                                                | `bindSource` ok, observe `UnknownSource` (tombstone floor is 5)  |

## BLOCKING

None.

## MAJOR

1. **Unbounded memory from unauthenticated refusals: the outcome log stores caller-controlled
   strings.** `observe` (agent_executor.cpp, `refuse` lambda, about line 3157 of the diff) logs
   `{o.principal, o.incarnation, o.eventId, ...}` into `Ingress::outcomes` before any check of the
   source. `UnknownSource` is the first check, so any signed origin, bound or not, reaches it. The
   count is bounded (256 entries per pid) but the entry size is not: `principal` and `eventId` have
   no length cap anywhere (the 64-hex test exists only in `bindSource`/`retireSource`). The probe
   shows about 1 GiB retained by 256 refusals. Also `outcomes[pid]` is created for any `pid` the
   caller names, including pids that are not actors, and `forget()` only runs on actor stop, so
   distinct bogus pids leak a deque each. The same unbounded strings are copied into `PostBox`
   items, the signed-fields encoding and the SHA-256 input while the ingress mutex is held, so one
   large event also stalls the pump thread's logging in `deliverHost`. Remedy: cap `eventId`,
   `principal`, `predicate`, `frame`, `units` and encoded tuple size before the lock (a new
   `EpistemicRefusal::InvalidObservation`, with limits in `EpistemicLimits`); store a digest or a
   truncated eventId in the log; refuse an unknown pid before touching `outcomes`.

2. **Per-source fairness does not hold for the outcome log: any source evicts any other's
   outcomes.** `log()` is one FIFO of 256 per actor. An unbound principal, which costs the actor
   nothing to refuse, evicts a victim's `Pending`/`Committed`/`Refused` entries (probe: 300 refusals
   erase a committed event). The host loses the answer to "was it committed, may I resend",
   including the `Retryable`/`NeedsHostAction` classes that exist so the host can act. Backpressure
   has the same weakness at the per-queue level: `ingressQueue` 64 over `ingressQueuePerSource` 8
   means 8 principals (bound sources, or one relay replaying 8 principals' genuine signed events,
   since `queuedBy` is keyed by origin signer, not transport) fill the queue and starve every other
   source with `IngressBackpressure`, which is deliberately not charged to anyone. Remedy: keep
   the log per (pid, principal) with its own bound (or keep unbound refusals out of the log and
   count them), reserve queue slots per bound source (`ingressQueue >= maxSources * perSource`
   is too large, so use a fair-share rule such as the per-source cap shrinking as the queue fills),
   and additionally count queued items per relay.

3. **`acquiredUncertainty` is unbounded, so the age and future windows are advisory.**
   `observe` accepts any finite non-negative uncertainty and widens both checks by it
   (`acquiredAt > receipt + unc + skew`, `acquiredAt < receipt - maxAge - unc`). Probe: a reading
   dated 100000 s ago, and one dated a year ahead, both commit with a large claimed uncertainty. A
   device whose clock lost sync (the realistic hardware case) can honestly do this; so can a hostile
   source. The supersession rule in `emitObserve` (`tol = old_unc + new_unc + skew`) has the same
   hole: a huge uncertainty on the new reading is never "early", so an old reading replaces a newer
   one. Remedy: add a required policy bound (for example `max_uncertainty`, hashed into the policy)
   and refuse above it with a retryable `ClockUnsynchronised`-class code; or clamp the widening to
   `max_age`.

4. **Design section 11 (hardware 5) is false: retire does not recover a RAM-counter device.** The
   text says a repeating counter "refuses `StaleIncarnation` until the operator retires it". The
   tombstone keeps the highest incarnation and rebinding needs a strictly higher one, so after
   retire the device (which still presents 1, or 5) is refused forever (probe rows 7-8). Worse,
   `bindSource` after retire **returns ok** and the arena revokes the binding later in
   `deliverHost`, logging the refusal under an empty event ID (so `ingestOutcome` for any real
   event ID never shows it). The host sees a successful bind and then `UnknownSource` for every
   observation, which reads as a transport fault. Remedy: either state in the design that such a
   device needs a host-issued incarnation (and say retire is not a recovery), and make `bindSource`
   report the revocation (a queryable binding status or a synchronous floor kept in `Ingress`
   from the advance/retire results), or let retire carry an explicit operator "reset floor" that is
   a typed, logged action.

5. **Queue bounds are released at drain, not at ingestion, so the real work in flight exceeds 64
   and one actor monopolises the pump for about a second.** `drainPosted` decrements `queued` and
   `queuedBy` when it moves an item to `state.hostEvents`, and it runs after every `pumpOne`
   (agent_executor.cpp:2869). `hostEvents` is unbounded and only turned into turns at the later
   `wake`. A producer can therefore refill the 64 slots between each drain, adding up to 64 events
   per pumped turn to `hostEvents`, each holding a full `WireRecord`. When they do run, `settle`
   exchanges the whole batch and runs every event in one wake (about 12.1 ms each): 64 events is
   about 0.77 s on the pump thread, and a pump with several flooded actors is seconds, during which
   timers, leases and other actors' turns do not run. Remedy: count an item against the queue
   until its turn completes (decrement in `deliverHost`), and cap ingestions per `settle` round
   (a time budget, since cost is data dependent) so other actors interleave; the cost number the
   lead already has makes 64 x 12 ms unsafe as a default for a shared pump.

## MINOR

1. **Dropped items leak bookkeeping and strand outcomes.** In `drainPosted`, a post for an actor
   that is `!live || inTurn` (3345) or unknown is dropped after `observe` incremented `queued`,
   `queuedBy` and logged `Pending`: the counters stay raised (a principal reaches 8 and is
   permanently `IngressBackpressure` if the actor never stops, e.g. `inTurn`), and the outcome
   stays `Pending` forever. The same applies to `state->hostEvents.clear()` at 2726 and 2882
   (wake refused). A dropped `refusalsOf` item leaves its `charges` entry, so `charge()` never
   returns fresh again and that principal's refusals are never charged. Remedy: one `settleDropped`
   helper that decrements, logs `Refused` with a retryable class and erases the charge.

2. **`setWallClockSynchronised` defaults to true and the flag is global.** A host that never calls
   it accepts every wall observation from boot, before any time service has run. Make the initial
   state unknown (refuse retryably until the host reports), and keep it per executor as now.

3. **A host wall-clock step is charged to every source.** After sync, an NTP step backwards of more
   than `clock_skew` makes honest readings `ObservationTimeMismatch`, forwards `StaleObservation`;
   both are `isContentRefusal`, so each device's `content_refusals` and contact time are updated
   though the host is at fault. Remedy: have the time service signal steps through the same flag
   (`ClockUnsynchronised` is already exempt), or exempt time codes while the flag was recently
   cleared.

4. **Ordering of source changes is fragile.** `bindSource`/`retireSource` use `putBack`, which
   inserts at the front of the post box, and `drainPosted` inserts a source change at the front of
   `hostEvents`. Two changes for one principal in one drain cancel out; across two drains
   (actor busy, `turns >= maxTurns`, `retireSource` is documented callable from any thread) the
   later change runs first: retire, bind(7) drained apart yields advance(7) then retire, which
   tombstones the live binding at the stale `highest`. Remedy: insert after the last pending source
   change, or carry a monotonic change number and run in that order.

5. **A forward sequence outlier locks the stream.** One signed event with `sequence = INT64_MAX`
   sets the stream floor; later events refuse `StaleSequence` (Terminal) until a higher incarnation
   is bound. That is per source and per design, but `outcomeClassOf` calls it Terminal for the
   outlier's successors as well, which tells the host "never resend". Say so in the design.

6. **`clock_.steady() + grant.lifetime`** overflows signed `duration` for very large lifetimes
   (undefined behaviour); saturate it. Steady `received_steady`/`last_contact` stamps persist in the
   arena and are not comparable on another executor; section 1d owns the rebasing, noted here only
   because ingestion stamps them now.

7. **Sentinels and Vortex rule.** I found no new sentinel in the diff: `old()` in `restoreState` was
   converted to `optional`, `std::get<0>` keys and `highest = 0` in `retireSource` are local
   accumulators, not absence markers, but `highest` of 0 for a never-bound principal is
   indistinguishable from "incarnation 0"; use `std::optional<std::uint64_t>` as `bindSource`
   already does for `bound`. The ingestion, bookkeeping and advance routines are emitted Vortex IR
   (`emitObserve`, `emitObserveRefusal`, `emitSourceChange`); the new C++ is ingress, transport and
   clocks, which the brief permits, though `epistemicCellsWithin` walks the arena in C++ on every
   ingestion (bounded to `ceiling + 1` cells) and wants a justification comment.

Verdict: FAIL

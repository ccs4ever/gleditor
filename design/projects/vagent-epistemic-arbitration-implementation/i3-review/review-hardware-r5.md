# I3-1 revision 5 review: hardware, sensors, fault tolerance

Read the whole of i3-design.md (revision 5) against spec 2.1, 2.2, 3.2, 5.1, 5.2, 7, 8.1. I checked
the physical allowance in `arena_manifold.hpp`.

## Round-4 findings: verification

- **M1, wall-clock coupling: resolved.**
  - The required `clock` key selects `wall` or `actor_mapped`.
  - Acquisition is compared with the matching receipt stamp (steady for `actor_mapped`).
  - `ClockUnsynchronised` applies only to wall-basis predicates, so an offline or RTC-less node
    ingests `actor_mapped` predicates.
  - The host owns step detection.
  - Steady times carry a clock-instance ID, and restore rebases or invalidates them.
- **M2, physical slots: resolved for I3-1's scope.**
  - The design prices about 100 slots per ingestion.
  - It gives time to exhaustion (248 days at 1 Hz aggregate, 25 days at 10 Hz).
  - It adds a separate `ArenaExhausted` refusal in a needs-host-action class.
  - Checkpoint and resume is the reclamation path. This matches spec 2.2's "future rebuild"
    honestly; see minor 1 and 2.
- **M3-M6 minors:**
  - Duplicate-contact text is now consistent: a duplicate refreshes nothing, and the test row
    agrees.
  - Rescaled sums and the 1e-150..1e150 bounds hold for sums of `max_candidates` reciprocals.
  - The retire tombstone preserves the incarnation floor.
  - `current` is defined.
  - Reboot without an incarnation bump surfaces as `StaleSequence` in `refused_count`.
  - The wall-step owner is the host.

## BLOCKING

None.

## MAJOR

None.

## MINOR

1. **The slot ceiling is memory-bound, not 2^31.** The default `allocationLimit_` is `ephemeralBit`
   (2^31 slots). At 32 bytes plus 12 per dimension per `CellSlot`, 2^31 slots is well over 64 GiB,
   so memory binds long before. Quote the actor's configured cell allowance and the actual bytes per
   ingestion, and restate the exhaustion horizon from that. The days figures are upper bounds.
1. **`ArenaExhausted` leaves no headroom for the recovery turns.** `retireSource`, advance turns and
   the refusal bookkeeping turn all allocate (tombstone about 10 cells, refusal records). At
   exhaustion, the retire path that `EpistemicBudget` relies on cannot run. Reserve a small slot
   headroom for these turns, or state that recovery is checkpoint and resume only. Also say whether
   events refused `ArenaExhausted` may be resent after the host action.
1. **`UnboundIncarnation` is classed Terminal.** The natural flow is: a rebooted device's first
   signed events arrive, `observe` refuses them, the host sees the higher incarnation and calls
   `bindSource`, and the events are valid again. Telling the host never to resend those bytes loses
   the first readings after every reboot. Class it as clearing after a bind (retryable or a
   bind-then-resend class).
1. **Host-asserted inputs rest on the host.** `setWallClockSynchronised` and the actor-mapped time
   mapping are host-trusted with no cross-check, so a wrong mapping is undetectable. That is
   acceptable for I3-1, but the clock mapping version should be resolved or rejected when I3-3
   lands. Keep that item on the later-slice list.
1. **Boot counter persistence is assumed.** A device that keeps its boot counter in RAM cannot
   express a higher incarnation, and so is locked out until the host's identity layer issues one.
   Add a line to the operator notes that incarnations must be persisted or host-issued.

## Tally

BLOCKING 0, MAJOR 0, MINOR 5.

VERDICT: PASS

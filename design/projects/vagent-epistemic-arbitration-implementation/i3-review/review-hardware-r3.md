# I3-1 revision 3 review: hardware, sensors, fault tolerance

Read the whole of i3-design.md (revision 3) against spec 2.1, 3.2, 5.1, 5.2, 7, 8.1.

## Round-2 findings: verification

- **M1, incarnation lifecycle: resolved.** Strictly increasing incarnations, supersession,
  `StaleIncarnation` and `unbindSource` are all specified. One residual gap is listed as minor 8.
- **M2, predecessor records and supports: resolved.** Principal-keyed supports mean there is no
  second support. Lower-incarnation records are ineligible on every key by rule. I4 must apply the
  rule; it is stated.
- **M3, fairness: mostly resolved.** There is a per-principal queue share and a keys-per-source
  bound. See minor 6.
- **M4, variance ceiling: resolved**, with `min_variance` and `max_variance`. See minor 5.
- **M5, cell model: arithmetic resolved.** The counter and the 32,768 ceiling are consistent with
  the field list, and 18 principals is stated honestly. It creates a new problem, major 2.
- **Minors:** `Duplicate` refreshes contact, `clock_skew`, stream time fields, bounded drain and
  observe-time bookkeeping are all resolved.

## BLOCKING

None.

## MAJOR

1. **Dedup runs before the incarnation check, so ID reuse after a reboot is refused.**

   - The digest covers every field, including the incarnation, and step 2 (dedup) precedes step 3
     (incarnation).
   - A rebooted device that restarts a counter-style event ID, which section 3 explicitly
     anticipates ("might reuse event IDs"), matches an ID still in the 16-pair window with a
     different digest. It is refused `ConflictingEvent`, a Terminal class, so the host is told never
     to resend.
   - The collision is certain when the previous boot was short, and otherwise happens whenever the
     new counter reaches an ID still in the window.
   - Fix: evaluate incarnation first. On advance, clear the dedup window with the stream reset, or
     key the window by (incarnation, ID). Test it.

1. **Replaced or retired devices leak the budget permanently.**

   - `max_sources` counts principals, supports are principal-keyed, and nothing reclaims the
     profile, supports or history of a principal that is no longer bound. `unbindSource` only
     revokes authorization.
   - A replaced sensor with a new key leaves roughly 1,600 cells. At about 18 active principals
     under the 32,768 ceiling, a handful of field replacements makes `EpistemicBudget` fire for
     healthy sources.
   - The refusal is Retryable but never clears. The leaked principal's last reading also stays
     current and unexpired until I3-3.
   - Fix: in 1b, let an operator retire a principal (`unbindSource(..., retire)`) that retires its
     records and supports and excises its profile and cells once no record is pinned. Report the
     refusal cause, either cells or principals, in the outcome.

1. **Receipt time uses `wallNow()`, which is wrong for embedded and unsynced clocks.**

   - The acquisition-versus-receipt check, the grant expiry and `receivedAt` (which I3-3 liveness
     will use) all depend on a clock that steps with NTP or an unsynchronised RTC at boot.
   - A backward step makes every new reading "acquired in the future", and it is refused
     `ObservationTimeMismatch` as Terminal (the content class). A forward step makes grants expire
     and retained records look ancient.
   - The class also blames the device for a local clock fault.
   - Fix: stamp both a monotonic receipt time and the wall time. Use the monotonic stamp for
     liveness, ordering and grant leases. Add a retryable `ClockUnsynchronised` refusal when the
     wall clock has stepped beyond a threshold, and keep the content class Terminal only for
     device-attributable mismatches.

1. **Acquisition-time monotonicity is enforced per stream, not per key.**

   - Step 4 refuses an acquisition time earlier than the stream's last as Terminal. A stream is
     (profile, predicate), shared across keys.
   - A radar, camera or multi-channel sensor legitimately emits keys (tracks) whose acquisition
     times are out of order because of per-key latency while the transport sequence is in order. The
     valid readings are discarded, and the Terminal class tells the host not to resend.
   - Fix: order acquisition time per (principal, key), keeping the transport sequence per stream.
     Also define what happens on a mapping-version change within an epoch (see minor 7).

## MINOR

5. `max_variance` has no checker ceiling. A policy of 1e300 satisfies "0 < min \<= max" but does not
   protect I4's arithmetic. Require `max_variance` below a named safe constant, or state what I4
   proves for an in-range value.
1. The per-principal share is 8 of 64, so 8 babbling principals still fill the actor queue. Consider
   a total-per-class cap and a dwell timeout, or document the limit.
1. Clock epoch and mapping-version regressions are not checked. A lower epoch arriving after a
   higher one is neither refused nor reset. A mapping-version refinement within an epoch can move
   acquisition backward. Define both (refuse the epoch regression, treat a mapping-version change
   like an epoch change for ordering).
1. Until 1c, a reboot cannot be recovered because the incarnation is fixed
   (`UnsupportedIncarnation`). Do not ship 1b to a deployment with real devices before 1c, and say
   so in the slice notes.
1. Heartbeat interval, timeout and freeze flags, and correction epoch are absent from the profile
   (the reasons list covers flags). Adding cells later is not reshaping, but I2U refuses populated
   roots, so fix this now or record the plan.
1. A first-time device whose observation is refused at `observe` (no profile yet) leaves only an
   outcome-log entry. A new device that is never accepted looks the same as one that never
   connected. Consider counting refusals per unbound-but-granted principal in executor counters.

## Tally

BLOCKING 0, MAJOR 4, MINOR 6.

VERDICT: FAIL

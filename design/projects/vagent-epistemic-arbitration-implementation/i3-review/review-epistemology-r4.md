# I3-1 revision 4: epistemology re-review

## Round-3 findings: status

| Finding                                     | Status                                                                                                                                                                                                         |
| ------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| R3-1 stale incarnation records stay current | Resolved: a higher bind queues an advance turn that retires lower records on every key. I4 requires incarnation equal to the profile's.                                                                        |
| R3-2 contact refresh                        | Resolved: a duplicate refreshes nothing, and only direct committed events and direct content refusals refresh contact. A relay-only source can now never show liveness, which is a limitation for I3-3 (R4-5). |
| R3-3 acquisition order                      | Resolved: per (principal, key) with uncertainties and skew.                                                                                                                                                    |
| R3-4 key limit                              | Resolved: terminal `EpistemicKeyLimit`.                                                                                                                                                                        |
| R3-5 writers                                | Resolved: the two `rtBSet_` sites named, with compiler guard assertions.                                                                                                                                       |
| R3-6 outcome key                            | Resolved.                                                                                                                                                                                                      |
| R3-7 signature boundary                     | Resolved: the host verifies, and the text states it.                                                                                                                                                           |

## New problems

### MAJOR

R4-1. **`retireSource` erases the replay floor, so old signed events become fresh evidence.**

- The retire turn excises the dedup window, streams and the profile, and revokes bindings. The
  highest incarnation, the last sequence per stream and the dedup state all lived there.
- If the operator rebinds the principal at the same incarnation (or the same grant), a genuine,
  correctly signed, old event (incarnation N, sequence 100) passes the host signature check and now
  commits as current evidence with a new receipt time. There is no staleness limit at ingress
  either: `observe` only rejects acquisition in the future, not acquisition arbitrarily far in the
  past.
- This is the replay-as-fresh-evidence case that spec 2.2 and 5.1 forbid ("expired IDs cannot be
  resubmitted as fresh").
- Fix: keep a minimal tombstone per retired principal (max incarnation, per-stream last sequence),
  or require any post-retire bind to use an incarnation strictly above the retired one and store
  that floor in the executor. Also enforce a policy `max_age` at ingress against `receivedWall`.
  Price the tombstone cells in `$epistemic_cells`. Test: retire, rebind, replay an old event, expect
  a terminal refusal.

R4-2. **An observation's incarnation, not the grant's, can advance the profile.**

- Section 3 and 6.2 let an observation whose incarnation is above the profile's perform the advance
  as its first step, "possible only if bindings race". But the `observe` table checks only "below
  the binding's" (`StaleIncarnation`) and `UnknownSource` is by (principal, predicate), not
  incarnation. So any bound principal can present a higher incarnation than its grant, passing
  `observe`.
- The effect is irreversible: it retires every lower record on every key and clears the dedup
  window, and it permanently locks out the genuine lower incarnation (`StaleIncarnation`, terminal)
  until `retireSource`. A corrupted or replayed boot counter, or a single signed event with a huge
  value, therefore causes lasting loss of the source's testimony without any host authorization.
- Fix: `observe` should require the observation's incarnation to equal the binding's. Only
  `bindSource` of a higher one advances the profile. Drop the in-turn advance, or confine it to an
  explicitly checked race window that is tested.

### MINOR

R4-3. The dedup window is cleared on advance. Replays of events of the superseded incarnation are
caught only at `observe` by the binding's incarnation. Add a test where the old incarnation's
binding is rebound, to confirm refusal.

R4-4. Acquisition tolerance (two uncertainties plus skew) lets a slightly older reading retire a
newer one within the tolerance. State this as accepted, with I4 freshness weighting by acquisition
time.

R4-5. A relay-only source never refreshes contact. I3-3 should define liveness for such sources (a
distinct relay attestation), or say they are excluded from heartbeat health.

R4-6. `receivedWall` stamp: the "unsynchronised" flag is host-reported, and there is no lower
staleness bound on acquisition time (covered by R4-1's `max_age`).

## Tally

BLOCKING 0, MAJOR 2 (R4-1, R4-2), MINOR 4 (R4-3 to R4-6).

VERDICT: FAIL

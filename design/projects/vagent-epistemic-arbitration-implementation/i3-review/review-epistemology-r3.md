# I3-1 revision 3: epistemology re-review

## Round-2 findings: status

| Finding                   | Status                                                                                                                                                                                |
| ------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| N1 incarnation ordering   | Mostly resolved: strictly increasing, `StaleIncarnation`, supersession, principal-keyed single support. A gap remains between bind and first ingestion (R3-1).                        |
| N2 clause writers         | Resolved: statements, `believes ... from`, peer tell and a compiler guard are enumerated. Restore never calls `rtBSet`. Initial facts and rule heads are not named explicitly (R3-5). |
| N3 replay damaging health | Resolved for counters (`content_refusals` vs `refused_count`), but contact-time refresh reopens a liveness channel (R3-2).                                                            |
| N4 digest                 | Resolved: executor-computed canonical digest, excluding `relayedBy`.                                                                                                                  |
| N5 token charset          | Resolved: 64 hex digits.                                                                                                                                                              |
| N6 terminal vs retryable  | Resolved, with one misclassification (R3-4).                                                                                                                                          |
| N7 latch reset            | Resolved: stated as I3-2.                                                                                                                                                             |
| N8 acquisition order      | Resolved, but the rule is too coarse (R3-3).                                                                                                                                          |

## New or remaining problems

### MAJOR

R3-1. **Superseded-incarnation records stay eligible until the new incarnation's first event.**

- Section 3 says a lower-incarnation record is ineligible "from the moment the profile advances",
  and the profile advances only inside an ingestion turn (6.3).
- `bindSource` of a higher incarnation supersedes the binding at once, but the arena profile is
  untouched. While the replacement device is silent, the old device's readings keep voting as
  current. The spec (2.1/2.2) says a replacement or reset "expires" the old state; a device being
  replaced is often the faulty one.
- Fix: have `bindSource` of a higher incarnation (or `unbindSource`) enqueue a profile-advance turn.
  Alternatively, define ineligibility against the executor's live binding, not the profile. Test
  "bind inc 2, no events yet: inc 1 records are ineligible".

R3-2. **Duplicates and refusals refresh `last contact`, which contradicts spec 2.1 and opens a
liveness spoof.**

- Spec 2.1: an exact authenticated retry "creates nothing and refreshes nothing". Revision 3 makes
  `Duplicate` refresh the profile's contact time (6.2), and the bookkeeping turn also sets last
  contact time for every refusal, including `refused_count` ones and relayed copies.
- A replayed or relayed old event therefore keeps a dead source looking alive to I3-3 heartbeat
  logic. That is a third party's testimony refreshing the source's standing, the same harm N3
  addressed for counters.
- Fix: refresh contact only for events that are directly received (no `relayedBy`), pass origin
  verification, and are not duplicates or replays. Otherwise follow the spec and refresh nothing. If
  the deviation is intended, it must be argued against the spec and priced, since heartbeat uses
  receive time.

### MINOR

R3-3. Acquisition-order refusal is per stream, which is (profile, predicate), not per key. A source
sampling key B earlier than key A, but sending B later, is refused `ObservationTimeMismatch`, a
terminal content refusal charged to the source. Make the monotonic check per (principal, key), and
tolerate regression within `acquiredUncertainty` plus `clock_skew`.

R3-4. `EpistemicBudget` is classed Retryable, but `keys_per_source` exhaustion cannot clear (there
is no key retraction before I3-2). Split the budget codes: transient cell-ceiling pressure versus
permanent key-count refusal.

R3-5. Name the remaining clause producers in the 1a test list: load-time declared facts, rule heads
deriving an arbitrated predicate, and host-posted beliefs. "Every writer enumerated" should list
them, or say they are impossible, since `rtBSet_` is also called at compiler.cpp ~4584 and ~4757.

R3-6. The outcome log is keyed by (principal, eventId), but incarnations may reuse event IDs by
design. Key by (principal, incarnation, eventId).

R3-7. State where the origin signature is actually verified. `CheckedObservation` carries no
signature, so section 7's "C++ verifies the origin signer" can only mean the host did so before
calling `observe`. The `relayedBy` flag is also a host claim, so refusal charging depends on it. Say
so in the trust-boundary text.

## Tally

BLOCKING 0, MAJOR 2 (R3-1, R3-2), MINOR 5 (R3-3 to R3-7).

VERDICT: FAIL

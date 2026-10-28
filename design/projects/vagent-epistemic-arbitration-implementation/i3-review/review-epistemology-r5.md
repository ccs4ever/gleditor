# I3-1 revision 5: epistemology re-review

## Round-4 findings: status

| Finding                               | Status                                                                                                                                                                                                                                          |
| ------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| R4-1 retire erases replay floor       | Resolved. The retire turn keeps a tombstone with the highest incarnation. A rebind needs a strictly higher incarnation. Required `max_age` refuses old events as `StaleObservation` at `observe`. The 1c tests cover retire, rebind and replay. |
| R4-2 observation advances incarnation | Resolved. Only `bindSource` advances. An observation must match the binding exactly (`StaleIncarnation` below, `UnboundIncarnation` above). The advance turn is queued ahead of pending ingestions.                                             |
| R4-3                                  | Resolved by the "also after the window is cleared" test.                                                                                                                                                                                        |
| R4-4                                  | Resolved by the per-key tolerance rule.                                                                                                                                                                                                         |
| R4-5, R4-6                            | Resolved: relay-only liveness is deferred to I3-3, and `max_age` plus the clock-basis split cover staleness.                                                                                                                                    |

No BLOCKING or MAJOR finding remains. Items below are minor.

## Minor findings

M1. **Digest includes host-mapped fields.** The digest covers every field except `relayedBy`,
including `acquiredAt`, `acquiredUncertainty`, `clockEpoch` and `clockMappingVersion`. Those are
host-derived mappings of the origin's signed time, not the signed payload. Two relays that map the
same event under different mapping versions get different digests, so the second copy is
`ConflictingEvent` rather than `Duplicate`. Spec 6.3 uses payload identity. Hash the origin-signed
payload only, or document that the host must map identically. The effect is noise, not
double-counting, because the first copy stays the only record.

M2. **Section 6.2 still says an above-profile incarnation "performs the advance".** Section 3 now
says only `bindSource` advances, and `observe` refuses `UnboundIncarnation`. Reword 6.2 as a
defensive check that refuses (or note it is unreachable), so the text and the code cannot diverge.

M3. **Bind-time floor check needs a host-side shadow.** `bindSource` refuses a lower incarnation or
a tombstoned principal at binding time, but it runs on the host thread while profiles and tombstones
live in the worker-owned arena. After resume the executor's bindings are empty. State where the
floor is read (a mirrored table rebuilt on resume, or let the bind succeed and refuse in the turn).
The turn-level `StaleIncarnation` check already makes this safe, but the bind result would otherwise
be misleading.

M4. **Tombstone content.** Specify "highest incarnation seen" as the maximum of the bound and the
profile incarnations, so a bound-but-silent incarnation is also a floor.

M5. **"Directly received" is a host contract.** Contact refresh and `content_refusals` depend on
`relayedBy` being absent. A replayed signed event sent by a different transport peer must be
labelled relayed. State this contract in section 3 and add a test (peer differs from origin,
therefore relayed). A direct replay by the origin's own key is otherwise charged and refreshes
contact.

M6. **Unbound or expired sources keep current records.** `unbindSource` and grant expiry revoke
authorization only, so past records stay `current` and visible to I4 indefinitely (no heartbeat
until I3-3). I4 must apply the policy `max_age` to current records at query time, not only at
ingress. Note it in section 6.

## Tally

BLOCKING 0, MAJOR 0, MINOR 6 (M1-M6).

VERDICT: PASS

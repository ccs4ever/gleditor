# I3-1 revision 2: epistemology re-review

## Round-1 findings: status

| R1 finding                             | Status                                                                                                                                         |
| -------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| B1 alias merges principals             | Resolved. Supports are named `<principal>#<incarnation>` and bind refuses alias collisions and second aliases (see N5 for a token-format gap). |
| B2 program `believe ... from` spoofing | Mostly resolved by the static `EpistemicIngressOnly`, but the list of writers is incomplete (N2).                                              |
| M3 changed value                       | Resolved by own-source transfer, one support per (identity, key).                                                                              |
| M4 receipt vs commit                   | Resolved: `TransportReceipt`, outcome log, executor-stamped receipt time.                                                                      |
| M5 grant recheck                       | Resolved (section 5 re-checks the binding in the adapter).                                                                                     |
| M6 relay                               | Resolved by origin signer plus `relayedBy`, with the digest in the dedup key.                                                                  |
| M7 idempotency                         | Resolved by the window, `ConflictingEvent`, and the sequence rule beyond the window.                                                           |
| Minors 8-11                            | Resolved.                                                                                                                                      |

## New problems introduced by the revision

### MAJOR

N1. **Incarnation ordering is not enforced, so one identity can have two current records.**

- Section 3 says a new incarnation retires its predecessor's current records, and "this design only
  enforces" that the device owns the boot counter. Nothing says incarnation must be strictly
  increasing.
- `bindSource` refuses a "second alias for the same (principal, predicate)". It does not say whether
  binding incarnation 2 replaces a live incarnation 1 binding, or is refused.
- If the old incarnation's binding stays live, or the host re-binds incarnation 1 after resume, then
  incarnation 1 events with a fresh sequence are accepted. They append to the old support and make
  it "current" again, and the same physical principal counts twice. This is the double-vote that
  identity-keying was meant to prevent.
- A lower incarnation is also an obvious replay or rollback vector. Dedup state for older
  incarnations is excised, per "only live and immediately previous kept".
- Fix: define `StaleIncarnation`. The profile stores the highest incarnation per principal, and
  ingress refuses lower or retired ones. Binding a higher incarnation atomically revokes the lower
  one. Retiring the predecessor's records must be total: every key, not only keys the new
  incarnation has reported. Test rollback and rebind.

N2. **Static refusal covers only "program text" statements.** Other writers of arbitrated clauses
exist: the `rtBSet_` call sites at compiler.cpp ~4584 and ~4757, and initial or declared facts.
Others are Horn-rule heads that derive an arbitrated predicate, host-posted beliefs, and any future
`assert`. A derived clause there would lack a record yet be indistinguishable from evidence to I4.

- Fix: enumerate every producer of clause dimension edges, and refuse or tag each. Rules deriving an
  arbitrated head should be `EpistemicIngressOnly` or explicitly derived and ineligible. State that
  I4 treats a support as eligible only if it has a current record whose profile matches the support
  token. That is a runtime backstop in addition to the checker. Add a test that goes through
  load-time facts and rule heads, not just statements.

N3. **Replay or relay can harm the honest source's health.** Section 5 runs a bookkeeping write on
any failure (reject count +1, refusal code) and the spec's I3-3 turns counts into suspicion and
fault. A replayed old event beyond the dedup window yields `StaleSequence` charged to the origin. A
relay or injected copy could therefore drive the source into SUSPECT or FAULTED, which is testimony
by a third party damaging a principal's standing.

- Fix: charge only failures that the origin itself caused. Do not charge when `relayedBy` is set, or
  when the refusal is `StaleSequence`, `Duplicate` or `ConflictingEvent` against an event whose
  signature check passed but which looks like a replay. At minimum, record the refusals in a
  separate counter that I3-3 gates on. Keep `reject count` for content refusals such as frame,
  units, variance and time.

### MINOR

N4. `payloadDigest` is supplied by the caller. Dedup and conflict detection are only as strong as
the host. Have the executor recompute it over a canonical encoding of the tuple and metadata, or
state that the host has verified the signature over the digest.

N5. The token `<principal>#<incarnation>` is ambiguous unless the principal alphabet excludes `#`.
Require a validated fingerprint charset (hex) at `bindSource`.

N6. Outcome handling: `Refused{StaleSequence}` and `Refused{Conflicting}` can never succeed on retry
with the same ID, yet section 4 says the host "retries a Refused event". Mark refusals as retryable
(budget, backpressure) or terminal.

N7. Inheriting a latched `FAULTED` across incarnation is conservative and consistent with spec 2.2,
but until I3-2 there is no reset authority. State that explicitly so tests do not expect a clear.

N8. A higher sequence with an older `acquiredAt` retires a newer-acquired current record. Retirement
is by arrival order, not by acquisition time. Flag it for I4 freshness, or refuse acquisition time
regress per stream.

## Tally

BLOCKING 0, MAJOR 3 (N1-N3), MINOR 5 (N4-N8).

VERDICT: FAIL

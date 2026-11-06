# I3-3D revision 1: security review (M2)

Verdict: **FAIL** (4 MAJOR, 9 MINOR). Section 2's four identity defects were checked against HEAD
and hold (details under "Verified claims"). The canonical-body rule, domain separation, audience
design and relay marking are sound. The failures are in revocation durability, report/heartbeat
timeliness, pairing, and one counted time code.

## MAJOR

**S-1. Revocation is fail-open across a restart (3.5, 13.1).** The design implements
`IRevocationRegistry` "over the controller", but its own fact 4 says device revocation lives in a
transient quarantine cache (`tqc_.revokedDeviceKeys`, `identity_network_controller.cpp:1124-1143`).
Scenario: a sensor key is revoked, the host restarts, the cache is empty, the revoked key's messages
verify again and commit. For `pinned` bindings the ledger is not consulted at all, so nothing else
catches it. Fix: state the registry's persistence source (the ledger or a stored revocation list)
and make the adapter fail closed (`KeyRevoked`/`RevocationUnavailable`, Retryable) until that source
has loaded, for every binding. Add a test: revoke, restart the registry, assert refusal. Also pass
the host wall clock, never a device time, as `timestamp` to `isDeviceRevoked`/`isSerialRevoked`
(`identity_engine.hpp:298-304` takes one); a device-claimed time would let a revoked key backdate.

**S-2. Reports and heartbeats have no stated timeliness check, so `expires` is ornamental (3.2, 3.8
step 5, 4.3, 6.1).** The row 7-9 checks (`ClockMappingMismatch`, `LateDelivery`, `MessageExpired`)
are defined only inside `observe` for observations. `health_report` and `heartbeat` have required
`expires`, but section 6 never applies it. Scenario: a relay captures a `fail` report (code
configured `isolates`) or a `raise`, withholds it, and delivers it hours later. Its sequence is
above `report_floor` (the floor moves only on commit), so it latches the source FAULTED, which only
an operator can undo, or it re-raises a reason an operator just removed with
`Clear {code, origin: device}`. Fix: run rows 7-9 on every kind before dispatch, in step 5, with
`UnknownClockMapping` for `free` epochs; add tests "a withheld `fail` delivered after `expires` does
not isolate" and "a withheld `raise` does not renew a cleared warning".

**S-3. Pairing can be starved and the nonce is not bound to the issuer (4.2).** Two defects. (a) "A
new one replaces it": any verified message for an unmapped `free` epoch starts pairing
(`UnknownClockMapping`, row 5), and rows 5-6 run before the stream floor, so a relay replaying one
old valid message once a second voids the outstanding nonce each time and the device's reply is
always `ClockPairUnsolicited`. At the default 1 s interval and 2 s timeout that is permanent denial
of a `free` device. (b) The text says "unknown nonce" refuses but not that the nonce is looked up by
(issuer, incarnation) and consumed on first valid reply. Nonces travel in clear; a different bound
source (or the relay) can sign a `clock_pair` echoing the victim's nonce under its own key,
consuming the victim's challenge. Fix: keep an unexpired challenge (never replace within the
timeout); start pairing only for a message above the host sequence floor; key pending challenges by
(issuer, incarnation) and compare the nonce in constant time; consume on first accepted reply; nonce
from the CSPRNG, with entropy failure refusing rather than falling back. (Offset forgery itself is
not possible: the device signs after receiving the nonce, so $\theta\in[t_1-d,t_2-d]$ holds whatever
the relay delays; a relay only widens $u_0$.)

**S-4. A relay or partition can make a healthy device count refusals through row 4 (4.3).** Row 4
(`StaleObservation`, `s - a > A + u_a`) is counted for direct and relayed copies. A device that
store-and-forwards and signs at send time (a common design: sign when transmitting) sends buffered
readings after a link cut with `s` late and `a` old. An attacker who cuts the link for longer than
`max_age` makes every buffered reading a counted device fault; `R`/`N` move for a healthy source. It
also contradicts the b1 test "a flush of stale readings from a healthy device moves nothing" (13.3).
The split rule "decided on signed bytes" is true but does not mean "device-caused": signed bytes are
the device's, the delay is the path's. Fix: keep row 4 uncounted (the v1 treatment) unless the
binding declares the device signs at acquisition (`sign_at_acquisition`); record the choice in Q7
for the ML expert. Rows 2 and 3 are safe as counted.

## MINOR

**S-5. Delegation cache key (3.5).** Keyed "(key, certificate digest)" it ignores the master. A
cached validation under master A would satisfy a binding naming master B, and a cache hit must still
re-run the active-master ledger lookup and revocation every message. Key by (master fingerprint,
key, digest) and say the hit skips only path validation.

**S-6. Wall-clock rollback (3.5).** The certificate window is judged against the host wall clock. An
attacker who controls the host's time source (NTP) can roll it back into a stolen key's expired
window; revocation, if present, still catches it, so this needs S-1 fixed. Fix: a host-memory
high-water mark of the wall clock seen by the adapter, and refuse delegated verification
(`ClockUnsynchronised`) when wall time is more than a configured step behind it. State the
assumption (a trusted time source) in the guide.

**S-7. Verify through `verifyDelegation`, not `DeviceDelegation` (3.5, 2).** At HEAD
`DeviceDelegation::verify(master)` passes `currentTime > 0 ? currentTime : issuedTimestamp`
(`user_permascroll.cpp:~245`), and `issuedTimestamp` is a TSV field outside the certificate, so with
`issuedTimestamp == 0` the window check is skipped entirely, not merely run at issue time. Section 2
fact 1 understates this. The adapter should call
`StandardCryptoEngine::verifyDelegation(cert, master, now)` directly and compare the certificate
subject key to `k`.

**S-8. Unbounded device-chosen integers (3.2, 4.3).** `sequence`, `clock_uncertainty`,
`acquired_uncertainty`, `sent`, `acquired` are full Int64. `s + u_a`, `u_a + u_d` and $u_M$ sums
overflow (UB, or a wrapped check that passes). `clock_uncertainty` also lengthens the freshness
bound of 5.1 ("path lateness within max plus its uncertainty") for heartbeats and reports, which
have no row 3. Fix: bound each at decode (for example below 2^52) and clamp $u$ in the freshness
test to `max_uncertainty`; compute in saturating or 128-bit integers.

**S-9. Payload hygiene (3.1, 3.3).** The tables say UTF-8 and length for `Str`, not charset.
`event_id`, `code`, `predicate`, `units`, `frame` reach the outcome key, audit entries and logs.
Newlines, NULs and bidi controls allow log injection and confusables. Fix: restrict identifier
fields to a printable ASCII class at decode; refuse unknown keys in `p`, `h` and the envelope (the
design says shape is checked, say "closed key sets"); refuse `c` on a pinned binding. New refusal
subjects carry only the principal token and code name: never the audience, a nonce, the certificate
DER, `k`, or OpenSSL's error strings.

**S-10. Pre-verification cost (3.8).** Garbage costs only decode, but a replayed valid message from
a bound issuer pays Ed25519 (50-70 us) and uncached path validation (to 300 us) before the floor
refuses it, and no budget applies per transport peer before step 4. Fix: refuse on the unverified
header's `sequence` at or below the host floor before the signature (this rejects only that exact
message, so it is safe), and add a per-peer verification budget. Also noted: step 3 refuses
`UnknownSource`/`AudienceMismatch` before the signature, which lets an unauthenticated peer test
whether an issuer is bound; accept only if the audience token is treated as non-secret (S-12).

**S-11. Device reports amplify (6.1, 17).** A compromised bound device can alternate `raise`/`clear`
at its full queue share. Each commit appends an audit entry, calls the R3-10 sink, and (I4-1b
obligation) bumps the agent's epistemic-state version, so a hostile sensor controls a counter other
predicates' cached arbitration depends on. Operator entries are protected (device entries evict only
device entries). Fix: a per-source minimum interval between report commits (setting), and say
whether a device-driven version bump invalidates other sources' state.

**S-12. Audience token is an identifier, not a secret (3.6).** It rides in clear in every message,
so every relay sees it. That is enough for its job (cross-agent replay needs the device's signature
too), but say so, and V-D must never use it as a capability or authenticator (Q2). Specify the mint:
CSPRNG, bind refused when it fails. Two resumes of one checkpoint share it (D2) and so share a
replay domain; the guide already carries that precondition.

**S-13. Deadline heap and timers (5.1, 5.3).** (a) "Min-heap of (deadline, pid, principal, stream)"
with a re-arm on every heartbeat grows by one entry per message under lazy deletion; use one entry
per key with a generation counter or indexed heap, and say the bound (sources x streams). (b) The
floor applies to `heartbeat_ms` only; `data_ms` and `freeze_window_ms` need the same floor or a
binding with `data_ms = 1` is the timer storm the floor exists to stop. (c) Coalesce due entries to
one `liveness` item per pid per wake. Cell census (12) is consistent with the 24,576 ceiling.

## Answers

**Q2 (token as V-D identity).** Yes, as an identity, with S-12's caveats: not secret, not a
capability, CSPRNG mint, D2 precondition. **Q3 (device key).** Agree. `deviceId` across rotation is
signed by the master, not the device, so a master compromise could move a calibrated reputation to
any key; keep it out of v1. **Q4 (incarnation auto-advance).** Allow, per binding, default `fixed`,
on three conditions: the advancing message passes S-2's timeliness check (a captured old
higher-incarnation message is not an advance); the step bound stays; and the advance rebinds every
predicate at once (the arena already supersedes lower bindings), audited. Pinned and delegated
alike; delegated needs a currently valid certificate. **Q5 (revoked-key residual).** Accept the
residual only with S-1 fixed. Add: the first `KeyRevoked` refusal for a bound issuer emits one
audit-sink event, so the operator learns of a live revoked device without polling; automatic
isolation stays refused (F14). Do not invent a revocation time for I4 to discount records by until
the revocation source records one.

## Checked and sound

- **Canonical body.** `bencode.cpp` refuses leading zeros, `-0`, unsorted or duplicate keys
  (`<= previousKey`, `:201`), trailing bytes and depth over 128, so the re-encode comparison is
  redundant but harmless. The 8-byte double is a bijection once `-0`, NaN and infinities are
  refused. Duplicate keys cannot reach the codec.
- **Domain separation.** Prefix-free argument holds: `xanadu-vagent-signed-v1\n` differs from the
  five existing domains at byte 7. No domain-less `sign()` of attacker-chosen bytes with a device
  key was found in `apps/common/xanadu` by grep of `signWithDomain`; the test enumerating `kDomain*`
  is the right guard.
- **Ed25519.** `StandardCryptoEngine::verify` already refuses a zero key and non-canonical `S`
  (`standard_crypto_engine.cpp:306-318`); the design adds the small-order key check. Add a refusal
  of non-canonical public-key encodings (y at or above p): a different `k` byte string cannot pass
  the issuer hash check, so this is hygiene only.
- **Text identities.** Issuer and audience are compared as lowercase text; uppercase spellings fail
  the equality. The `textEq` requirement covers the 16-hex `clock_instance`/`liveness_instance`
  (4.4); note that 16 hex digits are all-digit about once in 2,000 and `\d+e\d+` forms infer as
  Double, so the forcing test should include `00000000000e1234` and a leading-zero token, not only
  an all-digit one.
- **Relay marking.** From `TransportPeer` only; direct means peer equals issuer as text. Sybil
  relays would multiply fair shares, so the host must admit transport peers (record in the guide).
- **Replay.** Sequence order is checked before epoch (`compiler.cpp:4333-4343`), so a relay replay
  hits uncounted `StaleSequence`, not the now-counted `ClockEpochRegression`; the single sequence is
  enforced per kind by three separate floors (arena stream, host heartbeat, `report_floor`), not
  one; harmless because the kind is signed, but the 3.2 sentence "strictly increasing across all
  kinds" is a device rule, not a host check, and should say so.
- **Hiding a faulty source.** A device claiming `synchronised` with a bad clock lands in uncounted
  rows 7-8 and is visible only as a WARN diagnostic (4.3); a relay that delays three healthy sources
  can suppress that WARN through the common-cause test. Residual, consistent with the recorded
  selective-withholder power, which also covers false `silent`, `stale` and `frozen` flags.
- **Unlatch and operator reasons.** Reports are origin-tagged, cannot reach `healthEntry`, cannot
  unlatch, and `Clear` by origin separates operator and device reasons. Prune keeps the tombstone
  incarnation floor.

## Verified claims (section 2)

1. `trustDelegation` calls `verify(masterPubKey)` with the default time
   (`identity_network_controller.cpp:1005-1012`): confirmed, with the S-7 amplification.
1. `delegatedDeviceKeys_.insert_or_assign(masterFingerprint, ...)` at `:1011`: one key per master.
1. `IRevocationRegistry` (`identity_engine.hpp:295`): no implementer anywhere in
   `apps/common/xanadu`.
1. `signWithDomain`/`verifyWithDomain` concatenate with no length
   (`standard_crypto_engine.cpp:343-383`): confirmed. SHA-1 `Fingerprint` against 64-hex principals:
   not re-read; consistent with `computeFingerprint` use in `DeviceDelegation::verify`.

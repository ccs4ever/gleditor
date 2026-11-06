# I3-3 design: signed ingress, clock mapping, liveness and sensor reports

Status: revision 2.1. Revision 2.1 closes the ML round-2 confirmation's new MAJOR N1 and its
capacity MINOR (section 4.5, section 12, dispositions); the ML expert closed B1 and M1 to M4 in that
round. Revision 2 came after the M2 design round 1 (ML FAIL with 1 BLOCKING, 4 MAJOR and 12 MINOR;
SEC FAIL with 4 MAJOR and 9 MINOR; [reports](i3-3-review/)). Every finding is dispositioned in the
last section. Under [decisions.md](decisions.md) section 0.1, no confirmation round is run except
for the ML BLOCKING and MAJOR findings, which go back to the ML expert, and the SEC MAJOR findings,
which go back to SEC, each on its finding alone.

Authority: approved P19 v1.5 sections 2.1 ("Profiles, observations and provenance", lines 68-170),
2.2 ("Upgrade and checkpoint translation", lines 225-245), 3.2 ("Freshness and physical
uncertainty", lines 322-353), 4 (health grants and sensor reports, lines 466-477), 5.1 ("One
mutation path", lines 558-584), 5.2 (line 589, no extrapolation of old positions), 7 (the sensor
table, lines 899-921) and 8.2's lifecycle regressions (lines 980-1010). Plan: the I3-3D row of
[remaining-plan.md](remaining-plan.md) section 4.4 and slices I3-3a to I3-3d (section 4.5). Rulings
applied: O4 (the reference adapter), P2-2 (the signed header common to every kind is fixed here), F5
(the host time class "stays out of `f` until I3-3"), F14, F22 and A3 (incarnations), A2 and A6
(actor-wide health settings, reason origins), A9 (pass credits), D2 (fork precondition), R3-11
(version cells), the owner's M1 ruling that text identities are compared as text
([t1-validation.md](t1-validation.md) line 320), the I3-2 obligations on I3-3
([i3-2-design.md](i3-2-design.md) section 13, owner question 6 in section 14), and the ML expert's
answer to this design's Q7 (round 1, review-ml.md, "Answer to Q7").

Code citations are `path:line` at HEAD `cbe5f6c9` on `design/vagent-epistemic-arbitration`, read
with `git show HEAD:` because other workers hold uncommitted edits in `epistemic_emit_health.cpp`
and `compiler_gen.hpp`. Paths are relative to `apps/common/xanadu/` unless they start with `tests/`
or `design/`. The code cited in revision 1 at `1df5683c` is unchanged at `cbe5f6c9`, which touched
only design records. Parts the ML reviewer must read are marked **[ML]** in their headings: 4.3, 4.4
(segments), 5.5, 6.1 (the bounds), 7, 12 (census) and 17.

## 1. What this design decides

| #   | Decision                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 | Section |
| --- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------- |
| 1   | One signed message: a bencode envelope around a canonical bencode body, signed with Ed25519 over the domain `xanadu-vagent-signed-v1\n` followed by the body. Doubles travel as their 8-byte IEEE-754 bit pattern; `-0` and non-finite values are refused. Key sets are closed at every level, and identifier fields are printable ASCII.                                                                                                                                                                                                                                                                | 3.1     |
| 2   | A header common to every kind, with a table of which kind uses which field. Kinds: `observation`, `heartbeat`, `health_report`, `clock_pair`; `verification` is reserved for I3-4a, with its header fixed now. One sequence space per (issuer, incarnation) is a device rule; the host enforces it with three floors.                                                                                                                                                                                                                                                                                    | 3.2     |
| 3   | The principal is the SHA-256 of the signing device key, compared as text. A binding trusts a key `pinned` or `delegated`. Delegated keys are verified through `verifyDelegation(cert, master, now)` at receipt against a wall clock with a rollback guard. Revocation is checked on every message against a **durable** registry, and the adapter **fails closed** until it has loaded.                                                                                                                                                                                                                  | 3.5     |
| 4   | The audience is a non-secret 64-hex identifier minted once per arbitrating agent from the CSPRNG, kept in the arena and carried by resume and upgrade. It is never a capability or an authenticator. The payload also binds the predicate and its policy hash.                                                                                                                                                                                                                                                                                                                                           | 3.6     |
| 5   | `relayedBy` comes only from the transport peer the host hands the adapter. A listener that cannot authenticate its peer names itself, so W1 supports relay by construction.                                                                                                                                                                                                                                                                                                                                                                                                                              | 3.7     |
| 6   | The origin signs its raw device times. The host owns the mapping: `synchronised` or `paired`. A pairing challenge is keyed by (issuer, incarnation), never replaced within its timeout, started only by a message above the host floor, and consumed on the first accepted reply.                                                                                                                                                                                                                                                                                                                        | 4.1-4.2 |
| 7   | **[ML] A code is counted only if every site that emits it decides on signed fields alone.** Revision 1's split is corrected on four points: the support walk's mapped-order branch gets an uncounted code (`AcquisitionOrderMismatch`); non-finite or overflowing mapped values refuse `InvalidClockMapping`, uncounted; device hold time (`StaleObservation`) stays uncounted; `ConflictingEvent` stays uncounted. A script pins the emitting sites. Counted-code version 2 adds only `ObservationTimeMismatch` (row 2) and `UncertainAcquisition` (row 3) and counts all its codes for relayed copies. | 4.3     |
| 8   | The record is the mapping snapshot. Across time the agent keeps a clock **lineage of segments**: a new segment starts whenever an executor starts or the wall-minus-steady offset jumps beyond its bound (suspend, wall step). Rebase goes through the segment that contains a time, within one instance as well as across instances.                                                                                                                                                                                                                                                                    | 4.4     |
| 9   | Liveness: `heartbeat_ms` and `data_ms` expectations, both above a floor; freshness requires the path-lateness bound on every kind; an indexed deadline heap with one entry per key; due entries coalesced to one control item per pid per wake; arena flags written only on transitions.                                                                                                                                                                                                                                                                                                                 | 5       |
| 10  | [ML] Freeze is decided once per excitation window by a declaration-level check. If a majority of the excited streams freeze together, the declaration is flagged and no source is. An excitation-derived `frozen` expires one window after the excitation ends.                                                                                                                                                                                                                                                                                                                                          | 5.5     |
| 11  | [ML] Reasons get three separate bounds: operator `R - 1` (unchanged), device `Rd` and evidence `Re`. Neither devices nor operators can ever consume an evidence slot. Reports pass the same timeliness checks as readings, are rate-limited per source, and an operator `Clear` of a device reason raises the report floor so that a report signed earlier cannot re-raise it.                                                                                                                                                                                                                           | 6       |
| 12  | The binding table and the report vocabulary are configuration, carried as record groups in `system://settings` (Q1). The durable revocation store is Q9. I3-3 itself takes all three from its host as C++ values.                                                                                                                                                                                                                                                                                                                                                                                        | 9       |
| 13  | Seven slices, about 7,900 lines against the plan's 5,000 and revision 1's 7,300.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         | 13      |

No change to the store, the VM or the Vortex standard library is proposed. Three changes outside the
vagent runtime are put to the owner rather than assumed: the `SystemStoreModel` reader (Q1), where
durable device revocations live (Q9) and the executor's steady clock source (Q10).

## 2. What exists at HEAD

**Ingress.** `observe` (`vagent/runtime/agent_executor.cpp:3740`) takes a `CheckedObservation` whose
principal "is the origin signer, never the transport peer"
(`vagent/runtime/epistemic_ingress.hpp:26-48`). Nothing verifies a signature: the file comment says
the host's adapter "has already verified" it (`epistemic_ingress.hpp:5`), and no adapter exists
(X-3). `acquiredAt` and `acquiredUncertainty` are host-mapped doubles, and `clockMappingVersion` is
an opaque integer that nothing resolves (`epistemic_ingress.hpp:38-43`). The digest covers the
origin-signed fields **including the sequence**, and excludes times and the relay
(`agent_executor.cpp:3877-3889`).

**Time checks in `observe`.** A non-finite or negative mapped value refuses `InvalidAcquisition`
(`agent_executor.cpp:3837-3839`). A clock-basis mismatch refuses `ObservationTimeMismatch`
(`:3843-3844`), and so does a future mapped time (`:3856-3857`). A wall basis requires
`wallSynchronised` (`:3845`). Then come `UncertainAcquisition` (`:3853`) and `StaleObservation`
(`:3858`), all checked against host receipt.

**Time checks in the arena.** The stream check refuses a lower sequence (`compiler.cpp:4334`), an
epoch regression as `ClockEpochRegression` (`:4342-4344`) and a mapping regression within one epoch
as `ClockMappingRegression` (`:4348-4350`). The support walk emits `ObservationTimeMismatch` at two
sites with different inputs:

- An **older epoch** than the current record's (`compiler.cpp:4541-4545`), decided on the epoch.
- A **mapped acquisition** earlier than the current record's mapped acquisition minus both
  uncertainties and the skew (`compiler.cpp:4549-4556`), decided on host-mapped doubles.

**How the host counts an arena refusal.** `finishIngest` counts a refused event by its code name
alone: `Ingress::isHealthRefusal(code, relayed)` and the stream ring
(`agent_executor.cpp:1820-1824`). A retry guard skips the count when the committed entry is still in
the outcome log (`:1797-1800`). So a code name is counted wherever it is emitted, and the counted
list is only as sound as the least sound site that emits a listed name. Revision 1 missed this;
section 4.3 is built on it.

**The host time class (F5).** The counted list (`vagent/runtime/epistemic_health.hpp:74-92`, version

1. holds four signed-basis codes and two host-mapped ones (`InvalidAcquisition` and
   `ClockEpochRegression`, counted only for direct copies). The health half-life default is 32
   counted events (`vagent/runtime/epistemic_limits.hpp:55`). The pass-credit gap default is 1,024
   (`:65`).

**Liveness.** A commit writes `last_receive`; only a direct copy writes `last_contact`
(`compiler.cpp:4429-4436`). Nothing reads either. A resume drops both
(`vagent/runtime/epistemic_translate_registry.cpp:295-297`). Records carry `clock_instance`, a
random 16-hex name of the executor's steady clock
(`vagent/runtime/agent_executor_internal.hpp:422-432`), which is `std::chrono::steady_clock` through
`AgentClock::system()` (`vagent/runtime/agent_executor.hpp:150-154`): `CLOCK_MONOTONIC` on Linux,
which stops during suspend. The executor's deadlines are wall-clock and folded into
`earliestDeadline` (`agent_executor.cpp:3140-3160`).

**Health.** Reasons carry `kind`, `code` and `origin`
(`vagent/runtime/epistemic_abi_health.hpp:120-131`). The degrade path counts only operator reasons
against `R - 1` and leaves one slot "for the evidence-driven reasons I3-3 adds"
(`epistemic_emit_health.cpp:297` at HEAD). I4-D1 and the ratified A12 and A17 already name three
evidence-origin reasons for that slot: `gate_fraction`, `reference_suspect` and `gate_excluded`.
`Clear` carries only an optional code (`vagent/runtime/health_grants.hpp:115-118`).

**Identity.** `StandardCryptoEngine::verify` refuses a zero key and a non-canonical `S`
(`identity/standard_crypto_engine.cpp:306-318`). `signWithDomain` and `verifyWithDomain` concatenate
the domain and the message with no length prefix (`:343-383`). The five domains in use are listed at
`identity/identity_engine.hpp:23-30`. Four facts constrain reuse:

1. `DeviceDelegation::verify(master, currentTime)` passes
   `currentTime > 0 ? currentTime : issuedTimestamp` to `verifyDelegation`
   (`user_permascroll.cpp:245-248`). `issuedTimestamp` is a TSV field outside the certificate, and a
   zero `issuedTimestamp` skips the window check entirely.
   `IdentityNetworkController::trustDelegation` calls `verify` with the default 0
   (`identity/identity_network_controller.cpp:1005-1012`).
1. The controller keeps one delegated key per master (`delegatedDeviceKeys_`, `:1011`).
1. `Fingerprint` is 40 uppercase hex of SHA-1 (`identity/identity_layout.hpp:56-77`). A principal
   token is 64 lowercase hex (`agent_executor.cpp:3523-3527`), which `computeFingerprintSha256`
   produces (`identity/standard_crypto_engine.cpp:935-947`).
1. `IRevocationRegistry` (`identity/identity_engine.hpp:295-309`) has no implementer. Device
   revocation lives only in the controller's in-memory quarantine cache
   (`identity_network_controller.cpp:1121-1150`), which is empty after a restart. Master revocation
   lives in the ledger (`merkle_ledger.hpp:162`).

**Encodings.** `xanadu::bencode` refuses leading zeros, `-0`, unsorted or duplicate keys, trailing
bytes and depth over 128 (`bencode.cpp:158-201`). The actor wire writes a double as shortest decimal
and reads anything `from_chars` accepts (`vagent/runtime/wire.cpp:218-229`, `:300-309`).

**Translator.** Fourteen owned structures are registered (`epistemic_translate_registry.cpp:267`),
with host-state rows beside them (`:412-455`).

## 3. The signed message (I3-3a)

### 3.1 Envelope, body and canonical bytes

```text
envelope = bencode dict {            -- closed key set; nothing else
  "b": body bytes (string)           -- the signed bytes
  "c": certificate DER (string)      -- delegated bindings only; refused on pinned
  "k": signer public key (32 bytes)
  "s": Ed25519 signature (64 bytes)
}
body     = bencode dict { "h": header dict, "p": payload dict }   -- closed key sets
signed   = "xanadu-vagent-signed-v1\n" || body
```

The envelope is not signed and needs no canonical form. Tampering with `k` breaks the issuer check,
with `c` the delegation, and with `s` the signature. The body is canonical when
`bencode::decode(body).encode() == body`. The decoder already refuses every non-canonical form, so
the comparison is a belt over its braces; it costs one encode. An unknown key in the envelope, the
header or the payload is `MalformedMessage`, and so is a `c` on a pinned binding
(`UnexpectedCertificate`, once the binding is known at step 3).

| Type             | Encoding                                                          | Refused at decode                                    |
| ---------------- | ----------------------------------------------------------------- | ---------------------------------------------------- |
| Int              | bencode integer                                                   | $\lvert v\rvert \ge 2^{62}$ (overflow guard, S-8)    |
| Float            | bencode string of exactly 8 bytes, IEEE-754 binary64, big-endian  | NaN, ±inf, `-0`                                      |
| Bool             | bencode integer 0 or 1                                            | any other integer                                    |
| identifier       | bencode string of bytes 0x21 to 0x7E, length bounded per field    | anything else: no space, control, newline, NUL, bidi |
| Str (tuple text) | bencode string, valid UTF-8, length bounded                       | invalid UTF-8, over the bound                        |
| time (device)    | bencode integer, nanoseconds on the device clock named by `epoch` | the Int bound                                        |
| optional field   | absent key                                                        | present but empty                                    |

The identifier fields are `event_id`, `predicate`, `policy`, `frame`, `units`, `code`, `issuer` and
`audience`. They reach outcome keys, audit entries and logs, so restricting them at decode removes
log injection and confusables in one place. Tuple text is a reading, not an identifier, and stays
UTF-8. A refusal's log line and its outcome subject carry only the principal token and the code
name: never the audience, a nonce, the certificate, `k` or an OpenSSL error string.

The Int bound at $2^{62}$ (about 146 years in nanoseconds) leaves room for every sum section 4.3
forms. Those sums are still computed in 128-bit integers before conversion to seconds, so no
overflow can turn a refusal into a pass. Negative values pass decode, because whether a negative
time is a fault is a question about the device: section 4.3 row 1 answers it after the signature
proves whose bytes they are.

**Why 8 bytes and not the actor wire.** A device would otherwise have to reproduce `std::to_chars`
shortest round-trip exactly (`"1e+21"`, not `"1e21"`), and the signed format would freeze the actor
wire. A bit pattern is one `memcpy`, and with `-0` refused it is a bijection with the finite values.
`-0` is refused, not normalised, so two byte strings never mean one reading.

### 3.2 The common signed header

| Field               | Type          | Meaning                                                                                                    |
| ------------------- | ------------- | ---------------------------------------------------------------------------------------------------------- |
| `kind`              | identifier    | `observation`, `heartbeat`, `health_report`, `clock_pair`; `verification` reserved (I3-4a); others refused |
| `issuer`            | 64 hex        | `sha256hex(k)`: the source principal                                                                       |
| `incarnation`       | Int ≥ 1       | the device's boot or key generation (A3)                                                                   |
| `sequence`          | Int ≥ 0       | device rule: strictly increasing per (issuer, incarnation) across all kinds                                |
| `audience`          | 64 hex        | the receiving agent's audience identifier (3.6)                                                            |
| `sent`              | time          | device clock at signing                                                                                    |
| `epoch`             | Int ≥ 0       | the device clock's epoch; a reboot of a free-running clock changes it                                      |
| `basis`             | identifier    | `synchronised` (tracks wall time) or `free` (local monotonic)                                              |
| `clock_uncertainty` | Int, ns       | the device's bound on its clock against its basis; 0 for `free`                                            |
| `expires`           | time, opt.    | device clock after which the message is void                                                               |
| `low_watermark`     | Int ≥ 0, opt. | lowest sequence the issuer will still retransmit (P2-2)                                                    |

| Field                 | observation | heartbeat | health_report | clock_pair | verification (I3-4a) |
| --------------------- | ----------- | --------- | ------------- | ---------- | -------------------- |
| all of the first nine | required    | required  | required      | required   | required             |
| `expires`             | optional    | required  | required      | refused    | V-D decides          |
| `low_watermark`       | refused     | refused   | refused       | refused    | required             |

The single sequence space is a **rule for the device**, which costs it one counter. The host does
not check it as one space. It enforces three floors: the arena's per-predicate stream floor for
observations (`compiler.cpp:4334`), a host floor per issuer for heartbeats, and the arena's
`report_floor` for reports (6.1). That is harmless because the kind is under the signature, and it
is stated so that nobody reads the table as a host check. Gaps are expected. A refused field is a
`MalformedMessage`, so a verification-only field cannot ride on another kind.

**Retransmission is byte-identical.** A device that retransmits resends the same envelope. A new
signature over the same event ID with a new sequence is a different event as far as the digest is
concerned (section 2: the digest covers the sequence). Section 4.3 therefore keeps
`ConflictingEvent` uncounted, so a device that breaks this rule produces a diagnostic, not a health
count.

### 3.3 Payloads

- **observation**: `event_id` (identifier, at most `kMaxEventIdBytes`,
  `vagent/runtime/epistemic_limits.hpp:100`), `predicate`, `policy` (64 hex, the predicate's
  `policyHash`), `tuple` (list, typed by the declaration), `acquired` (time), `acquired_uncertainty`
  (Int ns), `frame`, `units`, `variance` (Float, optional, continuous only).
- **heartbeat**: empty dictionary.
- **health_report**: `action` (`raise`, `clear` or `fail`) and `code` (identifier, at most 64
  bytes).
- **clock_pair**: `nonce` (32 bytes).

### 3.4 Domain separation

One domain, `xanadu-vagent-signed-v1\n`, for every kind, with the kind under the signature. Because
`verifyWithDomain` concatenates without a length, the domains must be prefix-free. All six share
`xanadu-` and differ at byte 7 (`a`, `d`, `p`, `o` against `v`). A test enumerates every `kDomain*`
constant and asserts pairwise that neither is a prefix of the other. The version is in the domain: a
version 2 body is a new domain, and the version 1 reader is deleted.

### 3.5 Keys, delegation and revocation

A binding names the principal as 64 hex and one of two trust modes.

- **pinned**: the adapter checks `sha256hex(k) == issuer == binding.principal` as text, then the
  signature.
- **delegated**: as pinned, and the binding also names a master by the 64-hex SHA-256 of its key.
  The adapter resolves the master's key through a `MasterKeys` interface (reference implementation:
  `MerkleLedger::findByFingerprint`, active links only) and calls
  `StandardCryptoEngine::verifyDelegation(cert, masterKey, now)` **directly**, with `now` the host
  wall clock in seconds, never 0. It then compares the certificate's subject key with `k`.
  `DeviceDelegation::verify` is not used, because of fact 1. The certificate must be valid at
  receipt, and the mapped acquisition must not precede `notBefore`.

**Wall clock.** A delegated binding needs a synchronised wall clock; otherwise the message refuses
`ClockUnsynchronised` (Retryable), and the guide recommends pinned mode for such hosts. The adapter
keeps a high-water mark of the wall clock it has seen (host memory). A wall time more than
`vagent.epistemic_wall_rollback_ms` behind it also refuses `ClockUnsynchronised` for delegated
bindings, so an attacker who steers the host's time source cannot roll it back into a stolen key's
expired window without tripping it. The mark is lost at a restart; the guide states the remaining
assumption, a trusted time source.

**Cache.** A validated chain is cached by (master fingerprint, `k`, certificate digest) until the
earlier of `notAfter` and the cache bound (`vagent.epistemic_delegation_cache`). A cache hit skips
only path validation: the active-master lookup in the ledger and the revocation check run on every
message.

**Revocation, durable and fail-closed (S-1).** The adapter takes an `IRevocationRegistry` with an
added `ready()`. I3-3a1's reference implementation, `DurableRevocationRegistry`, has three inputs:

- the ledger, for masters (persistent, consulted by the 64-hex name);
- a durable list of signed `DeviceRevocationRecord`s, each verified under `kDomainDeviceRevocation`
  when loaded, for device keys and certificate serials;
- the controller's quarantine cache, as a live feed that also appends to the durable list.

Where the durable list lives is Q9. Until `ready()` is true, every message from every binding,
pinned or delegated, refuses `RevocationUnavailable` (Retryable): a restart can never reopen a
revoked key. The timestamp passed to `isDeviceRevoked` and `isSerialRevoked` is the host wall clock,
never a device time. A revoked key refuses `KeyRevoked` (NeedsHostAction). The first `KeyRevoked`
for a bound issuer emits one audit-sink event, so the operator learns of a live revoked device
without polling. Readings committed earlier stay current until an operator isolates (Q5). No
revocation time is invented for I4 to discount records by: the durable list has none to give.

**Key checks.** The adapter refuses a small-order key (`isSmallOrderPoint`) and a non-canonical
point encoding ($y \ge p$) as `InvalidSignerKey`. A non-canonical `S` is already refused by
`verify`.

**The principal is the device key.** One master's fifty sensors are fifty sources. Rotating a key
makes a new source, whose calibration starts from the prior (Q3).

### 3.6 Audience binding

The header's `audience` must equal the receiving agent's identifier, and the payload's `predicate`
and `policy` must equal the binding's predicate and its `policyHash`
(`agent_executor.cpp:3607-3609`).

The identifier is 64 lowercase hex from the CSPRNG. If entropy fails, the bind refuses
`EntropyUnavailable` (the existing code); nothing falls back to a weaker source. It is minted at the
first bind of an arbitrating agent and passed in `$turn.source_change`. The source-change routine
writes it to a three-cell record `EpistemicAudience` on the agent's `d.epistemic` rank only if none
exists, so a racing second bind keeps the first. `AgentExecutor::audience(pid)` returns
`std::optional<std::string>`. The adapter keeps a mirror, cleared at resume. A message that arrives
before the identifier commits refuses `AudienceUnavailable` (Retryable).

**It is an identifier, not a secret.** It travels in clear in every message, and every relay sees
it. That is enough for its job, because a cross-agent replay also needs the device's signature, and
it is all the job needs. It must never be used as a capability or an authenticator. Step 3 of 3.8
therefore lets an unauthenticated peer learn whether an issuer is bound to an audience; bindings are
not secret, and the design accepts that.

Comparison is as text, with `textEq` in the IR (`vagent/runtime/compiler_gen.hpp:905-920`). The
forcing test includes an all-digit identifier, a `\d+e\d+` form (`00000000000e1234…`) and a
leading-zero form, since a checkpoint types all three as numbers. The same applies to the 16-hex
`clock_instance` and the lineage segment names (4.4). Two resumes of one checkpoint share the
identifier and therefore a replay domain; D2 makes that an operator precondition.

### 3.7 Relay marking

The entry point is `ingest(bytes, TransportPeer peer)`, where `TransportPeer` holds one principal
token that the host's transport authenticated. If it equals the issuer as text, the copy is direct;
otherwise `relayedBy` is that principal. A listener that cannot authenticate its peer is itself the
relay, under a principal the host mints for it. W1's CLI is such an endpoint, so relay works through
it, and the relay-marking test runs through the CLI.

Since every counted code is now decided on signed bytes (4.3) and liveness is relay-agnostic (5.1),
the mark affects only the fair share per relay (`agent_executor.cpp:3968-3977`), the per-peer
verification budget (3.8), the audit field and `firstCharge`. The host must admit transport peers: a
peer that can mint relay identities at will multiplies its fair shares. The guide says so.

### 3.8 Verification order

1. **Per-peer budget.** A token bucket per `TransportPeer` (`vagent.epistemic_verify_per_peer`, per
   second) refuses `IngressBackpressure` (Retryable) before any work.
1. **Shape.** Size against `vagent.epistemic_signed_message_bytes`, envelope decode, closed key
   sets, the body's canonical form, field types and bounds, and the kind's field table. Refusals:
   `OversizeMessage`, `MalformedMessage`, `NonCanonicalMessage`, `UnsupportedKind`.
1. **Key and binding.** The key is canonical and not small-order, and `sha256hex(k) == issuer`. The
   audience resolves to a pid, and a binding of (pid, issuer) exists. `c` is present exactly when
   the binding is delegated. Refusals: `InvalidSignerKey`, `IssuerKeyMismatch`, `AudienceMismatch`,
   `AudienceUnavailable`, `UnknownSource`, `UnexpectedCertificate`, `DelegationMissing`.
1. **Seen-before, unverified (S-10).** A heartbeat or report whose sequence is at or below the host
   floor for its kind, or an observation whose (issuer, incarnation, sequence) is in the issuer's
   ring of recently verified sequences (bounded by `vagent.epistemic_health_replay_ring`), refuses
   `StaleSequence` or `Duplicate` without a signature check. This is safe: it refuses only a message
   carrying a header that a verified message has already used, and a forged message with that header
   would have failed anyway.
1. **Signature and trust.** Revocation readiness, then the signature, then delegation and
   revocation. Refusals: `RevocationUnavailable`, `BadSignature`, `DelegationInvalid`,
   `DelegationExpired`, `KeyRevoked`, `ClockUnsynchronised`.
1. **Timeliness, every kind except `clock_pair` (S-2).** The mapping rows 5 to 9 of 4.3, and the
   freshness bound of 5.1 for heartbeats and reports. A `clock_pair` is bounded by its challenge's
   timeout instead (4.2).
1. **Dispatch.** Observations go to rows 1 to 4 and `observe`, heartbeats to 5, reports to 6, and
   clock pairs to 4.2.

Steps 1 to 5 neither log, charge nor note anything, so an unauthenticated flood cannot grow or evict
the outcome log. From step 6 on the message is the issuer's, logged and charged by the existing
rules. The order puts every check that needs no signature before the Ed25519 verify (about 60 µs)
and the uncached path validation (to about 300 µs), so a replayed valid message costs a hash lookup.

### 3.9 What a gateway can do

| A gateway that…                | Is caught by                                             | Residual                                                                        |
| ------------------------------ | -------------------------------------------------------- | ------------------------------------------------------------------------------- |
| alters any signed byte         | signature                                                | none                                                                            |
| redirects to another agent     | audience                                                 | none                                                                            |
| redirects to another predicate | payload predicate and policy hash                        | none                                                                            |
| duplicates or replays          | step 4 (unverified), dedup, the floors                   | a heartbeat replayed within `max_path_lateness_ms` after a restart              |
| reorders within a stream       | stream floor refuses the earlier one                     | reorder equals drop; the gap diagnostic (5.2)                                   |
| delays                         | rows 7 to 9 and the freshness bound, on every kind       | delay within the bound is invisible; it costs liveness accuracy up to the bound |
| withholds a report             | timeliness (step 6); the clear floor (6.1)               | none past `max_path_lateness_ms`                                                |
| starves pairing                | the challenge is never replaced within its timeout (4.2) | none                                                                            |
| steals a nonce                 | challenges are keyed by (issuer, incarnation)            | none                                                                            |
| drops or withholds readings    | silence timers; sequence gaps                            | a selective withholder can raise `f` (I3-2 3.1, recorded)                       |
| delays several healthy sources | nothing                                                  | it can suppress the common-cause WARN (4.3); recorded                           |
| claims to be the origin        | `relayedBy` comes from the transport                     | none                                                                            |
| floods verified-looking bytes  | the per-peer budget, then step 4                         | the budget's rate                                                               |

### 3.10 Incarnations

A binding's incarnation policy is `fixed` (the operator binds each incarnation, as today) or
`signed`. Under `signed`, a message advances the bound incarnation only if four conditions hold: it
is fully verified; it passes step 6's timeliness checks, so a captured old message with a higher
incarnation is not an advance; the step is at most `vagent.epistemic_incarnation_step` (otherwise
`IncarnationStep`, NeedsHostAction); and the adapter rebinds every predicate of the principal in its
table at the new incarnation at once, because a superseding bind unbinds all of the principal's
lower bindings and each predicate must be bound again (`agent_executor.cpp:3645-3659`), with an
audit-sink event. The arena floor still refuses lower incarnations. Delegated bindings also need a
currently valid certificate. Auto-advance is Q4, which also carries the calibration question it
raises (17, m12).

## 4. Clock mapping (I3-3b)

### 4.1 Device times are signed; the mapping is the host's

All times below are in **seconds**: the adapter converts the signed nanoseconds once, after the
128-bit checks. The device signs $a$ (`acquired`), $s$ (`sent`), the epoch, $u_d$
(`clock_uncertainty`) and $u_a$ (`acquired_uncertainty`). The host holds a mapping per (issuer,
incarnation, epoch) to the policy's basis (`vagent/runtime/epistemic_policy.hpp:19-24`):

```math
M(x) = x + \theta,\qquad u_M(x) = u_0 + 10^{-6}\rho\,|x - x_0|,\qquad U = u_a + u_d + u_M(a)
```

Here $\theta$ is the offset, $u_0$ its uncertainty at anchor $x_0$, and $\rho$ the device drift
bound in ppm (the binding's, else `vagent.epistemic_clock_drift_ppm`). `CheckedObservation` carries
the signed times and the mapping as required fields, and `observe` computes $M(a)$ and $U$ and
classifies, so the classification has one check site. Tests that call `observe` directly supply the
fields through the ingestion tests' helper.

### 4.2 Mapping kinds and pairing

- **synchronised** (`basis` is `synchronised`). For a wall policy on a synchronised host,
  $\theta = 0$ and $u_0$ is `vagent.epistemic_wall_sync_uncertainty_ms`. For an `actor_mapped`
  policy the host composes it with its own wall-to-steady offset, sampled when the version is
  minted. A step in that offset is exactly what B1 found the arena counting; after 4.3 it can only
  reach uncounted codes.
- **paired** (`free` basis, or a tighter bound wanted). The adapter draws a 32-byte nonce from the
  CSPRNG (on failure, `EntropyUnavailable`, and no pairing) at host time $t_1$, and hands it to the
  host's `ChallengeSink`. It accepts one `clock_pair` reply signed at device time $d$ = `sent` and
  received at $t_2$. Since the device signed after it received the nonce and before $t_2$,
  $\theta \in [t_1 - d,\ t_2 - d]$. The mapping takes the midpoint, with $u_0 = (t_2 - t_1)/2$ and
  $x_0 = d$. A relay can only widen $u_0$ by delaying; it cannot move the interval.

**Challenge rules (S-3).** Pending challenges are keyed by (issuer, incarnation). The reply's issuer
must match its key, and the nonce is compared in constant time. A challenge is consumed by its first
accepted reply. An unexpired challenge is **never replaced**: a new trigger within
`vagent.epistemic_clock_pair_timeout_ms` is ignored, so a relay replaying old messages cannot void
the device's challenge. Only a message verified above the host's floor for its issuer can trigger a
pairing (a replay below it is refused at step 4 first). A reply that is late, carries an unknown
nonce or has the wrong issuer refuses `ClockPairUnsolicited`. Triggers are rate-limited by
`vagent.epistemic_clock_pair_interval_ms`.

**Re-pairing.** The adapter re-pairs when $u_M$ at the current time reaches
`vagent.epistemic_clock_repair_fraction` of the smallest `max_uncertainty` among the issuer's
bindings. Each mapping is a new version, numbered by the host from 1 per (issuer, incarnation,
epoch) within one `clock_instance`. The newest version is used for every reading of that epoch.

**Bind-time check (m10).** A binding of a synchronised-wall predicate whose `max_uncertainty` is at
or below `wall_sync_uncertainty_ms` can never accept a reading, so the bind refuses
`InvalidBinding`. A source whose `UncertainClockMapping` refusals persist through one common-cause
window logs a WARN naming the binding.

**One-way paths.** A device on a one-way path cannot be paired. It must present a `synchronised`
clock and a wall-basis policy; the guide says so.

**Unknown mapping.** A `free` epoch with no mapping refuses `UnknownClockMapping` (Retryable) and
starts a pairing, subject to the challenge rules.

### 4.3 [ML] The split between device and path, and what is counted

**The rule.** A code may be in the counted list only if every site that emits it decides on fields
the origin signed and on nothing the host mapped. Section 2 explains why: the host counts by code
name (`agent_executor.cpp:1820-1824`). A host-decided condition therefore needs its own code name,
even where revision 1 thought the classification at the call site was enough.

With $A$ = `max_age`, $k$ = skew and $U_{\max}$ = `max_uncertainty` of the policy, receipt $r$:

| Order | Site                                   | Test                                                                                        | Code                                                       | Decided on  | Counted  |
| ----- | -------------------------------------- | ------------------------------------------------------------------------------------------- | ---------------------------------------------------------- | ----------- | -------- |
| 1     | `observe`                              | a negative $a$, $s$, $u_a$ or $u_d$ (signed integers only)                                  | `InvalidAcquisition`                                       | signed      | yes      |
| 2     | `observe`                              | $a > s + u_a$: acquired after it was sent                                                   | `ObservationTimeMismatch`                                  | signed      | yes      |
| 3     | `observe`                              | $u_a + u_d > U_{\max}$                                                                      | `UncertainAcquisition`                                     | signed      | yes      |
| 4     | `observe`                              | $s - a > A + u_a$: the device held it                                                       | `StaleObservation`                                         | signed      | **no**   |
| 5     | `observe`                              | no mapping for the epoch                                                                    | `UnknownClockMapping`                                      | host        | no       |
| 5a    | `observe`                              | $\theta$, $u_0$ or a mapped value non-finite or out of range; basis mismatch                | `InvalidClockMapping`                                      | host        | no       |
| 6     | `observe`                              | $U > U_{\max}$ only because of $u_M$                                                        | `UncertainClockMapping`                                    | host        | no       |
| 7     | `observe`                              | $M(s) > r + u_d + u_M(s) + k$: sent in the future                                           | `ClockMappingMismatch`                                     | host        | no       |
| 8     | `observe`                              | $r - M(a) > A + U$: the path made it old                                                    | `LateDelivery`                                             | path        | no       |
| 9     | `observe`                              | $M(\text{expires}) + u_M < r$                                                               | `MessageExpired`                                           | path        | no       |
| A1    | stream check, `compiler.cpp:4342-4344` | epoch below the stream's                                                                    | `ClockEpochRegression`                                     | signed      | yes      |
| A2    | support walk, `compiler.cpp:4541-4545` | epoch below the current record's                                                            | `ClockEpochRegression` (was `ObservationTimeMismatch`)     | signed      | yes      |
| A3    | support walk, `compiler.cpp:4549-4556` | mapped acquisition earlier than the current record's beyond both uncertainties and the skew | `AcquisitionOrderMismatch` (was `ObservationTimeMismatch`) | host        | no       |
| A4    | stream check, `compiler.cpp:4348-4350` | mapping version regressed within one epoch and one `clock_instance`                         | `ClockMappingRegression`                                   | host defect | no, WARN |
| A5    | dedup, `compiler.cpp:4301-4303`        | two digests under one event ID                                                              | `ConflictingEvent`                                         | signed      | no       |

The existing `observe` sites that rows 5a to 8 replace are deleted, not renamed:
`InvalidAcquisition` on mapped doubles (`agent_executor.cpp:3837-3839`) becomes row 5a; the
clock-basis `ObservationTimeMismatch` (`:3843-3844`) becomes row 5a, because with the adapter the
record's basis is the policy's by construction and a mismatch is a host defect; the receipt-based
future check (`:3856-3857`), `UncertainAcquisition` (`:3853`) and `StaleObservation` (`:3858`)
become rows 3, 4, 6, 7 and 8. After the change:

- `ObservationTimeMismatch` is emitted only by row 2;
- `InvalidAcquisition` only by row 1;
- `UncertainAcquisition` only by row 3;
- `ClockEpochRegression` only by A1 and A2.

Each of those inputs is a signed field.

**Why row 4 stays uncounted (ML M1, SEC S-4).** Signed bytes are the device's, but a long hold
between acquisition and signing is usually the path's. A battery sensor that buffers raw readings
through an uplink outage and signs when the link returns has $s - a$ as long as the outage. Counted,
a flush of 14 or more stale readings after any outage would read SUSPECT for about 60 readings
afterwards (ML review M1 table), and an attacker who cuts the link for longer than `max_age` could
cause it at will. The same flush from a device that signs at acquisition behind a buffering gateway
would be row 8, uncounted. Whether an outage counts against a device must not depend on that
firmware detail. Row 4 feeds the time-class diagnostic only. SEC's alternative, counting row 4 for
bindings that declare `sign_at_acquisition`, is not adopted in v1: the flag would be the operator's
claim about firmware, and the counted list would then depend on configuration (Q11).

**Why `ConflictingEvent` stays uncounted (m3).** The digest covers the sequence, so a device that
re-signs a retransmission with a new sequence produces a conflict. Today the retry guard hides that
only while the committed entry is still in the outcome log. Counting equivocation needs a
content-only digest, which changes a stored field's meaning and therefore the shape version. That is
not worth a shape bump in I3-3. The retransmission rule of 3.2 is stated, and the code stays a
diagnostic.

**The pinned sites.** `tools/check-counted-sites.py` (new, about 80 lines, run by `make lint`'s
existing Python checks and by the I3-3d close) lists every `refuse(EpistemicRefusal::X)` in
`agent_executor*.cpp` and every `endObservation(kTurnResultX)` in `compiler*.cpp` and
`epistemic_emit_*.cpp` for each counted `X`. It fails unless the set equals the table above. A test
drives each pinned site with a mapped-only fault and asserts that nothing counted.

`kHealthRefusalCodesVersion` becomes 2:

| Code                       | v1: direct, relayed | v2: direct, relayed | Change                          |
| -------------------------- | ------------------- | ------------------- | ------------------------------- |
| `ObservationFrameMismatch` | yes, yes            | yes, yes            | none                            |
| `MissingVariance`          | yes, yes            | yes, yes            | none                            |
| `InvalidVariance`          | yes, yes            | yes, yes            | none                            |
| `InvalidValue`             | yes, yes            | yes, yes            | none                            |
| `InvalidAcquisition`       | yes, no             | yes, yes            | now signed integers only        |
| `ClockEpochRegression`     | yes, no             | yes, yes            | signed epoch; gains the A2 site |
| `ObservationTimeMismatch`  | no, no              | yes, yes            | row 2 only                      |
| `UncertainAcquisition`     | no, no              | yes, yes            | row 3 only                      |

Every counted code is decided on signed bytes, so `countsWhenRelayed` is true for all of them, and
I3-2's relay attenuation disappears ($q = 0$). The claim is "no fabrication", not "no influence": a
relay can still withhold good readings, or delay them into row 8, with the recorded effect.

**`common_cause`, now true.** No host fault reaches a counted site. The test (b1) covers two kinds
of break:

- **Shifted.** Eight sources on an `actor_mapped` predicate, four `synchronised` with a host wall
  step and four `paired` with a re-pair whose $\theta$ is off by more than its $u_0$. Each takes
  readings across the change, so A3 fires.
- **Broken.** $\theta$ = NaN on eight more sources, so row 5a fires.

It asserts that `R`, `N`, `health` and `health_fraction` are bit-identical for all sixteen.

**[ML] What the split cannot see, and the time-class diagnostic.** A device that claims
`synchronised` with a wrong clock produces rows 7 and 8, which look like a host fault. The host
keeps a per-source **time-class fraction** with the estimator's shape, in host memory and at
half-life `vagent.epistemic_time_class_half_life` (default 32, the health half-life). It is fed by
rows 4, 7 and 8 only, each sequence at most once through the stream mark. It is not fed by rows 5,
5a and 6, which every reboot of a `free` device would trip (m2), nor by `StaleSequence`, which a
relay could fabricate (m2). It logs a WARN when one source's fraction passes
`vagent.epistemic_time_suspect_fraction` while fewer than `vagent.epistemic_common_cause_sources`
others do. It is a `SourceHealth` diagnostic, never a reason and never `f`: per the ML answer to
Q7(b), an evidence-origin reason from it is not adopted.

A second WARN watches counted rows 2 and 3 (m9). When at least `common_cause_sources` sources hit
them within one window, the host logs "shared device time source" (one NTP server, GPS jamming: spec
7's shared clocks). Those refusals stay counted, since each device really did sign an inconsistent
or over-uncertain time, and the Schema notes say so.

**[ML] Counted version 1 to 2 without a reset** (ML answer to Q7(a)). Version 2 is a strict superset
of version 1 in which codes count. A v1-accumulated `f` can therefore only understate the v2
fraction, and it errs toward trust for about 32 counted events. The one carried contamination, v1's
`InvalidAcquisition` on mapped values, decays at the same half-life. Condition (m7): the record's
`health_fraction` stamp changes meaning at the version change without a calibration epoch. I4-D2
must not learn across it (17).

### 4.4 [ML] The snapshot, and the lineage of segments

**Per record.** A record is immutable and is the mapping's snapshot: `acquired_at` and
`acquired_uncertainty` were computed once under `clock_mapping_version`, and no query maps again.
I3-3b1 adds three named fields after the positional ones: `acquired_device`, `sent_device` and
`device_uncertainty` (Int ns), at 6 cells a record.

**Segments (ML M4).** A steady-basis time is comparable with another only within a stretch over
which the steady clock tracked real time. Two events break that:

- an executor restart, which starts a new `clock_instance`;
- a jump in $w - s$, the wall-minus-steady offset, within one instance. A suspend causes one,
  because `CLOCK_MONOTONIC` stops while the wall clock does not; so does a wall step under
  synchronisation.

The agent keeps a **clock lineage**: a list on its `d.epistemic` rank of at most
`vagent.epistemic_clock_lineage` segments, each
`{instance, start_steady, anchor_steady, anchor_wall, anchor_sync}`. `anchor_wall` and `anchor_sync`
are absent while the wall clock is unsynchronised, never 0. A segment covers the steady times of its
instance from `start_steady` up to the next segment's start, so no record needs a segment field:
steady time is monotone within an instance.

On every commit taken while synchronised, the ingestion turn compares the new $w - s$ with the
current segment's:

- if they differ by at most $\sigma_{\text{old}} + \sigma_{\text{new}} + 10^{-6}\rho_s\,\Delta s$,
  it overwrites the anchor in place (two scalar writes);
- otherwise it **appends a new segment** starting at this commit's steady time.

The first control turn of a new instance appends that instance's first segment.

`rtClockRebase(instance, t)` finds the segment containing $t$ (instances compared by `textEq`) and
returns the offset and uncertainty that carry $t$ into the current segment:

```math
\Delta = (w_i - s_i) - (w_j - s_j),\qquad
u_\Delta = \sigma_i + \sigma_j + 10^{-6}\rho_s\,(|t - s_i| + |\mathrm{now}_j - s_j|)
```

It fails when either segment lacks a wall anchor or $t$'s segment has left the lineage. It applies
**within one instance too**: a reading taken before a suspend, read after it, is rebased by the
suspend's length rather than looking that much younger, which is spec 5.2's "does not extrapolate an
old position as if it were a fresh measurement". A record that does not rebase is ineligible for
every use that needs its age or alignment, label pairing included (17, m8).

Detection at commit alone is not enough for the mappings in use; section 4.5 adds detection on the
host at every `observe` and timer wake, and invalidates the live mappings when it fires.

**Residual.** Without a synchronised wall clock a suspend is invisible, because no wall anchor
exists to show the jump. `CLOCK_BOOTTIME`, which counts suspend, would close it for suspend (not for
wall steps). Changing `AgentClock::system()` also changes what every steady deadline and grant
lifetime means, so it is Q10, not a decision here.

### 4.5 [ML] A jump invalidates the live mappings (revision 2.1, N1)

Segments fix stored records. They do not fix the mappings that map the next reading. Both kinds of
mapping into an `actor_mapped` policy's steady basis carry the host's offset from before a jump: a
`synchronised` composition samples the wall-to-steady offset when its version is minted, and a
`paired` mapping's $\theta$ is host steady minus device time at the last pairing. After a suspend of
length $D$, `CLOCK_MONOTONIC` has stopped, so both place every new reading $D$ in the future. Above
$u_d + u_M + k$ each reading refuses row 7 (`ClockMappingMismatch`, uncounted); below it, readings
commit with acquisition times off by up to $D$, which their $U$ does not cover. Nothing re-pairs:
$u_M$ grows by drift only, `UnknownClockMapping` never fires because a mapping exists, and nothing
re-mints a composition. Worse, revision 2 detected the jump only on a synchronised commit, so an
agent whose predicates are all steady-basis commits nothing after the suspend and never sees it.

**Detection on the host.** The adapter keeps a host copy of the current segment's anchor
$(s_a, w_a, \sigma_a)$. At every `observe`, before the reading is mapped, and at every timer wake,
it reads $(s, w)$ and tests

```math
\bigl|(w - s) - (w_a - s_a)\bigr| > \sigma_a + \sigma + 10^{-6}\rho_s\,(s - s_a)
```

under the ingress mutex, so a reading is either mapped before the jump is recorded (its receipt is
then before the jump too) or after the invalidation below. A jump found this way is recorded in host
memory at once and queues a control item that appends the arena segment (4.4) in the next control
turn; the commit-time comparison of 4.4 stays as a second detector and finds the same segment. The
test needs a synchronised wall clock; without one the residual of 4.4 stands (Q10).

**Invalidation.** A detected jump invalidates every steady-basis mapping of the instance at once:

- **paired** mappings are dropped. The next message of each such issuer refuses
  `UnknownClockMapping` (Retryable) and triggers a pairing under the challenge rules of 4.2. The
  device resends after the pair completes.
- **synchronised** compositions are re-minted with the post-jump offset as a new version. The
  version rises within the instance, so no `ClockMappingRegression` arises, and readings flow on
  without a pause.

Wall-basis mappings are untouched: a wall reading never passed through the steady clock. Records
committed before the jump keep their mapped times and are rebased by segment (4.4). No record can
commit with a pre-jump mapping after the jump: the test runs before mapping in the same critical
section, and items already queued were mapped before the jump with the mapping that held then, for
readings received then.

**What liveness and display show while a re-pair is pending.** A message refused at step 6 is not
fresh (5.1), so it refreshes nothing. While a paired source waits:

- `SourceHealth` gains a host field `clockPending` (the time the mapping was dropped, absent
  otherwise), and the host logs one INFO per source per jump.
- The source stays LIVE until its heartbeat deadline. If the pairing has not completed by then, the
  timer turn sets `silent` with a new cause, `clock`, so I4's eligibility reads it as missing data,
  which it is. A completed pairing and the next fresh message clear it like any other silence.
- The displayed state (FAULTED, SUSPECT, DEGRADED, NOMINAL) does not change, and `R` and `N` do not
  move: the refusals are host class and the device did nothing wrong. Stream `stale` flags fire on
  their own `data_ms` as before.

A device that cannot be challenged (no return path) is never paired in the first place (4.2), so the
drop can only strand a source that was pairable a moment ago; its outage lasts one round trip plus
the challenge rules' interval.

**Reboot of the device.** A new `epoch` has no mapping until paired. Old-epoch records keep their
mapped times, and nothing is re-mapped.

**U-19.** No path leads from a mapping or the lineage to the latch. A test resumes a latched source
with no lineage anchor, pairs it, feeds it readings and asserts it is still latched.

## 5. Liveness: heartbeats, silence and freeze (I3-3c)

### 5.1 Expectations and what counts as heard

A binding may declare `heartbeat_ms` and, per predicate, `data_ms`, `freeze_window_ms` and
`freeze_min_change`. All are optional device facts. Every duration among them is refused below
`vagent.epistemic_heartbeat_floor_ms` with `InvalidBinding` (S-13b), so no field can ask for a timer
storm.

A message is **fresh** when it verified, passed step 4 and step 6, and its path lateness $r - M(s)$
is within `vagent.epistemic_max_path_lateness_ms` plus $\min(u_d + u_M(s),
\text{max\_path\_lateness})$. Clamping the uncertainty stops a device from widening its own
freshness window with a large `clock_uncertainty`, which heartbeats and reports have no row 3 to
refuse (S-8). Fresh messages refresh liveness whether direct or relayed. This replaces I3-1's
direct-only contact rule (`compiler.cpp:4430`) and defines liveness for relay-only sources.

### 5.2 The state machine

Per profile, from the smallest `heartbeat_ms` among the issuer's live bindings on the agent:

```text
            bind (first deadline = bind + heartbeat_ms)
  UNHEARD ──────────────────────────────────────────┐
     │ fresh message                                 │ deadline passes
     v                                               v
   LIVE ── deadline passes (timer turn) ──────────> SILENT
     ^                                               │
     └──────────── fresh message (turn) ─────────────┘

  FAULTED (latched): no timer is armed; flags stay as they were.
  Reset: clears silent, stale, frozen and both liveness stamps (I3-2 6.4); UNHEARD again.
```

Per stream, from `data_ms`, `stale` is set when no reading committed within `data_ms` and is cleared
by the next commit.

**Arena state.** The profile holds `silent`, `silent_since`, `silent_cause` (`heartbeat`, `rebase`
or `clock`) and `liveness_instance`. Each stream holds `received`, `stale` and `clock_instance`.
**Host state:** the last fresh time per issuer, the heartbeat and report floors, the recent-sequence
ring, and the deadline heap. Heartbeats write the arena only on the transition from SILENT to LIVE.

`silent`, `stale` and `frozen` never move `R` or `N`, never add a reason and never change the
displayed state. `rtHealth` returns them beside the state. A gap above
`vagent.epistemic_sequence_gap_note` between consecutive verified sequences **of the issuer** (not
of a stream, m4) increments a host diagnostic.

### 5.3 The timer wake

Deadlines are on the steady clock. The heap is **indexed**: one entry per key (pid, principal,
optional stream) with a generation counter, updated in place on re-arm and never pushed again
(S-13a). Its size is at most sources × (1 + streams per source) per agent. `earliestDeadline` gains
the head, converted to wall time when asked. `tick` collects every due entry and posts **one**
`liveness` control item per pid carrying all of them (S-13c). The turn sets each flag only if the
stamp it was armed from is still the newest, so a commit that raced the timer wins. Latched sources
arm nothing. The same heap carries the expiry of excitation-derived `frozen` (5.5).

### 5.4 Resume and upgrade

**Upgrade** keeps the host state and carries the arena flags unchanged.

**Resume.** The first control turn of the new instance appends its first lineage segment (4.4).
Then, for each profile, it rebases `last_receive`, `last_contact`, `silent_since` and each stream's
`received` through `rtClockRebase`, taking the conservative end ($t + \Delta - u_\Delta$). After the
host rebinds, the first deadline is the rebased stamp plus the expectation, never the bind time. If
the rebase fails, the profile is set `silent` with cause `rebase` at once: no grace period and no
automatic fault reset (U-18, 8.2-R3d). A shape-1 profile without `liveness_instance` takes the
`rebase` path, so the field is additive and the shape version stays 1.

### 5.5 [ML] Freeze

Spec 5.1 allows freeze detection from "expected excitation, independent motion/process constraints
or device telemetry". I3-3 provides two of the three.

**Telemetry.** A device report whose vocabulary row is marked `freeze` adds a device reason in the
ordinary way (6.1). `rtHealth` reports `frozen` while that reason is present. The device's own
`clear`, an operator clear or a reset removes it.

**Excitation (revised for ML M3).** `AgentExecutor::declareExcitation(pid, predicate, from, until)`
records in host memory that the quantity is being driven, and returns a declaration id. The streams
it excites are those of that predicate whose binding sets `freeze_window_ms` and
`freeze_min_change`. The decision is made per declaration, not per reading:

1. At `from + freeze_window_ms`, and every window after that until `until`, the host posts a
   `freeze_check` control item for the declaration. A window shorter than the process dead time
   would flag healthy sensors, so the binding's window must exceed the dead time; the guide says so.
1. The turn evaluates each excited stream that has at least `vagent.epistemic_freeze_min_readings`
   readings in the window. A stream is a **candidate** if every non-key Float component moved less
   than `freeze_min_change` from the window's first reading.
1. If at least two streams were evaluated and the candidates are at least
   `vagent.epistemic_freeze_majority` of them, **no stream is flagged**. The declaration is marked
   `unconfirmed` (a host diagnostic and a WARN), because the likelier cause is that the actuator did
   not act (I4-D1 7.5's common-cause guard, applied to excitation). Otherwise each candidate is set
   `frozen`, with `frozen_until` set to `until + freeze_window_ms`.
1. An excitation-derived `frozen` clears at `frozen_until` (the timer of 5.3), at a reading that
   moves by at least `freeze_min_change`, or at a reset.

`freeze_min_change` must exceed the sensor's least significant bit and its noise over the window,
otherwise quantisation fakes a moving signal or hides a frozen one (m11). The guide says so.
`freeze_min_readings` exists only so that a window holding a single reading decides nothing. With
one excited stream there is no majority to test; the stream can be flagged, and the flag still
expires with the declaration. That residual is stated. The excitation declaration is host state, so
a resume pauses detection, which errs toward not flagging. Independent constraints across sources
are I4-9's (17).

### 5.6 Pruning begins

When a new principal's first commit would exceed `max_sources`, the ingestion routine looks for a
profile that meets all of these:

- it is NOMINAL and not latched;
- it has no live binding (the host passes the bound set in `$turn.ingest`);
- it holds no pinned record;
- it is `silent` or has never been heard.

The least `last_receive` wins, with ties broken by principal text. That profile is retired in the
same turn through `rtSourceDrop_` with `retire`, which keeps its tombstone and its incarnation
floor. Without a candidate the refusal stays `EpistemicBudget`. Whether a rebound pruned principal
keeps its calibration ledger is I4-D2's to state; otherwise prune and rebind is a whitewash back to
the prior (17, m12).

## 6. Sensor health reports (I3-3d)

### 6.1 [ML] Semantics

A verified, timely `health_report` (3.8 steps 1 to 6, so a withheld report past the path bound is
refused `LateDelivery` or `MessageExpired`, S-2) becomes a control item `report`. It runs its own IR
entry, `deviceReportEntry`, never `healthEntry`, and holds no grant.

The actor-wide **vocabulary** lists each code with its reason kind (`degraded` or `suspect`), a
`freeze` mark and an `isolates` mark. A code outside the vocabulary refuses `InvalidHealthCode`.

| Action  | Effect                                                                                                                                                    |
| ------- | --------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `raise` | add a reason `{kind from the vocabulary, code, origin device}`, unique by (kind, code, origin); `NoChange` if present                                     |
| `clear` | the recovery percept: remove the `device`-origin reason with that code, never an operator or evidence one; `NoChange` if absent                           |
| `fail`  | if the vocabulary marks the code `isolates`, latch FAULTED at once through the shared isolate body (5.1-16); otherwise act as `raise` with kind `suspect` |

**Three bounds, by origin (ML M2).** Operator reasons stay at most `R - 1`
(`epistemic_emit_health.cpp:297`). Device reasons are at most `Rd`
(`vagent.epistemic_health_device_reasons`, default 1). **Evidence reasons are at most `Re`**
(`vagent.epistemic_health_evidence_reasons`), which neither operators nor devices can consume. The
list holds at most `R - 1 + Rd + Re` entries, and the health bound becomes

```math
14 + 16\,(R - 1 + R_d + R_e) + 20A
```

At `R = 3`, `Rd = 1`, `Re = 1` and `A = 4` that is 158 cells, 16 more than I3-2's 142. Revision 1's
claim of "unchanged at the defaults" is withdrawn. I3-3 writes no evidence reason; I4 writes three
(`gate_fraction`, `reference_suspect`, `gate_excluded`). `Re`'s default is therefore I4-D2's to set
(Q8). I3-3d ships 1, the slot I3-2 reserved, and the census is priced with it. A raise past `Rd`
refuses `HealthReasonsFull` and keeps the reasons already held, so a newer warning never silently
evicts an older unrecovered one.

**Replay and timeliness.** The profile keeps `report_floor`, the sequence of the newest committed
report, raised only by a commit. A report at or below it refuses `StaleSequence`. An operator
`Clear {code, origin: device}` also raises `report_floor` to the issuer's highest verified sequence
at the time of the clear, which the host passes with the action (S-2). A report signed before the
clear and delivered after it, even within the path bound, cannot re-raise the reason.

**Rate (S-11).** Report commits from one source are at least `vagent.epistemic_report_interval_ms`
apart. A report inside the interval refuses `ReportRateLimited` (Retryable; logged, not counted).
This bounds the audit-sink calls and the epistemic-state version bumps a hostile sensor can cause
(17).

**Audit.** Each committed report appends an `actions` entry of kind `device_raise`, `device_clear`
or `device_isolate`, with `issuer` the source principal and no `grant` field. Repeats count in place
(F28). A device entry evicts only device entries.

**Latched sources and the operator.** A latched source's reports still raise and clear device
reasons and can never unlatch it. `Clear` gains an optional `origin`: `Clear {code, origin: device}`
removes a device reason; `Clear {}` and `Clear {code}` keep A6's meaning.

### 6.2 What a report can never do

A report can never clear a latch, touch an operator or evidence reason, move `R`, `N` or the gate
state, reach `healthEntry`, act for another principal, renew itself by replay or by a delivery
withheld past the path bound or across an operator clear, isolate a code not configured to, or
commit faster than the interval. A relay can deliver a report and cannot invoke a health action.

## 7. [ML] What feeds the estimator, and what does not

| Input                                                             | `R`, `N`                              | Reasons, latch                   | Flags                        | Through                                |
| ----------------------------------------------------------------- | ------------------------------------- | -------------------------------- | ---------------------------- | -------------------------------------- |
| counted refusal (v2: the eight codes of 4.3, at the pinned sites) | counted once per stream sequence (A9) | none                             | none                         | the I3-2 estimator                     |
| commit or pass credit                                             | as I3-2; credit bound per 7.1         | none                             | clears `silent`, `stale`     | unchanged                              |
| rows 4, 7, 8                                                      | none                                  | none                             | none                         | host time-class fraction (WARN only)   |
| rows 5, 5a, 6, 9; A3, A4, A5; `StaleSequence`                     | none                                  | none                             | none                         | lifetime counters and diagnostics only |
| heartbeat                                                         | none                                  | none                             | clears `silent`              | host; a turn only on the transition    |
| silence, staleness                                                | none                                  | none                             | sets `silent`, `stale`       | timer turn                             |
| excitation freeze                                                 | none                                  | none                             | sets `frozen` (expiring)     | `freeze_check` turn                    |
| device report                                                     | none                                  | device reasons; `fail` may latch | `frozen` via a `freeze` code | report turn                            |

### 7.1 The pass-credit bound (m4)

A9 credits a reading that passed every check but was dropped under load, up to
`mark + health_sequence_gap` (default 1,024), because a sequence was any Int64 a device chose.
Sequences are now signed and verified. The credit is therefore bounded by the **issuer's highest
verified sequence** for the incarnation (host memory), which is sound for any stride. A device with
a reading every 3 hours and heartbeats every 10 seconds has a per-stream stride of about 1,080, and
the old bound would have silently stopped crediting it. `health_sequence_gap` remains only as the
bound for copies that do not come through the adapter (direct `observe` calls in tests).

Consequences for I4 are obligations in section 17. A device can only lower its own weight through a
device reason, since it already controls its own variance claim. `silent`, `stale` and `frozen`
affect eligibility, not weight.

## 8. Checkpoint, resume and upgrade

Every structure below is registered with a `populate` hook, translated in
`epistemic_translate_ingress.cpp` (a2) or `epistemic_translate_clock.cpp` (b1 to c2), and covered by
a populated-state test through both copiers that asserts the shape version and the bit equality of
doubles.

| Structure (id)               | Where                     | Fields                                                                    | Resume                                 | Upgrade | Slice |
| ---------------------------- | ------------------------- | ------------------------------------------------------------------------- | -------------------------------------- | ------- | ----- |
| `agent.audience`             | agent rank record         | `token`                                                                   | Carried                                | Carried | a2    |
| `record.signed_time`         | record field (named)      | `acquired_device`, `sent_device`, `device_uncertainty`                    | Carried                                | Carried | b1    |
| `stream.clock`               | profile field (`streams`) | per stream `clock_instance`                                               | Carried                                | Carried | b1    |
| `agent.clock_lineage`        | agent rank list           | `instance`, `start_steady`, `anchor_steady`, `anchor_wall`, `anchor_sync` | Carried; a segment appended            | Carried | b2    |
| `profile.liveness`           | profile field             | `last_receive`, `last_contact`, `liveness_instance`                       | Carried, then rebased or marked silent | Carried | c1    |
| `profile.silence`            | profile field             | `silent`, `silent_since`, `silent_cause`                                  | Carried, rebased                       | Carried | c1    |
| `stream.liveness`            | profile field (`streams`) | per stream `received`, `stale`                                            | Carried, rebased                       | Carried | c1    |
| `stream.freeze`              | profile field (`streams`) | `frozen`, `frozen_until`                                                  | Carried, `frozen_until` rebased        | Carried | c2    |
| `profile.report_floor`       | profile field             | `report_floor`                                                            | Carried                                | Carried | d     |
| `profile.reasons` (existing) | profile list              | `origin` now `device` too; three bounds                                   | Carried                                | Carried | d     |

Revision 2 drops revision 1's per-stream freeze anchor list: the `freeze_check` turn reads the
window's readings from the stream's history, which is already retained. All structures are additive
with a documented absent value, so **the shape version stays 1**. The rename of the A2 and A3 codes
changes refusal names, not a stored field. The translator coverage script runs at the end of I3-3d.

New host-state rows:

| Host state                                                                                       | Upgrade | Resume                                                                               |
| ------------------------------------------------------------------------------------------------ | ------- | ------------------------------------------------------------------------------------ |
| binding table, vocabulary                                                                        | kept    | lost; the host reloads its configuration and rebinds                                 |
| audience mirror                                                                                  | kept    | cleared; re-read from the arena                                                      |
| delegation cache, wall high-water mark                                                           | kept    | lost; the next message re-validates; the mark restarts (guide: trusted time source)  |
| revocation registry                                                                              | kept    | reloaded from the durable list; fail-closed until `ready()`                          |
| mappings, pending challenges                                                                     | kept    | lost; `UnknownClockMapping` until re-paired; versions restart under the new instance |
| last fresh time, heartbeat floor, recent-sequence ring, highest verified sequence, deadline heap | kept    | lost; deadlines from the rebased arena stamps (5.4)                                  |
| per-peer budgets                                                                                 | kept    | lost                                                                                 |
| excitation declarations                                                                          | kept    | lost; detection pauses                                                               |
| time-class fractions, diagnostics, common-cause windows                                          | kept    | lost                                                                                 |

None of these losses can admit a replay. Floors, dedup and `report_floor` are in the arena;
heartbeats are bounded by `expires` and the path bound; a lost mapping or a not-ready registry only
refuses.

## 9. Configuration

### 9.1 Settings (`system://settings`, `defaultSettingSpecs()`)

| Setting (`vagent.epistemic_…`) | Default | Range          | Slice | Why this default                                                           |
| ------------------------------ | ------- | -------------- | ----- | -------------------------------------------------------------------------- |
| `signed_message_bytes`         | 4096    | 1024 to 65536  | a1    | an observation is under 1 KiB; the ceiling is `kMaxObservationSignedBytes` |
| `delegation_cache`             | 256     | 16 to 4096     | a1    | two sources per slot of the default `max_sources`                          |
| `verify_per_peer`              | 2000    | 10 to 100000   | a1    | a few percent of one core at about 60 µs a verify                          |
| `wall_rollback_ms`             | 1000    | 10 to 3600000  | a1    | larger than an NTP slew step; smaller than any certificate window          |
| `incarnation_step`             | 1024    | 1 to 2^31      | a2    | a thousand unobserved reboots                                              |
| `clock_drift_ppm`              | 100     | 0 to 10000     | b1    | twice a common crystal's ±50 ppm                                           |
| `clock_repair_fraction`        | 0.5     | 0.05 to 0.95   | b1    | re-pair at half the budget                                                 |
| `clock_pair_interval_ms`       | 1000    | 100 to 3600000 | b1    | at most one trigger a second per issuer                                    |
| `clock_pair_timeout_ms`        | 2000    | 10 to 60000    | b1    | a later reply gives $u_0$ above 1 s                                        |
| `wall_sync_uncertainty_ms`     | 100     | 0 to 60000     | b1    | NTP over the internet                                                      |
| `max_path_lateness_ms`         | 5000    | 10 to 600000   | b1    | a congested gateway; also the replay and withholding residual              |
| `time_class_half_life`         | 32      | 8 to 4096      | b1    | the health half-life (`epistemic_limits.hpp:55`), so both read alike       |
| `time_suspect_fraction`        | 0.25    | 0.01 to 1      | b1    | the I3-2 suspect fraction                                                  |
| `common_cause_sources`         | 3       | 1 to 128       | b1    | three agreeing sources point to a shared cause                             |
| `common_cause_window_ms`       | 10000   | 100 to 3600000 | b1    | several heartbeats at typical rates                                        |
| `clock_lineage`                | 8       | 2 to 64        | b2    | segments, not instances: restarts and suspends together                    |
| `steady_drift_ppm`             | 200     | 0 to 10000     | b2    | a host's monotonic clock is worse than a crystal                           |
| `heartbeat_floor_ms`           | 100     | 10 to 60000    | c1    | the floor for every binding duration                                       |
| `sequence_gap_note`            | 64      | 1 to 2^31      | c1    | per issuer; a gap above it is unusual for one device                       |
| `freeze_min_readings`          | 8       | 2 to 4096      | c2    | enough that one reading cannot decide a window                             |
| `freeze_majority`              | 0.5     | 0.1 to 1       | c2    | a majority freezing together points to the declaration, not the sensors    |
| `health_device_reasons`        | 1       | 1 to 8         | d     | one device warning at a time by default; the setting raises it             |
| `health_evidence_reasons`      | 1       | 1 to 8         | d     | provisional, the slot I3-2 reserved; I4-D2 sets the default (Q8)           |
| `report_interval_ms`           | 1000    | 0 to 3600000   | d     | a real device's status changes far more slowly                             |

### 9.2 The binding table and the report vocabulary (W1's carrier)

```text
binding    = {alias, principal, trust: pinned | delegated, master?,
              incarnation: fixed | signed, lifetime_ms, heartbeat_ms?, drift_ppm?,
              predicates: [{name, data_ms?, freeze_window_ms?, freeze_min_change?}]}
vocabulary = {code, kind: degraded | suspect, freeze, isolates}
```

Neither is a setting. A setting is a fixed-arity typed tuple (`system_docs.hpp:802-809`), and a
table of rows would need one schema alternative per length. The recommended carrier is a record
group in `system://settings`, with schema and notes pages, read by a new
`SystemStoreModel::records(group)` (Q1). I3-3 takes `SourceTable` and `ReportVocabulary` as C++
values from its host, and W1 builds them from the approved carrier. No key material appears in
either.

## 10. Typed refusals

New `EpistemicRefusal` members (`epistemic_ingress.hpp:71`), named by `epistemicRefusalName`,
classed by `outcomeClassOf` and turn results where marked "arena". "Logged" means written to the
outcome log and charged; none of these is counted.

| Code                       | Slice | Step  | Class           | Logged |
| -------------------------- | ----- | ----- | --------------- | ------ |
| `OversizeMessage`          | a1    | 2     | Terminal        | no     |
| `MalformedMessage`         | a1    | 2     | Terminal        | no     |
| `NonCanonicalMessage`      | a1    | 2     | Terminal        | no     |
| `UnsupportedKind`          | a1    | 2     | Terminal        | no     |
| `InvalidSignerKey`         | a1    | 3     | Terminal        | no     |
| `IssuerKeyMismatch`        | a1    | 3     | Terminal        | no     |
| `UnexpectedCertificate`    | a1    | 3     | Terminal        | no     |
| `DelegationMissing`        | a1    | 3     | Terminal        | no     |
| `RevocationUnavailable`    | a1    | 5     | Retryable       | no     |
| `BadSignature`             | a1    | 5     | Terminal        | no     |
| `DelegationInvalid`        | a1    | 5     | Terminal        | no     |
| `DelegationExpired`        | a1    | 5     | NeedsHostAction | no     |
| `KeyRevoked`               | a1    | 5     | NeedsHostAction | no     |
| `AudienceMismatch`         | a2    | 3     | Terminal        | no     |
| `AudienceUnavailable`      | a2    | 3     | Retryable       | no     |
| `PolicyMismatch`           | a2    | 7     | NeedsHostAction | yes    |
| `IncarnationStep`          | a2    | 7     | NeedsHostAction | yes    |
| `InvalidBinding`           | a2    | bind  | (return only)   | no     |
| `UnknownClockMapping`      | b1    | 6     | Retryable       | yes    |
| `InvalidClockMapping`      | b1    | 6     | NeedsHostAction | yes    |
| `UncertainClockMapping`    | b1    | 6     | Retryable       | yes    |
| `ClockMappingMismatch`     | b1    | 6     | Retryable       | yes    |
| `LateDelivery`             | b1    | 6     | Terminal        | yes    |
| `MessageExpired`           | b1    | 6     | Terminal        | yes    |
| `ClockPairUnsolicited`     | b1    | 7     | Terminal        | no     |
| `AcquisitionOrderMismatch` | b1    | arena | Terminal        | yes    |
| `ReportRateLimited`        | d     | 7     | Retryable       | yes    |

Existing codes reused at new steps: `IngressBackpressure` (step 1), `StaleSequence` and `Duplicate`
(step 4, unlogged), `ClockUnsynchronised` (step 5), `EntropyUnavailable` (bind, pairing),
`InvalidHealthCode` and `HealthReasonsFull` (reports). The resume and upgrade channel (P2-1) gains
no code.

## 11. Why C++, and why Vortex

C++, each for a reason the repository's rule names. Signature, key, certificate and revocation
checks are the authorization boundary. The codec, the budget and the unverified floor check sit in
front of that boundary. Mapping and pairing are host clock products. The time split is decided in
`observe` before the arena (F6's justification). The deadline heap is the host scheduler. Vortex IR
does everything that is state: the audience write, the lineage and its segment rule,
`rtClockRebase`, the flags, the `freeze_check` turn, pruning and `deviceReportEntry`. No `std:`
routine is added.

## 12. [ML] Price and measurements

| What                       | Estimate                                                      | Measured by                                        |
| -------------------------- | ------------------------------------------------------------- | -------------------------------------------------- |
| Ed25519 verify             | about 50 to 70 µs on one x86 core                             | a1: 10,000 messages; messages a second per core    |
| path validation, uncached  | about 100 to 300 µs                                           | a1: cache off and on                               |
| a replay refused at step 4 | under 2 µs                                                    | a1                                                 |
| record cells               | +6 a record                                                   | b1: census; `AFullEpistemicBudgetStillCheckpoints` |
| profile cells (I3-3 total) | +28 (ML m6 count: streams, liveness, silence, `report_floor`) | each slice's census                                |
| health bound               | +16 per `Re` slot (158 at the defaults)                       | d                                                  |
| agent cells                | 3 (audience) + about 12 per segment, about 99 at 8            | b2                                                 |
| ingestion cycles           | +about 80 (segment compare, stream fields)                    | worst case against the instruction grant           |
| `freeze_check` turn        | O(excited streams × readings in window)                       | c2: 128 streams                                    |
| timer and report turns     | about a degrade turn                                          | c1, d: 128 sources due in one wake, coalesced      |
| pairing bound on loopback  | $u_0$ under 1 ms                                              | b1                                                 |

**Census for I4-D2 and O7 (m6).** I3-2 R3-2 prices a source at $48 + 82 \cdot 16 + 142 = 1{,}502$
cells with 16 records. I3-3 adds $6 \cdot 16 + 28 = 124$, about 8.3%, and the evidence slot adds 16
more, which I3-3d ships (`Re = 1`). With everything that ships, a source costs 1,642 cells at 16
records and 586 at 4, so the 24,576-cell ceiling holds 14 sources at 16 records (16 before I3-3) and
41 at 4 records (47 before), the ML expert's round-2 figures. In label units, 124 cells is two or
three ledger entries per source at I4-D2's estimated 40 to 60 cells an entry. Each slice reports its
measured figure to I4-D2.

## 13. Slices

| Slice | Content                                     | Lines (r1 → r2) |
| ----- | ------------------------------------------- | --------------- |
| a1    | codec, verifier, durable revocation, budget | 1,100 → 1,300   |
| a2    | adapter, audience, relay, incarnations      | 1,000 → 1,000   |
| b1    | pairing, mappings, the split, pinned sites  | 1,400 → 1,500   |
| b2    | lineage of segments, jump invalidation      | 800 → 1,100     |
| c1    | heartbeats, silence, timer wake             | 1,300 → 1,300   |
| c2    | freeze by declaration, pruning              | 700 → 800       |
| d     | reports, three bounds, rate, clear floor    | 1,000 → 1,100   |
| total |                                             | 7,300 → 8,100   |

### 13.1 I3-3a1: the signed codec and the verifier (about 1,300 lines)

- **Content.** Envelope and body codec with closed key sets, the identifier class, the Int bound and
  the canonical check; encoders for tests and firmware; header and payload structs and the kind
  table; the domain constant and the prefix-free test; key checks; `verifyDelegation` directly, the
  wall high-water mark and the master-keyed cache; `DurableRevocationRegistry` with `ready()`; the
  per-peer budget; the unverified step-4 check.
- **Files.** New `vagent/runtime/epistemic_adapter_wire.{hpp,cpp}` and
  `vagent/runtime/epistemic_adapter_verify.cpp`. `identity/identity_engine.hpp` gets one line (the
  domain constant) and one method (`ready()` on `IRevocationRegistry`). New
  `tests/xuzz/vagent_epistemic_adapter_wire_test.cpp`. `tests/fuzz/fuzz_epistemic_ingress.cpp` gains
  a decode step.
- **Tests.** All of revision 1's. In addition:
  - revoke, restart the registry, and assert `RevocationUnavailable` until loaded, then
    `KeyRevoked`, for a pinned binding too;
  - a cache hit under master A does not satisfy a binding naming master B;
  - a wall clock rolled back past the high-water mark refuses delegated verification;
  - a zero `issuedTimestamp` delegation is still checked at receipt;
  - `c` on a pinned binding refuses;
  - an unknown key at each level refuses;
  - a newline, NUL or bidi control in each identifier field refuses;
  - an Int at $2^{62}$ refuses;
  - a non-canonical point encoding refuses;
  - a replayed valid message is refused at step 4 without a signature check (counted by a verify
    counter);
  - the per-peer budget refuses past its rate;
  - refusal subjects and log lines carry no audience, nonce, certificate or key.

### 13.2 I3-3a2: the adapter (about 1,000 lines)

- **Content.** As in revision 1, plus the CSPRNG mint with an `EntropyUnavailable` refusal and the
  four conditions of 3.10.
- **Files.** New `vagent/runtime/epistemic_adapter.{hpp,cpp}`. `agent_executor.{hpp,cpp}` (at most
  150 lines). `compiler.cpp` (at most 60 lines). Registry, new `epistemic_translate_ingress.cpp`,
  `system_docs.*`, new `tests/xuzz/vagent_epistemic_adapter_test.cpp`.
- **Tests.** As in revision 1. The forcing test uses all-digit, `\d+e\d+` and leading-zero
  identifiers. Further tests: a captured old higher-incarnation message delivered late is not an
  advance; an advance rebinds every predicate and emits one audit event.

### 13.3 I3-3b1: pairing, mappings and the time split (about 1,500 lines)

- **Content.** The mapping table and the challenge rules of 4.2; the bind-time check; the 4.3 split
  with the deleted and renamed sites (A2, A3 and 5a); counted v2; `tools/check-counted-sites.py`;
  the time-class fraction fed by rows 4, 7 and 8 with mark dedup; the shared-time-source WARN; the
  pass-credit bound of 7.1; step 6 for heartbeats and reports; record fields.
- **Files.** New `vagent/runtime/agent_executor_clock.cpp`. `epistemic_ingress.hpp`.
  `agent_executor.cpp` (at most 300 lines). `epistemic_health.hpp`. `compiler.cpp` (A2 and A3
  renames, stream scope, contact rule; at most 120 lines). New `epistemic_abi_clock.hpp`. New
  `epistemic_translate_clock.cpp`. Registry, `system_docs.*`, new `tools/check-counted-sites.py`,
  new `tests/xuzz/vagent_epistemic_clock_test.cpp`, and the ingestion tests' helper.
- **Tests.**
  - Each row of 4.3 at its boundary, with margins.
  - The `common_cause` test of 4.3: shifted $\theta$ on eight sources, NaN on eight; `R`, `N` and
    stamps bit-identical.
  - The pinned-sites script fails on a planted extra `ObservationTimeMismatch` site.
  - A device that signs at send and flushes 600 buffered readings after an outage moves no `R` or
    `N` (ML M1).
  - A relayed copy counts exactly the v2 codes.
  - A relay replaying an old message once a second cannot starve a `free` device's pairing (S-3a).
  - A second bound source echoing the victim's nonce under its own key is refused, and the victim's
    reply still pairs (S-3b).
  - A pairing bound contains the true offset in a simulated drifting clock.
  - The first pairing after a reboot does not move the time-class fraction.
  - A slow-predicate stream with a stride of 1,080 is still credited.
  - A wall-sync policy below `wall_sync_uncertainty_ms` refuses at bind.
- **Closes.** 3.2-7.

### 13.4 I3-3b2: the lineage of segments and jump invalidation (about 1,100 lines)

- **Content.** The lineage record, the segment rule, `rtClockRebase(instance, t)`, eviction; host
  jump detection at every `observe` and timer wake, the invalidation of 4.5, `clockPending` and the
  `clock` silence cause.
- **Files.** New `vagent/runtime/epistemic_emit_clock.cpp`. `epistemic_translate_clock.cpp`,
  registry, `system_docs.*`, `vagent_epistemic_clock_test.cpp`.
- **Tests.**
  - A rebase across a resume lands within its bound.
  - **A suspend between a record and the rebase**: simulated by stepping the wall clock and not the
    steady one. The record is rebased by the suspend's length and is not younger than it really is
    (ML M4).
  - **A suspend on an agent with only steady-basis predicates** (N1), with one `paired` and one
    `synchronised` source: the jump is detected at the first `observe` after it with no commit in
    between; the paired source refuses `UnknownClockMapping`, pairs once and resumes ingestion; the
    synchronised source's next reading commits under a new version; no record commits with a
    pre-suspend mapping; `R` and `N` are unchanged; a pairing that misses the heartbeat deadline
    sets `silent` with cause `clock`, and the next fresh message clears it.
  - A jump detected by a timer wake alone, with no message arriving, appends the segment.
  - A wall step under synchronisation starts a segment.
  - An unsynchronised anchor fails the rebase.
  - An evicted segment fails it.
  - No path re-maps a record.
  - U-19.
  - Populated resume and upgrade.
- **Closes.** U-17, U-19, 3.2-6, 3.2-8, 8.2-5d, 8.2-R3c.

### 13.5 I3-3c1: heartbeats, silence and the timer wake (about 1,300 lines)

- **Content.** As in revision 1, plus the indexed heap, coalesced `liveness` items, the floor on
  every binding duration, and the clamped freshness uncertainty.
- **Files.** As in revision 1.
- **Tests.** As in revision 1. In addition:
  - heap size stays at most sources × streams under 100,000 heartbeats;
  - 128 sources due in one wake produce one control item per pid;
  - `data_ms = 1` refuses at bind;
  - a heartbeat with a huge `clock_uncertainty` does not widen its freshness window;
  - a withheld heartbeat past the path bound does not revive a silent source.
- **Closes.** U-18, 3.2-4, 5.1-20, 5.1-22, 8.2-4f, 8.2-R3d.

### 13.6 I3-3c2: freeze by declaration, and pruning (about 800 lines)

- **Content.** `declareExcitation`, the `freeze_check` item and turn, majority, `frozen_until` and
  expiry, pruning.
- **Files.** `agent_executor_clock.cpp`, `epistemic_emit_health.cpp`, `compiler.cpp` (at most 100
  lines), registry, `vagent_epistemic_heartbeat_test.cpp`.
- **Tests** (replacing revision 1's million-reading test, m11):
  - **excitation declared, actuator failed**: four constant sources; none is frozen, and the
    declaration is `unconfirmed` (ML M3);
  - one stuck stream among four moving ones is flagged;
  - an excitation-derived `frozen` expires at `until + window` with no reset;
  - a window shorter than the dead time is a configuration the guide forbids, and the test documents
    its effect;
  - a telemetry `freeze` reason clears only by a device clear, an operator clear or a reset;
  - pruning picks the right profile, keeps the floor, and refuses when there is no candidate.
- **Closes.** 5.1-21; 2.2-6 in part.

### 13.7 I3-3d: sensor health reports (about 1,100 lines)

- **Content.** As in revision 1, with the three origin bounds, the rate limit and the clear floor.
- **Files.** As in revision 1, plus the evidence-bound setting.
- **Tests.** As in revision 1. In addition:
  - **a withheld `fail` delivered after `expires` does not isolate**;
  - **a withheld `raise` does not renew a cleared warning** (S-2), including one signed before an
    operator clear and delivered within the path bound;
  - a device at `Rd` cannot take an evidence slot, and neither can an operator;
  - a planted evidence reason fits when the device and operator bounds are full (ML M2);
  - a report inside the interval refuses `ReportRateLimited`;
  - the coverage and counted-sites scripts list nothing uncovered.
- **Closes.** 4-19, 5.1-10, 5.1-16, 7-4.

Order: a1, a2, b1, b2, c1, c2, d. b2 and c2 are independent. d needs a2, and b1 for step 6.

## 14. Refused alternatives

| Alternative                                                   | Why refused                                                                                         |
| ------------------------------------------------------------- | --------------------------------------------------------------------------------------------------- |
| Signature in `Envelope`, `ActorMessage` or the mesh wire      | Three wire formats for one consumer; the plan keeps the signature in the payload                    |
| The actor wire's encoding as the signed form                  | Shortest-decimal doubles on devices; it would freeze the actor wire                                 |
| One domain per kind                                           | The kind is already signed                                                                          |
| Principal = master identity                                   | Collapses an operator's sensors into one source                                                     |
| `trustDelegation`, `deviceKeyFor`, `DeviceDelegation::verify` | Window skipped or checked at issue time; one key per master (section 2, fact 1)                     |
| Revocation from the controller's cache alone (revision 1)     | Empty after a restart: fail-open (S-1)                                                              |
| Unsigned, host-mapped acquisition times                       | Keeps every time refusal ambiguous                                                                  |
| Classify by call site and keep shared code names (revision 1) | The host counts by name; a shared name leaks a host-decided site into `f` (B1)                      |
| Count row 4 (device hold)                                     | A healthy buffering device reads SUSPECT after any outage (M1, S-4)                                 |
| Count `ConflictingEvent` with today's digest                  | The digest covers the sequence, so a re-signed retransmission is a conflict (m3)                    |
| Evidence-origin reason from the time-class fraction           | Cannot tell a lying clock from a slow private link; fabricable; slot contested (ML answer to Q7(b)) |
| One-way mapping                                               | Rests on an operator's assertion the host cannot check                                              |
| A per-profile list of mapping parameters                      | No reader; about 18 cells a version                                                                 |
| Overwriting one anchor per instance (revision 1)              | Hides a suspend or a wall step; old readings look fresh (M4)                                        |
| Writing every heartbeat to the arena                          | A turn per heartbeat                                                                                |
| A heap with lazy deletion                                     | Grows by one entry per heartbeat (S-13)                                                             |
| Freeze decided per reading (revision 1)                       | No exit after the excitation; a failed actuator freezes every source (M3)                           |
| Freeze from repetition                                        | Spec 5.1 forbids it                                                                                 |
| Device reasons in the evidence slot (revision 1)              | A device could block I4's latch or reference guard (M2)                                             |
| Policy-declared report codes                                  | Forces a `vagent-policy-3` bump before I4-1a; A2 made health thresholds actor-wide                  |
| Newest device reason evicts the oldest                        | Silently drops an unrecovered warning                                                               |
| Operator-chosen audience strings                              | Uniqueness becomes the operator's burden                                                            |
| Binding table in a file named by a setting                    | Configuration outside the system xanadocs                                                           |
| `TransportPeer::Unauthenticated`                              | A sentinel; and it loses the per-relay share                                                        |
| Replacing a pending challenge on a new trigger (revision 1)   | Lets a relay starve pairing (S-3)                                                                   |

## 15. Proposed amendments (for decisions.md section 3)

| #   | Spec section | Amendment                                                                                                                                                                                                                                                                                                                                                                                        |
| --- | ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| A18 | 2.1, 3.2     | The origin signs its raw acquisition and send times, clock epoch and uncertainty claims, and the host maps them. A time refusal is estimator evidence only if every site that emits its code decides on signed fields alone, and the device's hold time is not evidence. Liveness is refreshed by fresh signed messages within a path-lateness bound, direct or relayed. (revised in revision 2) |
| A19 | 5.1          | The codes a sensor's report may raise, and those whose `fail` isolates at once, are actor-wide configuration, not policy fields (extends A2).                                                                                                                                                                                                                                                    |
| A20 | 5.1          | `Clear {code, origin: device}` lets an operator remove a device-origin reason, and raises that source's report floor; `Clear {}` and `Clear {code}` remove operator reasons only (refines A6).                                                                                                                                                                                                   |
| A21 | 5.1          | Reasons are bounded per origin: operator `R - 1`, device `Rd`, evidence `Re`. No origin may consume another's slots (refines A6's reserved slot). (new in revision 2)                                                                                                                                                                                                                            |

## 16. Open owner questions

Recommended answers in brackets; security's round-1 answers to Q2 to Q5 are folded in. Q7 is
answered by the ML expert and recorded, not open.

1. **Q1. The carrier.** May `SystemStoreModel` gain a reader for record groups, so the binding table
   and the vocabulary live in `system://settings`? [Yes; W1 implements it.]
1. **Q2. The audience identifier as V-D's stable agent identity.** \[Yes, as an identity only: it is
   not secret, never a capability or authenticator, minted from the CSPRNG with the bind refused
   when entropy fails, and shared by two resumes of one checkpoint under D2's precondition.\]
1. **Q3. Key rotation.** \[The device key is the principal in v1. A `deviceId` that survives
   rotation is signed by the master, so a master compromise could move a calibrated reputation to
   any key; keep it out of v1.\]
1. **Q4. Incarnation auto-advance.** \[Allow, per binding, default `fixed`, pinned and delegated
   alike, on the conditions of 3.10: full verification and timeliness, the step bound, an audited
   rebind of every predicate, and a valid certificate for delegated keys. It is open together with
   I4-D2's ruling on whether calibration carries across a principal's incarnations: if it does not,
   a device can bump its own incarnation back to the prior, the same whitewash m12 names for
   pruning.\]
1. **Q5. Revocation and committed readings.** \[Accept the residual, because S-1's durable,
   fail-closed registry is built. The first `KeyRevoked` for a bound issuer emits one audit-sink
   event. No automatic isolation (F14). No revocation time is invented for I4.\]
1. **Q6. Ratify A18 (as revised), A19, A20 and A21.**
1. **Q7 (answered by ML, round 1).** (a) Keep counted v2 without resetting `R` and `N`, with I4-D2's
   stamp obligation (17). (b) No evidence-origin reason from the time-class fraction. A later
   proposal is admissible only if it is fed by row 7 alone, deduplicated by the mark, with
   common-cause exclusion, its own evidence slot, and false-positive rates measured on reboot and
   private-slow-link traces.
1. **Q8 (new, ML M2). The evidence bound `Re`.** I4 writes three evidence-origin reasons, and
   whether they can coexist on one source decides `Re`. \[I4-D2 sets the default and prices it in
   its census; I3-3d ships 1 provisionally and the setting's range admits 8.\]
1. **Q9 (new, S-1). Where durable device revocations live.** The ledger holds master links; device
   revocations exist only as gossip into an in-memory cache. Options: (a) append signed
   `DeviceRevocationRecord`s to the identity ledger, an identity-layer change; (b) a revocation list
   in a store the host loads, verified record by record. \[(a), because revocation is identity state
   and every consumer of device keys needs it, not only vagent. Approval is needed because it
   changes the identity layer.\]
1. **Q10 (new, ML M4). The executor's steady clock.** `AgentClock::system()` uses `CLOCK_MONOTONIC`,
   which stops during suspend. \[Use `CLOCK_BOOTTIME` on Linux, which makes grant lifetimes and
   deadlines count suspended time, arguably the correct meaning. Segments stay for wall steps and
   for platforms without it.\]
1. **Q11 (new, S-4). Counting device hold time for declared sign-at-acquisition devices.** \[No in
   v1: the counted list would depend on an operator's claim about firmware.\]

## 17. Obligations on later slices

- **I4-1b.** Bump the epistemic-state version on every change of `silent`, `stale` and `frozen`, on
  every device reason added or removed, on a report-driven isolate and on a prune. **State whether a
  device-driven bump invalidates continuations and tokens of other predicates or other sources**
  (S-11); the report interval bounds its rate in the meantime.
- **I4-D2.**
  - The record's `health_fraction` stamp is never a learned feature across a counted-code version
    change. Either record the version per record or open a calibration epoch at the change (m7).
  - A record whose clock does not rebase is excluded from label pairing (m8).
  - State whether a rebound pruned principal (m12), or a principal at a new incarnation (Q4), keeps
    its calibration ledger.
  - Set `Re` (Q8).
  - Whether `frozen` readings train.
  - Price the census of section 12.
- **I4-6.** Call `rtClockRebase(instance, t)` for every steady-basis record, same instance included.
  A record that does not rebase is ineligible for age and alignment. Read the flags separately from
  gate state.
- **I4-7a.** Exclude non-rebasing records from label pairing (m8). Extend the prune predicate's
  "live reference".
- **I4-9.** Freeze from independent constraints across sources.
- **I4-8 and the 7.5 guard.** Write evidence reasons only within `Re`.
- **I3-4b1.** Extend the prune predicate with journal pins.
- **V-D and I3-4a.** Use the header unchanged; define the verification payload, `expires` and the
  low-watermark rules. Never use the audience as a capability (Q2).
- **W1.** Build `SourceTable` and `ReportVocabulary` from the approved carrier; mint the CLI
  endpoint principal; run relay marking through the CLI. The guide states:
  - the one-way-device rule;
  - pinned mode for hosts without wall synchronisation;
  - the trusted time-source assumption;
  - admission of transport peers;
  - `freeze_window_ms` above the dead time and `freeze_min_change` above the noise;
  - the revocation residual.

## Review dispositions (round 1)

Reports: [review-ml.md](i3-3-review/review-ml.md),
[review-security.md](i3-3-review/review-security.md). Every code claim was re-read at `cbe5f6c9`
with `git show HEAD:`. B1's two sites (`agent_executor.cpp:1820-1824`, `compiler.cpp:4549-4556`),
the digest covering the sequence (m3), the half-life default of 32 (m1) and S-7's `issuedTimestamp`
fallback all hold as reported.

### ML

| ID  | Severity | Resolution                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                | Section              |
| --- | -------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------- |
| B1  | BLOCKING | Accepted in full. (i) The support-walk epoch branch emits `ClockEpochRegression` (counted, signed) and the mapped-order branch emits `AcquisitionOrderMismatch` (uncounted). (ii) Row 1 tests signed integers only; mapped faults refuse `InvalidClockMapping`, uncounted, and the old `observe` sites are deleted. (iii) A rule that a code is counted only if every site emitting it is signed-only, pinned by `tools/check-counted-sites.py` and a per-site test. (iv) The `common_cause` test shifts $\theta$ on eight sources and sets NaN on eight. | 2, 4.3, 13.3         |
| M1  | MAJOR    | Row 4 uncounted; feeds the time-class diagnostic; 600-reading flush test. Same finding as S-4.                                                                                                                                                                                                                                                                                                                                                                                                                                                            | 4.3, 13.3            |
| M2  | MAJOR    | Separate per-origin bounds `R - 1`, `Rd` and `Re`; health bound $14 + 16(R-1+R_d+R_e) + 20A$ = 158 at the defaults; the "unchanged" claim withdrawn; `Re` default is Q8 for I4-D2; amendment A21.                                                                                                                                                                                                                                                                                                                                                         | 6.1, 9.1, 12, 15, 16 |
| M3  | MAJOR    | Freeze decided per declaration by a `freeze_check` turn; majority freezing flags the declaration, not the sources; excitation `frozen` expires at `until + window`; the failed-actuator test.                                                                                                                                                                                                                                                                                                                                                             | 5.5, 13.6            |
| M4  | MAJOR    | Lineage of segments: a jump in $w - s$ beyond its bound appends a segment; rebase by segment, within an instance too; suspend test; `CLOCK_BOOTTIME` put as Q10.                                                                                                                                                                                                                                                                                                                                                                                          | 4.4, 13.4, 16        |
| m1  | MINOR    | Half-life default 32 and the Q7(a) text corrected.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        | 4.3, 9.1             |
| m2  | MINOR    | Time-class fraction fed by rows 4, 7 and 8 only, mark-deduplicated; rows 5 to 6 and `StaleSequence` excluded, so a reboot or a relay cannot trip it.                                                                                                                                                                                                                                                                                                                                                                                                      | 4.3                  |
| m3  | MINOR    | Byte-identical retransmission stated as a device rule; `ConflictingEvent` stays uncounted until a content-only digest exists (a shape bump not worth taking in I3-3).                                                                                                                                                                                                                                                                                                                                                                                     | 3.2, 4.3             |
| m4  | MINOR    | Pass credit bounded by the issuer's highest verified sequence; gaps measured per issuer.                                                                                                                                                                                                                                                                                                                                                                                                                                                                  | 5.2, 7.1             |
| m5  | MINOR    | All time symbols in seconds after a 128-bit conversion.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   | 4.1, 4.3             |
| m6  | MINOR    | Census restated from I3-2 R3-2 (1,502 cells; 16 → 15 and 47 → 43 sources) and in label units.                                                                                                                                                                                                                                                                                                                                                                                                                                                             | 12                   |
| m7  | MINOR    | Obligation on I4-D2: the stamp is never learned across counted-code versions.                                                                                                                                                                                                                                                                                                                                                                                                                                                                             | 4.3, 17              |
| m8  | MINOR    | Non-rebasing records also excluded from label pairing (I4-D2, I4-7a).                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     | 4.4, 17              |
| m9  | MINOR    | A shared-device-time-source WARN on counted rows 2 and 3; the Schema notes say so.                                                                                                                                                                                                                                                                                                                                                                                                                                                                        | 4.3                  |
| m10 | MINOR    | Bind refuses `InvalidBinding` for a policy unable to accept any reading; a persistent `UncertainClockMapping` WARN.                                                                                                                                                                                                                                                                                                                                                                                                                                       | 4.2                  |
| m11 | MINOR    | `freeze_min_readings` rationale corrected; `freeze_min_change` must exceed the LSB and noise; the million-reading test replaced by the M3 test.                                                                                                                                                                                                                                                                                                                                                                                                           | 5.5, 9.1, 13.6       |
| m12 | MINOR    | Recorded as an I4-D2 obligation and tied to Q4's incarnation whitewash.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   | 5.6, 16, 17          |
| Q7  | answer   | Adopted as recorded: (a) v2 without a reset, with the m7 condition; (b) no evidence reason from the fraction.                                                                                                                                                                                                                                                                                                                                                                                                                                             | 4.3, 16              |

### ML round 2 (confirmation, revision 2.1)

B1 and M1 to M4: CLOSED by the ML expert (review-ml.md, "Round 2 confirmation").

| ID    | Severity | Resolution                                                                                                                                                                                                                                                                                                                                                                                                                                          | Section        |
| ----- | -------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------- |
| N1    | MAJOR    | Jump detected on the host at every `observe` (before mapping, under the ingress mutex) and every timer wake, not only at commit; a detected jump drops `paired` mappings (re-pair through `UnknownClockMapping`) and re-mints `synchronised` ones as a new version; `clockPending` and a `clock` silence cause after the heartbeat deadline; display and `R`, `N` unchanged; steady-only suspend test. Back to the ML expert on this finding alone. | 4.4, 4.5, 13.4 |
| M2-r2 | MINOR    | Capacity stated for what ships (`Re = 1`): 1,642 and 586 cells per source, 14 sources at 16 records and 41 at 4.                                                                                                                                                                                                                                                                                                                                    | 12             |

### Security

| ID    | Severity | Resolution                                                                                                                                                                                           | Section            |
| ----- | -------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------ |
| S-1   | MAJOR    | `DurableRevocationRegistry` over the ledger and a durable signed list (location Q9), fail-closed `RevocationUnavailable` until `ready()`, for every binding; host wall time passed; restart test.    | 3.5, 13.1, 16      |
| S-2   | MAJOR    | Step 6 runs rows 5 to 9 and the freshness bound on every kind except `clock_pair`; an operator clear raises `report_floor`; withheld `fail` and `raise` tests.                                       | 3.8, 6.1, 13.7     |
| S-3   | MAJOR    | Challenges keyed by (issuer, incarnation), never replaced within the timeout, triggered only above the floor, compared in constant time, consumed once, CSPRNG with refusal; two tests.              | 4.2, 13.3          |
| S-4   | MAJOR    | Same as ML M1: row 4 uncounted. The `sign_at_acquisition` option is recorded as Q11 and not adopted.                                                                                                 | 4.3, 16            |
| S-5   | MINOR    | Cache keyed by (master, key, certificate digest); a hit skips only path validation.                                                                                                                  | 3.5                |
| S-6   | MINOR    | Wall high-water mark with `wall_rollback_ms`; the trusted time-source assumption in the guide.                                                                                                       | 3.5, 17            |
| S-7   | MINOR    | `verifyDelegation(cert, master, now)` called directly with the subject-key comparison; fact 1 corrected.                                                                                             | 2, 3.5             |
| S-8   | MINOR    | Ints bounded at $2^{62}$ at decode; 128-bit sums; freshness uncertainty clamped.                                                                                                                     | 3.1, 4.1, 5.1      |
| S-9   | MINOR    | Identifier class 0x21 to 0x7E; closed key sets; `c` refused on pinned; refusal subjects limited to principal and code.                                                                               | 3.1                |
| S-10  | MINOR    | Step 4 refuses already-used headers before the signature; per-peer budget at step 1; the binding oracle accepted because bindings and the audience are not secret.                                   | 3.6, 3.8           |
| S-11  | MINOR    | `report_interval_ms`; the version-bump scope is an I4-1b obligation.                                                                                                                                 | 6.1, 17            |
| S-12  | MINOR    | Audience stated as a non-secret identifier, never a capability; CSPRNG mint with refusal; forcing test widened.                                                                                      | 3.6, 13.2          |
| S-13  | MINOR    | Indexed heap with generations and a stated bound; the floor on every binding duration; one coalesced item per pid per wake.                                                                          | 5.1, 5.3, 13.5     |
| —     | sound    | Non-canonical point encoding refused (hygiene); the single sequence space stated as a device rule over three host floors; transport-peer admission in the guide; WARN suppression by delay recorded. | 3.2, 3.5, 3.7, 3.9 |
| Q2-Q5 | answers  | Folded into the recommended answers.                                                                                                                                                                 | 16                 |

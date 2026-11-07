# I3-3 validation

Slices land in the order of [i3-3-design.md](i3-3-design.md) section 13. Each slice records what
changed, the measurements the design names for it, tests, deviations and limits.

## I3-3a1: the signed codec and the verifier

Source baseline: `c9654efe` on `design/vagent-epistemic-arbitration`. Authority: I3-3D revision 2.1,
sections 3.1 to 3.5, 3.8 steps 1 to 5, 10 (the a1 rows), 12 (the a1 rows) and 13.1, with the owner's
answers of 2026-11-06 ([decisions.md](decisions.md) section 0.2). Q9 approved the identity-layer
change: durable device revocations are signed records in the identity ledger. Paths are relative to
`apps/common/xanadu/` unless they start with `tests/` or `design/`.

### What changed

- **Domain and readiness** (`identity/identity_engine.hpp`). `kDomainVagentSigned`,
  `"xanadu-vagent-signed-v1\n"` (`:33`), and `IRevocationRegistry::ready()` (`:306`). Nothing else
  implemented the interface, so the method is pure virtual.
- **Typed non-canonical refusal** (`bencode.hpp`, `bencode.cpp:19-57`, `:208`). A leading zero, a
  negative zero and keys out of order or repeated now throw `bencode::NonCanonicalError`, a subclass
  of `std::runtime_error`. Existing callers that catch `runtime_error` behave as before.
- **Device revocations in the ledger** (`merkle_ledger.hpp:136-156`, `merkle_ledger.cpp:90`,
  `:297-324`, `:440-520`). `appendDeviceRevocation`, `deviceRevocations`, `revocationLeafHash` and
  `leafCount`. Revocations share the key links' Merkle tree under a tagged leaf (first line
  `kind:device_revocation`), so one root covers both. A key link's `sequence` is now its leaf index,
  and `generateProof` takes a leaf index. The two are equal while a ledger holds no revocations, so
  every existing caller is unchanged. The copy constructor replays the leaves in order. The TSV goes
  to version 2 with `revocation` blocks. `fromTsv` refuses any other version by number and refuses
  an unreadable revocation field, rather than returning a ledger that might be missing one.
- **Refusals** (`vagent/runtime/epistemic_ingress.hpp:117-153`). These are the thirteen a1 members
  of the section 10 table, plus `StaleSequence` and `Duplicate` for step 4. The two step-4 names are
  the existing turn-result strings (`agent_executor.cpp:4083-4112`). `outcomeClassOf` classes
  `RevocationUnavailable` Retryable and `DelegationExpired` and `KeyRevoked` NeedsHostAction
  (`agent_executor.cpp:3561-3576`). The rest are Terminal by default, as the table says.
- **Codec** (new `vagent/runtime/epistemic_adapter_wire.{hpp,cpp}`, 773 lines).
  - Types: `SignedHeader`, the four payloads, `SignedEnvelope` and `SignedMessage`.
  - `decodeSignedMessage` (`:398`). Envelope `{b, c?, k, s}`, body `{h, p}`, closed key sets through
    `Fields` (`:100`), the common header and the kind table (`decodeHeader`, `:191`), and the
    payloads (`:240`). It also runs the re-encode belt and never throws.
  - Int fields are bounded at $|v| < 2^{62}$. A Float is 8 bytes, big-endian, with `-0`, NaN and
    ±inf refused (`signedFloat`, `:495`). Identifiers are 0x21 to 0x7E. Tuple text is strict UTF-8
    (`isSignedText`, `:523`), applied when a2 types the tuple.
  - Encoders shared by tests and firmware: `encodeSignedBody` (`:430`), `encodeSignedEnvelope`,
    `signEnvelope` (`:473`) and `issuerOf`.
- **Verifier** (new `vagent/runtime/epistemic_adapter_verify.{hpp,cpp}`, 866 lines).
  - `SignedIngressLimits` holds the four a1 settings of 9.1 plus the replay ring. It validates them
    over the design's ranges (`:89`).
  - `isAcceptableSignerKey` (`:106`) refuses $y \ge p$ and small-order points.
  - `LedgerMasterKeys` (`:121`) reads the ledger's newest link under the 64-hex name. The link must
    be active, and its key must hash to the name, compared as text.
  - `DurableRevocationRegistry` (`load` `:145`, `admit` `:165`):
    - each ledger record is verified against its master's key before it counts (`verifyAgainst`,
      `:54`), and a record that fails is ignored and counted;
    - `admit` is the live feed: it verifies, appends to the ledger, persists and takes effect;
    - every answer is "revoked" until `load()` has run.
  - `SignedVerifier`:
    - step 1, a token bucket per transport peer (`admit`, `:357`), with the table bounded by
      `kMaxPeerBudgets`;
    - step 2, `decode`;
    - step 3, the key, then the binding lookup, then the trust shape;
    - step 4, `checkUnseen` (`:424`): host floors for heartbeats and reports, and a ring of
      observation sequences per issuer;
    - step 5, `checkSignature` (`:456`): readiness first for every binding, then the signature, then
      for a delegated binding the synchronised wall, the rollback mark, the master lookup, the cache
      keyed by (master, key, certificate digest), `verifyDelegation(cert, master, now)` called
      directly, the subject-key comparison, and the serial and master revocation (`checkDelegation`,
      `:281`), and last the device revocation;
    - `verify` (`:478`) runs steps 1 to 5 in order and logs only the principal token and the code
      name, at trace level, under `vagent.ingress`;
    - `noteSettled` (`:506`) and `forget` maintain step 4's memory, and `onFirstRevocation` calls
      once per issuer at its first `KeyRevoked`.
- **Fuzzer** (`tests/fuzz/fuzz_epistemic_ingress.cpp:116`, step 17). The decode step gives the codec
  either raw bytes or a well-formed signed message with one byte changed. It aborts unless decode
  refuses with one of the four shape codes, or yields a message whose body is the canonical encoding
  of its value and whose re-encoded envelope decodes to the same message.
- **Tests.** New `tests/xuzz/vagent_epistemic_adapter_wire_test.cpp`, 32 cases. It lands in both
  `vagent_actor_test` and `xuzz_test` through the `tests/xuzz` glob.

### Measurements (section 12, a1 rows)

From `SignedIngressMeasure.VerifyDecodeAndReplayCosts`: 10,000 observations of 635 bytes, and 1,000
delegated heartbeats. It ran on a 32-core x86 host at load average 10 to 13, `-O3`, OpenSSL 3. The
figures from the two binaries agreed within 3%.

| What                                | Design estimate     | Measured                                                        |
| ----------------------------------- | ------------------- | --------------------------------------------------------------- |
| decode, step 2                      | (not estimated)     | 8.8 µs: envelope 0.5, body bencode 4.2, canonical re-encode 2.0 |
| bare Ed25519 `verifyWithDomain`     | 50 to 70 µs         | 117 µs                                                          |
| pinned verify, steps 1 to 5         | (the verify)        | 133 µs, about 7,500 messages a second per core                  |
| delegated, path validation uncached | 100 to 300 µs extra | 498 µs, about 365 µs above the pinned figure                    |
| delegated, cached                   | (a hash lookup)     | 132 µs, the same as pinned: the hit costs nothing measurable    |
| replay refused at step 4            | under 2 µs          | 10.3 µs, no signature check (asserted by the counter)           |

Readings:

- **Ed25519 is about twice the estimate.** `StandardCryptoEngine::verify` builds an `EVP_PKEY` and
  an `EVP_MD_CTX` on every call (`identity/standard_crypto_engine.cpp:320-338`), and OpenSSL 3's
  provider lookups are a large part of the cost. A per-key `EVP_PKEY` cache would be an
  identity-layer change and is not made here.
- **Step 4 misses its estimate because of step 2.** A replay must be decoded before its header can
  be looked up. The lookup itself is a map find; the rest is the bencode tree and the belt
  re-encode. It is still 13 times cheaper than a verify. A zero-copy decoder could reach the
  estimate, but it would duplicate `xanadu::bencode`, so it is left for a measured need.
- **The `verify_per_peer` default is priced on the estimate.** At the default 2,000 a second and 133
  µs a verify, one peer can cost about 27% of one core, not "a few percent". The default is the
  design's, and no setting ships in a1 (deviation 1). The default is put to I3-3a2 to re-price, at
  500 for example.

### Tests

The traceability column is `i3-3-design.md` section 13.1.

| Design test (13.1, and revision 1's a1 tests by subject)                              | Test                                                                                                                                                                      |
| ------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| revoke, restart the registry: `RevocationUnavailable` until loaded, then `KeyRevoked` | `RevokedAcrossARestartIsRefusedUntilLoadedThenRevoked` (pinned and delegated, through a TSV round trip)                                                                   |
| a cache hit under master A does not satisfy master B                                  | `ACacheHitUnderMasterADoesNotSatisfyMasterB`                                                                                                                              |
| wall clock rolled back past the high-water mark                                       | `AWallClockRolledBackPastTheMarkRefusesDelegation` (and an unsynchronised host; pinned needs neither)                                                                     |
| a zero `issuedTimestamp` delegation is still checked at receipt                       | `ADelegationIsCheckedAtReceiptWhateverItsIssueTime` (deviation 6)                                                                                                         |
| `c` on a pinned binding refuses                                                       | `ABindingDecidesTheCertificateAndAMissingOneRefuses` (and `DelegationMissing`, `UnknownSource`)                                                                           |
| an unknown key at each level refuses                                                  | `AnUnknownKeyAtEveryLevelRefuses` (envelope, body, header, every kind's payload)                                                                                          |
| newline, NUL or bidi in each identifier                                               | `ANewlineNulBidiOrSpaceInAnyIdentifierRefuses` (also space, empty, non-ASCII, bounds, the three hex fields)                                                               |
| an Int at $2^{62}$ refuses                                                            | `AnIntAtTwoToTheSixtySecondRefusesAndOneBelowPasses` (header, payload, tuple; negative times pass)                                                                        |
| a non-canonical point encoding refuses                                                | `ASmallOrderOrNonCanonicalKeyRefuses` ($y = p$, $p + 1$, sign bit, small order)                                                                                           |
| a replayed valid message refused at step 4 without a signature check                  | `AReplayedSettledMessageIsRefusedBeforeItsSignature`, `TheRingIsBoundedByTheReplayRingSetting`                                                                            |
| the per-peer budget refuses past its rate                                             | `ThePerPeerBudgetRefusesPastItsRate` (refill, separate peers, before decode)                                                                                              |
| refusal subjects and log lines carry no audience, nonce, certificate or key           | `RefusalLogsCarryOnlyThePrincipalAndTheCode`                                                                                                                              |
| forged signature; a signature from another domain                                     | `AForgedOrOtherDomainSignatureRefuses`                                                                                                                                    |
| non-canonical but equal inputs                                                        | `ASecondSpellingOfTheBodyIsNonCanonical` (leading zero, key order, repeated key)                                                                                          |
| a delegated key beyond its certificate                                                | `ADelegationIsCheckedAtReceiptWhateverItsIssueTime` (expired and not yet valid)                                                                                           |
| canonical bytes, kind table, domains                                                  | `EveryKindRoundTrips…`, `AFloatIs…`, `TheKindTable…`, `TheEnvelopeIsClosedAndSized`, `EveryDomainIsPrefixFree…`                                                           |
| delegation and revocation paths                                                       | `ADelegatedKeyVerifiesThenHitsTheCache`, `ACertificateForAnotherKeyOrMasterRefuses`, `ARevokedSerialOrMaster…`, `AnUnverifiableRevocationRecord…`, `TheLiveFeedPersists…` |
| ledger shape                                                                          | `SignedLedgerTest.DeviceRevocationsShareTheTreeAndRoundTrip` (proof, copy, version 1 refused)                                                                             |

The remaining revision 1 a1 subjects are a2's: an alias bound to two principals, a relay claiming
another origin, a principal look-alike string, and a replay to another actor or predicate. They need
the adapter's binding table and audience.

All suites ran headless (`SDL_VIDEODRIVER=offscreen`, `SDL_AUDIODRIVER=dummy`,
`LIBGL_ALWAYS_SOFTWARE=1`, XDG under `build/xdg`) after `make -j$(nproc)` exited 0 with no warning
in changed code. Results are in [Suite results](#suite-results). Touched C++ is formatted with
clang-format 19.1.7.

### Deviations from the design

1. **The four a1 settings are not yet in `system://settings`.** Section 9.1 marks
   `signed_message_bytes`, `delegation_cache`, `verify_per_peer` and `wall_rollback_ms` as a1, but
   13.1's file list leaves out `system_docs.*`, which 13.2 lists for a2. a1 ships them as
   `SignedIngressLimits` with the design's defaults and ranges, validated. a2 adds the
   `defaultSettingSpecs()` entries and reads them. Until then nothing reads a setting nobody could
   set.
1. **`frame` and `units` are optional in the observation payload.** 3.3 lists them as required
   identifiers. Discrete policies declare neither, and `observe` compares them only for continuous
   predicates (`agent_executor.cpp:3862-3864`), so a required field would force a device to invent a
   value nobody checks. When present they are identifiers, never empty. When absent they read as
   empty text, which a continuous policy then refuses as `ObservationFrameMismatch`.
1. **Step 4 remembers settled headers, not verified ones.** 3.8 says "recently verified". A message
   verified and then refused Retryable downstream (`IngressBackpressure`, `UnknownClockMapping`,
   `ReportRateLimited`) must pass step 4 when its unchanged bytes are resent, or "resend unchanged"
   would end in `Duplicate` or `StaleSequence`. So `noteSettled` is the adapter's call once an
   outcome is final, and never after a Retryable refusal. The safety argument of 3.8 is unchanged:
   only a header a settled, verified message used is refused.
1. **A new header for the verifier.** 13.1 lists `epistemic_adapter_verify.cpp` only. Its
   declarations are in `epistemic_adapter_verify.hpp` rather than mixed into the wire header.
1. **A typed `bencode::NonCanonicalError`.** Without it `NonCanonicalMessage` could not be told
   apart from `MalformedMessage`, since the decoder throws on both. The change is in engine utility
   code, not the store, the VM or the stdlib, and existing callers see a `runtime_error` as before.
1. **Section 2 fact 1 (S-7) overstates the zero-issue-time hole.** With `issuedTimestamp == 0`,
   `DeviceDelegation::verify` skips the explicit window check. But
   `verifyDelegation(cert, master, 0)` then falls back to `std::time(nullptr)` for `X509_STORE_CTX`
   path validation (`identity/standard_crypto_engine.cpp:1089-1096`), so an expired certificate
   still refuses at HEAD. The real hole is a non-zero `issuedTimestamp` inside the old window: that
   judges the window at issue time and passes an expired certificate. The test pins both behaviours
   of the old road. The adapter's direct call, judged at receipt against the rollback-guarded host
   wall clock, closes it either way. The direct call is still right because `std::time` is not
   guarded by the mark.
1. **The controller's gossip is not yet wired to the registry.** 3.5 names the controller's
   quarantine cache as the registry's live feed. `DurableRevocationRegistry::admit` is that entry
   point, but `IdentityNetworkController` has no hook to call it, and 13.1's files do not include
   the controller. The host (a2 or W1) connects `handleIncomingDeviceRevocation` to `admit`. The
   controller today quarantines gossip records without checking their signature
   (`identity/identity_network_controller.cpp:575-583`, `:1113-1129`); `admit` verifies them.
1. **The first-`KeyRevoked` audit event is a hook.** `onFirstRevocation` fires once per issuer with
   the principal token; a2 installs the executor's audit sink there.
1. **"The mapped acquisition must not precede `notBefore`"** (3.5) needs the clock mapping, so it
   moves to I3-3b1.

### Limits carried forward

- **Revocation dates are not honoured.** A revocation is in effect from the moment it is known,
  whatever date it carries. The wall-clock timestamp the interface passes never un-revokes, so
  rolling the host clock back cannot reopen a key.
- **Serials are revoked for every master.** `IRevocationRegistry::isSerialRevoked` names no master,
  so a serial revoked by one master is revoked for all. That reading is conservative and errs toward
  refusal.
- **Any ledger master can revoke any key.** A device revocation signed by any master in the ledger
  counts for any device key, pinned keys included, which have no master relation. The worst a
  hostile master can do is deny service, never admit a key.
- **Threading is the host's.** `SignedVerifier` is not thread-safe and runs under the adapter's
  ingress mutex. The registry locks its own sets, but the ledger has no lock, so the host serialises
  ledger writers (`admit`, `appendKey`) with the adapter's reads.
- **Step 4's memory is lost at a restart.** The floors and ring are then empty, so replays fall
  through to the arena's floors and dedup after a full verify. That costs work, never admission.
- **The per-peer bucket's burst is one second of its rate.** With `kMaxPeerBudgets` (4,096) buckets
  that are not yet full, a new peer is refused `IngressBackpressure`.
- **Traceability.** a1 closes no rows. 2.1-45 is worked here and closes at W1. The user guide is
  unchanged, because the design assigns its I3-3 lines to W1 (section 17).

### Suite results

`make -j$(nproc)` exited 0 with no warning. Every suite ran headless with XDG under `build/xdg`.

| Suite                    | Result                                                                   |
| ------------------------ | ------------------------------------------------------------------------ |
| `vagent_actor_test`      | 1,101 passed                                                             |
| `xuzz_test`              | 3,277 passed                                                             |
| `gleditor_test`          | 830 passed                                                               |
| `zigzag_test`            | 256 passed                                                               |
| `fuzz_epistemic_ingress` | 436 runs in 120 s, seed 1, no crash, with `alloc_dealloc_mismatch=0` set |

The fuzzer setting works around a toolchain defect that predates this slice. Under clang 23 ASan
with GCC 16.2's libstdc++, `std::stable_sort`'s temporary buffer is allocated with
`operator new(nothrow)` and reported as freed with `free`. That happens in `Gen::mergedFor`
(`vagent/runtime/compiler.cpp:5595`), which this slice does not touch, while the fuzzer builds its
shared host before it reads any input. So every fuzzer that compiles an actor stops there. The
fuzzer reaches about 3 runs a second because each input drives the full executor.

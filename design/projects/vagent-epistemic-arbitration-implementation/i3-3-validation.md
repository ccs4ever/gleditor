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

## I3-3a2: the adapter

Source baseline: `ffb2c3e8` on `design/vagent-epistemic-arbitration`. Authority: I3-3D revision 2.1,
sections 3.6 to 3.10, 8 (the `agent.audience` row and the host-state table), 9.1 (the a1 and a2
rows), 10 (the a2 rows) and 13.2, and the plan's I3-3a entry
([remaining-plan.md](remaining-plan.md), "I3-3a. Signed-observation ingress adapter"). Section 13.2
says "as in revision 1"; revision 1 is not in the tree, because the design was committed once at
revision 2.1. The slice was therefore built from the sections above. Paths are relative to
`apps/common/xanadu/` unless they start with `tests/` or `design/`.

### What changed

- **The adapter** (new `vagent/runtime/epistemic_adapter.{hpp,cpp}`, 583 lines). `EpistemicAdapter`
  owns a `SignedVerifier` and the binding table, under one mutex.
  - `bind` (`:323`) refuses a row it cannot hold with `InvalidBinding`: a principal or master that
    is not 64 lowercase hex, no predicate or a repeated one, a lifetime that is not positive, or
    incarnation 0. It then binds each predicate through `bindSource`. If any bind is refused, it
    unbinds what it bound and returns that refusal.
  - `ingest` (`:375`) runs steps 1 to 5 with the adapter's lookup at step 3. `resolve` (`:157`) maps
    the audience to a pid through a mirror filled from `AgentExecutor::audience`. An audience no
    bound agent holds refuses `AudienceMismatch`. While some bound agent's identifier has not
    committed, the refusal is `AudienceUnavailable` instead, because that agent may be the one the
    device means.
  - Step 7 for observations (`observation`, `:232`):
    - the predicate must be in the row and live in the executor (`UnknownSource` otherwise);
    - the tuple is typed by the declaration (`typed`, `:39`), with an Int or Bool as an integer and
      a Float as its bit pattern;
    - text must be strict UTF-8, checked with `isSignedText`;
    - the relay comes from `TransportPeer` alone;
    - the payload's policy hash goes to `observe`.
  - Incarnations (3.10) under `signed`:
    - a step above `incarnation_step` hands `IncarnationStep` to `observe`, which logs it;
    - a timely message (`timely`, `:196`) within the step rebinds every predicate of the principal
      and emits one `IncarnationAdvanced` audit event (`advance`, `:212`);
    - an untimely one is passed through unchanged, and `observe` refuses `UnboundIncarnation`. A
      captured old message is therefore never an advance.
  - Step-4 memory (`settle`, `:182`). A queued observation's header waits in a bounded list
    (`kMaxPendingSettles`). It is noted once `ingestOutcome` reports an outcome that is final and
    not Retryable.
  - `onFirstRevocation` feeds a `KeyRevoked` audit event.
  - `AdapterLimits::fromModel` (`:85`) reads the five settings and the replay ring. A value out of
    range is warned about by name and keeps every default.
- **The executor** (133 lines across `agent_executor.{hpp,cpp}` and `agent_executor_internal.hpp`,
  within the 150 the design allows).
  - `bindSource` draws the audience before anything is bound (`agent_executor.cpp:3678`). The source
    is `mintAudienceToken()` (32 bytes of `RAND_bytes`, as 64 hex) or a mint the host injects. A
    failed draw refuses `EntropyUnavailable`. The token rides `$turn.source_change` as `audience`.
  - `finishSourceChange` reads the arena's record back into host memory (`:1900`). The arena is the
    authority, so after a resume the checkpoint's identifier replaces the new host's fresh mint.
  - `audience(pid)` (`:3743`) answers a committed identifier only, from any thread.
  - `boundPredicate` (`:3757`) and `hostTime` (`:3770`) give the adapter the bound policy, the
    parameter kinds and the clocks `observe` uses.
  - `observe` refuses `PolicyMismatch` on a policy hash that is not the binding's (`:3893`). It
    takes the adapter's `IncarnationStep` and `InvalidValue` at the places it would decide them
    itself (`:3886`), and anything else as `InvalidObservation`.
- **The routine** (`vagent/runtime/compiler.cpp:3900`, 23 lines). The source-change turn scans the
  agent's `d.epistemic` rank and appends an `EpistemicAudience` record with one `token` field, only
  if none exists. A racing second bind and a resumed checkpoint therefore keep the first identifier.
- **Translation**:
  - new `vagent/runtime/epistemic_translate_ingress.cpp`: `audienceOf` and `agentRankRecords`.
    Anything on the agent rank but the header and one well-formed audience refuses the transfer.
  - The plan carries the record, and the copier's pass 0 appends it after the new program's header
    (`epistemic_translate.cpp:665`).
  - The digest covers it, and `countOwnedCells` counts it.
  - `agent.audience` is registered Carried and Carried (`epistemic_translate_registry.cpp:281`),
    with six host-state rows from section 8.
  - `tools/check-translator-coverage.py` passes.
- **Settings**:
  - `defaultSettingSpecs()` gains `signed_message_bytes`, `delegation_cache`, `verify_per_peer`,
    `wall_rollback_ms` and `incarnation_step`, which closes a1's deviation 1.
  - Their constants move from the a1 headers to `vagent/runtime/epistemic_limits.hpp:104-131`, so
    `system_docs.cpp` names them without the adapter.
- **Refusals** (`vagent/runtime/epistemic_ingress.hpp:166-180`). `AudienceMismatch`,
  `AudienceUnavailable` (Retryable), `PolicyMismatch` and `IncarnationStep` (NeedsHostAction,
  logged), `InvalidBinding` (return only), and `UnknownClockMapping` (Retryable, deviation 1).
- **Tests.** New `tests/xuzz/vagent_epistemic_adapter_test.cpp`, 22 cases. Four existing tests
  change:
  - three now price the audience's 3 cells;
  - the empty-root upgrade test now expects the reconciler road to refuse (deviation 7).

### Tests

| Design test (13.2, the plan's I3-3a list, and a1's carried subjects)       | Test                                                                                         |
| -------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------- |
| the adapter feeds the existing ingestion end to end                        | `AVerifiedObservationIsCommittedUnderItsSource`                                              |
| an alias bound to two principals                                           | `AnAliasBoundToTwoPrincipalsRefuses`, `AFailedBindLeavesNoPredicateBound`                    |
| a relay claiming another origin                                            | `ARelayIsMarkedFromTheTransportOnly` (`relayed_by` is the peer; a relay's own key refuses)   |
| a string that merely looks like a principal                                | `ABindingTheAdapterCannotHoldRefuses` (uppercase, short, look-alike master, and four others) |
| an observation replayed to a different actor                               | `AReadingForOneActorNeverReachesAnother`, `AnUnknownAudienceIsAMismatch`                     |
| an observation replayed to a different predicate                           | `AnotherPredicatesPolicyRefusesPolicyMismatch` (and an unbound predicate)                    |
| the audience before it commits                                             | `TheAudienceIsUnavailableUntilTheBindCommits`                                                |
| the forcing test: all-digit, `\d+e\d+` and leading-zero identifiers        | `ANumberLikeAudienceSurvivesACheckpointAsText` (resumed, re-bound, a device still lands)     |
| the CSPRNG mint and `EntropyUnavailable`                                   | `AnEntropyFailureRefusesTheBind`                                                             |
| an advance rebinds every predicate and emits one audit event               | `ASignedAdvanceRebindsEveryPredicateOnce`                                                    |
| a captured old higher-incarnation message delivered late is not an advance | `ACapturedOldHigherIncarnationIsNotAnAdvance`                                                |
| the step bound                                                             | `AnAdvancePastTheStepRefusesIncarnationStep`, `AFixedBindingIsNotAdvancedByAMessage`         |
| the first `KeyRevoked` emits one audit event (Q5)                          | `TheFirstRevokedKeyIsAuditedOnce`                                                            |
| settled headers feed step 4; retryable ones do not                         | `ASettledReadingIsRefusedBeforeItsSignature`, `OnlyASynchronisedClockOnAWallPolicyMapsYet`   |
| the tuple typed by the declaration                                         | `TheTupleIsTypedByTheDeclaration`                                                            |
| registry and settings                                                      | `TheAudienceIsRegisteredAsCarriedState`, `TheFiveSettingsShipWithTheirDefaults`              |
| the audience carried by the schema road; refused by the reconciler road    | `ABoundRootWithNoProfileUpgradesOnlyBySchema`, `TheSavedHeaderIsComparedAndNotCopied`        |

### Deviations from the design

1. **Step 6 before I3-3b1.** Only a `synchronised` device on a `wall` policy maps, with
   $\theta = 0$. The mapped acquisition is `acquired`, the uncertainty is $u_a + u_d$, and the
   mapping version is 1. Everything else refuses `UnknownClockMapping`. The code is b1's, added
   early and classed Retryable, so a `free` device's resend passes once b1 pairs it. It is returned
   unlogged; b1's table logs it. Until b1, `observe`'s existing receipt checks are the time checks.
1. **Heartbeats, reports and clock pairs are verified, then refused `UnsupportedKind`.** Their
   dispatch is b1's, c1's and d's. Their headers are not noted, so nothing a later slice needs is
   consumed.
1. **The adapter has its own audit sink.** `IngressAuditSink` carries the kind, principal, pid and
   incarnations. `HealthAudit` is shaped for health actions (grant, issuer, action sequence), so the
   executor's sink would have needed fields that mean nothing here.
1. **One logging path for the adapter's step-7 refusals.** `CheckedObservation` gains `policy`,
   which `observe` checks against the binding (`PolicyMismatch`), and `adapterRefusal`, for
   `IncarnationStep` and `InvalidValue`. Both are logged and charged by `observe`'s own rules rather
   than by a second writer to the outcome log.
1. **The audience lives in host memory as well as in the arena.** `audience(pid)` must answer from
   the adapter's thread, and `readableState` is the pumping thread's. The host map is refreshed from
   the arena at every finished source change, so the arena stays the authority. The adapter's mirror
   maps audience to pid.
1. **`verify_per_peer` defaults to 500, not 2,000.** a1 measured 133 µs a verify, so 500 a second is
   about 7% of one core, where the design's 2,000 would be about 27%. The design's reasoning ("a few
   percent of one core") is kept and only the number changes. This is put to the owner.
1. **The reconciler road now refuses a bound agent with no profile.** The audience is carried state
   that the by-value road has no translator for, and dropping it would make the next resume mint a
   new identifier and lock out every configured device. This is the existing rule ("populated state
   cannot be silently erased") applied to the new record. The schema road carries it.
   `AnEmptyEpistemicRootStillUpgradesBothWays` becomes
   `ABoundRootWithNoProfileUpgradesOnlyBySchema`. An arbitrating agent that was never bound still
   upgrades on both roads.
1. **Settled means final, not queued.** A queued observation is noted only once its outcome is final
   and not Retryable, so a turn that refuses retryably leaves the bytes resendable. Past
   `kMaxPendingSettles`, a header is dropped unnoted, which costs a replay a full verify and nothing
   more.

### Measurements

- **Cells.** The audience record is 3 cells per agent (record, field name, token): section 12's "3
  (audience)". It is counted by `countOwnedCells` and therefore by the epistemic budget.
- **Line counts.** About 1,060 lines of source against the design's 1,000. The executor is at 133 of
  its 150 and `compiler.cpp` at 23 of its 60.

### Limits carried forward

- **The fuzzer has no adapter step yet.** Section 13.2's file list leaves it out, and the plan's
  I3-3a entry asks for one. The decode step from a1 covers the codec. An `ingest` step over a live
  executor is owed before the M2 review.
- **The identity controller's gossip is still not wired to `DurableRevocationRegistry::admit`**
  (a1's deviation 7). That is W1's.
- **`relayedBy` must be a principal token.** A host that mints listener principals (3.7) mints
  tokens. A peer whose principal is not one reaches `observe` as `InvalidObservation`.
- **Traceability.** a2 closes no rows. 2.1-45 closes at W1.

### Suite results

`make -j$(nproc)` exited 0 with no warning in project code. Every suite ran headless with XDG under
`build/xdg`, after touched C++ was formatted with clang-format 19.1.7.

| Suite                    | Result                                                                    |
| ------------------------ | ------------------------------------------------------------------------- |
| `vagent_actor_test`      | 1,123 passed                                                              |
| `xuzz_test`              | 3,298 passed, 1 failed (below); 3,299 passed on the run before formatting |
| `gleditor_test`          | 830 passed                                                                |
| `zigzag_test`            | 256 passed                                                                |
| `fuzz_epistemic_ingress` | 315 runs in 120 s, seed 1, no crash, with `alloc_dealloc_mismatch=0` set  |
| translator coverage      | `tools/check-translator-coverage.py` exits 0                              |

`E2EBinaryOrchestrationTest.aDraggedSelectionLandsWhereItIsDropped` failed once in the full
`xuzz_test` run and passed on the run before it and on three runs alone. It drives `build/xuzz`
through a drag and touches nothing in this slice, so it is recorded as timing under load, not as a
regression.

## I3-3b1: pairing, mappings and the time split

Source baseline: `114cd386`. Authority: I3-3D revision 2.1, sections 4.1 to 4.3, 5.1 (the freshness
bound), 7, 7.1, 8 (the `record.signed_time` and `stream.clock` rows and the host-state table), 9.1
and 10 (the b1 rows) and 13.3. The slice landed in two parts, each built by a Sonnet implementation
agent from the lead's written decisions and then reviewed, rebuilt and tested by the lead. Part A is
commit `de5887cd`, the time split in `observe`. Part B is this commit, the mapping table and pairing
in the adapter. Paths are relative to `apps/common/xanadu/vagent/runtime/` unless they start with
`tests/`, `tools/` or `design/`.

### What changed

- **The time split, part A** (`agent_executor_clock.cpp`, new).
  - `CheckedObservation` carries what the device signed as `DeviceTimes` and the host's mapping as
    `ClockMapping` (`epistemic_ingress.hpp`).
  - `classifySignedTimes` decides rows 1 to 4 in 128-bit integers. `classifyMappedTimes` decides
    rows 5 to 9.
  - `observe` runs both in table order. The old receipt-based sites are deleted, not renamed.
  - New refusals, all uncounted: `InvalidClockMapping` (NeedsHostAction), `UncertainClockMapping`
    and `ClockMappingMismatch` (Retryable), and `LateDelivery` and `MessageExpired` (Terminal).
- **The arena, part A.**
  - A2 emits `ClockEpochRegression`, and A3 emits the new uncounted `AcquisitionOrderMismatch`
    (`compiler.cpp`, about 4590 and 4601).
  - Each stream keeps its `clock_instance`, so a mapping version is compared only under one
    instance.
  - Records keep `acquired_device`, `sent_device` and `device_uncertainty` (u_d in ns) beside the
    mapped `acquired_at` and U.
  - Both structures are registered (`record.signed_time`, and `stream.clock` under a new `where` of
    "stream field") and carried by resume and upgrade.
- **Counted version 2** (`epistemic_health.hpp:80`). Eight codes, all counted for relayed copies.
  - `tools/check-counted-sites.py` pins every emitting site per code per file and runs under
    `make lint`.
  - `VagentEpistemicClockSites.APlantedCountedSiteFailsTheScript` plants an extra
    `refuse(EpistemicRefusal::ObservationTimeMismatch)` in a copy of the tree and sees the script
    fail.
- **Diagnostics, part A.** A per-source time-class fraction in host memory, fed by rows 4, 7 and 8,
  each stream sequence at most once. It warns when one source crosses `time_suspect_fraction` while
  fewer than `common_cause_sources` others do, and is exposed as `SourceHealth::timeClassFraction`.
  A second warning fires when `common_cause_sources` sources hit rows 2 or 3 within
  `common_cause_window_ms`. Neither is a reason or `f`.
- **The pass-credit bound** (7.1). Part A credits up to `verifiedSequenceBound` when it is present.
  Part B sets it to the issuer's highest verified sequence for the incarnation
  (`epistemic_adapter.cpp`, `noteVerified` and `verifiedBound`).
- **The mapping table, part B** (`epistemic_adapter_clock.{hpp,cpp}`, new). `ClockMappingTable` is
  keyed by (issuer, incarnation, epoch, device basis), with a wall and a steady mapping per mint and
  one version per mint.
  - **Synchronised.** Wall offset 0 with `wall_sync_uncertainty_ms`. The steady offset is the host's
    steady-minus-wall at mint.
  - **Free.** Paired only.
  - **Challenges.** Keyed by (issuer, incarnation) and never replaced inside
    `clock_pair_timeout_ms`. Rate-limited per issuer by `clock_pair_interval_ms`. The nonce is drawn
    from `RAND_bytes` and compared with `CRYPTO_memcmp`, together with the challenge's audience. A
    challenge is consumed only by an accepted reply.
  - **The mapping a reply proves.** The offset is the midpoint of $[t_1 - d, t_2 - d]$ in integer
    nanoseconds, $u_0 = (t_2 - t_1)/2$ and the anchor is $d$. The wall mapping adds the host's
    wall-sync uncertainty.
  - **Re-pairing.** Triggered when $u_M$ at the message's `sent` reaches `clock_repair_fraction` of
    a bound predicate's `max_uncertainty`.
- **Step 6 in the adapter, part B.**
  - Observations carry the table's mapping into `observe`, or none, and then `observe` refuses
    `UnknownClockMapping` and a pairing starts.
  - Heartbeats and reports get the mapped rows on `sent` and the 5.1 freshness bound (`liveness`).
    The rows' arithmetic is shared with `observe` through `epistemic_clock_rows.hpp`.
  - `timely()` for incarnation advances runs the same rows, so an advance needs a mapping.
  - `clock_pair` replies skip step 6 and go to the table; anything unsolicited, late or mismatched
    refuses `ClockPairUnsolicited` (Terminal, unlogged).
- **Binding, part B.** A `wall` predicate whose `max_uncertainty` is at or below
  `wall_sync_uncertainty_ms` refuses `InvalidBinding` at bind, and what the call bound is unbound
  again. `SourceBinding` gains `driftPpm`. A source whose `UncertainClockMapping` refusals persist
  past `common_cause_window_ms` gets one warning per streak, naming its alias.
- **Settings.** Part A added `time_class_half_life`, `time_suspect_fraction`, `common_cause_sources`
  and `common_cause_window_ms` to the executor. Part B added `clock_drift_ppm`,
  `clock_repair_fraction`, `clock_pair_interval_ms`, `clock_pair_timeout_ms`,
  `wall_sync_uncertainty_ms` and `max_path_lateness_ms` to `AdapterLimits`. All ten are in
  `defaultSettingSpecs()` with the design's defaults and ranges.
- **Tests.**
  - `tests/xuzz/vagent_epistemic_clock_test.cpp` (part A, 25 cases): every row of 4.3 at its
    boundary, the sites script, the time-class fraction and the warnings.
  - `tests/xuzz/vagent_epistemic_pairing_test.cpp` (part B, 21 cases).
  - `tests/xuzz/epistemic_time.hpp` is the shared helper that builds device times and an identity
    mapping for tests calling `observe` directly; every existing builder and the fuzzer use it. The
    fuzzer also perturbs the mapping.

### Design tests (13.3)

| Test                                                                        | Where                                                                                                      |
| --------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------- |
| each row of 4.3 at its boundary, with margins                               | `VagentEpistemicClock.Row1…` to `Row9…`, `SignedChecksRunWithoutOverflow`                                  |
| `common_cause`: shifted on eight, NaN on eight; `R`, `N`, stamps identical  | `HostFaultsOfEitherKindMoveNoEstimatorWeight`                                                              |
| the pinned-sites script fails on a planted site                             | `VagentEpistemicClockSites.APlantedCountedSiteFailsTheScript`                                              |
| a device flushing 600 buffered readings after an outage moves no `R` or `N` | `AFlushOfBufferedReadingsAfterAnOutageMovesNoWeight`                                                       |
| a relayed copy counts exactly the v2 codes                                  | `ARelayedCopyCountsExactlyTheVersion2Codes`                                                                |
| a relay replaying an old message cannot starve pairing (S-3a)               | pairing test, S-3a case                                                                                    |
| a second source echoing the victim's nonce is refused; the victim pairs     | pairing test, S-3b case                                                                                    |
| a pairing bound contains the true offset under drift                        | pairing test, 50 ppm simulated clock (re-pair mints version 2)                                             |
| the first pairing after a reboot does not move the time-class fraction      | `TheFirstPairingAfterARebootDoesNotMoveTheFraction`                                                        |
| a slow stream with a stride of about 1,080 is still credited                | `ASlowStreamIsCreditedUpToTheVerifiedSequence`, and through the adapter in the pairing test (stride 1,081) |
| a wall-sync policy below `wall_sync_uncertainty_ms` refuses at bind         | pairing test, bind-time refusal and the boundary that still binds                                          |

### Measurements

- **Pairing $u_0$ on loopback.** 30 µs, against the design's "under 1 ms".
- **Record cells.** Each record gains 6 cells (three fields), section 12's "+6 a record".
- **Line counts.** About 1,750 lines of source against the design's 1,500. The executor's net
  additions are 33 of its 300, and `compiler.cpp`'s 22 of its 120.

### Deviations and readings

1. **Where the table lives.** It is in `epistemic_adapter_clock.{hpp,cpp}`, because part A's
   classification took `agent_executor_clock.cpp`. `epistemic_clock_rows.hpp` shares the mapped rows
   between the adapter and `observe`.
1. **The device basis is part of the mapping key.** A `free` pairing and a `synchronised` mint then
   cannot overwrite each other. One version number covers both timelines of a mint.
1. **At most 64 epochs per issuer** (`kMaxMappingEpochsPerIssuer`), oldest dropped. This bound is
   not in the design. A verified device chooses its epochs, so without it the device could grow the
   table.
1. **Re-pairing is judged per bound predicate**, on its own timeline, against that predicate's
   `max_uncertainty`. This can trigger earlier than "the smallest bound" and never later.
1. **Heartbeats and reports are judged on the host's steady timeline**, with the smallest
   `clock_skew` among the source's predicates.
1. **Pairing is triggered only by a message at the bound incarnation**, or one a `signed` policy
   could advance to, and only when the host has a challenge sink. Elsewhere a challenge has no use.
1. **Negative device times on non-observation kinds are `MalformedMessage`.** This covers a negative
   `sent` on a heartbeat, report or pair, and a negative `clock_uncertainty` on a heartbeat or
   report. `InvalidAcquisition` would have added a counted site outside the pinned table.
1. **$t_1$ is read when the challenge is made, and $t_2$ when the reply's ingest begins.** This is
   the tightest interval the host can prove.
1. **The challenge and audit sinks run after the adapter's mutex is released**, so a host may
   re-enter the adapter from them. This also moves a2's audit emit.
1. **Synchronised mappings are minted only while the host's wall clock is synchronised.** Otherwise
   the reading refuses `ClockUnsynchronised` (wall) or `UnknownClockMapping`.
1. **The time-class fraction has a denominator.** A reading that clears row 9 feeds $N$ only. The
   per-source warning also needs $N$ at or above `suspect_min_events`. Its once-per-sequence ring is
   separate and sized by `health_replay_ring`.
1. **The new record fields follow `clock_instance` in the unordered tail**, not in
   `kObservationOrder`, whose positions the routine reads.
1. **`AcquisitionOrderMismatch` is charged as "other"**, so it no longer bumps `content_refusals` or
   the contact stamp. `StaleObservation`, `ObservationTimeMismatch` and `UncertainAcquisition`
   remain content refusals.
1. **a2's `OnlyASynchronisedClockOnAWallPolicyMapsYet` is now
   `ASynchronisedClockMapsOnBothTimelines`.** A synchronised device on an `actor_mapped` predicate
   now commits. a2's deviation 1 is closed.

### Limits carried forward

- **Dispatch of fresh heartbeats and reports** is still `UnsupportedKind` until I3-3c1 and I3-3d,
  and so are their step-4 floors.
- **Jumps in the host's wall-minus-steady offset** are I3-3b2's. A synchronised `actor_mapped`
  mapping keeps the offset it was minted with.
- **The adapter step in the fuzzer**, owed since a2, is still owed. Part A's fuzzer perturbs the
  mapping that reaches `observe`.
- **Traceability.** b1 closes 3.2-7 (design 13.3), recorded at M2 acceptance under decisions 0.1.

### Suite results

`make -j$(nproc)` and `make vagent_actor_test` exited 0 with no warning in project code; touched C++
is clang-format 19.1.7 clean; `make lint` (with `tools/check-counted-sites.py`) and
`tools/check-translator-coverage.py` exit 0. Every suite ran headless with XDG under `build/xdg`.

| Suite                    | Part A (`de5887cd`) | Part B (this commit)                                             |
| ------------------------ | ------------------- | ---------------------------------------------------------------- |
| `vagent_actor_test`      | 1,149 passed        | 1,170 passed; loopback $u_0$ 38 µs                               |
| `xuzz_test`              | 3,325 passed        | 3,346 passed; loopback $u_0$ 34 µs                               |
| `gleditor_test`          | 830 passed          | 830 passed                                                       |
| `zigzag_test`            | 256 passed          | 256 passed                                                       |
| `fuzz_epistemic_ingress` | 191 runs in 60 s    | 383 runs in 123 s, seed 1, no crash (`alloc_dealloc_mismatch=0`) |

## I3-3b2: the lineage of segments and jump invalidation

Source baseline: `5e5cfe42`. Authority: I3-3D revision 2.1, sections 4.4 and 4.5, 8 (the
`agent.clock_lineage` row), 9.1 (the b2 rows) and 13.4. A Sonnet implementation agent built it from
the lead's written decisions. The lead reviewed the host detection and the invalidation, formatted
one line, and ran every gate. Paths are relative to `apps/common/xanadu/vagent/runtime/` unless they
start with `tests/` or `design/`.

### What changed

- **The lineage** (new `epistemic_emit_clock.cpp`, 380 lines, all IR).
  - An `EpistemicClockLineage` record on the agent's `d.epistemic` rank holds a `segments` list.
    Each segment is `{instance, start_steady, anchor_steady, anchor_wall, anchor_sync}`, and the two
    wall fields are absent while the wall clock is unsynchronised.
  - At most `clock_lineage` segments are kept; the oldest is evicted past the bound.
  - `emitRtClockNote` applies the segment rule of 4.4 on every committed ingestion while
    synchronised (`compiler.cpp`, `callClockNoteIfPresent`). A step within
    $\sigma_\text{old} + \sigma_\text{new} + 10^{-6}\rho_s\Delta s$ moves the anchor in place, and
    anything larger appends a segment.
  - The first bind of an instance queues that instance's first segment (`bindSource` →
    `startLineageLocked`).
  - The host's $w$, $s$ and $\sigma$ reach the turn as `$turn.clock`.
- **`rtClockRebase(instance, t)`** (`emitRtClockRebase`). It returns $\Delta$ and $u_\Delta$ against
  the current segment, and fails for an unanchored segment, or one that is no longer in the lineage.
  Nothing calls it in production; I4-6 will.
  - Tests run the IR itself through `requestClockRebase` and `clockRebaseResult`, a read-only
    control turn of the new `clockEntry` that is rolled back after `TurnSpec::inspect` hands the
    host the turn record. That is a test and diagnostic surface, not an API for I4.
- **Host detection** (`agent_executor_clock.cpp`, `checkClock` and `checkClockLocked`).
  - The executor tests the wall-minus-steady offset against its host anchor under the ingress mutex.
    A NaN counts as a jump.
  - It runs first in `observe`, at the top of each `pumpUntilIdle` round and in `tick`. That is the
    nearest the tree has to I3-3c1's timer wake.
  - A jump bumps a generation, queues a segment note for every arbitrating actor, and logs INFO.
    Without a synchronised wall clock nothing is detected (4.4's residual, Q10).
- **Invalidation** (`epistemic_adapter_clock.cpp`, `ClockMappingTable::invalidateSteady`). The
  adapter's `followClock` runs first in every ingest.
  - When the generation has moved, paired mappings lose their steady side, so the next message
    refuses `UnknownClockMapping` and pairs.
  - Synchronised compositions are re-minted at the next version.
  - Wall mappings are untouched.
  - `EpistemicAdapter::clockPending` is the time a pairing's mapping was dropped. It is cleared by
    the completed pairing or by an unbind.
- **Translation.**
  - `agent.clock_lineage` is registered Carried and Carried.
  - `agentRankRecords` admits one well-formed lineage beside the audience and refuses a malformed or
    repeated one as `StateInconsistent` (new `epistemic_translate_clock.cpp`).
  - The copier's pass 0 and the digest carry it.
  - `distinctTextBytes` now counts agent-rank records, which the audience had been missing since a2.
- **Settings.** `clock_lineage` (8, 2 to 64) and `steady_drift_ppm` (200, 0 to 10000). The executor
  also reads `wall_sync_uncertainty_ms`, the same key the adapter reads.
- **Tests.**
  - New `tests/xuzz/vagent_epistemic_lineage_test.cpp`, 22 cases, driving the executor with a clock
    whose wall and steady parts move independently.
  - Three existing tests price the lineage's cells: the shape test, the cell-ceiling test and the
    health admission test.

### Design tests (13.4)

| Test                                                                        | Covered                                                                                                                                                                                                        |
| --------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| a rebase across a resume lands within its bound                             | yes                                                                                                                                                                                                            |
| a suspend between a record and the rebase rebases it by its length          | yes (wall stepped, steady not)                                                                                                                                                                                 |
| a suspend on an agent with only steady-basis predicates (N1)                | yes: detected at the first `observe`; paired refuses, pairs once and resumes; synchronised commits under a higher version; no pre-suspend mapping commits; `R`, `N` unchanged; `clockPending` set then cleared |
| … a pairing that misses the heartbeat deadline sets `silent` (`clock`)      | **no**: the silence state machine and the timer turn are I3-3c1's                                                                                                                                              |
| a jump found by a timer wake alone appends the segment                      | yes, by a pump with no message                                                                                                                                                                                 |
| a wall step under synchronisation starts a segment                          | yes                                                                                                                                                                                                            |
| an unsynchronised anchor fails the rebase; an evicted segment fails it      | yes                                                                                                                                                                                                            |
| no path re-maps a record                                                    | yes                                                                                                                                                                                                            |
| U-19: a latched source resumed with no anchor, paired and fed stays latched | yes                                                                                                                                                                                                            |
| populated resume and upgrade                                                | yes, plus a malformed-lineage resume refusal                                                                                                                                                                   |

### Measurements

- **The lineage's cells.** The record is 3 cells, plus 11 per anchored segment or 7 per unanchored
  one: 91 cells at the default bound of 8. Design 12 estimated about 99 with the audience's 3, and
  the measured total is 94. An agent holds 14 more cells than before at its first segment.
- **Line counts.** About 1,126 lines of source against the design's 1,100. The executor is at +49 of
  its 150 and `compiler.cpp` at +10 of its 80.

### Deviations and readings

1. **`clockPending` lives on `EpistemicAdapter`, not `SourceHealth`.** The adapter owns the mappings
   it describes.
1. **The `clock` silence cause and the timer turn are deferred to I3-3c1**, which builds the silence
   state machine. The pump stands in for the timer wake.
1. **An instance's first segment starts at steady time 0.0.** If it started at the first note, a
   reading acquired just before the bind would fall in no segment, and its rebase would fail.
1. **An unanchored current segment gets a new segment at the first synchronised commit**, rather
   than gaining an anchor in place. This errs toward refusing a rebase.
1. **A reading at or below the current anchor's steady time changes nothing.** Control items run
   ahead of data, so a reading queued before a jump must not start a bogus segment.
1. **The host anchor rolls on every reading**, mirroring the arena's in-place update. It is kept
   while the wall clock is unsynchronised, so a jump during that stretch is still caught when
   synchronisation returns.
1. **Segment notes for an actor whose admission is closed wait in `lineageOwed`** until it reopens.
   Otherwise a post could land between an upgrade's count of the post box and its swap.
1. **No reserve is kept for a segment appended at the cell ceiling.** Such a segment can make that
   one ingestion refuse `EpistemicBudget`.
1. **The translator reads a stored Int64 as a number**, because a checkpoint retypes whole-valued
   doubles. An absent `d.step` dimension means no link.

### Limits carried forward

- **The adapter step in the fuzzer** is still owed.
- **Q10, `CLOCK_BOOTTIME`, is unbuilt.** The owner approved it (decisions 0.2), but no slice is
  assigned to it. Without a synchronised wall clock a suspend stays invisible.
- **Traceability.** b2 closes U-17, U-19, 3.2-6, 3.2-8, 8.2-5d and 8.2-R3c (design 13.4), recorded
  at M2 acceptance.

### Suite results

`make -j$(nproc)` and `make vagent_actor_test` exited 0 with no warning in project code; touched C++
is clang-format 19.1.7 clean; `make lint` (with `tools/check-counted-sites.py`) and
`tools/check-translator-coverage.py` exit 0. Every suite ran headless with XDG under `build/xdg`.

| Suite                    | Result                                                                   |
| ------------------------ | ------------------------------------------------------------------------ |
| `vagent_actor_test`      | 1,192 passed                                                             |
| `xuzz_test`              | 3,368 passed                                                             |
| `gleditor_test`          | 830 passed                                                               |
| `zigzag_test`            | 256 passed                                                               |
| `fuzz_epistemic_ingress` | 311 runs in 125 s, seed 1, no crash, with `alloc_dealloc_mismatch=0` set |

## I3-3c1: heartbeats, silence and the timer wake

Source baseline: `77175afa`. Authority: I3-3D revision 2.1, sections 4.5 (the `clock` cause,
deferred from b2), 5.1 to 5.4, 7, 8 (the c1 rows), 9.1 (the c1 rows) and 13.5. Section 13.5 says "as
in revision 1", and revision 1 is not in the tree, so the slice was built from those sections. A
Sonnet implementation agent built it from the lead's written decisions. The lead reviewed the
simulated-clock change and ran every gate. Paths are relative to
`apps/common/xanadu/vagent/runtime/` unless they start with `tests/` or `design/`.

### What changed

- **Expectations.**
  - `SourceBinding` gains `heartbeat` and a per-predicate `data` map, and `EpistemicSourceGrant`
    gains `heartbeat` and `data`.
  - Every duration, the lifetime included, must be at least `heartbeat_floor_ms`. Both
    `EpistemicAdapter::bind` and `bindSource` refuse `InvalidBinding` otherwise (S-13b).
  - A source's heartbeat is the smallest among its live bindings.
- **Arena state** (new `epistemic_emit_liveness.cpp`, all IR, and `epistemic_abi_liveness.hpp`).
  - Profiles gain `silent`, `silent_since`, `silent_cause` and `liveness_instance`. Streams gain
    `received` and `stale`.
  - A commit writes the stamps and clears `stale` and `silent`. `last_contact` is now written for
    relayed copies too (5.1).
  - The I3-2 reset clears every flag and both stamps, and the deadline restarts from the reset.
  - `wouldChangeHealth` counts a silent or stale source as a change for a reset.
- **Heartbeats.** A heartbeat that verified and passed step 4 and the step-6 freshness bound is
  fresh. The adapter calls `AgentExecutor::noteFresh`, then `noteSettled`, which raises the floor,
  and returns a receipt with no event ID.
  - `noteFresh` re-arms the deadline. It writes the arena only on the SILENT-to-LIVE transition.
  - A heartbeat that is not fresh refreshes nothing.
  - Health reports stay `UnsupportedKind` until I3-3d.
- **The timer** (new `agent_executor_liveness.{hpp,cpp}`).
  - `LivenessHeap` is indexed: one entry per (pid, principal, optional stream), updated in place.
  - It is armed at bind, on each fresh message and on each commit (streams). After a resume it is
    armed from the arena's rebased stamps.
  - `tick`, and so `advanceSimulatedTime`, collects due entries into `liveness` control items.
    `earliestDeadline` includes the heap's head.
  - The liveness turn sets a flag only if no newer stamp is in the arena, so a commit that races the
    timer wins.
  - The cause is `clock` while the adapter reports a dropped mapping through `noteClockPending`,
    otherwise `heartbeat`.
  - Latched sources arm nothing.
- **Health.** `SourceHealth` gains `silent`, `silentCause` and `staleStreams`. The flags never move
  `R` or `N`, never add a reason and never change the displayed state.
- **Resume.**
  - `profile.liveness` is now Carried on resume, and copier pass 4's clearing is removed.
    `profile.silence` and `stream.liveness` are registered.
  - `emitRtLivenessRebase` runs from the new instance's first clock turn and rebases each stamp to
    $t + \Delta - u_\Delta$.
  - A failed rebase, or stamps with no `liveness_instance`, sets `silent` with cause `rebase` at
    once (U-18, 8.2-R3d).
- **Sequence gaps.** `EpistemicAdapter::sequenceGaps(principal)` counts gaps above
  `sequence_gap_note` between an issuer's consecutive verified sequences, across all kinds (m4).
- **Settings.** `heartbeat_floor_ms` (100, 10 to 60000) and `sequence_gap_note` (64, 1 to 2^31). The
  executor and the adapter both read the floor.
- **Simulated time.** The executor's steady reads go through `steadyNow()`, which applies the
  simulated offset `advanceSimulatedTime` already applied to the wall clock. Simulated time
  therefore moves both clocks together and no longer looks like a jump. Real clocks are untouched.
- **Tests.**
  - New `tests/xuzz/vagent_epistemic_liveness_test.cpp`, 23 cases.
  - `Translate.LivenessFieldsAreCarriedByBothDispositions`.
  - Existing tests change where the rules changed:
    - fresh heartbeats now return a receipt (pairing and adapter tests);
    - relayed commits refresh contact (`ACommittedRelayedCopyRefreshesContactToo`);
    - a lifetime below the floor refuses at bind;
    - resumed stamps are carried and rebased, not cleared.

### Design tests (13.5, and b2's deferred case)

| Test                                                                     | Covered                                                                                                          |
| ------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------- |
| the state machine: UNHEARD → SILENT, LIVE → SILENT → LIVE                | yes; no arena write across ten heartbeats while LIVE                                                             |
| heap size stays at most sources × streams under 100,000 heartbeats       | yes                                                                                                              |
| 128 sources due in one wake produce one control item per pid             | **partly**: 64 make one item, and 128 make two (deviation 1)                                                     |
| `data_ms = 1` refuses at bind                                            | yes, and any duration below the floor, at `bind` and `bindSource`                                                |
| a huge `clock_uncertainty` does not widen the freshness window           | yes                                                                                                              |
| a withheld heartbeat past the path bound does not revive a silent source | yes                                                                                                              |
| `stale` set and cleared; a commit racing the timer wins                  | yes                                                                                                              |
| flags move no `R` or `N` and no displayed state; reset clears them       | yes                                                                                                              |
| a latched source arms nothing                                            | yes                                                                                                              |
| b2's case: a pairing that misses the deadline sets `silent` (`clock`)    | yes; the next fresh message after pairing clears it                                                              |
| resume rebases conservatively; a failed rebase sets `silent` (`rebase`)  | yes: evicted, unanchored and shape-1 profiles; the first deadline comes from the rebased stamp; a latch survives |
| sequence gaps per issuer                                                 | yes                                                                                                              |
| populated resume and upgrade                                             | yes                                                                                                              |

### Measurements

- **Cells.** A profile at rest gains 2 cells (`liveness_instance`) and a stream 2 (`received`). A
  silent profile gains 6 more, once, and a stale stream 2. A profile that is both silent and stale
  measured +8, and repeating it adds nothing.
- **Line counts.** About 1,590 lines of source against the design's 1,300, mostly the heap's header
  and comments. The executor is at +51 of its 200 and `compiler.cpp` at +8 of its 100.

### Deviations and readings

1. **Not every wake fits one control item.** A turn's scratch is bounded at 32 KiB. A silence on an
   existing profile costs about 330 bytes, and one that creates the profile of a never-heard source
   about 3 KiB.
   - The host splits a wake's entries by estimated cost (`kLivenessItemBudgetBytes`,
     `kLivenessEntryBytes`, `kLivenessCreateBytes` in `epistemic_limits.hpp`). Up to 73 existing
     profiles fit in one item; 128 take two, and never-heard sources at least 16.
   - A failed turn undoes only its own chunk.
   - S-13c's purpose, bounded turns per wake rather than one per source, holds. Its letter, one item
     per pid, does not.
   - The alternative was to create profiles at bind, which would change what binding means for every
     existing test.
1. **An UNHEARD source's first silence creates its profile.** No profile exists before a reading.
   The item carries the binding's incarnation and `max_sources`, and a full principal budget ends
   that turn `EpistemicBudget`.
1. **A stream that has never committed is never stale.** It has no record to flag; the heartbeat
   covers the source.
1. **A completed pairing only clears `clockPending`.** The `clock` silence ends at the next fresh
   message.
1. **A profile with no stamps and no `liveness_instance` learns the instance**, rather than being
   marked `rebase`. Nothing exists to rebase. Stamps without an instance, or any failed rebase, take
   the `rebase` path, which also clears the unusable stamps.
1. **Liveness items are collected in `tick` only**, not in `pumpUntilIdle`. Simulated time now moves
   the steady clock as well as the wall clock (`steadyNow()`).
1. **Closed admission and unbinding.** A closed door re-arms a due entry one floor interval later
   instead of posting it. Unbinding the last binding erases the source's host liveness state, as
   `retireSource` does.
1. **Flags are written without an admission check.** An arena at its cell ceiling can exceed it by
   at most the flag cells above, per profile and stream.

### Limits carried forward

- **Health reports** (I3-3d) and freeze (I3-3c2) are next. `frozen` is not built.
- **The adapter step in the fuzzer** is still owed.
- **Q10** (`CLOCK_BOOTTIME`) is still unscheduled.
- **Traceability.** c1 closes U-18, 3.2-4, 5.1-20, 5.1-22, 8.2-4f and 8.2-R3d (design 13.5),
  recorded at M2 acceptance.

### Suite results

`make -j$(nproc)` and `make vagent_actor_test` exited 0 with no warning in project code; touched C++
is clang-format 19.1.7 clean; `make lint` (with `tools/check-counted-sites.py`) and
`tools/check-translator-coverage.py` exit 0. Every suite ran headless with XDG under `build/xdg`.

| Suite                    | Result                                                                   |
| ------------------------ | ------------------------------------------------------------------------ |
| `vagent_actor_test`      | 1,215 passed                                                             |
| `xuzz_test`              | 3,391 passed                                                             |
| `gleditor_test`          | 830 passed                                                               |
| `zigzag_test`            | 256 passed                                                               |
| `fuzz_epistemic_ingress` | 193 runs in 154 s, seed 1, no crash, with `alloc_dealloc_mismatch=0` set |

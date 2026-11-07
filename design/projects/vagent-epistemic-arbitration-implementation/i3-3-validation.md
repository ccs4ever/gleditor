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

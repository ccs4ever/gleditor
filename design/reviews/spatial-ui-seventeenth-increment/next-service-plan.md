# Next service prerequisite: normal publication reception and identity trust

## Scope and increment 17 status

This plan describes the next prerequisite after increment 17; it is not an implementation or an
end-to-end claim. Increment 17 is still pending the root-owned canonical build and native review at
the time of writing. Its inbox status work must be reviewed on its own terms before calling it
complete. This plan assumes no product behavior beyond the current source inspected here.

## Current production gap

The publication components already have real BitTorrent transport factories. Normal `Session`
startup does not install them. `Session::configureTestPublicationSwarm()` is the only path that sets
the publication network fields, and it explicitly installs a `MockVerified` identity callback and
relaxes DHT/address protections for colocated test peers. `publicationOutbox()`,
`publicationInbox()`, `publicationDiscovery()`, `publicationSubscriptions()`, and
`linkPackageExchange()` create persistent local services without a network transport unless that
test switch was used. The normal UI can prepare and retain local publications, but normal receive,
author/topic discovery, and announcement cannot reach a remote swarm. The truthful “no connection
configured / availability not checked” state should remain visible until a production configuration
exists.

Existing factories are useful seams, not proof of a complete service:

| Existing seam                                      | What it can do                                                   | What remains absent in normal startup                                                                     |
| -------------------------------------------------- | ---------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------- |
| `publicationSwarmTransport`                        | Seed dependencies and publish/resolve BEP 46 mutable names       | Session configuration and a non-mock publisher identity decision                                          |
| `publicationDownloadSwarmTransport`                | Resolve signed manifests and fetch/verify referenced content     | Session configuration and the user-visible normal connection settings                                     |
| `publicationDiscoverySwarmTransport`               | Resolve and verify author catalogs and topic/backlink rendezvous | Session configuration; live results still do not establish Oracle enrollment or content availability      |
| `publicationPollSeconds` in `system://settings`    | Govern local subscription polling cadence                        | No corresponding bootstrap/listen configuration; polling with no transport must not claim availability    |
| `configureTestPublicationSwarm` and CLI test flags | Exercise real transport against explicitly supplied test peers   | Test-only identity classification and test-only privacy relaxations; not a production configuration route |

The architecture already uses a worker-owned `SwarmContentSource` and worker threads for publication
jobs. The increment should reuse those owners/factories. It should not create a second network
stack, put libtorrent callbacks on the UI thread, or use a test flag as an undocumented normal mode.

## Evidence and identity boundaries

There are four separate facts to represent and never infer from one another:

1. **Publication pointer/key control.** A valid BEP 46 signature establishes control of the mutable
   key for that signed pointer and sequence. It does not establish a civil identity or enrollment.
1. **Authorship provenance.** GPG verification of the signed authorship record identifies the key
   that signed that record. This is not automatically a binding to the BEP 46 publishing key.
1. **Ledger inclusion and enrollment.** `IdentityNetworkController` can validate identity entries
   and Merkle inclusion against its own `EnginePipeline::root()`. Oracle attestations are checked
   against the pipeline's registered, non-revoked oracle entries and quorum rules. These checks only
   mean something if the client has acquired and authenticated the ledger history/root it follows.
1. **Content availability and validity.** A successful catalog/query or signed pointer is not a
   fetched publication. Manifest validation, exact pin checks, and dependency verification are the
   later steps that can establish content availability for the pinned snapshot.

Current code has cryptographic building blocks, but no ordinary Session bootstrap path for a trusted
ledger checkpoint/history, no user-visible choice of which ledger root/oracle quorum to follow, and
no normal publication identity callback that can safely return `Verified`. The controller's default
`EnginePipeline` is empty. A peer inclusion proof is correctly checked against the controller's own
root, but the source does not show a production source of that root. Device challenge-response also
requires the claimed fingerprint's delegated device key to have been verified and registered. The
`DeviceDelegation` verifier can validate a signed delegation, and `trustDelegation()` can register
it, but that is a primitive, not an enrollment/onboarding workflow. Local publication identity is a
BEP 46 Ed25519 key; publication authorship has an OpenPGP signing key. The binding between them must
be explicit and verified, never guessed from matching display names or email strings.

`PublicationIdentity::{Unknown, MockVerified, Verified}` already reserves the production result
without treating absence of an identity verifier as success. Keep `MockVerified` confined to the
explicit test-swarm configuration. Normal reception can be enabled before publisher enrollment is
implemented, provided UI and persisted status continue to show `Unknown`/not enrolled distinctly and
cannot convert a valid signature into `Verified`.

## Smallest prerequisite sequence

### 1. Govern normal transport configuration

Add a typed publication-network configuration through the existing sovereign settings model and a
visible settings form. It needs, at minimum, an explicit enable/disable choice, listen interface,
and repeatable bootstrap node host/port entries. Validate malformed addresses and ports at the
settings boundary, and display the effective state. Do not hard-code public bootstrap nodes or
silently fall back to the test swarm. The default should preserve today's privacy behavior: no
publication reception/discovery traffic until the user enables and configures it. When enabled,
retain the current publication-factory privacy policy: DHT enabled, local peer discovery disabled,
trackers disabled, and the generic swarm constructor clears public bootstrap routers. DHT is
reachable only through the explicitly named bootstrap nodes in this configuration. Preserve
`restrictDhtToDistinctNetworks=true`, `allowManyConnectionsPerAddress=false`, and the normal DHT
packet rate. The test-only relaxations (distinct-network restriction off, same-address duplicates
on, elevated packet rate) must not leak into this path. Do not infer publication policy from generic
`SwarmContentSource::Options` defaults. Keep settings separate from the content cache and from
per-store authorship settings.

Use the current Session lazy construction seam to build the existing outbox, inbox, discovery,
subscription, and link-package factories with the same validated configuration. Derive the ephemeral
reader listen port from the configured bind address safely; the existing test code does this by
replacing the substring after the last colon, which should not become the normal configuration
parser. Keep mutable keys and catalogs tied to the existing permascroll identity. Ensure transport
lifecycle, retry, and shutdown remain owned by the existing worker/service objects. A settings
change should reconfigure only after a deliberate service restart boundary; it must not mutate an
active transport from the UI thread.

### 2. Make identity enrollment a real prerequisite for `Verified`

Before a production verifier returns `PublicationIdentity::Verified`, define and implement how a
client obtains an authenticated ledger history/checkpoint and how the user chooses or confirms the
trust root. Feed that verified history into the existing `EnginePipeline` validation path; validate
block chaining, Merkle roots, entry/vote signatures, revocation state, and attestation
quorum/expiry. Persist the accepted checkpoint and policy with explicit versioning and refuse
corruption or rollback rather than silently starting an empty ledger. Bind the publication BEP 46
key to an enrolled fingerprint through a signed, validated device delegation or an equally explicit
identity record. OpenPGP authorship remains a separate signature until an explicit cryptographic
binding relates its fingerprint to that enrollment.

The onboarding form must explain the evidence it has and the authority it follows. Unknown
publisher, valid but unenrolled publisher, bad pointer signature, invalid ledger proof, revoked
device, and unavailable cited content need distinct outcomes. Do not label an unknown publisher
“untrusted” when no trusted root has been configured, and do not describe a valid BEP 46 signature
as enrollment.

This step can be staged: transport configuration can support receive/discovery with identity
classification still Unknown. Production announcement must remain blocked at `NeedsVerification`
until the verifier has a trusted ledger and a valid binding. No trust-on-first-use promotion should
happen merely because a network response is well-formed.

### 3. Wire normal service construction to the settings and trust owners

After the settings and identity prerequisites exist, add one production Session
factory/configuration path that composes the existing transport factories with the validated
settings and the real identity verifier. Keep `configureTestPublicationSwarm()` separately named and
behaviorally test-only. Avoid per-service copies of host parsing, identity decisions, or transport
defaults. Surface a concise configured/disabled/connecting/error state in the current publication
and discovery status forms; a blank bootstrap list should yield a clear unavailable/not configured
state, not a background retry loop that claims a network failure.

## Test plan

Tests should prove the seams and evidence distinctions without pretending to prove a public network:

- Settings schema/default tests: normal startup remains disabled or otherwise explicitly configured
  according to the selected privacy default; invalid listen/node values refuse; test-only flags do
  not appear in the ordinary settings flow. Assert the publication-specific policy: DHT on, LSD and
  trackers off, no implicit bootstrap routers, named nodes only, distinct-network restriction on,
  same-address duplicates off, and normal packet rate.
- Session construction tests with injected deterministic transport factories: disabled creates no
  transport; enabled passes the configured interface/nodes and production privacy defaults; changing
  config does not silently mutate a running worker.
- Identity tests: an empty or unauthenticated ledger cannot produce `Verified`; a valid manifest
  with unknown publisher remains unknown; valid ledger inclusion plus valid delegation and current
  non-revoked enrollment can produce the verified classification; wrong root, invalid signature,
  missing delegation, expiry, and revocation each refuse with separate status/reason.
- UI status tests: pointer-signature result, enrollment result, and referenced-content availability
  appear as separate facts. A catalog result must not be rendered as a completed download.
- Keep existing fake transports/doubles for unit logic only. Do not add fake callbacks named as
  native services or assert network reachability from those tests.

## Native validation plan

The native acceptance should wait until a real bootstrap/trust configuration exists. Use two
isolated profiles and an explicitly configured pair of nodes on the repository's rootless
network-namespace path for reproducible transport proof, while preserving production privacy
options. Then perform the visible workflow: configure a normal endpoint, publish a local snapshot,
observe the blocked status before enrollment, configure/verify the identity authority and key
binding, explicitly announce, search by author and topic from the second profile, inspect the signed
catalog, fetch and verify the selected exact pin, close/reopen, and confirm the retained snapshot
opens offline. Include invalid and revoked identities and unavailable dependencies, and confirm each
refusal remains distinguishable.

That test proves the configured transport and user-visible protocol path in a controlled network. A
separate user-chosen live endpoint test would be needed before claiming public DHT reachability. It
must report exactly which endpoint and trust root were used; neither unit tests nor a loopback
fixture prove Internet availability.

## Scope guard

This increment should add no new network service and no simulated normal callback. It should not
change the store or publication disk format unless persisted trust checkpoints require a
deliberately versioned format. Do not weaken normal DHT privacy defaults to make a local fixture
pass. Keep transport configuration, identity enrollment, and content availability as separately
visible decisions and results.

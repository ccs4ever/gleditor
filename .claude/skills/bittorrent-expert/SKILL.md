---
name: bittorrent-expert
description: >-
  Guide to the BitTorrent layer of the xanalogical engine and xuzz as the code implements it: the
  libtorrent session, the four BEP 10 wire extensions, BEP 46 mutable names and topic rendezvous,
  the Merkle identity ledger with Hashcash and oracle votes, and transcopyright holes and key
  delivery. Use when designing, debugging, or implementing swarm transport, publication and
  discovery, peer authentication, identity consensus, or transcopyright.
---

# BitTorrent in the xanalogical engine

Everything here lives in `apps/common/xanadu/` and needs no graphics device. The `xudu_` prefix on
extension names and the `xudu:topic:` rendezvous prefix are protocol identifiers, not references to
a program. This file describes what the code does; the reasoning is in `design/`:

- [`bep10-wire-extensions.md`](../../../design/bep10-wire-extensions.md)
- [`bep46-publication-and-discovery.md`](../../../design/bep46-publication-and-discovery.md)
- [`oracle-identity-model.md`](../../../design/oracle-identity-model.md)
- [`permascroll-holes-and-transcopyright.md`](../../../design/permascroll-holes-and-transcopyright.md)
- [`swarm-topic-ledger-and-search.md`](../../../design/swarm-topic-ledger-and-search.md)
- [`btfs-and-permascrolls.md`](../../../design/btfs-and-permascrolls.md)

A design document can describe more than is built. Check the code before relying on a behaviour.

## Where things are

| Concern                                         | Code                                                                         |
| ----------------------------------------------- | ---------------------------------------------------------------------------- |
| libtorrent session, DHT, mutable items, topics  | `swarm.hpp` / `swarm.cpp` (`SwarmContentSource`)                             |
| Which bytes an address names, and checking them | `resolver.hpp` (`ContentSource`, `Resolver`, `VerifiedPieceCache`)           |
| Background swarms and their lifecycle           | `managed_torrent.hpp` (`SystemTorrentManager`)                               |
| Wire layouts, message types, limits             | `identity/identity_layout.hpp`                                               |
| Bencode encode and decode of every message      | `identity/identity_serialization.hpp`                                        |
| Peer and torrent plugins, the controller        | `identity/identity_network_controller.hpp`                                   |
| Ledger pipeline, oracle rules, Hashcash         | `identity/identity_validation.hpp`                                           |
| The seam where payment would be checked         | `identity/payment_verifier.hpp`                                              |
| Append-only identity ledger                     | `merkle_ledger.hpp`                                                          |
| Author catalogs and topic targets               | `author_catalog.hpp`, `publication_discovery.cpp`                            |
| Full-text search over what the swarm announced  | `swarm_catalog_index.hpp` (SQLite FTS5), `swarm_catalog.hpp`                 |
| Publish, adopt, outbox, subscriptions           | `publication.hpp`, `publication_outbox.cpp`, `publication_subscriptions.cpp` |
| Holes                                           | `scroll.hpp` (`HoleReason`), `enfilade/holefilade.cpp`                       |
| Transcopyright encryption and pricing           | `transcopyright_crypto.hpp`, `transcopyright_logic.hpp`                      |
| Verified piece and span cache                   | `lmdb_cache.hpp` (`LMDBContentCache`)                                        |

## The session

`SwarmContentSource` owns one libtorrent session and is the only thing that talks to it.

- **One socket.** Payload transfer, the wire extensions and metadata rendezvous all use the
  session's BitTorrent listener (`listenInterfaces`, default `0.0.0.0:0`). No auxiliary port is
  opened.
- **No port mapping.** UPnP and NAT-PMP are switched off in the session settings. Do not describe
  the transport as inheriting them.
- **Limits.** 128 connections for the session and 16 per torrent. `maximumPublicationTopics`
  defaults to 128. A read waits `readTimeout` (30 s) for its pieces and a magnet waits
  `metadataTimeout` (60 s) for metadata. There is no upload or download rate limit in the code.
- **Alerts.** The alert mask is narrowed to the categories the code acts on. `poll()` drains them
  and is meant to be driven by an owning background worker; torrent handle calls stay on that owner
  thread. Keep libtorrent calls and alert processing off the render thread.
- **Options for closed swarms.** `restrictDhtToDistinctNetworks` and
  `allowManyConnectionsPerAddress` exist because the network-namespace tests share a few addresses;
  leave the defaults alone for real use.
- **Metadata first.** `addMagnet(..., metadataOnly)` followed by `startDownload()` lets the host
  check a torrent's file list and resource budget before any payload is fetched.

## BEP 10 wire extensions

Four extensions are advertised in the extension handshake's `m` dictionary, with fixed local ids:

| Extension              | Local id | Message range |
| ---------------------- | -------- | ------------- |
| `xudu_identity_lookup` | 2        | `0x01`–`0x0F` |
| `xudu_oracle_vote`     | 3        | `0x10`–`0x1F` |
| `xudu_oracle_verify`   | 4        | `0x20`–`0x2F` |
| `xudu_transcopyright`  | 5        | `0x30`–`0x3F` |

A frame on the wire is a 4-byte big-endian length, the BitTorrent extended message id (20), the
remote peer's id for the extension, then the payload. The payload is one `MessageType` byte followed
by a bencoded dictionary, at most `kMaxPayloadBytes` (64 KiB). `on_extended` drops a frame whose
length disagrees with its body, and a peer that has been isolated is not listened to.

| `MessageType`             | Code   | Payload struct or purpose                                 |
| ------------------------- | ------ | --------------------------------------------------------- |
| `IdentityQuery`           | `0x01` | `IdentityQueryMsg`: a fingerprint or an email to look up  |
| `IdentityResponse`        | `0x02` | `IdentityResponseMsg`: the entry with its inclusion proof |
| `PeerAuthChallenge`       | `0x03` | `PeerChallenge`                                           |
| `PeerAuthResponse`        | `0x04` | `PeerChallengeResponse`                                   |
| `ConnectionDenial`        | `0x05` | the peer is refused                                       |
| `OracleVoteBroadcast`     | `0x10` | `VoteEntry`                                               |
| `OracleConsensusQuery`    | `0x11` |                                                           |
| `OracleConsensusResponse` | `0x12` |                                                           |
| `EmailVerifyRequest`      | `0x20` | `EmailVerifyRequestMsg`, carrying a Hashcash stamp        |
| `EmailVerifyChallengeAck` | `0x21` |                                                           |
| `EmailVerifyAttestation`  | `0x22` | `OracleAttestation`                                       |
| `TcInvoiceQuery`          | `0x30` | `TcInvoiceQueryMsg`: `keyId`, `requestedBytes`            |
| `TcInvoiceResponse`       | `0x31` | `TcInvoiceResponseMsg`: price, wallet, payment challenge  |
| `TcSettleRequest`         | `0x32` | `TcSettleRequestMsg`: the ticket and the payer's KEM key  |
| `TcKeyDelivery`           | `0x33` | `TcKeyDeliveryMsg`: the wrapped CEK, signed by the author |

Rules that hold across the layer:

- **Decoders return `std::expected`.** Every decode names its failure in `SerializationError`; a
  malformed frame is refused, never read as an empty message.
- **An entry without a proof is discarded.** An identity and its Merkle proof travel as one message
  because either alone is useless, and an entry whose proof does not check is not kept with a
  caveat.
- **A new message** needs its `MessageType` value in the right range, a layout struct with
  `isValid()`, an encode and decode pair, a case in `IdentityPeerPlugin::on_extended`, and a
  round-trip test. Extend the tables above in the same change.

## Peer authentication

A connection is challenged with a random nonce and answers with a signature from its device key. The
challenge is unpredictable, not secret. A peer that fails is isolated and disconnected, and the
controller tracks quarantined peers. `DeviceDelegation` (`user_permascroll.hpp`) is how a master
OpenPGP fingerprint hands signing authority to a device key; the controller records a delegation
only when it verifies against the master's public key.

## BEP 46 mutable names and discovery

- **A name is a public key and a salt.** `createMutableKeys()` mints an identity without asking
  anyone. `MutablePointer` is an infohash and a sequence number, and the higher sequence is the
  later answer; that is all "current" means.
- **Signing.** `mutableSigningBuffer()`, `signMutableItem()` and `verifyMutableItem()` are the only
  route. An item returned by the DHT is verified before it is recorded.
- **Publication is not done until acknowledged.** `publicationAcknowledged()` is true only after a
  DHT node acknowledged that exact signed put. A value read back from the local DHT cache is not
  completion.
- **Author catalogs** are published under the salt `catalog`. Each entry has its own salt (at most
  64 bytes, never `catalog`, in ascending order), a title, an infohash and a sequence. The catalog
  is signed; the host checks the signature.
- **Topic rendezvous.** A topic's DHT target is the SHA-1 of `xudu:topic:` followed by the canonical
  topic: one topic, at most 128 bytes, no control characters. `joinPublicationTopic()` uses the
  session's socket, with no payload torrent.

## The identity ledger

- **`MerkleLedger`** is an append-only log linking an OpenPGP fingerprint to a verified email
  address, hashed with SHA-256 into an incremental Merkle tree (merklecpp). A revocation is another
  entry, not an edit. The ledger can be sealed into a torrent and announced through a mutable name.
- **`EnginePipeline`** validates what arrives: it stages blocks, appends votes, generates and
  verifies inclusion proofs, and decides whether an oracle is authorised. A voter must be at least
  30 days old (`kMinVoterAgeSeconds`), and timestamps may be off by at most 300 s
  (`kMaxClockSkewSeconds`).
- **`HashcashEngine`** mines and verifies stamps and tracks replays. The default difficulty is 20
  bits. Costly requests, email verification among them, carry a stamp.
- **Email verification** is an oracle's job: request, challenge acknowledgement, attestation. The
  attestation confirms an SMTP challenge. There is no DKIM verification in the code.

## Holes and transcopyright

An address is a scroll and an offset, and the address space never contracts. A span that cannot be
shown is a hole with a reason (`HoleReason`): `Withheld`, `Revoked`, `Takedown`,
`TranscopyrightLock`, or `Unsealed` for local primedia not yet sealed into a torrent. Spans that
reference a hole stay valid.

- **Encryption.** ChaCha20-Poly1305 over a whole segment, span keys derived by HKDF-SHA256 from the
  segment master key, and CEKs wrapped to the reader with an X25519 KEM (a 104-byte payload).
- **One keyId, one segment.** The nonce is derived from the keyId so none has to be shipped. Reusing
  a keyId for a second segment reuses a keystream. Mint a fresh keyId per sealed span.
- **The purchase.** Invoice query, invoice response with a single-use payment challenge, settle
  request, key delivery. An invoice stands for 300 s. A challenge that was never issued for that
  keyId, or was already spent, is refused.
- **Payment is a seam, on purpose.** There is no currency, state channel or settlement system.
  `PaymentVerifier` is where one would plug in. The default is `RefusingVerifier`: a node not told
  how it is paid gives nothing away. `AlwaysAcceptVerifier` says what it is on every call. Do not
  write as though per-byte settlement happens.
- **Keys stay put.** A CEK leaves the author's machine only wrapped under a paying reader's public
  key, and the reader's private KEM key never leaves its process.

## Storage, and which arena is which

- **`LMDBContentCache`** holds verified pieces once, keyed by infohash and piece index, with
  reference counts. Two lookup tables point into them without copying: `vspans` for scroll
  coordinates and `ext_spans` for external torrent streams. A piece is evicted when its last
  reference goes.
- **`VirtualMemoryArena`** (`virtual_memory_arena.hpp`) is not swarm storage. It reserves a range of
  address space and maps file segments into it at fixed addresses, falling back to anonymous memory
  where `MAP_FIXED` is unavailable. `SegmentedOpsSpool` and `SegmentedPrimediaSpool` use it so a
  spool can grow without moving. It has nothing to do with LMDB.
- **`ArenaManifold`** is unrelated to both: it is the ephemeral cell arena of the slice model.

## Testing

- Unit and wire tests are in `tests/xudu/`: `swarm.cpp`, `torrent.cpp`, `identity_test.cpp`,
  `merkle_ledger_test.cpp`, `managed_torrent_test.cpp`, `author_catalog_test.cpp`,
  `swarm_catalog_index_test.cpp`, the `publication*` files and the three `transcopyright_*` files.
- `make test/swarm` and `make test/publication-swarm` run `tools/swarm-netns-test.sh`, which puts
  two peers on separate network stacks with `build/xudu-swarm-peer`. They need unprivileged user
  namespaces and the `veth` kernel module. `make test/publication-local` drives `build/xuzz` across
  three local processes and covers sealing and restart, not DHT discovery.
- A peer plugin can be given a frame sink in place of a connection, which is how the wire tests run
  without a network.

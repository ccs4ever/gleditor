---
name: bittorrent-expert
description: >-
  Expert protocol runbook and architectural guide for BitTorrent swarms, BEP 10 wire extensions,
  BEP 46 mutable DHT naming, Merkle identity ledgers, and Transcopyright micropayments in gleditor,
  xudu, and zigzag. Use when designing, debugging, or implementing P2P swarm transport, author discovery,
  live collaboration, identity consensus, and micropayment state channels.
---

# BitTorrent & Sovereign Swarm Protocol Architecture

This skill defines the technical specifications, packet formats, cryptographic state machines, and
performance guidelines for BitTorrent peer-to-peer protocols across the **gleditor**, **xudu**
(xanadoc editor), and **zigzag** (multidimensional visualizer) ecosystems.

```
       +-------------------------------------------------------------------------+
       |                  BitTorrent Peer Wire Transport (uTP / TCP)             |
       |                   (Zero Additional Ports, MSE/PE Encrypted)             |
       +-------------------------------------------------------------------------+
             │                    │                    │                   │
             v                    v                    v                   v
     [ BEP 10 Plugins ]    [ BEP 46 Mutable ]   [ Merkle Ledger ]   [ Transcopyright ]
     - xudu_live_op        - Author Catalogs    - PGP Fingerprints  - Per-Byte Invoicing
     - xudu_identity       - Topic Rendezvous   - DKIM Attestation  - CEK Key Delivery
     - xudu_oracle_vote    - Monotonic seq      - O(log N) Proofs   - Coordinate Holes
```

______________________________________________________________________

## 1. The Swarm as a Sovereign Transport Layer

In traditional decentralized systems, identity, collaborative editing, and micropayments are
delegated to auxiliary HTTP servers, sidecars, or blockchain RPCs. In `xudu`, **the BitTorrent peer
wire is the sole transport layer**:

1. **Zero Additional Ports**: All metadata, identity handshakes, live operational transforms, and
   payment settlement travel over the single established BitTorrent TCP/uTP socket.
1. **Unified NAT Traversal & Encryption**: All traffic inherits libtorrent's built-in hole punching,
   UPnP, NAT-PMP, and MSE/PE stream encryption.
1. **Swarm Locality**: Peers collaborating on a specific Xanadoc or quoting the same permascroll
   interact directly over the swarm carrying that content.

______________________________________________________________________

## 2. BEP 10 Wire Extension Protocol Runbook

### 2.1 Handshake Negotiation (`on_extension_handshake`)

Clients negotiate supported extensions in the standard BEP 10 extension handshake dictionary (`"m"`
mapping):

```json
{
  "m": {
    "xudu_live_op": 1,
    "xudu_identity_lookup": 2,
    "xudu_oracle_vote": 3,
    "xudu_oracle_verify": 4,
    "xudu_transcopyright": 5
  }
}
```

### 2.2 Packet Framing Format

Every extended packet follows standard 4-byte big-endian length prefix framing:

```
┌───────────────────────────────┬───────────────────────────────┐
│ Length Prefix (4 bytes, BE)   │ Total bytes following length  │
├───────────────────────────────┼───────────────────────────────┤
│ Message ID (1 byte)           │ 20 (0x14 = kBtMsgExtended)    │
├───────────────────────────────┼───────────────────────────────┤
│ Extended Message ID (1 byte)  │ Remote Extension ID (1..5)    │
├───────────────────────────────┼───────────────────────────────┤
│ Payload (Variable bytes)      │ MessageType (1B) || Bencode   │
└───────────────────────────────┴───────────────────────────────┘
```

### 2.3 Extended Envelope & Message Types (`identity_serialization.hpp`)

Payloads begin with a 1-byte `MessageType` followed by canonical Bencoded payload dictionaries:

| Message Type             | Hex Code | Payload Schema & Purpose                                                                                                        |
| :----------------------- | :------- | :------------------------------------------------------------------------------------------------------------------------------ |
| `IdentityQuery`          | `0x01`   | `{"fp": <fingerprint>, "nonce": <int64>}` — Query remote peer's PGP binding.                                                    |
| `IdentityResponse`       | `0x02`   | `{"entry": <bencoded IdentityEntry>, "proof": <merkle_audit_path>}` — Returns verified record with $O(\log N)$ inclusion proof. |
| `PeerAuthChallenge`      | `0x03`   | `{"challenge": <32-byte random bytes>, "epoch": <timestamp>}` — Mutually authenticates peer wire connection.                    |
| `PeerAuthResponse`       | `0x04`   | `{"sig": <pgp/ed25519 signature>, "device_salt": <salt>}` — Cryptographic response proving identity ownership.                  |
| `OracleVoteBroadcast`    | `0x10`   | `{"vote": <bencoded VoteEntry>}` — Gossips weighted consensus vote for Oracle quorums.                                          |
| `EmailVerifyRequest`     | `0x20`   | `{"email": <address>, "pubkey": <armored_key>}` — Submits out-of-band email attestation task.                                   |
| `EmailVerifyAttestation` | `0x22`   | `{"token": <signed_dkim_attestation>}` — Oracle signs verified email attestation token.                                         |
| `TcInvoiceQuery`         | `0x30`   | `{"span_start": <u64>, "span_len": <u64>, "scroll": <str>}` — Requests per-byte transcopyright price invoice.                   |
| `TcInvoiceResponse`      | `0x31`   | `{"invoice_id": <str>, "nano_xu_per_byte": <u64>, "total_cost": <u64>, "pay_addr": <str>}` — Author pricing terms.              |
| `TcSettleRequest`        | `0x32`   | `{"invoice_id": <str>, "receipt": <signed state channel ticket>}` — Submits micropayment settlement.                            |
| `TcKeyDelivery`          | `0x33`   | `{"invoice_id": <str>, "wrapped_cek": <AES key wrapped under reader public key>}` — Author unlocks encrypted hole.              |

### 2.4 Collaborative Live Editing (`xudu_live_op`)

Real-time collaborative typing operates with **zero raw text in live ops**:

- Transmits compact 48-byte `CompactOpNode` descriptors referencing canonical `GlobalSpan`
  addresses.
- Eliminates local spool pollution, prevents merge-conflict fragmentation, and guarantees
  convergence across distributed peers.

______________________________________________________________________

## 3. BEP 46 Mutable Naming & Topic Rendezvous

### 3.1 Permanent Content Addresses

Content addressing creates an append-only paradox: modifying or appending to a document alters the
infohash. BEP 46 solves this by introducing cryptographic public key indirection on the Mainline
DHT:

$$
\text{Permanent Docuverse Name} = (\text{Publisher Ed25519 Public Key}, \text{Salt})
$$

- $\text{Target} = \text{SHA-1}(\text{PublicKey} \parallel \text{Salt})$: The 20-byte DHT routing
  slot.
- $\text{Seq}$: Monotonically increasing 64-bit integer preventing rollback attacks.
- $\text{Sig} = \text{Ed25519\_Sign}_{\text{PrivKey}}(\text{Salt}, \text{Seq}, \text{InfoHash})$:
  Author signature over current publication root.

### 3.2 Author Catalogs (`bep46:<pubkey>/catalog`)

Authors publish their lifetime works under their root catalog (`salt="catalog"`):

- Bencoded manifest containing document titles, canonical salts (`doc:philosophy-notes`), root
  microversions, and Merkle roots.
- Downstream readers follow authors by pinning their 32-byte Ed25519 public key.

### 3.3 Deterministic DHT Topic Rendezvous Swarms

Topic discovery requires no centralized indexers:

```math
\text{Topic Rendezvous Target} = \text{SHA-1}("xudu:topic:" \parallel \text{canonicalize}(\text{topic}))
```

- Curators and readers announce signed link packages to the topic's DHT infohash.
- Peers discover relevant documents, annotations, and backlink packages directly from the topic
  swarm.

______________________________________________________________________

## 4. Decentralized Merkle Identity Ledger

Implemented via `microsoft/merklecpp` in
\[`apps/xudu/core/merkle_ledger.hpp`\](file:///data/git/gleditor/apps/xudu/core/merkle_ledger.hpp):

1. **Append-Only Tree Structure**:
   - Leaf nodes hold verified `IdentityEntry` records and `VoteEntry` consensus endorsements.
   - Internal nodes maintain SHA-256 binary hash trees.
1. **$O(\log N)$ Inclusion Proofs**:
   - Peers verify author identity authenticity in $< 1\,\text{ms}$ using lightweight Merkle audit
     paths without syncing the entire global ledger.
1. **Sybil Resistance via Dynamic Hashcash PoW**:
   - Peer registration and oracle voting require solving dynamic Hashcash proof-of-work challenges
     calibrated against network difficulty.
1. **DKIM Out-of-Band Attestation**:
   - Oracles verify domain ownership and email control via cryptographic SMTP/DKIM
     challenge-response and sign ephemeral attestation tokens.

______________________________________________________________________

## 5. Permascroll Holes & Transcopyright Economics

### 5.1 Coordinate Space Invariance

In Project Xanadu, primedia is addressed by immutable coordinates:

$$
\text{Address} = (\text{ScrollId}, \text{Offset})
$$

If an author withholds, embargos, or redacts a passage, the address space must **never contract or
collapse**:

- Withheld spans remain permanent **Holes** in the coordinate fabric.
- Downstream Edit Decision Lists (EDLs) maintain 100% stable references.

### 5.2 Transcopyright Locks (`HoleReason::TranscopyrightLock`)

- **Permissionless Quotation**: Anyone can quote or transclude any passage without prior
  negotiation.
- **Reader-to-Author Settlement**: When a reader views a document, the client pays the origin author
  directly per byte rendered.
- **In-Memory Decryption**: Content Encryption Keys (CEKs) are delivered over BEP 10
  `xudu_transcopyright` and decrypted directly into the GPU text layout buffer without leaking
  plaintext to disk.

______________________________________________________________________

## 6. Systems & 120 FPS Realist Implementation Rules

To maintain an 8.33 ms frame budget at 120 FPS:

1. **Zero Libtorrent Calls on Render Thread**:
   - `libtorrent::session`, `handle.status()`, and alert processing MUST live exclusively on a
     background worker thread.
   - Render thread (`drawFrame`) performs zero syscalls, zero mutex acquisitions, and zero heap
     allocations.
1. **64-Byte Cache-Line Aligned Atomic Telemetry**:
   - Telemetry snapshots exchanged between network and render threads must be `alignas(64)` and
     updated via lock-free atomic pointer exchange (`std::memory_order_release` /
     `std::memory_order_acquire`).
1. **Zero-Copy Memory-Mapped Storage**:
   - Inode exhaustion is prevented by writing downloaded pieces into LMDB database pages using
     `VirtualMemoryArena` (`mmap(MAP_FIXED)`).
   - Text layout reads string views directly from mapped pages without copying.
1. **Dynamic Rate Limiting**:
   - Background seeding is throttled to 512 KiB/s during active user interaction and expands to
     network capacity only when quiescent.
   - Total active peer connections capped at 128 (16 per swarm) to prevent NAT table exhaustion.

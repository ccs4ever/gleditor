# Distributed Vortex Transport Alternatives and Cross-Machine OR-Parallelism

**Status**: Architectural Study **Scope**: Follow-up study to `design/vortex-distribution.md`,
`design/vortex-multithreading-study.md` (§6.4–§6.5), and `design/vicon-andor-parallelism.md` (§1–§3)
**Worktree**: `/data/git/gleditor-restore/.worktrees/vortex-multithreading`

______________________________________________________________________

## 0. Executive Summary and the Two Open Questions

The Spool-Mesh implementation (`apps/common/xanadu/vortex/mesh_*.{hpp,cpp}`) proves that Vortex can
distribute immutable functional jobs, actor messages, promoted write-sets (`Promoted` packaged as
`ValueSeed` bundles), and hibernated actor checkpoints over Xanadu's existing BitTorrent layer
(`SwarmContentSource`, BEP 10 `xanadu_vortex_actor`, BEP 46 mutable items) without introducing a
second network daemon.

However, both the theoretical design (`design/vortex-distribution.md:214-281, 397-416`) and the
empirical lessons from bringing up the rootless swarm test suite (`MeshCheckpointNetworkTest`,
`MeshHostTest`, `MeshCliTest`) expose two foundational open questions:

1. **Viability of BitTorrent as a Message and State Transport**: Where does piggybacking actor
   control (`ActorSpawn`, `ActorSend`, `ActorMonitor`, `ActorDown`), speculative write-sets
   (`ActorDeliverWriteSet` carrying a `ValueSeed` carrier `InfoHash`), and actor migration
   (`publishMutable`) onto `libtorrent` fight the language runtime, and what transport architectures
   (Erlang/OTP 26+, Partisan/QUIC, Julia `Dagger.jl`, libp2p/IPFS, Iroh/Quinn with BLAKE3/Bao,
   Kafka/Redpanda log tailing, and SQS/NATS/ZeroMQ work queues) should be used **instead of** or
   **in addition to** BitTorrent?
1. **The Shared `choiceStack_` Table and Cross-Machine "OR-Parallelism"**: In
   `design/vortex-multithreading-study.md:638-648`, intra-machine OR-parallelism splits
   `VortexVM::choiceStack_` (`vortex_vm.hpp:151`) and `ChoicePoint::mark` (`vortex_vm.hpp:87-93`,
   `arena_manifold.hpp:170-186`) into per-worker copy-on-write `ArenaManifold` overlays over a
   shared in-RAM `const Manifold *base_`. Why does this model break across machines—and even within
   a machine at choice depth $> 1$—and how can distributed OR-parallel search, spark stealing,
   `#Cut` cancellation, and deterministic `d.submission` ordering be realized?

### Key Conclusions

- **Separate the Control/Streaming Plane from the Archival Swarm Plane**: BitTorrent (`libtorrent`)
  is an archival bulk-distribution engine, not an RPC or actor fabric. Its swarm lifecycle
  (seed-to-seed disconnection, exponential reconnect backoffs), BEP 9 `ut_metadata` round-trip
  latency (hundreds of milliseconds to seconds before reading a single byte), per-result `.torrent`
  disk sealing (`mesh_value.cpp:152-274`), single-socket TCP/uTP head-of-line blocking, and BEP 46
  DHT put/get latency (1.5–8.0 s, 1,000-byte limit) make it unsuitable as the *sole* transport for
  fine-grained actors or parallel search.
- **Recommended Hybrid Architecture**: Retain `SwarmContentSource` (`libtorrent`) for sealed
  archival publications, multi-peer permascroll seeding, `AUTHORSHIP.tsv` provenance, and
  transcopyright settlement, while adding a **direct QUIC side-channel** (Iroh/Quinn or Partisan
  style, bound to a dedicated UDP port advertised via `quic_port` in the BEP 10 handshake and
  authenticated 1:1 by the existing Ed25519 `DeviceDelegation::devicePublicKey`,
  `user_permascroll.hpp:38-45`).
- **Exploit the `SegmentedOpsSpool` $\cong$ Kafka Partition Isomorphism**: `SegmentedOpsSpool`
  (`segmented_ops_spool.hpp:39-77, 219-484`) is already a partitioned, page-aligned, append-only log
  of 64-byte `CompactOpNode` records (`compact_op.hpp:28-65`). Instead of minting three ad-hoc
  `.torrent` files on disk per `ValueSeed` (`mesh_value.cpp:203-270`, `mesh_swarm.cpp:227-243`),
  stream contiguous `CompactOpNode` slices directly from memory by monotonic `(StoreId, opIndex)`
  offset paired with inline or BLAKE3/Bao-verified slices for any newly authored primedia spans,
  falling back to 64 KiB BitTorrent Merkle pieces only when a consumer lags behind a sealed segment.
- **Replace Blind Push Retries with Credit-Windowed Pull and SQS Leases**: Replace the blind retry
  loop in `MeshHost::run` (`mesh_host.cpp:588-611`) with worker-driven credit pull (ZeroMQ/NATS
  style) and SQS-style visibility timeouts + heartbeat lease renewal on `d.frontier`.
- **Two-Tier Adaptive Distributed OR-Parallelism**:
  - For **combinatorial logic search (`vprolog`, `Vlog`)**, use **DelPhi/Derby Oracle Path
    Recomputation** (shipping a 16–64 byte branch-index vector from root to the stolen `#Choice`
    plus the frozen base `GlobalOpRef`), upgrading to **Muse Incremental Shadow-Delta Shipping**
    (exporting the arena prefix `[0..mark)` and reconstructing `overlay_`) only when a path crosses
    a heavy deterministic compute threshold (`cyclesSinceRoot >= kRecomputeCycleThreshold`).
  - For **coarse speculative macro-branches**, use **Microversion Branch Promotion (`promote()`)**
    streamed via offset tailing.
  - Coordinate all tiers over the four Vicon dimensions (`d.or`, `d.branch`, `d.frontier`,
    `d.submission`) using near-root spark stealing, hierarchical Dewey-prefix `#Cut` cancellation
    (`std::stop_token`), and a lexicographical Dewey reorder buffer on `d.submission`.

______________________________________________________________________

## 1. Critique of BitTorrent as Sole Message Transport in `mesh_swarm.cpp`

In the current codebase, all mesh communication rides on `xanadu::SwarmContentSource`
(`swarm.hpp:162-165`), multiplexing control frames over BEP 10 (`identity_layout.hpp:272-279`,
`actor_wire.cpp:76-160`), bulk values over ad-hoc `ValueSeed` torrents (`mesh_value.hpp:50`,
`mesh_value.cpp:152-274`, `mesh_swarm.cpp:152-243`), and durable actor pointers over BEP 46
(`mesh_checkpoint.cpp:143-223`).

### 1.1 Five Structural Pathologies Observed in Code and Execution

1. **The Seed-Disconnects-from-Seed Lifecycle Trap**: Under BEP 3, once two peers hold 100% of a
   torrent's pieces (such as a shared `job.program` or `job.base` in `mesh_swarm.cpp:212-224`), both
   are *seeders* with no pieces to trade. Standard BitTorrent swarm management closes redundant
   seed-to-seed connections. Even with `close_redundant_connections = false` when
   `identityController` is attached (`swarm.cpp:255-256`), simultaneous outbound connects trigger
   duplicate `peer_id` collisions and reconnect backoffs (`min_reconnect_time`, `max_failcount`,
   `swarm.cpp:260-261`), and idle swarm connections drop when piece exchange completes
   (`swarm.hpp:286-288`).
1. **Ad-Hoc `.torrent` Creation & BEP 9 `ut_metadata` Latency for Write-Sets**: Inline actor
   payloads are capped at 32 KiB (`actor_wire.cpp:80`) inside a 64 KiB BEP 10 frame
   (`kMaxPayloadBytes = 64UZ * 1024`, `identity_layout.hpp:30`). Every promoted write-set
   (`ActorDeliverWriteSet`) invokes `publishValueImpl` (`mesh_value.cpp:152-274`) to produce a
   `ValueSeed` (`mesh_value.hpp:50`), which:
   - Seals primedia chunks to disk (`userPermascroll().sealIncremental`, `mesh_value.cpp:203`),
   - Seals local ops segments to disk (`sealLocalSpool`, `mesh_value.cpp:223`),
   - Generates and writes `.torrent` files (`writeTorrentSeed`, `mesh_value.cpp:269`), and
   - Registers **three separate torrents** (`payloads`, `history`, `carrier`) in `libtorrent`
     (`mesh_swarm.cpp:227-243`). On the receiving side, `prepareTorrent` (`mesh_swarm.cpp:152-170`)
     calls `addMagnet` and blocks in `waitForMetadata(hash, options_.metadataTimeout)` (default
     60,000 ms, `mesh_swarm.hpp:28`) for a full BEP 9 `ut_metadata` exchange before downloading a
     single byte.
1. **Single-Socket Head-of-Line (HoL) Blocking and Cumulative Fragmentation**: All BEP 10 extensions
   (`xanadu_live_op` id 21, `xanadu_scroll_sealed` id 22, `xanadu_publications` id 23, identity ids
   24–27, `xanadu_vortex_actor` id 6; `swarm.cpp:596-604`, `identity_network_controller.hpp:53-64`)
   share the same TCP/uTP socket send buffer (`pc_.send_buffer()`, `swarm.cpp:673`,
   `identity_network_controller.cpp:649`) as 16 KiB BitTorrent piece blocks. Urgent control signals
   (`ActorDown`, `#Cut`, heartbeats) queue behind bulk piece transfers, and `on_extended` must
   manually wait for cumulative byte fragments across TCP segments (`swarm.cpp:685-687`,
   `identity_network_controller.cpp:174-178`) before dispatching actor messages
   (`identity_network_controller.cpp:205-232`).
1. **BEP 46 DHT Latency and the 1,000-Byte Ceiling**: `CheckpointPublisher::begin`
   (`mesh_checkpoint.cpp:161-191`) and `resolveCheckpoint` (`mesh_checkpoint.cpp:200-223`) rely on
   BEP 44/46 DHT mutable items. Because BEP 44 caps `v` at 1,000 bytes, the DHT item can hold only a
   `MutablePointer` (`InfoHash hash`, `sequence`; `swarm.hpp:45-55`), requiring a multi-second
   Kademlia put/get followed by a full magnet + BEP 9 + piece download cycle.
1. **Single Background Owner Serialization and Blind Push Drops**:
   `MeshSwarmTransport::requireOwner()` (`mesh_swarm.cpp:71-76`) forces all content operations onto
   one thread via `ownerCall` futures (`mesh_host.cpp:57-77`). Meanwhile, `MeshSwarmTransport`
   silently drops incoming actor frames when its 4 MiB / 256-event inbox (`maximumActorBytes` /
   `maximumActorEvents`, `mesh_swarm.hpp:29-30`) is full (`mesh_swarm.cpp:47-49`), and
   `MeshHost::run` (`mesh_host.cpp:588-611`) blindly re-pushes `ActorSpawn` every `options_.retry`
   without backpressure credits or visibility leases.

______________________________________________________________________

## 2. Three Domain Perspectives on Transport Alternatives

To address Pathologies 1–5 systematically, we examine how three mature distributed systems domains
solve the exact problems facing Vortex: **language runtimes** (which separate actor control from
bulk binaries), **content-addressed P2P networks** (which replace two-phase `.torrent` metadata
fetches with stream-multiplexed Merkle verification), and **log/queue brokers** (which replicate
append-only logs by offset and schedule work via heartbeated leases).

### 2.1 Language Runtime Message Passing (Erlang/OTP 26+, Partisan, Julia `Dagger.jl`)

Production distributed language runtimes strictly separate their **control plane** from their **bulk
data plane**:

- **Erlang/BEAM (Classic vs. OTP 26+ vs. Partisan)**:
  - *Classic Distributed Erlang* used one TCP connection per node pair. Sending a large binary
    stalled `net_ticktime` heartbeats and monitor signals on the same socket, causing false node
    disconnections—the exact failure mode of multiplexing `xanadu_vortex_actor` with BitTorrent
    piece transfers.
  - *OTP 26+* introduced 64 KiB message fragmentation and multi-channel DFLAGs so control signals
    and heartbeats bypass bulk term transfers. Furthermore, BEAM splits memory at a **64-byte
    threshold**: small terms copy onto the process heap, whereas binaries $> 64$ bytes live on a
    globally reference-counted heap (`ProcBin`) and pass by descriptor.
  - *Partisan* replaces Erlang's full-mesh TCP with pluggable overlays (HyParView membership +
    Plumtree broadcast) and **parallel named channels over QUIC**, isolating gossip, actor control,
    and bulk data onto independent streams with zero head-of-line blocking.
- **Julia (`Distributed.jl` and `Dagger.jl`)**:
  - `Distributed.jl` passes lightweight one-sided `Future` / `RemoteChannel` references on the
    control socket and pulls bulk array bytes only on an explicit `fetch()`.
  - `Dagger.jl` schedules a DAG of `Thunk`s by data locality: the scheduler control plane exchanges
    compact task descriptors and dependency states, while large `Chunk` payloads stream out-of-band
    directly between worker memory arenas.

### 2.2 Content-Addressed & P2P Networking (libp2p/IPFS vs. Iroh/Quinn + BLAKE3/Bao)

Evaluating modern P2P stacks against Xanadu's core primitives (`GlobalSpan`, `GlobalOpRef`, 64-byte
`CompactOpNode`, and `DeviceDelegation`):

1. **libp2p + IPFS (QUIC + Noise/TLS 1.3 + GossipSub + Bitswap/GraphSync/CARv2)**:
   - *Transport*: Native QUIC stream multiplexing eliminates TCP head-of-line blocking and resolves
     simultaneous-open races deterministically.
   - *Why Bitswap is a poor fit for `SegmentedOpsSpool`*: Standard IPFS Bitswap addresses individual
     blocks via CIDv1 (36+ bytes overhead per block) and chatty `Want-Have`/`Want-Block` protobufs.
     At 64 bytes per `CompactOpNode`, per-op CIDs add >50% metadata bloat and require sequential
     round-trips to walk causal parent chains. **CARv2 byte-range streaming** or **GraphSync**
     selectors avoid per-op wantlists, but carry heavy IPLD/protobuf framing complexity.
   - *GossipSub*: Epidemic multicast trees propagate microversion tips (`LiveOpBroadcast`) and
     OR-parallel `#Cut` tokens in $O(\log N)$ hops (< 30 ms WAN) vs. multi-second DHT polling.
1. **Iroh / Quinn Architecture (QUIC + Ed25519 `EndpointId` + BLAKE3/Bao Verified Streaming)**:
   - *1:1 Cryptographic Identity Match*: In Xanadu, every node already holds a 32-byte Ed25519
     device key signed by its OpenPGP master key (`DeviceDelegation::devicePublicKey`,
     `user_permascroll.hpp:38-45`). In Iroh/Quinn, a peer's QUIC `EndpointId` is verbatim a 32-byte
     Ed25519 public key authenticated in the TLS 1.3 handshake (RFC 7250 raw public keys). This
     collapses transport setup and `PeerChallenge`/`PeerChallengeResponse`
     (`identity_layout.hpp:498-522`) into a single 1-RTT (or 0-RTT resumed) QUIC handshake.
   - *BLAKE3 + Bao Verified Streaming vs. BEP 9 `ut_metadata`*: BLAKE3 is a SIMD-accelerated Merkle
     tree hash with 1 KiB leaf chunks. Bao streams arbitrary byte ranges
     (`GlobalSpan{scroll, start, length}`, `publication.hpp:69-82`) interleaved with their ~3%
     logarithmic proof nodes, verifying slices on the fly from the 32-byte root hash alone—**with
     zero `.torrent` file and zero BEP 9 metadata round-trip**.
   - *ALPN Stream & Datagram Multiplexing*: Dedicated QUIC streams handle `/vortex/actor/1`
     (bidirectional RPC), `/vortex/spool/1` (offset log streaming), and `/vortex/spark/1`
     (work-stealing), while RFC 9221 unreliable QUIC datagrams broadcast urgent `#Cut` cancellation
     in single-digit milliseconds.

### 2.3 Message Brokers & Log/Queue Frameworks (Kafka/Redpanda, SQS, NATS JetStream, ZeroMQ)

#### The `SegmentedOpsSpool` $\cong$ Kafka Partition Isomorphism

Xanadu's `SegmentedOpsSpool` (`segmented_ops_spool.hpp:39-138, 219-484`) is structurally identical
to a Kafka or Redpanda partition log:

| Primitive             | Kafka / Redpanda / JetStream                 | Xanadu `SegmentedOpsSpool` & `Store`                       | Code Location                                              |
| :-------------------- | :------------------------------------------- | :--------------------------------------------------------- | :--------------------------------------------------------- |
| **Atomic Record**     | `Record` batch entry                         | 64-byte `alignas(64)` `CompactOpNode`                      | `compact_op.hpp:28-65`                                     |
| **Segment File**      | Page-aligned `.log` segment                  | 64 KiB Merkle-aligned segment (1,024 ops + 64 KiB header)  | `segmented_ops_spool.hpp:53-77, 221-227`                   |
| **Partition Key**     | `(Topic, PartitionId)`                       | Logical `(StoreId, AuthorKey, Branch)` over physical spool | `store.hpp:52-78, 169`                                     |
| **Monotonic Offset**  | 64-bit `offset`                              | 1-based monotonic `opIndex` (`opCount + 1`)                | `segmented_ops_spool.hpp:107-111, 255`                     |
| **Active vs. Sealed** | Active `.log` $\to$ rolled read-only segment | Active RW segment $\to$ sealed `PROT_READ` segment         | `segmented_ops_spool.hpp:316-372`                          |
| **Log Compaction**    | Key-based changelog compaction + snapshots   | `Store::rebuildManifold()` folding + `checkpointView`      | `store.hpp:413`, `store.cpp:486`, `mesh_actor.cpp:669-694` |

Because `rawOps()` (`segmented_ops_spool.hpp:386`) exposes a contiguous virtual-memory array of
64-byte `CompactOpNode` structs, any write-set range $[O_{\text{start}}, O_{\text{end}})$ is a flat
$(O_{\text{end}} - O_{\text{start}}) \times 64$ byte slice.

**The Primedia Span Rule for Streaming `CompactOpNode` Slices**: Because `CompactOpNode`
(`compact_op.hpp:28-65`) is an Edit Decision List (EDL) record carrying
`PrimediaSpan{scrollId, spanStart, spanLength}`, a raw `CompactOpNode` slice is self-contained only
when its operations are structural/scalar (`op.value`) or reference already-shared scrolls. Whenever
a write-set mints new text/primedia bytes in the author's `localScroll`
(`userPermascroll().sealIncremental`, `mesh_value.cpp:203`) or `ArenaManifold::scratch_`
(`arena_manifold.hpp:705`), the streaming frame (`SpoolSlice`) must bundle the contiguous
`CompactOpNode` array together with an inline `ScratchSpan` byte buffer (or BLAKE3/Bao verified
stream slice for large spans). Streaming this pair directly from RAM via a Kafka-style
`Fetch(storeId, fromOpIndex, maxOps)` cursor eliminates 4–8 synchronous disk writes and three
`.torrent` registrations per result, cutting write-set transfer latency from **500–60,000 ms** to
**< 1 ms on LAN / 1 RTT on WAN**. When a lagging peer requests an offset below `activeStartIndex`,
it seamlessly falls back to fetching the sealed 64 KiB Merkle piece via BitTorrent.

#### Queue Semantics: SQS Visibility Timeouts and ZeroMQ/NATS Credit Windows

In `MeshHost::run` (`mesh_host.cpp:588-611`), tasks are statically bound to `client->grants` and
blindly re-sent every `options_.retry` until `grant.expires` (failing the entire future if one
worker stalls) or until the worker's 128-message mailbox drops frames with `ActorError::MailboxFull`
(`mesh_actor.cpp:348`). Industrial task brokers solve this via three mechanisms that map directly
onto Vortex's idempotent jobs (`jobHash(job)`, `mesh_jobs.cpp:243`) and the `d.frontier` rank
(`vicon-andor-parallelism.md:225-227`):

1. **SQS Visibility Timeout ($T_{\text{vis}}$) + Heartbeat Lease Renewal**: When worker $W_i$ claims
   a task or OR-spark, the coordinator transitions the cell on `d.frontier` from `ready` to
   `in_flight` with `leaseExpires = now + T_vis`. Periodic heartbeat pings (`ActorAck` / `ActorSend`
   frames invoking `MeshActorRuntime::heartbeat` at `mesh_actor.cpp:580`) extend the lease. If $W_i$
   stalls or disconnects, `leaseExpires` elapses and the cell returns to `d.frontier:ready` for
   another worker to steal—without failing the parent future.
1. **Dead-Letter Quarantine (`attemptCount`)**: If a poison-pill task crashes or times out above
   `maxAttempts` (e.g., 3), it moves to `d.frontier:dead_letter` and surfaces a deterministic error.
1. **ZeroMQ / NATS Credit-Based Pull Stealing**: Workers advertise available execution slots
   (`CreditUpdate{availableSlots}`). The host dispatches tasks or sparks only against positive
   worker credit, eliminating mailbox overflow.

______________________________________________________________________

### 2.4 Master Transport Comparison Matrix

| Architectural Dimension                  | Pure BitTorrent (`libtorrent` + BEP 10/46) *(Current)*              | Erlang OTP 26+ / Partisan (QUIC)                                | libp2p + IPFS (QUIC + GossipSub + CARv2)  | Iroh / Quinn (QUIC + Ed25519 + Bao)               | Kafka Log Tailing + SQS/NATS Leases                | **Recommended Vortex Hybrid** (QUIC Control/Log + BT Archival)                               |
| :--------------------------------------- | :------------------------------------------------------------------ | :-------------------------------------------------------------- | :---------------------------------------- | :------------------------------------------------ | :------------------------------------------------- | :------------------------------------------------------------------------------------------- |
| **Control vs. Bulk Split**               | Shared TCP/uTP socket; 64 KiB BEP 10 cap (`identity_layout.hpp:30`) | Fragmented multi-channel carrier sockets + 64 B `ProcBin` split | Independent QUIC streams per protocol     | Independent QUIC streams + RFC 9221 datagrams     | Separate control RPC vs. zero-copy log fetch       | **QUIC streams/datagrams for control & hot logs; BT for cold swarms**                        |
| **Head-of-Line Blocking**                | Severe (control queues behind 16 KiB piece blocks)                  | Eliminated across channels                                      | Eliminated (per-stream QUIC flow control) | Eliminated (per-stream QUIC + datagrams)          | Partition-isolated streams                         | **Eliminated** on QUIC control & streaming planes                                            |
| **Connection Lifecycle**                 | Seed-disconnects-from-seed; dup `peer_id` drops                     | Long-lived node mesh                                            | Persistent scored peer mesh               | Persistent QUIC endpoints + connection migration  | Persistent broker/peer sessions                    | **Persistent Ed25519 QUIC sessions** independent of torrent state                            |
| **Small/Med Result Latency (1–256 KiB)** | **300–60,000 ms** (`.torrent` disk seal + BEP 9 `ut_metadata`)      | **< 1 ms LAN / 1 RTT WAN**                                      | **5–30 ms** (GraphSync / CARv2 stream)    | **< 1 ms LAN / 1 RTT WAN** (inline or Bao stream) | **< 1 ms LAN / 1 RTT WAN** (in-RAM offset tailing) | **< 1 ms LAN / 1 RTT WAN** (inline $\le 16\text{ KiB}$; offset/Bao stream $> 16\text{ KiB}$) |
| **Write-Set Disk I/O on Hot Path**       | 4–8 disk writes + 3 `addTorrent` calls (`mesh_swarm.cpp:227-243`)   | Zero (in-memory send)                                           | Optional blockstore flush                 | Zero (stream from RAM with BLAKE3 tree)           | Zero (mmap page-cache tailing)                     | **Zero on hot path** (stream `rawOps()` + `ScratchSpan` from RAM)                            |
| **Span / Piece Verification**            | 64 KiB piece SHA-1 after full `.torrent` fetch                      | TLS/AEAD transport MAC only                                     | Per-block CID multihash                   | **1 KiB BLAKE3/Bao slice verified on the fly**    | CRC32C per record batch                            | **1 KiB Bao slice (hot) + 64 KiB Merkle piece (sealed segments)**                            |
| **Identity & Auth Handshake**            | Multi-step BEP 10 `PeerChallenge` (`identity_layout.hpp:498-522`)   | Shared cookie or mTLS                                           | Noise / TLS 1.3 PeerId                    | **1:1 Ed25519 TLS 1.3 raw public key (1-RTT)**    | SASL / mTLS                                        | **1:1 `DeviceDelegation::devicePublicKey` in QUIC TLS 1.3**                                  |
| **State Checkpoint / Migration**         | BEP 46 DHT (1.5–8.0 s, $\le 1000\text{ B}$ pointer)                 | Process state copy over channel                                 | DHT or direct stream                      | Direct QUIC stream + Pkarr/DHT pointer            | Compacted snapshot at offset $K$ + tail $K+1$      | **Direct QUIC snapshot at offset $K$ + op tailing; async BEP 46 backup**                     |
| **Work Scheduling & Fault Recovery**     | Blind push retry (`mesh_host.cpp:588-611`); hard grant expiry       | Process links/monitors (`{'DOWN', ...}`)                        | App-defined (transport substrate only)    | App-defined (transport substrate only)            | **Credit pull + SQS visibility timeout + DLQ**     | **Credit-windowed pull + SQS visibility lease on `d.frontier`**                              |

______________________________________________________________________

## 3. Shared `choiceStack_` Table and Cross-Machine "OR-Parallelism"

Establishing a sub-millisecond QUIC control and streaming plane (§2) removes the network latency
barrier to fine-grained coordination, allowing us to address the second open question: **how to
distribute `choiceStack_` and OR-parallel search across worker arenas and physical machines.**

### 3.1 Re-Examining the `choiceStack_` Table (`design/vortex-multithreading-study.md:638-648`)

In `design/vortex-multithreading-study.md` (§6.5, L638–648), transitioning `vprolog` and `Vlog` from
single-threaded backtracking to intra-machine multithreading required replacing four shared
structures:

| Component                     | Code Location                                      | Single-Threaded                       | Intra-Machine Multithreaded (`vicon-andor-parallelism.md`)                                    | Why Cross-Machine Distribution Breaks It                                                                                                                                                    |
| :---------------------------- | :------------------------------------------------- | :------------------------------------ | :-------------------------------------------------------------------------------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **`ArenaManifold &arena_`**   | `vortex_core.hpp:256`, `worker_context.hpp:54, 62` | One shared mutable arena              | One private CoW `ArenaManifold` per worker overlaying a shared in-RAM `const Manifold *base_` | Remote machines do not share RAM; `base_` must be mounted via `GlobalOpRef`, and parent ephemeral overlays live only in the parent machine's RAM.                                           |
| **`choiceStack_`**            | `vortex_vm.hpp:151`                                | One global `std::vector<ChoicePoint>` | Per-worker choice stack with untried alternatives exposed as stealable sparks on `d.or`       | A `ChoicePoint` (`vortex_vm.hpp:87-93`) holds raw `CellRef`s (`cursor`, `altOp`, `stackFrame`) that carry `ephemeralBit = 0x80000000` (`arena_manifold.hpp:24`).                            |
| **`ChoicePoint::mark`**       | `vortex_vm.hpp:90`, `arena_manifold.hpp:170-186`   | Global high-water `zigzag::Mark`      | High-water `Mark` in the owning worker's private `ArenaManifold`                              | `zigzag::Mark` is **12 local `uint32_t` vector lengths** (`cellCount`, `linkSize`, `shadowCount`, `trailSize`, etc.) indexing one worker's private C++ vectors—meaningless on another node. |
| **`ChoicePoint::cutBarrier`** | `vortex_vm.hpp:92`, `vortex_vm.cpp:174, 200-206`   | Index into global `choiceStack_`      | Index into local stack + structural prune on `d.or` / `d.frontier` with `std::stop_token`     | A local stack index cannot cancel subtrees stolen by remote machines without a network-wide epoch/Dewey-prefix cancellation protocol.                                                       |

### 3.2 The Depth $> 1$ Binding Hazard (Even Intra-Machine!)

There is a subtle architectural constraint in `design/vicon-andor-parallelism.md` (§1.2, L68–80)
that becomes critical when stealing choice points at depth $> 1$:

- When `Vlog::bind(v, t)` executes (`vlog.cpp:86-94`), it links the `d.clone` rank of `t` and `v`.
- If `v` is a cell in the frozen `base_`, `ArenaManifold::shadow(v)`
  (`arena_manifold.cpp:1464-1503`) copies `v`'s `CellSlot` into the parent worker's private `slots_`
  vector, records `v` in `shadowOrder_` and `overlay_`, and splices the `d.clone` edge in the
  parent's `links_` vector. If the parent also constructed compound terms via `Vlog::makeTerm`
  (`vlog.cpp:34-62`), those cells carry `ephemeralBit` in `slots_[0 .. mark.cellCount)`.
- **Consequence**: Giving a thief worker a *blank* `ArenaManifold` over `base_` only reproduces the
  correct environment at **root choice depth 0** (before any parent bindings exist). At choice depth
  $d \ge 1$, a blank overlay over `base_` **loses every variable binding and term minted by the
  parent branch between the root goal and the stolen `#Choice`!**
- Empirical confirmation in the codebase: `MeshActorRuntime::checkpointView`
  (`mesh_actor.cpp:672-676`) already enforces this invariant by returning `ActorError::NotQuiescent`
  whenever `choiceDepth() != 0` or `outstandingMarks() != 0`, proving that raw active choice points
  cannot be exported as ordinary actor snapshots.

______________________________________________________________________

### 3.3 Four Models for Distributed OR-Parallelism in Vortex

To distribute OR-parallel search across machines (and across isolated worker arenas at depth
$d \ge 1$) while preserving the share-nothing single-writer law, we evaluate four concrete models:

```text
Model 1: Oracle Path Recomputation (DelPhi / Derby)
  Node A ──[ (Base GlobalOpRef, RootGoal, OraclePath=[0,2,1]) (24 B) ]──> Node B
  Node B mounts Base, replays path [0,2,1] in ~20 us, then explores subtree locally.

Model 2: Incremental Trail / Shadow-Delta Shipping (Distributed Muse)
  Node A ──[ (Base GlobalOpRef, altOp, slots_[0..cellCount], shadowOrder_[0..shadowCount]) ]──> Node B
  Node B hydrates private ArenaManifold overlay directly at Mark and jumps to altOp.

Model 3: Microversion Branch Promotion (Promoted Store Branch)
  Node A runs promote() at #Choice ──[ Streams CompactOpNode delta ]──> Node B
  Node B folds promoted microversion as its new frozen base_ and starts with a clean arena.

Model 4: Distributed Vicon Coordination (d.or, d.branch, d.frontier, d.submission, #Cut)
  Near-root credit pull + Dewey-prefix #Cut datagrams + d.submission lexicographical reorder buffer.
```

#### Model 1: Recomputation via Oracle Bitstrings / Path Strings (DelPhi / Derby Prologs)

- **Mechanism**: Never serialize `ArenaManifold` vectors, `zigzag::Mark`, or `ephemeralBit` cells.
  Every choice point at depth $d$ is uniquely identified by its **Dewey / Oracle Path** from the
  root goal:

  ```math
  \text{OracleSpark} = \langle \text{programRef}, \text{baseRef}, \text{rootGoal}, \text{path} = [c_0, c_1, \dots, c_{d-1}, \text{altIdx}_d] \rangle
  ```

  When Node B steals an `OracleSpark`, it constructs a fresh `ArenaManifold` over the immutable
  `baseRef` (already locally cached) and runs `rootGoal` from the beginning. For the first $d$
  `OpcodeKind::Choice` instructions (`vortex_vm.cpp:881-889`), the VM follows branch $c_i$
  deterministically *without* pushing backtrack frames onto `choiceStack_`. At depth $d$, it takes
  branch $\text{altIdx}_d$ and switches to normal local backtracking execution.

- **Why It Fits Vortex**:

  - **Wire size is $O(d)$ bytes** (typically 16–64 bytes total), easily fitting in a single inline
    control packet.
  - **Zero `ephemeralBit` relocation**: Node B mints its own ephemeral cells and `d.clone` bindings
    deterministically during the fast-forward replay.
  - **Solves depth $> 1$ intra-machine stealing too**: Even on the same machine, an idle thread can
    replay a 16-byte oracle path over the shared `const Manifold *base_` in a few microseconds
    without locking or copying the parent thread's `ArenaManifold`!

- **Boundary Condition**: Ideal when the deterministic prefix cost $T_{\text{recompute}}(d)$ is
  below a cycle threshold (`cyclesSinceRoot < kRecomputeCycleThreshold`, defined in Model 4 below).
  Degrades if the prefix between root and depth $d$ executes an expensive VQL scan or VPL array
  reduction.

#### Model 2: Incremental Trail / Shadow-Delta Shipping (Distributed Muse / Kabu-Wake)

- **Mechanism**: When the deterministic prefix before `#Choice` exceeds `kRecomputeCycleThreshold`,
  Node A exports the exact prefix of its `ArenaManifold` bounded by `ChoicePoint::mark`
  (`vortex_vm.hpp:90`, `arena_manifold.hpp:170-186`):
  1. `slots_[0 .. mark.cellCount)` (which holds both newly minted ephemeral cells and the shadowed
     `CellSlot` copies of base cells, where `slots_[dense].birthOp` records the original base
     `CellRef`, `arena_manifold.cpp:1488`),
  1. `shadowOrder_[0 .. mark.shadowCount)` (recording which base `CellRef`s were shadowed,
     `arena_manifold.cpp:1501`),
  1. `links_[0 .. mark.linkSize)`, `content_[0 .. mark.contentSize)`, and
     `scratch_.bytes()[0 .. mark.scratchSize)`, plus `mark.liveLinks` and `mark.liveContent`, and
  1. The cursor continuation (`altOp`, `stackFrame`, `cursorPC_` at `vortex_vm.hpp:150`, and any
     dynamically minted `arenaDims_` at `arena_manifold.hpp:713` or federation/quote prefixes up to
     `mark.proxyCount` and `mark.quoteOccurrenceCount`).
- **Hydration Invariants on Node B**: Because `ArenaManifold` stores cells, links, and scratch
  content in flat, contiguous CSR vectors (`arena_manifold.hpp:701-715`) using dense indices
  (`ephemeralBit | dense`), copying the prefix vectors `[0 .. mark.*)` verbatim into a fresh
  `ArenaManifold` over the **same** `base_` preserves every `ephemeralBit | dense` reference without
  pointer relocation. To satisfy `ArenaManifold`'s internal invariants, Node B must also:
  - **Reconstruct `overlay_`** (`std::unordered_map<CellRef, std::uint32_t>`,
    `arena_manifold.hpp:715`) by scanning `dense` in `0 .. mark.cellCount` and inserting
    `overlay_[slots_[dense].birthOp] = dense` whenever `!isEphemeral(slots_[dense].birthOp)`, so
    `ArenaManifold::denseOf(ref)` (`arena_manifold.cpp:258-270`) resolves shadowed base variables
    instead of falling through to `base_`;
  - **Initialize `liveLinks_ = mark.liveLinks` and `liveContent_ = mark.liveContent`**
    (`arena_manifold.hpp:720-721`) so the compaction heuristic (`arena_manifold.cpp:814, 1288`) does
    not misfire; and
  - **Push a split floor onto `floors_`** (`arena_manifold.hpp:722-736`) so subsequent child choice
    points trail modifications to imported slots (`dense < mark.cellCount`,
    `arena_manifold.cpp:1505-1524`) while preventing Node B from backtracking above the stolen split
    point.
- **Intra-Machine Concurrency Caveat**: Because `ArenaManifold` contains no internal locks, a thief
  thread on the same machine cannot `memcpy` directly from a running parent worker's vectors; either
  the parent snapshots the prefix when publishing a heavy spark or the copy occurs while the parent
  is paused at a cooperative scheduler yield.

#### Model 3: Microversion Branch Promotion (`promote()` Store Branching)

- **Mechanism**: At coarse speculative split points, Node A calls `promote()`
  (`arena_manifold.hpp:801-804`) up to the choice point, translating ephemeral cells into durable
  `CompactOpNode`s on a lightweight branch `MicroversionId` (`Promoted`, `arena_manifold.hpp:237`)
  and streaming those ops (plus any new primedia bytes) via offset log tailing (§2.3) to Node B.
  Node B folds the branch microversion into a frozen `Manifold` and starts with an empty
  `ArenaManifold`.
- **Trade-off**: 100% hypertime-traceable (`GlobalOpRef`), so every speculative branch can be
  inspected in the hypertime graph (`hypertime_graph.cpp`). Higher CPU/memory cost than Models 1–2,
  making it the right choice for coarse macro-speculation rather than tight inner-loop WAM search.

#### Model 4: Distributed Coordination (`d.or`, `d.branch`, `d.frontier`, `d.submission`, `#Cut`)

Regardless of whether a spark's environment is transferred via Model 1 (Oracle), Model 2 (Shadow
Delta), or Model 3 (`Promoted` Branch), cluster-wide coordination follows four rules mapped onto the
host's coordination arena (`vicon-andor-parallelism.md:212-270`):

1. **Near-Root Credit-Driven Spark Stealing and `d.branch` Binding**: Remote workers steal only near
   the root of the `d.or` tree (depth $d \le K_{\text{root}}$, where subtree work $\gg$ RTT) using
   explicit worker credit windows (`CreditUpdate`) and SQS visibility timeouts (`leaseExpires`) on
   `d.frontier`. When a spark is leased to a worker, the host links the spark cell on `d.branch`
   (`vicon-andor-parallelism.md:222-224`) to the assigned worker's `ActorPid` / proxy cell.
1. **Adaptive Oracle-vs-Delta Switching**: Each `ChoicePoint` (`vortex_vm.hpp:87-93`) is augmented
   to record the cumulative instruction cycle count `cyclesSinceRoot` (proposed in Stage 3 below,
   mirroring the cycle counter in `VortexVM::run` at `vortex_vm.hpp:126` / `vortex_vm.cpp:1052`). If
   `cyclesSinceRoot < kRecomputeCycleThreshold` (e.g., 10,000 cycles $\approx 50\ \mu\text{s}$), the
   spark ships as a **Model 1 Oracle Path** ($\le 64$ bytes); otherwise it ships as a **Model 2
   Shadow Delta** (or **Model 3 Promoted Branch** if cross-store).
1. **Hierarchical Dewey-Prefix `#Cut` Cancellation**: Every spark carries its lexicographical Dewey
   tree coordinate (e.g., `0.2.1`). When any branch executes `OpcodeKind::Cut` (`vortex_vm.cpp:902`,
   invoking `VortexVM::cut` at `vortex_vm.cpp:200-206`) at barrier depth $b$, the coordinator marks
   posward siblings on `d.or` as `cut` on `d.frontier` and broadcasts a compact cancellation frame
   (`CutPrune{jobHash, deweyPrefix, barrierDepth}`) via QUIC datagram or pub-sub topic
   `vortex.job.<jobHash>.cut`. Any worker whose active Dewey coordinate is a right-sibling under
   that prefix immediately fires `stop_source.request_stop()`, halting at the next `stepScheduler()`
   boundary (`vortex_vm.hpp:127`, `vortex_vm.cpp:1079-1110`) and dropping its `ArenaManifold` in
   $O(1)$ time with zero trail unwinding.
1. **Deterministic `d.submission` Reorder Buffer**: To preserve observable left-to-right
   Prolog/Vicon solution order across variable-latency machines, remote solutions
   (`ActorDeliverValue`) carry their Dewey coordinate and insert into the host's `d.submission`
   rank. The consumer cursor drains solutions strictly in lexicographical Dewey order, holding
   right-branch solutions until all left-sibling branches on `d.or` have completed or failed.

______________________________________________________________________

### 3.4 Distributed OR-Parallelism Models Comparison Table

| Criterion                                      | Model 1: Oracle Path Recomputation (DelPhi/Derby)                                  | Model 2: Incremental Shadow-Delta (Distributed Muse)                                    | Model 3: Microversion Promotion (`promote()`)                         | Hybrid Adaptive Policy (Model 1 + Model 2 + Model 4)                   |
| :--------------------------------------------- | :--------------------------------------------------------------------------------- | :-------------------------------------------------------------------------------------- | :-------------------------------------------------------------------- | :--------------------------------------------------------------------- |
| **Wire Payload per Stolen Spark**              | **16–64 bytes** (`[c_0, ..., c_d]`)                                                | **4–64 KiB** (`slots_`, `shadowOrder_`, `links_` prefix)                                | **4–256 KiB** (`CompactOpNode` + `ScratchSpan` slice)                 | **16–64 B** when `cycles < 10k`; **4–64 KiB** when `cycles >= 10k`     |
| **Sender Steal Export Cost**                   | **$O(d)$** ($\sim 50\text{ ns}$, zero arena copy)                                  | **$O(\text{mark})$** ($\sim 5\text{–}50\ \mu\text{s}$ CSR slice export)                 | **$O(\text{promote})$** ($\sim 0.5\text{–}5\text{ ms}$ op minting)    | **$O(d)$** for fast prefixes; **$O(\text{mark})$** after heavy compute |
| **Receiver Startup Cost**                      | **$O(T_{\text{recompute}}(d))$** ($\sim 10\text{–}50\ \mu\text{s}$ for pure logic) | **$O(\text{mark})$** ($\sim 5\text{–}50\ \mu\text{s}$ vector copy + `overlay_` rebuild) | **$O(\text{fold})$** ($\sim 0.5\text{–}5\text{ ms}$ manifold advance) | Bounded at **$\le 50\ \mu\text{s}$** across both regimes               |
| **`ephemeralBit` Safety**                      | **Absolute** (no `CellRef`s cross the wire)                                        | **Exact** (prefix `[0..mark)` + `overlay_` rebuild keeps identical dense indices)       | **Absolute** (`promote()` translates all refs to durable op indices)  | **Exact / Absolute**                                                   |
| **Solves Depth $> 1$ Intra-Machine Stealing?** | **Yes** (replays parent bindings on thief's private arena)                         | **Yes** (prefix snapshot exported at spark creation or cooperative yield)               | **Yes** (mounts promoted branch)                                      | **Yes** (unifies intra-machine and cross-machine spark stealing!)      |
| **Heavy Prefix Compute Resilience**            | Poor (re-executes expensive prefix)                                                | **Excellent** (preserves all computed bindings/terms)                                   | **Excellent** (persists computed state in hypertime)                  | **Excellent** (switches to Model 2/3 when `cyclesSinceRoot` is high)   |
| **Distributed `#Cut` Reclamation**             | `stop_token` + drop local arena ($O(1)$)                                           | `stop_token` + drop local arena ($O(1)$)                                                | `stop_token` + abandon branch                                         | **Dewey-prefix datagram + `stop_token` + $O(1)$ arena drop**           |
| **Observable Solution Order**                  | Lexicographical via `d.submission`                                                 | Lexicographical via `d.submission`                                                      | Lexicographical via `d.submission`                                    | **Strict serial equivalence via `d.submission` Dewey reorder buffer**  |

______________________________________________________________________

## 4. Concrete Architectural Roadmap and C++ vs. Vortex Boundary

### 4.1 Three-Stage Implementation Roadmap

1. **Stage 1 — Streaming Spool Offset Replication & SQS Leases (Over Existing Transport)**:
   - Extend `LiveOpBroadcast` / `xanadu_vortex_actor` with a `SpoolSlice` frame carrying contiguous
     64-byte `CompactOpNode` arrays directly from `SegmentedOpsSpool::rawOps()`
     (`segmented_ops_spool.hpp:386`) paired with an inline `ScratchSpan` payload buffer for any
     newly authored primedia bytes, bypassing `makeTorrent` and disk sealing
     (`mesh_value.cpp:203-270`) on the hot path for write-sets $\le 64\text{ KiB}$.
   - Replace the blind retry loop in `MeshHost::run` (`mesh_host.cpp:588-611`) with worker credit
     grants and SQS-style visibility timeouts (`leaseExpires` renewed via runtime heartbeats,
     `mesh_actor.cpp:580`) on `d.frontier`.
1. **Stage 2 — Direct QUIC + Ed25519 Side-Channel (`MeshQuicTransport`)**:
   - Bind a dedicated UDP socket (`quicPort`) for `MeshQuicTransport` (avoiding kernel UDP packet
     demuxing conflicts with `libtorrent`'s uTP/DHT socket in `swarm.hpp:110`), advertise
     `quic_port` in `XanaduPeerPlugin::add_handshake` (`swarm.cpp:616-627`), and authenticate direct
     QUIC sessions in 1-RTT using the peer's existing 32-byte Ed25519
     `DeviceDelegation::devicePublicKey` (`user_permascroll.hpp:38-45`).
   - Route `ActorSpawn`, `ActorSend`, `ActorDown`, `#Cut` datagrams, and BLAKE3/Bao verified
     `GlobalSpan` / `SegmentedOpsSpool` streams over independent QUIC streams, eliminating TCP
     head-of-line blocking, 64 KiB framing limits, and seed-to-seed disconnects while keeping
     `SwarmContentSource` (`libtorrent`) for archival torrents and transcopyright.
1. **Stage 3 — Adaptive Oracle + Shadow-Delta OR-Parallel Scheduler**:
   - Add Dewey coordinate tracking and `cyclesSinceRoot` to `ChoicePoint` (`vortex_vm.hpp:87-93`).
   - Implement **Model 1 (Oracle Path Recomputation)** and **Model 2 (Arena Prefix Hydration with
     `overlay_` reconstruction)** for both intra-machine depth $> 1$ spark stealing and
     cross-machine OR-parallelism, coordinated by `d.or`, `d.branch`, `d.frontier` visibility
     leases, Dewey-prefix `#Cut` cancellation, and the `d.submission` reorder buffer.

### 4.2 Why the Transport & VM Substrate Is C++ while Coordination Policy Is Vortex

In accordance with `GEMINI.md` (*"New C++ code must justify why it isn't being written in Vortex"*),
the boundary between C++23 substrate and Vortex standard-library code is strictly drawn:

- **C++23 Substrate (Hardware, OS, and Interpreter Invariants)**: `SegmentedOpsSpool` virtual memory
  paging (`rawOps()`, `PROT_READ` sealing), QUIC UDP socket multiplexing and TLS 1.3 RFC 7250
  Ed25519 handshakes, SIMD BLAKE3/Bao tree verification, and `ArenaManifold` CSR vector slicing /
  `overlay_` hydration operate on raw OS descriptors and cache-line-aligned (`alignas(64)`) memory
  buffers below the cell-and-rank abstraction of Vortex bytecode.
- **Vortex Standard Library (Scheduling Policy & Coordination Ranks)**: Because the C++ substrate
  projects spark queues, worker assignments, visibility leases, and solution streams onto the four
  Vicon dimensions (`d.or`, `d.branch`, `d.frontier`, `d.submission`), high-level work-stealing
  heuristics, credit-window allocation policies, and custom search combinators can be authored
  directly in Vortex over existing standard-library rank traversal functions.

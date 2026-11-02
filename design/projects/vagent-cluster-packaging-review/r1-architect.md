# Round 1 critique: System Architect (vagent-cluster-packaging.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 3 MAJOR, 4 MINOR.

## BLOCKING

None.

## MAJOR

### 1. (§8.1, §8.3) Verification and compilation stalls transport owner thread

**Problem:** §8.1 (lines 557, 573–575) and §8.3 (lines 608–634) assign streaming Merkle leaf
verification, CAS whole-file SHA-256 hashing (up to `max_package_bytes` = 64 MiB), and
`CompiledProgram::fromUnit()` AST checking/compilation directly to the single `MeshSwarmTransport`
owner thread (`NB_TR`). Running CPU-intensive and disk-bound verification on the network thread
stalls all other swarm network polling, QUIC stream multiplexing, and actor message delivery.

**Evidence:**

- In `apps/common/xanadu/vortex/mesh_host.hpp:11–12`, the core transport contract mandates: *"All
  methods except source() belong to one background network owner. Peer callbacks supply
  authenticated events and never prepare metadata or compute."*
- In `apps/common/xanadu/vortex/mesh_swarm.hpp:103, :122`, `requireOwner()` enforces that network
  polling (`pollDirectChannel()`, ZeroMQ/QUIC heartbeats, discovery) runs on this single thread.
- If `NB_TR` blocks to compute SHA-256 over a 32–64 MiB bundled library or execute the full
  front-end actor compiler (`compiler.cpp` AST lowering), the transport cannot poll sockets. Remote
  peers encounter QUIC packet buffer overruns and heartbeat timeouts, and incoming actor messages in
  the transport inbox stall.

**Fix:** Decouple transport event polling from package verification and compilation. The transport
owner thread must only enqueue the pending fetch and initiate downloads. Multi-megabyte SHA-256
hashing, CAS atomic commits, and `fromUnit` compilation must be executed on a background worker
thread (or worker context pool). Upon completion, a completion event must be posted back to the
runtime/owner thread to register `retainProgram` and re-inject waiting actor events.

______________________________________________________________________

### 2. (§6.2, §13, §16) Undefined CAS retention quota, pinning, and durability

**Problem:** §16 (lines 903–904) claims *"Disk usage is bounded by node operator storage quotas and
purged via LRU unpinned blob sweeping."* However:

1. §13 (Settings) and P21.7 omit any storage quota setting (e.g. `vagent.cluster.cas_max_bytes`),
   violating House Rule 4 (*"No arbitrary ceilings: a required bound is priced and exposed as a
   setting or grant limit"*).
1. No pinning or reference-counting protocol is defined in `package_cas.{hpp,cpp}`. If an unpinned
   LRU sweeper runs when storage is near quota, it can evict blobs belonging to currently running
   agents. If a guest host crashes and must be restarted by `NativeHostSupervisor` (P20 §12.1), the
   supervisor cannot re-stage the missing blob from CAS, causing permanent agent failure.
1. §6.2 writes temporary files `<cas_dir>/tmp/<uuid>.part` and renames them to `<cas_dir>/<sha256>`
   without calling `fsync()` on the file or the directory, risking 0-byte or corrupted CAS entries
   on node crashes. Furthermore, no cleanup policy or startup sweep is specified for abandoned
   `.part` files in `tmp/`.

**Evidence:**

- Grepping `vagent.cluster.` in §13 reveals no setting bounding total CAS disk footprint.
- Section 6 defines no `pin(sha256)` / `unpin(sha256)` API, nor does it specify how `AgentExecutor`
  or `NativeHostSupervisor` retain claims on active blobs.
- Section 6.2 lines 401–406 detail temporary file spooling and `renameat` without fsync durability
  or recovery for orphaned `.part` files after timeout or restart.

**Fix:**

1. Add `vagent.cluster.cas_max_bytes` (default 10 GiB) and `vagent.cluster.cas_target_bytes`
   (default 8 GiB) to §13 and P21.7.
1. Define a pinning reference-counting mechanism in `package_cas`: blobs are pinned by active
   programs retained in `AgentExecutor` with live agents, active staging directories in
   `NativeHostSupervisor`, and in-flight `PendingProgramFetch` entries. Eviction sweeps only
   unpinned blobs with access times older than a watermark.
1. In §6.2, mandate `fsync()` on `<uuid>.part` prior to `renameat`, followed by a directory `fsync`
   on `<cas_dir>/`. Specify an epoch startup sweep of `<cas_dir>/tmp/` to unlink orphaned `.part`
   files left by crashed processes.

______________________________________________________________________

### 3. (§8.3, §11.2, P21.1) Layering conflict on PendingProgramFetch ownership

**Problem:** There is an architectural contradiction between §8 and §11/P21.1 regarding the
ownership of `PendingProgramFetch` and the coupling of Vortex `MeshActorRuntime`:

- In §8.1 (diagram lines 557, 566, 577) and §8.3, `PendingProgramFetch` is owned by the network
  transport boundary (`NB_TR`), holding incoming `AuthenticatedActorEvent`s outside the actor
  runtime until the program is fetched, compiled, and registered.
- In §11.2 (lines 771–773) and P21.1 (line 789), the queue is placed inside `MeshActorRuntime` via a
  proposed `AsyncProgramResolver` callback hook in `vortex/mesh_actor.{hpp,cpp}`.

**Evidence:** `MeshActorRuntime` (`apps/common/xanadu/vortex/mesh_actor.hpp`) is the generic,
language-agnostic distributed actor runtime in Vortex. Embedding `PendingProgramFetch`—with its
`.vnpkg` bencode schemas, `xanadu::InfoHash` torrent dependencies, and cluster fetch states—directly
into `MeshActorRuntime` breaks the dependency hierarchy (`AGENTS.md`). Furthermore, if network
transport admission buffers events until `retainProgram` succeeds (as specified in §8.1/§8.3),
`MeshActorRuntime` requires no internal pending queue or resolver modification.

**Fix:** Reconcile §8 and §11/P21.1: Keep `PendingProgramFetch` strictly in
`apps/common/xanadu/vagent/cluster/pending_fetch.{hpp,cpp}` at the transport and cluster supervisor
layer. Update P21.1 and §11.2: do not inject packaging-specific fetch queues into
`vortex/mesh_actor.{hpp,cpp}`. If `MeshActorRuntime` requires an asynchronous program resolver hook
for local/in-engine spawns, keep the signature minimal and generic (e.g. returning a pending token
or future) without coupling Vortex core to cluster packaging structures.

## MINOR

1. (§11.1) **DRY reuse of `apps/common/xanadu/bencode.hpp`:** §11.1 proposes `package_format.cpp` as
   a "canonical bencode encoder/decoder". The repo already has a fully-tested, canonical BEP-3
   implementation in `apps/common/xanadu/bencode.hpp`. `package_format` should be a typed schema
   mapper for `VnpkgManifest` over `xanadu::bencode::Value`, avoiding duplicated bencode parsing.
1. (§8.1, §8.2) **Visibility of `prepareTorrent` in `mesh_swarm.hpp`:** §8.1 line 571 and §8.2 line
   599 cite calling `prepareTorrent(infoHash, maxBytes)` (`mesh_swarm.hpp:113`). However,
   `prepareTorrent` is currently a private helper in `MeshSwarmTransport`. P21.5 should explicitly
   propose exposing a public torrent slice preparation method on `MeshHostTransport` and
   `MeshSwarmTransport`.
1. (§4 K8, §6.2, §7.3) **Hard-link file permission synchronization:** §6.2 sets CAS blobs to mode
   `0400` (read-only), while §7.3 stages bundled libraries with mode `0500` (read and execute).
   Because hard links share the same underlying filesystem inode and permission bits, invoking
   `chmod(0500)` on a staged hard-link mutates the CAS blob's permissions as well. The specification
   should standardize on mode `0500` for CAS binary blobs or document that staging must account for
   shared inode permission semantics.
1. (§7.1) **Mach-O `@executable_path` handling:** §7.1 validates `@loader_path` and `@rpath` for
   Mach-O binaries but does not mention `@executable_path`. Standalone native helpers or tools
   sometimes link against `@executable_path/../lib`. The validator in `package_inspector` should
   explicitly reject or define policy for `@executable_path`.

VERDICT: NOT CONVERGED

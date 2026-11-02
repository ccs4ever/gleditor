# Round 2 critique: System Architect (vagent-cluster-packaging.md v0.3)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 2 MINOR.

## Status of previous BLOCKING/MAJOR findings

- **R1.M1 (§8.1, §8.3 — Transport thread stalling during verification/compilation):** RESOLVED. In
  §4 K5 (lines 280–288), §8.1 (diagram lines 681, 693–702), §8.2 (lines 724, 730–734), and §8.3
  (lines 774–777), all streaming SHA-256 and Merkle verification, atomic CAS commits (`fsync` +
  `renameat`), `package_inspector` validation, and `CompiledProgram::fromUnit` compilation are
  explicitly offloaded to a dedicated background package worker pool (`NB_WK`). The
  `MeshSwarmTransport` owner thread (`NB_TR`) remains strictly non-blocking per
  `mesh_host.hpp:11–12`. When compilation finishes, a completion event is dispatched back to the
  runtime/owner thread to register `retainProgram` and invoke `completePendingProgram`.
- **R1.M2 (§6.2, §13, §16 — CAS retention quota, pinning, durability, and cleanup):** RESOLVED.
  Section 4 K8 (lines 327–345), §6.2 (lines 502–521), §13 (line 991), and P21.7 explicitly define
  `vagent.cluster.cas_max_bytes` (default 10 GiB) bounding total unpinned CAS storage.
  Reference-counted pinning (`pin(sha256)` / `unpin(sha256)`) protects blobs in active use by
  running agents (`AgentExecutor`), staged hosts (`NativeHostSupervisor`), and in-flight fetch tasks
  so LRU eviction sweeps only unpinned blobs. CAS spooling enforces `0600` flags with `O_NOFOLLOW`,
  `fsync()` on the part file, atomic `renameat`, directory `fsync()`, and an epoch startup sweep
  unlinking orphaned `.part` files in `<cas_dir>/tmp/`.
- **R1.M3 (§8.3, §11.2, P21.1 — Layering conflict on `PendingProgramFetch`):** RESOLVED. Section 4
  K5 (lines 289–292), §8.3 (lines 740–753), §11.1, and P21.1 isolate `PendingProgramFetch` strictly
  within `apps/common/xanadu/vagent/cluster/pending_fetch.{hpp,cpp}` at the cluster supervisor
  layer. `MeshActorRuntime` (`vortex/mesh_actor.{hpp,cpp}`) is limited to a minimal,
  language-agnostic admission deferral protocol (`ActorError::ProgramPending`, pre-admission mailbox
  buffering up to `maxMailboxBytes`, lease wait crediting, upgrade suspension, and
  `completePendingProgram`), preserving Vortex core layer purity (`AGENTS.md`).

## Status of previous MINOR findings

- **R1.m1 (§11.1 — DRY reuse of `bencode.hpp`):** RESOLVED. §11.1 and P21.2 specify that
  `package_format.cpp` reuses the canonical BEP-3 implementation in
  `apps/common/xanadu/bencode.hpp`.
- **R1.m2 (§8.1, §8.2 — Public `prepareTorrent` visibility):** RESOLVED. P21.5 and §8.2 define
  public `preparePackageBlob(hash, maxBytes, destCasPath)` on `MeshHostTransport` and
  `MeshSwarmTransport`.
- **R1.m3 (§4 K8, §7.3 — Hard-link inode permission mutation):** RESOLVED. Section 4 K8 and §7.3
  forbid raw POSIX `link()` between CAS (`0400`) and staging (`0500`), mandating CoW reflink
  (`FICLONE` / `clonefile`) or streaming copy with verified SHA-256.
- **R1.m4 (§7.1 — Mach-O `@executable_path` rejection):** RESOLVED. Section 4 K4, §7.1, and §14.2
  explicitly reject `@executable_path` with `InsecureRpath`.

## Audit of v0.3 architectural changes

- **`PackageBundle` transport envelope (§8.1–8.2, P21.5):** Cleanly replaces the problematic
  `SpoolSliceBundle` reuse with a dedicated bencoded container demultiplexed via a 1-byte frame tag
  (`0x02`) on QUIC stream Lane 1. Maintains protocol separation without mutating `Store` slice
  transports.
- **Cross-platform `canonical_soname` mapping (§4 K3, §5.2, §8.4):** Decouples logical `programRef`
  from OS-specific library naming conventions. `targets[<triple>]` maps canonical op-log sonames to
  platform-staged libraries and system aliases without mutating the abstract AST or invalidating
  multi-target signatures.
- **Pre-admission mailbox buffering & lease extension (§4 K5, §8.3):** Resolves race conditions
  where early messages to pending PIDs were dropped with `UnknownProcess` and prevents premature
  lease expiration during large swarm pulls.

## BLOCKING

None.

## MAJOR

None.

## MINOR

1. (§8.3 step 7, §11.2) **Thread context for `completePendingProgram`:** Note explicitly that
   completion callbacks dispatched from the background worker pool must be marshaled onto the
   runtime thread driving `MeshActorRuntime` (the thread invoking `pumpOne` and `receive`) before
   calling `completePendingProgram`, ensuring thread-safety of the internal actor tables.
1. (§13) **Low watermark for CAS eviction:** Consider adding an optional
   `vagent.cluster.cas_target_bytes` setting (default 8 GiB, or 80% of `cas_max_bytes`) so LRU
   eviction sweeps down to a low watermark rather than running repeatedly on every incoming byte
   past the maximum cap.

VERDICT: CONVERGED

# Round 1 disposition (`design/vagent-cluster-packaging.md` v0.2 → v0.3)

Tally across all seven Round 1 critics:

| Critic      | BLOCKING | MAJOR | MINOR | Verdict       |
| ----------- | -------- | ----- | ----- | ------------- |
| `writer`    | 0        | 0     | 0     | CONVERGED     |
| `vortex`    | 2        | 1     | 3     | NOT CONVERGED |
| `codebase`  | 1        | 3     | 4     | NOT CONVERGED |
| `architect` | 0        | 3     | 4     | NOT CONVERGED |
| `ergo`      | 1        | 2     | 4     | NOT CONVERGED |
| `zigzag`    | 0        | 1     | 3     | NOT CONVERGED |
| `security`  | 3        | 3     | 5     | NOT CONVERGED |

All seven BLOCKING findings, thirteen MAJOR findings, and twenty-three MINOR findings are
**ACCEPTED** and resolved in v0.3 as recorded below.

______________________________________________________________________

## BLOCKING

1. **`vortex` B1 / `codebase` B1 (§4 K2, §8.1–8.2, P21.5): `.vnpkg` inline fast-path cannot reuse
   `SpoolSliceBundle` or `ingestBundle`; `prepareTorrent` is private.**

   - **Ruling: ACCEPTED.** `SpoolSliceBundle` and `decodeSpoolSliceBundle`
     (`mesh_value.cpp:746–785`) are strictly typed for Xanadu `Store` slices (`PublishedValue`
     11-field dictionary), and `MeshSwarmTransport::prepareTorrent` (`mesh_swarm.hpp:113`) is
     `private`.
   - **Resolution in v0.3:** Define a dedicated `PackageBundle` envelope (`sendPackageBundle` /
     `ingestPackageBundle` on `MeshHostTransport` and `MeshSwarmTransport`, demultiplexed by a
     1-byte frame discriminator on QUIC lane 1 or a dedicated QUIC stream lane) and a public
     `preparePackageBlob(hash, maxBytes, destCasPath)` method on `MeshHostTransport` /
     `MeshSwarmTransport` in P21.5.

1. **`vortex` B2 / `ergo` B1 (§4 K1, §4 K3, §5.2, §7.4, §8.4): Cross-platform `soname` in `op_log`
   breaks logical `programRef` in multi-target packages.**

   - **Ruling: ACCEPTED.** `b.library(name, soname, ...)` records one canonical `soname` string in
     `op_log`, while `targets[<triple>]` may target Linux (`.so`), macOS (`.dylib`), and Windows
     (`.dll`).
   - **Resolution in v0.3:** Add `name` (logical library name matching `b.library(name, ...)`) and
     `canonical_soname` (the `soname` recorded in `op_log`) to each entry in
     `targets[<triple>].bundled_libs`, and add an optional `system_library_aliases` dictionary
     (`{canonical_soname: target_soname}`) to `targets[<triple>]`. At load and Step 7 `dlopen` time,
     the target's `bundled_libs` and `system_library_aliases` map the canonical `op_log` library
     declaration to the target's staged `<soname>` or platform system library without altering
     `op_log` or `programRef`.

1. **`security` B1 (§5.2, §8.1–8.3, §13): Lack of swarm cluster trust policy and package ingress
   authentication.**

   - **Ruling: ACCEPTED.** Self-signed Ed25519 manifests without a trust root check before
     allocating `PendingProgramFetch` slots or downloading up to 64 MiB allow unauthenticated remote
     DoS and unapproved package execution.
   - **Resolution in v0.3:** Add `vagent.cluster.trust_policy` (`pinned` | `cluster_peers` | `any`,
     default `pinned`) and `vagent.cluster.trusted_keys` (list of trusted Ed25519 public keys /
     author roots) to §5.2, §8.1–8.3, §9, and §13 ( plus
     `vagent.cluster.max_pending_fetches_per_peer`, default 4). Verify the manifest's Ed25519
     signature against `trusted_keys` (or authenticated cluster peer identity under `cluster_peers`)
     **before** allocating a `PendingProgramFetch` slot or pulling torrent slices; add a §14.2
     escape probe asserting unauthenticated self-signed packages are rejected with
     `ActorError::NotAuthorized` before any swarm pull or CAS write.

1. **`security` B2 (§7.1, §7.4, §15): Dynamic linker resolution failure and missing `DT_SONAME` /
   `DT_RUNPATH` contract.**

   - **Ruling: ACCEPTED.** On ELF platforms (glibc, musl, FreeBSD `rtld`), pre-loading
     `<staging>/lib/<soname>` satisfies a dependent's `DT_NEEDED: <soname>` only when the loaded
     library carries a matching ELF `DT_SONAME` tag (or `$ORIGIN/lib` is in `DT_RUNPATH`).
   - **Resolution in v0.3:** Mandate in §7.1 and §7.4 that every bundled ELF shared library carry an
     internal `DT_SONAME` tag exactly equal to its manifest `soname` (refusing missing or mismatched
     `DT_SONAME` with `MissingSonameTag`, never a warning), permit `$ORIGIN` / `$ORIGIN/lib` in
     `DT_RUNPATH` as defense-in-depth, and load bundled libraries in topological order with
     `RTLD_NOW | RTLD_GLOBAL` inside the single-tenant `vagent-native-host` process (or match exact
     `DT_SONAME` link-map entries) so transitive symbols resolve deterministically.

1. **`security` B3 (§4 K4, §7.4, §9.2): Contradiction with P20 (`vagent-native-agent.md` §8.1/§8.3)
   on Phase 2 `openat` and OpenBSD `rpath`.**

   - **Ruling: ACCEPTED.** `vagent-native-agent.md` §8.1 and §8.3 keep `openat` governed by Landlock
     (read-only access to staging and `/usr/share/terminfo`, zero writes) and retain `rpath` in
     OpenBSD `pledge("stdio rpath recvfd")` so `newterm()` can read terminfo entries during Step 9.
   - **Resolution in v0.3:** Align §4 K4, §7.4, and §9.2 with `vagent-native-agent.md` §8.1–8.3:
     Phase 2 revokes `PROT_EXEC` on `mmap`/`mprotect` (`PR_SET_MDWE` + phase-2 seccomp on Linux,
     dropping `prot_exec` in `pledge("stdio rpath recvfd")` on OpenBSD, `ProhibitDynamicCode` on
     Windows) while read-only filesystem isolation is enforced by Landlock / Capsicum / `unveil` /
     Seatbelt / AppContainer ACLs.

______________________________________________________________________

## MAJOR

1. **`vortex` M1 (§4 K5, §8.3, P21.1): `PendingProgramFetch` drops messages to pending PIDs, expires
   leases, and races with live upgrade.**

   - **Ruling: ACCEPTED.** Index pending `ActorPid`s in `PendingProgramFetch` so messages
     (`ActorSend`, `ActorDeliverValue`, `ActorDown`) arriving for a spawning/resuming `pid` while
     its package is being fetched are buffered in a bounded per-PID pre-admission queue (up to
     `grant.maxMailboxBytes`) rather than rejected with `UnknownProcess`; freeze/extend lease
     expiration check by the fetch duration (requiring `grant.expiresAt > wallNow` at fetch start
     and crediting fetch wait time at admission); and suspend turn execution for an actor with an
     in-flight live `upgrade` fetch so its mailbox cannot overflow or mutate state mid-upgrade.

1. **`codebase` M1 / `zigzag` m2 (§4 K2, §5.2, §14.2): `SignedProvenance` / `verifyProvenance` is
   GPG-specific, whereas `.vnpkg` uses Ed25519.**

   - **Ruling: ACCEPTED.** Replace `SignedProvenance` / `verifyProvenance` citations with
     `PackageProvenance` signed and verified in-tree with Ed25519 (`xanadu::PublicKey`,
     `xanadu::identity::Signature64`, `xanadu::verifySignature` in `common/xanadu/crypto.hpp` and
     `identity_layout.hpp`), matching the Ed25519 pattern of `PublishedValue`
     (`mesh_value.hpp:33–43`).

1. **`codebase` M2 / `architect` m2 (§2, §4, §8, §14, P21.5): `prepareTorrent` is private and absent
   from `MeshHostTransport`.**

   - **Ruling: ACCEPTED.** Addressed together with `vortex` B1 / `codebase` B1 by declaring public
     `sendPackageBundle`, `ingestPackageBundle`, and `preparePackageBlob` methods on
     `MeshHostTransport` and `MeshSwarmTransport` in P21.5.

1. **`codebase` M3 / `architect` M3 (§8.3, §11.2, P21.1): Layering of `PendingProgramFetch` and
   `ActorError::ProgramPending` admission protocol.**

   - **Ruling: ACCEPTED.** Keep `PendingProgramFetch` strictly inside
     `apps/common/xanadu/vagent/cluster/pending_fetch.{hpp,cpp}` at the cluster/transport supervisor
     layer (`AGENTS.md` layer hygiene), and limit P21.1 in `vortex/mesh_actor.{hpp,cpp}` to a
     minimal, language-agnostic admission deferral hook: adding `ActorError::ProgramPending`,
     allowing `receive()` / `spawn()` / `resume()` / `upgrade()` to defer `ActorSpawnAck` and buffer
     messages for pending `ActorPid`s when the registered resolver returns
     `ActorError::ProgramPending`, and providing `completePendingProgram(programRef, result)` to
     resume or fail the parked admissions once the cluster layer finishes fetching and compiling.

1. **`architect` M1 (§8.1, §8.3): Verification and compilation on the transport owner thread stalls
   `MeshSwarmTransport`.**

   - **Ruling: ACCEPTED.** Offload streaming SHA-256 / Merkle verification, atomic CAS file commits
     (`fsync` + `renameat`), `package_inspector` header validation, and `CompiledProgram::fromUnit`
     compilation to a background package-worker pool (`vagent/cluster/pending_fetch.cpp`), leaving
     the `MeshSwarmTransport` owner thread strictly non-blocking (`mesh_host.hpp:11–12`). When the
     background worker finishes, it posts a completion event back to the owner/runtime thread to
     call `retainProgram` and `completePendingProgram`.

1. **`architect` M2 / `security` m5 (§6.2, §13, §16): Undefined CAS retention quota, pinning,
   `fsync` durability, and `.part` cleanup.**

   - **Ruling: ACCEPTED.** Add `vagent.cluster.cas_max_bytes` (default `10 GiB`) to §13 and P21.7;
     specify reference-counted CAS blob pinning (`pin(sha256)` / `unpin(sha256)`) across active
     `CompiledProgram` entries in `AgentExecutor`, staged hosts in `NativeHostSupervisor`, and
     in-flight `PendingProgramFetch` tasks so LRU eviction sweeps only unpinned blobs; require
     `O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW` (`0600`) for `<cas_dir>/tmp/<uuid>.part`, `fsync` on
     the file before `renameat`, `fsync` on `<cas_dir>/`, and an epoch startup sweep unlinking stale
     `.part` files in `<cas_dir>/tmp/`.

1. **`ergo` M1 (§5.1, §10.1, §12 P21.6): Underspecification of `"source+native"` packages.**

   - **Ruling: ACCEPTED.** Specify the CLI workflow for `"source+native"` in §5.1 and §10.1
     (`vagent package app.vagent --target x86_64-linux-gnu=build/x86_64/myskills.so -o app.vnpkg`),
     automatic `kind = "source+native"` inference when `.vagent` input is paired with `--target`,
     package-time verification that every target module exports `vagent_native_skills()` covering
     all `native skill` declarations in the source `Unit`, and the explicit boundary that
     `"source+native"` binds `native skill`s via `vagent_native_skills()` C function tables (whereas
     libffi sequences and authorities require the native builder API of `"native"`).

1. **`ergo` M2 (§9.1, §10, §12 P21.6): Authority approval CLI must accept `.vnpkg` packages.**

   - **Ruling: ACCEPTED.** Extend `vagent native approve` and `vagent native withdraw` in §9.1,
     §10.3, and P21.6 to accept `<file.vnprog | file.vnpkg | programRef>`, computing the K1 logical
     `programRef` when given a `.vnpkg`.

1. **`zigzag` M1 (§5.1, §8.2): Remove "sealed Store scroll" / "program store scroll" terminology and
   unify `program_body` for `"pure"` and `"source+native"`.**

   - **Ruling: ACCEPTED.** In §5.1 and §8.2, specify that `program_body` for `"pure"` and
     `"source+native"` is exclusively the canonical bencoded `Unit` AST (never a "Store scroll"),
     and that the receiving node compiles `Unit` (or `op_log` for `"native"`) via
     `CompiledProgram::fromUnit` into the node's local P17 sealed base `Store` + `programManifold`.

1. **`security` M1 (§4 K3, §7.1, §9.2): System library shadowing / hijacking via `bundled_libs`.**

   - **Ruling: ACCEPTED.** Enforce at both package creation (`vagent package`) and node staging
     (`package_inspector`) that `bundled_libs` `soname`s are strictly disjoint from declared system
     libraries and a reserved system library denylist (`libc.*`, `libm.*`, `libdl.*`,
     `libpthread.*`, `librt.*`, `ld-*`, `libnss_*`, `libSystem.*`, `kernel32.dll`, `ntdll.dll`),
     refusing collisions with `SystemLibraryShadowingForbidden`.

1. **`security` M2 (§7.1, §7.3, §11.1): Receiving node must run `package_inspector` before
   staging.**

   - **Ruling: ACCEPTED.** Mandate that the receiving node's background ingest/staging worker runs
     `package_inspector` on every downloaded module and bundled library before committing/staging—
     verifying `DT_SONAME` equality, rejecting `PT_INTERP` on shared libraries, rejecting any
     `DT_RPATH`/`DT_RUNPATH` outside `$ORIGIN` / `$ORIGIN/lib` (and rejecting `@executable_path` on
     macOS), and verifying acyclic topological `DT_NEEDED` order.

1. **`security` M3 / `architect` m3 (§4 K8, §6.2, §7.3, §16): Replace CAS hard-linking with CoW
   reflink (`FICLONE` / `clonefile`) or streaming copy.**

   - **Ruling: ACCEPTED.** Forbid raw POSIX `link()` between `<cas_dir>/<sha256>` (`0400`) and
     `<staging_dir>/.../lib/<soname>` (`0500`) so staged files never share mutable inode metadata
     with the CAS; use CoW reflink (`ioctl(dst, FICLONE, src)` on Linux, `clonefile` on macOS) or
     streaming copy with `O_RDONLY | O_NOFOLLOW | O_CLOEXEC` and SHA-256 verification.

______________________________________________________________________

## MINOR

- **`vortex` m1 (`publishValue` citation):** Cite both overloads in `mesh_value.hpp:68–74, :87–94`.
- **`vortex` m2 / `codebase` m4 (`seed()` ownership):** Cite `MeshSwarmTransport::seed()`
  (`mesh_swarm.hpp:91`) instead of `SwarmContentSource::seed()`.
- **`vortex` m3 (Lexicographical key sorting):** State explicitly in §4 K1 that `targets` dictionary
  keys are sorted lexicographically by raw byte value per BEP-3.
- **`codebase` m1 (`mesh_actor.cpp` citation):** Cite `mesh_actor.cpp:238, :322–324`.
- **`codebase` m2 (`COMMON_XANADU_SRCS`):** Note in P21.8 that `Makefile:633` auto-discovers `.cpp`
  files under `apps/common/xanadu/`.
- **`codebase` m3 (`kProgramFormat` lines):** Cite `program.hpp:37–39`.
- **`architect` m1 (`bencode.hpp` reuse):** Specify in §11.1 that `package_format.cpp` reuses
  `apps/common/xanadu/bencode.hpp`.
- **`architect` m4 (Mach-O `@executable_path`):** Explicitly reject `@executable_path` in bundled
  Mach-O libraries in §7.1 (permitting only `@loader_path` / `@rpath` resolving within `<staging>`).
- **`ergo` m1–m4 (CLI ergonomics):** Default `--target` for single-target `.vnprog` to the local
  host triple; clarify `System Libs` in `vagent package inspect`; allow
  `--bundle-dir <triple>=<dir>`; and state that `vagent run <file.vnpkg>` accepts `--ledger` and
  `--grant-policy` and enables `vagent.cluster.enabled` and `vagent.native.enabled`.
- **`zigzag` m1 (Canonical BitTorrent `pieceLength`):** Fix canonical `pieceLength = 262144` (256
  KiB) and flat basename in `makeTorrent` so `info_hash` and `targets_digest` are deterministic
  (K6).
- **`zigzag` m3 (`sha256:` display prefix):** Clarify in §4 K1 and §10.2 that `programRef` is a bare
  64-hex string internally and `sha256:` in `vagent package inspect` is a display label.
- **`security` m1–m4:** Enforce the ASCII regex allowlist
  `^[a-zA-Z0-9_][a-zA-Z0-9._-]*\.(so(\.[0-9]+)*|dylib|dll)$` (`<= 255` bytes, no leading `-`) on
  `soname`; use `O_CREAT | O_EXCL | O_NOFOLLOW` (`0600`) on `.part` files; add
  `vagent.cluster.max_pending_fetches_per_peer` (default 4); and require `0600` key file
  permissions.

# Round 1 critique: Vortex & Spool-Mesh Expert (vagent-cluster-packaging.md)

Paths below are relative to the repo root. Tally: 2 BLOCKING, 1 MAJOR, 3 MINOR.

## BLOCKING

### 1. (§4 K2, §8.1–8.2, P21.5) Inline fast-path cannot reuse `SpoolSliceBundle` or `ingestBundle`

**Problem:** §4 K2, §8.1–8.2, and P21.5 specify that packages $\le 1\text{ MiB}$ are packed into a
`SpoolSliceBundle` (`mesh_value.hpp:158–164`) and ingested directly via `ingestBundle`
(`mesh_swarm.hpp:90`). In C++, `SpoolSliceBundle` and `ingestBundle` are strictly typed for Xanadu
`Store` slices, not `.vnpkg` packages:

1. `decodeSpoolSliceBundle` (`mesh_value.cpp:746–785`) enforces a rigid 5-field dictionary
   (`version`, `manifest`, `carrier`, `history`, `payloads`) and explicitly invokes
   `decodeValue(manifest)` (`:765`).
1. `decodeValue` (`mesh_value.cpp:104–111`) strictly checks `decoded.asDict().size() != 11` and
   verifies that the manifest is a serialized `PublishedValue` (authenticated `Store` state root
   with `origin`, `root`, `publication`, `signature`).
1. A `.vnpkg` package (which starts with magic `\x89VAGNPKG\n` and has the 8 fields `api_version`,
   `format`, `kind`, `compiler_format`, `program_body`, `targets`, `provenance`, `signature`) will
   be rejected immediately with `"spool slice: invalid canonical bundle envelope"` or
   `"mesh value: invalid canonical envelope"`.
1. `VerifiedSliceContentSource::ingestBundle` (`mesh_swarm.cpp:705`) stores slices in an in-memory
   content source overlay for reading store spans; it does not commit files into the CAS directory
   (`<deployment>/packages/cas/`).
1. On the pull path, §8.2 claims Node B invokes `prepareTorrent(infoHash, maxBytes)`
   (`mesh_swarm.hpp:113`). But `prepareTorrent` is `private` in `MeshSwarmTransport`, downloads
   payloads to `options_.downloads / hash.hex()`, and has no mechanism to commit verified files to
   the package CAS.

**Evidence:**

- `design/vagent-cluster-packaging.md` §4 K2 (lines 184–187), §8.2 (lines 591–596), §12 (P21.5)
- `apps/common/xanadu/vortex/mesh_value.hpp:158–164`
- `apps/common/xanadu/vortex/mesh_value.cpp:108–111`, `:751–767`
- `apps/common/xanadu/vortex/mesh_swarm.hpp:90`, `:113`
- `apps/common/xanadu/vortex/mesh_swarm.cpp:700–713`, `:715–743`

**Fix:** Do not reuse `SpoolSliceBundle` or private `prepareTorrent` for cluster packages. Define a
dedicated package transmission envelope (e.g. `PackageBundle` or QUIC Lane 2 `PackageFrame`) and
explicit public transport methods (`sendPackageBundle` / `ingestPackageBundle`, and
`seedPackageBlob` / `fetchPackageBlob`) in P21.5 that decode `.vnpkg` packages and commit verified
blobs directly into `<cas_dir>/<sha256>`.

### 2. (§4 K1, §4 K3, §5.2, §8.4) Target-specific SONAME in `op_log` breaks logical `programRef`

**Problem:** §4 K1 establishes that a package possesses a single logical `programRef` computed over
`canonical_body` (the `op_log` for native agents), and §4 K7 lists non-ELF platforms
(`aarch64-apple-darwin`, `x86_64-pc-windows-msvc`). However:

1. `vagent-native-agent.md` §6.2/§11.2 records `b.library(name, soname, ...)` directly into the
   `op_log` via `VN_KIND_NATIVE_LIBRARY` with a concrete `soname` string literal (e.g.
   `"libengine.so.1"`).
1. If an author packages companion libraries across platforms (e.g. `libengine.so.1` on Linux,
   `libengine.1.dylib` on macOS, `engine.dll` on Windows), the `op_log` can only contain one
   hardcoded `soname`.
1. If `op_log` records `"libengine.so.1"`, then when the package is unpacked on macOS, the host
   loader receives a binding manifest demanding `"libengine.so.1"`, while `<staging>/lib/` contains
   only `libengine.1.dylib`. Loading fails because `bundled_libs` in §5.2 has no mapping from the
   `soname` in the `op_log` to the platform-specific library filename.
1. If the author is forced to compile a separate `.vnprog` per OS, each platform gets a different
   `op_log`, which produces a different `programRef`. That destroys logical `programRef` equivalence
   and directly breaks heterogeneous migration (§8.4).

**Evidence:**

- `design/vagent-cluster-packaging.md` §4 K1 (lines 157–177), §4 K3 (lines 192–207), §5.2 (lines
  314–322), §8.4 (lines 635–650)
- `design/vagent-native-agent.md` §6.2 (line 511), §11.2 (lines 1116, 1125, 1131–1133)

**Fix:** Specify that `b.library(name, ...)` in `op_log` uses a platform-neutral logical library
name (e.g. `"engine"`), or add a `logical_name` / `logical_soname` field to `bundled_libs` in §5.2.
When the supervisor stages the target slice, it maps or symlinks the logical library name/soname to
the target's actual staged file (`soname`) so that `dlopen` resolves across Linux, macOS, and
Windows without altering `op_log` or `programRef`.

## MAJOR

### 1. (§4 K5, §8.3, P21.1) `PendingProgramFetch` drops messages to pending PIDs and expires leases

**Problem:** `PendingProgramFetch` queues remote `ActorSpawn`, `ActorContinuation` (resume), and
`upgrade` requests while missing packages or binary slices are fetched asynchronously (up to 30s).
This creates three unhandled runtime failure modes in `MeshActorRuntime`:

1. **Message Dropping for Pending Actors:** For a resumed/migrating actor, peer nodes send messages
   addressed to `ActorPid` (`ActorSend`, `ActorDeliverValue`, `ActorDown`). In
   `MeshActorRuntime::receive` (`mesh_actor.cpp:440–441`), `find(*message.target)` looks in
   `actors_`. Because the actor has not been instantiated yet (its continuation is parked in
   `PendingProgramFetch`), `find` returns `nullptr`, and `receive()` immediately rejects the message
   with `ActorError::UnknownProcess`! `PendingProgramFetch` only keys by `programRef` and has no
   index or mailbox for pending `ActorPid`s.
1. **Lease Expiration During Fetch:** An incoming `ActorGrant` has an `expiresAt` timestamp
   (`mesh_actor.hpp:55`). During an asynchronous fetch taking up to `vagent.cluster.fetch_timeout`
   (30s), wall-clock time elapses. When the package arrives and `spawn()` or `resume()` is called,
   `wallNow >= grant->expiresAt` (`mesh_actor.cpp:174`, `:258`), causing admission to fail with
   `ActorError::NotAuthorized`.
1. **Concurrent Mutation During Live Upgrade:** If an actor undergoing live upgrade continues
   running its turn loop while `newGrant.programRef` is being fetched, incoming messages can inflate
   its mailbox past `newGrant.maxMailboxBytes`, causing `upgrade()` (`mesh_actor.cpp:713`) to fail
   with `ActorError::ResourceLimit` when the package finally arrives.

**Evidence:**

- `design/vagent-cluster-packaging.md` §4 K5 (lines 221–231), §8.3 (lines 604–634), §12 (P21.1)
- `apps/common/xanadu/vortex/mesh_actor.hpp:55`
- `apps/common/xanadu/vortex/mesh_actor.cpp:174`, `:258`, `:440–441`, `:703–718`

**Fix:**

1. Extend `PendingProgramFetch` to index pending `ActorPid`s for resumed actors, and update
   `MeshActorRuntime::receive()` to queue messages addressed to pending PIDs instead of dropping
   them with `UnknownProcess`.
1. Explicitly specify that the authorizer/transport must grant a lease duration exceeding
   `fetch_timeout`, or clamp/renew lease deadlines upon admission.
1. Specify that an actor undergoing live upgrade suspends turn execution and mailbox popping until
   the new program is fetched and reconciled.

## MINOR

- **`publishValue` signature citation (§1.1 line 50).** §1.1 cites `mesh_value.hpp:68–74`. Note that
  `publishValue` is overloaded at `:68–74` (taking `xanadu::SignedProvenance`) and `:87–94` (taking
  `ValueProvenanceSigner`).
- **`SwarmContentSource::seed` method name (§8.1 sequence diagram step 2).** The diagram references
  `SwarmContentSource::seed()`. `SwarmContentSource` uses `addTorrent(..., seeding = true)`
  (`swarm.hpp:174–175`), while `seed()` is a method on `MeshSwarmTransport` (`mesh_swarm.hpp:91`).
- **Lexicographical key sorting in `targets_digest` (§4 K1 line 170).** To guarantee deterministic
  `programRef` calculation across different bencode implementations, explicitly state that
  dictionary keys in `targets` must be sorted lexicographically by raw byte value per BEP-3.

VERDICT: NOT CONVERGED

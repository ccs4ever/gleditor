# Round 1 critique: Security & Sandboxing Expert (vagent-cluster-packaging.md)

Paths below are relative to the repo root. Tally: 3 BLOCKING, 3 MAJOR, 5 MINOR.

## BLOCKING

### 1. (§5.2, §8.1–8.3, §13) Lack of swarm cluster trust policy and package ingress authentication

**Problem:** §5.2 lines 326–332 defines package `provenance` (`author`, `device`, `public_key`) and
`signature` (Ed25519 signature over package metadata). However, nowhere in the document is there a
cluster trust policy, an authorized author/key allowlist, or an authentication check performed
against node trust roots before admitting a package into `PendingProgramFetch`. The signature
verification in §14.2 line 856 (`verifyProvenance`) checks only that `signature` is mathematically
valid for `provenance.public_key`. Because Ed25519 keypairs can be generated freely, any untrusted
or adversarial peer on the swarm can self-sign a crafted `.vnpkg` and emit an `ActorSpawn` or
`ActorContinuation` referencing it.

Upon receiving the frame, the transport thread (§8.1, §8.3) immediately enqueues the request into
`PendingProgramFetch`, announces torrent `InfoHash`es, streams up to 64 MiB across the network,
commits slices into `<deployment>/packages/cas/`, and compiles bytecode. An attacker can trivially
exhaust the 32 `PendingProgramFetch` slots ($32 \times 64\text{ MiB} = 2\text{ GiB}$ per node),
saturating node disk and network bandwidth. Furthermore, for pure agents or native agents operating
under `vagent.native.grant_policy = deny` (where no authorities are requested, so the authority
ledger is never consulted), untrusted code from unauthenticated authors is admitted and executed on
the node without any cluster authorization.

**Evidence:** `design/vagent-cluster-packaging.md:330` embeds `public_key` in the manifest without a
trust root binding. `design/vagent-cluster-packaging.md:564-576` and `:608-634` enqueue and fetch
missing packages without authenticating the author against cluster trust roots. Table 13 lacks any
`vagent.cluster.trusted_keys` or `vagent.cluster.trust_policy` setting.

**Fix:** Add a mandatory cluster package trust policy before swarm ingestion:

1. Introduce a cluster setting `vagent.cluster.trust_policy` (`pinned` | `cluster_peers` | `any`,
   defaulting to `pinned`) and `vagent.cluster.trusted_keys` (a list of authorized Ed25519 public
   keys or deployment identity roots).
1. When an `ActorSpawn`, migration continuation, or inline bundle arrives referencing an uncached
   `programRef`, the transport thread must verify that `manifest.provenance.public_key` matches an
   authorized key in `vagent.cluster.trusted_keys` (or originates from an authenticated cluster peer
   in `MeshHostTransport`) *before* allocating a `PendingProgramFetch` slot or downloading slices
   from the swarm.
1. Add a test gate in §14.2 asserting that an unauthenticated, self-signed `.vnpkg` is dropped at
   network ingress with `ActorError::NotAuthorized` without initiating swarm downloads or CAS
   writes.

### 2. (§7.1, §7.4, §15) Dynamic linker resolution failure and missing DT_SONAME/DT_RUNPATH contract

**Problem:** §7.4 specifies that `vagent-native-host` pre-loads bundled libraries in topological
order via `dlopen("<staging>/lib/<soname>", RTLD_NOW | RTLD_LOCAL)` and then calls
`dlopen("<staging>/module.so", RTLD_NOW | RTLD_LOCAL)`. However, the dynamic linker (glibc `ld.so`,
musl, FreeBSD `ld-elf.so.1`) matches an already-loaded shared library against a dependent's
`DT_NEEDED: <soname>` string **only if** the loaded object's internal ELF `DT_SONAME` tag matches
the `DT_NEEDED` string.

1. §7.1 line 422 states: "If a library lacks `DT_SONAME`, its basename is used, and a warning is
   emitted." When a library lacking `DT_SONAME` is loaded via `dlopen("<staging>/lib/<soname>")`,
   the linker records `l_soname = NULL` and `l_name = "<staging>/lib/<soname>"`. When `module.so` is
   subsequently loaded, the linker tries to resolve `DT_NEEDED: <soname>`. Because `l_soname` is
   NULL and `l_name` does not equal `<soname>`, the linker fails to match the already-loaded
   library. It then falls back to search paths (`RPATH`/`RUNPATH`/system paths), fails to find the
   bundled non-system library in `/usr/lib`, and crashes with `NativeLoadError`!
1. §15 line 870 refuses `$ORIGIN/lib` in `DT_RUNPATH`, claiming topological `dlopen` eliminates the
   need for dynamic search. But without `$ORIGIN/lib` in `DT_RUNPATH` or an exact `DT_SONAME` match
   in the linker's link map, topological `dlopen` alone cannot resolve dependencies.
1. Furthermore, loading dependencies with `RTLD_LOCAL` (§7.4 line 495) prevents transitive
   dependencies not explicitly enumerated in a dependent's direct `DT_NEEDED` list from resolving
   symbols from earlier libraries.

**Evidence:** `design/vagent-cluster-packaging.md:421-422` admits libraries lacking `DT_SONAME` with
a warning. `design/vagent-cluster-packaging.md:495-498` invokes `dlopen` with `RTLD_LOCAL` without
requiring `DT_SONAME` equality or providing search paths.

**Fix:**

1. Mandate `DT_SONAME` equality: `vagent package` and the staging supervisor must require that every
   bundled ELF shared library contains an internal `DT_SONAME` entry exactly matching its declared
   `soname`. Bundling any library lacking `DT_SONAME` or with a mismatched `DT_SONAME` must be a
   hard packaging error (`MissingSonameTag`), never a warning.
1. Require that `module.so` and bundled libraries include `DT_RUNPATH = "$ORIGIN/lib:$ORIGIN"`
   (which §7.1 line 427 permits) as defense-in-depth so the dynamic linker's standard resolution
   path finds co-staged libraries even if link-map matching behaves inconsistently across libc
   implementations.
1. Because `vagent-native-host` is a dedicated, single-tenant sandboxed process for one agent,
   specify `RTLD_GLOBAL` (or document the exact dependency link-map propagation) so bundled
   companion libraries can resolve mutual symbols cleanly.

### 3. (§7.4) Contradiction with sibling document (P20) regarding Phase 2 openat and rpath lockdown

**Problem:** §4 K4 (line 218), §7.4 (line 501), and §9.2 (line 677) state that Phase 2 sandbox
lockdown "denies `openat`" and drops `rpath` on OpenBSD. This directly contradicts sibling document
`design/vagent-native-agent.md` (P20). In `vagent-native-agent.md` §8.3 lines 721–730, `openat` is
explicitly permitted in seccomp across both Phase 1 and Phase 2 because ncurses `newterm()` is
called during agent turn execution in Step 9 ("Serve") and requires `openat` to read terminfo
databases (`/usr/share/terminfo`). Filesystem access is governed by Landlock, which restricts
`openat` to read-only access on staging and terminfo, denying all writes. On OpenBSD
(`vagent-native-agent.md` §8.1 line 675), `rpath` is explicitly retained in
`pledge("stdio rpath recvfd")` for the same reason.

If a cluster node revokes `openat` in seccomp or drops `rpath` in `pledge` during Step 8 as
specified in §7.4, any agent opening a terminal will immediately crash with `EPERM` or trigger an
OpenBSD `SIGABRT` pledge violation on its first turn, breaking the first test (§14.1) and all
terminal agents.

**Evidence:** `design/vagent-cluster-packaging.md:501` states: "Phase 2 (Step 8): The host calls
`prctl(PR_SET_MDWE, ...)` and installs the second seccomp filter denying `mmap`/`mprotect` with
`PROT_EXEC` and denying `openat`." Contradicts `design/vagent-native-agent.md:721, :727-730`
(`openat` governed by Landlock, not denied by seccomp) and `:675` (`pledge` retains `rpath`).

**Fix:** Realign §4 K4, §7.4, and §9.2 with `vagent-native-agent.md` §8.1–8.3:

1. On Linux, Phase 2 seccomp revokes `PROT_EXEC` on `mmap`/`mprotect` and `MREMAP_MAYMOVE` on
   executable mappings; `openat` remains governed by Landlock (which permits read-only access to
   staging and `/usr/share/terminfo` and denies all writes).
1. On OpenBSD, Phase 2 `pledge` retains `rpath` (`pledge("stdio rpath recvfd")`), dropping only
   `prot_exec`.
1. Clarify macOS Seatbelt: the profile denying `MAP_JIT` and unapproved `file-map-executable` is
   applied during `sandbox_init` in Step 6, as Seatbelt provides no dynamic post-load API.

## MAJOR

### 1. (§4 K3, §7.1, §9.2) System library shadowing and hijacking via bundled_libs

**Problem:** §4 K3 defines system libraries as those residing in `vagent.native.library_paths` and
bundled libraries as companion libraries supplied via `--bundle-lib`. However, nowhere in §5.2
(manifest schema), §7.1 (binary inspection), or §9.2 (threat analysis) is there a check preventing a
package from declaring a bundled library with `soname = "libc.so.6"`, `"libpthread.so.0"`,
`"libdl.so.2"`, `"libncursesw.so.6"`, or Windows system DLL names (`kernel32.dll`, `ntdll.dll`). An
adversary can craft a package bundling a trojaned `libc.so.6` or `libncursesw.so.6`. When
`vagent-native-host` loads bundled libraries in Step 7 before `module.so`, the dynamic linker will
bind symbols from the attacker-controlled bundled library instead of the host system library,
subverting process runtime integrity before Phase 2 lockdown.

**Evidence:** `design/vagent-cluster-packaging.md:354-356` requires only that `soname` contains no
slashes. No validation checks `soname` against declared or standard system library names.

**Fix:** Mandate that both the packaging tool (`vagent package`) and the receiving node's supervisor
enforce a strict **system library shadowing check**: The set of `bundled_libs` `soname`s must be
strictly disjoint from all declared system libraries in `vagent.native.library_paths` and a reserved
system library denylist (`libc.*`, `libm.*`, `libdl.*`, `libpthread.*`, `librt.*`, `ld-*`,
`libnss_*`, `kernel32.dll`, `ntdll.dll`, `libSystem.*`). Any collision must cause immediate package
rejection with `SystemLibraryShadowingForbidden`.

### 2. (§7.1, §7.3, §11.1) Receiving node lacks binary header and ingest validation

**Problem:** §7.1 describes static ELF/Mach-O/PE inspection (reading `DT_NEEDED`, checking
`DT_SONAME`, validating `DT_RPATH`/`DT_RUNPATH`) as an activity performed by `vagent package` during
package creation. However, cluster nodes ingest packages from the swarm or untrusted clients. An
attacker can craft a `.vnpkg` manifest and binary blobs without using `vagent package`. If the
receiving node merely copies/links blobs from CAS into `<staging>/lib/` without running header
inspection, a package containing a binary with an insecure `DT_RPATH` (e.g. `/tmp` or `/dev/shm`),
an ELF `PT_INTERP` header, or circular/missing dependencies will be staged and executed by
`vagent-native-host`.

**Evidence:** `design/vagent-cluster-packaging.md:416` begins "When packaging a native agent via
`vagent package`...". §11.1 lists `package_inspector.cpp` but §11.2 does not wire it into
`supervisor.cpp` or the CAS ingestion pipeline on the receiving node.

**Fix:** Mandate that `NativeHostSupervisor::stageNativeTarget` (or the CAS ingestion verifier)
executes `package_inspector` on the receiving node prior to host process spawn:

1. Verify that every staged library's internal `DT_SONAME` matches its manifest `soname`.
1. Verify that `DT_RPATH` / `DT_RUNPATH` contains only `$ORIGIN` or `$ORIGIN/lib` (or is absent),
   rejecting any other path with `InsecureRpath`.
1. Validate the DAG and topological order on the actual staged binaries, rejecting cycles or
   unfulfilled `DT_NEEDED` entries before spawn.

### 3. (§6.2, §7.3, §16) CAS blob hardlink inode sharing and in-flight corruption hazard

**Problem:** §4 K8, §7.3 (line 472), and §16 (line 919) specify that staging "copies or hard-links
the verified CAS blobs into this structure." Hard-linking a CAS blob (`<cas_dir>/<sha256>`) to
`<staging_dir>/lib/<soname>` causes the staging directory entry and the CAS storage entry to share
the exact same inode.

1. In §6.2 line 406, CAS blobs are committed with mode `0400`. In §7.3 line 473, staged binaries are
   set to mode `0500`. Modifying permissions on the staged hard-link modifies the CAS blob's inode
   permissions across the entire node.
1. If multiple native hosts for different agents share the same hard-linked CAS blob, any file
   attribute manipulation or lock affects all concurrent agents.
1. Hard-linking skips verifying that the CAS blob on disk still matches its SHA-256 digest at host
   launch time, leaving it vulnerable to bit rot or local tampering between ingest and launch.

**Evidence:** `design/vagent-cluster-packaging.md:472` states "The supervisor copies or
hard-links...". `design/vagent-cluster-packaging.md:919` prices staging based on hard-links.

**Fix:** Replace naive hard-linking with Copy-on-Write reflinking or streaming copy:

1. Use atomic reflink via `ioctl(dst, FICLONE, src)` (or `copy_file_range`) on supported filesystems
   (Btrfs, XFS, ZFS, APFS `clonefile`), which provides $< 1\text{ ms}$ zero-copy instantiation while
   ensuring completely independent inodes.
1. When reflink is unsupported, copy the bytes from CAS opening with
   `O_RDONLY | O_NOFOLLOW | O_CLOEXEC` and streaming to destination `0500`.
1. Forbid raw POSIX `link()` unless `st_nlink` and inode isolation are strictly audited.

## MINOR

1. (§5.2) **SONAME Character Set Validation:** `soname` validation in §5.2 should explicitly specify
   an ASCII regex allowlist: `^[a-zA-Z0-9_][a-zA-Z0-9._-]*\.(so(\.[0-9]+)*|dylib|dll)$` with a
   maximum length of 255 bytes, forbidding leading hyphens (`-`) to avoid command-line option
   injection in tooling.
1. (§6.2) **Temporary File Ingestion Hygiene:** When opening `<cas_dir>/tmp/<uuid>.part`, the
   implementation must pass `O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW` with file mode `0600` to
   prevent symlink race conditions in multi-tenant environments.
1. (§8.3) **Fetch Request Flooding per Origin:** `PendingProgramFetch` limits total pending fetches
   to 32 (`vagent.cluster.max_pending_fetches`), but does not limit fetches per peer origin,
   allowing a single misbehaving peer to monopolize all 32 fetch slots. Add a per-origin throttle
   (e.g. `max_pending_fetches_per_peer = 4`).
1. (§10.1) **Private Key Handling in CLI Examples:** §10.1 line 703 shows
   `--key ~/.vagent/keys/alice.key`. Storing raw Ed25519 private keys directly in home directories
   should specify file mode `0600` and integrate with Xanadu's existing keystore conventions.
1. (§13) **CAS Garbage Collection Setting:** CAS disk storage grows as packages are ingested. Table
   13 should expose a setting `vagent.cluster.cas_max_bytes` (e.g. 10 GiB) and specify an LRU
   eviction policy for unpinned package blobs.

VERDICT: NOT CONVERGED

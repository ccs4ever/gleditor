# Round 2 critique: Security & Sandboxing Expert (vagent-cluster-packaging.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings

- **B1 (§5.2, §8.1–8.3, §13 — Swarm cluster trust policy and ingress authentication): RESOLVED.**
  Section 4 Ruling K2 (lines 209–219), §8.1 (lines 688, 704), §8.3 (lines 758–760), and Table 13
  introduce `vagent.cluster.trust_policy` (`pinned`, `cluster_peers`, `any`, defaulting to `pinned`)
  and `vagent.cluster.trusted_keys`. Ingress frames and inline bundles must verify the manifest's
  Ed25519 signature against trusted keys *before* allocating a `PendingProgramFetch` slot or
  initiating swarm downloads. §14.2 adds the `UntrustedPackageIngress` escape probe verifying that
  unauthorized packages are rejected with `ActorError::NotAuthorized`. Furthermore,
  `vagent.cluster.max_pending_fetches_per_peer` limits concurrent fetches to 4 per origin.
- **B2 (§7.1, §7.4, §15 — Dynamic linker contract and mandatory `DT_SONAME` equality): RESOLVED.**
  Section 7.1 lines 533–535 and §4 Ruling K4 lines 257–263 make internal `DT_SONAME` header tags
  mandatory for all bundled ELF libraries: a missing or mismatched `DT_SONAME` tag fails closed with
  `MissingSonameTag` (eliminating silent fallback to filename). Defense-in-depth search paths
  (`$ORIGIN` and `$ORIGIN/lib` in `DT_RUNPATH`) are permitted, and Step 7 loads bundled libraries
  with `RTLD_NOW | RTLD_GLOBAL` (§7.4 lines 615–618, 630–631, 640–641, 650), ensuring symbols
  resolve deterministically. §14.2 adds the `MissingSonameTag` escape probe.
- **B3 (§4 K4, §7.4, §9.2 — Sibling document P20 alignment for Phase 2 `openat` and `rpath`):
  RESOLVED.** Section 4 Ruling K4 lines 269–275, §7.4 lines 620–623, and §9.2 line 795 realign Phase
  2 lockdown with `design/vagent-native-agent.md` §8.1–8.3: seccomp Phase 2 revokes `PROT_EXEC` and
  `MREMAP_MAYMOVE` via `PR_SET_MDWE`, while `openat` remains governed by Landlock (permitting
  read-only access to staging and `/usr/share/terminfo`, with zero writes). On OpenBSD (§7.4 lines
  642–644), Phase 2 `pledge` retains `rpath` (`pledge("stdio rpath recvfd", nullptr)`), dropping
  only `prot_exec` so ncurses `newterm()` can read terminfo databases during Step 9.
- **M1 (§4 K3, §7.1, §9.2 — System library shadowing denylist): RESOLVED.** Section 4 Ruling K3
  lines 248–251, §7.1 lines 541–545, and §14.2 line 1044 define an explicit system library shadowing
  denylist (`libc.*`, `libm.*`, `libdl.*`, `libpthread.*`, `librt.*`, `ld-*`, `libnss_*`,
  `libSystem.*`, `kernel32.dll`, `ntdll.dll`). Any collision in `bundled_libs` fails closed with
  `SystemLibraryShadowingForbidden`.
- **M2 (§7.1, §7.3, §11.1 — Receiving node header inspection): RESOLVED.** Section 7.1 line 528,
  §7.3 lines 579–580, §8.1 line 700, and §11.2 lines 876–878 mandate that the receiving node's
  background worker pool executes `package_inspector` on every downloaded blob prior to staging,
  validating `DT_SONAME` equality, verifying that `DT_RPATH`/`DT_RUNPATH` is restricted to `$ORIGIN`
  or `$ORIGIN/lib` (and rejecting Mach-O `@executable_path` with `InsecureRpath`), rejecting
  `PT_INTERP` on shared libraries, and verifying acyclic topological order before
  `stageNativeTarget` is called.
- **M3 (§4 K8, §6.2, §7.3, §16 — CAS reflink staging and inode isolation): RESOLVED.** Section 4
  Ruling K8 lines 340–345, §7.3 lines 591–593, §15 line 1062, and §16 lines 1119–1122 strictly
  forbid raw POSIX `link()`, mandating copy-on-write (CoW) reflink (`ioctl(dst, FICLONE, src)` on
  Linux, `clonefile` on macOS) or streaming copy (`0500`) with `O_RDONLY | O_NOFOLLOW | O_CLOEXEC`
  and SHA-256 verification, guaranteeing that staged files never share mutable inode metadata with
  CAS blobs.
- **Minors 1–5 (Regex allowlist, temporary file hygiene, peer throttling, key permissions, CAS
  quota): RESOLVED.** All five minor items are incorporated into v0.3: §4 K3 line 245 defines the
  ASCII regex allowlist for `soname`; §6.2 lines 506–507 passes
  `O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW` (`0600`) for `.part` files; §8.3 line 759 limits
  pending fetches to 4 per peer; §10.1 line 809 specifies `0600` file mode for private keys; and
  §6.2 line 517 and Table 13 add `vagent.cluster.cas_max_bytes` (10 GiB) with ref-counted pinning.

## BLOCKING

None.

## MAJOR

None.

## MINOR

None.

VERDICT: CONVERGED

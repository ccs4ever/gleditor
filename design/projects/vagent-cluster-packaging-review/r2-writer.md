# Round 2 critique: Technical Writer (vagent-cluster-packaging.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 5 MINOR.

## Status of previous findings

- **MAJOR 1 (§12, §13 — Settings registration and host options):** Resolved in v0.2/v0.3. Proposal
  P21.7 was added to Table 12 (`apps/common/xanadu/system_docs.cpp`,
  `apps/common/xanadu/vagent/runtime/program.hpp`), explicitly registering all 14 `vagent.cluster.*`
  settings schemas in `system_docs.cpp` and adding `ClusterSettings` to `LocalHost::Options`
  (`program.hpp`). Proposal P21.8 was also added covering Makefile auto-discovery, test target
  `test/vagent-cluster-packaging` in `TEST_PROGRAMS`, fuzz targets, and the `vagent.cluster` logging
  category in `design/logging-migration.md`.
- **MAJOR 2 (§12, §16 — Operational costs, pricing, and fallbacks):** Resolved in v0.2/v0.3. Every
  proposal in Table 12 (P21.1–P21.8) now includes explicit "Price / Cost" and "Fallback" columns.
  Section 16 ("Properties and Costs") details guarantees D1–D5 and operational budgets: CAS storage
  footprint (10 GiB quota), inline QUIC Lane 1 fast-path vs. swarm pull latency, control plane
  memory ceiling ($< 256\text{ KiB}$ for `PendingProgramFetch`), background worker verification
  offloading ($\approx 2\text{–}5\text{ ms}$), and host CoW reflink staging latency
  ($< 1\text{ ms}$).
- **MINOR 1 (§2, §4 K1, §6.1, §10.2 — Hash algorithm naming for `programRef`):** Resolved in
  v0.2/v0.3. All sections consistently specify SHA-256 (matching `program.cpp:26`). Glossary §2, CAS
  manifest filenames (`sha256-...`), and `vagent package inspect` output uniformly display bare
  64-hex strings with the `sha256:` presentation label.
- **MINOR 2 (§Contents — Table of Contents link text drift):** Resolved in v0.2/v0.3. All 18 Table
  of Contents entries (lines 18–35) match the exact titles of their corresponding `##` section
  headings and resolve to valid anchor slugs.
- **MINOR 3 (§4 K3 — Library declaration syntax):** Resolved in v0.2/v0.3. Section 4 Ruling K3 lines
  182–195 and 236–252 clearly distinguish module code declarations (`b.library(name, soname, ...)`)
  from packaging-time companion declarations (`--bundle-lib` and manifest `bundled_libs`), removing
  all misleading `bundle "libmyengine.so.1"` pseudocode syntax.
- **MINOR 4 (§1.2 — Sibling document cross-reference):** Resolved in v0.2/v0.3. Line 71 fully
  qualifies the citation as `(.vnprog, vagent-native-agent.md §7.1)`.
- **MINOR 5 (§12 P21.5 — Bundle streaming wording):** Resolved in v0.2/v0.3. P21.5 explicitly
  declares public `sendPackageBundle` / `ingestPackageBundle` demultiplexed via frame tag `0x02` on
  QUIC Lane 1 and public `preparePackageBlob(hash, maxBytes, destCasPath)` on `MeshHostTransport`
  and `MeshSwarmTransport`.

## Audit of v0.3 revisions

- **Transport Framing & PackageEnvelope (§4 K2, §8.1–8.2, P21.5):** Clean separation between Xanadu
  Store slices (`SpoolSliceBundle`, tag `0x01`) and container packages (`PackageBundle`, tag `0x02`)
  on QUIC Lane 1. Public `preparePackageBlob` directly commits downloaded blobs to CAS.
- **Cross-Platform Library Mapping (§4 K1, §5.2, §8.4):** Target slices cleanly pair `name`,
  `canonical_soname`, and target `soname` in `bundled_libs`, complemented by
  `system_library_aliases`, allowing runtime mapping at load/staging time without altering canonical
  `op_log` or `programRef`.
- **Swarm Trust Policy & Ingress Authentication (§4 K2, §8.1, §9.2, §13):** `trust_policy`
  (`pinned`, `cluster_peers`, `any`) and `trusted_keys` enforce Ed25519 author signature
  verification before allocating fetch slots or initiating swarm downloads. Unauthenticated requests
  fail immediately with `ActorError::NotAuthorized`.
- **Dynamic Linker Contract & Shadowing Denylist (§4 K3, §4 K4, §7.1):** Enforces mandatory internal
  `DT_SONAME` header tags matching manifest `soname` (`MissingSonameTag`), rejects `PT_INTERP`,
  allows only `$ORIGIN` / `$ORIGIN/lib` in `DT_RUNPATH` (`InsecureRpath`), rejects Mach-O
  `@executable_path`, and strictly forbids shadowing reserved system libraries (`libc.*`, `libm.*`,
  etc.).
- **Background Package Worker Pool & Admission Protocol (§4 K5, §8.1, §8.3, P21.1):** Verification,
  CAS commits, and compilation run on a background package worker pool off the transport owner
  thread. Incoming messages for pending PIDs buffer up to `grant.maxMailboxBytes`, fetch wait time
  is credited against lease expiration, and live upgrade turn execution suspends until admission.
- **CAS Durability, Quota & Reflink Staging (§4 K8, §6.2, §7.3, P21.3):** Unpinned CAS storage is
  bounded by `vagent.cluster.cas_max_bytes` (10 GiB) with ref-counted pinning. Spooling uses `0600`
  with `O_NOFOLLOW` and `fsync()`. Raw POSIX `link()` is forbidden in favor of CoW reflink
  (`FICLONE` / `clonefile`) or streaming copy (`0500`).
- **Markdown Hygiene & Codebase Citations:** Both `mdformat --check --wrap 100` and
  `mdl -s .mdl_style.rb -i` pass with 0 errors or warnings on `design/vagent-cluster-packaging.md`.
  All citations to `mesh_actor.{hpp,cpp}`, `mesh_value.{hpp,cpp}`, `mesh_swarm.hpp`,
  `agent_executor.hpp`, `program.hpp`, and sibling design specs are verified accurate.

## BLOCKING

None.

## MAJOR

None.

## MINOR

1. **(§5.2 line 462, §9.1 line 805) Crypto header citation path:** Lines 205–206 and 463 cite
   `common/xanadu/crypto.hpp` alongside `identity_layout.hpp`. In the repository, Ed25519 types
   `Signature64` and `Fingerprint` reside in `apps/common/xanadu/identity/identity_layout.hpp:220`,
   `PublicKey` in `apps/common/xanadu/mutable_link.hpp:58`, and crypto operations in
   `apps/common/xanadu/identity/standard_crypto_engine.hpp` and `transcopyright_crypto.hpp`. Update
   the citation to reference `identity/identity_layout.hpp` and
   `identity/standard_crypto_engine.hpp`.
1. **(§Header line 6) Metadata review record link:** The metadata table status row links to
   `[record](projects/vagent-native-agent-review/README.md)`. For the dedicated cluster packaging
   critique suite, point to `projects/vagent-cluster-packaging-review/`.
1. **(§10.3 lines 908, 911) CLI authority approval examples omit `--kind`:** The illustrated
   commands `vagent native approve greeter.vnpkg --as deployer` and `vagent native withdraw ...`
   omit the authority kind. Under `vagent-native-agent.md` §10.5 and P20.8, ledger approvals bind
   `(programRef, kind)` pairs; add `--kind <kind>` (e.g. `--kind terminal`) to the example
   invocations.
1. **(§5.2 line 420) Canonical key ordering for `system_library_aliases`:** For complete bencode
   determinism across independent packaging implementations, state explicitly in §5.2 that keys in
   `system_library_aliases` must be sorted lexicographically by raw byte order per BEP-3.
1. **(§10.1 line 860) Clarify `--bundle-dir` automatic dependency resolution:** In §10.1 line 860
   (`--bundle-dir aarch64-linux-gnu=build/aarch64/libs`), state explicitly that `--bundle-dir` scans
   the directory, matches shared library basenames against the K3 regex allowlist, extracts internal
   `DT_SONAME`s, and automatically resolves topological dependency ordering.

VERDICT: CONVERGED

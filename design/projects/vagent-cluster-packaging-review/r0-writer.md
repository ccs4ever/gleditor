# Round 1 critique: Technical Writer (vagent-cluster-packaging.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 2 MAJOR, 5 MINOR.

Checks run:
`mdformat --check --wrap 100 --extensions gfm --extensions dollarmath --extensions frontmatter` and
`mdl -s .mdl_style.rb -i` both pass cleanly on `design/vagent-cluster-packaging.md` (0 errors).

Codebase and sibling document citations spot-checked:

- `mesh_actor.hpp:47–57` (`ActorGrant`), `:114–120` (`ActorContinuation`), `:162–165` (`spawn`),
  `:170–174` (`resume`), `:190–191` (`pumpOne`), `:205–216` (`upgrade`) all exist as cited.
- `mesh_actor.cpp:322–324` (`ActorPid` assignment and `continuation->pid` check in `resume`) exists
  as cited.
- `mesh_value.hpp:33–43` (`PublishedValue`), `:68–74` (`publishValue`), `:158–164`
  (`SpoolSliceBundle`), `:167–172` (`computeSpoolMerkleLeaves`, `verifySpoolMerkleLeaves`) all exist
  as cited.
- `mesh_swarm.hpp:50–124` (`MeshSwarmTransport`), `:75–76` (`sendBundle`), `:90` (`ingestBundle`),
  `:113` (`prepareTorrent`) all exist as cited.
- `agent_executor.hpp:246` (`retainProgram`), `:247–248` (`resolveProgram`) exist as cited.
- `program.hpp:39` (`kProgramFormat = "vagent-program-3"`) exists as cited.
- Cross-references to `vagent-native-agent.md` (§3, §6, §7.1, §7.3, §8.1, §10.5, §12.3, §17) and
  `vagent-shared-program-and-live-upgrade.md` (§2.2) accurately align with the approved companion
  specs.

The findings below address structural gaps and polish opportunities within
`design/vagent-cluster-packaging.md`.

## BLOCKING

None.

## MAJOR

### 1. (§12, §13) Missing proposal to register `vagent.cluster.*` settings in `system_docs.cpp`

**Problem:** Section 13 specifies 10 configuration options in the `vagent.cluster.*` namespace and
line 795 asserts that "Configuration options are integrated into system document schemas under the
`vagent.cluster.*` namespace:". However, Section 12's Proposal Register (Table 12, P21.1–P21.6)
contains no proposal modifying `apps/common/xanadu/system_docs.cpp` (where `defaultSettingSpecs()`
populates xanadoc schema entries) or defining a `ClusterSettings` struct in `LocalHost::Options` /
`MeshTransportOptions`. Without a proposal, an implementer must either leave the settings unbacked
by system documents and host options or modify existing engine files without an approved proposal,
violating House Rule 6.

**Evidence:** `design/vagent-cluster-packaging.md:782–790` (Table 12 lists only P21.1–P21.6,
omitting `system_docs.cpp`), `:795–810` (§13 Settings). Compare `vagent-native-agent.md:1572` where
P20.3 explicitly covers `system_docs.cpp` and `vagent/runtime/program.hpp`.

**Fix:** Add a dedicated proposal (e.g., `P21.7`) to Table 12:

- **Change:** Add `defaultSettingSpecs()` entries for `vagent.cluster.*` in
  `apps/common/xanadu/system_docs.cpp` and declare `ClusterSettings` in `LocalHost::Options`
  (`vagent/runtime/program.hpp` or `mesh_host.hpp`).
- **Where:** `apps/common/xanadu/system_docs.cpp`, `apps/common/xanadu/vagent/runtime/program.hpp`.
- **Why it cannot live in new code:** System settings schemas and host options are centralized in
  existing core runtime files.
- **Without it:** Cluster configuration cannot be read or validated from system xanadocs.

### 2. (§12, §16) Proposal Register lacks operational costs, pricing, and fallbacks

**Problem:** House Rule 1 ("price costs, record refused alternatives") and House Rule 6 ("any change
to existing engine/runtime/core files must be a numbered proposal with files, why it cannot live in
new code, cost and fallback") require pricing costs and recording fallbacks. Under the review brief,
"a proposal lacks justification, price or fallback" is a MAJOR defect. Table 12 provides "Why it
cannot live in new code" and "Without it", but omits cost pricing and fallback definitions for
P21.1–P21.6. Furthermore, unlike `vagent-native-agent.md` (which details memory, latency, and
staging costs in §16), `vagent-cluster-packaging.md` contains no section pricing bandwidth, memory,
or disk overheads for CAS staging and swarm fetches.

**Evidence:** `design/vagent-cluster-packaging.md:782–790` (Table 12 omits price and fallback
columns); lines 870–885 (jumps directly from §15 Refused Alternatives to §16 Open Questions without
a Properties and Costs section).

**Fix:**

1. Add "Cost" and "Fallback" columns or subsections to Table 12 for P21.1–P21.6 (pricing queue
   memory bounds, CAS disk usage, descriptor overhead, and defining explicit fallback paths such as
   falling back to synchronous refusal or local-only execution if swarm distribution is disabled).
1. Add a short "Properties and Costs" section detailing the operational prices of package
   distribution: CAS storage footprint, QUIC Lane 1 bandwidth for inline bundles, and
   `PendingProgramFetch` queue memory.

## MINOR

### 1. (§2, §4 K1, §6.1, §10.2) Inconsistent hash algorithm naming for `programRef`

**Problem:** Section 4 Ruling K1 (line 165) specifies:

```text
programRef = SHA-256(L(kProgramFormat) || L("vagent-package/1") || L(kind) || L(canonical_body) || L(targets_digest))
```

However, §2 Glossary (line 115) defines `programRef` as a "Blake3/SHA-256 digest", §6.1 (lines
384–385) lists `blake3-a8f2...vnpkg` beside `sha256-4b10...vnpkg`, and §10.2 (line 710) shows
`vagent package inspect` output as `Logical Ref: blake3:9f8e7d6c5b4a...`. In existing code
(`program.cpp:26`), `refOf` computes a 64-character SHA-256 hex string.

**Evidence:** `design/vagent-cluster-packaging.md:115`, `:165`, `:384–385`, `:710`.

**Fix:** Standardize on SHA-256 across all sections. Update line 710 to print
`Logical Ref: sha256:9f8e7d6c5b4a...` (or raw 64-hex), align line 115 to state SHA-256, and
harmonize the filenames in §6.1.

### 2. (§Contents) Table of Contents link text drifts from section headings

**Problem:** The Table of Contents link display text on five entries has drifted from the actual
`##` section headings:

- Line 23: link text `[6. Content-Addressed Store & Layout]` drops `(CAS)` and `Local Deployment`
  from `## 6. Content-Addressed Store (CAS) & Local Deployment Layout`.
- Line 25: link text `[8. Distribution Protocol over Swarm]` drops the prefix `Internode` and
  replaces `MeshSwarmTransport` with `Swarm` from
  `## 8. Internode Distribution Protocol over MeshSwarmTransport`.
- Line 26: link text `[9. Authority Ledger & Security]` drops `Across the Cluster` from
  `## 9. Authority Ledger & Security Across the Cluster`.
- Line 29: link text `[12. Proposed Changes (`P21.1`–`P21.6`)]` drops `to Existing Code` from
  `## 12. Proposed Changes to Existing Code (`P21.1`–`P21.6`)`.
- Line 31: link text `[14. Verification, Test Fixtures & Probes]` drops the word `Escape` from
  `## 14. Verification, Test Fixtures & Escape Probes`.

**Evidence:** `design/vagent-cluster-packaging.md:23, 25, 26, 29, 31` vs lines 373, 540, 648, 778,
813\.

**Fix:** Synchronize the link text in lines 23, 25, 26, 29, and 31 to match the actual heading text.

### 3. (§4 K3) Pseudocode declaration syntax confusion for bundled companion libraries

**Problem:** Line 198 states `- **Bundled Libraries (bundle "libmyengine.so.1"):**`, mirroring line
195's `- **System Libraries (soname "libncursesw.so.6"):**` as if `bundle` were a surface language
declaration or native builder method. In `vagent-native-agent.md` §6.2 and §6.4, module code only
declares `soname` via `b.library(name, soname, ...)`. Companion libraries are declared in the
package manifest's `bundled_libs` table (§5.2) and on the CLI via `--bundle-lib` (§10.1).

**Evidence:** `design/vagent-cluster-packaging.md:198` vs `vagent-native-agent.md:553`.

**Fix:** Clarify that module declarations continue to use `soname "libmyengine.so.1"`. The host
loader resolves declared `soname`s against staged `bundled_libs` in `<staging_dir>/lib/` before
falling back to system paths.

### 4. (§1.2) Unqualified sibling document cross-reference in §1.2

**Problem:** Line 70 refers to `P20's packaging artifact (.vnprog, §7.1)`. Unlike surrounding lines
(e.g., lines 69, 85, 90, 101, 106) which explicitly cite `vagent-native-agent.md §X.Y`, line 70
omits the document title, creating ambiguity with local section references.

**Evidence:** `design/vagent-cluster-packaging.md:70`.

**Fix:** Qualify the reference: `(.vnprog, vagent-native-agent.md §7.1)`.

### 5. (§12 P21.5) Wording implies adding `sendBundle` / `ingestBundle` which already exist

**Problem:** Proposal P21.5 describes its change as adding `.vnpkg` inline bundle streaming
(`sendBundle` / `ingestBundle`) and torrent pull hooks in `vortex/mesh_swarm.{hpp,cpp}` and
`vortex/mesh_host.{hpp,cpp}`. However, `sendBundle` and `ingestBundle` already exist and are
implemented on `MeshSwarmTransport` (`mesh_swarm.hpp:75–76, 90` and `mesh_swarm.cpp:600, 701`).

**Evidence:** `design/vagent-cluster-packaging.md:788` vs
`apps/common/xanadu/vortex/mesh_swarm.hpp:75–76, 90`.

**Fix:** Refine P21.5's description to state that it reuses the existing `sendBundle` and
`ingestBundle` methods for `.vnpkg` slice transmission and adds package announcement and torrent
pull hooks to `MeshHostTransport` / `MeshSwarmTransport`.

VERDICT: NOT CONVERGED

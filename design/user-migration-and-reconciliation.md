# User Migration, Cross-Device Permascroll Synchronization & Branch Convergence

## 1. Executive Summary

In Project Xanadu, a user's writing environment is sovereign, decentralized, and intrinsically
multi-device. Authors create, revise, and annotate documents across multiple physical machines:
personal laptops while traveling offline, primary desktop workstations at home, shared studio or
laboratory terminals, and temporary public kiosks.

When an author makes unpublished edits to a xanadoc on their laptop while disconnected from the
network, and concurrently modifies the same document on their workstation, two distinct dimensions
of divergence emerge upon reconnection:

1. **Permascroll Divergence (Primedia Address Space)**: Both offline machines independently append
   raw keystrokes to their local author permascroll (`active.primedia`). Because both machines
   started from the same pre-travel offset $P_0$, both machines assign identical character
   coordinates `localScroll 0, offset P_0` to entirely different UTF-8 byte sequences.
1. **OSMIC Hypertime Divergence (Operation DAG)**: In the document's Edit Decision List (EDL), both
   machines apply operations originating from the same common base microversion (e.g., State 4). In
   OSMIC hypertime, this forks the operation DAG into concurrent branch lineages (e.g., State `4a1`
   on the laptop versus State 5 and 6 on the workstation).

This specification establishes the architectural solution to both challenges:

- **Rejection of the "Spool Concatenation & Span Remapping" Antipattern**: Permascroll addresses are
  permanent, immutable coordinates. Shifting span offsets in stored `CompactOpNode` records violates
  address stability, breaks Merkle piece alignment, and risks silent text substitution.
- **Sovereign Device Subscroll Salting**: All device primedia spools anchor to the author's Master
  Identity Fingerprint (`btpk:<master_fingerprint>:permascroll`), salted by unique device
  identifiers (`permascroll/workstation`, `permascroll/laptop`). Ingestion binds peer segments as
  author subscrolls (`ScrollId = 1, 2, ...`). **Zero byte offsets shift, and zero binary operations
  are rewritten.**
- **Visual, Non-Destructive Convergence via Native Transclusion**: Rather than flattening divergent
  text with destructive Git-style conflict markers (`<<<<<<< HEAD`), `xuzz` visualizes parallel
  branches in the 2D Hypertime Graph and 3D Volumetric Beams (`beams.cpp`). The author synthesizes
  editions by dragging and transcluding spans across versions, producing a new synthesized state
  while preserving both historical branches permanently.
- **The 'Branch Convergence & Permascroll Reconciler' Assistant**: An integrated workflow and
  exchange format (`.xuzzpkg`) providing 1-click discovery, automated subscroll registration, and
  Vortex-driven synthesis within `system://keymap`.

______________________________________________________________________

## 2. The Fundamental Primedia Axiom & The Remapping Fallacy

### 2.1 The Disk Reality of Offline Authoring

Under the native Xanadu engine architecture (`store-slice-convergence.md` §13), a document store
directory carries **no primedia**. A document is an Edit Decision List (`ops.nodes`) composed of
64-byte `CompactOpNode` structures pointing to character spans in the author's sovereign
permascroll:

```
$XDG_DATA_HOME/xuzz/permascroll/<master_fingerprint>/active.primedia
```

When an author travels with a laptop:

1. **Base State ($S_0$)**: Both workstation and laptop share base primedia length $P_0$.
1. **Laptop Offline Edits**: The user types $N$ bytes of new text. The laptop's `active.primedia`
   grows from $P_0$ to $P_0 + N$. Its `ops.nodes` records operations with `scrollId = 0` and span
   starts in $[P_0, P_0 + N)$.
1. **Workstation Local Edits**: The user types $M$ bytes on the workstation. Its `active.primedia`
   grows from $P_0$ to $P_0 + M$. Its `ops.nodes` records operations with `scrollId = 0` and span
   starts in $[P_0, P_0 + M)$.

If the laptop store directory is copied naively to the workstation, the workstation attempts to
resolve `(scroll=0, offset=P_0 + \delta)` against its *own* `active.primedia`, silently reading
workstation keystrokes instead of laptop keystrokes. This is the **Silent Coordinate Substitution
Bug**.

### 2.2 Why Spool Concatenation & Offset Rewriting Fails

A superficial proposal might attempt to "merge" the two files by appending the laptop's byte slice
$[P_0, P_0 + N)$ to the end of the workstation spool at $[P_0 + M, P_0 + M + N)$, and adding an
offset delta $\Delta = M$ to every `CompactOpNode` in the laptop store:

$$
\text{spanStart}' = \text{spanStart} + \Delta
$$

This approach is an architectural antipattern that is fundamentally prohibited in `xanadu`:

1. **Violation of Coordinate Immobility**: In Project Xanadu, a primedia address is not a memory
   offset; it is a permanent coordinate in the author's cosmic permascroll. Addresses never move.
1. **Destruction of Op Immutability**: Sealed `SegmentedOpsSpool` segments are mapped read-only
   (`PROT_READ`). Rewriting binary op files invalidates Merkle tree piece hashes and corrupts
   pre-computed hash links.
1. **Invalidation of External Citations**: Any external links, private reader bookmarks, or
   `system://activity` logs generated on the laptop pointing to the original offsets become dangling
   or corrupted pointers.

______________________________________________________________________

## 3. Sovereign Device Subscroll Architecture

The principled resolution leverages **Device Subscroll Salting**, anchoring all devices to the
author's Master Identity while partitioning local capture spools.

```
                  ┌────────────────────────────────────────────────────────┐
                  │                 Master Identity Key                    │
                  │        Fingerprint: 64-hex SHA-256 (Master CA)         │
                  └───────────────────────────┬────────────────────────────┘
                                              │
                      ┌───────────────────────┴───────────────────────┐
                      │ Issues X.509 Delegation Certs                 │
                      ▼                                               ▼
         ┌─────────────────────────┐                     ┌─────────────────────────┐
         │ Workstation Device Key  │                     │    Laptop Device Key    │
         │ Device ID: "workstation"│                     │    Device ID: "laptop"  │
         └────────────┬────────────┘                     └────────────┬────────────┘
                      │                                               │
                      │ Appends Keystrokes                            │ Appends Keystrokes
                      ▼                                               ▼
         ┌─────────────────────────┐                     ┌─────────────────────────┐
         │ Subscroll:              │                     │ Subscroll:              │
         │ permascroll/workstation │                     │ permascroll/laptop      │
         │ Base offsets: [0 ... M) │                     │ Base offsets: [0 ... N) │
         └────────────┬────────────┘                     └────────────┬────────────┘
                      │                                               │
                      └───────────────────────┬───────────────────────┘
                                              │
                                              │ Offline Exchange / Reconciler Ingestion
                                              ▼
                                 ┌─────────────────────────┐
                                 │ Workstation Store       │
                                 │ Scroll 0: workstation   │
                                 │ Scroll 1: laptop (peer) │
                                 │ Zero byte offsets shift │
                                 └─────────────────────────┘
```

### 3.1 Device Salt Derivation

In `apps/common/xanadu/user_permascroll.hpp`, each machine initializes its local
`UserPermascroll::Config` with an explicit device identifier:

```cpp
struct Config {
  std::filesystem::path storageDir;
  identity::Fingerprint masterIdentity;
  MutableKeys deviceKeys;
  std::string deviceId{"main"}; // e.g., "workstation", "laptop"
  std::size_t segmentAlignmentBytes{64UZ * 1024};
};
```

The BEP 46 mutable DHT salt is derived systematically:

```cpp
currentScroll_.salt = (config_.deviceId == "main")
                        ? "permascroll"
                        : "permascroll/" + config_.deviceId;
```

And the globally published scroll identifier resolves to:

$$
\text{globalScrollKey} = \texttt{"btpk:<master\_fingerprint>:permascroll/"} + \text{deviceId}
$$

### 3.2 Multi-Device Storage Topology on Disk

To support seamless local multi-device reconciliation without network swarms, the storage directory
hierarchy under `$XDG_DATA_HOME/xuzz/permascroll/<master_fingerprint>/` is structured as:

```
~/.local/share/xuzz/permascroll/<master_fingerprint>/
├── active.primedia                          <- Symlink to local machine's active spool
├── devices/
│   ├── workstation/
│   │   ├── active.primedia                  <- Keystrokes typed on workstation
│   │   └── segments/                        <- BitTorrent sealed segments
│   ├── laptop/
│   │   ├── active.primedia                  <- Ingested keystrokes from laptop
│   │   └── segments/
│   └── public_kiosk_20261008/
│       └── active.primedia                  <- Ephemeral session spool
```

### 3.3 Zero-Shift Ingestion Semantics

When the workstation ingests an offline session from the laptop:

1. The laptop's unsealed slice $[P_0, P_0 + N)$ is copied into `devices/laptop/active.primedia`.
1. The workstation registers `permascroll/laptop` as an author subscroll in the store's table,
   allocating internal `ScrollId = 1`.
1. The laptop's operations are imported into `SegmentedOpsSpool`. In every imported operation node:
   - `scrollId` is mapped from `0` to `1`.
   - `spanStart` and `spanLength` remain **strictly unchanged**.
1. **Result**: Both sets of keystrokes retain their exact character coordinates. Zero bytes shift.

______________________________________________________________________

## 4. OSMIC Hypertime Branching & Divergence Mechanics

When both machines edit the same document concurrently, they fork the document's Edit Decision List.

```
                           State 1 ──> State 2 ──> State 3 ──> State 4 (Base Fork)
                                                                 │
                                         ┌───────────────────────┴───────────────────────┐
                                         ▼                                               ▼
                                      State 5                                         State 4a1
                                  (Workstation)                                       (Laptop)
                                         │                                               │
                                         ▼                                               ▼
                                      State 6                                         State 4a2
                                (Workstation Head)                                  (Laptop Head)
                                         │                                               │
                                         └───────────────────────┬───────────────────────┘
                                                                 │
                                                                 │ Transclusion Synthesis
                                                                 ▼
                                                              State 7
                                                       (Synthesized Edition)
```

### 4.1 Bijective Base-26 Branching (`MicroversionId`)

OSMIC avoids arbitrary text merge commits by tracking an append-only directed acyclic graph (DAG) of
operations:

- **Mainline States**: Sequential integers `1`, `2`, `3`, `4`.
- **First Divergent Branch**: When the workstation commits State 5, the laptop's concurrent edit
  cannot claim State 5. It automatically branches as `4a1` (`parent=4, branch=1, number=1`).
- **Sequential Branch Steps**: Subsequent laptop keystrokes advance along the branch: `4a2`, `4a3`.
- **Hierarchical Branching**: If a third device diverges from `4a2`, it forks as `4a2b1`.

In `apps/common/xanadu/store.cpp`, `Store::apply()` handles concurrent operation admission:

```cpp
auto onward = parent.next();
if (!opsSpool.contains(onward)) {
  putOp(onward, op);
  return onward;
}
std::uint32_t ordinal = 1;
while (opsSpool.contains(parent.branch(ordinal))) {
  ordinal++;
}
auto branched = parent.branch(ordinal);
putOp(branched, op);
return branched;
```

Divergence is never a collision error; it is a first-class branch in the hypertime manifold.

______________________________________________________________________

## 5. Visual Representation: 2D Graph & 3D Volumetric Beams

Users are never forced to navigate raw base-26 ordinals or hex node identifiers. `xuzz` provides two
synchronized visual representations.

### 5.1 2D Topological Hypertime Graph (`hypertime_graph.cpp`)

The Hypertime Graph overlay presents the evolution of the document across two spatial dimensions:

1. **Topological Branch Lanes**: Mainline states occupy Lane 0. The laptop's branch `4a1...4a2`
   occupies Lane 1, rendered with a distinct lineage hue (e.g., Amber or Emerald).
1. **Operation Badges**: Each microversion disc displays its primary operation kind:
   - `'I'` (Insert): Keystrokes appended.
   - `'D'` (Delete / Limbo): Spans removed to limbo.
   - `'T'` (Transclude): Spans quoted from another document or branch.
   - `'S'` (Structure): ZigZag manifold cells linked or reordered.
1. **Dynamic Author Aliases**: Floating badges automatically label branches (e.g.,
   `"Workstation Draft"`, `"Laptop Flight Edits"`), positioned dynamically to prevent edge
   occlusion.
1. **Temporal Scrubber**: A bottom scrubber allows continuous scrubbing across historical states
   with real-time text previews.

### 5.2 Mathematical Diffing & 3D Optical Beams (`beams.cpp`)

When the author selects both branch tips (`State 6` and `State 4a2`) in the graph and clicks **"Open
in 3D"**:

1. **Exact Coordinate Diffing (`Store::diffVersions`)**: Instead of heuristic line diffing
   (LCS/Myers), Xanadu computes exact set intersections over character coordinates
   `(scroll, address)`:
   - **Universal Spans (Identity Gold)**: Characters present in both versions.
   - **Unique Spans (Mint)**: Additions present exclusively in one branch.
   - **Limbo Spans (Crimson)**: Characters deleted in that branch.
1. **Side-by-Side 3D Layout**: Both document versions render side-by-side in 3D world space. The
   camera automatically aligns to the first site of divergence.
1. **Volumetric Optical Beams**:
   - **Identity Gold Beams (`0xFFD700FF`)**: Luminous glass ribbons bridge across the 3D space,
     connecting identical spans between the workstation draft and laptop draft.
   - **Unique Passages**: Glow in soft mint with no connecting beams, immediately showing what was
     written on each machine.

______________________________________________________________________

## 6. Frictionless Synthesis Workflow: Native Transclusion

Traditional version control forces authors to resolve conflicts by destroying one version or
introducing syntactical markers (`<<<<<<< HEAD`). In `xuzz`, synthesis is **non-destructive,
traceable, and native**.

```
┌────────────────────────────────────────────────────────────────────────────────────────┐
│                                 Xuzz 3D Synthesis View                                 │
├───────────────────────────────────┬────────────────┬───────────────────────────────────┤
│        Workstation (State 6)      │  Optical Beams │          Laptop (State 4a2)       │
├───────────────────────────────────┼────────────────┼───────────────────────────────────┤
│ Section 1: Introduction           │════════════════│ Section 1: Introduction           │
│ (Universal shared text)           │════ Gold ══════│ (Universal shared text)           │
│                                   │                │                                   │
│ Section 2: Methodology (Draft A)  │   [No Beams]   │ Section 2: Methodology (Draft B)  │
│ [Workstation unique additions]    │                │ [Laptop unique additions]         │
│                                   │                │                                   │
│                                   │  <-- Drag Mint ├───────────────────────────────────┤
│                                   │      Span Here │ [Adopt Laptop Paragraph into Head]│
│                                   │                │ [Keep as Alternative Edition]     │
└───────────────────────────────────┴────────────────┴───────────────────────────────────┘
```

### 6.1 Step-by-Step Synthesis Walkthrough

1. **Launch Convergence View**: The author selects `State 6` and `State 4a2` in `HypertimeGraph` and
   opens them side-by-side.
1. **Reviewing Divergent Spans**: Luminous gold beams connect the shared sections. Divergent
   paragraphs stand out clearly in mint highlighting.
1. **Interactive Transclusion**:
   - The author highlights the superior phrasing written on the laptop in Section 2.
   - The author drags the selection across the 3D viewport into Section 2 of the workstation draft
     (or clicks **"Adopt Selected Span"**).
   - Alternatively, items can be dropped into the Pouch Drawer (`system://pouches`) for staging.
1. **State Advancement (State 7)**:
   - `Store::transclude()` records an `OpKind::Transclude` operation.
   - The document advances to State 7 (Synthesized Edition).
   - A new gold beam instantly connects State 7 to the laptop's State 4a2, verifying that the text
     shares the identical primedia coordinate.
1. **Historical Invariant**: Both parent branches (`State 6` and `State 4a2`) remain fully preserved
   in hypertime forever. The author can jump back to either branch at any time.

______________________________________________________________________

## 7. Session Exchange Container (`.xuzzpkg`)

To make transferring sessions between machines completely painless without requiring cloud servers
or BitTorrent swarms, `xuzz` defines the **Session Package Container** (`.xuzzpkg`).

### 7.1 Container Specification & Architecture

A `.xuzzpkg` is a single-file POSIX `ustar` tar archive with optional transparent `libzstd` frame
compression and decompression. When stored on disk or transmitted over peer transports, the archive
encapsulates five mandatory member files:

```
session.xuzzpkg (.tar or .tar.zst)
├── MANIFEST.tsv           <- Hardened TSV metadata: author fingerprint, versions, offsets
├── primedia.slice         <- Raw unsealed primedia bytes typed since fork point P_0
├── ops.nodes              <- Binary CompactOpNode records (exact 64-byte alignment)
├── device.crt             <- Base64 DER RFC 8410 X.509 v3 device delegation certificate
└── signature.sig          <- Raw 64-byte Ed25519 signature over raw MANIFEST.tsv
```

### 7.2 Member File Specifications

1. **`MANIFEST.tsv` (Metadata Manifest)**:

   - Encoded as UTF-8 tab-separated values.
   - Enforces the project-wide **Hardened TSV Standard**:
     - Strict field key format matching regex `^[a-z_][a-z0-9_]*$`.
     - Zero tolerance for carriage returns (`\r` / `0x0D`), triggering immediate parse failure.
     - Prohibition of duplicate keys; duplicate entries abort with `DuplicateKey`.
     - Mandatory fields: `manifest_version`, `master_fingerprint`, `device_id`, `device_key`,
       `base_version`, `head_version`, `primedia_offset`, `primedia_length`, `timestamp`.
     - Optional extensible fields preserved in `extraFields`.

   ```tsv
   manifest_version	1
   master_fingerprint	a1b2c3d4e5f6... (64-hex SHA-256)
   device_id	laptop
   device_key	e5f6a7b8c9d0... (64-hex Ed25519)
   base_version	4
   head_version	4a2
   primedia_offset	1048576
   primedia_length	4096
   timestamp	1791480000
   ```

1. **`primedia.slice` (Unsealed Primedia Payload)**:

   - Raw binary bytes representing new keystrokes appended on the authoring device since base offset
     `primedia_offset`.
   - Length must match `primedia_length` in the manifest bit-for-bit.

1. **`ops.nodes` (Operation Records)**:

   - Packed sequence of 64-byte `CompactOpNode` structures representing document operations minted
     on the peer device since `base_version`.
   - Total byte size must be an exact integer multiple of `sizeof(CompactOpNode)` (64 bytes).

1. **`device.crt` (X.509 Delegation Certificate)**:

   - Single-line Base64-encoded DER representation of an RFC 5280 / RFC 8410 X.509 v3 certificate.
   - Issued and signed by the Master Root CA Key (`master_fingerprint`), granting signing authority
     to the workstation/laptop key (`device_key`).

1. **`signature.sig` (Cryptographic Detached Signature)**:

   - Exactly 64 bytes of raw binary Ed25519 signature computed over the exact byte stream of
     `MANIFEST.tsv` using the authoring device's private key.

### 7.3 Error Taxonomy (`SessionValidationError`)

Implementations return explicit error codes via `std::expected<T, SessionValidationError>`:

| Error Code               | Meaning                                                                 |
| :----------------------- | :---------------------------------------------------------------------- |
| `MissingManifest`        | Archive lacks `MANIFEST.tsv`                                            |
| `MissingPrimedia`        | Archive lacks `primedia.slice`                                          |
| `MissingOps`             | Archive lacks `ops.nodes`                                               |
| `MissingDeviceCert`      | Archive lacks `device.crt`                                              |
| `MissingSignature`       | Archive lacks `signature.sig`                                           |
| `CorruptArchive`         | Archive is malformed, truncated, or invalid tar header                  |
| `CorruptManifest`        | Manifest line structure is invalid                                      |
| `DuplicateKey`           | Manifest contains duplicate field names                                 |
| `InvalidKeyFormat`       | Manifest key violates regex `^[a-z_][a-z0-9_]*$`                        |
| `CarriageReturnRejected` | Manifest contains forbidden `\r`                                        |
| `MissingRequiredField`   | Manifest is missing a mandatory key                                     |
| `InvalidFieldFormat`     | Field value cannot be parsed into expected numeric type                 |
| `IdentityMismatch`       | Author binding check failed: `master_fingerprint` does not match target |
| `InvalidDeviceKey`       | Public key format or length is invalid                                  |
| `InvalidSignature`       | Ed25519 signature verification failed                                   |
| `InvalidDeviceCert`      | Delegation certificate signature or constraints invalid                 |
| `CorruptOpsNodes`        | `ops.nodes` size is not a multiple of 64 bytes                          |
| `PrimediaLengthMismatch` | `primedia.slice` byte count does not match manifest                     |
| `IoError`                | Filesystem read/write error                                             |
| `SerializationError`     | Archive packing or compression error                                    |

### 7.4 C++ Programming Interface (`session_container.hpp`)

The interface provides chaining setters and explicit `std::expected` return types:

```cpp
namespace xanadu {

struct SessionPackage {
  std::uint32_t manifestVersion{1};
  std::string masterFingerprint;
  std::string deviceId;
  std::string deviceKey;
  std::string baseVersion;
  std::string headVersion;
  std::uint64_t primediaOffset{0};
  std::uint64_t primediaLength{0};
  std::uint64_t timestamp{0};
  std::vector<std::pair<std::string, std::string>> extraFields;

  std::vector<std::uint8_t> primediaSlice;
  std::vector<CompactOpNode> opsNodes;
  std::string deviceCert;
  std::array<std::uint8_t, 64> signature{};

  SessionPackage *setMasterFingerprint(std::string fp) noexcept;
  SessionPackage *setDeviceId(std::string id) noexcept;
  SessionPackage *setPrimediaSlice(std::span<const std::uint8_t> slice);
  SessionPackage *setOpsNodes(std::vector<CompactOpNode> ops) noexcept;
};

[[nodiscard]] std::expected<std::vector<std::uint8_t>, SessionValidationError>
exportPackage(const SessionPackage &pkg);

[[nodiscard]] std::expected<SessionPackage, SessionValidationError>
importPackage(std::span<const std::uint8_t> archiveBytes,
              std::optional<std::string_view> expectedMasterFp = std::nullopt);

} // namespace xanadu
```

### 7.5 CLI & GUI Exchange Commands

- **Export Session (Laptop)**:
  `xuzz session export --doc "design-spec" --out ~/laptop_flight_edits.xuzzpkg`
- **Import Session (Workstation)**: `xuzz session import --in ~/laptop_flight_edits.xuzzpkg`

On import, `xuzz`:

1. Verifies that `master_fingerprint` matches the workstation's author identity.
1. Verifies the X.509 `device.crt` and `signature.sig`.
1. Writes `primedia.slice` into
   `~/.local/share/xuzz/permascroll/<fp>/devices/laptop/active.primedia`.
1. Appends operations into the target store's `ops.nodes` under branch `4a1...`.
1. Preserves the **Zero-Shift Invariant**: character spans and primedia slices round-trip
   bit-for-bit with exact coordinate stability.
1. Automatically launches the **Branch Convergence Assistant** modal.

______________________________________________________________________

## 8. The 'Branch Convergence Assistant' UI Specification

The Convergence Assistant is a native UI overlay in `xuzz` designed to guide authors through
reconciliation with zero cognitive friction.

### 8.1 UI Components & Layout

1. **Convergence Banner**: Appears at the top of the window when divergent branch tips are detected:
   `[!] Concurrent edits detected from "laptop" (State 4a2). [Review & Converge] [Dismiss]`
1. **Comparative 3D Canvas**: Splits the canvas into dual side-by-side world-space panels with
   active gold beams.
1. **Convergence Action Palette**: A floating tool panel offering structured reconciliation actions:
   - **Adopt Selected Span (`Ctrl+Shift+A`)**: Transcludes the highlighted passage from the peer
     branch into the active head.
   - **Retain Both as Named Editions (`Ctrl+Shift+E`)**: Names both branch tips (e.g.,
     `"Edition: Concise"` vs `"Edition: Detailed"`) and concludes reconciliation without
     modification.
   - **Synthesize New Edition (`Ctrl+Shift+S`)**: Creates a fresh microversion inheriting all
     uncontested spans and opens the editor for manual finishing.

### 8.2 Vortex Keymap Binding

In adherence to project rules, all reconciliation actions are registered in the `system://keymap`
system store via Vortex function calls:

```vortex
(defaction "xuzz.convergence.adopt_span" [src_span target_pos]
  (xanadu.store/transclude (active_document) target_pos src_span)
  (xanadu.ui/refresh_beams))

(defaction "xuzz.convergence.create_synthesis_edition" [base_version peer_version]
  (xanadu.store/synthesize_branches (active_document) base_version peer_version))
```

______________________________________________________________________

## 9. Architectural Invariants & Security Rules

1. **Coordinate Stability Invariant**: Primedia byte coordinates MUST NOT be shifted or relocated
   during cross-device reconciliation. All multi-device ingestions MUST allocate distinct device
   subscroll descriptors (`ScrollId`).
1. **Cryptographic Identity Verification**: An incoming session package MUST NOT be ingested into an
   author's permascroll unless its X.509 device certificate traces back to the author's Master
   Identity Key.
1. **Non-Destructive Hypertime Guarantee**: Importing a peer branch MUST NOT overwrite or prune
   existing microversions in `ops.nodes`. Branches MUST coexist in the DAG until explicitly
   transcluded.
1. **Offline Air-Gap Safety**: Exchange bundles (`.xuzzpkg`) MUST NOT require network connectivity,
   DHT lookups, or external trackers to verify and import.

______________________________________________________________________

## 10. Summary & Roadmap

| Phase       | Milestone                   | Deliverables                                                                                                       |
| :---------- | :-------------------------- | :----------------------------------------------------------------------------------------------------------------- |
| **Phase 1** | Subscroll Storage Hierarchy | Implement `permascroll/<fp>/devices/<device_id>/` topology and multi-spool registration in `user_permascroll.cpp`. |
| **Phase 2** | Session Container Tooling   | Implement `.xuzzpkg` packaging, `xuzz session export`, and `xuzz session import` CLI commands.                     |
| **Phase 3** | Convergence Assistant UI    | Implement dual-view 3D comparative canvas, diffing beam triggers, and the Convergence Palette in `xuzz`.           |
| **Phase 4** | Vortex Integration          | Bind convergence actions into `system://keymap` and implement automated synthesis routines in Vortex.              |

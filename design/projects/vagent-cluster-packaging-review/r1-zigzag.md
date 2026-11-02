# Round 1 critique: Xanadu / ZigZag Purist (vagent-cluster-packaging.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 1 MAJOR, 3 MINOR.

## Audit: P17 Alignment, Manifold Grounding & Authority Ledger

### 1. `programRef` & `program_body` vs. P17 Sealed Program Store (§4 K1, §5.1–5.2)

- **Clean Transport Boundary:** The `.vnpkg` container format is cleanly positioned at the network
  transport and cluster distribution boundary, operating outside document stores in the same manner
  as `.vnprog` (`vagent-native-agent.md` §7.1).
- **Primedia & Permascroll Purity:** Section 15 explicitly refuses embedding multi-megabyte native
  binaries inside the author's permascroll store. Permascrolls store immutable UTF-8 primedia spans
  and edit decisions, not compiled platform binaries. Payload binaries reside in the filesystem CAS
  (`<deployment>/packages/cas/<sha256>`) and are staged into isolated guest directories
  (`<staging_dir>/lib/`) with file mode `0500`, preserving Nelsonian transclusion provenance and OS
  paging efficiency.
- **Deterministic Compilation to Base Store + Manifold:** On the receiving node,
  `CompiledProgram::fromUnit` compiles the canonical abstract program (`Unit` or `op_log`) into a
  sealed base `Store` and `programManifold` per P17 §1.3 and §2.2. Because AST lowering and
  `zigzag::promote` operate deterministically from genesis, the resulting canonical base `CellRef`s
  (1..M) are bit-for-bit identical across all nodes. Running agents attach ephemeral overlays
  (`ArenaManifold(&programManifold)`), allowing mutable state deltas (`PublishedValue`) to migrate
  seamlessly across nodes without re-compilation or graph reconstruction.
- **Tension in §5.1 and §8.2:** As detailed in MAJOR 1 below, §5.1 and §8.2 mistakenly introduce the
  phrases "sealed Store scroll" and "program store scroll", which conflates an operations spool with
  a primedia scroll and contradicts §5.2's canonical bencode schema.

### 2. Authority Ledger & Provenance Integration (§5.1, §9)

- **Strict 2-Degree Rank Invariant Preserved:** Ruling K1's single logical `programRef` prevents
  topological fan-in in the private authority store (`authorities/`, `vagent-native-agent.md`
  §10.5). Because a single `programRef` covers all target architectures and bundled libraries,
  approvals require exactly one head cell. The ledger maintains a 1D rank of kinds on `d.authority`,
  1D event ranks on `d.approval`, and a chronological chain on `d.decision`.
- **Transclusion over Link Fan-In:** Decisions transclude (quote) approval and declaration spans
  rather than linking, preventing many-to-one link fan-in and link eviction. Revocations remain
  strictly 1-to-1 along `d.revocation`.
- **Store & Permascroll Isolation:** The authority ledger continues to use its private permascroll
  (`authorities/permascroll/`), keeping audit history completely isolated from published document
  permascrolls.
- **No Transport Leakage into Cells (R13):** No torrent locators, magnet URIs, or InfoHashes are
  injected into zzstructure cells. Swarm locators live exclusively in the `.vnpkg` bencoded manifest
  and CAS index, satisfying Rule R13.

## BLOCKING

None.

## MAJOR

### 1. (§5.1, §8.2) Inconsistent `program_body` taxonomy and ontological category error

**Problem:** §5.1 (line 286) states that for `"pure"` packages, `program_body` "contains the
canonical `Unit` or sealed `Store` scroll." §8.2 (lines 592–593) states that for the fast-path
`SpoolSliceBundle`, "Node A packs the manifest, program store scroll, and target binary slices..."
This creates three severe problems:

1. *Ontological & Hypermedia Violation:* In Nelsonian hypermedia and `store-slice-convergence.md`
   (R1–R14), a `Store` is an operations spool (`SegmentedOpsSpool` of `CompactOpNode` records)
   representing edit decisions and structure maps over concatext. A `Scroll` (or `Permascroll`) is a
   1D sequence of append-only primedia characters. Calling a store a "scroll" is an ontological
   category error.
1. *Internal Inconsistency & Nondeterministic `programRef`:* §5.2 (lines 343–344) explicitly defines
   `program_body` as: "For `"pure"` and `"source+native"`, this is the canonical bencoded `Unit`
   representation." If §5.1 allows `program_body` to be either a bencoded `Unit` OR a "sealed Store
   scroll", the exact same pure vagent program produces two completely different `canonical_body`
   byte strings, yielding diverging `programRef` digests under Ruling K1. This directly violates
   Ruling K6 (deterministic packaging) and Ruling D1 (deterministic multi-node identity).
1. *Execution Pipeline Disconnect:* `CompiledProgram` on the receiving node has only
   `fromSource(source, name)` and P20.2 `fromUnit(Unit, name, ref)`. There is no `fromStore` or
   `fromScroll` entry point. In §8.1 step 11 and §14.1 step 5, Node B explicitly compiles via
   `CompiledProgram::fromUnit()`. If `program_body` contained a serialized Store, `fromUnit` could
   not ingest it.

**Evidence:**

- `design/vagent-cluster-packaging.md` §4 K1 (lines 159–171), §5.1 (line 286), §5.2 (lines 342–344),
  §8.2 (lines 591–596)
- `design/store-slice-convergence.md` §§1–3 (lines 13–104), R1–R8
- `design/vagent-shared-program-and-live-upgrade.md` §1.3 (lines 110–139), §2.1–2.2 (lines 142–238)
- `apps/common/xanadu/vagent/runtime/program.hpp:74`, `:114–126`
- `apps/common/xanadu/vagent/runtime/program.cpp:50–83`

**Fix:**

- Remove "or sealed Store scroll" from §5.1 line 286, leaving `program_body` as the canonical
  bencoded `Unit` representation (identical to `"source+native"`).
- In §8.2 lines 592–593, replace "program store scroll" with the canonical package manifest (which
  embeds `program_body`).
- Explicitly state that `.vnpkg` packages distribute the canonical abstract program input (`Unit` or
  `op_log`), and that compilation on the receiving node via `CompiledProgram::fromUnit` produces the
  sealed base `Store` and `programManifold` locally per P17 §1.3 and §2.2.

## MINOR

- **Underspecified canonical parameters for BitTorrent `info_hash` calculation (§5.2, §7.1, §11).**
  Ruling K1 incorporates `targets_digest` into `programRef`, and §5.2 embeds `info_hash` for
  `module` and each `bundled_libs` entry in the `targets` dictionary. However, a BitTorrent
  `info_hash` depends on piece length (`pieceLength` in `torrent.hpp:234, :266`) and the torrent
  name string. If one packager defaults to 256 KiB (`torrent.hpp`) and another to 64 KiB
  (`mesh_value.hpp:146` / `store-slice-convergence.md`), the resulting `info_hash` values will
  diverge for identical binary files, causing `targets_digest` and `programRef` to diverge and
  violating Ruling K6. Specify the canonical `pieceLength` (e.g. 256 KiB) and canonical naming (flat
  basename) used for `makeTorrent`.
- **Imprecise provenance citation for Ed25519 canonical bencode dictionary (§5.2).** §5.2 lines
  357–358 states: *"provenance: Authoring identity metadata matching `xanadu::SignedProvenance`
  (`common/xanadu/publication.hpp`)."* In the codebase, `SignedProvenance` is declared in
  `provenance.hpp:122` as a GnuPG TSV record (`std::string tsv; std::string signature;`). The
  Ed25519 canonical bencode dictionary in §5.2 (`author`, `device`, `timestamp`, `public_key`)
  follows the mesh's Ed25519 identity scheme (e.g. `PublishedValue` in `mesh_value.hpp:33–43`).
  Update the citation to avoid confusing GnuPG TSV provenance with mesh Ed25519 publication
  metadata.
- **Distinguish CLI display prefix from internal `programRef` encoding (§4 K1, §10.2).** In §10.2
  (line 715), the CLI inspect output displays `Logical Ref: sha256:9f8e7d6c5b4a...`. In
  `program.cpp:26` and `vagent-native-agent.md` §7.1, `programRef` is stored and compared internally
  as a bare 64-character lowercase hex string (`toHex32(sha256Digest(...))`). Clarify that the
  `sha256:` prefix is a CLI presentation badge only, and that `programRef()` on `AgentProgram` /
  `CompiledProgram` returns the bare 64-character hex string.

VERDICT: NOT CONVERGED

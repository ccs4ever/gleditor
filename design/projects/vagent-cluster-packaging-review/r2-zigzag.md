# Round 2 critique: Xanadu / ZigZag Purist (vagent-cluster-packaging.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings

- **R1 M1 (§5.1, §8.2: "Store scroll" terminology and `program_body` taxonomy):** RESOLVED. The
  confusing "Store scroll" and "program store scroll" phrases have been completely removed. §5.1,
  §5.2, and §8.2 now unambiguously specify that `program_body` contains exclusively the canonical
  bencoded `Unit` AST for both `"pure"` and `"source+native"` packages (and canonical `op_log` for
  `"native"`). On the receiving node, the background package worker pool compiles `Unit` via
  `CompiledProgram::fromUnit` into the local P17 sealed base `Store` and `programManifold` (§5.1,
  §8.2).

## Verification of Round 1 MINOR findings and v0.3 changes

- **R1 m1 (Canonical BitTorrent `pieceLength`):** RESOLVED. §4 K6 explicitly standardizes piece
  length at `pieceLength = 262144` (256 KiB) with a flat basename `<sha256>`, guaranteeing
  deterministic BitTorrent `info_hash` and `targets_digest` calculations across all packagers.
- **R1 m2 (Ed25519 provenance citation):** RESOLVED. §2, §4 K2, §5.2, §9.2, and §15 replace legacy
  GPG TSV `SignedProvenance` citations with `PackageProvenance`, verifying in-tree Ed25519
  signatures (`xanadu::PublicKey`, `Signature64`, `verifySignature`).
- **R1 m3 (Bare 64-hex string vs display prefix):** RESOLVED. §4 K1 and §10.2 clarify that
  `programRef` is stored, compared, and indexed internally as a bare 64-character lowercase hex
  string, with `sha256:` serving solely as a CLI presentation label.
- **Topological & Convergence Audit of v0.3:**
  - The dedicated `PackageBundle` envelope on QUIC Lane 1 (§4 K2, §8.2, P21.5) cleanly separates
    container packages from Xanadu `Store` slices (`SpoolSliceBundle`), maintaining strict layer
    hygiene and eliminating type confusion in `ingestBundle`.
  - Cross-platform library mapping (`canonical_soname`, `system_library_aliases` in `targets`)
    operates purely at staging and dynamic loading time, without mutating `op_log`, `Unit`, or
    logical `programRef`.
  - Authority ledger integration (§9.1) binds directly to the single logical `programRef`,
    maintaining 1D ranks on `d.authority`, 1D event ranks on `d.approval`, and transcluded spans on
    `d.decision` with zero link fan-in.
  - Multi-megabyte binary payloads remain strictly excluded from permascrolls and Store spools
    (§15), preserving Nelsonian transclusion semantics and OS paging behavior.

## BLOCKING

None.

## MAJOR

None.

## MINOR

None.

VERDICT: CONVERGED

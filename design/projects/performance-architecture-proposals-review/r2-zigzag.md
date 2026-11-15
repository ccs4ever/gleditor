# Round 2 critique: Xanadu / ZigZag Purist (performance-architecture-proposals.md)

Paths below are relative to the repo root.
Tally: 0 BLOCKING, 0 MAJOR, 2 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R1.1 (LMDB Manifold checkpoints violate R9 and hypertime branching integrity): RESOLVED — LMDB Manifold checkpoints were completely rejected and moved to Refused Alternatives (§4 #2). Cold fold replay from `ancestralPath` over the ops spool is strictly preserved in §2.2 (P2), maintaining the spool as the single source of truth.
- R1.2 (Direct indexing via cached `dimOrdinal` resurrects privileged dimensions): RESOLVED — Cached `dimOrdinal` and direct dimension indexing were rejected (§4 #3). R12 dimension uniformity is preserved in §2.3 (P3); CSR `DimLink` runs remain keyed uniformly by `DimRef` and scanned linearly.
- R1.3 (Pre-lowered `DecodedOp` instruction cache abandons hyperstructural execution): RESOLVED — The monolithic linear `DecodedOp` vector cache and global manifold epoch invalidation were rejected (§4 #4). §2.5 (P5) preserves hyperstructural execution over ZigZag ranks (`d.spin`, `d.grab`, `d.step`) by replacing heap allocations in `VortexCore::wingOf` with stack-allocated `inplace_vector<CellRef, 8>` and optimizing `OpcodeKind` dispatch directly.
- R1.4 (Flat arrays indexed directly by raw `CellRef` break under the R8 `ephemeralBit` partition): RESOLVED — Raw `CellRef`-indexed flat arrays are explicitly prohibited in §2.1 (lines 103–105); open-addressed flat maps with generation tags are mandated instead, respecting R8 `ephemeralBit` (`0x8000'0000U`).
- R1.5 (Standalone WAM-style VM trail duplicates and desynchronizes from `ArenaManifold::mark()`/`release()`): RESOLVED — §2.1 (lines 78–99) harmonizes the two-tier conditional LIFO `vmTrail_` with `ArenaManifold::trail_`, ties rollback directly to `ChoicePoint::mark` (`Mark::serial`), and safely coordinates cut barrier boundaries and worker stealing without desynchronization.

## BLOCKING

None.

## MAJOR

None.

## MINOR

- (§2.3, lines 166–167) Redundant quote check in proxy branch: In `ArenaManifold::linked`, `isQuoteOccurrence(from)` is described as being checked inside the `slots_[d].valueKind == ExternRef` branch. However, quote occurrences are always minted with `ValueKind::None` (`apps/common/xanadu/zigzag/arena_manifold.cpp:570`), so they can never have `ValueKind::ExternRef`. Checking `isQuoteOccurrence(from)` inside the proxy branch is unreachable dead code and can be omitted entirely.
- (§2.1, line 87) Explicit rollback sequencing: When implementing `backtrack()`, unwinding `vmTrail_` strictly *before* executing `core_.arena().release(cp.mark)` ensures that any VM teardown or reference accounting observing the arena does not dereference reclaimed cell slots.

VERDICT: CONVERGED

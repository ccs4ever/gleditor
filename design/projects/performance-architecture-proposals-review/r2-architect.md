# Round 2 critique: System Architect (performance-architecture-proposals.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 4 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R1.B1 (Missing Numbered Proposal Register (P1..Pn) for Core Engine Files): RESOLVED — Section 3
  introduces a formal Proposal Register for `P1` through `P5` enumerating exact target files, C++ vs
  Vortex justification, quantified costs, and concrete fallbacks.
- R1.B2 (Unsound Ring-Buffer Undo Log & Ignoring Existing `ArenaManifold::trail_`): RESOLVED —
  Ring-buffer overwrite semantics and $O(1)$ inner-`cut()` reset were eliminated; replaced by a
  contiguous LIFO `std::vector` trail with watermarking (`vmTrailMark`), synchronized with
  `ArenaManifold::Mark`, and ring buffers moved to Refused Alternatives #1.
- R1.B3 (Unbounded LMDB Manifold Checkpointing & Content Cache Conflation): RESOLVED — Disk-backed
  `Manifold` checkpointing in `LMDBContentCache` was removed from the active proposals and moved to
  Refused Alternatives #2, citing R9 materialised-view honesty, DAG branching, and
  `LMDBContentCache` role/security isolation.
- R1.M1 (Layer Boundary Bleed: Citing OpenGL Buffer in Headless Engine Proposal): RESOLVED — All
  citations of `StreamBufferGL` and UI/rendering abstractions have been eliminated from the
  proposal.
- R1.M2 (Bit-Packing in `CellSlot` Violates House Style Rule 2): RESOLVED — Ad-hoc bit-packing into
  `CellSlot::flags` was rejected; `P3` reorders `ArenaManifold::linked` to inspect
  `slots_[d].valueKind == ValueKind::ExternRef` directly, preserving Rule 2 and
  `sizeof(CellSlot) == 32`.
- R1.M3 (Mutation Epoch Invalidation Thrashing & Unbounded Instruction Cache): RESOLVED — Monolithic
  `DecodedOp` caching and coarse global epoch invalidation were moved to Refused Alternatives #4;
  replaced by `P5`'s zero-allocation `wingOf` and streamlined `getOpcodeKind`.
- R1.M4 (Bespoke Paged Directory Instead of Leveraging `VirtualMemoryArena`): RESOLVED — `P2` now
  proposes leveraging `VirtualMemoryArena` directly (committing 4 KiB pages on demand) for
  sequential store folds, falling back to open-addressed hashing only for synthetic sparse test
  indices (`U-P2`).
- R1.M5 (Missing C++ vs Vortex Architectural Justification for VM Side Tables): RESOLVED — Section
  2.1 and Proposal Register entry `P1` provide clear architectural justification for maintaining VM
  host execution lifetimes (`shared_ptr`, `vector`) in C++ tables separate from `CellSlot`.

## BLOCKING

None.

## MAJOR

None.

## MINOR

- (§2.5, line 215 & §3 P5) Header and namespace for `inplace_vector`: Line 215 cites
  `beman::inplace_vector<CellRef, 8>` via `<gleditor/cpp26.hpp>`. In accordance with repository
  conventions (`AGENTS.md`), `inplace_vector` is housed in `<gleditor/cpp26_inplace_vector.hpp>`
  under namespace `gleditor::cpp26::inplace_vector`.
- (§2.5, line 217 & §5 U-P3) Clarify `wingOf` fixed capacity vs heap fallback: Line 217 mentions
  "falling back to heap only if exceeded" while `U-P3` specifies returning
  `inplace_vector<CellRef, 8>` directly. Because a fixed C++ return type cannot conditionally
  heap-allocate without a variant/wrapper, clarify that standard opcodes are guaranteed $\le 8$
  operands (typically $\le 4$), with `function_ref<void(CellRef)>` or `wingOfHeap` reserved for
  unbounded rank walks.
- (§2.1, line 110) `VMTrailEntry` variant sizing: Line 110 prices `VMTrailEntry` at 24 bytes. A
  `std::variant<uint64_t, CellRef, shared_ptr<ActivationFrame>>` typically measures 24 bytes on its
  own due to alignment and discriminator fields, bringing the parent struct to 32 bytes with tags
  and `CellRef key`. Clarify that a tagged union or custom discriminator is intended to maintain the
  24-byte footprint.
- (§2.4, line 190) Scope of inline accessor in `SegmentedOpsSpool`: Clarify that only the dense
  index accessor `SegmentedOpsSpool::get(std::uint32_t)` is moved inline into
  `segmented_ops_spool.hpp`, while `get(const MicroversionId &)` remains out-of-line in
  `segmented_ops_spool.cpp` to avoid exposing hash/bencode parsing dependencies in the spool header.

VERDICT: CONVERGED

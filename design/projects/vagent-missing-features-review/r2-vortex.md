# Round 2 critique: Vortex & Spool-Mesh Expert (design/vagent-metacircular-missing-features.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 2 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R1.B1 (Cross-Arena Raw CellRef Leaks): RESOLVED — Pattern A handles in-turn staged passes in one
  arena; Pattern B exchanges immutable `MicroversionId` strings across actors.
- R1.B2 (Inverted spawn Lifecycle & Dangling Lineage Base): RESOLVED — Dynamic image spawn moved to
  `VagentAdapter`/`executor` with cached `ActorLineage` fold and P4 cooperative wake.
- R1.B3 (Trail Rollback & Backtracking from #Choice/#Fail in match): RESOLVED — `match` lowers to
  deterministic decision trees (`#Equal`, `#Branch`, `#ExtractField`), eliminating
  `#Choice`/`#Fail`.
- R1.B4 (Missing d.mesh.relocation Marker for OpHandle): RESOLVED — §3.2 and §5.2 formally mandate
  `d.mesh.relocation -> "cell"`, `extraRoots` with `d.dims`, and P16 fixup.
- R1.M1 (Missing Store Binding and Fallibility on promote()): RESOLVED — P27 supplies mutable
  `Store*`, derives `parentVersion` from `primaryCurrentVersion()`, and returns
  `Result<Str, ZigzagError>`.
- R1.M2 (Hallucinated consumeWork(bytes) & maxOutputBytes Cap): RESOLVED — `consumeWork()` takes 0
  args; string ingestion is bounded by arena scratch limit returning `ActorError::ResourceLimit`.
- R1.M3 (Missing Fallibility & Pseudo-Type any on std:zigzag): RESOLVED — Primitives return
  `Result<T, ZigzagError>` and use `CellValue` union instead of `any`.
- R1.M4 (Layering Inversion: Exposing spawn_image in VortexStdLib): RESOLVED — Removed from
  `vortex_stdlib`; placed in `VagentAdapter`/`executor.cpp` (P29).

## BLOCKING

(None)

## MAJOR

(None)

## MINOR

### 1. (§3.2, lines 238–246) Clarify Default Parent Version on Uncommitted Stores

Section 3.2 specifies that `parentVersion` is derived from `boundStore_->primaryCurrentVersion()`.
On a fresh or newly created store that has no commits yet, `primaryCurrentVersion()` returns an
empty string or `std::nullopt`. Note explicitly that `zigzag::promote` treats an empty parent
version as the initial genesis commit for that store.

### 2. (§4.2, line 360; §5.1, line 382) Distinction between Static and Dynamic Agent Spawning

Section 4.2 uses `let worker := spawn CompilationWorker()` to instantiate a statically declared
agent type, while §5.1 introduces `let child := spawn image(image_root) as "worker" with grant` for
dynamically emitted code manifolds. Both forms are coherent, but a brief sentence in §5.1 noting
that `spawn image` is the dynamic counterpart to static `spawn AgentDecl` will assist implementers
in `parser.cpp` and `checker.cpp`.

______________________________________________________________________

VERDICT: CONVERGED

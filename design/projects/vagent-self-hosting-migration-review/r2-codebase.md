# Round 2 critique: Codebase Expert (design/vagent-self-hosting-migration.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R1.B1 (Absence of Proposal Register P18–P25): RESOLVED — Section 6 now incorporates a formal
  Proposal Register (`P18`–`P25`) specifying location, problem statement, concrete change, price
  accounting in links/cells/cycles, and recorded fallback alternatives.
- R1.B2 (R1 AST Declaration Gap and Gate 1 Failure): RESOLVED — Proposal `P20` explicitly specifies
  the R1 AST declaration schema and AST marshaling extensions in `r1_ast.{hpp,cpp}`, Section 3
  documents the canonical 2-degree manifold topology, and Validation Gate 1 verifies structural
  isomorphism across all 23 `mini_*.vagent` samples.
- R1.B3 (Broken LoC Count and Omission of Header Files): RESOLVED — All 54 files in
  `apps/common/xanadu/vagent/` (25 `.cpp` files with 24,983 lines and 29 `.hpp` files with 4,087
  lines, totaling exactly 29,070 lines) are accounted for, `frontend.{cpp,hpp}` is included, and the
  retained C++ substrate is correctly priced at ~9,500 LoC.
- R1.M1 (Conflation of `ArenaManifold` and `Store` Sealing): RESOLVED — Compilation is explicitly
  isolated to an ephemeral scratch `ArenaManifold` lifecycle (`P21`) with RAII teardown, decoupling
  in-memory bytecode cells from durable `CompactOpNode` spool sealing and preventing cell leaks into
  `LineageStore`.
- R1.M2 (Undefined Bit-Exact Quine vs Live Monotonic `CellRef`s): RESOLVED — Section 4 Phase 5 and
  Track E define quine verification via normalized structural graph isomorphism (`FragmentStructure`
  in `tests/xuzz/vagent_metacircular_quine_test.cpp`) and Merkle root hashes of emitted bytecode,
  resolving monotonic cell allocation variance.
- R1.M3 (Incoherent Opcode Minting and Genesis Primitives): RESOLVED — Section 4 Track 3.1 and
  Section 5 Track C1 specify that `stdlib/emitter.vagent` emits integer scalar cells representing
  `OpcodeKind` ordinals via `asInt64(op)`, ensuring compatibility with `MetaRuntime::registerCode`.
- R1.M4 (Unaddressed `noCell` Sentinels Across VAgent C++ APIs): RESOLVED — Proposal `P25` formally
  mandates replacing `noCell` sentinels with `std::optional<zigzag::CellRef>` across all public
  engine signatures (`inlang_checker.hpp:42`, `inlang_emitter.hpp:31`, `executor.hpp:224`).
- R1.M5 (Erroneous OS Substrate Justification): RESOLVED — Spurious SDL3 event and socket claims
  were excised from Section 2, replaced with verified engine justifications (interpreter dispatch
  loop, P16 relocation, turn atomicity, and 64-byte `CompactOpNode` cache-line alignment).

## BLOCKING

(None.)

## MAJOR

(None.)

## MINOR

(None.)

VERDICT: CONVERGED

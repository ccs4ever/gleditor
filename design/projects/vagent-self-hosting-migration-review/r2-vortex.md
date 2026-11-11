# Round 2 critique: Vortex & Spool-Mesh Expert (design/vagent-self-hosting-migration.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 2 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R1.B1 (Scope Leak on #Return and #RunResume): RESOLVED — Formally captured in `P18` with caller
  `varsHead` preservation on the `-d.stack` frame cell and scope restoration on return/resume.
- R1.B2 (Uncut Scanning Choice Points Threaten AST Rollback): RESOLVED — Formally captured in `P19`
  (`OpcodeKind::Cut` in scanning success continuation) and integrated into §4 Track 3.1 and §5 Track
  C1.
- R1.B3 (Unbudgeted Execution Limits): RESOLVED — Dedicated §7 establishes an elevated
  $5 \times 10^7$ cycle budget, $500,000$ cell cap, $2,000$ call stack depth, and $10,000$ scanner
  backtrack limit.
- R1.B4 (Conflation of CompactOpNode and ArenaManifold): RESOLVED — Decoupled in §2, §3, and `P21`;
  compilation AST and bytecode emission execute purely in an isolated in-memory
  `ArenaManifold scratchArena`, with `CompactOpNode` cache alignment strictly governing underlying
  `SegmentedOpsSpool` persistence.
- R1.M1 (Insufficiency of boot_writer and boot_linker): RESOLVED — In-manifold cell lifecycle
  clarified through `P21` scratch arena lifecycle and existing `make_cell` builtin allocation
  primitives.
- R1.M2 (Unspecified Host Driver Protocol and Compiler ABI): RESOLVED — Formally specified in `P23`
  (thin C++ runtime shims invoking fragments via `runFragment()` / `RunCall`), `P25`
  (`std::optional<CellRef>` returns), and §4/§5 validation gates.
- R1.M3 (Omission of Handle Relocation d.mesh.relocation): RESOLVED — Explicitly mandated in §3
  (line 152) and §4 (Track 3.1 line 232) that internal cell operands are tagged on
  `+d.mesh.relocation` for safe `ArenaManifold::promote()`.

## BLOCKING

None. All previous BLOCKING issues have been resolved.

## MAJOR

None. All previous MAJOR issues have been resolved.

## MINOR

### 1. (§6, line 376) Cost description under P19 refers to CompactOpNode size

**Problem:** Under Proposal `P19`, the cost is listed as:

> *"Cost: 1 opcode (8 bytes in CompactOpNode / 1 cell in manifold), executed only once on successful
> scan completion."*

In the OSMIC storage model (`apps/common/xanadu/compact_op.hpp:26–42`), every `CompactOpNode` is
exactly 64 bytes (cache-line aligned), not 8 bytes (though the opcode integer value payload is 8
bytes). In the execution runtime, bytecode is held in `ArenaManifold` as a single cell.

**Evidence:**

- `apps/common/xanadu/compact_op.hpp:27`: `static_assert(sizeof(CompactOpNode) == 64);`
- `design/vagent-self-hosting-migration.md:376`.

**Fix:** Refine the wording to:

> *"Cost: 1 opcode (1 cell in `ArenaManifold` / 64-byte `CompactOpNode` when persisted to store),
> executed only once on successful scan completion."*

### 2. (§1, line 21; §5, line 340) Explicit mention of `make_cell` alongside bootstrap primitives

**Problem:** §1 line 21 states:

> *"2. The Genesis Bootstrap: Pure in-manifold writer (`Value`) and linker (`Link`) primitives
> minted at genesis and reachable via `std:meta (boot_writer, boot_linker)`."*

and §5 line 340 states:

> *"Track C1 (`stdlib/emitter.vagent`): Emits wrapped opcode sequences via genesis `boot_writer` and
> `boot_linker` using `asInt64(op)`."*

While `make_cell` is an established builtin in `apps/common/xanadu/vagent/compiler.cpp:2353` and
`executor.cpp:1076`, `boot_writer` and `boot_linker` only mutate or link existing cells. Mentioning
`make_cell` alongside `boot_writer` and `boot_linker` in the text will prevent any confusion for
future implementors.

**Evidence:**

- `apps/common/xanadu/vagent/bootstrap.cpp:235, 359` (mutates/links existing cell arguments).
- `design/vagent-self-hosting-migration.md:21, 340`.

**Fix:** Add `make_cell` to the list of primitives in §1 (line 21) and §5 (line 340):

> *"Allocates opcode and operand cells via `make_cell`, writes values via `boot_writer`, and
> connects opcode topology (`spin`, `grab`, `step`, `values`) via `boot_linker`."*

______________________________________________________________________

VERDICT: CONVERGED

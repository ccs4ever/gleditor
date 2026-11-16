# Round 3 critique: Language Design & Ergonomics (performance-architecture-proposals.md)

Tally: 0 BLOCKING, 2 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R2 M#1 (fixed-capacity `inplace_vector` imposed an eight-operand ceiling): RESOLVED — P5 and U-P3
  now specify an `OperandList` with heap spill for larger arities.

## BLOCKING

None.

## MAJOR

### 1. (§2.5, line 224) The return-type migration omits existing `std::vector` consumers

Full title: P5’s `OperandList` return type either breaks existing consumers or converts them back to
allocating vectors

**Problem:** P5 changes `inputsOf` and `outputsOf` to return `OperandList`, but the proposal only
specifies its indexing and range interface. Existing code explicitly initializes
`std::vector<CellRef>` from these methods. Without a conversion, those sites stop compiling; with an
implicit conversion, those calls still allocate vectors, including on the common small-arity paths
P5 aims to optimize. The Proposal Register lists no consumer changes.

**Evidence:** The proposed return-type change is at
`design/performance-architecture-proposals.md:224-230`; P5’s registered files are at line 250.
Existing explicit vector initializations include
`apps/common/xanadu/vortex/vortex_stdlib.cpp:918,927` and
`apps/common/xanadu/vortex/vortex_vm.cpp:1745,1807,1892` (with additional sites in `vortex_vm.cpp`,
including lines 2938 and 3316).

**Fix:** Inventory and update all explicit vector consumers to use `OperandList` or a range. Add
every changed consumer file to P5’s Proposal Register. Where a vector is required by an API,
identify that boundary and price its conversion.

### 2. (§2.5, line 231) Opcode fast path lacks a classification guard

Full title: A direct opcode lookup can misclassify runtime slots as instructions

**Problem:** The proposal suggests looking up `opcodeMap_` directly when a cell is “known to be an
instruction cursor,” but it does not define how callers establish that condition or preserve the
current classifier’s exclusions. `CellRef` is a `uint32_t`, and runtime slots use the same type.
Removing those checks from a general classification path can return an opcode for a cell currently
treated as a non-opcode slot.

**Evidence:** P5’s fast-path wording is at `design/performance-architecture-proposals.md:231-233`.
`VortexVM::getOpcodeKind` first returns `nullopt` for cells in `cellReferences_`, `streamSlots_`, or
`iteratorSlots_`, then checks `opcodeMap_` and numeric values
(`apps/common/xanadu/vortex/vortex_vm.cpp:576-589`). Runtime state can populate those maps
independently, for example `vortex_vm.cpp:732` and `vortex_frames.cpp:451,473`; `CellRef` is
`std::uint32_t` (`apps/common/xanadu/zigzag/dim_vector.hpp:21`).

**Fix:** Keep the existing general classifier’s precedence. If profiling supports a faster path,
define a separate opcode lookup for call sites with a proven instruction-cell precondition, and
specify how that precondition is enforced.

## MINOR

None.

VERDICT: NOT CONVERGED

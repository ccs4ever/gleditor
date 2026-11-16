# Round 5 critique: Codebase Expert (performance-architecture-proposals.md)

Tally: 0 BLOCKING, 1 MAJOR, 0 MINOR.

## BLOCKING

(None)

## MAJOR

### 1. (§2.5, line 286) P5 omits explicit-vector consumers outside the Vortex files

Full title: Add VQL compiler and test vector boundaries to the P5 scope

**Problem:** P5 changes `inputsOf` and `outputsOf` to return `OperandList` and forbids implicit
conversion to `std::vector`. Its migration list and Proposal Register omit existing explicit-vector
consumers outside the listed Vortex files. Those sites will fail to compile unless they are migrated
or explicitly construct an owned vector.

**Evidence:** `apps/common/xanadu/vql/compiler.cpp:1392,1410` initializes `std::vector<CellRef>`
from `core_.inputsOf/outputsOf`. `tests/xuzz/vortex_core_test.cpp:353,358` does the same. The
explicit-vector consumers in `vortex_stdlib.cpp:918,927` are already listed.

**Fix:** Add the VQL compiler and affected test file to P5’s migration scope and Proposal Register.
Specify whether each consumer switches to `OperandList` or explicitly constructs a vector.

## MINOR

(None)

VERDICT: NOT CONVERGED

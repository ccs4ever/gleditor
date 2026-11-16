# Round 1 critique: Language Design & Ergonomics (performance-architecture-implementation-plan.md)

Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

## BLOCKING

None.

## MAJOR

None.

## MINOR

None.

## Review notes

- Surface-language behavior is explicitly preserved: M4 authorizes no syntax or arity-limit changes,
  keeps arbitrary-arity operands through vector spill, and requires preserving operand ordering,
  reference/master resolution, runtime-slot rewriting, and opcode-classification precedence
  (`design/performance-architecture-implementation-plan.md:202-219`). This matches the accepted P5
  contract (`design/performance-architecture-proposals.md:380-420`) and the current `wingOf`
  reference/master resolution and VM runtime-slot rewrite
  (`apps/common/xanadu/vortex/vortex_core.cpp:382-398`;
  `apps/common/xanadu/vortex/vortex_frames.cpp:326-335`).
- The migration identifies core, VM, stdlib, VQL/VPL compiler, and test consumer boundaries,
  explicitly forbids implicit vector conversion, requires owned conversions only at actual ownership
  boundaries, and gates compilation of every app/test. The plan further permits returning to vector
  buffers if measurements do not justify the hybrid representation
  (`design/performance-architecture-implementation-plan.md:202-219,327-331,340`).
- Failure reporting is specified for the stateful optimization: prepare storage before mutation;
  allocation failure leaves restorable state and identifies the executing `failingClause`; budget
  refusal rolls back and returns a failed `ExecutionResult` with that clause; gates exercise
  preparation-point failures and rollback
  (`design/performance-architecture-implementation-plan.md:231-245,267-275`). This aligns with the
  current VM's per-op `ExecutionResult` failure identity
  (`apps/common/xanadu/vortex/vortex_vm.cpp:759-765`). Operand spill failure occurs during operand
  materialization and the plan requires copy/move/spill failure coverage without changing language
  arity semantics (`design/performance-architecture-implementation-plan.md:204-207,215-219`).
- Staged defaults are usable: eight is provisional, M0 gathers arity/empty/spill distributions, M4
  compares capacities and span/visitor alternatives, and vector fallback remains available. The
  accepted proposal likewise treats eight as a measurement hypothesis, not a fixed language limit
  (`design/performance-architecture-implementation-plan.md:104,215-219,340`;
  `design/performance-architecture-proposals.md:389-420`).

VERDICT: CONVERGED

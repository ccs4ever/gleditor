# Round 1 critique: Vortex & Spool-Mesh Expert (vortex-io-redesign.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 1 MAJOR, 0 MINOR.

## BLOCKING

None.

## MAJOR

### 1. (§7.1, lines 314–327) Pending execution has no scoped core proposal

**Problem:** The document requires a new VM/host suspension and resumption protocol that preserves
the live cursor, frames, bindings, guards, and choice points while parked. It says this must extend
the actual stepping protocol, but leaves the implementation boundary, shared lowering, and resource
cost unspecified. This is a core/runtime change and the workflow requires each such change to be
isolated in a numbered Proposal Register entry. The document has no Proposal Register. Without it,
implementers could put await in an individual caller, add an opcode with divergent language
behavior, or return from `run` while losing the continuation; the text does not select or bound
those paths.

**Evidence:** The design says: “A proposed Pending execution result and runtime await primitive must
extend the actual VM/host stepping protocol” (§7.1, lines 321–323), and requires preserving “its
cursor, frames, output bindings, guards, and execution phase” while under outstanding choices (§7.1,
lines 316–319). The live VM exposes `step`, `run`, and `stepScheduler` as the execution lifecycle
(`apps/common/xanadu/vortex/vortex_vm.hpp:672-676`). `run` is a cycle-bounded loop that handles stop
and terminal conditions internally (`apps/common/xanadu/vortex/vortex_vm.cpp:4854-4889`); there is
no Pending result or parked continuation in this interface. The existing `RelationNext` seam is
explicitly a host registration, not VM state, and is non-trailed
(`apps/common/xanadu/vortex/vortex_vm.hpp:691-697`); its execution calls the provider inline
(`apps/common/xanadu/vortex/vortex_vm.cpp:834-926`). The workflow requires exact-file proposals,
caller/compiler justification, cost, and fallback (skill §1.6).

**Fix:** Add a numbered proposal before implementation. At minimum scope the VM result/state and
stepping changes in `apps/common/xanadu/vortex/vortex_vm.hpp` and `.cpp`, and the owner
pump/continuation integration in the exact host file(s) selected after inspection. State how the
existing shared Vortex standard-library/language bridge surface represents await so VQL, VPL,
vprolog, Vlace, VIcon, and VAgent do not grow independent runtime protocols; list exact
compiler/stdlib files if changed. Explain why this cannot live in caller code or compiled Vortex
alone. Price retained VM/choice state and completion dispatch, and state the fallback if the
proposal is declined (no async await feature; do not substitute blocking RelationNext or a deferred
caller-runs handler). Preserve that the host, not a worker, owns arena publication. If no existing
source file is changed by the eventual design, explicitly say so and explain how the proposed
semantics compose from existing primitives.

## MINOR

None.

VERDICT: NOT CONVERGED

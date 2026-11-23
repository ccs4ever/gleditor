# Round 5 disposition

- Reviewed target: `design/vortex-io-redesign.md`, version 0.6.
- Round-five target SHA256: `dcdd62c33bb1d1c7abe3e7f4d83d4c99d81a16210ec4637e1bb651a8c9d8d207`.
- Focused roles: Vortex, Codebase, Architect. Aggregate: 0 BLOCKING, 1 MAJOR, 0 MINOR; the Architect
  reported the MAJOR, while Vortex and Codebase converged.

## System Architect

- The Vortex/core/frontend boundary was implied but not normative: **Accepted.** Section 6.1 now
  states that generic submit admission, Pending/Await/resume, typed outcomes, and lifetime behavior
  live in the shared Vortex capability protocol. Frontend compilers/adapters own grammar, types,
  purity/effect placement, evaluation multiplicity, lowering, and frontend-local continuation state.
  P1, P4, and P5 repeat the ownership split. Actor restrictions use a generic host execution
  profile; VAgent-specific staging remains in the VAgent adapter.
- Existing VQL-specific VM behavior: **Recorded as baseline debt, explicitly out of scope.** The
  design cites the current closure-arity and parameter-pipeline exceptions in
  `vortex_vm.hpp:308–310` and `vortex_vm.cpp:1714–1718`. Removing those references is a separate
  project, is not P1/P4 work, and is not an I/O enablement gate.
- Acceptance evidence: a baseline-aware check compares new I/O coupling only, verifies direct Vortex
  plus VQL/VPL/vprolog/Vlace/VIcon/VAgent lowerings exercise the same generic capability trace, and
  verifies actor restrictions come from host profile rather than a VAgent branch in the VM/core.

## Follow-up scope

Round six is limited to the Architect's changed domain: the normative layer boundary, proposal
ownership clauses, and baseline-aware acceptance check. The actor durable ledger, language surface,
ZigZag topology, settings, and transient payload policy are otherwise unchanged. P1–P6 remain
proposals only; no runtime source was changed.

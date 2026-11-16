# Common brief: performance implementation plan critique

Target: `design/performance-architecture-implementation-plan.md` in `/data/git/gleditor-restore`.
The owner accepted all P1-P5 in `design/performance-architecture-proposals.md` v8 and requested an
implementation plan in its execution order, reviewed by the six Vortex workflow roles plus a C++
performance expert, all using Luna subagents. This review must not ask again for proposal approval.
Do not read or inspect `.worktrees/vql-completion`.

Read the accepted proposal and relevant live source; exact integration points and file:line
citations matter. Review the implementability, sequence, failure/rollback behavior, measurement and
release ThinLTO/PGO lifecycle. Existing review history is under
`design/projects/performance-architecture-proposals-review/`, especially r7/r8 performance reports.
This document is a plan, not a claim that runtime work or benchmarks already happened.

Enforce optional absence, uniform dimensions/topology, immutable operations and unchanged storage
invariants, layer boundaries, host-C++ justification, priced lifetimes, and an exact numbered
Proposal Register. New planned APIs/tools are not hallucinated existing APIs when explicitly
identified as deliverables. Do not reopen accepted proposals merely because benchmarks are future
gates; flag a gate that cannot establish its intended property.

Reviewers are read-only except their assigned `rN-role.md` report. Return the standard critique:
`# Round N critique: ROLE (performance-architecture-implementation-plan.md)`, tally, BLOCKING,
MAJOR, MINOR sections, numbered findings with section/line, Full title, Problem, Evidence, Fix and
final `VERDICT: CONVERGED` or `VERDICT: NOT CONVERGED`. For follow-ups list prior B/M status. A
blocking defect violates a hard invariant or cannot work; a major leaves implementers unable to
choose correctly. Do not upgrade polish; allow at most five minor items. Zero B/M means converged.
Parent verifies all B/M, writes dispositions, revises, and reruns affected roles until all seven
converge. Limited slots require batches of at most three concurrent reviewers.

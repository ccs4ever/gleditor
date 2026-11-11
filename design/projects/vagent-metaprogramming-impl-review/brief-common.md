# Implementation-plan critique — common brief

**Target:** `/data/git/gleditor-restore/design/vagent-metaprogramming-implementation.md`
(the phased implementation plan, v1).

**Reference spec:** `/data/git/gleditor-restore/design/vagent-metaprogramming.md` v1.19 —
converged after 18 critique rounds; §10.1 lists the new-code inventory, §10.2 the
approved P15/P16 register and owed adapter-API seams, §15 the deferred work. P15, P16
and the adapter seams are **approved**. The plan's job is to sequence that inventory.

**Prior review record:** `design/projects/vagent-metaprogramming-review/` (r1–r18 +
`CONVERGENCE.md`). The spec's mechanisms are verified; critique the *plan*, do not
reopen spec rulings unless the plan misreads one.

## What a useful critique looks like here

This is a build-order document, not a mechanism spec. The questions that matter:

- **Dependency correctness** — does each phase actually depend only on what earlier
  phases deliver? Is anything used before it's built (e.g. a resolver needed by an
  earlier exit criterion, a checker flag consumed by a later lowering step)?
- **Completeness** — does the plan cover *every* item in §10.1's new-code inventory
  and §10.2's register + owed adapter seams? Missing Makefile wiring, prelude
  linkage, genesis-hook integration, test-harness needs?
- **Sequencing feasibility** — can each phase ship green independently? Are there
  hidden couplings (e.g. `run` needing durability semantics, P15 needing fragments
  to exist before its tests mean anything)?
- **Exit-criteria testability** — is each "Exit" line actually checkable, or does it
  name behavior that needs machinery the phase doesn't deliver?
- **Citation accuracy** — plan cites were transposed from the spec at v1.19; verify
  the load-bearing ones against `apps/common/xanadu/` still (they're starting
  coordinates, and a wrong one misleads the implementer).

## Severity taxonomy

- **BLOCKING** — a phase cannot ship as written; a dependency is missing or
  inverted; an exit criterion is untestable by construction.
- **MAJOR** — a stated step is wrong about the code, a needed work item is absent,
  or two phases have an unstated coupling that will surface mid-implementation.
- **MINOR** — wording ambiguity, missing cite, exit criterion vaguer than it could
  be, ordering preference without a correctness argument.
- **NIT** — style/consistency.

## Report format (write this file yourself)

`design/projects/vagent-metaprogramming-impl-review/r1-<your-role>.md`:

- verdict line (CONVERGED / NOT CONVERGED)
- counts (B/M/m/nits)
- per-finding: severity, plan line ref, what it claims, what the code/spec actually
  shows, suggested fix

Then return a compact summary: verdict + counts + your top-3 findings.

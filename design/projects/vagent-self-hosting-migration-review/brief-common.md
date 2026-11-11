# Review Brief — `design/vagent-self-hosting-migration.md`

- **Target Document:** `design/vagent-self-hosting-migration.md`
- **Companion Documents:** `design/vagent-metacircular-compiler.md`,
  `design/vagent-metacircular-compiler-implementation.md`, `design/vagent-metaprogramming.md`,
  `design/vagent-implementation.md`, `design/store-slice-convergence.md`
- **Review Directory:** `design/projects/vagent-self-hosting-migration-review/`

## Standing Rules (`AGENTS.md` / `GEMINI.md` / `vortex-design-critique`)

- **Argued, not asserted:** Every design choice or stage boundary must cite the code (`file:line`),
  price the cost in bytes/cycles/links/cells, and state what alternative was refused.
- **Cells and dimensions over C++ side tables and bit-packing:** Any durable or runtime structure
  must live in the manifold (`ArenaManifold` / `Store`) on filed dimensions and respect the 2-degree
  ZigZag rank invariant (at most one posward and one negward neighbor per `(cell, dim)`).
- **No sentinels outside wire/on-disk formats:** APIs use `std::optional` or `std::expected`, never
  `noCell`.
- **Vortex over C++:** New C++ code must justify why it isn't written in Vortex/VAgent; new Vortex
  stdlib code must leverage existing stdlib functions.
- **Proposal Register:** Every change to existing code must be tracked in the numbered Proposal
  Register (`P<k>`).
- **Convergence Criterion:** All six critics (`writer`, `vortex`, `codebase`, `architect`, `ergo`,
  `zigzag`) must return `VERDICT: CONVERGED` with 0 `BLOCKING` and 0 `MAJOR` findings.

## Output Format for Critics

Each critic writes its findings to
`design/projects/vagent-self-hosting-migration-review/r<N>-<role>.md` using:

```markdown
# Round <N> critique: <Role Name> (design/vagent-self-hosting-migration.md)

Paths below are relative to the repo root.
Tally: <B> BLOCKING, <M> MAJOR, <m> MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R<prev>.1 (<short title>): RESOLVED | PARTLY | OPEN — <one-line reason>

## BLOCKING

### 1. (§X.Y, line L) <Short title under 80 chars>

**Problem:** <Precise statement of the defect or contradiction.>

**Evidence:** <Exact quotes from the doc + file:line citations from the repository.>

**Fix:** <Concrete replacement wording, grammar production, cell/dimension topology, or proposal.>

## MAJOR

### 1. (§X.Y, line L) <Short title under 80 chars>
...

## MINOR

### 1. (§X.Y, line L) <Short title under 80 chars>
...

VERDICT: CONVERGED | NOT CONVERGED
```

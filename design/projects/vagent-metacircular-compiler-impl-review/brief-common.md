# Review Brief — `design/vagent-metacircular-compiler-implementation.md`

- **Target Document:** `design/vagent-metacircular-compiler-implementation.md`
- **Companion Documents:** `design/vagent-metacircular-compiler.md`,
  `design/vagent-metaprogramming.md`, `design/vagent-metaprogramming-implementation.md`,
  `design/vagent-implementation.md`
- **Review Directory:** `design/projects/vagent-metacircular-compiler-impl-review/`

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
`design/projects/vagent-metacircular-compiler-impl-review/r<N>-<role>.md` using:

```markdown
### [<ROLE>-<N>] <SEVERITY>: <one-line title>
- **Location**: `design/vagent-metacircular-compiler-implementation.md:<line-range>` (and `apps/...:<line-range>` where applicable)
- **Evidence**: quote or exact citation showing the gap, contradiction, or cost.
- **Fix**: concrete change that resolves the finding.
```

End the report with:

- `SUMMARY: BLOCKING=<n> MAJOR=<n> MINOR=<n> NIT=<n>`
- `VERDICT: CONVERGED` (when `BLOCKING == 0 && MAJOR == 0`) or `VERDICT: NEEDS-REVISION`

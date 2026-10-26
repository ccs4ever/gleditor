# vagent implementation review checkpoints

These reviews cover implementation iterations, not completion of the initial-release language. The
earlier [design incorporation gate](../vagent-incorporation-review/README.md) records the four Luna
specialists' convergence on the incorporated resolutions and isolated examples. Historical reviews
retain their original scope; a passing frontend check does not validate execution traces.

## Current scalar compiler and guide

The scalar compiler now passes 39 tests on real Vortex graphs. It supports typed scalar expressions,
bounded search, assignments, lexical block locals and conditionals, including explicit discarded
results. It is an internal kernel; calls, full activation frames, loops and actor execution remain
separate work.

| Reviewer                              | Final disposition                           | Scope                                                                                       |
| ------------------------------------- | ------------------------------------------- | ------------------------------------------------------------------------------------------- |
| Technical writer (Luna)               | [Converged](guide-r2-writer.md)             | Guide teaching, keyword explanations, status caveats and rollback wording.                  |
| Language design and ergonomics (Luna) | [Converged](compiler-r3-language.md)        | Scalar semantics, block scope, boundedness versus discarded values and failure.             |
| Vortex expert (Luna)                  | [Converged](compiler-r2-vortex-followup.md) | Generated graphs, choices, cuts, temporaries, undo and conditional control flow.            |
| Codebase expert (Luna)                | [Converged](compiler-r4-codebase.md)        | Current compiler source and test snapshot; result normalization and transaction boundaries. |

The codebase review prompted explicit result-use handling for discarded conditional arms. Its
earlier concern about enclosing undo of `:=` was withdrawn after checking §§4.2, 5.8 and 6.7; the
specification now states that boundary explicitly. The Vortex review's no-`else` typing concern was
also withdrawn against §8.2's explicit Unit requirement. The final reports supersede their earlier
findings only within their stated scope.

## Shared runtime and frontend

- [Shared runtime review](runtime-r1.md): converged on ChoiceDepth, trail and Dewey lifecycle,
  checked integer arithmetic and exact integer comparisons.
- [Frontend audit](frontend-r1.md): independent counterexamples drove checker fixes and identified
  the missing effect-recovery grammar.
- [Effect-recovery design review](effect-recovery-design.md): converged on the narrow statement
  boundary, recovery typing and external-action limitations incorporated in v3.9.
- [Checker API safety review](checker-r3-safety.md): identified the need to enforce syntax budgets
  even when callers construct an AST directly rather than using the parser.
- [Final checker codebase review](checker-final-r4-codebase.md): converged after iterative
  inheritance and AST guards, purity/resolution work accounting, indexed member/goal lookup and
  weighted signature comparison. The final frontend suite passes 108 tests, including the direct API
  and large-input regressions. Earlier checker reports retain the findings and their original
  snapshot hashes; this report closes those findings on the current snapshot.
- [Checker language recheck](checker-final-language.md): converged on the AST preflight and its
  coverage before recursive checking.

The [implementation progress record](../../vagent-implementation.md) owns current test results and
remaining work. The [module-checking plan](../../vagent-module-checking-plan.md) records unresolved
import and adopting-agent template checks; it is not an implemented module loader.

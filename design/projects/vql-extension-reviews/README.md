# VQL extension reviews

Five requested perspectives, using `gpt-6.1-sol` at medium effort, informed
[VQL beyond lenses](../../vql-workflow-extensions.md).

These are brainstorming reports with scoped source inspection, not implementation or benchmark
certifications. Preserve their chronological context: initial reviews examined `732b8280`. The user
then requested the VQL implementation from `worktree-bridge-cse_01W9M6tyvaLMw6tcNVDeWkzk`,
fast-forwarding main to `7617d3f0`. Supplements revise findings resolved by that update; the
synthesis uses the updated baseline. Final critiques record remaining concerns and suggestions
before synthesis revision.

| Perspective                | Report                                           | Main contribution                                                                                |
| -------------------------- | ------------------------------------------------ | ------------------------------------------------------------------------------------------------ |
| ZigZag/Xanadu Purist       | [01-purist.md](01-purist.md)                     | Primary Nelson sources, identity, relationship occurrences, provenance and annotation durability |
| Technical writer           | [02-technical-writer.md](02-technical-writer.md) | Evidence/status distinctions, terminology and specification corrections                          |
| Vortex/systems expert      | [03-vortex.md](03-vortex.md)                     | Actual runtime seams, ownership, asynchronous effects, replay and recovery                       |
| Language/ergonomics expert | [04-language.md](04-language.md)                 | Cardinality, contracts, equality, effects and compositional library design                       |
| Workflow user              | [05-workflow-user.md](05-workflow-user.md)       | Cross-service journeys, offline work, shell history, partial failure and selective publication   |

The initial reviewers worked independently. The language and writer reviews consumed earlier
reports; runtime/language/writer reviewers reconciled the worktree update. Purist, writer and
workflow reviewers then challenged the synthesized proposal. The synthesis records unresolved
choices instead of treating every suggestion as an agreed specification.

Session verification by the coordinating agent:

- `make -j$(nproc) xuzz_test` completed after the fast-forward.
- The rebuilt `xuzz_test --gtest_filter='VQL*'` passed all 107 tests from 10 suites, using offscreen
  video, dummy audio, software rendering and isolated XDG data/config/cache paths.
- Markdown formatting and linting apply to the new proposal and reports. Existing VQL design
  examples and proposed library pseudocode are not certified runnable by those checks.

No account was connected and no external workflow action was dispatched. Research used primary
conceptual and protocol sources linked in the proposal and reports. The new files are a proposal;
the canonical VQL specification changes came from the requested worktree.

The subsequent requested critique loop is recorded in [critique rounds](critique-rounds.md),
including findings, dispositions and the explicit stopping criterion.

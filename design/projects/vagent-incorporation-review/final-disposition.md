# Final incorporation gate: four reviewers quiescent

The incorporated vagent specification is version 3.8. Its final frozen input is `r4-input.txt`. The
current `design/vagent-agent-language.md` matches that input byte for byte; its SHA-256 is
`69b764378f27879194281375fca3f78b0e0da839e576fa2df63ed52d449a7854`.

All four independent Luna reviewers returned **CONVERGED with no actionable findings** on this same
input:

- [Technical writer](r4-technical-writer.md)
- [Vortex expert](r4-vortex-expert.md)
- [Agent-language design and ergonomics expert](r4-agent-language-ergonomics.md)
- [Additional codebase expert](r4-codebase-expert.md)

No BLOCKING, MAJOR or MINOR finding remains open at the incorporated specification's stated scope.
Deliberately deferred extension designs are identified as deferred, not treated as built features or
silently adopted core proposals.

## Incorporated changes

The specification itself now carries all fourteen release decisions and deferred directions in §18,
with consistent initial boundaries in the introduction, mechanism table, rulings, checkpoint costs,
deliberation/suspension, communication, module naming, tests, replay properties and R8
reconciliation. Its §11.3 defines the deterministic harness; §12.5 illustrates the bounded same-node
Contract-Net library direction. StoreWriter remains unavailable initially, and deferred work does
not inherit approval from P1–P14.

The integration reviews corrected the status-table layout and required a complete single-turn
input/semantics contract, a distinction between within-source ordering and cross-source P4
arbitration, and finite service for admitted local/deadline work under sustained mailbox load.
Restoring ready/deadline data alone is no longer described as reproducing external scheduling. These
are requirements on the unbuilt adapter/P4 design, not claims that current `pumpOne` implements
local scheduling.

The user's subsequent request adds §12.6: **23 independent small examples**, each with setup, code
and an expected trace. They explain fields/locals, Bool comparisons, search and precedence,
reversible assignment, iteration/generation, helper return/contracts, scanning, handler versus plan
failure, beliefs and sources, goal kinds, adoption/subgoals, waits, questions/fallback, space-scoped
messages, native effects, skill replacement, behavior composition, handler priority,
yield/cancellation and orderly stop. The review fixed a reserved plan identifier and a Markdown
paragraph break; all four reviewers checked the corrected final set.

## Review and verification scope

The review iterated on the complete incorporated document, not merely the historical companion
recommendations. R1 produced integration corrections; R2 calls stopped at a usage limit and have no
pass recorded; R3 reviewed the user's expanded examples; R4 reviewed the final corrections and three
additional focused units. Exact inputs and dispositions are retained for traceability.

Documentation validation covers Markdown formatting, lint and whitespace, complete three-cell status
rows, one entry for each U-A1–U-A14, 23 small fenced units, unchanged P1–P14 proposal rows, and
equality with the final reviewed input. No runtime acceptance gate is reported as executed: vagent
remains specification-only and the examples are not compiler-verified programs.

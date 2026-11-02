# Round 6 critique: PL Design & Ergonomics Expert

Source shorthand `xanadu/` means `apps/common/xanadu/`.

Review target: `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md` (integrated
v1.3; exact content matches the frozen approved input SHA-256
`c4c9506fd39f40f673931b35f37a6abac6e0510d0ec176507167dcafe050693f`). P19.1–P19.8 approval was
verified in `design/projects/vagent-epistemic-arbitration-review/approval-p19.md`. This is a review
only; no repository files were changed.

Tally: 0 BLOCKING, 1 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R3.E1 (undefined `GoalTerm`): RESOLVED — §4 defines `Term` as one predicate term.
- R3.E2 (invalid `if let` examples): RESOLVED — examples are clearly marked proposal fragments and
  use the base conditional shape.
- R3.E3 (missing AST/checker contract): RESOLVED — §4 specifies typed query/policy/action fields and
  static errors.
- R3.E4 (undeclared example calls): RESOLVED — examples no longer depend on `log(...)` or
  `fail(message)`.
- R4.E1 (undefined `LogicVar`): RESOLVED — §4 defines it as `'?' Ident`, consistent with
  `design/vagent-agent-language.md:1359–1360`.

## BLOCKING

None.

## MAJOR

### 1. (§4, line 335) Ranked ties have contradictory failure semantics

**Problem:** The unqualified rule says “No candidates, a tied maximum, or multiple unresolved
clusters produce ordinary query failure.” That makes a `ranked` query fail for tied maxima and
appears to fail for multiple clusters, while U-C4 (§1, line 46) says `ranked` exposes tied
alternatives and §5.3 (lines 453–456) specifies enumerating tied candidates by stable ID. The
contradiction is observable for two eligible candidates with equal W: following §4 yields failure,
while following U-C4/§5.3 yields two alternatives. A checker/runtime implementer cannot determine
which behavior the approved language promises.

**Evidence:** The current language defines `Believes` and `ForBelieves` as distinct forms
(`design/vagent-agent-language.md:1340–1341`; current parser handles them at
`xanadu/vagent/parser.cpp:889–911`). The proposal extends modifiers to both typed nodes (§4, lines
315–317), so the failure sentence applies to the same surface family that includes `ranked`. U-C4
and §5.3 explicitly prescribe ranked tie exposure.

**Fix:** Scope the failure rule by modifier, for example: “`truest` fails on a tied maximum;
`consensus` fails when no eligible component or when more than one unresolved component remains;
`ranked` enumerates all eligible candidates, including tied maxima, and fails only when exhausted.”
If `BeliefQuery` itself returns only one result and enumeration is exclusively `ForBelief`, state
that distinction in the grammar and error contract.

## MINOR

None.

VERDICT: NOT CONVERGED

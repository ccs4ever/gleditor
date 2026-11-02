# Round 2 critique: Technical Writer (vagent-extended-capabilities-and-belief-vlog-unification.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 4 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R1.1 (Contradiction in String Scanning Signatures & Composability): RESOLVED — Section 3.2 and 3.3
  cleanly separate position generators/functions (`find`, `upto`, `any`, `many`, `match`, `bal`
  returning 1-based `Int` without advancing `&pos`) from movement functions (`tab`, `move` advancing
  `&pos` and returning `Str`). Composability `tab(find("foo"))` works as specified, and fallible
  methods return `optional`, strictly adhering to the sentinel ban.
- R1.2 (Missing Refused Alternatives & Open Questions Table): RESOLVED — Dedicated §1.4 Refused
  Alternatives (covering monolithic clause ranks, ad-hoc matchers, scanning return types, and
  unfreshened rule unification) and §1.5 Open Questions Table (`U-B1`–`U-B4` with concrete defaults)
  are fully integrated.
- R1.3 (Ambiguous Cross-References Omitting vagent Doc Name): RESOLVED — External cross-references
  (e.g., `vagent §5.3, §6.6`, `vagent §5.9`, `vlog §4.1`) now carry explicit markdown links to
  companion documents, and Table 6 in §6 has been completely reconciled.
- R1.4 (Inaccurate Native Skill Status, Lowering & Value Return): RESOLVED — Section 6 Table 6 and
  Proposal P18.6 accurately acknowledge existing runtime dispatch in `agent_executor.cpp`, plan turn
  yielding is correctly specified as branching to `haltOp_` (`#Halt` / `rtSettle_`), and return
  value unpacking from `match.result` into the return register is fully documented in §4.3.
- R1.5 (Conflating effectId with Host token): RESOLVED — The sequence diagram and step-by-step
  lowering delineate the agent-internal effect ID allocated from `$next_id` staged on `d.outbox`
  (`kOutboxEffectId`) from asynchronous host execution and cancellation.

## BLOCKING

None.

## MAJOR

None.

## MINOR

### 1. (§TOC, lines 67–84; §Reference Links, lines 1064–1084) TOC Anchor Slug Alignment

Full title: Discrepancies Between Table of Contents Anchor Targets and Body Section Headings

**Problem:** Several anchor link references in the Table of Contents use slugs that do not match the
GitHub Flavored Markdown slugs generated from the actual section headings:

- Line 1064: `[toc-04]: #24-belief-mutation-mechanics-believe-forget-revise-assert--from` vs.
  heading line 341:
  `### 2.4 Belief Mutation Mechanics ('believe', 'forget', 'revise', 'assert ... from')`. The slug
  should be `#24-belief-mutation-mechanics-believe-forget-revise-assert--from` or single hyphen
  depending on renderer, and the TOC entry text at line 67 says
  `[2.4 Belief Mutation Mechanics ('believe', 'forget', etc.)][toc-04]` rather than matching the
  heading title.
- Line 1075: `[toc-15]: #43-effect-lowering-in-plans-suspension--turn-yielding` vs. heading line
  791: `### 4.3 Effect Lowering in Plans: Suspension and Turn Yielding` (uses "and", generating slug
  `#43-effect-lowering-in-plans-suspension-and-turn-yielding`).
- Line 1076: `[toc-16]: #44-effect-staging-in-handlers-fire-and-forget` vs. heading line 849:
  `### 4.4 Effect Staging in Handlers: Fire-and-Forget Semantics` (missing `-semantics`).
- Line 1084: `[toc-24]: #6-compatibility-verification--migration-matrix` vs. heading line 1024:
  `## 6. Compatibility, Verification & Migration Matrix`.
- In §6, `### Acceptance Verification Battery` (line 1036) is not listed under Section 6 in the TOC.

**Fix:** Align the TOC titles, section headings, and anchor targets so all TOC links resolve
smoothly across all markdown viewers.

______________________________________________________________________

### 2. (§2.9, line 560; §4.5, line 874) Residual Bare Companion Cross-References

Full title: Omission of Companion Document Links on Residual Section References

**Problem:** While most companion cross-references were updated to use full markdown links, two
residual instances remain unqualified:

1. §2.9 (line 560): "`vagent` already solves this through the settle loop (§4.2 step 6)." In this
   document, §4.2 is `Host Binding Contract and Contention Dispatch`. The intended reference is
   `[vagent §4.2 step 6](file:///data/git/gleditor/design/vagent-agent-language.md#L450)`.
1. §4.5 (line 874): "The adapter derives its timer heap from `waiting.deadline` cells across all
   intentions (§5.6)." In this document, §5.6 is `Proposal P18.6`. The intended reference is
   `[vagent §5.6](file:///data/git/gleditor/design/vagent-agent-language.md#L587)`.

**Fix:** Add explicit markdown links to `vagent-agent-language.md` for both citations.

______________________________________________________________________

### 3. (§4.3, lines 830, 842) Residual "Token" Wording for Agent-Allocated Effect ID

Full title: Stylistic Polish of Effect ID Terminology in Step-by-Step Plan Lowering

**Problem:** While the sequence diagram (lines 812–814) and outbox field specification (line 832,
`kOutboxEffectId`) correctly use `effect_id` and `$T_{\text{effect}}$` from `$next_id`, the step 1
heading and step 3 text retain legacy "token" terminology:

- Line 830: "1. **Token Allocation**: Read `$next_id`, increment it, and bind the token to
  $T_{\text{effect}}$."
- Line 842: "Write `waiting.key = token`."

**Fix:** Update step 1 heading to "1. **Effect ID Allocation**" and step 3 to "Write
`waiting.key = T_{\text{effect}}`" for absolute consistency with the sequence diagram and
`kOutboxEffectId`.

______________________________________________________________________

### 4. (§2.4, line 386) Idempotent Source Linking for Duplicate Unkeyed Beliefs

Full title: Clarify Idempotent Insertion Behavior for Repeated Unkeyed Belief Assertions

**Problem:** In §2.4 (lines 385–387), when asserting an unkeyed predicate fact that already exists
on `+d.clause`:

> "If found: link a new peer cell for $S$ to the posward tail of $C_{target}$'s `+d.source` rank."

If source $S$ calls `believe P(1)` twice on an unkeyed predicate, it is beneficial to clarify that
insertion is set-idempotent: if a peer cell for $S$ already exists on $C_{target}$'s `+d.source`
rank, the assertion is a no-op, preventing redundant duplicate source cells.

**Fix:** Explicitly state that `+d.source` maintains set semantics: if
$S \in \text{Sources}(C_{target})$, the insertion is an idempotent no-op.

______________________________________________________________________

VERDICT: CONVERGED

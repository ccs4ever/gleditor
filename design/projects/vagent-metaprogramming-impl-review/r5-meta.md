# r5 — metaprogramming / staging-semantics review (final round)

**Verdict: CONVERGED** — all three r4 minors landed; no staging-semantics finding
reaches major in v5's edited text. Two clause-level residues remain, nit-grade.

Counts: **0 BLOCKING · 0 MAJOR · 0 MINOR · 6 NIT** (4 carried from r4, 2 new residues)

## m7–m9 verification

| r4 | Plan ref (v5) | Status |
| --- | --- | --- |
| m7 `'splice' Postfix` + peeling | :107-109 | **Closed** — "`splice` as `'splice' Postfix` with the peeling rule that re-attaches `splice f(args)`'s arglist (spec :736-738 — without it §9.1's `splice inc(count)` parses as a call-shaped-operand refusal)". Verified: EBNF at spec :714, peeling rule at spec :737-739 (§5.1 spans :711-774), §9.1's `splice inc(count)` at :2797. Operand level, re-association rule, cite and failure-mode rationale all present — the suggested text landed verbatim. |
| m8 `splice c := 3` re-scope | :170-172 | **Closed in substance** — now reads "§5.2's 'a `splice` is never a place' rule applying to *all* splices, ordinary `let`-bound or `Code`-splice alike (spec :784-786 — cap$N is not re-assignable)". Verified: spec :784-786 carries exactly that rule ("a check error", cap$N binding not a re-assignable slot) inside §5.2 (:775-819). The M13 edges cite is corrected to spec :456-470. The mis-scoping defect is gone — the universal quantifier is explicit. Placement residue → n5. |
| m9 `run`/`graft` arm split | :177-179 | **Closed in substance** — "the `run` refusal arm lands with the member surface in phase 3, the `graft` arm with the mutation verbs in phase 4" — exactly the phase attribution m9 asked for. Phase-3 exit (:314-318) tests only `run`; nothing over-promises. Landing-site residue → n6. |

## Majors-only pass over v5's edited text (full v4→v5 diff)

0 majors. Every edited region verified against code/spec:

- **R4-A promote root (:441-450)** — the agent-cell-alone ruling landed with all
  four spec-wrong arms of the spanning-root alternative refuted in place (bare
  grafts dying with the incarnation spec :375-377/:1791-1799, graft-adds without
  prune-removes, replay-doubling under (d), the `patch:<n>` alias-blindness
  :1322-1324), `:2265-2266` re-attributed to (a)'s classification walk, and the
  durable-state-on-agent-ranks argument (:1870-1871, :2465) answering the
  drops-silently concern. No staging-semantics interaction: promote/durability
  is orthogonal to expand/check/`compileFragment`, and "the only program-rooted
  promote is the one-time image seed named below" resolves at :451
  (`promote` of the compile arena / `adoptOpRecords`).
- **Orphan residual re-scope (:411-414)** — "one `dimensionNamed`-invisible
  orphan at its *first* fold — the first-use edge split" with the `A`/`B`/`A'`
  mechanism named (`store.cpp:1066`, `arena_manifold.cpp:1781`, `:2030-2042`);
  the per-checkpoint accrual claim is gone.
- **`unlearn` drop classes (:437-441)** — mid-rank repoints promote fine /
  tail drop is true-removal → tombstone only; the `learn_plan`-supersede
  mis-pairing is removed.
- **`d.vars` repoint precision (:434-435)** — "the keying record's `+d.values`
  edge repoints" per spec :1300.
- **Cites** — `dimDims && home` restored (:395); "nineteen cells, eighteen
  filed names (`home` is a plain genesis cell, not a dimension)" (:400-401);
  `static` baseline `:952-960` (:205 — verified: the range covers the
  :958-960 `choiceDepth`/`outstandingMarks` refusal the old `:952-955` missed);
  spec `:634-635` (:441); `arena_manifold.hpp:833-836` (:446, the
  folds-only-root's-subgraph claim — distinct from the `promote` signature cite
  :844-846 at :426).

## Carried nits (r4, all persist — none contradict the fixes above)

- **n1** (:237-239) — `static`'s site-context arm still sits under
  `compileFragment`; only the emitted-fragment arms are its duty.
- **n2** (:230-233) — descriptor enumeration still drops the parameter *count*
  spec :914 names first.
- **n3** (:220-222) — phase-2 exit's `static` list still lacks the kernel-side
  result-type-mismatch mention the bullet names (:203). Below exit granularity.
- **n4** (:167) — "(§5.3 — textual substitution would yield …)" still reasons
  from §5.4's `splice c(f())` argument (spec :905-906), not §5.3.

## New residues (nit-grade)

- **n5** — `splice c := 3` (:170-172) is grammatically still inside the
  `Code`-splice `def` expansion's "(its refusal edges: …)" parenthetical —
  "plus" placement could suggest the check lives on the M13 code path, though
  the clause's own "all splices" language overrides it. It pairs more naturally
  with the residual-splice check that follows the paren.
- **n6** — the m9 split's landing sites are loose: phase 3's bullet (:273)
  still names the pair "`run`/`graft` … is a named refusal" where only `run`
  exists, and phase 4's mutation-surface/attachment-points bullets (:336-349)
  never name the splice-only `graft` arm phase-2 assigns them. The normative
  attribution is in the plan (phase-2 sentence); a half-clause each side —
  "`run` (phase 4's `graft_*` inherit the same check)" plus naming the arm in
  phase 4's bullet — would close it cleanly.

## Top 3 for the parent

1. **All three r4 minors verified landed** — m7 verbatim (`'splice' Postfix` +
   peeling + spec :736-738 + the §9.1 call-shaped-operand rationale); m8 via
   explicit all-splices scope language and the correct §5.2 :784-786 cite; m9
   via the phase-2 attribution split ("`run` … phase 3, `graft` … phase 4").
   Spec cites all check against section boundaries (§5.1 :711-774, §5.2
   :775-819, §5.4 :899-935, §5.5 :936-978).
2. **R4-A promote-root rewrite is staging-clean** — the round's major landed
   with each spec-wrong arm refuted in place and no interaction with the
   expand/emission pipeline; the image-seed exception resolves at :451.
3. **Only residue worth a half-clause (n6)** — phase 3's bullet still pairs
   "`run`/`graft`" and phase 4 never names the graft arm it now owns; readable
   through phase-2's explicit assignment, so nit-grade, but the two
   half-clauses would tidy it.

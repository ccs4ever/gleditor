# r4 — metaprogramming / staging-semantics review

**Verdict: CONVERGED** — M1 landed correctly and all six m-residuals closed in
substance. Three clause-level residues remain in v4's new staging text; no
staging-semantics error reaches major.

Counts: **0 BLOCKING · 0 MAJOR · 3 MINOR · 4 NIT**

## M1 verification — the `let`-per-arg marshalling clause (plan :158-163)

The clause landed with every required element, each faithful to the spec:

| Required | v4 text | Spec truth |
| --- | --- | --- |
| fresh `let` per arg | "each argument binds a fresh `let` local at the expanded body's head" | :903-905 verbatim rule |
| once-only eval | "evaluated once" | :905 "each argument evaluated once" |
| arg order | "in argument order" | :905 same |
| param refs re-pointed | "parameter references re-pointed at the synthesized bindings" | §9.1's `{ let p0 := count; p0 + 1 }` (:2797) implies re-point, not rename — correct choice, since `cloneExpr` preserves names verbatim (:869) and stamp-level re-pointing is collision-free |
| cap$N distinct | "the synthesized `let` is the design's only one — `cap$N` remains a pure alias (§5.3)" | :839-840 "pure binding-table alias, not a synthesized `let`" |

Plus the two guards my suggested text carried: "M13's non-`Code` args marshal
identically" (spec :906-908 — Code args are statically-known bindings, never
re-evaluated, correctly implied by omission) and the failure-mode rationale
("textual substitution would yield observable multi-eval on impure actuals" —
spec :905-906's `splice c(f())` argument). The marshalling correctly attaches to
*splice substitution* as a class, not to M13's expansion — so ordinary
`let`-bound splices get it too. No contradiction with §5.3 item 4: the
marshalling `let` *wants* the value-copy semantics `cap$N` must avoid —
call-by-value is the point.

## Residual verification (r3 minors + nits — all closed or reduced)

| r3 | Status | Where |
| --- | --- | --- |
| m1 prefix productions | **Closed in substance** :103-106 — both productions named with arity facts (`static` Unary + mandatory operand, spec :960-961 ✓). Operand-level/peeling residue → new m7. |
| m2 `$cap$N`-anchor interim | **Closed** :292-294 — "the named arm lands here with the classification; pre-phase-3 a `$cap$N` anchor resolves under the single-activation shape or fails the kernel's generic `bindings_` lookup (`compiler.cpp:475-483`), an accepted interim" — exactly the two-branch interim the code gives. |
| m3 splice-only arms | **Closed in substance** — evaluated-position bar + no-head/no-descriptor at :170-173 (§5.4 :924-934 ✓), `run`/`graft` arm at :267-268. `graft`-timing residue → new m9. |
| m4 `static`-in-emitted-body | **Closed in substance** :230-233 — both spec arms (site-context judgment; `static(param)` emission error, spec :968-972 ✓) under `compileFragment` ownership. Placement wrinkle → n1. |
| m5 descriptor param-rank | **Closed** :223-225 — `+d.step` chain off `+d.grab`, declaration order, `+d.vars`-named cells, `+d.name`-labelled, `+d.values`-typed — spec :914-917 verbatim. Count-field drop → n2. |
| m6 `static` result half | **Closed** :196-199 — run → type-matched `result`-cell read (mismatch own refusal) → cursor unlink → `literal()` emission → baseline check; spec :952-955 order preserved. |
| n1 cite drifts | **Closed** — `Makefile:695` now correctly names `VAGENT_SYNTAX_OBJS` (:113; Makefile drifted to :694/695); `checker.cpp:770-779` (:256) fixed. |
| n2 M13 refusal-class edges | **Partially closed** — both named at :166-167, but `splice c := 3` is mis-grouped and mis-cited → new m8. |
| n3 §5.8 provenance | **Closed** :290-291 — "the language spec's §5.8 (design doc :847-854)" now names both documents correctly (design doc :846-854 does carry the frame-bound paragraph; §5.8 itself is the language spec's). |
| n4 spill/restore | **Closed** :266-267 — "spilling *and restoring* `result`/`produced` around the call and failing when `produced` is false" (spec :262-264 ✓). |
| n5 programRef per-program | **Closed** :238-241 — "written once per program at image finalization (not per fragment — the image record `compileFragment`'s marking feeds)". |

## New findings

### m7 — the `splice` production's operand grammar and peeling rule unnamed — MINOR

**Plan ref:** :103-105 — "`splice` wherever an operand can follow (§5.1)".

**What the spec shows:** the production is `'splice' Postfix` (§5.1 EBNF,
spec :714), so the operand greedily parses `splice f(args)`'s `f(args)` as a
call — and the **peeling rule** (spec :736-738) is what re-attaches the
outermost arglist to the `splice`; "any other call-shaped operand is a check
error" (it never splices a call *result*). A production implemented as named —
prefix + operand — with no peeling leaves `splice inc(count)`'s arglist on the
operand, hitting the call-shaped-operand refusal: the phase-2 exit's own §9.1
shape dies at parse. The (§5.1) cite carries the truth (EBNF + peeling paragraph
sit together), but the phase-1 bullet is where the operand level and the
re-association decision get made — one clause closes it: "`splice` takes a
`Postfix` operand with the §5.1 peeling rule — `splice f(args)`'s arglist
re-attaches to the `splice`; other call-shaped operands refuse".

### m8 — `splice c := 3` mis-filed under M13's edge list with a wrong cite — MINOR

**Plan ref:** :164-167 — "`Code`-splice `def` expansion (its refusal edges: …
opaque `Code` actuals, and `splice c := 3` — cap$N is not re-assignable, spec
:464-470)".

**What the spec shows:** `splice c := 3` is §5.2's "a `splice` is never a
place" rule (spec :784-786 — a captured name expands to a `cap$N` *binding*),
an edge of **splice substitution generally**, not of `Code`-splice `def`
expansion; the `:464-470` cite covers the M13 block (opaque actuals :463-466,
diagnostic split :466-469, the `effect`-bar :469-472) but not this rule.
Grouping it inside "its refusal edges" invites scoping the refusal to template
calls only — `let c := code"…"; splice c := 3` is the ordinary case it must
also refuse. Fix: move the edge to the splice-substitution clause (it pairs
naturally with the residual-splice check) and cite §5.2 :784-786.

### m9 — the splice-only `run`/`graft` refusal names a verb phase 4 owns — MINOR

**Plan ref:** :172-173 ("the `run`/`graft` refusal arm lands with the member
surface in phase 3") and :267-268 ("`run`/`graft` on a `Code`-parameterized
(splice-only) fragment is a named refusal").

**What the plan shows:** the `graft_*` verbs land in phase 4 (:331-337);
phase 3's member surface has `run` only. The refusal is spec-correct (§5.4
:924-934), but the `graft` half has nothing to attach to until phase 4 — the
phase-3 exit can't exercise it, and the "lands with the member surface in
phase 3" sentence over-promises. One word fixes it: "the `run` arm lands here;
`graft`'s rides its verb in phase 4" — or fold the shared descriptor-check
into phase 3's `run` path and say the `graft` verbs inherit it.

## Nits

- **n1** — the first `static` arm under `compileFragment` (:230-232) is
  mis-homed: "a `static` inside a parameterized fragment is judged in the
  context the parameter lands in" is the *splice site's* post-substitution
  re-check — phase-2 machinery in expand/check, not fragment emission. Only
  the emitted-fragment arms (`static` eval, `static(param)` emission error)
  live in `compileFragment`. Harmless as stated, but the bullet's framing
  could read as a compileFragment duty for a context it never sees.
- **n2** — the descriptor enumeration (:223) dropped the parameter **count**
  field spec :914 names first ("the descriptor record holds the parameter
  *count* and an ordered rank of parameter cells") — v3's "(count = arity)"
  is gone; the chain implies it, the cite carries it.
- **n3** — the phase-2 exit's `static` list (:214-216) enumerates checker-arm
  refusals + baseline; the kernel-side result-type-mismatch refusal now named
  in the bullet (:197) has no exit-side mention (defensive recheck — arguably
  below exit granularity).
- **n4** — `(§5.3 — textual substitution would yield …)` (:163): the
  parenthetical reasons from §5.4's `splice c(f())` rationale, not §5.3 —
  reads as a cite for the wrong section.

## Pipeline feasibility answer

Still yes — the staging pipeline remains implementable phase-by-phase with no
inversions. The marshalling now correctly sits in phase 2's substitution step
(the only place the merged-AST `{ let p0 := …; … }` shape can be built); the
splice-only refusals split cleanly (evaluated-position at expand, `run` at the
member surface); `static`'s three-context story (site / emitted body /
emission error) has an owner per arm. The remaining items are attribution and
wording fixes inside correctly-sequenced phases.

## Top 3 for the parent

1. **m8** — `splice c := 3` is filed under the `Code`-splice `def`'s refusal
   edges with cite :464-470; it's §5.2's "a `splice` is never a place" rule
   (:784-786) applying to *all* splices — scoping it to M13 misses the
   ordinary `let`-bound case.
2. **m7** — the new prefix-production clause states position but not operand
   grammar: `'splice' Postfix` + the peeling rule (spec :736-738) is what makes
   `splice f(args)` bind its arglist — without it the phase-2 exit's own §9.1
   example parses as a call-shaped-operand refusal.
3. **m9** — the splice-only `run`/`graft` refusal lands "with the member
   surface in phase 3" but `graft_*` verbs are phase 4; the graft half is
   untestable where it's promised.

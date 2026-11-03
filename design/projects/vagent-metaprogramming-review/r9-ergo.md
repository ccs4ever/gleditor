# Round 9 critique — Programming Language Design & Ergonomics (v1.8)

Scope: v1.8 deltas only — verification of R8-F1/R8-F2 landings, plus new ergonomic defects in the
changed regions (:1146-1149 headless-`run` class, :1363-1422 adapter-owned lineage store,
:1503-1528 two-arm target descriptor + declared-handler gap, :1564-1572 `run`-operand records,
:1615-1639 role-split writeback, :1732-1749 genesis (a) positional, :1767-1795 (d)→(e) hand-off,
:1826-1873 label rules + `code_named` fallthrough, :2295-2309 adapter step, :2336 P16 row,
:2800-2844 changelog). Cross-checked against `vagent-agent-language.md` (V15 handler anonymity) and
the doc's own type rules (M3, :175-177 "`Code<T>` is a distinct type, not `Cell`").

## r8 verdicts

- **R8-F1 (`plan_named`/`symbol_named` `run`-operand arm dead, MINOR)** — **RESOLVED.** The arm is
  dropped exactly as ruled: "there is **no `plan_named`/`symbol_named` arm** — both builtins return
  `Opt<Cell>` where `run` demands `Code<T>` and M3 bars the silent `Cell`→`Code` upgrade, so such a
  `run` never typechecks and nothing is recorded" (:1568-1571); the session side names the
  consequence plainly — "the session-side surface for it is the type error, and the durable-side
  one the record refusal of §6.6" (:1146-1149) — and the changelog carries it (:2811-2814). The
  retained `code_named(L)`-qualified-L route has its own seam — R9-F1.
- **R8-F2 (descriptor convention can't describe `patch:<n>`; anonymous kernel handlers, MINOR)** —
  **RESOLVED.** The convention is now two-armed and honest about storage: "the **verbatim
  `patch:<n>` clause label** for learned-clause targets — a learned clause's name lives on its
  `+d.name` verbatim, so the record stores `patch:<n>` exactly, never a canonicalized
  `<module>:<Agent>.patch:<n>` the `plan_named` match would then miss" (:1506-1509). The
  declared-handler gap is stated verbatim: "a kernel-emitted handler clause is anonymous under spec
  V15 … so `learn_where`/`learn_body` on a *declared* handler hits the same refusal — durable
  guard/body patching works on named plans and on learned handlers … never on declared ones"
  (:1520-1524). The replacement half of that gap is still unstated — R9-F2.
- **r8 nit bundle** — `plan_named`'s row now says it "matches every clause `+d.name` label verbatim,
  event-handler clauses included" (:1050); the `code_named` example reads `demo:Forager.f` not
  `patch:` (:1853); `unlearn`'s reverse read is now explicitly cell→name (:1525-1528); the
  nested-literal direction is corrected to outermost-`src:` + descending `+o:c` (:1828-1834) with
  arm order pinned (`site-chain → +o:c arms → +n last`, :1836-1838); the descriptor carries the
  attach parent for replay (:1509-1510); `code_named` gained the program-image fallthrough (the
  R6-F2 residual, :1863-1867) and the `reify:`-label workflow names `label_of` (:1868).

## The questions posed

**Is `code_named`-only `run` a reasonable surface?** For the label spellings, yes and better than
v1.7: `run(code_named("#foo"))` resolves a literal head in session *and* post-resume through the
new `d.code`→program-image fallthrough (:1863-1867), learned clauses stay invocable through
`plan_named` + `test_of`/`body_of`, and `run(plan_named(…))` failing statically is the *right*
failure — the M3 boundary is a feature. The qualified-name requirement is discoverable in the wrong
direction: `code_named` is documented as verbatim-label-only (:1853), yet the doc also keeps a
route — `code_named(L)` with qualified `L` — that *cannot* satisfy it (R9-F1).

**Is the anonymous-handler refusal understandable?** The mechanism is honest ("a patch that cannot
be re-found at genesis is a patch that lies", :1519-1520) and the declared-handler gap is now
named. What a user cannot learn from it is that declared handlers are also durably *irreplaceable
and unremovable* — R9-F2.

**Does the adapter-owned store leak into anything a user sees?** No. The `Store` is never a
user-facing object — it stays in adapter-API space (`resume` takes or verifies it, :1378-1379,
:2295-2301, :2336). The two user-visible consequences are stated cleanly: `learn` fails closed on a
prefix-less lineage (:1411-1417) and an artifact upgrade is a respawn, not a migration (:606-612).
The one seam is a diagnostic nit, below.

## New findings (v1.8 deltas)

### R9-F1. MINOR — the retained qualified-`L` `code_named` `run`-operand route is dead at both ends

v1.8 keeps `code_named(L)` with a qualified `L` as "the only route a `run`-role record has to the
qualified resolution path" (:1566-1567, P16 row :2336). Tracing the surface end to end:

- **In session the spelling always fails.** `code_named` matches verbatim labels over `d.code`
  (all `reify:`-prefixed) then the program-image label walk (`src:`/`#label`) — the first-`:`
  split is scoped *away* from it: "never to `code_named`, where `demo:Forager.f` is just a label"
  (:1853, :1863-1867). No head label can carry `demo:Forager.f`, so the `Opt` is empty and the
  `run` operand statement-fails — every time, deterministically.
- **Post-resume it fails identically.** The operand expression re-evaluates per `run`; the
  genesis-rebound `int64(head)` in the slot is masked by the next `code_named` call, which returns
  empty again. So a qualified `run`-role record that *resolves* at genesis (a `learn_method`
  symbol's `+d.values` lands on a head, :1611-1618) writes a value nothing ever reads — the
  successful arm is unreachable.
- **The only observable arm is destructive.** If the qualified name resolves head-less (a `def`
  symbol) or not at all, "the record is malformed, the patch drops like an unresolved name"
  (:1619-1621, :1632-1634) — the *whole* learned patch evaporates at genesis, behind a fault
  record, for a spelling that already failed in session. The trap shape: a
  `run(code_named("demo:Forager.f"))` inside a rarely-taken branch of a learned body passes
  `closed` (the operand is name-recorded), survives the session unexercised, then drops the clause
  at resume.

The text also retains one stale phrase: "it fails `code_named`'s `d.code` walk in session" (:1566)
predates the fallthrough — it now fails `code_named` outright.

Fix (either arm, one clause each): **(a)** give `code_named`'s miss a second fallthrough — the
qualified `symbol_named` walk → `+d.values` → the §6.1 op-or-head hop, head required — so
`run(code_named("demo:Forager.f"))` works uniformly in session and resume, symmetric with what
genesis already does and unambiguous under the disjointness argument; or **(b)** refuse a
`:`-bearing non-`src:`/`reify:` `L` at emission with a named diagnostic ("qualified names are not
fragment labels — a `def`/`method` is `call`able, not `run`nable"), deleting the qualified-L arm
and its genesis machinery. (a) is the better surface: it makes the retained record class honest.

### R9-F2. MINOR — the declared-handler gap names patching but not replacement/removal: declared handlers are durably immortal

:1520-1524 covers `learn_where`/`learn_body` — in-place patching — but the same anonymity defeats
the *name-keyed* verbs too: dedupe-by-name can never match a nameless clause, so `learn_handler` on
the same event is always additive; `unlearn` matches only patch target names (:1525-1528); `prune`
is session-only (:1064). Net: a declared handler cannot be durably replaced *or* removed — post-
resume the image folds it back (:1800-1805) while the learned shadow replays alongside it, and
coexistence is governed only by `priority`. A user reaching for "the `learn_handler` twin of
`learn_plan`'s redefinition-as-replacement" gets silent coexistence instead.

Fix: extend the gap sentence one clause — "…never on declared ones, which are likewise
un-*replaceable*: dedupe-by-name cannot match them, so a `learn_handler` on the same event
coexists with the declared clause under priority ordering rather than superseding it" — or mint
generated `+d.name`s on kernel handler clauses (the alternative the finding already names).

### R9-F3. MINOR — the `label_of` workflow can't be spelled through the obvious route

"a `reify:<src>:<serial>` label is read off a live head with `label_of`" (:1868-1869) — but
`label_of(c: Cell)` takes `Cell` (:1052) while every head-producing read yields `Code<Cell>`
(`code_named`, `test_of`, `body_of`), and no `Code`→`Cell` coercion exists ("`Code<T>` is a
distinct type, not `Cell`", :175-176; the refused `Code`-as-`Cell` synonym, :2360). The reachable
path is `cells"…"` — `cells"me → d.code[i]"` for the registered head, `cells"clause → d.spin"` for
the installed one — but the sentence doesn't say so, so the named workflow type-fails at the first
spelling a user tries (`label_of(code_named(…))`).

Fix: name the acquisition path in the same sentence ("read off a live head with `label_of` — the
head reached as a `Cell` through `cells"… → d.code[…]"` or `… → d.spin`"), or add a
`head_of(c: Code): Cell` read.

### Nits (not findings)

- The prefix-less-lineage `learn` refusal (:1411-1417) has no named diagnostic distinct from the
  closedness refusal — a deployment failure surfaces as an ordinary `learn` fail; worth naming
  ("no program lineage") in §5.6's diagnostic list.
- "lands in this class by intent" (:1146) — a `plan_named` `run` never reaches `run`, so it lands
  in the headless class only by analogy; harmless but loose.
- The divergence clause's "a session `run` retains the *emission-time* binding" (:1635) covers
  literal operands; a `code_named(L)` operand follows the *live* lookup per call, so session
  already tracks redefinitions there — the stated divergence is narrower for that shape.
- The descriptor's "`patch:<n>`" arm (:1506-1507) overfits the convention: a learned clause may
  carry any `name` argument; the stored value is the clause's `+d.name` verbatim whatever its
  spelling — "the clause's verbatim `+d.name` (conventionally `patch:<n>`)" generalizes it.
- `learn_handler` is absent from the dedupe-by-name list (:1108) — a second `learn_handler` with
  the same `name` appends a second same-labeled clause, leaving `plan_named(name)` first-hit
  ambiguous; probably intended (priority differentiates handlers) but worth one clause. Pre-dates
  v1.8; noted for completeness.

## Summary

Both r8 minors landed cleanly: the `run`-operand record is `literal` + `code_named(L)`-only with
the `plan_named`/`symbol_named` arm explicitly negated (:1568-1571), and the descriptor convention
gained the verbatim `patch:<n>` clause arm plus the stated declared-handler gap (:1506-1524). The
adapter-owned store stays invisible to user code — good. Three minors remain: the kept qualified-L
`code_named` `run` route fails in session *and* resume (its only live effect is dropping patches —
R9-F1), the declared-handler gap omits the replace/remove half (R9-F2), and the named `label_of`
workflow doesn't typecheck through the obvious spelling (R9-F3). All one-clause fixes.

VERDICT: CONVERGED

# Round 8 critique — Programming Language Design & Ergonomics (v1.7)

Scope: v1.7 deltas only — verdicts on r7 findings (R7-F1; residuals of R6-F2, R6-F4; the nit
bundle) plus new ergonomic defects in the changed regions (:1454-1467 clause/method patch-target
split, :1500-1509 `run`-operand records, :1530-1554 role-split rebind + headless-`run` genesis arm,
:1119-1126 headless-`run` session arm, :1730-1765 labels/reserved prefixes/`+o:c`/clones/foreign
heads, :2446-2452 U-M11). Verified against `vagent-agent-language.md` v3.9 (V15 handlers, §5.7) and
the code (`vagent/lexer.cpp:14-24` Ident alphabet; `vortex_core.cpp:119-127` `exportSymbol`
`+d.values→entryOp`; `vortex_vm.cpp:854-856` `Call` reads `toInt(inputs[0])`).

## r7 verdicts

- **R7-F1 (`plan_named`/patch-target first-`:` split vs `patch:<n>` storage, MINOR)** —
  **RESOLVED.** The two-layer fix landed cleanly: `patch:` joins `src:`/`reify:` as a module-name
  reservation — "may name no module — a module declaration carrying one is a compile-time refusal"
  (:1749-1751) — so `patch` can never hijack the module arm, and the v1.6 dead text ("unless a module
  literally takes that name") is gone (:1539-1542). The split test itself is uniform now: "the first
  `:` separates the module qualifier … only when it resolves on `programRef`'s walk, otherwise the
  whole string is the name" (:1745-1748, :1767-1769), `plan_named` is defined as verbatim `+d.name`
  matching (:1775-1777), clause targets route through it and method/symbol targets through
  `symbol_named`'s module-`d.vars` walk (:1457-1462), and `unlearn` matches the recorded name
  (:1464-1467). `plan_named("patch:5")` now works end-to-end: `patch` can't be a module → whole
  string is the name → verbatim `+d.name` hit. One wording seam survives — see R8-F2.
- **R6-F2 residual (`code_named` hole for literal heads)** — **PARTIALLY RESOLVED, as ruled.**
  The *foreign* case is now named exactly as the disposition required: "a `code"…"` head living in
  *another* artifact's image has no re-resolution spelling at all this round … deferred as U-M11"
  (:1762-1765), with the U-M11 entry present (:2446-2452). The *own-image* case — the original
  finding — is still uncallable: "a caller re-resolving a literal head by label goes through `run`'s
  record path or the same program-image walk" (:1760-1762), and neither is a `std:meta` member (the
  read group ends at `label_of`, :1023-1035; `code_named` walks `d.code` only). So "across `resume`
  you re-resolve by label" (:1781-1782) has a spelling for learned heads and none for literal heads.
  Residual MINOR — the one-line fix the doc's own disjointness argument invites (`code_named` falls
  through to the program-image walk on a `d.code` miss) remains untaken.
- **R6-F4 residual (nested-literal `+o:c` arm)** — **RESOLVED in substance.** The arm is now in
  the rule, matching the proposed fix: "a *nested* literal … labels from its innermost `src:` site
  and chains `+o:c` per nesting level … a clone of a nested literal keeps its `+o:c` arms — so
  cloned literals cannot collide" (:1732-1738), `+n` appends last (:1737-1738). Residual: the clone
  trigger is still "M13 expansion" (:1736) — plain `splice`-site clones (`cloneExpr` per site,
  :723-726, :200-201) are uncovered by the *assignment* rule even though the uniqueness check's scope
  already counts them ("literals, splices, M13 clones", :1741-1743). One clause: "cloned by
  expansion — `splice` substitution or M13 `def` expansion." MINOR.
- **r7 nits** — user-`#label` uniqueness scope resolved ("across the merged program emission
  set", :1741); "expansion site's position" ambiguity resolved (call-site reading, :1736); the
  "module literally takes that name" dead arm deleted. Still standing: `symbol_named` listed among
  first-`:`-splitting lookups though it takes `owner`/`name` separately (:1745-1748, and implied
  again at :1459-1461); `code_named`'s required `reify:<src>:<serial>` spelling still unguessable
  with no stated `label_of` workflow.

## New findings (v1.7 deltas)

### R8-F1. MINOR — the `run`-role record's `plan_named`/`symbol_named` arm presupposes operands that cannot typecheck

`run` takes `c: Code<T>` (:1037); `plan_named` and `symbol_named` return `Opt<Cell>` (:1026, :1030)
— and M3 bars the silent upgrade outright: "only `Code` may be spliced or `run` — a raw `Cell`
handle never silently becomes executable" (:176-177), with "`Code` as a `Cell` synonym" a refused
alternative (:2242). So `run(plan_named(…))` / `run(symbol_named(…))` are **static type errors** —
the emitter never sees either operand shape, and the arm at :1505-1506 ("a `plan_named`/
`symbol_named` operand spelled with a literal qualified name → that qualified name") records a form
that cannot be emitted. Two consequences:

- The arm is dead text — and it is the only arm that could mint a *qualified* `+d.name` on a
  `run`-role record as stated. The reachable path to one is `code_named(L)` with a qualified `L`
  (:1504-1505: any `Str` literal is recorded verbatim) — a misuse that fails `d.code` lookup at run
  time but leaves the record for genesis. Worth saying plainly.
- The **session-side** headless-`run` refusal (:1122-1126 — "`run` of a head-less program `def`
  symbol … refused at the descriptor lookup") cannot fire through the stated surface either: a
  `def`-symbol `Cell` can never reach `run`. (Even if it could, the clause/symbol cell is not
  head-shaped — `+d.values` names an entry op or a head, never a descriptor — so the refusal lands
  on the descriptor shape, not on the symbol's headlessness.) The **genesis-side** arm (:1550-1554)
  stays live via the `code_named` route, which is the arm worth keeping.

Fix: scope the qualified `run`-operand arm to `code_named(L)` operands — or add the `Cell`→`Code`
upgrade member §6.1 already implies ("a `validate`+re-descriptor step the caller requests",
:1094-1095), e.g. `code_of(cell): Opt<Code>`; widening `run` to `Cell` silently is the refused §11
alternative. Changelog :2651-2652 repeats the arm, so fix both.

### R8-F2. MINOR — "resolved qualified name — `<module>:<Agent>.<member>`" can't describe `patch:<n>` targets; kernel handler clauses are anonymous

Two seams inside the :1454-1467 split:

1. The target descriptor "records the target's **resolved qualified name** —
   `<module>:<Agent>.<member>`" (:1454-1455). Under verbatim `+d.name` storage a *learned* clause
   named `patch:5` records `patch:5` — not three-part — and module-level symbols record two-part
   `module:member` (the `[<Agent> '.']` segment is optional, :1767). If a recorder instead
   canonicalizes to `demo:Forager.patch:5`, `plan_named`'s verbatim match misses the `+d.name`
   `patch:5` → replay drop — the r7-F1 ambiguity transplanted from storage to the gloss. One clause:
   "records the name `plan_named`/`symbol_named` re-resolves — the clause's `+d.name` verbatim."
2. `learn_handler` is now listed among `plan_named`-resolving clause targets (:1457-1458), but
   `+d.name` is stated only for *plan* clauses (:1775-1776), and spec V15 makes declared handlers
   anonymous: "`on Event [guard] { }`" (language spec :265). `graft_handler`/`learn_handler` mint
   *named* clauses (:1040 "same shape"), so learned handler replacements resolve fine — but a
   kernel-emitted handler clause has no `+d.name`, so `learn_where`/`learn_body` on a *declared*
   handler hits the anonymous-target refusal (:1462-1464). That is a real, currently unstated
   capability gap: durable guard/body patching works on named plans and learned handlers, never on
   declared handlers. Say it, or give kernel handler clauses generated `+d.name`s.

### Nits (not findings)

- "labels from its **innermost** `src:` site" (:1733) — the `src:` head is the *outermost* anchor;
  the `+o:c` chain is what reaches the innermost. "the innermost site's full `src:` position" reads
  truer.
- Clone-of-nested arm order is unspelled (:1736-1738): `site-chain ++ literal's +o:c arms ++ +n` is
  the natural read, but "the ordinal appends to whatever chain the site produced" could also place
  `+n` before the literal's arms. Harmless under verbatim matching — pin it for the implementer.
- A user `#label` on a cloned literal is silently replaced by `src:site+n` — the clone arm reads as
  replacing whatever label the literal had; a user who labels a twice-spliced literal loses the
  `#label` on the clones. Worth one clause.
- `code_named`'s "`patch:near_water` is just a label" example (:1748) — `patch:` is a *clause*-label
  prefix the doc itself bars from head labels (:1541-1542); a qualified symbol name
  (`demo:Forager.f`) is the sharper non-splitting example.
- `unlearn` "resolves … through the same split — clause args by `plan_named`, symbol args by
  `symbol_named`" (:1465-1467) reverses the lookup direction — those are name→cell; the cell→name
  read is the clause's `+d.name` / the symbol cell's content.
- `+d.grab` gains a third §6 meaning — descriptor param chain (:980), clause attach, now the
  call/run role label on ref-records (:986, :1530). Occupancy-wise sound; dense.
- `plan_named` is now load-bearing for handler targets (:1457-1458) while its row and §6.7 text say
  "plan" (:1030, :1775-1776) — state that it walks every clause `+d.name`, event handlers included.
- `learn_plan`'s descriptor: "clause target" gloss doesn't say where the `goal` field rides
  (:1454-1462); replay needs it.
- The session/genesis **divergence clause** (:1566-1571) is honest and correctly placed — a learned
  fragment's in-session calls keep emission-time bindings while resumed calls follow re-points.
  Ergonomically surprising but documented; no action.

## Summary

v1.7 disposes of R7-F1 as designed: `patch:` module-reservation kills the hijack, `plan_named` is
verbatim `+d.name`-matching, and the patch-target split names clause vs symbol resolution cleanly.
The nested-literal `+o:c` arm and the foreign-head deferral (U-M11) landed per ruling. Two minors
remain in the deltas: the `plan_named`/`symbol_named` `run`-operand arm is unreachable because those
members return `Cell` where `run` demands `Code` (R8-F1 — and the session-side headless-`run`
refusal inherits the unreachability), and the "resolved qualified name" gloss plus anonymous
declared handlers leave two seams in the split (R8-F2). The R6-F2 own-image `code_named` hole stays
named-but-uncallable; the clone-label trigger still says "M13" where "expansion" is meant. All
one-clause fixes.

VERDICT: CONVERGED — wording-level minors only

# Round 5 critique — Programming Language Design & Ergonomics (v1.4)

Scope: v1.4 deltas only — convergence check on r4 findings plus new ergonomic defects in the
changed regions (:592, :600-613, :654-656, :702-750, :791-793, :801-807, :957-964, :1060,
:1116-1121, :1385-1393, :1413-1419, :1502-1520, :1847-1848, plus §9.2–§9.4 and §13). Verified
against `vagent-agent-language.md` v3.9 (grammar :1302-1377, handler binding :1205-1206, symbol
table §5.7) and the doc's own canonical forms.

## R4 verdicts

- **R4-F1 (MAJOR, `graft_handler` formals)** — **RESOLVED in substance.** :1118-1121 now reads
  "the handler's bound names: the event cell under `occurrence`, or under the `as`-name when the
  target clause declares one; event fields are read through the bound event exactly as declared
  handlers read `occurrence.f`, never bound as formals" — the proposed fix, consistent with spec
  :1205-1206 ("binds the event as `occurrence`, or as the name after `as`") and :2153 (fields are
  not imported locals). The unbound-read pathology and the all-fields refusal are gone. One
  residual wording defect → R5-F2 below.
- **R4-F2 (`WithinBudget` field)** — **RESOLVED.** :1848 `event WithinBudget { n: Int }`; the
  `emit WithinBudget{n: w}` at :1858 now typechecks against spec `emit E{...}` (:1219).
- **R4-F3 (label parse)** — **RESOLVED.** :1502-1508: user `#label`s alone may not spell `:`/`"`;
  `src:`/`reify:`/`patch:` labels are "opaque label strings, matched verbatim"; first-`:` splitting
  scoped to qualified-name lookups and gated on "only when it resolves as a module"; :1514-1516
  `code_named` "walks `d.code` only … no rank-order tie-break is ever needed" — the vacuous
  twin sentence is gone.
- **R4-F4 (duplicated paragraph)** — **RESOLVED.** :957-964 is one merged paragraph: per-ref-cell
  marker (with the one-`-d`-neighbour eviction argument), `isCellRelocationHandle` cite folded in,
  `+d.ref` inventory role, and "Name-records skip both halves" — no repetition, no contradiction.
- **R4-F5 (stranded sentence)** — **RESOLVED.** :791-793 reads as one sentence: "…so `validate`
  can check wing arity, `graft_*` can compare signatures as ordered `(name, type)` pairs — binding
  is name-keyed, so names are part of the match (§7.2) — and `run` can find the cells it spills."
- **R4-F6 (`Code` arity + EBNF)** — **RESOLVED.** EBNF :592 `Type ::= … | 'Code' [ '<' Type '>' ]`
  produces bare `Code`; arity chain stated at :654-656 (§5.2) and :177-179 (M3); §6.2 :1060 now
  says "when the fragment is statically known"; M6 :234-235 agrees.
- **R4-F7 (§13 double-`reify` unpriced)** — **NOT DISPOSITIONED — still a defect.** The r4
  disposition's minor batch never mentions ergo-F7, and v1.4's §13 `learn` bullet (:2085-2088) is
  unchanged: "`learn` adds a `d.patch` cell + head-chain records + checkpoint ops for live fragment
  cells + P16's two boundary passes" — the two O(fragment) recipe replays per fragment argument and
  the dead original (:1266-1268, :1310-1311) are still unpriced. Carried over as R5-F4.
- **R4-F8 (splice-only position)** — **RESOLVED.** :801-807: "refused in evaluated positions …
  only as a `splice`/template actual, never as a `let`/`var` initializer or call argument — a
  `let` binding left dead by full substitution is erased by `expand`, or refused by the checker
  where erasure is unsafe."
- **R4-F9 (`symbol_named` arity)** — **RESOLVED.** Genesis chain (:1386-1391) now spells "the §6.7
  split (`module_named` on the module qualifier, `symbol_named(owner, name)` on the rest)"; the
  grammar `[<module> ':'] [<Agent> '.'] <member>` is stated once at :1517-1520. (A residual
  *domain* question about `symbol_named`'s owner arg is R5-F1 — it was writer R4-6's other half.)
- **R4-F10 (P16 marks name-recorded slots)** — **RESOLVED.** :1413-1416 "every emitter-minted
  int64 ref cell **whose `+d.refs` record carries no `+d.name`**"; the §6.1 layout block (:952-954)
  agrees.
- **R4-F11 (nits)** — **RESOLVED.** (a) :1045-1046 "learned ones are conventionally named
  `patch:<n>`" (no more false "default"); (b) :1332-1334 "read off the clause's `d.name` label
  through the `plan_named` resolution path"; (c) :1131-1132 "`owner` takes the same
  bare-or-qualified convention as the goal/event names"; (d) :600-613 — `code` legal everywhere,
  `splice`/`static` (and `run`/`value_of` under `std:meta`) member-name-only after `.`.

## Delta checks that pass

- **§9.2/§9.3/§9.4 typecheck against the spec grammar.** `goal X() perform` / `goal Wait(w: Int)
  perform` (spec :727 `goal Survey(target_id: Str, …) perform`); `predicate` as `TopDecl`;
  `plan timed for Wait(w)` per `PlanDecl ::= 'plan' Ident 'for' Ident [ '(' IdentList ')' ] …`
  (spec :1340 — the IdentList binds *goal parameter names* per :741-742, so the doc's
  "name-keyed" formal rule at :1115-1117 is correct); `{ navigate_to("water") }` is a `Block`
  Primary (spec :1365) hence `Code<()>`; `&`-conjunction and `?t` unification legal in
  `where`/`believes` (spec :745 precedent); `emit WithinBudget{n: w}` per spec :1219; plan-param
  capture `w` → `cap$0` in §9.4 is exactly §5.3's documented capture.
- **`cap$N` stamp story is consistent.** :711-715: the stamp names the captured binding itself
  (checker resolves straight to it), env records demoted to diagnostics/readability (`cap$N`
  renders as the captured *name*); :709-711 value-bindings-only rename + program-member refs stay
  name-resolved; :728-730 + :744-750 frame-bound captures get per-activation aliases and refused
  `cells"$cap$N"` anchors — consistent with M4 :192-195 and §6.6's `closed` list.
- **`occurrence` reads are coherent** for grafted tests/bodies: `occurrence.f` through the bound
  event, and :1141's "nullary (or the clause's standard formals)" correctly routes the `as`-name
  case for `graft_where`/`graft_body` onto declared clauses.
- **`code_named` verbatim + `run`-operand name-record by label** (:1392-1394, resolved through
  `code_named`) is self-consistent: opaque labels never split, qualified names always do.

## New findings (v1.4 deltas)

### R5-F1. MINOR — `symbol_named`'s owner/d.vars story contradicts itself and spec §5.7; §9.3's call won't resolve under any consistent reading

Three texts give three resolution stories. :1112-1114: "resolved against the agent's own
**module's** `d.vars` (`symbol_named(owner, name)` taking the **agent** as owner)" — mixed within
one sentence. The new §6.7 grammar (:1519): "the remaining dotted prefix names the owning
**`Agent`'s `d.vars` cell**". Spec §5.7 (:627-629): the module's `d.vars` holds **all** symbols —
goals/events/questions as **bare** names, members as dotted `Agent.def` names. Reading the doc
consistently with the spec (and with `graft_method` :1129, which mints symbols "named `owner.name`"
on the module's `d.vars`): `symbol_named(owner, name)` must look up `owner.name` — or `name` alone
for module-scope symbols — on the resolved **module's** `d.vars`. Then :1519's "owning `Agent`'s
`d.vars`" names the wrong cell, and §9.3's `symbol_named("Forager", "Feed")` (:1832) looks up
`Forager.Feed` — but goal `Feed` is a bare module-scope name, so the call fails. This is the
unaddressed half of writer R4-6 (the disposition ruled the *split*, not the *domain*).

**Fix.** State once: "the qualified name's dotted prefix qualifies the member name on the
*module's* `d.vars` — `symbol_named(owner, name)` looks up `<owner>.<name>` there (a bare symbol
is looked up by `name` alone); goals/events/questions are module-scope names." Then §9.3 needs
`symbol_named("demo", "Feed")` (module owner, bare goal) or a stated agent-relative alias rule.

### R5-F2. MINOR — :1118-1121: the `as`-name arm has no referent for `graft_handler` (a minted clause has no "target clause")

"`graft_handler`'s formals are the handler's bound names: the event cell under `occurrence`, or
under the `as`-name when **the target clause declares one**" — `graft_handler` *mints* the clause;
there is no target clause, no `as` declaration, and no surface for a grafted clause to carry one.
As written it implies a fragment declaring `a: Add` is acceptable when "the target clause"
declares `as a` — for an append verb that clause does not exist. The `as`-name case is already
correctly covered for re-pointing verbs by :1141's "the clause's standard formals".

**Fix.** "…the event cell under `occurrence` (a minted clause binds `occurrence`; the `as`-name
rule applies when `graft_where`/`graft_body` re-point a clause that declared one)". Alternatively,
if a minted clause is meant to take its bound name from the fragment's declared formal, say so —
that's a third story the doc currently does not tell.

### R5-F3. MINOR — :1503: `reify` label spelled `reify:<serial>` — the `<src>` segment was dropped in the v1.4 rewrite

The canonical form everywhere else is `reify:<src>:<serial>` (:1035, :2229, and §6.7's own next
sentence which explains the `<serial>`'s provenance). Under a durable per-agent counter
`reify:<serial>` would still be unique, so this is a spelling inconsistency, not a collision bug —
but the rewritten bullet now disagrees with two other normative texts.

**Fix.** `reify:<src>:<serial>` at :1503.

### R5-F4. MINOR (carried over, never dispositioned) — §13 still omits `learn_*`'s two-`reify` cost

R4-F7 verbatim: `learn_*` mints two reified copies per fragment argument (:1266-1268), each an
O(fragment cells + links) recipe replay, and leaves the original dead — the most surprising cost
on the surface — yet §13's `learn` bullet (:2087-2088) prices only "`a d.patch` cell + head-chain
records + checkpoint ops + P16's two boundary passes". The orchestrator never ruled on it (absent
from r4-disposition's minor batch).

**Fix.** (repeated from r4) One §13 clause: "`learn_*` mints two fresh copies per fragment
argument (registered + installed) — O(fragment) recipe replay per copy — and leaves the original
dead."

### Nits (not findings)

- :1517 lumps `symbol_named(owner, name)` under the single-string `[<module>':'][<Agent>'.']
  <member>` grammar though it consumes an already-split pair — harmless once R5-F1 lands.
- §9.2/§9.3 qualified names assume module `demo` (`demo:Guard.cautious`,
  `demo:Forager.navigate_to`) with no `module demo` shown — cosmetic; the examples are fragments.

## Summary

v1.4 discharges all ten dispositioned ergo findings in substance — the R4-F1 major is resolved
(the bound-event formals now match spec :1205's rule verbatim), and §9.2–§9.4 now parse and
typecheck against the spec grammar including `PlanDecl`'s IdentList, `Block`-as-Primary,
`emit E{…}` and `?`-unification. Remaining defects are wording-level: four minors — the
`symbol_named` owner/d.vars incoherence (three conflicting stories; §9.3's call is its casualty),
the referent-less `as`-name arm on `graft_handler`, the `reify:` label spelling slip, and the
carried-over unpriced double `reify` — plus two nits. No mechanism rulings needed; a v1.5 nit pass
suffices.

VERDICT: CONVERGED — wording-level minors only

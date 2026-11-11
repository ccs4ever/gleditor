# R3 Review — Architect lane (ordering / deliverability freeze pass on v0.3)

**Verdict: CONVERGED.** Spec `design/vagent-actor-executor.md` v0.3 re-read against
`apps/common/xanadu/vagent/{compiler,meta,checker,ast,lineage}.{hpp,cpp}`,
`apps/common/xanadu/vortex/{mesh_actor.{hpp,cpp},vortex_vm.{hpp,cpp}}`,
`apps/common/xanadu/zigzag/arena_manifold.{hpp,cpp}`, and the reference specs
`vagent-agent-language.md` / `vagent-metaprogramming.md`. v0.2→v0.3 diff inspected in full via
`git diff b4bc08d7 82c1e45c`.

Counts: **0 BLOCKING, 0 MAJOR, 7 MINOR, 2 NIT.**

All three r2 dispositions in my lane landed and verify end-to-end: the ANF hoist lowers every gate
statement, the purity ruling keeps the gate legal under the only coherent oracle, `+d.owner` is
emitted and filed at M0 with dispatch named as consumer at M1, and M2's contract additions are
consistent with their M3/M4 consumers. What remains is precision work — an unnamed purity oracle,
an unreserved implicit-arg name, the actor-side decl identity, and several carried-over minors.
None of it blocks the gate or falsifies the milestone order.

______________________________________________________________________

## Verification answers (the six assigned questions)

### 1. The §3.8 rulings end-to-end — the gate lowers unambiguously under ANF

Walked both cited statements against §3.8 :296-345:

- **`learn_where(plan_named("demo:Guard.cautious"), code"believes threat(?t) & t > 0.9")`**
  (`vagent-metaprogramming.md:2825`). `learn_where` is a statement in the `on Attack` body →
  statement-boundary host step. `plan_named(…)` is nested in the arg list → ANF-hoisted to a
  preceding host step producing temp `t1` (`Opt<Cell>` — binds as a value; the language's
  yield-or-fail convention applies where the temp is consumed at `learn_where`'s `Cell` param,
  `vagent-metaprogramming.md:2830`). The `code"…"` literal is not a call — no hoist; the head is
  emitted at compile and marshalled as a `+d.vars` arg per the ABI (:320-323). Result:
  `t1 := plan_named(…)` → `learn_where(t1, <frag>)`. **Unambiguous.**
- **`for c in clauses_of(symbol_named("demo","Feed"))`** (`vagent-metaprogramming.md:2866`).
  `symbol_named` nested → hoisted host step (`Opt<Cell>` → unwrap-or-fail at `clauses_of`'s `Cell`
  param). `clauses_of` is named explicitly in the statement-position bullet ("`clauses_of` heads of
  `for`", :307-308) → host step yields `[Cell]`; the loop iterates the temp. Inside the body:
  `weight_of(c)` in `let`-RHS → hoisted temp (`Opt<Float>` — needed live for `if [w]`'s else
  branch); `set_weight(c, w + 0.1)` / `set_weight(c, 0.1)` are branch statements → host steps;
  `w + 0.1` is pure arithmetic evaluated in-segment. **Unambiguous.**
- **`if believes sees("food","water") & not believes learned_rule("food_water")`** (:2857): two
  hoisted host steps → `?b1`, `?b2`; `if ?b1 & not ?b2`. The ANF bullet gives this exact transform
  (:333). Gate `believes` calls are all ground → boolean results; no multi-binding semantics needed.

Residual ambiguities are minor (findings 3–5): the statement-position bullet's example list omits
`believes` itself, and the spine's control-graph shape (loop-back/branch edges between segment
regions — needed since a `for` body contains host steps) is implied by "statement-segmented" rather
than stated. For the gate — synchronous host steps, no mid-loop suspension — the implication
suffices.

### 2. Fragment purity vs the gate — nothing in the worked examples refuses

- **`code"believes threat(?t) & t > 0.9"`**, **`code"believes sees(\"food\",\"water\")"`**,
  **`where believes threat(?t) & t > 0.7`**: `ExprKind::Believes` is not in `pureBody`'s impure
  arm (`checker.cpp:1032-1053`); it falls to the default child-recursion → pure. `Term` likewise.
  `&`/`>`/`not` are pure operators. **All pure.**
- **`code"{ navigate_to(\"water\") }"`** (:2860): `navigate_to` is a `def` call — not in the
  ruling's example (`believes` reads) or ban list (`believe`/`graft`/`emit`/`learn_*`). Under the
  only coherent oracle — `pureBody`/`derivePurity` (`checker.cpp:1026-1030,:1086-1103`: a `Call` is
  pure iff the callee derives `pure`; an empty-body def is pure) — the call is pure and the
  payload's descriptor `pure` flag sets. A strict verb-whitelist reading would also ban the
  `& t > 0.9` arithmetic in the doc's own inline example (:303), so it self-refutes; "refuse at
  check time" (:336) already gestures at the checker's machinery. **Gate survives** — but the
  oracle should be named (finding 1).
- The narrowing's scope is *all* `code"…"` payloads including learned bodies — broader than the
  graft machinery's `test`-only purity (`meta.cpp:1325` vs `:1339` `pureRequired=false` for
  bodies). This is **forced by the monolith model, not an oversight**: a `code"…"` head can never
  contain a spine, so a host verb inside one could never be lowered. §9.3's body is pure → gate
  unaffected. The consequence (learned plans can never `emit`/`achieve` — effects need another
  channel) is the stated owner-flagged narrowing; worth one line acknowledging it reaches bodies,
  not just tests.

### 3. `+d.owner` on clauses — resolves the ordering question; one binding still unstated

- M0 emits `+d.owner → <agent symbol>` per clause and files `d.owner` in `kAgentDimensionNames`
  (:478-479). Milestone order is **consistent**: M0 owns emission (the edge's natural home — it is
  compile-time structure), M1 owns the dispatch that is its consumer ("dispatch keys on it", :344).
  No inversion.
- Single-agent lowering is unbroken: the filter degenerates to all-match; one extra edge per
  clause.
- Residual (finding 6): the filter's *other* half is unstated — what the clause's `+d.owner`
  compares against. `spawn A(args)` carries the decl identity (`vagent-agent-language.md:1222`,
  `:1502`), but where the agent cell records "this actor is a `Worker`" (its own `+d.name`, a
  `+d.owner`-shaped decl edge) is unwritten — persisting half of my r2 finding 4 the disposition
  adopted only clause-side. Also §3.2 step 5's dispatch text (:178-179) never mentions the filter.
  Gate impact: nil — both gate modules are single-agent (`Guard`, `Forager`).

### 4. M2's added contract items — internally consistent with M3/M4 consumers

- **`$step` via segment descriptor, not a formal** (:497-499): the rationale **verifies** —
  `runFragment` checks descriptor arity against parameter cells and run args
  (`meta.cpp:702-712` — "descriptor arity disagrees", "run args differ from fragment arity"), and
  `signatureOf` exists (`meta.cpp:1143`). A `$step` formal would fail both. M4's consumer — the
  intention record's `step` field (`vagent-agent-language.md:596-615`) — matches against per-segment
  descriptors; resume enters there. Consistent.
- **Per-segment Return-tail** (:498): matches the established convention — fragment tails are
  `Return`, not `Halt` (`vagent-metaprogramming.md:269-270`); the Return is what hands the executor
  control between segments for the host step and what lets a suspension *not* resume. Consistent.
- **Belief term-cell encoding** (:500-501): `#Unify`-consumable `+d.grab` term heads — `Unify`/
  `IsVar`/`MakeVar`/`Deref` exist (`vortex_vm.hpp:80-84`); consumed by the M2 fragment-position
  `believes` walk (agent-language:1500's one-`#Choice`-per-clause) and the `believe` verb's
  fact-minting. Consistent.
- **M3→M4 comparand**: bound-body-name recording lands at M3 (:504-505) so M4's resume check has
  its comparand — the r1 lesson applied correctly.

### 5. The M6 amendment flagging — adequate

§3.8 :335-339 says "amends M6" and names "the reference spec's M6 allowance for impure verbs in
`code"…"`" — `vagent-metaprogramming.md` is a named related document (:22-23) and M6 is a
searchable mandate ID; the conflicting text is `:271-274` ("the other non-suspending actions are
allowed and stay impure"). Header :4-5 flags the three r2 rulings for owner confirmation. The
flagging is adequate; a line-range cite would be nicer but M6 is a sufficient locator. Carried
residual: the *edit itself* still has no milestone — "Unbundled paperwork" (:511-512) names only
§6.3 + the user guide (r2 finding 9).

### 6. New majors in v0.3 text — none

The v0.3 delta is: header status, the §3.2 exit-protocol rewrite, the three §3.8 rulings, `+d.owner`
+ dim filing at M0, and M2's contract items. All verified above or under "Verified-as-accurate".
One cross-lane note worth recording (finding 7): the rewritten failure path is correct **as
written** only under an unstated invariant — see below.

______________________________________________________________________

## Findings (new, v0.3 text)

### 1. The purity oracle is unnamed — `def`-call admission is inferential

**Severity: MINOR.** §3.8 :335-339: "`code"…"` payloads and `where` clauses may contain only pure
verbs — `believes` reads". The worked examples contain exactly the unclassified case —
`code"{ navigate_to(\"water\") }"`, a `def` call (an "action" per `vagent-agent-language.md:2859`:
synchronous method call). It survives only under the pureBody reading (a `Call` is pure iff the
callee's derived `pure_` flag holds — `checker.cpp:1026-1030,:1090-1103`), which is clearly the
intent but never named. **Fix:** one line — "the check is `pureBody` (`checker.cpp:1020`); the
verdict rides the fragment descriptor's `pure` flag, so a `def`/`run` call is admitted iff the
callee derives pure — §9.3's `navigate_to` qualifies". Also state that the narrowing reaches
*body* payloads too (learned bodies are pure-only — forced by the monolith model, but say it).

### 2. The belief channel's `agent` binding — name, owner, and callsite coverage unstated

**Severity: MINOR.** §3.8 :340-343: "the where-evaluator binds the agent cell into fragment scope
on `+d.vars` (implicit arg), so the inlined clause-walk reads `agent +d.belief`". Three small gaps:
(a) the binding name `agent` is implied by the read path, not declared reserved — a user
`+d.vars` entry or param named `agent` collides; (b) "the agent cell" should say *the evaluating
actor's* cell — it arrives as `ActorExecution.stateRoot` (`mesh_actor.hpp:89-97`), and the
r2-asked "whose cell under a different actor" is thereby answered only implicitly; (c) the binding
is scoped to "the where-evaluator" — a `believes`-bearing fragment run via `run`/`test_of`+`run`
outside a `where` gets no `agent` and fails unstatedly (the purity rule makes `code"believes…"`
writable anywhere). Gate impact nil — wheres are the only gate consumers. **Fix:** "the evaluating
actor's agent cell (`ActorExecution.stateRoot`) bound under the reserved name `agent`; every
evaluator that runs a `believes`-bearing head binds it — the `where` selector today". Also answer
whether `believes t from s` (`checker.cpp:2036-2038`) resolves `s` against `agent`'s `$name`.

### 3. Statement-position bullet omits `believes`; non-ground statement `believes` unclassified

**Severity: MINOR.** §3.8 :307-310 names "`learn_*`, `set_weight`, `clauses_of` heads of `for`,
`believe`/`forget`" — `believes` (the read) is absent from the list, covered only by the ANF
bullet's `if believes` example (:331). Harmless for the gate (all its statement `believes` calls
are ground → bool), but a non-ground `believes p(?x)` in statement position is unclassified: does
the host step return first-match-bindings, fail, or enumerate? **Fix:** add `believes` to the
statement-position list and state the non-ground convention (recommend: host step yields the
first-derivation binding or fails — matching `#Choice`-walk semantics without fragment machinery).

### 4. The spine's control-graph shape is implied, not stated

**Severity: MINOR.** With ANF, conditions hoist out — `if ?b`, `for c in <temp>` keep pure
conditions — so the remaining mechanism is control edges *between* segment regions (the §9.3 `for`
body contains per-iteration `weight_of`/`set_weight` host steps). "Statement-segmented resumable
graphs" (:255) plus "resumes at the next segment" (:309) reads as a CFG but never says branch/
loop-back edges are spine structure — or whether a suspension inside a `for` records iteration
state in the record's `frame`. Gate-safe (no mid-loop suspension) but M4's resume consumes it.
**Fix:** one line in §3.5 — "a spine is a segment control graph: structured constructs lower to
edges between segment regions; `step` names a segment, `frame` carries in-region iteration state".

### 5. The ABI should say an `Opt` result binds the `Opt` — not a refusal

**Severity: MINOR.** §3.8 :321-324: "writes results back onto `+d.vars` … or translates a
refused/`expected`-error verb into the statement-level failure path". Under ANF, `weight_of(c)` →
temp; `if [w]`'s else branch exists *because* empty-`Opt` is a legitimate result — if the ABI
treated it as the error path, the example's else is dead and `set_weight(c, 0.1)` never seeds.
Distinguish: an `Opt<T>`-typed verb returning empty binds the `Opt`; the yield-or-fail convention
(`vagent-metaprogramming.md:2830`) lives at the consumer's unwrap sites (`learn_where`'s `Cell`
param, `w + 0.1`). **Fix:** "results bind by declared type — an `Opt` binds the `Opt` itself; the
statement fails only on a refused/`expected` return or where the consuming site requires the
wrapped value".

### 6. `+d.owner` — actor-side decl identity unstated; filter absent from §3.2 step 5; misplaced sentence

**Severity: MINOR.** The clause edge landed (:478-479) but the comparison target didn't: dispatch
must know *this* actor's decl — `spawn A(args)` carries it (`vagent-agent-language.md:1222,:1502`)
yet no line says the agent cell records it (`+d.name` = qualified decl name or its own `+d.owner →
<agent symbol>`). §3.2 step 5's dispatch (:178-179) doesn't mention owner-filtering at all, and the
mechanism sits inside §3.8's *belief-channel* bullet (:344) where it's topically orphaned — it is
dispatch machinery, not beliefs. **Fix:** "the agent cell carries `+d.owner → <its agent decl
symbol>` at spawn; §3.2 step 5 filters the `d.clause` enumeration to `+d.owner == <own decl>`".
Single-agent modules are unaffected either way. Optional: an M1/§10 two-agent dispatch test —
"no silent cross-agent handler stealing" is currently unexercised by the test plan.

### 7. Cross-lane note — the rewritten failure path is correct only under an unstated invariant

**Severity: MINOR (cross-lane; the exit protocol is the vortex lane's r3 verify item — flagging for
dedup).** §3.2 :194-199 — `cut(1)` → `release(m0)` → `backtrack(cursor)` onto the surviving base
CP. Verified safe **as written**: `pushChoicePoint` mints no arena cells (`vortex_vm.cpp:279-309`
— reads only; `.mark = core_.arena().mark()` at :302), and the spec's ordering puts nothing between
`m0 = mark()` and the CP push, so `cp.mark`'s counts equal `m0`'s — making `backtrack`'s
`release(cp.mark)` a no-op truncation after `release(m0)`, with the floors/marks stack staying
well-nested LIFO (`arena_manifold.cpp:1695-1698`). The latent hazard: `release` resizes
`trail_`/`slots_`/`links_`/`liveLinks_` *up* with default entries when the passed mark's counts
exceed current state (`arena_manifold.cpp:1657-1676`) — so if any arena write ever slips between
`mark()` and the CP push (a reordered step 2, a minted CP-entry cell), the pad-landing release
corrupts the arena — including inflating `liveCells`, §4.6's new invariant. **Fix:** state the
invariant — "no arena write between `m0` and the base-CP push (the pad's `release` must never be
asked to grow the arena)" — or have M1 assert `cp.mark` counts == `m0`'s. Also cosmetic: `cut(1)`
is described as "releasing their marks" (:194) — `cut` *discards* (keeps writes, :336-340); the
subsequent `release(m0)` makes it moot, but the wording is loose.

### 8. NIT — §3.2 step 5's `runFragment` is stale post-M2; `let ?b :=` syntax

**Severity: NIT.** (a) §3.2 :179 "each body = runFragment" — true at M1 (stubs are fragments) but
stale once M2's spines land; "the segment runner (a one-segment stub runs as `runFragment`)".
(b) §3.8 :333 `let ?b := believes(…)` — `?`-names are unification vars (`vagent-agent-language.md
:686-702`); a plain `t`/emitter-temp reads truer for an ANF illustration. (c) Header :5 "(§3.8,
§9)" — the three rulings live in §3.8; §9 records them only on ratification.

______________________________________________________________________

## Carried residuals (r2 items, not re-opened)

§3.4's "`believes`/`believe`/`forget`/`revise` are host-routine verbs … never opcodes" (:227) is
still stale for the fragment-position `believes` (its clause-walk *is* `#Unify`/`#Choice` opcodes)
— the cite fix landed, the reword didn't. Interim emission semantics: M0's "single-segment stub"
names *bodies*; a `where` head containing `believes` (unemittable until M2) gets no stated
stub/refuse ruling. Unbundled paperwork (:511-512) still names only §6.3 + user guide — the
M6-text edit, §4.2/§6.4/§10-table/§11.1-P4-row amendments still float. `reclaim_deadline`'s
host-duty driver, the §11.3 harness deliverables list, the adopted-intention channel, post-M4 gate
placement, step-6's counter conflation, the `$`-seed lump, and M1's outbox-staging channel all
persist from r2 findings 5/9/10 — none touches the gate's lowerability.

______________________________________________________________________

## Verified-as-accurate (v0.3 deltas)

- DeclKind count corrected to **19** (`ast.hpp:184-204` — exactly 19 enumerators).
- Clause-walk cite corrected to `vagent-agent-language.md:1479-1500` — :1500 is literally
  "`believes t` — clause walk with `#Unify`, one `#Choice` per clause".
- `signatureOf`/arity rationale for descriptor-`$step` verified (`meta.cpp:702-712,:1143`).
- `d.verb`/`d.owner` join M0's filed-dim list (:325,:479) — both are used mid-turn.
- Exit-protocol mark ordering: `m0` before base-CP push keeps the floors stack well-nested;
  `pushChoicePoint` writes nothing (`vortex_vm.cpp:279-309`), so `release(m0)` then
  `backtrack`→`release(cp.mark)` is LIFO-consistent and corruption-free *under the stated order*.
- Init/fatal cite `vagent-agent-language.md:369-372` — contains exactly the `ChildStopping` /
  `$initialized`-fatal arms quoted.
- `learnWhere` exists (`meta.cpp:1802`); `graftable` requires test-pure, not body-pure
  (`meta.cpp:1325,:1339`); `ExprKind::Believes` is pureBody-pure; `run` is opaque-impure
  (`vagent-metaprogramming.md:1166`).

______________________________________________________________________

VERDICT: CONVERGED

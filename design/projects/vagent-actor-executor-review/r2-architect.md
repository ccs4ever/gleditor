# R2 Review — Architect lane (freeze pass on v0.2)

**Verdict: NOT CONVERGED.** Spec `design/vagent-actor-executor.md` v0.2 re-read against
`apps/common/xanadu/vagent/{compiler,meta,ast,lineage}.{hpp,cpp}`,
`apps/common/xanadu/vortex/mesh_actor.{hpp,cpp}`, and the reference specs
`vagent-agent-language.md` / `vagent-metaprogramming.md`.

Counts: **4 MAJOR, 6 MINOR, 1 NIT, 0 BLOCKING.**

The v0.2 revision landed nearly all of my r1 dispositions — the milestone order is now internally
consistent (M2 owns segmentation+ABI, M3 tests adopted intentions only, M4 owns suspension), §3.8
names both call positions and a real ABI, M0 got the DeclKind table/prelude/declared head names,
§5.1–§5.2 are owned by M0.5, and the settings home moved off `ActorOptions`. But the §3.8
position-split — the revision's centerpiece — is incomplete in exactly the spots the §9.2/§9.3 gate
exercises: expression-position calls are unclassified, the in-fragment `believes` walk has no stated
path to `d.belief`, and M6's `code"…"` allow-list permits impure verbs that no stated lowering can
emit. The gate examples still do not lower end-to-end under this document's own text.

______________________________________________________________________

## Disposition check (what landed from r1)

| r1 finding                                                 | v0.2 status                                                                                                                                          |
| ---------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1 BLOCKING — M3 tests need M4 machinery; seam unowned      | **Landed.** M2 owns "statement-segmented body lowering … the §3.8 ABI"; M3 = "adopted intentions only"; mid-suspension tests moved to M4 (:469-479,:524-528) |
| 2 BLOCKING — `believes` in `code"…"` contradicts §3.4      | **Landed as the position split**, but the split is under-specified in three ways — new findings 1–3                                                    |
| 3 MAJOR — M0 enumeration                                   | **Mostly landed** (DeclKind disposition table, `std:agent` prelude, declared body-head names, directory cell, dim list). Residuals: findings 4, 8, 10   |
| 4 MAJOR — tunables in `ActorOptions`; P9 unowned           | **Landed.** §4.2 "Settings home (r1-corrected) … never ActorOptions" (:348-352); M0.5 owns `defaultSettingSpecs` rows (:461-463)                        |
| 5 MAJOR — arbitration counters have no channel             | **Landed.** "`ActorExecution` gains the arbitration counters" at M0.5 (:461). (The two secondary notes — pidKey rotation, `soleQueued` — not adopted; tolerable, §5.2 now covers the accretion class) |
| 6 MAJOR — §3.2 diverges from §4.2                          | **Partially.** Base CP, Dewey reset, per-turn scope, scope unlink, `compact_every` all added (:170-188). Drain kind-order and the §4.2 amendment itself still floating — finding 9 |
| 7 MAJOR — §5.1 unowned                                     | **Landed.** M0.5 owns §5.1+§5.2 (:463, :404, :414)                                                                                                     |
| 8 MAJOR — spec-amendment paperwork unowned                 | **Partially.** "Unbundled paperwork" names only §6.3 + the user guide (:482-483); §4.2/§6.4/§10-table/§11.1-P4-row still float — finding 9               |
| 9–11 MINOR — reclaim driver; harness shape; gate placement | **Persist** — finding 10                                                                                                                              |
| 12 NIT — `vagent run`; M1 staging channel                  | Staging channel still unnamed — finding 5                                                                                                              |

______________________________________________________________________

## New findings

### 1. The §3.8 position split has no rule for expression-position verb calls — the gate's own statements nest them

**Severity: MAJOR.** §3.8 :296-306 defines exactly two positions: **fragment position** ("inside a
`where`/`code"…"` fragment" → inline clause-walk graph) and **statement position** (named examples
are all statement *heads*: "`learn_*`, `set_weight`, `clauses_of` heads of `for`, `believe`/`forget`
in a handler or plan body" → host step). Both gate examples exercise a third class the split never
classifies — verb calls in expression position inside a statement:

- `learn_where(plan_named("demo:Guard.cautious"), code"…")` — `plan_named` is nested inside the
  host step's *argument list* (`vagent-metaprogramming.md:2824`). "The emitted segment ends at the
  call" cannot run `learn_where` with an arg that is itself an unevaluated host verb.
- `for c in clauses_of(symbol_named("demo", "Feed"))` — `symbol_named` nested inside the `for`-head
  call (`vagent-metaprogramming.md:2855`).
- `if believes sees("food","water") & not believes learned_rule("food_water")` — `believes` is an
  expression form (`'believes' Term`, grammar at `vagent-agent-language.md:1369`) sitting in a
  body-statement's condition — neither inside a fragment nor a statement head.
- `let w := weight_of(c)` (:2858) — borderline (the `let` RHS is quasi-boundary), but the general
  case (`w + 0.1` feeds `set_weight(c, w + 0.1)` — `set_weight` nested in an arithmetic arg) is not.

The two candidate fixes price very differently — **hoist nested calls to preceding statement
boundaries** (ANF-style: `let t := plan_named(x)` is itself a boundary host step; cheap, composes
with the stated ABI unchanged) versus **expression continuations in the segment spine** (a segment
can suspend mid-statement and resume into a pending operand slot; a materially harder emission
contract). The doc must pick one — the worked examples do not lower without the ruling. Note the
pure reflect reads (`plan_named`, `symbol_named`, `weight_of`, `label_of`, `test_of`, `body_of`,
`agent_cell` — all marked *pure* at `vagent-metaprogramming.md:1156-1163`) are exactly the calls that
nest; a uniform alternative is "pure verb reads lower to inlined walk graphs wherever they appear;
impure verbs are statement-boundary host steps" — but that requires a prescribed subcircuit per read
verb, which today exists only for `believes` (`vagent-agent-language.md:1500`).

**Proposed fix:** add a third bullet to §3.8: "a verb call in operand position is hoisted by the
emitter to a statement-boundary host step whose result name the operand then reads" (the ANF choice —
recommend it, since it leaves the ABI and the spine format unchanged), or name the inline-subcircuit
set per pure verb. Either way, walk §9.2's `learn_where(plan_named(…))` and §9.3's
`if believes …`/`clauses_of(symbol_named(…))` through the chosen rule once in §3.8 to prove the
gate lowers end-to-end.

### 2. M6 permits impure verb calls inside `code"…"`; §3.8's split gives them no lowering — a spec-vs-spec contradiction

**Severity: MAJOR.** `vagent-metaprogramming.md:272-274` (M6, ratified): "`adopt`, `emit`,
`believe`, `install`, `graft`, `run` itself … and the other non-suspending actions are **allowed**
and stay impure" — *inside `code"…"`*. Under v0.2's split, fragment position receives exactly one
lowering: `believes` → inline clause-walk. Every other verb inside a fragment has none: a host step
cannot fire inside `runFragment` (no segment boundary exists mid-fragment — "No host call leaves the
fragment boundary", :301), and no inline graph is prescribed for `believe`/`graft_*`/`install`/
`adopt`. §3.4 still rules `believe`/`forget`/`revise` "host-routine verbs … never opcodes" (:223) —
so `code"{ believe learned_rule("x") }"` — a legal M6 fragment, and precisely the shape a
`learn_plan` *body* payload could take — is unlowerable under this document. `emit` is the one
arguable exception (its lowering is an in-graph trailed outbox write,
`vagent-agent-language.md:1502`), which only sharpens the need for a per-form ruling rather than a
blanket one.

The gate's three fragment payloads happen to be pure (`believes …`, `{ navigate_to("water") }`), so
the exit criteria survive — but the corpus currently states two contradictory rules on a construct
users can write today.

**Proposed fix:** rule in §3.8 that fragment bodies admit only the pure/inlinable verb set
(`believes` today; any pure reflect read that later gains a subcircuit), with impure calls refused at
emission with a named diagnostic — and amend M6's allow-list in the same commit (add it to the
paperwork list, finding 9). Price the refused alternative: fragments that segment (kills V4's
"fragments cannot suspend" simplification — correctly refused).

### 3. The in-fragment `believes` walk has no stated channel to `d.belief` — the gate's central input is under-bound

**Severity: MAJOR.** §3.8 :298-301 lowers fragment-position `believes` to "the inlined clause-walk
graph." A clause walk needs a starting cell — the predicate's clause rank on the agent's `+d.belief`.
The agent cell cannot be captured at emission: it is minted per agent at spawn (M0's own ordering,
:452-455), while the `code"…"` head is shared program data in the sealed image — `+d.refs` captures
are compile-arena cells. The one in-language accessor, `agent_cell()`, is itself a `std:meta` reflect
verb (`vagent-metaprogramming.md:162,1156`) — i.e., a host step, barred inside fragments by this
doc's own rule. `self` is the agent's *name* (`$name`), not the cell
(`vagent-agent-language.md:1278`). So the fragment cannot name its input; the evaluator must supply
it — and no text says it does.

The channel exists and is nearly free: `ActorExecution.stateRoot` **is** the agent cell
(`vagent-agent-language.md:126`; the field is already threaded to the executor,
`mesh_actor.hpp:89-97`). `runFragment` binds args on the cursor's `+d.vars` (`meta.hpp:305`), so the
selector — or every `runFragment` callsite — binds the agent cell (or the `+d.belief` rank head
directly) under a reserved name the `believes` emission reads. But which name, bound where, and
*whose* cell when a learned clause's grafted test runs under a different actor — all unstated, and
the answer is load-bearing for §9.2's `where` re-evaluation.

**Proposed fix:** one line in §3.8's fragment bullet: "the evaluator binds the running actor's agent
cell on the cursor's `+d.vars` under the reserved name `agent` (from `ActorExecution.stateRoot`)
before `runFragment`; the `believes` walk reads `agent`." Also state whether `believes t from s`'s
source qualifier reads `agent`'s `$name`.

### 4. The clause→agent ownership edge is still unowned — multi-agent modules dispatch wrong, silently

**Severity: MAJOR.** Persisting from r1 finding 3, still unlanded: M0 puts every `plan`/`on`/`def`
clause on the *shared module symbol's* `+d.clause` rank (:451) with the shape
`+d.name`/`+d.grab`/`+d.spin` — no edge records which `agent` decl authored the clause. In a
two-agent module — `agent Worker` + `agent SearchCoordinator`, same module
(`vagent-agent-language.md:1727,1737`) — an `on E` handler authored under `Worker` lands on `E`'s
rank indistinguishably from `SearchCoordinator`'s, and §3.2 step 4's dispatch ("event type symbol →
`d.clause` handlers", :177) runs *both* agents' handlers on either actor. Not refused — wrong. The
spawn side is the mirror gap: M0 mints "agent cell" (:453) but never names the channel mapping a
`spawn Worker()` request to the `Worker` decl (a module can declare several; the spawn→decl link —
decl `+d.name` match or an explicit edge — is unwritten).

**Proposed fix:** add `+d.agent → <agent decl symbol>` (or the decl's qualified-name label) to the
M0 clause-cell shape, and a dispatch filter line in §3.2 step 4. Cheap stopgap if ownership is
deferred: the M0 `DeclKind` disposition table refuses a second `agent` decl per module with a named
diagnostic — honest, and keeps §12.1-class programs out until the edge lands.

### 5. §10's M1 test cites outbox staging that no milestone ≤ M1 owns; the drain is owned twice

**Severity: MINOR.** §10 :519: "**M1**: deliver event → handler fires → **outbox staged** →
committed." Staging an entry needs either `emit` emission — scheduled nowhere (the scalar kernel
refuses it today, `compiler.cpp:714-716`; no milestone lists `Emit`) — or adapter-minted cells, which
the test doesn't say. Meanwhile §8 M1 lists "C++ drain" (:467) and M4 lists "outbox staging +
post-commit drain" (:478) — the drain appears owned by both. As written the M1 gate test is
ambiguous about which machinery proves it. **Fix:** "outbox entry staged by the §11.3 harness
(`emit` emission lands with the effect statements at M4 — or name its milestone)"; and split M4's
line to "effect-entry staging + idempotence keys" since the drain itself is M1's.

### 6. The M0 stub→M2 spine transition leaves dispatch shape and spine scope unstated

**Severity: MINOR.** M0 bodies emit as "a single-segment stub" (:458-459); M1 dispatches them via
`runFragment` (:466) — **consistent and implementable**: a stub is a `compileFragment`-shaped head
and `runFragment` needs nothing more (`meta.hpp:305`; verified — r1 question answered yes). But two
things are unstated: (a) whether the stub is a *degenerate spine* — descriptor + one segment — so
M2's segment runner runs M1 dispatch unchanged, or a plain fragment, in which case M2 replaces the
dispatch path (small rework either way; say which, because "each body lowering starts as a
single-segment stub" reads both ways); (b) §3.5 scopes spines to *plan* bodies ("plan bodies lower
as statement-segmented resumable graphs", :250-251) while §3.8 puts host steps in "a handler or plan
body" (:304) — `on` bodies must segment too for the gate's `learn_where`/`believe`/`for` calls, just
without `step`-path resume. **Fix:** "every clause body emits a spine; a no-verb body is a
one-segment spine; `step`-dispatch applies only to intention resume (§3.5)."

### 7. The per-`ExprKind` emission schedule is still missing — including what an unemitted kind does to an M0 compile

**Severity: MINOR.** The DeclKind disposition landed; its statement-kind twin did not. ~30 of ~55
`ExprKind`s hit the scalar kernel's default refusal (`compiler.cpp:712-716`): `Emit`,
`Believe`/`For`/`While`/`FindAll`, `ForBelieves` (`ast.hpp:61` — in-graph per
`vagent-agent-language.md:1490`), the effect statements (`achieve`/`adopt`/`ask`/`await`/`drop`/
`spawn`/`yield`/`terminate`). Which gain emitters at M2 ("segment spine emission") vs M4 is never
decomposed, and the interim rule is unstated: a body containing `learn_where` at M0 — does the
lowering refuse the whole compile (then even M1's harness cannot load §9.2's module — fine, but say
so) or mint a refusing stub segment? **Fix:** one line per kind-group with its milestone, plus the
interim refusal semantics in the M0 item.

### 8. §3.8's clause-walk citation points at the wrong document; §3.4's "never opcodes" is now stale

**Severity: MINOR.** §3.8 :300-301: "the pure-arena belief-match subcircuit the reference spec
prescribes … (`vagent-metaprogramming.md` §9.1's clause-walk lowering)" — metaprogramming §9.1 is
the `splice`/`code"n: Int | n+1"` example (:2791-2808); the prescribed lowering is
`vagent-agent-language.md:1500` ("`believes t` — clause walk with `#Unify`, one `#Choice` per
clause") plus std:agent's beliefs part (:1546-1550). And §3.4's "`believes`/`believe`/`forget`/
`revise` are host-routine verbs (§3.8), never opcodes" (:223) now contradicts §3.8 for `believes`
itself — the fragment-position lowering *is* opcodes (`#Choice`/`#Unify`). **Fix:** correct the cite;
reword §3.4 to "the belief *writes* are host routines; `believes` reads lower to the clause-walk
subcircuit (§3.8)."

### 9. Paperwork residual — four reference-spec amendments still float, plus M6's allow-list if finding 2 lands

**Severity: MINOR.** "Unbundled paperwork" (:482-483) schedules only §6.3 + the user guide. Still
ownerless: §4.2's ten-step contract vs §3.2's drain order (`rankAfter` insertion order ≠ the
contract's kind-ordered drain — `vagent-agent-language.md:349-350` — and ready-service still runs
inside every `m0` rather than as wake-turn work); §6.4's per-condition `vagent.condition_fuel` →
the aggregate account (:789); §10's settings table (:1563-1576) vs the ≥8 new `vagent.*` names;
§11.1's arbitration paragraph + the P4 row (:1644-1657,:1670) — the mesh change lands at M0.5, the
ratified P4 text doesn't say so anywhere; and M6's `code"…"` allow-list (finding 2). **Fix:** extend
the unbundled line to "spec edits travel with the implementing milestone: §4.2+§6.4 with M2, §10
table with M0.5, §11.1+P4-row with M0.5, M6 allow-list with M2."

### 10. Carried-over residuals (each small)

**Severity: MINOR.** (a) `reclaim_deadline`'s host-duty driver still has no owner — §4.6 :386-387
names the duty, not who pauses ingress/wakes to quiescence. (b) M1's "§11.3 harness shape" is still
a shape — deliverables list (loopback transport, deterministic issuer, mock host-binding registry,
staged-outbox/`$stats` exposure, run-until-idle) still unwritten. (c) M3's "adopted intentions" —
the adoption channel is unstated (in-language `adopt` needs an emitter — finding 7 — or the harness
mints the record; say which), and the intention-record schema is split fuzzily between M3 ("minted
at adoption, bound-body-name recorded") and M4 ("intention records + step resume") — name M3's
subset. (d) The gate bullet still sits after M4 in §8/§10 though nothing in §9.2/§9.3 consumes an M4
deliverable — move it to end-M3 or state the placement is cosmetic. (e) §3.2 step 6's "bounded by
arbitration counters" (:181) conflates the §4.1 source-arbitration deficit counters with intra-turn
ready-service bounds (population caps + turn fuel do that) — reword or define. (f) `$`-reserved
seeds still a lump (:453).

### 11. NIT — "the surviving base CP"

**Severity: NIT.** §3.2's failure note (:196): "fail_step runs against the surviving base CP" —
backtracking *consumes* the CP; the base CP's alternative is the landing pad, not a survivor.
"Runs as the base CP's alternative" is the shape.

______________________________________________________________________

## Verified-as-accurate (v0.2 deltas)

- M0 stub + M1 `runFragment` dispatch is implementable as written — `runFragment(head, args)` needs
  only a fragment head (`meta.hpp:305`; binding on cursor `+d.vars` per `meta.cpp:664-800`).
- §3.8 ABI claims check out: `resolveDimension` refusal at `meta.cpp:818-825`, `mintMetaBindSet`'s
  no-marks rule at `meta.cpp:65-68`, `learnBody`'s `+d.spin` repoint at `meta.cpp:1850-1851`.
- M2/M3/M4 test-plan rows are now internally consistent with the milestone owns (the r1 M3
  falsification is repaired); `ActorExecution` counter gain is at M0.5 (:461).
- Scalar-kernel refusal wall re-verified: `run`-only `Call` arm at `compiler.cpp:641-648`, default
  refusal at `:712-716`.

VERDICT: NOT CONVERGED

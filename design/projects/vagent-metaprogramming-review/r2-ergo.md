# Round 2 critique — Programming Language Design & Ergonomics (v1.1)

Scope: same as r1 — type rules, scoping/hygiene, staging discipline, error paths,
learnability, consistency with `design/vagent-agent-language.md`. Re-verified code
claims: `parser.cpp:851` gate, `checker.cpp:554-562` scalars/arity, `:765-779`
builtin sig table, `:2061-2071` allowlist + builtinTypes, `checker.hpp:12-27`
CheckLimits, `compiler.hpp:28-34` CompiledStatement, spec §8.9 grammar (`Type`
admits `'(' ')'`, `believes`/`findall`/`for`/`if` are `Expr`), §5.8 frames,
§5.9 `Cell`.

## R1 verdicts — all resolved

- **F1 (def-param contradiction)** — RESOLVED. M13 (lines 331-340) adopts the
  template model; §5.2 (562-575) makes the statically-known list exclusive:
  `Code` param legal only during its `def`'s expansion; refused list now reads
  "field, `reflect`-family result, any `def` parameter outside its own
  expansion". Residual-splice-is-compile-error stated (575, 1042).
- **F2 (position/purity re-verification)** — RESOLVED. §5.2 (566-570) re-checks
  the substituted body in the site's `context.bounded`/`suspend`/`body` (the
  machinery verified at checker.cpp:109-198,1234); §6.3 (871-875) gives
  per-graft-point requirements — `graft_where`/`graft_handler` tests `pure`,
  bodies `Code<()>` may be impure, `graft_method` matches effect-ness.
- **F3 (`static` closedness)** — RESOLVED. M5 (195-210)/§5.5 (644-672):
  form-level closedness, `let x := static e` is the binding form, `code` params
  forbidden, arena-reading forms forbidden by name (`cells`, `believes`,
  `findall`, `run`, host/effect calls, implicit names), results restricted to
  `Int`/`Float`/`Str`/`Bool`/`()`.
- **F4 (call-by-name)** — RESOLVED. §5.4 (621-629): call-by-value via fresh
  `let` locals, argument order, matches `run`'s frame discipline; §9.1 shows it.
- **F5 (shadowed names)** — RESOLVED. §5.1 (508-518): checker rejects
  `def`/`method` named `splice`/`static`; `x.splice()` still works;
  `splice f(args)` never splices a call result; `static` is `Unary`-prefixed
  with the `static 60 * 1000` caveat stated.
- **F6 (capture-liveness)** — RESOLVED. §5.3 (608-611) states the invariant.
- **F7 (purity/`Code<()>`/place)** — RESOLVED. §5.2 (555-560): `code"…"` pure,
  non-producing bodies `Code<()>` (spec `Type` admits `()`, line 1395 — the
  annotation is writable), `splice` never a place.
- **F8 (nested literals/diagnostics)** — RESOLVED. §5.1 (528-548): nested
  example, `src:m:l:c+o:c` offset format, `#` stripped pre-lex.
- **F9 (naming/expr-decl boundary)** — RESOLVED. §5.1 (538-539) states
  fragments-are-expressions; glossary `splice` (127) disambiguates `#Splice`.

## Findings

### R2-F1. BLOCKING — §6.6/M11/§9.3: `closed` sees captured-binding links, not program-side `Call`/ref targets — the flagship `learn` is non-durable

**Problem.** `closed` = "only outside references are state-side binding cells";
its provenance is a state-side bit on `CompilerBinding` (§6.6, ~996-1003). That
inspects *captured names*. It does not inspect the fragment's int-content
references to **program-side** cells: a `def`/`method`/`std:meta` call inside a
fragment lowers to a `Call` whose target is an int64 naming the callee's graph;
`emit` references the event's cell. Those targets are program skeleton — never
promoted (V12, §4.5 row 3). §4.5's own "intra-graph reference ints" column and
P16 relocate *fragment-owned* ref cells, but `promote`/`importValue` can only
translate handles to cells it promotes: a ref naming an unpromoted program cell
either rehydrates to garbage, or — if OpHandle reachability promotes it — drags
the program skeleton into the checkpoint, re-violating the V12 amendment M11
exists to scope. This is the r1 BLOCKING ("int64 refs die at promote") one
boundary over, and `closed` as defined cannot see it.

**Evidence.** §9.3 (1264-1292): `learn_plan("Feed", "patch:near_water", test,
code"{ navigate_to(\"water\") }")` — `navigate_to` is a `def` (1269), so the
learned body carries a program-side `Call` ref. §6.6 (996-1003): `closed`
provenance is binding-side only. §10.2 P16: "Scope: fragment-owned ref cells
only — the program skeleton is not promoted anyway." `believes`/predicate/
`cells"##"` refs are name-resolved at run time and *are* durable — only
call/event refs break.

**Fix.** Pick one and state it: (a) `closed` is redefined over *all* outside
references — fragment emission refuses `closed` when any ref cell (Call
target, event cell, embedded `run`/`graft` call) names a non-state,
non-fragment cell — and §9.3 is rewritten accordingly (`navigate_to`'s body
inlined or reached by a durable name); or (b) fragment calls bind by qualified
name — recorded on `+d.refs` and re-resolved at genesis through the same
name-path patches already use (`d.vars` symbol walk, the §6.3
"callable-by-name" mechanism) — and `closed` is defined as "no non-durable
outside references". Either way, state the user-level contract in one
sentence: a `learn`able fragment is self-contained — literals, its own params,
`believes`, `cells"##…"`, and (under (b)) name-bound calls.

### R2-F2. MAJOR — §5.4/§6.1/§6.2/§8.5/§9.1: `run`'s `args: [Cell]` marshalling is internally inconsistent and unusable in v1

**Problem.** The signature `run(c: Code<T>, args: [Cell]): T` (glossary 130,
table 771, M6 213) cannot be what the examples do: `run(c, [3, flag])` (627)
passes `Int`+`Bool` — not `Cell`, and a heterogeneous list, which `[T]` typing
(spec §5.9/§8.7) rejects outright; `run(inc, [count])` (1237) passes an `Int`
var; `run(fitness, [candidate])` (1184) passes a `Code` (distinct from `Cell`,
M3). Even if `[Cell]` is the intended contract — cells *holding* the argument
values — there is no way to construct one: `cells"…%"` literals mint
string-content cells only (spec §8.7; `cell_path.hpp:31`), `set_weight` writes
`Float` only, and no `box`/`value_of` verb exists. So `run` literally cannot
receive an `Int` argument in v1. Boundary checks are also unplaced: descriptor
comparison is a runtime check (§7.2), but when the `Code` is a let-bound
literal the checker knows the signature — `run(inc, [x, y, z])` arity/type
should be a check error, unstated. And `run` on `Code<()>`: does it yield `()`
(produced=true) or always fail (produced=false)? M6 says "yields `T` (or fails
when `produced` is false)" — the `()` case is unwritten.

**Fix.** Choose one convention and repair the other side: (a) keep `[Cell]` —
args name value cells, `run` dereferences content into the frame — then add
`box`/`unbox` (`cell_of`/`value_of`) verbs so scalar args can be constructed,
fix all three examples, and state which param types are marshallable
(records/lists/`Opt` presumably are not — say so); or (b) make `run` a checker
special form taking *values* — `run(c, a, b)` sugar or a param-name-keyed
record — matching §5.8's "arguments hang on the frame's `d.vars`/`d.values`"
(they are values, not cells). Also: state compile-time arity/type checking
when the fragment is statically known; `run` on `Code<()>` yields `()`; and
whether `Code`/`Cell` coerce as `args` elements.

### R2-F3. MAJOR — §5.3: the `cap$N` binding contract is underdetermined — copy vs alias decides `closed` and whether `splice`/`run` diverge

**Problem.** "a synthesized `let cap$N := <name>` is inserted at the head of
the innermost block enclosing the literal" (592-600). Read as a real `let`,
this *copies* the captured value into a fresh local at literal-eval time.
Consequences if so: (a) `splice` reads the snapshot; `run`/`graft` reads the
live captured cell — `var x := 0; let c := code"x+1"; x := 5; splice c` → 1,
`run(c)` → 6 — a silent divergence between the two composition modes; (b) the
copy lives in an enclosing local — never state-side → `closed` is false for
every capturing fragment → `learn` degrades to capture-free literals, which
makes the whole "captured state-side bindings" design of `closed` pointless.
Read as an *alias* — `cap$N` names the definition-site cell directly — both
paths see the same live cell and `closed` tests the captured cell's provenance,
which is the only reading coherent with §5.3's "the cell *is* the variable"
caveat and with `closed` existing at all. The doc's prose ("cap$N thereby binds
the definition-site cell") suggests alias; the `let` notation says copy. A
second axis is also unspecified: which cell the binding names under the two
local models — scalar-kernel statics (`localCells_` minted per compile,
compiler.cpp:432-434) vs the §5.8 frame ABI (locals on frame `d.vars`, fresh
per `#Call`, vortex_vm.cpp:860). The "one dead frame cell per captured call"
caveat presumes the frame model; under static locals, every `Code` escaping a
parameterized `def` aliases one capture cell — `def adder(n): Code { code"x|x+n" }`
called twice returns fragments both reading the latest `n`.

**Fix.** State explicitly: `cap$N` is a name alias for the captured binding's
cell — the synthesized "let" is a scope entry for the checker, not an emitted
copy (or, if snapshot is intended, say that `splice` snapshots at literal-eval
while `run` reads live, and accept that `closed` applies only to capture-free
fragments — but then delete the state-side-binding machinery). Captured cells
must be referenced by *links* (wing/`+d.captures`-style) so `promote` relocates
them — int refs would re-hit R2-F1. State which cell model a `cap$N` binding
records, or mark it ABI-debt with the observable rule written down.

### R2-F4. MAJOR — §6.6/§6.7/§6.1: durable identity by `d.name` has no uniqueness or minting rule

**Problem.** `unlearn` and genesis resolve recorded heads *by name* through
`code_named`/`plan_named` (§6.6 965-971; §6.7 1013-1018). But `reify` mints a
fresh head with unspecified `d.name` — if it carries the source's label
(`src:m:l:c` or user `#label`), two live installed heads collide; duplicate
user `#label`s have no rule; `code_named`'s match semantics (first in rank
order? any? error?) are unstated. A `learn`ed reify — §8.5's search loop
produces exactly these — records a name that resolves to the wrong head: the
wrong patch replays, the wrong attach is undone, silently.

**Fix.** `reify` mints a fresh unique label (e.g. `reify:<src>:<serial>` or a
label parameter); `learn`/`d.code` registration refuses or renames a colliding
label; state `code_named`'s match semantics.

### R2-F5. MAJOR — §6.7: `d.code` registration trigger over-registers; "stored" is unimplementable

**Problem.** "Every `code"…"`-born head is registered when first `run`/`graft`ed
or stored" (1012-1013). Under P16, `d.code` reachability is what promotes a
fragment — so every *session* `run` or `graft` drags the fragment into every
subsequent checkpoint (promotion + relocation cost) for zero benefit: handles
are valid within the incarnation and meaningless after `resume` regardless.
"Or stored" has no mechanism — `:=` into a `var` is a value write; nothing can
observe "a `Code` was stored".

**Fix.** Register only on `learn_*` (already eager, 963-964) plus an explicit
`register(c)` verb if session label-lookup is wanted; delete "or stored"; if
eager registration is deliberate (session `code_named`), say so and price it
in §13.

### R2-F6. MAJOR — M13/§5.2: the template rule's boundary is unspecified

**Problem.** "A `def` with `Code<…>` parameters is a template expanded at each
call site" (331-340) leaves the edges users hit: (a) trigger scope — does
`Opt<Code<Int>>`, `[Code<Int>]`, or a `Code`-bearing record param count?
(the sane rule: a parameter whose declared type is `Code<…>`; nested `Code` is
an ordinary opaque param, composing through `run`); (b) coverage — member
`def`s in agents/behaviors/skills, `answer`/`question`? (c) the call-site
error — `Code` arguments must be statically known (literal, let-bound literal,
enclosing `Code` param); an opaque arg (field, `code_named` result) needs a
specified error, e.g. "cannot expand `f`: `Code` argument is opaque —
templates compose literal fragments only"; (d) **control flow in expanded
bodies** — `code"…"` refuses `return` (547-548) but the template `def`'s own
body may contain `return`/`yield`/`consume`/`terminate`; inlined, `return`
exits the *caller* — the same capture class `cap$N` fixes for names, unhandled
for control — refuse these in template bodies or specify rebinding;
(e) two-phase checking — are template bodies checked at declaration or only
per call site? errors surface per-site — say so; (f) refactor consequence —
adding a `Code` param silently converts a runtime `def` to a template and the
errors land on its `effect`/`native`/`capacity`/`graft_method` uses, not the
signature.

**Fix.** One paragraph in M13 or §5.2 covering trigger scope, member coverage,
the statically-known-arg rule + call-site error text, the control-flow
refusal, and per-site checking.

### R2-F7. MINOR — diagnostics for the new machinery

**Problem.** §5.1 specifies `src:m:l:c+o:c` for in-body errors; the rest of
the error surface is unwritten: (a) `cap$N` must render as the captured name —
"`cap$3` has type `Str`, expected `Int`" is unactionable; (b) site-context
re-verification failures (the F2 fix) — does the error name the splice site,
the fragment, or both? Should be e.g. `splice at src:demo:20:5: fragment
src:demo:12:4+2:3 contains ':=' (needs bounded position); this context is
pure`; (c) clones must retain source positions or the +offset mapping dies at
substitution — state it in the cloneExpr spec; (d) `learn`'s `closed` refusal
should name offending captures/refs; (e) nested `code"…"` needs chained
offsets (`+o:c` per level) — unstated; (f) template-body errors point at the
call site with the template decl noted.

**Fix.** A diagnostics paragraph in §5.6 or §7.1.

### R2-F8. MINOR — deref-after-consume contract for `graft`/`installed`

**Problem.** M14 (342-351) states the second-graft refusal; unstated:
(a) `run`/`reify`/`label_of` on an *installed* fragment stay legal — install
is occupancy, not destruction; (b) the occupancy marker is a trailed backlink —
a drained/backtracked graft frees the `Code` for re-graft; (c) which backlink
combination is the test — "any of `-d.grab`/`-d.spin`/`-d.values`" vs the
kind-specific one; (d) `prune`/re-point frees installability; (e) **a
`code"…"` literal evaluates to the same head every evaluation** — a literal in
a loop or returned by a `def` cannot be grafted twice; factories/loops need
`reify` — worth an explicit example, since `for … graft(code"…")` is the
natural user mistake.

**Fix.** Extend M14's contract paragraph with (a)-(e).

### R2-F9. MINOR — consistency and small semantic edges

(a) "Statically known" operand set: transitive `let` chains (`let b := a`)
unstated; `var`-bound literals deserve an explicit refusal line (users will
write `var c := code"…"; splice c`); §5.2's list omits fragment `CodeParams`
(§5.4 adds them — unify in one place). (b) `run` is parametric (`Code<T> → T`)
— inexpressible in the `Function` table (checker.cpp:765-779 gives `text` as
monomorphic `any`); "checker-special (like `text(x)`)" understates it — it is a
real special form, as are the bare `Code` params on `graft_*`. (c)
`graft_where`/`graft_handler` "the clause's standard formals" — for
`graft_plan` the clause is new; the formals are the *goal's*; the
param→formal binding convention (name or position) and `occurrence`
availability for grafted handler bodies are ABI debt — state intent or
restrict v1 grafted tests to nullary; relatedly, **grafted tests cannot bind
`?`-vars for the body** (fragment graphs don't export bindings) — `learn_plan`
clauses are weaker than declared `where`+body — worth stating. (d) Fragments
are always non-effect (M6) → `graft_method` onto an `effect` member always
refuses — say so; the descriptor carries no `effect` flag, consistent, but
note it. (e) `test_of`/`code_named` yield `Code<Cell>`; `run` then yields an
unreadable `Cell` (no `value_of`; `weight_of` is Float-only) — the promised
"validate+re-descriptor" upgrade has no named verb — name it or drop it.
(f) `reify` on `reflect`-wrapped program graphs is U-M7 — §8.5's
`reify(base_rule)` only works when `base_rule` is literal-born; say so.
(g) `reify` "minting a fresh *closed* copy" (772) overclaims: closed iff the
source's captures are state-side. (h) `static` edges: does `let x := <literal>`
count for closedness vs `let x := static e`? `var` exclusion is implicit;
bare `splice`/`static` with no operand — name the parse error; `static` in a
spliced fragment re-evals per site — price note. (i) Opaque `run` is
compile-impure — even provably-pure reflected tests cannot `run` inside
conditions; state the cost (a `run_checked` variant doing a runtime descriptor
check is the candidate). (j) Spec bookkeeping to note: vagent §8.1's
contextual list gains `code`/`splice`/`static`, §8.7's scalars gain `Code`,
and `builtinTypes` (checker.cpp:2069-2071) needs `Code` or a user `record
Code`/`def Code` collides.

## Summary

v1.1 resolves every r1 finding cleanly — the expand stage, M13 templates,
call-by-value splice, and the rewritten `static`/`closed` disciplines are all
present and consistent with the spec's grammar (verified: `()` is a Type,
`believes`/`&`/`for` are `Expr`, no keyword collisions). The remaining
ergonomic debt is the `Cell`↔value marshalling boundary (`run` args, scalar
reads/writes), the `cap$N` binding contract's alias-vs-copy ambiguity, durable
naming uniqueness, the template rule's edges, and — the one hard item — the
`closed` flag's blindness to program-side `Call`/event refs, which makes
§9.3's own `learn` example non-durable under any ABI outcome.

VERDICT: NOT CONVERGED

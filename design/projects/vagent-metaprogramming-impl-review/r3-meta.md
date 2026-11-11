# r3 — metaprogramming / staging-semantics review

**Verdict: NOT CONVERGED** (all r2 findings closed — F1–F6 and the nits landed
near-verbatim; one new work-item omission in phase 2's headline bullet, plus
clause-level residues)

Counts: **0 BLOCKING · 1 MAJOR · 6 MINOR · 5 NIT**

## r2 closure verification (all closed)

| r2 | Claim | v3 status |
| --- | --- | --- |
| F1 | skeleton refusal needs locus/scope/recursion | **Closed** :115-117 — "walks the merged program *and* each literal body recursively — any `Splice`/`Static` node anywhere is a named `DiagnosticCode` refusal at expand … nested literals emit records through the same recursion" — the suggested sentence, adopted. |
| F2 | `check()` env-walk fallback + call-site ripple | **Closed** twice: phase 1 names the ~10 `vagent_checker_test.cpp` ripples + the defaulted-parameter-or-test-edit option (:124-126); phase 2 resolves the un-stamped rule to **env-walk fallback** with the reason (`checker.hpp:30` public API; refusing breaks the suite) and names the missing-`Code`-record refusal (:170-173). |
| F3 | `closed` = outside-ref classification, not capture set | **Closed** :174-177 — "off the per-fragment *outside-reference classification* `expand` leaves behind: `cap$N` captures and name-resolvable program refs both count *toward* `closed`; a program-side ref carrying neither is the refused class that clears it" — the correct three-way reading of §6.1's glossary (spec :158). |
| F4 | descriptor schema under-stated | **Closed** :210-215 — logical fields now enumerated (ordered `(name,type)` param cells, result type, `pure`/`closed`/`effect`-ness, `result`/`produced` records, `+d.values` channel). Small edge-vocabulary residue → m5 below. |
| F5 | `static`'s §5.5 set half-named | **Closed** :165-168 — all four checker arms named (pureBody impurity, arena-reading forms incl. implicit names, literal-expressible result type, closedness) + per-arm exit tests (:200-202). |
| F6 | `run` special form "under the import" | **Closed** :244-245. |
| n7 | `VAGENT_SYNTAX_OBJS` third list | **Closed** :108-110 — offset-only diagnostics chosen, with the alternative named (cite drifts one line — n1). |
| n8 | M13's edges compressed | **Closed** :155-157 — `recursive-template` DiagnosticCode, control-flow bar, `effect`/`native`/`capacity`/`graft_method` bar all named (two refusal-class edges still unlisted — n2). |
| n9 | `learn_*` conditional-on-P16 unacknowledged | **Closed** :328-330 — the split *is* the honoring, stated. |

Cite spot-checks, all clean: `compiler.hpp:15-18` (`{cell,type}` — the
marker-bit baseline exact), `compiler.hpp:23-26` (`ScalarCompilerLimits`),
`compiler.cpp:884-891` (`localReferences_`/`externalReferences_` key on
`const Expr *`), `compiler.cpp:500-502` (preflight refusal arm), `:108-110`
(baseline invariant), `:160-181` (render check + `externalReferences_`),
`:769-772` (`dimConstant` mint), `:475-483` (anchor→`bindings_` resolution —
feeds m2), `checker.cpp:2061-2067` (allowlist), `:2068-2069` (`builtinTypes`),
`:827` (`pureBody`), `parser.cpp:848-855` (prefix gate),
`vortex_vm.cpp:98-100`/`:854-893` (`setOpcodeKind`, `Call` frame writes —
`toInt(inputs[0])` at :856 confirms the head-vs-operand claim),
`vortex_core.cpp:14`/`:50-69` (ctor `initGenesis`; 17 mints + `home`/`d.dims`
= nineteen), `:196-229` (`writeValue` covers Int64/Double/Bool/None only),
`publication.cpp:1093-1103`/`:1129-1131`, `dimension_registry.cpp:194-217`
(`getOrCreate` fold = injected base), `store.cpp:3836` (`adoptOpRecords`),
`:1418`/`:1425` (the disposition's corrected pair), `arena_manifold.cpp:34-46`/
`:78-105`/`:289-290`, `arena_manifold.hpp:845-847` (`promote`),
`worker_context.hpp:52-54` (takes `const Manifold *`, not `Store` — the
tightened wording is correct), `mesh_actor.cpp:182`/`:261`/`:473-477`/`:659`,
`mesh_evaluator.cpp:735`, `mesh_jobs.cpp:304-318`, `overlay.cpp:46-82`,
`mesh_value.cpp:473` (`d.op-label`), `or_parallel.cpp:181-184`, Makefile
`:649`/`:688-690`/`:692`/`:698-699`/`:1147`/`:1348-1351`. Spec-side:
§5.3's frame-bound paragraph (:846-854), §5.4's marshalling (:903-908) and
splice-only rules (:924-934), §5.5 (:938-977), §5.6 pipeline (:979-1038),
genesis (d)/(e) (:2346-2406) — v3's renderings all faithful.

## New findings

### M1 — `splice`'s call-by-value marshalling is absent from phase 2's work list — MAJOR

**Plan ref:** :153-157 — "splice substitution via `cloneExpr`" is the whole
statement of substitution mechanics.

**What the spec shows:** §5.4 (:903-908) defines the marshalling the
substitution must perform — "the expand stage binds each argument to a fresh
`let` local at the head of the expanded body — each argument evaluated once,
in argument order — matching `run`'s frame discipline rather than textual
inlining (`splice c(f())` must not evaluate `f()` per parameter use)" — and
M13's non-`Code` args marshal identically. §9.1's own worked example spells
the output shape: `splice inc(count)` → `{ let p0 := count; p0 + 1 }`. This
is distinct work from `cloneExpr`: synthesizing `let` decls with generated
names and re-pointing the cloned body's parameter references at the new
bindings (re-stamp or rename). It is also the design's *only* synthesized
`let` — §5.3 makes a point that `cap$N` is *not* one — so a reader who treats
"substitution" as capture-avoiding textual replacement gets multi-eval
semantics on impure actuals: a semantic bug, not a diagnostic-code gap. The
phase-2 §9.1 exit would catch a wrong implementation (its expansion *is* the
let-marshalled form), and the bullet cites §5.4 for dead-`let`, so the rule
is discoverable — but the plan's own contract is that each bullet states its
work items; this one omits the load-bearing half of its headline step.

**Suggested fix:** extend the substitution item — "splice substitution is
call-by-value (§5.4, §9.1's `{ let p0 := …; … }` shape): each argument binds
a fresh `let` local at the expanded body's head, evaluated once in argument
order, parameter references re-pointed at the synthesized binding; M13's
non-`Code` args marshal identically. The synthesized `let` is the design's
only one — `cap$N` remains a pure alias (§5.3)."

### m1 — the `splice`/`static` prefix *productions* are nowhere in phase 1 — MINOR

**Plan ref:** :101-103 (parser bullet: `code` gate, `Primary`/`Type`,
`parseCodeBody` only); :115-117 (skeleton refuses `Splice`/`Static` nodes);
:130-132 (the "re-parses as the prefix form" rationale).

**What it misses:** for the skeleton's refusal to have anything to refuse,
the parser must first *produce* `ExprKind::Splice`/`Static` — the
unary-prefix productions (`static` is `Unary`-prefixed, operand mandatory,
spec :960-961; `splice` wherever an operand follows, §5.1). The refusal
bullet's rationale implies the prefix form lands this phase, but the parser
bullet never lists it. Without it `splice inc(count)` fails as a parse or
unknown-name error — the phase-1 exit's "`splice`/`static` refusal tests"
would pass for the wrong reason, at the wrong locus, with no
`DiagnosticCode`. One line in the parser bullet closes it.

### m2 — the `$cap$N`-anchor-on-frame-bound refusal has no interim statement — MINOR

**Plan ref:** :154 (phase 2 does `$`-anchor rewriting), :264-268 (phase 3
states the frame-bound classes "before the capture-slot refusal rules").

**What the code/spec shows:** §5.3 refuses `cells"$cap$N"` anchors on
frame-bound captures; anchor resolution is compile-time through `bindings_`
(`compiler.cpp:475-483` — a missing entry dies `UnknownName`). Under phase
2's interim no-call ABI there is no frame distinction — every local is a
one-activation `localCells_` cell — so a rewritten `$cap$N` anchor either
resolves (an acceptance §5.3 refuses) or fails the generic `UnknownName`.
Either way the named arm cannot land before phase 3's classification; the
plan never says which interim behavior holds. One clause — e.g. "the
`$cap$N`-anchor-on-frame-bound named refusal rides this phase's
classification; pre-phase-3 such anchors resolve under the single-activation
shape or fail the kernel's generic anchor lookup" — pins it.

### m3 — the `Code`-parameterized (splice-only) fragment class's refusal arms — MINOR

**Plan ref:** :159-160 (dead-`let` covers only the *dead* `let` case), :281
(phase-3 exit names no `run`-on-`Code`-param test).

**What the spec shows:** §5.4 :924-934 — a fragment with `Code`-typed
parameters is splice-only: "refused by `run`/`graft` (it has no value-level
parameter to receive) **and refused in evaluated positions** — never a
`let`/`var` initializer or call argument"; consumed literals emit "no head
and no descriptor". A *live* `let x := <splice-only>` isn't dead-`let`
territory; the evaluated-position arm and the `run`/`graft` arm are named
nowhere in phases 2–3 (the statically-known judgment governs `splice`'s
operand, not the literal's position). One parenthetical per bullet.

### m4 — `static` inside emitted-fragment bodies has no owner — MINOR

**Plan ref:** :165-168 (checker arms), :183-186 (kernel eval), :210-223
(`compileFragment` silent on `Static`).

**What the spec shows:** :968-972 — a `static` inside a *parameterized*
fragment is "judged in the context the parameter lands in" (e.g.
`static(splice f …)` resolved at the site), and `static(param)` inside a
fragment *emitted* for `run`/`graft` "errors at emission, the operand being
unvalued there". Neither the site-deferral nor the emission-error arm is
named; presumably the body's `Static` nodes ride phase 2's eval machinery
inside `compileFragment` — say so, plus the emission-time refusal.

### m5 — descriptor parameter-rank edge vocabulary still unnamed — MINOR

**Plan ref:** :210-215.

**What the spec shows:** :914-917 — the parameter cells chain `+d.step` off
the descriptor's `+d.grab` in declaration order, are the same cells
`+d.vars` entries name, each `+d.name`-labelled and `+d.values`-typed. `run`
name-keys frame binding off the labels; `validate` walks the chain; `graft_*`
compares the ordered pairs. v3 states the logical fields and the
`result`/`produced` `+d.values` channel but not the parameter rank's
channels — the sibling `+d.refs`/`+d.recipe` schema is "stated in full"
(:215-219) while this stays at field level. One clause.

### m6 — `static`'s kernel arm omits the result half — MINOR

**Plan ref:** :183-185.

**What the spec shows:** :952-955 — after the no-mark run, the kernel
"reads the `result` cell (`CellValue`) — which must match `e`'s checked type
— then unlinks the eval cursor … and emits the value as a `literal()` in
the enclosing graph". The bullet names run/unlink/baseline but not the
type-matched result read nor the `literal()` emission — the entire output
side of the feature, and a testable arm (result-type mismatch is its own
refusal). A few words.

## Nits

- **n1** — `Makefile:695` is `VAGENT_SYNTAX_TEST_OBJS`; `VAGENT_SYNTAX_OBJS`
  is :696 (:109). Similarly `checker.cpp:765-779` — the `std:agent` member
  rows open at :770; :765-769 is the `builtin` lambda tail (:232).
- **n2** — "its refusal edges:" (:155) reads exhaustive but omits two
  refusal-class edges: opaque `Code` actuals ("cannot expand `f`", spec
  :464-466 — arguably under the statically-known judgment) and `splice c :=
  3` (cap$N is not re-assignable, spec :470).
- **n3** — "frame-bound under §5.8 (spec :847-854)" (:266-267): the cite
  lands in *this* doc's §5.3; §5.8 is the language spec's — the two-`§`
  reading muddles which document.
- **n4** — `run` "spill" (:263) vs spec's "spills and restores … or fails
  when `produced` is false" (:262-263) — the restore/failure half unnamed.
- **n5** — the `programRef` record "written at emission beside
  `compileFragment`'s marking" (:226) can read per-fragment; it is the
  program-image record (one per program, at image finalization).

## Pipeline feasibility answer

Yes — the staging pipeline remains implementable phase-by-phase. No
inversions found: every new mechanism statement lands in the phase that can
honor it (four `static` arms checker-side with per-arm exit tests; M13
edges beside the expansion that produces them; the frame-bound classes
correctly deferred to phase 3 where the per-activation ABI first exists —
the interim analysis in m2 is a stated-interim gap, not an inversion; the
(e) dim arm and the re-genesis loop close the r2 majors as ruled). `closed`
flows phase-2 tables → phase-3 descriptor → phase-4 re-verification with
each producer before its consumers. The single major is an omitted work
item in an otherwise correctly-sequenced phase, not a dependency defect.

## Top 3 for the parent

1. **M1** — phase 2's "splice substitution via `cloneExpr`" omits §5.4's
   call-by-value marshalling (fresh `let` locals, once-per-arg, in order —
   the design's *only* synthesized `let`); a textual-substitution misread is
   an observable semantic bug. The §9.1 exit would catch it, but the work
   item itself is absent from the bullet.
2. **m1** — phase 1 never lists the `splice`/`static` prefix *productions*;
   without them the phase's own refusal tests exercise parse errors, not
   the named expand `DiagnosticCode`.
3. **m3/m5** — two phase-3 schema surfaces still lighter than their "stated
   in full" sibling: the splice-only class's `run`/`graft` +
   evaluated-position refusal arms, and the descriptor parameter rank's
   `+d.step`/`+d.name`/`+d.values` channel names.

# r1 — metaprogramming / staging-semantics review

**Verdict: NOT CONVERGED**

Counts: **2 BLOCKING · 6 MAJOR · 4 MINOR · 3 NIT**

Lens: does the build order respect the parse → resolve → expand → check →
kernel staging of §5.6? Findings are ordered producer/consumer pairs across
phase boundaries: what each stage *emits* vs. what a later item *consumes*,
and whether any consumer is scheduled before its producer.

## Direct answers to the staging questions

1. **Can `code"…"` be checked pre-expand?** Yes — but only if somebody parses
   the body. Per §5.6 the parser emits `Expr{kind:Code, text:body}`; the
   body is *raw text* until `parseCodeBody` runs, and the plan assigns
   body-parse to phase-2's `expand`. Under the plan's own allocation the
   phase-1 checker sees only the raw literal — it cannot compute `T`, purity,
   or arity (B1). A skeleton `expand` (body-parse + seeded resolve + table
   emission, all splices uniformly residual-refused) in phase 1 is the clean
   fix.
2. **Does phase-3 `run` need the splice-expanded merged AST?** Structurally
   no — `run` consumes `Code` *values*; the merged tree only needs to be
   splice-free, which phase-2's residual check guarantees. The real
   dependency is sideways: `run`, `value_of` and all five resolvers are
   `std:meta`-gated names, and the import allowlist + signature-table rows
   are scheduled in phase 4 (B2).
3. **Hygiene `cap$N` ordering:** all `cap$N` *consumers* sit correctly at
   phase 2+ (stamp-honoring check, `CompilerBindings` emission, `$`-anchor
   rewrite, `run`/closed walks). One producer/consumer inversion: the
   scope-stamp *field* is a phase-1 item while phase-0's resolve stamps it
   and `cloneExpr` preserves it (M3).
4. **Citation check:** every load-bearing cite verified —
   `parser.cpp:848-855` (`!= "cells"` gate, exact), `checker.cpp:555-562`
   (`validateType` scalars + arity arm), `:30-36` (`snake` accepts `$`),
   `:827` (`pureBody`), `:2061-2067` (import allowlist), `lexer.cpp:333-335`,
   `:14-24`, `:379-398`, `compiler.cpp:104-110`, `:160-181` (`binding()`
   render check + `externalReferences_`), `:407-417` (`validName`), `:475-483`
   (anchor bindings), `:500-502`, `:705-707`, `:853-855` (`localCells_`),
   `:884-891` (`const Expr *`-keyed tables), `checker.hpp:23-31`,
   `compiler.hpp:23-26` (no `staticFuel` — confirmed absent), `Makefile:688-690`,
   `vortex_core.cpp:196-254`, `vortex_vm.cpp:854-893`, `:736-743`,
   `:1220-1225`, `:98-100`, `worker_context.hpp:52-54`,
   `mesh_actor.cpp:119-122`,`:156`,`:182`,`:188`,`:261`,`:271-273`. All good
   as starting coordinates.

---

## Findings

### B1 — Phase 1's checker work and exit presuppose a body producer the phase does not deliver — BLOCKING

**Plan ref:** lines 48–65 (phase 1 items + exit) and 73–76 (phase 2 `expand`
bullet).

**What the plan claims:** phase 1 makes "fragments parse, check, and carry
types"; the checker gains "fragment table keyed by `SourceLocation`; purity
flags; per-fragment arity table"; exit is "`vagent check` accepts/rejects
`code"…"` programs per §5/§7.1 … diagnostics map to inner-literal offsets."

**What the code/spec shows:** per §5.6 the parser produces
`Expr{kind:Code, text:body}` and "the `code` body's own parse happens in
`expand` through `parseCodeBody`"; `expand` "emits the merged `Program` —
plus the fragment table" and "`check` gains the table as a parameter — a
signature change" (`checker.hpp:30` today is
`check(const Program&, CheckLimits)`; `Checker::run` at
`checker.cpp:2058`). The plan itself puts "code-body parse" under phase 2's
`expand` bullet. So in phase 1, per the plan's own words, **no stage ever
parses a body**: the checker sees only the raw literal `text`. It cannot
compute `T` ("the fragment body's checked result type, computed at the
literal's position", §5.2), cannot evaluate body purity/arity (all three are
functions of the body), cannot produce an inner-offset diagnostic (nothing
parsed to offset into), and the fragment table has no producer — yet phase
2's `expand` bullet doesn't list "emit the fragment table" either, so the
table's producer is absent from *every* phase. §7.1's first guarantee ("a
`code"…"` body is fully type-checked in its definition-site environment")
is unreachable, so the exit line is untestable by construction.

Also unlisted: `apps/vagent/main.cpp:46-53` calls `parser.parseProgram()` +
`check()` inline. Whoever produces bodies, `vagent check` must be rewired
through `frontend`'s driver for the pipeline to run — no phase names that
edit.

**Suggested fix:** repartition phase 1 to land a **skeleton `expand`**:
(i) `parseCodeBody` invocation per `Code` literal (the method still lives in
`parser.cpp`), (ii) per-literal resolve seeding, (iii) fragment-table
emission — and nothing else; every `Splice`/`Static` node is uniformly
refused in phase 1 (the residual-splice nuance arrives with substitution in
phase 2). The `check(program, table)` signature then lands with its
producer in phase 1 instead of as later churn; the phase-1 checker `Code`
arm looks the record up, checks the body under its own env walk (still the
scope truth until phase-2 stamp-honoring), and computes `T`/purity/arity.
Phase 2's `expand` bullet then explicitly adds "emit the fragment table" and
the composition semantics. Alternative cheaper fix: keep the allocation but
say the checker drives `parseCodeBody` itself in phase 1 and price the
phase-2 migration when `expand` takes over — but then phase 2's "code-body
parse" bullet is a no-op to delete. Either way, add the `main.cpp` →
`frontend` rewiring to the phase that needs it.

### B2 — Phase-3's callable surface depends on the `std:meta` surface scheduled in phase 4 — BLOCKING

**Plan ref:** lines 99–108 (phase 3 `run`/`value_of`/resolvers + exit) vs.
lines 123–124 (phase 4: "checker import-allowlist + builtin member-signature
table entries").

**What the plan claims:** phase 3 delivers `run(c, [args])` special form +
`value_of` in the checker, the five resolvers (`label_of`, `clauses_of`,
`code_named`, `plan_named`, `symbol_named`), and `reflect`/`reify`; exit is
"`run(code"…")` executes … resolver tests incl. qualified-name and
headless-`run` refusal."

**What the code/spec shows:** every one of those names is a `std:meta`
member or the `run` special form (§6.1's member table, spec lines
1152–1176). `std:meta` is *not* implicitly imported — the checker allowlist
at `checker.cpp:2061-2067` today accepts only `std:agent`/`std:goal`, so
`import std:meta` is a hard refusal until the allowlist entry lands.
§5.1 scopes `run`/`value_of`'s reserved status to "when `std:meta` is
imported" — a user `def run` stays legal unimported, which means the `run`
special form cannot be unconditionally bound without deviating from the
spec. So under the plan's literal allocation, phase-3 test programs cannot
write `run(...)`, `value_of(...)`, or any resolver call: the allowlist entry
and the signature-table rows are phase-4 items, a strict producer-after-
consumer inversion of phase-3's exit.

**Suggested fix:** split the `std:meta` surface by consumer, not by phase:
the `import std:meta` allowlist entry plus the read-side signature rows
(`run` form gating, `value_of`, the five resolvers, `reflect`/`reify`,
`validate` if phase-3 `run` wants it) move to phase 3; the mutation-verb
rows (`graft_*`/`prune`/`learn_*`/`unlearn`/`set_weight`/`sprout_synapse`)
stay in phase 4. If instead `run` is meant to be an always-on special form
with user-def shadowing allowed pre-import (a defensible but different read
of §5.1), say so explicitly — and the resolver rows still have to move.

### M3 — Scope-stamp field is produced in phase 1 but consumed by phase-0 machinery; the seeding channel is underspecified — MAJOR

**Plan ref:** line 35–38 (phase 0: resolve "stamping `Identifier` binding
identities on `Expr`"; "`cloneExpr`/`cloneDecl` preserve the stamp") vs.
line 56 (phase 1: "`ast.hpp`: … + the scope-stamp field").

**What the code shows:** `Expr` (`ast.hpp:97-107`) has no stamp field today —
it is created by the phase-1 bullet. Phase-0's resolve pass cannot "stamp …
on `Expr`" against a field that doesn't exist, and `cloneExpr` cannot
"preserve" it. A literal producer/consumer inversion within the frontend
slice. Trivially fixed, but it's exactly the stamp-ordering hazard this lens
exists to catch — and if phase-0's resolve ships stamping nothing, phase-2's
`expand` (which consumes stamps for `cap$N` and scope walks) silently starts
on empty input.

Additionally, the seeding channel is under-specified in both spec and plan:
§5.6 has `expand` "invoke the same helper per `code"…"` body subtree, seeded
with the *literal's scope*" — but the stamp is described as "`Identifier`
binding identity — scope id plus binding ordinal". A `Code` literal is not
an `Identifier`; its enclosing scope must ride the stamp field on the Code
node itself (or on every `Expr`), or `expand` has no scope-id to seed with.
Say explicitly: the stamp carries scope-id on all `Expr` nodes, plus
binding-ordinal on `Identifier`s — `cloneExpr` preserves both.

**Suggested fix:** move the `Expr` stamp field (and any
`cloneExpr`/`cloneDecl` preservation code for it) into phase 0; keep only
the `ExprKind::{Code,Splice,Static}` additions in phase 1; state the
scope-id-on-every-`Expr` rule.

### M4 — `splice`/`static`/`run`/`value_of` name-reservation refusals are absent from every phase — MAJOR

**Plan ref:** parser changes land at lines 53–55 (phase 1); no phase lists
the §5.1 refusals.

**What the spec shows:** §5.1 — "`splice`/`static` are prefix forms wherever
an operand can follow … a user `def`/`method` named `splice` or `static` is
unreachable as a bare call — the checker rejects such declarations … the
refusal extends to `let`/`var`/parameter bindings named `splice`/`static`,
and to `run`/`value_of` declarations and bindings when `std:meta` is
imported." The moment phase-1's parser accepts `splice x`/`static x` as
prefix forms, a pre-existing program containing `def splice` or
`let static` silently changes meaning (the call parses as the form). The
checker refusal is the compatibility guard — it must land in the same phase
as the parser change (phase 1 for `splice`/`static`; phase 4 for
`run`/`value_of`, gated on the import per §5.1). It appears in no phase's
item list.

**Suggested fix:** add "reserved-form declaration/binding refusal
(`splice`/`static`)" to phase 1's checker bullet and
"`run`/`value_of`-under-`std:meta` declaration/binding refusal" to phase 4's
checker bullet.

### M5 — Three §10.1-mandated work items are dropped entirely: `staticFuel`, dead-let erasure, and the `closed` flag — MAJOR

**Plan ref:** phase 2 (lines 73–86) and phase 1 (lines 59–62).

1. **`staticFuel`.** §10.1 (spec lines 3070–3072): "`static` needs a
   `staticFuel` entry in `ScalarCompilerLimits` (`compiler.hpp:23-26`) plus
   the two expansion budgets in `CheckLimits`." The plan threads the
   `CheckLimits` budgets in phase 1 but never adds `staticFuel` — yet
   phase-2's `static` evaluation is "no-mark vm run" whose signature is
   `vm_.run(cursor, staticFuel)` (§5.5). `ScalarCompilerLimits` today has
   only `maxNodes`/`maxDepth` (`compiler.hpp:23-26`). Phase 2's static eval
   has no fuel source as listed.
2. **Dead-`let` erasure.** §5.4: "a `let` binding left dead by full
   substitution is erased by `expand`, or refused by the checker where
   erasure is unsafe" — mandatory for splice-only literals ("emits no head
   and no descriptor"), and needed in practice by §9.1/§9.4: `let inc :=
   code"…"`/`let policy := code"…"` become dead after their only use
   (`splice`) is substituted. If the residue is not erased, the merged tree
   still contains a `Code` literal whose lowering is phase-3 emission —
   phase-2's "merged-AST lowering" can't compile the worked examples
   cleanly (the scalar preflight would refuse it, `compiler.cpp:500-502`).
   Erasure is nowhere in phase 2's `expand` list.
3. **`closed` flag computation.** §5.6's checker computes "per-fragment
   arity/purity/closed tables"; §6.1's descriptor record carries
   `["closed"] +d.values → true/false` (spec line 1074); §7.2's runtime
   checks read it; `reify` preserves it ("closed iff the source was");
   `learn` re-verifies against it. The plan emits the descriptor in phase 3
   (line 96–97, including "`closed` flag's provenance" marker bit) and
   re-verifies closedness in phase 4, but **no phase computes the flag** —
   phase 1 lists "purity flags" only, phase 2's "static closedness" is the
   unrelated `static`-form rule. The computation needs the capture set,
   i.e. `expand`'s `cap$N` analysis, so it belongs with phase-2's checker
   items (or phase-1's if B1's skeleton-expand lands early with a
   free-name/conservative variant — say which).

**Suggested fix:** add all three to the named phases (1: `staticFuel` field;
2: erasure + `closed` flag computation).

### M6 — `learn_*`'s session-side patch machinery is listed under phase 5, but phase 4 consumes it — MAJOR

**Plan ref:** lines 128–129 (phase 4 `learn_*`/`unlearn` "at session scope")
vs. lines 139–149 (phase 5 `patch:<n>` serials, `d.code` separation).

**What the spec shows:** `learn_*` writes its durability records **at learn
time, in-session** — §6.6 (spec lines 1854–1855): "append a patch cell to
the agent's `d.patch` rank — and mint the installed clause's `+d.name`
label itself as `patch:<n>`"; §6.1 (spec line 1174): "graft one fresh copy,
register a second fresh `reify` on `d.code`, record `d.patch`". `unlearn`
"undoes the latest patch" — it *reads* `d.patch` records (spec line 1937:
"the latest patch whose recorded target name equals it") — it cannot exist
without them. The plan's phase-5 items describe the *durability semantics*
(monotone serial across genesis, no-ABA on resume, replay dedupe, `d.code`
*separation* for the closed taxonomy) but read as if the structures are
born there. As written, phase 4's `learn_*` mints no `patch:<n>`, appends no
`d.patch`, registers nothing on `d.code` — and `unlearn` has nothing to
undo.

**Suggested fix:** repartition explicitly — session-side record machinery
(`patch:<n>` mint off the `d.vars` counter, `d.patch` append, `d.code`
registration, alias recording) → phase 4 with `learn_*`/`unlearn`;
durability properties (monotonicity across genesis, non-recycling/no-ABA,
verb-replayed dedupe, `(e)`/`(f)` rebind/replay split, `d.code` taxonomy)
→ phase 5.

### M7 — `code_path.cpp` and `expand.cpp` are never added to `VAGENT_FRONTEND_SRCS` — MAJOR

**Plan ref:** line 43–44 covers only phase-0's three files.

**What the code shows:** `VAGENT_FRONTEND_SRCS` (`Makefile:688-690`) is an
explicit `addprefix` list — not a glob — and phase 1's `code_path.cpp` and
phase 2's `expand.cpp` appear in no Makefile item. The plan itself cites
"the `cell_path.cpp` omission bug is precedent for forgetting" and then
repeats the omission twice. Any `checker.cpp`→`code_path` or
`frontend`→`expand` reference links out.

**Suggested fix:** one Makefile bullet per phase adding that phase's new
`.cpp` (phase 1: `code_path.cpp`; phase 2: `expand.cpp`; plus `expand.hpp`-
side if headers are listed anywhere).

### M8 — `run`'s per-activation frame binding is owed ABI whose status the plan doesn't assess — MAJOR

**Plan ref:** lines 99–100 (phase 3 `run`).

**What the code/spec shows:** §6.2 binds args "on a fresh per-activation
`#Call` frame *by parameter name, as a value*" and §6.2/§5.3 put fragment
`let` locals on "the §5.8 frame discipline" — but the current scalar kernel
mints `localCells_` **statically per compile** (`compiler.cpp:853-855`),
which the spec itself calls "the interim no-call shape". §10.1 records
"the `run`/`graft_*` verbs need the call ABI (`#Call` + frame + spill + the
`d.entry` head resolution) — owed by vagent §11.1 itself, not new debt
here." The VM-side `#Call`/`Return` frame mechanism exists
(`vortex_vm.cpp:854-893`); the *compiler-side* name-keyed spill and
per-activation binding cells do not. If the §11.1 debt is still owed, a
parameterized `run(c, [args])` cannot bind its args in phase 3 — only a
nullary `run(code"…")` would execute.

**Suggested fix:** state the call-ABI's delivery status explicitly. If it is
not yet delivered, either scope phase-3's exit to a nullary
`run(code"…")` + descriptor-arity refusal test, or add the frame-binding
emission to phase 3's item list (or a named pre-phase). Note fragment
`let` locals' per-activation rule (the §5.3 ABI-debt class) rides the same
machinery.

### m9 — Phase-2 exit's verification level is ambiguous given what lowering covers today — MINOR

**Plan ref:** lines 84–86. `vagent`'s only command is `check`
(`apps/vagent/main.cpp:14-22`); `compileStatement` is statement-level and
test-invoked only (no whole-program/artifact path exists), and §9.4's
`emit`/plan bodies are outside today's scalar subset. "§9.1 and §9.4 expand
correctly" can only mean expand-output AST assertions + `vagent check`
green — say so, and that their *execution* awaits phase 3+. Similarly scope
"hygiene tests (§5.3)": merged-code `cap$N` resolution is testable in
phase 2; the capture-liveness/dead-frame rules need phase-3 `run` +
capture-slots.

### m10 — `check()` is a public API: stamp-honoring needs an un-stamped-input rule — MINOR

**Plan ref:** line 77–78. `check(const Program&, CheckLimits)` is exported
(`checker.hpp:30-31`); callers (tests, programmatic `Program` builders) may
skip resolve. Phase 2's "stamp-honoring (resolve owns name truth)" must say
whether un-stamped `Identifier`s/`cap$N` fall back to the env walk or are
refused — silently env-walking stamped names would let a stale stamp and
env disagree unchecked.

### m11 — Interim refusal semantics for the new `ExprKind`s are unstated — MINOR

**Plan ref:** phases 1–2. In phase 1, `Splice`/`Static`/`Code` reaching
`compileStatement` hit preflight's clean "construct is not supported"
refusal (`compiler.cpp:500-502`) — acceptable, but say so (it is the interim
contract, since `vagent check` accepting a `code"…"` program must not imply
compilability). Related: bodies containing `run(`/resolver calls are legal
per §5.5/§6.1 but uncheckable until the member surface exists (phase 3–4) —
a phase-2 body check must refuse them as unknown names; note the
limitation so it isn't read as a regression.

### m12 — Capture-slot wing emission and the `+d.refs` completeness split should be named — MINOR

**Plan ref:** lines 94–104. §5.3's capture-slot wrap (operand emission
wraps the binding cell on the operand's wing; decode via `masterOf(slot)`
on `asInt64` failure, the `-d.clone` backlink read at
`vortex_core.cpp:358-365`) is what makes captured fragments decodable by
`wingOf` and discoverable by the `closed` walk — distinct from phase-2's
`CompilerBindings` *lookup* entry, and load-bearing for `run`-on-closure and
`learn`. Name it in phase 3's `fragment.cpp` bullet. Also clarify which
`+d.refs`/`+d.recipe` records are complete at phase 3 (`reify` needs the
full recipe) vs. augmented in phase 6 (`d.mesh.relocation` markers), so
phase-6's "emission marking" item doesn't duplicate or contradict phase 3's.

### n13 — Spec version cite is stale — NIT

**Plan ref:** lines 3, 217. Plan says "v1.18, converged at review round 18"
and "verified at v1.18"; the spec is **v1.19** (spec line 5; the 1.19 entry
is approvals-only, no mechanism changes — P15/P16 approved, which the plan
already reflects). Update the label and the cite-verification baseline.

### n14 — `cloneExpr` field enumeration omits non-`ExprPtr` fields — NIT

**Plan ref:** line 38–40. `Expr` also carries `text`, `names`,
`annotation`, `binding`, `loc` (`ast.hpp:98-107`) — `loc` in particular is
required by §5.6's "+offset" diagnostics surviving substitution, and
`binding`/`names` carry `for`/`as` and record names. Enumerate them
explicitly; the checklist is the point.

### n15 — `Code` should join `builtinTypes` beside the scalars entry — NIT

**Plan ref:** line 59–60. `checker.cpp:2068-2071`'s `builtinTypes` set
shadows built-in type names against user declarations; `Code` gains scalar
status in `validateType` but should also be shadow-protected, or a user
`record Code` passes the scalars lookup inconsistently.

---

## Summary of the pattern

Every BLOCKING/MAJOR here is the same shape: the plan sequences *files* in
the right order but allocates *dataflow products* to the wrong side of the
stage boundaries they cross — the parsed body and fragment table (expand's
products, consumed by check), the stamp field (ast.hpp's product, consumed
by resolve), the `std:meta` member surface (a table's product, consumed by
phase-3's exit), and the session patch records (learn-time writes, consumed
by `unlearn` and genesis). Phase order in the map is right; the item-level
assignment inside the phases needs one repartition pass: skeleton-expand
(or a named interim driver) into phase 1, the read-side `std:meta` surface
into phase 3, the stamp field into phase 0, and session patch machinery
into phase 4.

# r2 — metaprogramming / staging-semantics review

**Verdict: CONVERGED** (all r1 blockers and majors closed; residual is
specification-sharpening, no remaining inversions)

Counts: **0 BLOCKING · 0 MAJOR · 6 MINOR · 3 NIT**

## r1 closure verification

| r1 | Claim | v2 status |
| --- | --- | --- |
| B1 | no body producer; table unowned; `main.cpp` stale driver | **Closed.** Phase 1 lands the skeleton `expand` (per-literal `parseCodeBody`, seeded resolve, **fragment-table emission** keyed `SourceLocation`/ordinal, :107-115), `check(program, table)` lands *with its producer* (:118), `main.cpp:46-53` rewiring is a named phase-1 item (:125-127). |
| B2 | `std:meta` surface after its consumers | **Closed.** Phase 3 gets allowlist entry (`checker.cpp:2061-2067` — verified accepts only `std:agent`/`std:goal`), read-group signature rows (`:765-779` — verified the `std:agent` member table), prelude skeleton, all 14 pure members + `run` + `reify` (:203-215); phase 4 gets all 11 mutation verbs (:262-267). The read/mutation split matches §6.1's purity table exactly — every member-table row is now owned. |
| M3 | stamp field phase-1, consumed phase-0 | **Closed** per disposition's minor-bundle ruling — the ordering note (:86-89) states the field is additive and lands early with its consumers; the seeding channel is now specified: "scope-id on every `Expr`, plus binding-ordinal on `Identifier`s" (:71-73). |
| M4 | reserved-name refusals absent | **Closed.** `splice`/`static` decl/binding refusals ride the parser change in phase 1 (:122-124); `run`/`value_of`-under-import + `src:`/`reify:`/`patch:` module-declaration refusals in phase 3 (:228-231; §6.7 :2500-2504 cite verified). |
| M5 | `staticFuel`, dead-`let`, `closed` dropped | **Closed.** All three land: `staticFuel`→`ScalarCompilerLimits` (:167-169, `compiler.hpp:23-26` verified field-less today); dead-`let` erasure (:148-150); `closed` computed in phase-2 check off expand's `cap$N` analysis (:158-160) — see F3 for a precision note. |
| M6 | `learn_*` session/durable split | **Closed.** Session machinery (two `reify`s, `d.code` registration, `d.patch` append, `patch:<n>` mint, `unlearn` read side) explicitly in phase 4 (:275-284); durable properties (monotone serial, no-ABA, two-copy invariant) in phase 5 (:351-354). |
| M7 | `code_path.cpp`/`expand.cpp` not wired | **Closed.** Both join `VAGENT_FRONTEND_SRCS` in phase 1 (:105-106, :113-115); `expand.cpp`'s edit lands with the skeleton's creation, correctly. |
| M8 | call-ABI status unassessed | **Closed.** Preconditions section (:26-30) states the compiler-side debt; phase 3 prices a minimal convention (`Call` + `d.vars`/`d.values` name-keyed binding + `result`/`produced` spill + `+d.entry` hop, :224-227) and the exit scopes to "nullary plus the convention's arg coverage" (:240). |
| m9-m12, n13-n15 | — | **Closed.** Phase-2 exit names ScalarHarness vs expand+check-only halves (:175-180); un-stamped-input rule required (:154-157, see F2); preflight/unknown-name interim contracts stated (:111-113, :160-162); capture-slot wing emission + `+d.refs` schema as emission contract (:188-196); v1.19 labels fixed (:3, :448); `ast_clone` field enumeration complete (:74-77); `Code` joins `builtinTypes` (`:2069` verified, set opens :2068). |

Spot-checked new v2 cites, all clean: `Makefile:649/692/698/1147/1348-1350`
(test-home claims exact — `vagent` links `VAGENT_OBJS+VAGENT_FRONTEND_OBJS`
only, no kernel; `vagent_runtime_test`'s `filter-out` covers all of
`apps/common/xanadu/vagent/`), `arena_manifold.cpp:34-46/78-105/289-290`
(`base_∪arenaDims_` union, untrailed map write, attach-time `d.stores`/
`d.store-refs`), `dimension_registry.cpp:194-217` (`rebuildManifold` fold is
the injected base), `publication.cpp:1093-1103/:1129-1131` (`Store()` no-id
mint, `setContentSource`+`bindPublishedLocalScroll` pair), `store.cpp:123`,
`mesh_actor.cpp:659/:473-477`, `mesh_actor.hpp:81` (`ActorMessage`
non-optional), `vortex_core.cpp:50-69` (**17** `mintNamedDimension` calls +
`home`/`d.dims` pair = nineteen — the r1 mislabel is fixed), `vortex_vm.cpp:50-67`,
`compiler.cpp:769-772`, `mesh_evaluator.cpp:735`, ScalarHarness
`vagent_compiler_test.cpp:18-46`.

## New / still-open findings

### F1 — The skeleton's "uniform refusal" needs its locus, scope and recursion stated — MINOR

**Plan ref:** :107-113 (phase-1 skeleton bullet).

**What it claims:** "Every `Splice`/`Static` node is uniformly refused."

**What the code/spec shows:** §5.1 makes `splice`/`static` prefix forms
"wherever an operand can follow" — so stray `Splice`/`Static` nodes exist at
top level, in contract conditions, guards, `let` initializers — *outside* any
`code"…"` literal and therefore outside the skeleton's three named
per-literal duties (body-parse, seeding, table emission). The bullet does not
say the refusal is expand-side (the phase-1 checker bullet has no Splice/Static
arm) nor that the skeleton walks the *whole program* plus every literal body
*recursively* — nested `code"…"` literals inside bodies need records of their
own, or the checker's `Code` arm inside an outer body finds no record (a body
containing a literal whose own body holds a `splice` must refuse the same
way). The failure mode if misread is benign — a stray splice reaching `check`
falls to `infer`'s default arm, a generic `InvalidDeclaration` "unknown
expression kind" (`checker.cpp:1719-1721`) — clean but wrong-code and
wrong-position diagnostics, and a missing-record lookup for nested literals.

**Answer to the round-2 question:** yes, the intermediate is coherent —
`check` sees *no* unexpanded splice; the refusal is a clean expand-side
diagnostic *provided* the skeleton walks the whole program and recurses into
bodies. That provision is implied ("every … node") but never stated; one
sentence pins it.

**Suggested fix:** add to the bullet: "the skeleton walks the merged program
*and* each literal body recursively — any `Splice`/`Static` node anywhere is a
named `DiagnosticCode` refusal at expand, so `check` never meets either kind
and needs no arm; nested literals emit records through the same recursion."

### F2 — `check()`'s public-API contract under the new pipeline: signature ripple and the *forced* un-stamped rule — MINOR

**Plan ref:** :118 (`check(program, table)`), :154-157 ("pick one and say
it").

**What the code shows:** `check` is exported (`checker.hpp:30`) and its
existing callers bypass the entire new pipeline —
`tests/xuzz/vagent_checker_test.cpp` calls `check(*program)` /
`check(*program, CheckLimits{...})` at ~10 sites (:25, :686, :692, :704, :710,
:725, :746, :750, :764 …) on parser output with **no resolve, no expand, no
table**. Two consequences the plan doesn't state: (i) the new table parameter
ripples to those call sites — either it takes a default or the test file is a
phase-1 edit beside `main.cpp`; the plan names only `main.cpp`. (ii) The
"pick one" for un-stamped input is not actually open: **refusing un-stamped
`Identifier`s breaks the entire existing checker suite**, which never runs
resolve — the only consistent choice is "un-stamped names fall back to the
env walk" (exactly §5.6's asymmetry: stamps honored where present, env walk
where absent). Same for a `Code` node with no table record — a programmatic
caller gets a named refusal, say so beside the stamp rule.

**Suggested fix:** name the fallback explicitly ("env walk — `check()`
remains callable on resolve-less input; refusing breaks every existing
caller"), name the missing-record rule, and add the test-call-site update (or
defaulted parameter) to the phase-1 item.

### F3 — The `closed` flag's input is the outside-reference classification, not the capture set — MINOR

**Plan ref:** :158-160 — "the `closed` flag is computed here off `expand`'s
`cap$N` capture set".

**What the spec shows:** §6.1's glossary (:158) — closed means *no
program-side reference that lacks name-resolution*: a `cap$N` capture is the
**state-side** arm that keeps a fragment closed; a name-resolved program ref
(the `+d.refs` name-record class) is also closed; the refused class is a
program-side ref carrying neither. A capture-set-only reading computes the
wrong flag in both directions — a fragment *with* captures can be closed, and
a fragment with *zero* captures but an unresolvable program-side ref is not.
Placement in phase 2 is correct (expand's `cap$N` env records + stamps give
check the full outside-ref set; the descriptor emission that consumes the
flag is phase 3, `learn`'s re-verification phase 4).

**Suggested fix:** reword to "computed off the per-fragment outside-reference
classification expand leaves behind — `cap$N` captures and name-resolvable
program refs count toward `closed`; a program-side ref with neither is the
refused class that clears it".

### F4 — The descriptor schema is under-stated beside the contract the plan states "in full" — MINOR

**Plan ref:** :188-190 — "descriptor (carrying phase 2's computed `closed`
flag …)".

**What the spec shows:** the descriptor is the signature-as-cells (:149) —
ordered `(name, type)` parameter cells (count = arity), result type name,
`pure` *and* `closed` flags, `effect`-ness for `graft_method` (§7.2 :2568),
and the `result`/`produced` cell records `run` derefs through `+d.values`
(:914-920, :1377-1384). `run`'s same-phase minimal call convention reads
`result`/`produced` off it; `graft_*`/`run` *compare descriptors* at runtime
(§7.2). The plan states the `+d.refs`/`+d.recipe` schema "in full" and leaves
the descriptor — the record the verbs actually read — to the spec term alone.

**Suggested fix:** one line in the phase-3 bullet enumerating the descriptor
fields (param name→type cells + result type + `pure`/`closed`/`effect`-ness +
`result`/`produced` records) beside the `+d.refs`/`+d.recipe` contract.

### F5 — `static`'s §5.5 refusal set is half-named — MINOR

**Plan ref:** :154 ("`static` closedness"), :179-180 (exit tests
"baseline-violation refusals" only).

**What the spec shows:** §5.5's refusal surface has four arms — impure
(`pureBody`, `checker.cpp:827`: no actions/assignments/`cells"…%"`), the
arena-reading *form* bar (`cells"…"` any mode, `believes`, `findall`, `run`,
implicit names `self`/`parent`/`inner`/`outer`/`occurrence`), the
literal-expressible result-type rule (`Int`/`Float`/`Str`/`Bool`/`()` — a
`Cell`/`Code` result is a stale-handle refusal), and the form-level closedness
the plan names. The kernel-side items (no-mark `vm_.run(cursor, staticFuel)`,
cursor unlink, baseline check, `literal()` emission) are covered; the
checker-side list names only closedness, and the exit tests only the
baseline arm.

**Suggested fix:** name all four checker-side refusals and add a per-arm
refusal test to the exit.

### F6 — `run`'s special-form *recognition* should say "under the import" — MINOR

**Plan ref:** :211 ("`run(c, [args])` as the checker special form"),
:228-229 (decl/binding refusal gated on `std:meta` imported).

**What the spec shows:** §5.1 — a user `def run` stays legal *unimported*,
which is coherent only if the special form is not unconditionally bound: an
unimported `run(c, [x])` call must resolve to the user's `def`, not the form.
The refusal bullet gates the *declaration* side correctly; the
form-recognition side never states its gate. One clause closes it.

## Nits

- **n7 — `VAGENT_SYNTAX_OBJS` is a third explicit list** (Makefile:695 —
  token/lexer/parser only). The per-phase Makefile items cover
  `VAGENT_FRONTEND_SRCS`; if phase-1 `parser.cpp` work references
  `code_path.cpp` (the `src:m:l:c+o:c` mapping) or `expand`, the syntax
  binary's link set needs the same edit — or keep `parseCodeBody`'s
  diagnostics offset-only and let the reporter map. Name the constraint
  either way.
- **n8 — M13's edges compressed into "Code-splice `def` expansion"** (:147):
  the `recursive-template` named `DiagnosticCode` (distinct from a
  `maxExpansionDepth` budget death), the control-flow refusal in template
  bodies (`return`/`yield`/`consume`/`terminate`), and the
  `effect`/`native`/`capacity`/`graft_method` bar are refusal-test surface
  worth one parenthetical.
- **n9 — `learn_*`'s "conditional on P16" flag is never acknowledged**
  (spec :91, :156, :374, :1174): the plan's own structure already honors it
  (session machinery phase 4, the P16-requiring ref-record restore arm
  deferred to phase 6 :387), but one clause saying so prevents a
  learn-in-4-vs-P16-in-6 false-inversion reading.

## Top 3 for the parent

1. **F1** — the phase-1/phase-2 skeleton split *is* a coherent intermediate
   (expand-side refusal means `check` never sees `Splice`/`Static`), but the
   refusal's program-wide, body-recursive scope is implied, not stated — one
   sentence pins locus + traversal completeness + nested-literal records.
2. **F2** — `check()` stays a public API over resolve-less input: the
   signature change ripples to ~10 `vagent_checker_test.cpp` call sites the
   plan doesn't list, and the "pick one" un-stamped rule is actually forced —
   only env-walk fallback keeps the existing suite green.
3. **F3/F4** — two emission-contract precision gaps feeding phase 3: `closed`
   is computed off the outside-reference classification (captures keep a
   fragment closed; they don't break it), and the descriptor schema —
   param/result/pure/effect-ness/`result`/`produced` — is what `run` and
   `graft_*` actually compare at runtime, worth stating "in full" like its
   sibling contract.

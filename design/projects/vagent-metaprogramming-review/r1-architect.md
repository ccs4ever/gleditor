# R1 — System Architect critique

Scope: architectural layering, DRY consolidation, C++-vs-Vortex division of labour, resource
lifecycle, mechanical sympathy, and configuration hygiene. Claims were verified against the tree;
all citations are to files as they stand today.

## Verdict summary

The layering is broadly right: everything new lives under `apps/common/xanadu/vagent/` (headless
engine layer, no UI deps), `std:meta` correctly goes to a Vortex-source prelude rather than C++ per
the house rule, and P15 is the only proposed core change — properly registered. The failures are in
coverage and mechanism: P15 as registered does not close the channel it exists to close; `static`'s
arena discipline collides with the document's own §4.2 hazard; splice expansion needs an AST channel
the const-AST checker does not have; and `reify` is a verb with no stated mechanism. The doc's code
citations verified cleanly throughout (compiler.hpp:23-34, compiler.cpp:96-110/189-193/512-569/
849-872, checker.cpp:553-567/827/861-865/1410-1428, cell_path.cpp:17-183, lexer.cpp:379-398,
vortex_vm.cpp:73-146/616-895/1156-1207/1209-1342, arena_manifold.hpp:594-679/722-730).

## Findings

### 1. MAJOR — P15 guards the `Value`/`Splice` op bodies but not the output-writeback paths (§6.5, §10.2)

**Problem.** The kind-desync channel P15 exists to close is wider than the registered sites. P15
checks `getOpcodeKind` on the `Value` and `Splice` *opcode bodies* and on `runPipeline`'s
`writeValue(paramCell)`. But three further paths write content into cells chosen by wing slots, and
wing slots are exactly what M9 makes mutable:

- `step()`'s generic output writeback: `core_.writeValue(outCells[i], 0, std::nullopt,
  newOutputs[i])` for every non-structural op (`vortex_vm.cpp:1288-1294`);
- the memo-hit writeback (`vortex_vm.cpp:1269-1272`);
- `runPipeline`'s `writeValue(outCells[i], ...)` (`vortex_vm.cpp:343-346`) — the row names only
  `writeValue(paramCell)` at `:337`.

An output wing is a `+d.grab` NEG slot cell holding `int64(target)` (`vortex_core.cpp:398-414`); a
`Value` write to that slot retargets it. Under M9/§6.4 rewriting wings is *legal*, so a mutated or
deliberately pathological graph can point an output wing at an op cell, and the next `step`'s
writeback rewrites the op's `int64` through a path P15 never inspects — the exact `opcodeMap_` vs.
content desync the ruling is sold on (§12: "P15 restores a single decode truth" is then false).

**Fix.** Register the check at the funnel, not the call sites: `VortexCore::writeValue` /
`VortexCore::splice` (`vortex_core.cpp:196-254`) is the one body every listed path already goes
through. Because `opcodeMap_` is VM-private (`vortex_vm.hpp:186`), give `VortexCore` an optional
kind predicate installed by the VM (or hoist the check into the VM's two writeback loops plus
`runPipeline`'s two writes — but enumerate all of them in the row, not two of five). Either way the
register must also scope the refusal to the *write arm*: a `Value` read of an op cell's `int64` is
what `validate` uses to check kinds (§7.2), so "refuse a target that `getOpcodeKind` decodes" cannot
apply unconditionally. Price the per-write lookup count accordingly (output writeback runs once per
op per step — still one hash hit each).

### 2. MAJOR — `static`'s arena discipline is unspecified and the obvious implementation trips the doc's own §4.2 hazard (§5.5, M5)

**Problem.** `static e` "runs it in the compile arena with `vm_.run(cursor, staticFuel)`" on "the
same `VortexVM` the kernel already holds". That run mints real cells into the artifact arena: op
cells via `mint()` → `vm_.mintOpcode` (`compiler.cpp:512-514`), so they enter `opcodeMap_`
(`vortex_vm.cpp:75`); temporaries and literal cells; and a cursor appended to home's `+d.cursors`
rank (`spawnCursor`, `vortex_vm.cpp:148-161`). Neither cleanup alternative is stated:

- **mark + release** — the obvious move, mirroring `bounded()` — mints op cells under a live mark
  and then releases them. §4.2 item 2 describes exactly this as the map-poisoning hazard ("the
  dense slot could be re-minted as a non-op cell that `opcodeMap_` still claims is an op") and
  asserts it "cannot happen" because ops are minted only with no marks outstanding
  (`compiler.cpp:108-110`). `static` under a mark breaks that invariant in the kernel itself.
- **no release** — the eval graph, its `opcodeMap_` entries (consistent, since the cells really are
  ops), a registered cursor on `+d.cursors`, and temporaries all persist in the compile arena.
  Whatever sealing reads from that arena then carries dead weight per `static`, and an agent-side
  cursor cell is reachable from `home` if the artifact is promoted from it.

A third option — a scratch arena/VM — contradicts "the same `VortexVM`" and is unpriced.

**Fix.** Pick one and write it down with its price: (a) no-mark evaluation + explicit unlink of the
eval cursor + a priced leak of the eval graph (smallest, honest); (b) mark+release *plus* a P16
registering `opcodeMap_` eviction for cells truncated by `release` — a real second core change, so
its absence must be justified against (a); (c) emit the static fragment without `mintOpcode` (the
doc's own `New`+`Value` route), which dodges the map entirely but needs a second emission path —
priced. State which and why.

### 3. MAJOR — `splice` expansion has no AST channel: the checker is const-in/void-out, and `Expr` cannot be copied as specified (§5.2, §5.6, M4)

**Problem.** M4 says "the checker substitutes the fragment's checked AST … the merged AST lowers
once". Two mechanical facts stand in the way, neither addressed:

- `Checker::infer(const Expr &expression, Environment &env, Context)` walks a const tree
  (`checker.cpp:1121`) and `check(const Program &program, CheckLimits)` returns
  `std::expected<void, …>` (`checker.hpp:29-30`). The checker cannot emit a merged AST: producing
  one needs either a mutable pass over `Program` (signature changes through `infer`'s recursion and
  the public entry), or a new expand stage that rewrites `Program` between parse and check.
- `Expr` children are `std::unique_ptr` (`ast.hpp:90-109`); there is no clone helper in `vagent/`
  (grep finds none). Splicing one fragment at N sites *requires* deep-copying the checked subtree —
  sharing is structurally impossible (one parent per node) and semantically wrong (kernel tables
  key on `const Expr *`: `localReferences_`, `types_`, `paths_`, `discarded_` —
  `compiler.cpp:884-891`; shared nodes would share local cells, breaking §5.3 hygiene and "N sites
  mint N graphs").

This is not a detail: the choice between a rewriting `check`, a mutable `infer`, and a separate
`expand` stage determines whether fragment bodies are checked once (definition-site) or twice, and
where partial-check results live for subtrees that must *not* be re-inferred in the splice site's
environment.

**Fix.** Specify the channel in §5.6/§10.1: either an `expand` stage (`Program → Program`, runs
after parse, before `check` — and then say how it gets each literal's definition-site environment,
since that needs a name-resolution walk before the checker has run), or a documented `cloneExpr` +
mutation convention inside `check` (and then `check` stops being `void`-returning or takes a
mutable program — say which). Add `cloneExpr` to the §10.1 file list and budget expansion copies
(see finding 12).

### 4. MAJOR — `reify` has no stated mechanism; the admitted-missing primitive is hand-waved as "compile-assisted" (§6.1, §15 U-M7)

**Problem.** `reify(c): Code<T>` is in the main package surface — a v1 verb — described only as "a
fresh closed copy of a literal-born fragment (compile-assisted)". Nothing says what the compiler
emits to assist, what a copy covers, or how wing ref-boxes are remapped. A graph copy must:

- remap every wing slot's `int64` ref and every `reference()` jump target to the copy's cells —
  needs an old→new association (a scratch pairing dim or an O(set) lookup per edge);
- decide a dimension schema — ops may carry `+d.contract` clause ranks (§6.4 explicitly allows
  `requires`/`ensures` on grafted methods), `d.cache` pins, and param pipelines; a fixed-schema copy
  silently drops them;
- face the primitive U-M7 itself defers: per-cell dimension enumeration is C++-only
  (`dimensionsOf`, `arena_manifold.hpp` CellGraph concept) — claimed unnecessary here because
  literal-born fragments have kernel-known shape, but that claim is asserted, not argued.

An implementer can legitimately read this as "write a Vortex fixed-schema copy", "re-emit the
fragment from a stored recipe", or "defer the verb" — three different builds.

**Fix.** Spell out the mechanism in §6.1 or §10.1: either the kernel emits a per-fragment copy
recipe (a second descriptor rank listing copy-relevant dims — the honest version of
"compile-assisted"), or `reify` is a fixed-schema Vortex copier over `grab`/`step`/`spin`/`values`/
`vars`/`name`/`contract` with a stated rule for dims outside that set (refuse or strip-and-flag in
the copy's descriptor). State what happens to contract clauses and pipelines; if the answer is
"stripped", price the silently weaker copy.

### 5. MINOR — `import std:meta` needs the import allowlist and builtin signatures, neither in §10.1's file plan (§6.1, §10.1)

`Checker::run` rejects every import except `std:agent` and `std:goal` (`checker.cpp:2061-2067`), and
builtin member signatures are a hardcoded table under `*qualifier == "std:agent"`
(`checker.cpp:765-779`). `import std:meta` is a check error as the code stands, and `run`, the
`graft_*` family, `Opt<Cell>` readers etc. need entries in that table — beyond the "checker-special
like `text(x)`" the table already concedes for `run`. Add both to the §10.1 file list ("import
allowlist; std:meta member signatures").

### 6. MINOR — `validate`'s opcode ceiling and impure-op set are magic numbers that must not live in prelude source (§7.2)

The check "every op cell's `int64` decodes within `[0, Linked]`" is "checkable in Vortex ops" only by
comparing against the integer value of `OpcodeKind::Linked` (currently 47, `vortex_vm.hpp:27-93`),
and the `pure`-flag scan needs a table of impure kinds — both duplicating the enum in Vortex source
where a later appended opcode silently stales it (the enum's tail comment exists precisely because
ints are serialized data). Bind the ceiling and the impure-kind set as cells through the adapter
(the same V10 `ensureDimension`-style binding the doc already uses for `d.code`/`d.patch`/
`d.weight`/`d.synapse`), or generate the prelude constants from `OpcodeKind` at build time. Same
point, smaller: `next_rand` is named "SplitMix-style" but no `Xor`/shift opcode exists
(`vortex_vm.hpp:27-93`) — name the actual multiply/add recurrence or register the gap.

### 7. MINOR — M2's price overstates owed work; the CodeParams parse site is unnamed (§3 M2, §5.1)

"the parser needs an expression entry point (it parses units today)" is wrong:
`Parser::parseExpression()` already exists and consumes the whole source (`parser.hpp:25`,
`parser.cpp:271-281`). The genuinely new work is narrower: accepting `code` beside `cells` in the
`PrefixedStringLiteral` case (`parser.cpp:848-854`), plus the `#label`/`CodeParams '|'` header. To
keep the "no shadow grammar" ruling load-bearing, say where the header is parsed — a
`parseCodeBody` entry on the real `Parser`, not a hand-rolled scanner in `code_path.cpp`. Also the
EBNF `'code' StrLit` depicts two tokens; the lexer produces one `PrefixedStringLiteral`
(`lexer.cpp:386-393`) — write it as such or note it is metagrammar.

### 8. MINOR — new vagent files hit the Makefile's explicit source lists, which are already stale (§10.1)

`COMMON_XANADU_SRCS` globs (`Makefile:643`), so new files reach the library — but
`VAGENT_FRONTEND_SRCS` (`Makefile:688`) and `VAGENT_SYNTAX_OBJS` (`Makefile:693`) are explicit lists
that drive `vagent`, `vagent_frontend_test`, `vagent_syntax_test` and `fuzz_vagent_frontend`. They
already omit `cell_path.cpp`: `build/apps/common/xanadu/vagent/checker.o` has an undefined
`parseCellsPath` reference (`nm -C`), so the frontend-test/fuzz binaries cannot link today. Either
`code_path.cpp`/`fragment.cpp`/`code_path.o` need adding to those lists (a Makefile edit the doc
should own — P11 territory), or the lists should be switched to the same `find` glob. Say which in
§10.1; flag the pre-existing `cell_path.cpp` omission as needing repair either way.

### 9. MINOR — reflect verbs mint wrapper cells per call with no pricing or reuse rule (§6.1)

`test_of`/`body_of` "wrap raw clause graphs as `Code<Cell>`" — a `Code` requires a head cell with
descriptor (~6-10 cells per §5.2's pricing). On a *pure read* called per-turn-per-clause that is an
unpriced allocation stream; repeated reflection inside handlers accumulates dead wrapper cells
between checkpoints. Specify either a canonical shared "unknown-signature" head the package reuses,
or memoize wrappers per clause cell on `d.code`. One sentence in §13's cost list is enough.

### 10. MINOR — `d.patch` replay cost grows monotonically; dead patches are never reaped (§6.6, §13)

Genesis "walks `d.patch` in ordinal order and re-runs each graft"; a patch whose target no longer
resolves "is skipped with a `ConditionFault`-style record" but stays on the rank, so it is
re-skipped at every subsequent genesis — replay is O(all patches ever recorded), not O(live
patches). `unlearn` removes only patches whose target still resolves to an unlearnable clause.
State a reaping rule (genesis drops unresolved patches after recording the fault, or a periodic
`unlearn`-style sweep) — a compaction, not a ceiling, so the house rule holds.

### 11. MINOR — `splice` name collides with `OpcodeKind::Splice` (§5.1)

The unquote `splice c` and the content-restate opcode `Splice` (`vortex_vm.hpp:53`, used by P15) are
different things spelled the same; P15's row reads "`Value` and `Splice` … refuse a target" while
§5 talks about `splice` expansion. One line in §2 distinguishing `splice` (unquote, checked away)
from `#Splice` (opcode, survives to runtime) prevents real confusion in the P15 discussion.

### 12. MINOR — the expansion budgets are gestures, not named settings (§3 M4, §5.5)

`splice count and expansion depth are budgeted like maxNodes/maxDepth` and `staticFuel` is named —
but nested `code"…"` bodies each spawn a fresh `Parser`/`check` with fresh per-instance limits
(`ParseLimits` `parser.hpp:14-17`, `CheckLimits` `checker.hpp:12-27`), so nesting depth and
cumulative expansion count are *not* bounded by any existing knob; each level also runs its own
check and possibly `static` eval. Name the new fields (e.g. `CheckLimits::maxExpansionDepth`,
`maxSplices`) and state that the counter is threaded through the recursive parse, not per-parser.

### 13. MINOR — `validate`'s cost model misstates what bounds it (§7.2)

"costs O(fragment cells + links) in `consumeWork` budget" — `consumeWork` is a C++ counter charged
inside rank walks (`Linked`'s loop, `New`'s append walk, `vortex_vm.cpp:686/703/747`); a
Vortex-coded `validate` pays per executed op under the turn's `maxCycles`, and its cycle guard
("a seen-set like `Linked`'s") must be cells — a rank membership test per visited op, O(n·membership)
unless the walk is restricted to the linear `+d.spin`/`+d.step` chains where a step counter
suffices. Restate the bound honestly; if the seen-set is needed for `Jump`/`Call` target graphs,
say what structure bounds the membership test.

### 14. MINOR — the `closed` flag needs a provenance channel the kernel does not have (§5.3, §6.1, M11)

"the flag is set at emission" — but `CompilerBindings` maps a name to `{cell, type}`
(`compiler.hpp:15-19`); nothing distinguishes a state-side binding cell (checkpointed) from a
program-side one (not). `closed` is defined as "outside references are all state-side", so the
kernel needs the adapter to mark which bound cells are state cells — a new bit on `CompilerBinding`
or a marked-binding set, one sentence in §10.1 — else `learn`'s load-bearing check is computed on
information that doesn't exist.

VERDICT: NOT CONVERGED

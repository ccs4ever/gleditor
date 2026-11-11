# Round 10 critique (vortex role): the cells"…" path feature

Scope: the uncommitted `cells"…"` implementation — lexer/parser/checker/compiler under
`apps/common/xanadu/vagent/`, the new `cell_path.{hpp,cpp}`, `OpcodeKind::Linked` and its
execution in `vortex_vm.{hpp,cpp}`, the pure-allowlist and mnemonic additions in
`mesh_evaluator.cpp` and `vql/compiler.cpp`, and the test battery under `tests/xuzz/`.
Line numbers verified against the working tree.

## What the feature is

`cells"…"` is one lexer token (`PrefixedStringLiteral`, token.hpp:22, merged at
lexer.cpp:386) whose string body is VQL path source. `vagent/cell_path.cpp` reuses the real
`vql::Parser` and then *narrows* the result: `##`, bare integer ids or `$binding` anchors;
signed dimension steps; posward `%` creations; one link-presence shape filter; at most one
traversal and it must be last; a step after a multi-creation step is refused. Anything else
the VQL grammar accepts — predicates, projections, clone tails, store/generator/version
anchors, computed creation values, yield modes, bidirectional dimensions — is refused with a
diagnostic, not silently interpreted.

The checker types every `cells"…"` as `[Cell]` (checker.cpp:862, :1411), validates `$` anchors
against bound `Cell` bindings, gives `Cell` `==`/`!=` identity semantics, and treats a `%`
step as an arena write for purity purposes. The scalar kernel lowers anchors, `New`/`Value`
for creations, and `Linked` for the rank traversal (compiler.cpp:745-855).

## Checks against Vortex constraints

- **Opcode numbering is persistence.** Opcodes mint via `makeScalarCell(int64(op))`
  (vortex_vm.cpp:74), so the enum's integers are on-disk data. `Linked` is appended at
  `vortex_vm.hpp:92` after `ChoiceDepth` — the only safe position. `getOpcodeKind`'s
  serialized-kind ceiling follows it (vortex_vm.cpp:110). No renumbering of existing kinds;
  `CompactOpNode` and the `ops.nodes` fixtures keep their layout.
- **`Linked` is a structural op everywhere it matters.** The two result-write suppression
  sites (vortex_vm.cpp:340-342, :1289-1291) list it beside `New`/`Clone`/`Link`/`Break`, so
  the op writes a rank under the head, never a `CellValue` into it. The pure-artifact
  allowlist gained it at mesh_evaluator.cpp:232, consistent with `New`/`Link`/`Break` being
  there: it is arena-local, allocates and links, and reaches no host boundary.
- **Input contract.** `#LINKED subject dim neg [follow] [filterDim filterNeg]`
  (vortex_vm.cpp:654-724). `resolveCell` prefers `inCells[i]` when contained and only then
  treats `inputs[i]` as an int CellRef (:50-67) — so a bound subject cell resolves to the
  cell itself, and the optional `follow` reads through its int content for `$`-anchor
  ref-boxes. The emitter honours the slot convention: slot 3 is bound whenever `follow` or
  a filter is present, slots 4-5 only for a filter (compiler.cpp:781-789).
- **Re-execution is idempotent.** `Linked` unlinks the head's previous `+d.grab`/`+d.step`
  materialization before rebuilding (vortex_vm.cpp:683-695). Two ordering details matter and
  are correct: the successor is read *before* `breakLink` (breaking first orphans the rest
  of the rank), and cleanup runs even when the subject is gone, or a re-execution would keep
  a stale rank instead of yielding `[]`.
- **Non-circular ranks, empty results.** `ArenaManifold::link` maintains reciprocal edges
  with no wraparound, so `linked(head, NEG)` is legitimately `nullopt`: `##/-d.items` yields
  `[]`, and negward traversal from a tail returns the prefix — covered by tests, matching the
  engine's rank semantics rather than contradicting them.
- **Budgets.** Both the cleanup loop and the member walk `consumeWork()`
  (vortex_vm.cpp:686, :703), which throws on exhaustion (arena_manifold.hpp:420-426); a
  cyclic `+d.step` hand-built by another writer cannot hang cleanup, and `seen` bounds the
  member walk. `Linked` mints ref-boxes under the head inside the caller's mark, so choice
  rollback trails the cells and links it creates — same discipline as `New`.
- **Identity.** `Cell` is a ref-box (int content naming the arena cell); `==`/`!=` compare
  identity, ordered comparisons refused. `[Cell]` is a head cell whose `+d.grab`/`+d.step`
  rank holds ref-boxes — the same layout `binding()` fabricates for `CellList` bindings
  (compiler.cpp:466+), so a `[Cell]` external binding and a `cells"…"` result are
  interchangeable.
- **`emitCells` (compiler.cpp:745-855).** Creation cells are minted up front so a later step
  can bind as subject a cell an earlier `New` only produces at run time. A single `%` hands
  itself to the next step; multi-`%` siblings stay rank siblings (which is why the validator
  refuses a step after them — ambiguous subject). Bare `%` skips the `Value` op so the cell
  gets *no* content rather than empty-string content. A last-step traversal materializes at
  run time under the bound head; otherwise `materialize()` links the result statically.

## Findings

### Fixed during implementation (recorded for the trail)

- **[Cell] assignment dropped the rank.** `x <- cells"…"` compiled (CellList==CellList) then
  ran a content-only `copy()`, leaving the binding `""` with no rank. Now a `cells` literal
  on the right emits into the binding head itself, and any other `CellList` source is
  refused at preflight (compiler.cpp:330-335, :645-651).
- **Cleanup order orphaned the rank.** `Linked` originally `breakLink`ed a box's `+d.step`
  before reading its successor; only the first box would ever be unlinked. Successor is now
  read first (vortex_vm.cpp:687-691).
- **Stale rank on dead subject.** Cleanup originally ran only when the subject resolved; a
  re-execution whose `$`-anchor referent had died kept the old rank. Cleanup now runs
  whenever a head is bound (vortex_vm.cpp:680-682).
- **`#"N"` is not VQL syntax.** Literal cell anchors are bare integers; `#123` never parses.
  Tests and docs use `cells"123"`; `#` forms stay negative tests.
- **Serialized-boundary test used `ChoiceDepth + 1` as "too large".** After appending
  `Linked` that value is valid; the test now uses `Linked + 1`.
- **A plain temp cell entered the op chain.** `emitCells` briefly pushed the creation box
  itself into `ops`, which would have earned a spurious `+d.spin` successor. Removed; only
  `Value` ops remain.
- **Embedded quotes terminate the vagent string.** `cells"…%\"first\"…"` needs escaped
  quotes in source — inherent to the prefixed-string form and now documented.

### Open minors

- **Literal anchors are validated at compile time only.** `cells"123"` checks
  `arena.contains` at preflight; a cell deleted between compile and run still materializes a
  ref-box naming it. `Linked` handles a dead subject by yielding `[]`, but an anchor-only
  path has no runtime check — the dead ref-box silently lands in the list. Acceptable for a
  single-arena turn; worth a runtime liveness check if paths ever run across a delete.
- **`cells"$v"` reads the ref via a `Value` write.** The op writes the binding's content
  into a box whose own content is then materialized — correct but non-obvious
  (compiler.cpp:824-834); the comment carries the *why*. A dedicated `Deref`-style op would
  be clearer but is not owed.
- **`inCells.size() >= 3 || inputs.size() >= 3`** mirrors `New`'s loose gate; the two counts
  are parallel in practice (inputs are resolved values of inCells), so the `||` is
  belt-and-braces, not a live ambiguity.
- **Subset asymmetry vs compiled VQL.** `vql/compiler.cpp`'s own `compilePathExpression`
  still refuses traversal outright (its latent `Link`-output drop means chained traversal
  can't work there either); `cells"…"` does not depend on it, but VQL-compiled paths remain
  create-only. Worth an entry in that file's stage doc, not a blocker here.
- **`New`'s "already used" failure is unreachable from `cells"…"`.** `New` refuses a bound
  output cell that is already linked (vortex_vm.cpp:736-743), which would fire if a create
  path re-executed inside a retried choice. The checker bars `%` from pure contexts, so a
  creation can never sit in a retried branch — safe today, but the coupling lives in the
  purity rule, not in the op; a future `bounded()` use that admits mutation would hit it.
- **The 14 failing tests in the full suite** (Subscription/Evidence/Irc/VPL plus one headless
  GL text render) are all against HEAD files untouched by this change — pre-existing breaks
  in the earlier evidence/VPL commits, not regressions.

## Verification

- 56 `VAgentCompilerTest` + lexer/parser/checker cells tests + `VortexLogicTest`/
  `VortexVmTest` — 204 tests, all green on this build.
- Full `xuzz_test` (2573 tests): the only failure attributable to this change
  (`InvalidSerializedOpcodeKindsAreNotDecoded`'s stale boundary) is fixed and passes; the
  remaining 14 are the pre-existing failures above.

VERDICT: sound; minors open, none blocking. The feature adds exactly one opcode, stays
inside the opcode-graph discipline, and refuses rather than approximates the VQL surface it
doesn't support.

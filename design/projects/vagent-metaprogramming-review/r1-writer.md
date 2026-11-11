# Round 1 critique (writer lane): vagent-metaprogramming.md

Lane: clarity, internal consistency, specification completeness, cross-reference accuracy, example
conformance, Markdown hygiene.

Verification note: every file:line citation spot-checked against the tree resolved correctly —
`compiler.hpp:28-34` (`CompiledStatement`), `compiler.cpp:108-110` (the quoted refusal verbatim),
`vortex_vm.cpp:73-114` (`mintOpcode`/`getOpcodeKind`/`opcodeMap_`/`asInt64`/`Linked` ceiling),
`:131-146`, `:217/:234/:255`, `:654-723` (`Linked` + seen-set at :699-701), `:736-743`, `:771-806`,
`:1156-1158`, `:1220-1240`, `:1306-1318`, `:1333-1335`, `vortex_vm.hpp:89-93` and `:186`,
`lexer.cpp:379-398` (any-identifier `PrefixedStringLiteral` merge), `token.hpp:22`,
`checker.cpp:553-567` (`validateType` incl. `Cell`), `:827` (`pureBody`), `:1412-1429`,
`compiler.cpp:465-498`, `:512-522` (`mint`/`spin`/`reference`), `:578-588` (`bounded`), `:849-872`
(`emitStatement`), `cell_path.{hpp,cpp}` cites, `vql/compiler.cpp:71` ("whole-rank traversal"),
`vortex_core.cpp:61` (`d.name`), `vortex_core.hpp:29` (`CellValue`), `arena_manifold.hpp:670-679`
(`compact` refusal), `:722-730` (WAM conditional trailing), `arena_manifold.cpp:1506-1612`
(`trail`/`discard`/`release`). Sibling-section references verified: spec §4.3/§4.4/§5.1 (rank
removal, no tail pointer)/§5.3/§5.7 (`module:Agent.travel`, `tried`)/§5.8 (frames, spill)/§5.9
(`Cell` ref-box)/§6.3/§6.4 (isolation)/§6.7 (contract cause)/§7.5/§8.1 (prefix error, `Ident '{'`
case convention)/§8.7 (`cells"…"`, `Cell`)/§10 (std:agent prelude, `text`/`intentions`)/§11.1/§14
(single-turn determinism)/§15 (fuzz rule); V1–V13 glosses all match; P1–P14 exists and P15 correctly
continues the register; R5/R6/R7/R8/R12/U1 glosses in §14 match `store-slice-convergence.md` and the
spec's §16 phrasing; `Opt<T>`/`[x]` presence test/`believes`/`succeed when`/`for x in e do`/event
literal syntax all conform to the spec's grammar.

## Findings

1. **MAJOR — `reflect` is used as an operation name but never defined** (§5.2, §6.1, §6.6, §15
   U-M7, Appendix). The text relies on it normatively: "a `reflect` result — is a check error"
   (§5.2), "a reflect-wrapped graph" (§6.1), "a `reflect`ed program-side graph" (§6.6), "program-side
   `reflect` output" (§15 U-M7), and the Appendix's "`reflect`-family reads". But the §6.1
   `std:meta` member list contains no `reflect` — the reads are named `program`, `module_named`,
   `symbol_named`, `clauses_of`, `test_of`, `body_of`, `plan_named` — and the glossary does not
   define it either. An implementer cannot tell whether `reflect` is a distinct verb to build or a
   collective noun for the reads, and the splice-legality and `learn`-refusal rules both hinge on
   the term. *Fix:* either add `reflect` to the §6.1 surface and the glossary as the named family of
   program-structure reads, or replace each use with the concrete members ("a `test_of`/`body_of`
   result", "`symbol_named`-family reads").

2. **MAJOR — §5.2's splice legality rule contradicts itself on `def` parameters.** It first
   requires `c` "to name a **statically known** fragment: a `let`-bound `code"…"` literal or a
   `code` parameter of a `def` whose expansion is itself being checked", then sentences later lists
   "a `def` parameter" among the *not* statically known values whose `splice` "is a check error".
   The same `Code`-typed `def` parameter lands on both sides of what the text itself calls "the line
   between quasiquote and eval" — an implementer cannot derive which ruling applies. *Fix:* state
   the distinction explicitly, e.g. "a `Code<T>` parameter is statically known only while its `def`
   is being expanded with a concrete fragment argument at a call site the checker is inlining; in
   every other context a parameter, field or reflection result is opaque and composes only through
   `run`/`graft`."

3. **MAJOR — the durability story contradicts the document's own stored-int staleness rule**
   (§4.1/§4.5 vs M11/§6.6/§6.7 and §12). §4.5 and §6.7 establish that durable references "are
   structural (links and `d.name` labels), never stored ints", because `promote` renumbers and a
   stale `Int` "can name the wrong cell silently". Yet §4.1's own substrate emits every
   `Jump`/`Call`/`Choice`/`Branch` target as `reference()` — `makeScalarCell(int64(target))`
   (`compiler.cpp:515-518`) — the identical int-content-names-dense-id mechanism the document
   declares stale. A `d.code`-registered or `learn`ed fragment promoted at checkpoint is rebuilt
   with fresh dense ids on `resume`, so every intra-fragment target int aliases an unrelated cell;
   `closed` constrains *links*, not int content, so even a closed fragment containing a `def` call
   carries a stale ref; and genesis re-grafting (§6.6) re-points the head's links but never repairs
   internal target ints (`validate` runs at graft install, not at genesis). As written, `learn` of
   any non-straight-line fragment silently corrupts across `resume`, undercutting M11 and §12's
   "durable mutation is explicit" property. *Fix:* specify the mechanism — targets emitted as link
   structure, a loader/genesis re-resolution step for fragment ref-ints (plus how ref-ints are told
   from data ints), or an explicit restriction of `learn`/`d.code` durability to `validate`-proven
   ref-free graphs — and add an "intra-graph reference ints" note to §4.5's table.

4. **MAJOR — the file fails the repository's own Markdown gates; every normative table is
   destroyed.** `mdformat --check --extensions gfm --extensions dollarmath --extensions frontmatter
   --wrap 100 design/vagent-metaprogramming.md` reports "File … is not formatted"; `mdl -i` reports
   MD022 at lines 583, 1039, 1041, 1043, 1045 and 1065. On disk, the header field table, §1 status
   table, §2 glossary, §4.5 survival table, §6.1 `std:meta` surface, §10.2 P15 register and §11
   refused-alternatives table are all flattened into `\`-escaped wrapped paragraphs — the exact
   corruption the Makefile warns plain `mdformat` produces without its gfm plugin, and illegible for
   the two tables (API surface, proposal register) an implementer most needs. (The file was modified
   at 2026-11-02 20:16:52 during this review, after a clean 1159-line version existed; this finding
   is against the file as it now stands.) *Fix:* re-run `mdformat --extensions gfm --extensions
   dollarmath --extensions frontmatter --wrap 100` (or `make format` with the plugins installed) and
   confirm `mdl -i` clean.

5. **MINOR — broken relative reference: `r10-vortex.md`** (§4.4). The name resolves to
   `design/r10-vortex.md`, which does not exist; the review file is
   `design/projects/vagent-review/r10-vortex.md` (its §1/scope does cover the `cells"…"` `Linked`
   substrate, so the citation content is right — only the path is wrong). *Fix:* cite
   `projects/vagent-review/r10-vortex.md`.

6. **MINOR — §9.3's worked example does not type-check under the document's own language rules.**
   `believes sees(food, water)`, `believes learned_rule(food_water)` and `navigate_to(water)` use
   Prolog-style bare atoms, but vagent has none: predicates take typed arguments and "Strings serve
   as atoms" (spec §6.1), so `food`, `water` and `food_water` are unbound identifiers —
   `UnknownName` errors under the checker discipline the doc inherits. If `navigate_to` is meant as
   an `effect`, the body additionally violates M6's refusal of suspending calls inside `code"…"`.
   Likewise §5.5's `static (2 * pi)` uses an undeclared `pi` inside a form that requires closedness.
   *Fix:* declare `predicate sees(what: Str, near: Str)` and write `sees("food", "water")`,
   `learned_rule("food_water")`, `navigate_to("water")` as a `def`, and `static (2 * 3.14159)` or a
   `static`-bound `pi`.

7. **MINOR — §1 deferred rows are incomplete and mis-ordered.** The table lists U-M6 before U-M5
   and gives no rows to U-M7 (generic deep-copy, referenced from the `reify` row) or U-M8 (prelude
   grafting), though §15 defines U-M1–U-M8. *Fix:* reorder U-M5 before U-M6 and add the two missing
   rows.

8. **MINOR — "§6.5.3" pseudo-subsection citation** (§7.3). §6.5 has three numbered list items, not
   subsections; "§6.5.3" reads as a section that does not exist. *Fix:* write "§6.5, item 3" or give
   the rule a name.

9. **MINOR — `splice`/`static` capture call position, undocumented.** §5.1 says the contextual
   keywords "stay legal as identifiers elsewhere", but with `'splice' Postfix` and `'static' Unary`
   in `Primary`, the spellings `splice(x)` and `static(x)` parse as the new forms applied to `(x)` —
   a user function or field named `splice` or `static` can never be invoked or read in the bare
   `f(x)` position. *Fix:* one sentence in §5.1 or §13 recording the reservation.

10. **MINOR — the `code` prefix's adjacency rule is unstated** (§5.1). The spec's `CellsLit` note
    documents that `cells "##"` is an identifier followed by a string, not a literal; §5.1's
    `'code' StrLit` production never says `code` inherits the same adjacency, though the lexer only
    merges `ident'"'` with no gap (`lexer.cpp:381-383`). *Fix:* add a clause mirroring the spec's
    `CellsLit` note.

11. **MINOR — no open-questions apparatus.** House style calls for an open-questions table; the
    doc substitutes §7.3 ("what remains unproved") and §15, which is defensible but leaves genuine
    open items (the `reflect` name, fragment ref-int relocation, splice-`def` expansion order)
    without a recorded home. *Fix:* a short open-questions table before the Appendix, or fold the
    three items above into §7.3.

## Summary

The document is unusually strong on evidence — every file:line citation spot-checked was accurate,
and all section/V/P/R/U-A references into `vagent-agent-language.md` resolve correctly for the v3.9
numbering. The blocking work is editorial-structural: the file on disk fails `mdformat --check` and
`mdl -i` with its tables flattened, an undefined `reflect` operation name carries normative weight,
the §5.2 splice rule states `def` parameters on both sides of the quasiquote/eval boundary, and the
`learn`-durability mechanism conflicts with the document's own stored-int staleness accounting for
intra-fragment `Jump`/`Call`/`Choice`/`Branch` target ints.

VERDICT: NOT CONVERGED

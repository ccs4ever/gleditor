# Writer critique, round 2: design/vagent-agent-language.md (v2.0, 1417 lines)

Mechanical checks: `mdformat --wrap 100` with gfm, dollarmath and frontmatter reports the file
clean. `mdl` (repo `.mdl_style.rb`) prints nothing. Every `](#anchor)` link resolves to a heading
slug. Fences pair up. I checked about a dozen `file:line` citations against `mesh_actor.cpp/.hpp`,
`arena_manifold.hpp`, `vortex_core.cpp` and `identity_layout.hpp`. All match (402-405, 344, 350,
501, 521, 668-684, `maxActors` 128, `promote` at 833-836). The structure is now good: status column,
glossary, priced rulings, refused alternatives, conditional properties. What remains is mostly the
claim "a grammar that parses its examples" (Appendix), which does not yet hold, plus status honesty
about sibling documents that are themselves unbuilt.

## Round-1 findings

| #   | Sev      | Status   | Note                                                                                                                                                              |
| --- | -------- | -------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1   | BLOCKING | RESOLVED | §1 table has a State column; withdrawn items are listed in the Appendix. See new finding 1 for the remaining "built" overclaim.                                   |
| 2   | BLOCKING | RESOLVED | §9.1 has a "Beyond link/value" column and the circular "proves" is gone.                                                                                          |
| 3   | MAJOR    | RESOLVED | §14 states conditions; the deadline bounds waits. The adjectives are gone.                                                                                        |
| 4   | MAJOR    | RESOLVED | §3 rulings with prices, §13 refused alternatives, §17 open questions.                                                                                             |
| 5   | MAJOR    | RESOLVED | Ten dimensions with a "why not existing" column; settings named in §10.                                                                                           |
| 6   | MAJOR    | MOSTLY   | Glossary added and "ring/headcell" retired. One drift left (finding 7).                                                                                           |
| 7   | MAJOR    | PARTLY   | Grammar rewritten and precedence added. Several examples still do not obey it (findings 2-5).                                                                     |
| 8   | MAJOR    | RESOLVED | Cross-arena links removed; clone views for many-to-one.                                                                                                           |
| 9   | MAJOR    | RESOLVED | Citations verified; microsecond claim gone. Minor: `cloneMaster` takes `(ref, cloneDim)` (`arena_manifold.hpp:522`), the prose writes a one-argument call (§5.3). |
| 10  | MAJOR    | RESOLVED |                                                                                                                                                                   |

Disposition items (AOP protocol, overridable selection, commitment variants, test harness, d.cursors
refusal, comparison matrix) are deferred or refused with reasons. I do not re-raise them.

## New findings

1. MAJOR. Header, §1 table, V6a/V7, §11.1. "Built" is used for mechanisms whose only specification
   is a sibling document that is itself unbuilt. `vicon-goal-directed-language.md` line 4 says
   "specification only; nothing here is wired into the gleditor build", yet §1 marks "Reversible
   assignment `<-`" as **built** (citing vicon §4.3, and `<-` is defined at vicon line 245 under
   that section). AGENTS.md says Vlog §4-§5 are built and §6-§7 (clauses, solver) are not, gated on
   U1. §1 marks "Beliefs ... Vlog §6" as **built (Vlog)**, and V7 relies on `assertz`/`retract` and
   the `d.clause` layout (Vlog §6.1-6.2). `grep -rl 'assertz\|retract' apps/` finds only
   `system_docs.cpp` and `vprolog/main.cpp`, not a Vlog database. The "owes C++" header row lists
   three items; the compiler for `<-`, the `std:goal` package (`tab`, `match`, `upto`, `to`, `!`),
   and the Vlog database are also unbuilt prerequisites. Fix: split the State column into "mechanism
   built" (arena trail, `#Choice`) and "front end or library built" (no for vicon and Vlog §6). Add
   a row or a short "Prerequisites that are specifications" list in the header: vicon compiler,
   `std:goal`, Vlog database/`std:logic`, Vortex `std:collections`. Say that vagent's schedule
   depends on them.

1. MAJOR. §8.5 `ThrusterNav.move` contradicts §8.3. The body is
   `s <= max_speed & { speed := s; thrusters.set(s, heading) }`. The block is an operand of `&`,
   which §8.3 defines as a speculative position. §8.3's own table allows `:=` and `effect` calls in
   bounded positions only, and V6 says the compiler rejects an effect inside `&`. So the first
   capacity example in the document fails the checker. `thrusters` is also an undeclared ambient
   name, never listed in §8.8. Fix: write the method as `s <= max_speed` (a guard statement that
   fails the method) followed by statements:
   `s <= max_speed\n speed := s\n thrusters.set(s, heading)`; or use an `if ... then` statement. Say
   what `thrusters` is (a native host object from `mars:hardware`-style bindings, as §12.3 says) or
   rename it.

1. MAJOR. §8.2 versus §8.4 and §8.9. §8.2 says "A chain `a < b < c` works as in Icon". The grammar
   has `Cmp ::= Concat [ CmpOp Concat ]`, which accepts one comparison, and §8.4 level 7 says
   "non-associative as written; chained comparison is explicit". Both cannot hold, and the Appendix
   advertises "a grammar that parses its examples". Fix: pick one. Icon behaviour is
   `Cmp ::= Concat { CmpOp Concat }` left-associative, with §8.4 saying "left-associative, so
   `a < b < c` is `(a < b) < c`, which tests both". Update the note in §8.4 accordingly.

1. MAJOR. Examples use names the document does not introduce. Specifically:

   - §12.3 `survey` calls `tab(match("SPEC:"))` and `upto(";")`, but the module imports only
     `std:agent`, and §8.11 says `tab`/`move` come "from `std:goal`". §8.8 lists no ambient names
     for them. Add `import std:goal` to the example or list the scanning functions in §8.8.
   - §12.3 top-level `goal Reach ... succeed when position() == location` calls a `Mobility` method
     bare. Per §8.5 a bare call needs a `uses` of the capacity, and a top-level goal has no `uses`.
     Either allow `uses` on `GoalDecl` (grammar has none), or write `Mobility.position()` and say
     how a qualified call finds the installed skill at run time (it is the agent's). Same issue for
     §6.2 `battery()` and `reserve_floor()`: in §6.2 they are calls, in §12.3 `reserve_floor` is an
     agent parameter (no parentheses). Make the two snippets agree.
   - §12.2 `emit Payload{...} for [dest]` has no `in`. §7.2 says `for r` restricts delivery "to the
     listed handles, which must be members" of a space. Which space does a bare `for` use, and is
     `dest` a member of it? Say that the default space is `outer` (as for a plain `emit`), or
     require `in`, or let `for` alone address handles directly. As written, the Loopback/Sealed
     example may deliver nowhere.
   - §8.5 uses `CollisionWarning`, `ObstacleEvaded`, `TargetLogged` and `Point` without declaring
     them. This is acceptable in a fragment; say "fragment, events elided" once.

1. MAJOR. §8.1 reserved words omit `spawn` and `return`, both used in the grammar (`Action` and
   `Stmt`). The list also treats `drop`, `holds`, `when`, `succeed` as keywords yet the grammar uses
   `drop` as both a statement and the head of `drop when`; say that `drop when` is lexed as two
   words and decided by position. Fix: add `spawn`, `return`; state the count and that the list is
   exactly the set of keyword tokens in §8.9 (check mechanically, because this is what a lexer is
   generated from).

1. MAJOR. §8.9 note on record literals versus blocks has a hole. "`Ident '{'` in an expression is
   always a record literal." `PlanDecl ::= ... [ 'where' Expr ] Block` puts a block after an `Expr`.
   `plan p for G(x) where ready {` would parse `ready { ... }` as a record literal. The same holds
   for `succeed when flag` followed by a `{`-opened next clause on the same line, and for a handler
   guard written without brackets. Fix: require parentheses or a bracket around `where`'s condition
   (`where [ Expr ]`, as handlers do) or state that the block must start on a new token that cannot
   continue an expression, and say how the parser decides. This is the kind of thing the proposed
   lexer has to get right on day one.

1. MAJOR. §13 last row and §6.7 first row are corrupted. §13 ends with a row whose first cell is
   `` `&&`/\` `` and whose reason cell is empty. A refused alternative with no reason violates the
   document's own style, and the row shows a rendering fault (an unescaped `|` inside a code span
   splits the table cell, or a backtick is unbalanced). §6.7 row 1 shows "the enclosing `&`/\`" for
   the same reason. Fix: write `` `&&` and `\|\|` `` with the pipe escaped as §8.4 does, and fill
   the reason (for example: "Icon's `&` and `|` are generator conjunction and alternation; borrowing
   boolean spellings would make a reader expect short-circuit booleans and break V11"). In §6.7
   write "the enclosing `&` / `\|` expression".

1. MINOR. Terminology drift on "envelope". The glossary defines envelope as "the `ActorMessage`
   carrying an event; never visible to vagent code". But V5, §5.1 (`d.outbox`: "staged envelopes"),
   §5.2, §9.1 and §15 speak of "envelope cells" in the arena, which vagent code and the adapter do
   see. These are staging records the adapter turns into an `ActorMessage`. Fix: call the arena
   cells "outbox entries" and keep "envelope" for the wire message, or amend the glossary to say
   both.

1. MINOR. Undefined or late-defined terms. "Performative" is used in §6.5's pseudocode and §7.1 but
   not defined (glossary or text). "Relay", "host", "launcher agent" and "view" (clone view) are
   used before definition; §2 defines "space" with "host agent" but "launcher" appears first in
   §5.4. `mark`/`release`/`discard` carry counter-intuitive meanings (discard keeps the writes,
   release undoes them), stated only in §4.2 step 6. Add three glossary rows (performative, clone
   view, mark/release/discard).

1. MINOR. Process references leak into the spec. V6 says "round 1 showed", §13 says "round 1 found
   it placed before the fallible action in every example". A reader of the design without the review
   history has no "round 1". Fix: state the reason itself ("an example placed `commit;` before the
   fallible call, which protects nothing") and leave review history to the Appendix.

1. MINOR. §11.2 "the adapter about 800 lines with tests; the front end about the size of vprolog's"
   is an estimate with no basis. The house style prices rulings with evidence. Give `wc -l` of the
   comparison module (`apps/common/xanadu/vprolog/`) or drop the number.

1. MINOR. §8.10 row 5 says `in`/`every` from SARL's `Schedules` map to `after`/`periodic`; the row
   for `wake` says "`emit e in inner for [self]` is not offered" but the example form is not what
   SARL's `wake` does and the "Why" cell restates the conclusion rather than the reason. Rewrite as
   "SARL `wake` posts an event to the agent itself. In vagent an agent's internal events are belief,
   goal and plan events, and a timer or `yield` provides deferral."

1. MINOR. Numbers in examples. `priority 100` (§8.5, §12.3), `50.0` and `12.0` are example literals,
   which is fine, but §14/§10 claim "none is a constant in code". Add one sentence in §8.5 that
   example literals stand for values that a real program reads from skill parameters or settings, so
   the AGENTS.md magic-number rule is not read as violated by the spec.

1. MINOR. §12.1 `for n in 1 to partitions do spawn Worker() in inner` leaves `n` unused. Write `_`?
   The grammar has no such binder; either allow `_` in `for` or accept the unused variable and say
   so.

1. MINOR. The Contents list has 18 numbered sections plus an appendix. Sections 14-18 are short.
   Comparison (§18) could move before "Open questions" so the document ends on the open items; this
   is optional. The title of §1 ("What is being claimed") is better than version 1.0's, but the
   section is a status table; "Status of each mechanism" says what it is.

## Summary

Counts: 0 BLOCKING, 7 MAJOR (findings 1-7), 8 MINOR (findings 8-15). The design is coherent and the
citations hold. The MAJORs are: "built" labelling of features that sit on unbuilt sibling
specifications (1); examples and grammar that disagree with their own rules (2-6); two corrupted
table rows (7). All are local edits; none needs a design change.

VERDICT: NOT CONVERGED

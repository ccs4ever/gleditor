# Writer critique, round 3: design/vagent-agent-language.md (v3.0, 1600 lines)

Mechanical checks: `mdformat --wrap 100` (gfm, dollarmath, frontmatter) reports the file clean on a
copy. `mdl -s .mdl_style.rb` prints nothing. No prose line is over 100 columns outside tables.
Contents anchors match the headings (18 sections plus the appendix, in order). The round-2 corrupted
table rows are fixed (§6.7 row 1, and the §13 `&&`/`||` row, which now carries a reason). Citations
I re-checked still hold: `vicon/compiler.cpp:12-22` (the deviation note, which does name "a new
opcode or a new ChoicePoint field"), `vicon/compiler.cpp:358-361` (`:=` and `<-` both emit a trailed
Bind), and the `std:` registrations at `vortex_stdlib.cpp:1451-1469` (`std:collections`,
`std:string`, `std:logic`, `std:goal`, `std:math` exist as registered names). The revision is a
clear improvement in status honesty and in the staging of effects.

## Round-2 findings

| #    | Sev   | Status   | Note                                                                                                                                                                                                                                                                               |
| ---- | ----- | -------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1    | MAJOR | RESOLVED | Header has a `prerequisites` row, §1 defines built / prerequisite / owed, Vlog §6 is labelled a prerequisite. Leftovers are findings 3 and 4.                                                                                                                                      |
| 2    | MAJOR | RESOLVED | `ThrusterNav` is a `native skill`; no effect sits inside `&` any more; `thrusters` is gone.                                                                                                                                                                                        |
| 3    | MAJOR | RESOLVED | Comparison is non-associative in §8.2, §8.4 and the grammar alike, and §8.2 says "write `a < b & b < c`". I proposed Icon chaining; the document chose differently and is consistent, which is what mattered.                                                                      |
| 4    | MAJOR | PARTLY   | `tab`, `match`, `upto` are imported from `std:goal` in §12.3. But the imports create a cycle (finding 2).                                                                                                                                                                          |
| 5    | MAJOR | RESOLVED | Reserved and contextual keyword lists are separate; `import std:agent` is explained.                                                                                                                                                                                               |
| 6    | MAJOR | RESOLVED | The record-literal rule names `where`, `Contract` and goal clauses and requires parentheses; `where believes battery(?level) {` parses because a `Term` ends at `)`.                                                                                                               |
| 7    | MAJOR | RESOLVED | Tables fixed.                                                                                                                                                                                                                                                                      |
| 8-15 | MINOR | RESOLVED | "envelope" is the wire message only; "performative", "clone view", "mark/release/discard", "launcher" are glossary rows; "round 1" is gone from the body; §11.2 gives measured line counts (1,632 + 3,176 = 4,808, matching my `wc -l`); the example-literals sentence is in §8.5. |

## New findings

1. MAJOR. §8.2, §8.9 static restrictions, §12.2. The `Bool` rule contradicts its own remedy and one
   worked example. §8.2 says "A `Bool`-typed expression is rejected as a condition, a guard, or an
   operand of `&`, `|` or `not`, with the message 'compare with `== true`'". The same section says
   `a == b` "produces `b` or fails", so `sealed == true` produces a value of type `Bool` and is
   itself a `Bool`-typed expression; by the rule as written it is rejected too. §12.2's guard
   `[a.level > alert_level & sealed == false]` has exactly that shape (a comparison producing
   `false`, an operand of `&`). The rule needs a syntactic form, not a type test. Fix: "a variable,
   field, literal, call or other expression of type `Bool` whose root is not a comparison or `not`".
   Add one sentence saying `x == true` succeeds with `true` when `x` is true, so the remedy is
   self-evidently total.

1. MAJOR. §12.3. The example's module structure is circular. `mars:exploration` declares
   `capacity Mobility`, `Science` and `Routes`, and imports `Chassis`, `Spectrometer` and
   `TerrainPlanner` from `mars:hardware`. The closing paragraph says those skills implement
   `Mobility`, `Science` and `Routes` (native skills, and "a vagent skill implementing `Routes`").
   An `implements` clause must name a capacity in scope in the skill's own module, so
   `mars:hardware` would have to import `mars:exploration`, which imports it. A reader copying the
   example gets an unresolved name. Fix: declare the three capacities in `mars:hardware` (or a third
   module `mars:capacities`), import them with the skills, delete the capacity block from the
   listing, and say in one line where each is declared. §8.5 keeps `Navigation` and `ThrusterNav` in
   one fragment and is fine.

1. MINOR. Header `owes C++` says "five changes in `mesh_actor.cpp`", but §11.3 lists (a)-(e) with
   (c) marked "not needed ... Deferred", and its closing line says vagent depends on (a), (b), (d)
   and (e). Write "four changes (a, b, d, e); (c) is deferred".

1. MINOR. §1 defines "Prerequisite" as "specified by a sibling document and not yet built", yet the
   header calls vicon's front end and `std:goal` "partly built", and the `<-` row says "built" while
   the compiler that emits it is a prerequisite. Say once in §1 what exists (lexer, parser,
   compiler, driver under `apps/common/xanadu/vicon/`; the deviation note says what the compiler
   cannot yet do) and make the `<-` State cell read "built in the VM and arena; the vicon compiler's
   use of it is partly built", so "built" never leaves open whether it means the VM or vicon.

1. MINOR. §6.1 writes `at(rover, 3, 4)` and `at(rover, ?x, ?y)`. §8.7 has no atom or symbol type,
   and in expression position `rover` is an `Ident` naming a variable that does not exist. Use
   `"rover"`, or add an atom literal to §8.1 and §8.7 and price it (a cell per atom, compared by
   content).

1. MINOR. Terminology drift. (a) "local delivery" (§1, V6) and "host delivery" (§4.2, §7.5, §14)
   name one thing, and the glossary has neither. (b) §6.3 says `adopt` "returns its handle"; §8.6
   says it returns an `intention`; "handle" was retired in the glossary. Add a glossary row "host
   delivery" (an effect result or percept posted by the adapter; not a mailbox message) and use the
   defined terms throughout.

1. MINOR. §6.6 lists `x := m(args)` and `m(args)` as effect suspensions, but §12.3 uses
   `let reading := spectra(target_id)`. State that `let x := m(args)` is one too (the ask row
   already says "also `let x := ask`"), and say whether a call nested in a larger expression
   (`f(spectra(t))`) is rejected, since the intention would suspend mid-expression.

1. MINOR. §8.9 gives `install S(args)` and `behavior B(args)` both as agent members (`AgentOnly`)
   and as actions. The text says only that `believe` members are asserted before `on Initialize`.
   Say when the member forms run (before `on Initialize`, in declaration order) and that the action
   form is the run-time one; otherwise `install Loopback()` in §12.2 has two readings.

1. MINOR. §12 has no worked example of delegation (`achieve ... by a within d`), `ask`/`answer`,
   `tell`, `await` or a `maintain` goal; they appear only as table rows and the `KeepCharged`
   fragment in §6.2. These carry the most new rules (V4, the §6.6 restrictions, read-only answers).
   A §12.4 of about 25 lines (a requester that delegates with a deadline and an `else`, a receiver
   with `accepts` and an `answer`) would exercise the grammar's `Action` rules. §12.3 also repeats
   the `Reach` and `Survey` goal declarations of §6.2; let one place own them.

1. MINOR. §5.2's diagram uses `(+d.x)`, `(-d.clone … master)` and `…` without a legend. Add one
   line: `+d` is the posward neighbour, `-d` the negward, `…` elides the rest of the rank.

1. MINOR. §8.9 uses nonterminals it never defines: `Duration`, `IntLit`, `Literal`, `Ident`, `Word`,
   `NEWLINE`. Add one line under the grammar saying these are the lexical forms of §8.1.

1. MINOR. §6.6 and V6 say a handler's effect is "staged without waiting", and step 7 of §12.3 says a
   later `EffectFailed` "finds no intention and is counted as late". For a handler-staged effect
   there never was an intention. Say in §6.6 where its result goes (counted in `late_deliveries`, or
   raised as an event).

1. MINOR. Structure, optional. §8 is about 340 lines in eleven subsections. §8.10 and §8.11
   (comparison tables against SARL and vicon) could follow §9 so a reader reaches lowering sooner.

## Summary

Counts: 0 BLOCKING, 2 MAJOR (findings 1, 2), 11 MINOR (3-13). The document is argued, priced and
honest about status. The remaining MAJORs are local: a type rule that rejects its own recommended
remedy and a worked example (1), and a worked example whose module imports are circular (2). Both
are one-paragraph edits.

VERDICT: NOT CONVERGED

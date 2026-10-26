# Writer critique, round 4: design/vagent-agent-language.md (v3.1, 1858 lines)

Mechanical checks: `mdformat --wrap 100` (gfm, dollarmath, frontmatter; v0.7.22) `--check` passes on
a copy. `mdl` with `.mdl_style.rb` prints nothing. The one non-table line over 100 columns is inside
the §6.5 `text` fence, which MD013 exempts (`ignore_code_blocks`). Contents anchors match the 18
headings plus the appendix. Version 3.1 agrees in the header and the appendix. Counts recheck:
eleven dimensions in V10, §5.1, §10; header "seven changes" equals (a), (b), (d) to (h).

## Round-3 findings

| #     | Sev   | Status   | Note                                                                                                                                                  |
| ----- | ----- | -------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1     | MAJOR | RESOLVED | §8.2 `Bool` rule is syntactic and names `sealed == false` as accepted; §12.2 repeats it consistently.                                                 |
| 2     | MAJOR | RESOLVED | Capacities and skills live in `mars:hardware`; `mars:exploration` imports them; no cycle.                                                             |
| 3     | MINOR | RESOLVED | Header says seven changes. The §11.3 closing sentence is clumsy (finding 4 below).                                                                    |
| 4     | MINOR | RESOLVED | §1 says what exists under `vicon/` and why vagent does not reuse its compiler.                                                                        |
| 5-8   | MINOR | RESOLVED | Atoms are strings (§6.1); "host delivery" is a glossary row; `let x := m(args)` and nested-call rejection are in §6.6; member-form timing is in §8.5. |
| 9     | MINOR | PARTLY   | §12.4 exists and exercises `ask`, delegation, `await`, `tell`. `Reach`/`Survey` declarations are still duplicated between §6.2 and §12.3 (finding 6). |
| 10-12 | MINOR | RESOLVED | Diagram legend, lexical-nonterminal note, handler effect results (§6.7 "effect failure" row).                                                         |
| 13    | MINOR | OPEN     | §8.10/§8.11 still sit before §9. Optional; not re-raised.                                                                                             |

## New findings

1. MAJOR. Header `prerequisites`, §1, §8.11, §12.3. The scanning functions the example imports are
   not built, and the header says they are. §12.3 writes `import std:goal (tab, match, upto)`, and
   §8.11 says string scanning is "same, from `std:goal`". The header lists "`std:goal`'s scanning
   functions (registered in `vortex_stdlib.cpp`)". `buildGoalModule` (`vortex_stdlib.cpp:3025-3735`)
   exports only `to`, `from`, `seq`, `repeat`, `write`, `writeSeq`, `limit`, `cmp`, `alt`, `fail`,
   `succeed`, `scan`, `subject`, `pos`, plus the host routines `every`, `bang`, `if`, `while`,
   `until`. A search for `tab`, `match`, `upto`, `move` finds them only in the vicon spec
   (`design/vicon-goal-directed-language.md:345-348, 392`). By §1's own definition they are a
   prerequisite, not built, and the header's parenthesis claims the opposite. Fix: in the header
   write "`std:goal`'s `tab`, `move`, `match`, `upto` (specified in vicon §7; the module registers
   `scan`, `subject` and `pos` today)"; add a §1 table row for scanning with State "`scan`,
   `subject`, `pos` built; the matching functions a prerequisite"; and have §12.3 say once that its
   scanning line depends on that prerequisite.

1. MAJOR. §12.3, §6.2, §8.5. `Point` is used as a type and a record constructor
   (`Point{x: ..., y: ...}`, `from_point.x`) and is imported by `import std:math (Point)`. The
   standard library's math module has no such type (`buildMathModule`, `vortex_stdlib.cpp:1490`,
   exports functions such as `abs` and `min`; `grep Point` over `vortex_stdlib.cpp` finds nothing),
   and §10 does not list one for `std:agent`. A reader copying the example gets an unresolved
   import. Fix: declare `record Point { x: Float, y: Float }` at the top of `mars:hardware` and
   export it, drop the `std:math` import from both modules, and use the same declaration in the §6.2
   fragment (or say "`Point` is declared in §12.3").

1. MINOR. §6.6, §8.9. The prose says "a deadline is required ... for an effect it is declared on the
   capacity method", but `MethodSig` makes `'within' Duration` optional for every method, and the
   "Static restrictions" bullet in §8.9 does not list it. Add to that bullet "an `effect` method
   must declare `within`, and a non-effect method may not", or state which.

1. MINOR. §11.3 closing sentence: "vagent depends on all seven of (a), (b) and (d)-(h)". Write
   "vagent depends on seven of the eight: all but (c)." Also the header's "seven changes" could name
   them once: "(a, b, d, e, f, g, h)".

1. MINOR. Terminology. (a) `accepts` is "a capacity every agent has" (§7.1) but §10 names the
   capacity `Accepts` with method `accepts`; say "the `Accepts` capacity" in §7.1. (b) "restorer"
   and "goal intention" are load-bearing in §6.2-§6.3, §6.4 and §12.3 and are not glossary rows.
   Add: "goal intention: the permanent intention of a maintain goal, never running a plan itself";
   "restorer: the `achieve` subgoal a goal intention adopts when `holds` fails".

1. MINOR. §6.2 and §12.3 both declare `predicate position/battery` and
   `goal Reach/KeepCharged/Survey` with the same text. Mark the §6.2 block "repeated in §12.3" or
   cut the duplicate from §12.3 and say so; two copies will drift.

1. MINOR. §8.2: "or `c & e` when `e` contains nothing restricted to bounded positions" is hard to
   parse. Say "or `c & e`, provided `e` contains no `:=`, `consume` or `terminate` (§8.3), since `e`
   is then a speculative position".

1. MINOR. §12.4. The example is not self-contained: `warehouse` and `deadline` are parameters, but
   nothing says who spawns `Warehouse` or how `Dispatcher` receives its name. Add one sentence ("a
   launcher module spawns `Warehouse` and passes its name"), or spawn it in `Initialize`. The
   comment "tells its delegator" should read "tells its parent, which is the delegator here".

## Summary

Counts: 0 BLOCKING, 2 MAJOR (1, 2), 6 MINOR (3-8). Both MAJORs are one-paragraph edits to the status
header and to the example's imports. Prose, status honesty, cross-references, tables and examples
are otherwise consistent, and every round-3 MAJOR is resolved.

VERDICT: NOT CONVERGED

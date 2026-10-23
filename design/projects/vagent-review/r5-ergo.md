# vagent review, round 5 (final) - ergonomics and surface-language critic

Document: `design/vagent-agent-language.md` v3.2 (2002 lines). Read: my round-4 critique and the
disposition; §1, §5.5-5.9, §6.5-6.7, §7, §8 (all), §9.1, §10, §11, §12 (all), §13-§14. Every example
in §8.5 and §12 was re-checked by hand against the §8.9 grammar, §8.2-8.4 and the
Bool/purity/position rules. SARL claims are from my own knowledge (SARL is not in the repo).

Tally: 0 BLOCKING, 3 MAJOR, 9 MINOR.

## Part 1. Round-4 BLOCKING / MAJOR findings

| R4 # | Topic                                          | Status                                                                          |
| ---- | ---------------------------------------------- | ------------------------------------------------------------------------------- |
| 1    | `ask a.q(args)` has no static type             | RESOLVED: top-level `question`, `AnswerDef` implements it, `ask` typed (l.863)  |
| 2    | reserved words usable as params but unreadable | RESOLVED: `ParamList` takes `Ident`, `Word` only after `.`/fields/keys (l.1249) |

Round-4 minors: #3 (transitive purity), #4 (return as action; bottom type), #5 (case convention), #6
(`x == 1 | 2` warning), #8 first half (no `as` on belief triggers), #9 first half (`for r` takes a
name), #10 (SARL self-delivery row), #11 (var initialiser timing), #12 (prose) and #13 (`Point`,
`Map`) are fixed. Carried over, still open, all MINOR: see finding 11.

## MAJOR

### 1. (§8.9 `SkillDecl` l.1182-1184, §8.10 l.1283, l.240) A vagent-written skill cannot say `uses`

`SkillDecl` bodies are `{ VarDecl | MethodDef | LifecycleDef }`; `UsesDecl` is a `Member` of agents
and behaviors only. Yet §8.10 justifies dropping SARL's `requires Cap` with "a skill's needed
capacities are found by `uses` in its methods", and l.240 says "capacities are imported with
`uses`". There is no place in the grammar to write it. §8.5's name-resolution rule (l.1096-1098)
lists "the agent's or behavior's own `def`s, then used capacities" and is silent about a skill. So a
skill that composes another capacity (a `Sealed` transport calling `Navigation`, a planner calling
`Logging` explicitly) is unwritable, and the stated reason for a SARL divergence refers to syntax
that does not exist.

Fix: add `UsesDecl` to the skill body (`{ UsesDecl | VarDecl | MethodDef | LifecycleDef }`), extend
the resolution sentence ("in a skill: its own `def`s, then capacities it `uses`, then `std:agent`"),
and have the checker reject a skill that calls a capacity it does not `uses` (as it does for
agents). State at the same time that `effect def` is legal only in `capacity` (a vagent-written
skill cannot implement an effect, because the body would run inside the turn);
`MethodDef ::= MethodSig Block` currently admits `effect def f() { }`.

### 2. (§10 l.1396-1410, §11.2, §11.1) Settings, build targets and the log category are …

Full title: 2. (§10 l.1396-1410, §11.2, §11.1) Settings, build targets and the log category are
changes to existing code and are not in the register

The owner's ruling covers "anything else" in existing code. The document adds eleven `vagent.*`
settings "read from `system://settings`" (l.1396). AGENTS.md says "Adding a setting means adding a
`defaultSettingSpecs()` entry, which is easy to skip" (`system_docs.cpp`); governance rule 2
requires these values live in a system xanadoc. That is an edit to existing code with schema,
defaults and units, and it is neither listed in §11.2 nor mentioned in §10. Two smaller ones of the
same kind: `Logging` "writes through the `vortex.agent` category, which the logging migration
(`design/logging-migration.md`) must register" (l.1393), which is a change to a registry owned by
another plan; and `apps/vagent`, the new test binary and the `tests/fuzz/` targets (l.1892) need
Makefile rules, which is existing build logic AGENTS.md says not to hand-pick.

Fix: add rows to §11.2 in the same format. "P10. `vagent.*` entries in `defaultSettingSpecs()`. Why
not elsewhere: system xanadocs are the only configuration store (governance §2) and the settings
table is closed. Without it: the adapter reads compiled-in constants, which governance forbids, so
this one should be approved with the first adapter commit." "P11. Makefile: `vagent`, `vagent_test`,
fuzz targets. Without it: no build." For the log category, say vagent uses an existing category
until the migration registers `vortex.agent`, naming the fallback. Add them to the list at l.60
("P1-P11").

### 3. (§11.2 P1, §9.1, §12.3) The worked example depends on P1 without saying so, and …

Full title: 3. (§11.2 P1, §9.1, §12.3) The worked example depends on P1 without saying so, and P1's
"Without it" understates the loss

P1's fallback is "methods may not be called from speculative positions, so `f(x) & g(y)` with
methods `f` and `g` is a compile-time error". §8.3 defines speculative as "an operand of `&`, `|`,
`to`, a comparison or a call, or anything inside one". By that definition an argument of a call is
speculative, so `cost(direct(here, location))` in §12.3 (both are capacity methods) is a call inside
a call and would be rejected without P1; so would `follow(route)`-style nesting of any two methods,
and every `if m(x) then ...` condition with a method. The example the document points to as its
showcase compiles only with P1, and §12.3 never cites P1. Also, since every statement's lowering
begins with `#ChoiceDepth` (§9.1 row 1), "static depths only" must mean the compiler counts choice
points statically, which is possible only if no speculative call exists; the fallback is therefore
"no nested or combined method calls", not the mild restriction the cell implies.

Fix: (a) rewrite P1's "Without it" to say exactly that: "no method call may appear as an operand of
`&`, `|`, `to`, a comparison, or as an argument of another call; `cost(direct(a, b))` must be
written `let r := direct(a, b); cost(r)`"; (b) cite P1 in §12.3's lead-in ("the route statement
relies on P1; without it, bind each call to a `let` first") and show the `let` form as the fallback
code. If that form defeats the example (the point is one speculative statement), say so, because it
tells the owner what P1 buys.

## MINOR

### 4. (§1 l.60, §10 l.1376-1378, §12.3 l.1590, §8.11) Prerequisites are relied on …

Full title: 4. (§1 l.60, §10 l.1376-1378, §12.3 l.1590, §8.11) Prerequisites are relied on without a
number or a fallback

`std:goal`'s `tab`, `match`, `upto` (§12.3, §8.11 "once `std:goal` registers them") and Vlog §6's
clause database (§1 l.77, §10) are "approved there, not here" (l.1480). `vortex_stdlib.cpp:3006`
already builds the `std:goal` module, so registering scanning functions is an edit to existing
stdlib. The doc is honest that they are prerequisites, but the owner's test is "proposal number and
a stated fallback", and §12.3 has neither: if scanning is not approved,
`let mineral := reading ? (...)` does not run. Fix: one sentence per prerequisite in §11.2's
footnote paragraph: "without vicon §6, §12.3 extracts the mineral with `std:string` (`split`, l.1 of
vortex_stdlib.hpp) and drops the scanning example; without Vlog §6, `std:agent` keeps its own clause
walk (§10 'beliefs')". Both fallbacks are already implied; make them explicit.

### 5. (§8.6 l.1122, §8.9 `Action`, §8.1 l.947-949) `return` is defined only for methods

The table says `return [e]` "leaves the method, answer or `def`" ("method" and "`def`" are the same
thing). A handler or plan body has no stated meaning, and `if done then return` in a plan is the
natural early exit. Also the newline rule lists "a keyword such as `yield`" as ending a statement
but not `return`, so `return` followed by a newline then an expression is ambiguous between
`return e` and `return; e`. Fix: "In a handler, `return` ends it successfully, keeping its writes;
in a plan, it ends the plan, after which `succeed when` is tested as at the end of the body; in an
`answer` or `def` it ends it with `e`." And name `return`, `fail`, `consume`, `terminate`, `none`,
`self` as statement-ending keywords; `return` takes an operand only on the same line.

### 6. (§5.5 l.512, §8.8) Implicit event fields have no types

`source`, `space`, `time`, `ordinal` are "always present" and "absent for a host delivery", but §8.8
gives no types. `for t.source` (l.1506), `[q.space == outer]` and `h.time` all depend on them. The
reads-succeed-or-fail rule (§8.2) makes the examples work, but the checker needs the types. Fix: add
four rows to §8.8: `source: Opt<Agent>`, `space: Opt<Space>`, `time: Time`, `ordinal: Int`.

### 7. (§12.4 l.1790) The `Opt<Agent>` explanation is wrong

"`courier` is `Opt<Agent>`, so if the spawn failed the `achieve` statement fails at once." But
`courier := spawn Courier() in inner` assigns the name at once (§8.6: "the name is known at once"),
and nothing handles `SpawnFailed`, so `courier` stays `some` after a refused spawn; the `achieve`
then fails later, by `DeliveryFailed` (§8.6 l.1125-1126), not at once. The `Opt` only covers the
period before `Initialize` has run. Fix either way: reword to that, or add
`on SpawnFailed [occurrence.child == courier] { courier := none }` (comparison on `Opt` fails when
`none`, which is what you want) and keep the sentence.

### 8. (§14 l.1852) Stale cross-reference "(h)"

"given P7 and (h)": the lettered register is gone; fair pumping is P8. Also §8.5's fragment emits
`Moved{x: x, y: y}` (l.1089) while §10 declares a std event `Moved{name}` (l.1389, §4.5): a program
that imports `std:agent` has two `Moved` types. Rename the fragment's event (`Stepped`).

### 9. (§8.5 l.1096-1098, §12.1-12.4) Is `import std:agent` needed

The text says std:agent functions and capacities are in scope "without declaring them", and §12.2
uses none of them and has no import, but §12.1, §12.3, §12.4 write `import std:agent` for
`Initialize`, `intentions()`, `is_maintain`, and `Map`. Say once: "`std:agent` is always in scope
for functions and capacities; its event and type names need `import std:agent`", or make it implicit
entirely.

### 10. (§8.1 l.947-955, §8.9) Newlines outside blocks, and a silent trap at line starts

Event, capacity, skill and goal bodies are not blocks, yet the lexer rule is context-free ("a
newline ends a statement when..."), so multi-line `capacity` bodies (§8.5, §12.3) put `NEWLINE`
tokens where the grammar has none. Say: "outside a `Block`, newlines are whitespace" (round-4 #15;
the l.953 sentence about declarations does not cover bodies). Separately, "a line that begins with
`-`, `*`... begins a new statement" means

```
let total := a
           - b
```

silently becomes two statements, the second a discarded unary minus. Add a checker warning for an
expression statement whose value is discarded and which has no action, assignment or effect.

### 11. Carried over from round 4, still open, all MINOR

- `parent`, `inner`, `outer`, `occurrence` are contextual but are `Primary` alternatives, so
  `let parent := 3` is undecided (R4 #7). Reserve all four in expressions.
- `priority IntLit` cannot be negative, so a behavior cannot order a handler after the agent's
  default-0 one (R4 #8). Allow a sign.
- `await E` without `as` is a syntax error for an event with no needed fields (R4 #16).
- Timers have a signature (l.1387) and no example: is `uses Schedules` required, what is `source` of
  the timer event, how is a held `Timer` cancelled when `none` (R4 #14). Six lines in §12.4.
- `ensures` has no name for the result (§8.8 lists none), so only a pre-state postcondition can be
  written; add `result` to §8.8 in `ensures` clauses.
- `key`, `record`, `event`, `goal`, `plan`, `stop`, `drop` are reserved everywhere, which are the
  names a programmer reaches for first for locals (the examples all avoid them: `key_id`). `key`
  matters only at the start of a `PredParam`; make it contextual.
- `plan p for G` without a parameter list means "the goal's parameters are in scope" only by a
  clause in the static-restrictions list (l.1273); say so in §5.7 or §8.9.
- `vagent run demo:depot:Warehouse(units)` has the module path and the agent name separated by the
  same `:` as module segments, and no syntax for passing an `Agent` value (§12.4 says the name "is
  passed"). Give one: `module:path/Agent`, or `--name` output feeding the next command.

## Verdict basis

The round-4 gaps are closed: `ask` is typed through `question`, parameters read, conditions are pure
transitively, and every worked example parses against the grammar and type-checks under the stated
rules. What stands in the way is a grammar hole (a skill has no `uses`, finding 1) and two points
where the register of §11 does not yet satisfy the owner's ruling: existing-code edits missing from
it (finding 2) and a fallback for P1 that is weaker than stated and is relied on by the §12.3
example without a citation (finding 3). Each is a paragraph or a few register rows. Everything else
is polish.

VERDICT: NOT CONVERGED

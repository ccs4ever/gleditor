# vagent review, round 1 — ergonomics and surface-language critic

Document: `design/vagent-agent-language.md` (1726 lines). Line numbers below are from that file.
Siblings read for consistency: `vicon-goal-directed-language.md` (§4.3-5.4, §6),
`vlace-functional-language.md` (import grammar, `let`), `vql-agent-experiment.md` (§2-3). SARL
claims are from my knowledge of SARL, not from the repo. Where I could not verify something I say
so.

Tally: 8 BLOCKING, 11 MAJOR, 9 MINOR.

## BLOCKING

### 1. (§5.5, §5.4 and §9.1, §10.5-10.6) StructLiteral versus Block is ambiguous in …

Full title: 1. (§5.5, §5.4 and §9.1, §10.5-10.6) StructLiteral versus Block is ambiguous in every
`if`, `for`, `from` and `scan` header

`StructLiteral ::= Identifier '{' [StructFieldList] '}'` (l.842) and `Block` are both legal after an
identifier. Header positions place an `Expr` directly before a `Block`:

- `for i in 0..expectedParts {` (l.1182). `expectedParts {` parses as a struct literal.
- `on TargetLogged(drone, target) from inner_context {` (l.689) and
  `on PartitionResult(...) from inner_context {` (l.1202).
- `scan packet {` (l.1529) and `scan rawSpectra {` (l.1649).
- `if cmd == "survey" {` is only safe because the right operand is a literal. `if x == y {` is not.

Rust solves this by forbidding struct literals in header position. The grammar has no such
restriction. A conforming parser cannot parse the document's own examples.

Fix: add a "no-struct" expression context for the `Expr` of `if`, `while`, `for ... in`, `scan`,
`from`, `every`, `achieve ... else` and handler guards, as Rust does. Alternatively make struct
literals unambiguous by requiring `Name { field: expr }` with at least one `ident ':'` lookahead;
that does not help for empty literals. Either way, add the rule to §5.5 as an explicit note.

### 2. (§5.5 `EmitStmt`, §5.4) `to` is both the range operator and the emit unicast …

Full title: 2. (§5.5 `EmitStmt`, §5.4) `to` is both the range operator and the emit unicast keyword,
so `emit E into S to X` mis-parses

`EmitStmt ::= 'emit' Expr ['into' Expr] ['to' Expr]` (l.781) and
`RangeExpr ::= AdditiveExpr [('to'|'..') ...]` (l.805). Each `Expr` after `emit`/`into` is greedy:

- `emit Foo into inner_context to worker` parses as `into (inner_context to worker)`, a range
  generator.
- `emit Foo to worker` parses as `emit (Foo to worker)`.

The `to` form has no example in the document and cannot be written. The sibling
`vql-agent-experiment.md` uses `into inner_context to $worker` (l.~100) and inherits the same
collision.

Fix: use a different token for the recipient filter. `emit E into S for R`, `emit E into S -> R`, or
SARL-faithful `emit E [scope]` with a predicate all work. Or demote `to` to a vicon-only generator
spelled `i to j` inside `std:goal`. Either way, `to` must not be both.

### 3. (§5.5 `CommitStmt` and `PrimaryExpr`) `commit;` inside a conjunction does not parse

`PrimaryExpr` contains `'commit'` and `Stmt` contains `CommitStmt ::= 'commit' [';']`.
`ExprStmt ::= Expr [';']` also matches. Three problems:

- The grammar is ambiguous about which production a lone `commit` is.
- The §10.3 example `check_feasibility(dest) & commit; & navigate_to(dest)` (l.1375) cannot parse.
  The `;` terminates the statement and `& navigate_to(dest)` then begins a statement with a binary
  operator. Only the `ExprStmt` reading is available, and the `;` is consumed by it.
- Where `commit` is written without a `;` (conjunct form), nothing says it succeeds and yields a
  value. A conjunct that yields nothing breaks `&`.

Fix: delete `CommitStmt`. Keep `commit` only as a `PrimaryExpr` that always succeeds, and write it
as a conjunct: `check_feasibility(dest) & commit & navigate_to(dest)`. In a block it is an ordinary
`ExprStmt`. Remove every trailing `;` after `commit` that sits inside a conjunction (l.1375, 1631,
1640).

### 4. (§10.3 and §10.6 `PlanDirectPath`, `PlanRidgeBypass`, `TraverseRidge`) The worked …

Full title: 4. (§10.3 and §10.6 `PlanDirectPath`, `PlanRidgeBypass`, `TraverseRidge`) The worked
examples do the opposite of what their comments claim

`commit` is `#Cut`: "Prunes remaining alternative candidate plans on the current choice rank", and
`<-` becomes permanent (§10.3, l.1383-1385). The examples place `commit` before the fallible action:

- `TraverseRidge` (l.1375): `check_feasibility(dest) & commit; & navigate_to(dest)`. The comment
  says "if navigation fails, `<-` updates are automatically undone!". They are not: `commit` already
  made them permanent.
- `PlanDirectPath` (l.1629-1632): `(battery) & { energyReserve <- ... ; commit } & drive(loc)`. If
  `drive` fails, `PlanRidgeBypass` is never tried, because the `|` alternative was cut. The §10.6
  comment (l.1623) says the opposite, and `TraverseToTarget` is defined by
  `PlanDirectPath | PlanRidgeBypass`.
- Even if the cut did not prune, the 15 units of energy already deducted would stay deducted. Ridge
  bypass would then debit a further 35.

This is a design smell, not only a typo. The natural reading of `&` then `commit` then action puts
the commit on the wrong side of the failure point every time. Reserving state and then acting is
exactly the order a programmer writes.

Fix: (a) rewrite the examples so `commit` comes after the last fallible step, `drive(loc) & commit`.
(b) Decide whether `commit` is the right primitive for staging. A scoped form `commit { ... }` or
`atomic { ... }` that prunes only on block success is far less error-prone than a free-standing cut.
Prolog's `!` is the same trap and is famous for it. (c) Add an example showing a failed action
restoring `<-` state, since that is the headline feature.

### 5. (§5.5, §10.5, §10.6) `import std:agent;` is not in the grammar, and the three …

Full title: 5. (§5.5, §10.5, §10.6) `import std:agent;` is not in the grammar, and the three
module-path spellings disagree

- `ImportDecl ::= 'import' (Identifier | StringLiteral) ...` (l.711). `std:agent` is neither: it is
  three tokens with `:`.
- `ModuleDecl` uses `QualifiedIdent` with `::` (`module mars::exploration`, l.1565, matches the
  grammar).
- `vicon` writes `std:goal`, `vlace` writes `import "std:xanadu"` as a string (vlace l.1407,
  `import_decl` l.412), and VQL/VQL-agent write `import std::agent;`.
- After import there is no qualified use. `Type`, `Pattern`, `SpawnStmt` and `Expr` take a bare
  `Identifier`, so `std::math::Point` or `mars::Foo` cannot be written anywhere. `QualifiedIdent` is
  used only in `ModuleDecl`.

Fix: pick one spelling for the family. I recommend the string form `import "std:agent"` (vlace) or
`::` throughout. Make `QualifiedIdent` the reference form in `Type`, `Pattern`, `SpawnStmt` and
`PrimaryExpr`. State name resolution (imports bring names into scope; selective list
`import "std:math" (Point, distance)`).

### 6. (§5.5 `SpawnStmt`, `AchieveStmt`, `EmitStmt`) `spawn` is a statement but is used …

Full title: 6. (§5.5 `SpawnStmt`, `AchieveStmt`, `EmitStmt`) `spawn` is a statement but is used as
an expression

`let child = spawn PatrolDrone into inner_context` (l.683) needs `spawn` to be an `Expr`
(`StateDecl ::= 'let' ... '=' Expr`). Grammar: `SpawnStmt` is a `Stmt` only. The same applies to
`achieve ... else` when its success should be consumed (§10.1 diagram), and to `setSkill`.

Also `workerPool.push(child)` and `AgentId` have no definition (`[AgentId]` is listed as a type in
l.678; `AgentId` is declared nowhere). `Lifecycle.spawn` in §8.2 returns `AgentId` but is shadowed
by the keyword.

Fix: move `spawn` (and `emit`, which should return an `EventId` or `()` for symmetry) into
`PrimaryExpr` as expression forms returning `AgentId`. Define `AgentId`, `SpaceId`, `ContextId`,
`TimerId` as built-in opaque types in a "Built-in types" subsection.

### 7. (§5.5 `Pattern`/`PatternField`, §10.5) Named-field pattern and typed binder collide

`PatternField ::= [Identifier ':'] (Identifier | Literal | '_')` (l.839) means `name: binder`. The
example `on RawTelemetryPacket(packet: String)` (l.1528) therefore parses as "field `packet`, bound
to a variable named `String`", not "binder `packet` of type `String`". The handler
`Initialize(args)` has no type at all.

Also: positional binding, `on TargetDetected(targetId, dist, coords)`, binds by declaration order of
the event's fields, not by name. Reordering fields in the `event` silently rebinds `dist` to
`coords`. Event field types are never checked against the handler.

Fix: make binding by field name the default, consistent with the `StructLiteral` the emitter writes:
`on TargetDetected { targetId, distance as dist } if dist < 50.0`. Drop positional binding, or keep
it only for single-field events. Disallow `: Type` in patterns, since the type is the event's.
Specify that the pattern `Identifier` is the event type and is statically resolved.

### 8. (§5.2, §5.5 `Type`) Several examples are invalid under the grammar's own type and …

Full title: 8. (§5.2, §5.5 `Type`) Several examples are invalid under the grammar's own type and
member productions

- `-> ()` (`stop`, `halt`, `Lifecycle.kill`) is used throughout, but
  `TupleType ::= '(' Type {',' Type} ')'` (l.849) needs at least one `Type`. Unit cannot be written.
- `[String; [String]]` as a map type (l.1177). `ArrayType ::= '[' Type [';' IntegerLiteral] ']'`. A
  map is not expressible, yet `.insert`, `.get`, `.set`, `.append`, `.len()` and `.push` are all
  called on built-ins that nothing defines.
- `CapacityMember ::= MethodSig ';'` (l.722) requires a semicolon, but every example omits it
  (l.593-597, 1226-1228), contradicting both the grammar and §5.1's "semicolons are optional".
- `ModuleDecl` and `ImportDecl` require `;` while everything else makes it optional (and the
  examples are inconsistent, `module mars::exploration;`).
- `plan TraverseRidge` (l.1369) is at top level, but `PlanDecl` is only an
  `AgentMember`/`BehaviorMember`.
- `/* ... */` (l.1590) is used but no comment lexing is specified.
- A bare `{ ... }` as a conjunct (l.1629) is legal via `Block` as `PrimaryExpr`, but see finding 9
  on what it yields.

Fix: define `Type ::= ... | '(' ')'` (unit), a `Map`/`Set`/`List` generic form
(`Map<String, [String]>` or `{K: V}`), and a normative "Lexical rules" subsection (comments,
newlines, `;`, identifiers, numeric literals, string escapes). Run every example through the grammar
by hand, or better by `vagentc --parse`, before the next round. I count at least a dozen mismatches
above.

## MAJOR

### 9. (§5.1, §5.5) "Semicolons are optional" with no newline rule creates call-continuation bugs

`PostfixExpr` accepts `'(' ArgList ')'` after any primary. In §10.6 (l.1637-1638):

```
let waypoint = Point(loc.x + 10.0, loc.y + 5.0)
(batteryLevel() > 35.0) & { ... }
```

Without a newline-terminates-statement rule the second line is a call suffix on the first
(`Point(...)(batteryLevel() > 35.0)`). The same hazard is `[`-indexing after a line ending in an
expression, and `{ ... }` after a `)`. JavaScript, Kotlin and Swift each carry a specific rule for
this.

Fix: specify automatic statement termination (Kotlin/Go style: a newline ends a statement when the
last token can end one, and a line starting with `(`, `[`, or a binary operator continues). State
that a leading `(` or `[` never continues a statement, or require leading-`.`/operator continuation
explicitly.

### 10. (§5.1, §10.2, §5.5) Booleans versus success/failure: the document uses both with no bridge

Vicon is clear: comparisons succeed with a value or fail (vicon §5.1). vagent says plans use
"success or failure rather than boolean tests" (l.1340), yet:

- Capacity methods return `Bool` (`drive(loc) -> Bool`, `move`, `sendPayload`). In
  `... & drive(loc)` does a returned `false` fail the conjunction? `false` is a success carrying the
  value `false`. The example comment ("succeeds if wheels achieve traction", l.1586) assumes false
  means failure.
- `if x {}` / `while x {}` / `if dist < 50.0` in handlers use the boolean reading.
- There is no `&&`, `||`, `and`, `or`, or `not`. `!` is unary negation, but in vicon `!x` is the
  bang generator. `&` and `|` are the only connectives, and they mean conjunction/alternation of
  generators. A reader writing `if a > 1 | b > 2` gets a two-branch generator with a possible second
  resume, not an or.
- Guards `if Expr` on handlers: is the guard "first result is truthy" or "succeeds"?

Fix: add a §5.6 "Truth and failure". Pick one rule. My recommendation: comparison and
`Bool == false` both fail in a goal-directed context (a `Bool` result is lifted: `true` succeeds
with `()`, `false` fails), `if`/`while`/guards take the bounded first result, and `&`/`|` are
explicitly the goal-directed forms. Provide `&&`/`||`/`not` for plain Bool computation, or state
they are deliberately absent and show the rewriting `if (a > 1) | (b > 2)`. Rename or remove unary
`!` to avoid vicon's bang.

### 11. (§5.5 expression ladder) Precedence differs from Icon/vicon and the ladder is surprising

Vagent's ladder, high to low: `scan`/unary, `* / %`, `+ -`, `to ..`, `\`, relational, `&`, `|`,
`<-`, `= += -=`.

- Vicon and Icon bind `\` tighter than arithmetic (it is a postfix-like limit on a generator). Here
  `retry() + 1 \ 3` limits the sum, and `i to j \ 3` limits the range, while `x \ 3 < 5` is
  `(x \ 3) < 5`. §10.2 prints `\ n` with a space, which looks like a unary form.
- `scan` sits at multiplicative-operand level, so `scan s { ... } & drill()` and `a * scan s {...}`
  are both legal.
- `a | b & c` is `a | (b & c)`, the boolean convention. In Icon `&` has the lowest precedence, so
  `a | b & c` is `(a | b) & c`. Writing `PlanA() | PlanB() & cleanup()` means opposite things in the
  two languages while `std:goal` is meant to be the same machinery (vicon §5).
- `<-` is below `|` and `&`, and `=` below that. `x <- a & b` assigns the conjunction. `a & x <- b`
  is `(a & x) <- b`, an invalid LHS. A plan whose guard is a conjunct that then reverses an
  assignment must wrap parentheses. This is the exact shape of §10.6 (`(battery) & { ... }`) and
  will recur.
- `<-1` lexes as `<-` then `1`: `if dist <-5.0` is a reversible assignment. Maximal munch; Rust
  removed `<-` for this reason.
- `0..4`: `0.` can lex as a float prefix. `..` inclusive/exclusive is never stated, and `to` is
  inclusive in vicon; two spellings of one operator with possibly different bounds.

Fix: (a) bind `\` as a postfix on `PostfixExpr` as in vicon; (b) adopt Icon's order for `&`/`|` or
state the deliberate reversal and the cost; (c) put `=`, `<-` at the same level as one
right-associative assignment and require the LHS to be an lvalue grammar production
(`Place ::= Identifier {FieldSuffix|IndexSuffix}`) so the mis-parses above are syntax errors; (d)
require whitespace or parenthesis for `<-` followed by a sign, or spell it `:=`-style as vicon does
(`x <- e` is Icon's reversible assignment, `:=` is the irreversible one); (e) pick one range token
and state its bounds.

### 12. (§5.2, §5.3, §3.1 item 9, §6.1) Contracts, the goal/plan link, and events …

Full title: 12. (§5.2, §5.3, §3.1 item 9, §6.1) Contracts, the goal/plan link, and events lifecycle
have no syntax

- The ten-entity list and the lowering table promise `maintain` (`-d.contract`) and `ensure`
  (`+d.contract`) on goals and pre/post-conditions on capacity methods (§4.2), but
  `GoalDecl ::= 'goal' Identifier '(' ParamList ')' Block` and `MethodSig`/`MethodDef` have no
  `requires`/`ensures`/`maintain` clause. §10.4 even says an intention re-verifies its `maintain`
  contract on resume. Nothing can be written that does so.
- `PlanDecl` names no goal. In §10.6, `goal SurveyTarget` is a body that `achieve`s a conjunction of
  two *plans* called by name, and `plan TraverseToTarget` is called like a function. §10.1 describes
  "plan selection on `+d.choice` among candidates of a goal" (Jason-style `+!g : ctx <- body`), but
  there is no `plan P for G` and no way to add a second alternative to a goal from elsewhere.
  `where` on plans (guard) is never used in an example.
- `achieve G` inside `goal G` of the same shape reads as recursion; the trigger
  `on MissionCommand ... { achieve SurveyTarget(...) }` and the goal body `achieve A & B else {}`
  use `achieve` for two different things (invoke a goal versus run an expression with a fallback).
- `abort intention;` takes no target (IntentionStmt has `[Expr]`, but no way to obtain an intention
  handle). `achieve` returns nothing, so there is no handle. `suspend`/`resume` are unusable.
- Built-in events (`Initialize`, `Destroy`, `HolonGoalFailed`, `IntentionFailed`) are used in
  handlers but declared nowhere: not in §8, not in the grammar. Their payload types are unknown
  (`Initialize(args)`).

Fix: add `GoalDecl ::= 'goal' Id '(' ParamList ')' { 'maintain' Expr | 'ensure' Expr } [Block]`; add
`PlanDecl ::= 'plan' Id 'for' GoalRef '(' ParamList ')' ['where' Expr] Block` so candidate plans
attach to the goal (and `d.choice` is built from them); add `requires`/`ensures` clauses to
`MethodSig`/`MethodDef`; have `achieve` return an `IntentionId`; add a `std:agent` events table
(Initialize, Destroy, IntentionFailed, HolonGoalFailed, Timer) with payload types.

### 13. (§5.3, §5.4, §8.2) `uses`/`requires`/mount versus SARL, and `setSkill` as a pseudo-call

- In SARL `uses Cap` imports a capacity's methods into the agent or behavior scope and the skill is
  installed separately (`setSkill(new Impl)`). In vagent `uses ThrusterNavigation as Navigation`
  *installs* a skill. The word keeps its keyword but not its meaning. `requires` (a SARL feature of
  skills, as I remember it, to declare needed capacities) is used here on agents and behaviors, so a
  SARL reader reads both backwards. I could not check SARL's grammar from the repo; flag for the
  owner.
- The doc says it follows SARL ("Where SARL established ...", §5 table) but never lists what it
  changed. SARL `[guard]` in brackets becomes `if guard`; `occurrence.field` becomes positional
  destructuring; `emit(e)[scope]` becomes `emit E into S to R`;
  `in`/`wake`/`fires`/`install`/`uninstall` are absent.
- `setSkill(NewSkill as Cap)` (l.783) looks like a call but `as` inside parentheses is not an
  expression and the leading lower-camel name is reserved. Compare VQL-agent's
  `swap Cap with NewSkill;`. Every other statement is a space-separated keyword form.
- Skills are named in `uses` by type, so `setSkill(SwarmTransport as Transport)` takes a skill
  *type*; no way to pass construction parameters or an instance with state
  (`LoopbackTransport(buffer: 8)`).
- Bare `stop()` / `move()` call capacity methods with no qualification. Two required capacities with
  the same method name (`halt` on both Mobility and a hypothetical Brake) have no disambiguation
  syntax. SARL has the same issue but offers explicit `getSkill(Cap).m()`; vagent offers nothing.
- Behavior mounting inside an agent is a declaration (`behavior ObstacleAvoidance`), but SARL's
  behaviors are registered at runtime with parameters; here no constructor arguments, and
  `attachBehavior(b: Behavior)` in §8.2 takes a `Behavior` value with no way to make one.

Fix: either rename (`install X as Cap` for the declaration, `uses Cap` as SARL's import-of-methods)
or add a "Differences from SARL" table. Replace `setSkill(...)` with `swap Cap with Skill(args)` or
`install Skill(args) as Cap`. Add `Cap.method()` qualification. Give `behavior B(args)` parameters.

### 14. (§8.2) The standard library cannot be written in the language because its names are keywords

`Lifecycle.spawn`, `DefaultSpace.emit`, `Schedules.every`, `Schedules.in`: `emit`, `spawn`, `every`,
and `in` (`for x in`) are reserved words in §5.5 (`every` as `EveryStmt` and `in` in `ForStmt`).
`std:agent` declares `def emit(event: Event) -> ()` so the module the doc defines does not parse as
vagent. `DefaultSpace` is also a capacity name in §8.2 and a space value in
`emit X into DefaultSpace` (l.648, 659); the VQL sibling uses `default_space`. SARL names the
capacity methods `emit`, `wake`, `in`, so the shape is intentional, but the language chose keywords
for the same operations.

Timers: `every(intervalMs, event) -> TimerId` returns an id and nothing cancels it; there is no
`cancel(TimerId)`, no `wake`, and no access to current time. Replay determinism (§11.1) requires
time and randomness to be a capacity and not an ambient call.

Fix: either keep the keyword forms and drop §8.2's duplicate method names, or make the statements
sugar over the capacity calls and rename the keywords. Specify `Schedules`:
`after(delay, event) -> TimerId`, `every(period, event) -> TimerId`, `cancel(TimerId)`, and a `Time`
capacity (`now()`) plus `Random` so replay is deterministic. Rename the `DefaultSpace` capacity or
the `DefaultSpace` value.

### 15. (§5.5 `Stmt`/`Block`, §5.1 "blocks yield values") Block semantics under …

Full title: 15. (§5.5 `Stmt`/`Block`, §5.1 "blocks yield values") Block semantics under backtracking
are unspecified

The doc says "expressions are first-class, and blocks yield values" (l.575), but
`Block ::= '{' {Stmt} '}'` has no tail expression: the last `ExprStmt` yields or not by an unstated
rule. `IfStmt`, `ForStmt` and `WhileStmt` are statements, so `let x = if c { a } else { b }` is
impossible, contradicting "expressions are first-class". In a generator, does `{ s1; s2 }` backtrack
into `s1` when `s2` fails? Vicon §4.4 says statement-level expressions are *bounded* (a discard at
the boundary). If so, a `<-` in an earlier statement is trailed against the enclosing mark but its
generator is gone, and every `Block` conjunct in §10.6 is atomic with respect to `|`. If not,
statement ordering is a conjunction and `;` is `&`. The `commit`/`<-` story rests on this and is not
stated.

Fix: add a §5.6 that states: statements in a Block are bounded; Block value is the last `ExprStmt`
(or `()`); failure of any statement fails the Block; `if`/`while`/`for` are expressions (make
`IfStmt` an `IfExpr`, matching `ScanExpr`'s own placement); `;` versus newline versus `&`
differences. Show the 10.6 plan lowered to an explicit mark/discard sequence.

### 16. (§5.2, §5.4, §9.1) Context versus space: `into inner_context` mixes two kinds …

Full title: 16. (§5.2, §5.4, §9.1) Context versus space: `into inner_context` mixes two kinds, and
ambient names are magic

`emit ... into inner_context` and `from inner_context` use a *context*, while §5.4 says
`into SpaceExpr`. `inner_context`, `outer_context`, `DefaultSpace`, `self`, `args` appear as unbound
identifiers: none is declared by a grammar rule or `std:agent`. `self.id` and a bare `targetsSeen`
are both used for agent state (l.644, 658 versus 1630); inside a skill, bare `pos`.
`SpaceDecl ::= 'space' Id ['is' Id] ['{' FieldList '}']` is never used in an example, and the `is`
clause has no semantics. §9.1 says "across a swarm space" but declares none, so the
distributed-vs-local choice of §7.4 is never shown at the surface. `from Expr`'s meaning (filter on
source context? space? agent?) is not defined.

Fix: define the ambient bindings in a "Scope and names" subsection (`self`, `self.id`,
`defaultContext`, `innerContext`, `outerContext`, `defaultSpace`). State that a context coerces to
its default space in `into`. Specify that state fields are accessed bare and `self.` is an optional
disambiguator (Kotlin/SARL rule), and show `space`, an explicit `swarm space Alerts` declaration,
and a `join`/`leave` statement in at least one example.

### 17. (§9.1, §9.2) Example correctness and idiom

- 9.1 `expectedParts * 2` hard-codes the worker's return length (`scanPartition` returns 2 items
  each); an aggregator that finishes on count of hits instead of count of partitions is a bug, not a
  simulation. It also tracks no set of received partitions, so duplicate delivery doubles the count.
- 9.1 `let mut currentHits = pendingQueries.get(qId)` then `.append` then `.set` suggests values are
  copied (correct for a manifold) but nothing says so. `pendingQueries.insert(qId, [])`, `.get`,
  `.set`, `.len()`, `.append` and `idx.toString()` are invented.
- 9.1: the `WorkerDrone` is spawned without `Initialize` and replies `into outer_context`: meaning
  "the holon's inner context as seen from the child" is never explained, yet it is the central holon
  idiom.
- 9.2 `emit SecurityModeUpgraded { ... }` is an undeclared event (l.1268), and
  `SecurityAlert(level) if level > 8` re-fires `setSkill` on every alert, with no guard that the
  swap is already done. A reader will copy this.
- 9.2 never shows that a `DataReporter` call after the swap reaches the new skill; no output is
  shown.
- `LoopbackTransport`/`SwarmTransport` return `true` from empty bodies: they demonstrate nothing
  about swap.

Fix: replace `expectedParts * 2` with a received-partition set; declare every event used; add a
before/after trace to 9.2.

### 18. (§5.1, §5.5 versus vicon, vlace, VQL) Sibling-language inconsistency table is …

Full title: 18. (§5.1, §5.5 versus vicon, vlace, VQL) Sibling-language inconsistency table is
missing and the gaps are gratuitous

Vagent claims to share vicon's machinery (`std:goal`, l.1283). Surface differences, with no stated
reason:

| Concern         | vicon (l.)                     | vagent (l.)                                       | other siblings                    |
| :-------------- | :----------------------------- | :------------------------------------------------ | :-------------------------------- |
| assign / revers | `:=` / `<-` (245-249)          | `=` / `<-`                                        | vlace `let =`                     |
| conditional     | `if e then e else e`           | `if e { } else { }` (stmt)                        | vlace `if..then..else`            |
| loop            | `every e do e`, `while e do e` | `every e do { }`, `while e { }`, `for x in e { }` |                                   |
| scan            | `s ? e`, `tab`, `move`         | `scan s { }` (block)                              |                                   |
| module path     | `std:goal`                     | `std:agent`, `mars::x`                            | vlace `"std:xanadu"`, VQL `std::` |
| guard keyword   | n/a                            | handler `if`, plan `where`                        | VQL-agent `where` for both        |
| not / bang      | `!x` bang generator            | `!` unary not                                     |                                   |

`every e do { }` is `do` plus braces, both of which delimit the body; `for` and `every` overlap
(`for x in gen` is `every x := !gen do`). Using `=` for assignment while comparing with `==` and
defining with `let x = ...` makes `a = b` (assign) read as equality to Rust/Kotlin and Icon
programmers and obscures the reversible/irreversible pair that the design is built on.

Fix: add a "Relationship to the siblings" table: either adopt the vicon spelling for the shared
constructs, or state each divergence with a price. At minimum use `:=` for irreversible assignment
so the `<-` / `:=` pair matches Icon and vicon, use one guard keyword, and drop `do` when a braced
block follows.

### 19. (§10.2, §10.5) Examples in prose use syntax the grammar does not have

- `every $sample := identify_targets() do collect_sample($sample)` (l.1352): `$` identifiers and
  `:=` are not in §5.5. This is vicon/VQL syntax pasted into the vagent section.
- `retry_connect() \ 3` fine, but `\ n` in the heading is a unary look.
- `tab(many(" "))`, `move(1)`, `match`, `upto` (§10.5) come from `std:goal` (vicon l.346-348, 392)
  but §10.5/10.6 `import std:goal;` lists no names and §8 does not mention them; `scan` bodies are
  statements with `let header = tab(...)` where a failed `tab` should backtrack but the statement is
  bounded (finding 15).
- `scan x { ... commit; emit ... }` inside a reactive handler (§10.5): there is no enclosing plan,
  so what does `commit` cut? A handler is turn-atomic (§4.3), so `commit` there is either a no-op or
  a surprise.

Fix: rewrite the §10.2 example in vagent syntax; add a one-paragraph "std:goal names visible in
vagent" list; state what `commit` does outside a plan (recommend: a static error).

## MINOR

### 20. (§5.5 `RelOp`) No string, set or membership operators; `+` on strings

`"match_alpha_" + idx.toString()` uses `+` for strings and `.toString()` on `Int`, but no operator
or method table exists. `==` on events, cells and lists is undefined. Add a "Built-in operations"
table (or cite `std:math`/`std:string`).

### 21. (§5.2, naming) Mixed naming conventions

Capacities use camelCase (`batteryLevel`, `sampleSpectra`, `attachBehavior`), while plan bodies use
snake_case (`check_feasibility`, `navigate_to`, `collect_sample`), types are PascalCase, keywords
are lowercase, and `setSkill` is camelCase keyword. Pick one (snake_case matches Rust and vicon;
camelCase matches SARL). Document it.

### 22. (§5.5 `HandlerDef`) Priority has no defined default or tie rule and uses a naked constant

`with priority IntegerLiteral`: default? ties? Descending? `CRITICAL_THRESHOLD` (l.1303) is never
defined and is a naked magic number in the language semantics; per `AGENTS.md` (architectural
governance) it should come from `system://settings`. SARL has no priority at all, so name this as an
extension.

### 23. (§5.2) Events lack SARL's base fields

No `source`, `timestamp`, `id` or `occurrence` access, so `on E { reply to e.source }` cannot be
written, and a holon cannot tell which child emitted `PartitionResult`. Add implicit fields
`source: AgentId`, `at: Time` and a way to bind the whole event (`on e: Foo(...)`). Add event
`extends`.

### 24. (§5.2) Skill `install`/`uninstall` hooks and `fires` missing

Stateful skills (`ThrusterNavigation` with `let mut currentSpeed`) need an `install`/`uninstall`
hook, and the swap example needs to say what happens to the old skill's state. Not an essential, but
SARL users expect it.

### 25. (whole document) No entry point and no testing story

There is no `main`, `launch`, or boot-agent form, so a program cannot start. The determinism claim
(§11.1) would make testing straightforward and the doc does not use it. Suggest:
`test "name" { given spawn A; send E {...}; expect emitted F {...}; expect state a == 1 }` run on a
single `ArenaManifold` with a manual clock, mock skills via `swap`, and `assert` statements. Also:
no `try`/`trap` handling for `#Trap` -> `IntentionFailed` (§10.2).

### 26. (§5.2) Beliefs and queries

BDI beliefs are `let mut` fields. The doc claims BDI but offers no belief queries, no
event-triggered goals (Jason `+!g`/`+b`), and no belief-change events. The sibling VQL could supply
them, but the surface never says so. At least say beliefs are plain fields and non-reactive in v1.

### 27. (§5.1) Token-level details to pin down

`~` is unary with no meaning in the doc; `%` and `-=` have no semantics; numeric literal forms
(`1_000`, `0x`), string escapes, `\` versus escape, identifier charset, and keyword list are not
enumerated. A reserved-word list (about 40 words) belongs in §5.1.

### 28. (§5.5 `ImportDecl`) Import list and `as` both on one import

`import std:math (Point, distance)` places the list after the path; `as` precedes it in the grammar
(`'as' Identifier ['(' IdentList ')']`). Combined use is unspecified. Align with vlace's
`import_name` rules.

## Verdict basis

The shape (SARL's tripartite split, `on E`/`emit`/`uses`) is easy to read and the Rust/Kotlin-like
surface is a sound choice. The grammar as written cannot parse its own examples (findings 1, 2, 3,
5-8), and the headline BDI feature, reversible assignment plus `commit`, is shown being used
backwards (finding 4). Booleans versus failure, block semantics, and the goal-to-plan link are
unspecified (10, 12, 15). These are fixable, but they are BLOCKING for a document that presents
itself as a specification and a formal grammar.

VERDICT: NOT CONVERGED

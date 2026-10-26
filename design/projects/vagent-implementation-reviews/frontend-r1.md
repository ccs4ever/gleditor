# Frontend iteration 1: independent parser/checker audit

Verdict: **revise before passing the frontend quality gate**. This audit is read-only with respect
to production source. Findings below were reproduced through the current `build/vagent check -` CLI,
whose binary modification time was newer than `checker.cpp` at the time of the probes. All commands
used the six headless/XDG environment settings. Sources are complete units unless explicitly
described as a source family. The four already-known builtin-name, relational ordering,
inherited-signature and standard-import issues are not counted again here.

The parser's syntax tests remain 42/42 passing. That evidence covers syntax, not the independent
static-semantics failures below. No Vortex compiler or actor execution is inferred from a successful
frontend check.

## Major: successful negation exports an impossible binding

This invalid branch is accepted with exit 0:

```vagent
predicate p(value: Int)
event Tick
agent A {
  var result: Int := 0
  on Tick { if not believes p(?x) then result := x }
}
```

`not e` succeeds when `e` fails (§8.2). Its speculative query/unification cannot supply a live
binding to `x` on that success path. `Checker::infer` for `Unary` instead infers the child using the
caller's environment, so `term` inserts `x` and the enclosing `if` sees it. Infer negation in an
isolated environment and export no bindings from it. Preserve reads of already-bound variables.

## Major: inheritance-cycle validation revisits every diamond path

The recursive `cycle` lambda in `Checker::declaration` has an active set but no completed-node
cache. It traverses shared ancestors again on every branch and starts again for every declaration. A
valid acyclic input of only 1,997 bytes timed out after three seconds:

```text
capacity C0 {}
for each i from 1 through 23, append these declarations:
  capacity Li extends C(i-1) {}
  capacity Ri extends C(i-1) {}
  capacity Ci extends Li, Ri {}
```

The names above denote generated identifiers such as `L1`, `R1`, `C1`; the `for` is a generator
recipe, not vagent syntax. Every inheritance edge has a valid capacity target and there are no
cycles or duplicate bases. Path count doubles at each diamond, so ordinary parser token/depth limits
do not bound this check adequately. Use linear-time graph validation with active/completed colors
(preferably iterative to bound C++ stack use) and reuse completed results. Keep cycle and
invalid-base diagnostics, rather than silently pruning actual cycles.

## Major: pure inherited capacity implementations are missed

This valid use of a pure inherited method is rejected as `ImpureCondition`:

```vagent
event Tick
capacity Base { def value(): Int }
capacity Derived extends Base {}
skill Impl implements Derived { def value(): Int { 1 } }
agent A {
  uses Base
  install Impl()
  var count: Int := 0
  on Tick { if value() > 0 then count := 1 }
}
```

Installing a skill exposes its implemented capacities including inherited capacities (§5.3).
`Checker::function` gathers the used capacity's inherited signatures, but its purity scan matches
skills only when `skill.names` directly contains the used capacity name. `Impl` directly names
`Derived`, so `Base` appears to have no implementation and its pure method is marked impure. Resolve
skill implementation coverage through the capacity ancestry closure; preserve conservative purity
over every applicable implementation, including dynamic replacements.

## Major: asserted wildcard facts are accepted as ground

This nonground initial belief is accepted:

```vagent
predicate p(value: Int)
agent A { believe p(?_) }
```

The `LogicVariable` branch of `Checker::term` skips `_` before checking `bind`. The plain `_`
identifier branch correctly rejects a wildcard when assertions must be ground, but the explicit
logic-variable spelling bypasses that check. Reject both wildcard spellings for `believe`, `revise`,
initial beliefs and `tell`; continue allowing them in matching/forgetting queries.

## Minor: a failing alternation arm incorrectly removes definite bindings

This valid condition is rejected because `x` is reported unknown:

```vagent
predicate p(value: Int)
event Tick
agent A {
  var result: Int := 0
  on Tick { if (believes p(?x) | fail) then result := x }
}
```

Every successful result binds `x`: the other arm can never succeed. The alternation branch merges
only bindings present in both environments before its bottom-type handling. Merge definite bindings
over successful arms; an arm known to be `fail`/bottom contributes no successful path. Do not expose
a binding when a genuinely successful alternative leaves it unbound.

## Minor: intentions list scope is mistaken for a call-site restriction

This read-only helper is rejected as `InvalidPosition`:

```vagent
event Tick
agent A {
  var count: Int := 0
  def how_many(): Int { *intentions() }
  on Tick { count := how_many() }
}
```

The §10 function table says `intentions()` returns top-level intentions and that dropping one covers
its subgoals. It does not prohibit reading that list from a pure helper or an answer.
`Checker::call` instead requires the syntactic body to be Handler or Plan. Remove that unrelated
body restriction; keep the returned collection limited to top-level intentions in lowering/runtime.
If an actual call-site restriction was intended, that requires an explicit specification change.

## Minor: integer-minimum validation depends on literal spelling

This valid minimum Int is rejected:

```vagent
agent A { var value: Int := -0x08000000000000000 }
```

Its magnitude is exactly 2^63. The lexer accepts the spelling, but `Unary` recognizes the signed
minimum through three exact strings after stripping underscores. Equivalent hexadecimal or decimal
spellings with leading zeros miss that special case and are passed to positive-Int validation. Parse
the unsigned magnitude/radix and compare the value to 2^63; do not compare spellings. Keep positive
2^63 and negative magnitudes larger than it rejected.

## Minor: new query/await bindings bypass name-case validation

A query binding `?Value` is accepted even though value bindings must use snake_case (§8.1):

```vagent
predicate p(value: Int)
event Tick
agent A { on Tick { if believes p(?Value) then () } }
```

The new-variable branch of `Checker::term` inserts the name without applying `snake`; event-await
bindings are likewise inserted directly, while ordinary handler `as` bindings are checked. Apply the
common value-binding validation at query and await binding introduction, retaining `_` as the
special anonymous match where allowed.

## Specification/API gap: effect-call recovery has no production

With `Sensor.read` declared as an effect returning Int with a deadline, this plan statement fails
parsing at `else`:

```vagent
event Tick
capacity Sensor { effect def read(): Int within 1s }
native skill Host implements Sensor
goal Work() perform
agent A {
  uses Sensor
  install Host()
  on Tick { adopt Work() }
  plan work for Work { let value := read() else 0 }
}
```

Section 6.6 includes effects in the deadline table and says an `else e` supplies recovery after a
wait fails, but §8.9 gives recovery only to `achieve`, `ask` and `await`; ordinary postfix calls
have no `else` production. The dangling-else rule also omits effect calls. Current parsing matches
the grammar: `if c then x := read() else x := 0` attaches `else` to the `if`, not the effect call.
Naively attaching `else` to every Call would steal existing outer-if branches from ordinary calls.

A consistent language/AST contract must resolve this before effect lowering is considered complete:
identify how effect-call recovery is spelled and how it is disambiguated from outer `if` recovery.
Do not silently drop the promised recovery, and do not pretend the parser knows a call's effect
status before name/type resolution. No syntax change was made by this audit.

A related edge worth explicitly deciding is `x <- read()` in a plan: the checker currently accepts
it, while §6.6's specified assignment wrappers name `:=` and `let`, not reversible assignment. The
front end and lowering must share an explicit permitted-wrapper rule; the audit does not assume an
unlisted cross-turn reversible statement has been specified.

## Controls that behaved correctly

- Read-only answers reject transitive field-writing helpers, including multi-hop call chains.
- An effect argument containing bounded `:=` is rejected as an invalid speculative position.
- Optional list-element assignment accepts `none` using the stored optional type rather than the
  unwrapped read type.
- Explicit subsets of goal parameter bindings expose those named parameters correctly.
- `std:goal` aliases with selected exports resolve their selected scanning functions.
- The AST recursion and constructed-tree bounds return typed parser diagnostics in the syntax
  battery, and malformed grammar does not escape through the public parser API.

The known standard-import alias/export and builtin-type shadowing findings were independently
reproduced but are left with their existing owner. A successful check of either is not evidence that
module loading or typed record lowering is complete.

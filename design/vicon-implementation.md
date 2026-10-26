# vicon implementation contract

Working contract for the staged implementation of
[`vicon-goal-directed-language.md`](vicon-goal-directed-language.md). It pins the emission idioms,
file layout, and public interfaces so the pieces can be built in parallel without design drift.
Where this file and the spec disagree, the spec wins on semantics and this file wins on concrete
mechanism — it exists because the spec names constructs but not opcode graphs.

## File layout (§8.2 of the spec)

```
apps/common/xanadu/vicon/
  token.hpp          lexer.{hpp,cpp}
  ast.hpp            parser.{hpp,cpp}
  compiler.{hpp,cpp} driver.{hpp,cpp}
apps/vicon/main.cpp      // REPL + batch runner, mirrors apps/vprolog/main.cpp
apps/viconc/main.cpp     // standalone compiler, mirrors apps/vlacec
tests/xudu/vicon_{lexer,parser,compiler,driver,e2e}_test.cpp
```

`apps/common/xanadu` and `tests/xudu` are glob-picked by the Makefile; only the two new app targets
need explicit rules.

## Runtime model

A compiled program is a graph of opcode cells chained on `+d.spin`, executed by one cursor
(`VortexVM::spawnCursor`). Every expression is a generator:

- **Producing** = writing the result into a designated result cell and jumping to a shared `Halt`
  opcode (`haltOp`). `VortexVM::run` stops when the cursor PC reaches a `Halt` without executing it
  — that *is* Icon's suspension.
- **Resuming** = `vm.backtrack(cursor)`: pops the top `ChoicePoint`, `release()`s its `Mark`, and
  lands the PC on `altOp`. The generator's `altOp` produces the next result and reaches `haltOp`
  again.
- **Failing** = `backtrack` finds no choice point and the VM's `#Fail` returns `!success`; the
  driver reports `std::nullopt`.

The driver boundary is the only place results leave the machine; inside a run, backtracking consumes
nested generators inline. `Driver::resume()` is therefore roughly
`vm.setCursorOpcode(cursor, failOp); vm.run(cursor)` and read the result cell on `"Halted"`.

### The retry idiom — one `altOp` reused per generator

Each generator emits `#Choice` once at entry and re-pushes the *same* `altOp` on each production, so
a resume consumes the frame and immediately replaces it:

```
entry:   Choice  NEXT            // CP{altOp = NEXT, mark}
BODY:    produce -> out          // first result
         Jump    CONT
NEXT:    <step>                  // e.g. cur += k
         <test>                  // e.g. Lte cur, hi
         Branch  test -> AGAIN   // falls through to Fail when exhausted
         Fail
AGAIN:   Choice  NEXT            // re-push identical frame
         produce -> out
         Jump    CONT
```

Exhaustion falls to `#Fail`, which pops the next outer choice point — the protocol every consumer
relies on.

### `e1 | e2` alternation

```
entry:   Choice  ALT
         [e1 code]               // e1's CPs sit above ALT's
         Jump    CONT
ALT:     [e2 code]
CONT:
```

e1's inner `#Fail` pops ALT only when e1 is fully exhausted; a later outer resume pops e2's CPs the
same way. Ordering is the choice stack's LIFO order, which is Icon's resumption order for free.

### `e \ n` limitation — kill flag

A resume cannot be intercepted once a CP is on the stack, so instead of intercepting, stale CPs are
*poisoned*: every generator emitted inside the limited `e` checks a per-site `kill` scalar cell at
the head of its resume path (`altOp` = `Branch kill -> KILL; ...` where `KILL` is a `#Fail` cell).

```
entry:   Choice  EXIT            // catches natural exhaustion
         Jump    E_ENTRY
GATE:    cnt--
         Eq      cnt, 0 -> t
         Branch  t -> SETKILL
         Jump    DELIVER
SETKILL: kill := 1
DELIVER: out := eResult
         Jump    CONT
KILL:    Fail
EXIT:    Fail                    // propagates limit's failure outward
```

When `cnt` hits 0 the flag is set; each remaining live e-CP pops exactly once, sees the flag, and
fails through — `O(live CPs)` cheap pops, so `\` on an infinite generator (`repeat`) terminates.

### Bounded evaluation — flag + drain (§4.4)

A bounded expression must take its first result and commit. `discard(mark)` drops the arena mark but
leaves the choice stack; the stale CPs are removed by an explicit drain whose produce side effects
are idempotent:

```
entry:   ok := 0; flag := 0
         Choice  GRAB
         [e code]               // produce path: out := res; flag := 1;
                                 //              ok   := 1; Jump DRAIN
DRAIN:   Fail                   // pops each stale CP; its altOp sees
                                 // flag=1 and Falls through to the next
GRAB:    Branch  ok -> CONT     // e produced at least once
         Fail                   // e produced nothing -> propagate failure
```

Fresh-evaluation loops (`while`, `until`) reset `flag := 0` at the loop head: the drain that ran
after the previous produce already *physically removed* the stale CPs, so resetting cannot revive
anything.

### Loops

- **`every e1 do e2`** — `Choice EXIT(altOp=DONE)`; unbounded `e1`; body `e2`; closing `#Fail`. e2's
  alternatives drain into e1's resumption (LIFO), which is Icon's nested-exhaustion order.
- **`while e1 do e2`** — e1 is re-entered each iteration under the bounded-eval idiom (flag reset
  per iteration); e1's exhaustion pops `EXIT(altOp=DONE)`.
- **`until e1 do e2`** — `while` with the sense inverted: body runs while the condition fails.
- **`break` / `next`** — flags consulted on every produce path inside the loop's dynamic extent:
  `bflag` jumps to the after-loop continuation (writing `break e`'s result first), `nflag` jumps to
  the loop head. The compiler threads a loop context (`bflag`, `nflag`, exit op) through emission.

### Comparison is a generator of its right operand

`i < j` emits `Lt i j -> t`; `Branch t -> produce j`; `Fail`. It produces `j` or nothing — no
boolean type exists.

### String scanning (iteration 3)

`s ? e` mints a cursor cell on `d.scan` (content = `&pos` scalar; `+d.grab` = subject cell;
posward-most cursor on `d.scan` is current). Cursor writes are trailed writes under the enclosing
mark, so `release` on backtrack restores `&pos` for free (spec §6.2).
`tab/move/find/many/upto/match/any/bal` are generators emitted with the same retry idiom.

## `std:goal` package

`buildGoalModule(CellRef mod)` in `vortex_stdlib.cpp`, called from `bootstrap()` after
`buildLogicModule`. `d.scan` is minted in `VortexCore::initGenesis()` beside `d.clause` (a
`SystemDimensions::scan` slot — data, not a learned name; `mintDimension` is the mechanism).

Exports (entry opcodes, dual-wing operand cells, `routineBindings_` registered):

| symbol                                                                       | contract                                             |
| ---------------------------------------------------------------------------- | ---------------------------------------------------- |
| `to`                                                                         | in: lo,hi,step,out,cont — retry-idiom generator      |
| `bang`                                                                       | in: rank-head,out,cont — one `#Choice` per axis step |
| `alt`                                                                        | in: e1Entry,e2Entry — two-arm choice                 |
| `limit`                                                                      | in: eEntry,n,out,cont — kill-flag counter            |
| `cmp`                                                                        | in: op,left,right,out,cont — produce right or fail   |
| `fail`                                                                       | `#Fail` alone; `succeed` produces `&null` once       |
| `if`                                                                         | in: condEntry,thenEntry,elseEntry                    |
| `while`/`until`/`every`                                                      | in: condEntry,bodyEntry                              |
| `case`                                                                       | sugar over `alt`+`cmp`                               |
| `repeat`/`seq`                                                               | unbounded produce / unbounded integer sequence       |
| `scan`,`subject`,`pos`,`tab`,`move`,`find`,`match`,`many`,`upto`,`any`,`bal` | scanning family (I3)                                 |
| `coexpr`,`activate`                                                          | OPTIONAL — pinned island, last                       |

The compiler emits the *same* opcode graphs inline (it does not `Call` these — the suspension
boundary is the driver, not the routine frame), and the symbols exist for `import()`/reflection and
the andor-parallelism consumers. Tests pin both paths to the same behaviour.

## Coding standards (§8.3)

`std::optional` for failure-as-value, `std::expected` for refusals that name a reason, chainable
mutators, `gleditor::cpp26::*` polyfills (`function_ref` for the result sink, `inplace_vector` for
control-structure arms), named spdlog categories `vicon.compile`/`vicon.resume`, tests in
`tests/xudu/` matching `vprolog_*_test.cpp` style. No `__cpp_lib_*` checks, no sentinels (`noCell`
is absence, never a value).

## Iterations

1. **I1** — this contract, headers, Makefile targets, app skeletons.
1. **I2** — lexer, parser, `std:goal` module, compiler+driver (parallel; file- disjoint).
   Acceptance: `every write(1 to 3)` prints `1 2 3`;
   `line ? write(tab(find("a")) || tab(find("b")))` restores `&pos` on failure.
1. **I3** — string scanning family.
1. **I4** — `break`/`next`/`case`, `repeat`/`seq`, `<-` reversible assignment, `viconc`,
   `tests/fuzz/fuzz_vicon.cpp`, `make format`/`lint`. Co-expressions are spec-optional and land only
   if the earlier iterations do.

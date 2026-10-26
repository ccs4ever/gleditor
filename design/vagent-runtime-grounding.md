# vagent runtime grounding: calls, standard routines and the native boundary

This note records the implementation boundary found while starting vagent's Vortex backend against
[the v3.8 specification](vagent-agent-language.md). It is a code audit, not a proposal to replace
Vortex execution with an AST evaluator. The relevant distinction is between a symbol naming a cell,
an executable opcode graph, and a C++ host-dispatch API. They are not interchangeable.

## The result

`#Call` does **not** invoke `VortexStdLib::call`, resolve a native function, bind its arguments, or
recognize its label. It pushes a return frame when the call has a successor, then sets the same
cursor's program counter to the integer target in its first input. The VM has no native routine
registry/dispatcher in that path.

`VortexStdLib::call` is a separate C++ API. Some of its labels select C++ implementations; otherwise
it copies values into registered argument cells, creates another cursor, runs that cursor and reads
registered output cells. A vagent compiler cannot treat every successful `resolve()` as an ordinary
callable function. That would silently execute exported Nops or leave the caller's continuation
unexecuted.

## The actual API paths

### 1. VM graph call and return

In `apps/common/xanadu/vortex/vortex_vm.cpp:670–690`, `OpcodeKind::Call`:

1. Renders the call's input and interprets its first value as the target opcode ref.
1. Reads the call opcode's posward `d.spin` successor.
1. If that successor exists, creates a frame containing its ref and pushes it on the cursor's
   `d.stack` chain.
1. Sets the **same cursor** to the target. It does not read extra argument inputs as a function ABI.

`Return` at `vortex_vm.cpp:693–707` pops that frame and jumps to its saved successor. The function's
body must actually execute Return. Reaching the end of a plain library graph does not implicitly pop
a frame: `run()` treats `noCell` PC as successful completion (`vortex_vm.cpp:1134–1165`). A Call
with no successor creates no frame, which is why the vagent compiler must retain its specified
post-Call continuation/Nop rule.

`step()` obtains an opcode kind from the VM map and treats an unrecognized cell as Nop
(`vortex_vm.cpp:1003–1019`). `run()` repeatedly invokes `step()` until a Halt, missing PC, failure
or cycle bound. Neither path calls the standard-library C++ dispatch API.

### 2. Standard-library symbol and invocation metadata

`VortexStdLib::resolve` walks the module and symbol cells, then follows `d.values` to the exported
entry (`vortex_stdlib.cpp:1931–1947`). It does not verify a vagent calling convention, distinguish
host stubs from graphs, or find a return instruction.

The private `RoutineBinding` table contains input and output cell refs
(`vortex_stdlib.hpp:459–463`). It is argument/result metadata, **not a native callback registry**.
Read-only `routineInputs()` and `routineOutputs()` accessors are public
(`vortex_stdlib.hpp:377–381`, `vortex_stdlib.cpp:3831–3847`). A compiler/loader can use that
metadata when constructing an explicit graph ABI, but #Call does not consume it automatically.

The host path is:

```text
C++ caller
  -> VortexStdLib::call("std:module/symbol", values)
  -> resolve(path)
  -> VortexStdLib::call(entryCell, values)
       -> selected label-specific C++ branch, or
       -> copy argument values into RoutineBinding input cells
       -> vm.spawnCursor(entryCell, "call")
       -> vm.run(newCursor, 100)
       -> render RoutineBinding output cells
```

The path overload is `vortex_stdlib.cpp:1967–1973`. The entry overload starts at `:1976`; its
generic registered-routine path is `:2189–2206`, and its fallback repeats the new-cursor pattern at
`:2209–2225`. Calling this API while a speculative vagent cursor is live introduces a second
cursor/activation rather than executing a normal call on vagent's stack.

### 3. What vicon provides as precedent

Vicon's driver starts and resumes compiled graphs through host-side `goalProduce`/`goalResume`
(`vicon/driver.cpp:34–51`). Those helpers start/run a cursor and use sentinel choice points
(`vortex_stdlib.cpp:3738–3775`). The driver is a generator resume loop; it is not a dispatch bridge
that notices arbitrary native calls during `VortexVM::run`.

Vicon `Compiler::emitCall` special-cases `write`, then looks for user procedures in `procTargets_`.
An unknown name is refused (`vicon/compiler.cpp:692–728`). User calls bind arguments and a
`__vicon_ret` continuation, then Jump through an indirection cell (`:729–769`); they do not
establish a reusable native #Call ABI. That older Bind/Jump convention is specifically not vagent's
proposed activation-frame convention. Vicon scanning currently refuses compilation with
`UnsupportedConstruct` (`vicon/compiler.cpp:559–561`).

## Classify the packages vagent plans to reuse

A routine can be useful to C++ tests or drivers without being directly callable by a running graph.
The classification below is based on its actual constructor and invocation path.

| Package/symbols                                                              | Actual representation                                                                                                                                        | What that establishes for vagent                                                                                                                                                               |
| ---------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `std:math` arithmetic and clamp                                              | Real primitive opcode entries/chains with registered input/output cells; for example `Min`, `Max`, `Add`, `Div` (`vortex_stdlib.cpp:1490–1657`).             | Useful graph bodies, but caller argument/result transfer and Return plumbing still need an explicit ABI.                                                                                       |
| `std:string/to_lower`, `to_upper`, `trim`, `clean`                           | Real `ToLower`, `ToUpper`, `Trim` opcodes; `clean` links two (`:1659–1712`).                                                                                 | Can be lowered as real call-site primitives or wrapped/copied graphs. There is no implicit host call involved.                                                                                 |
| `std:logic/unify`, `var`, `is_var`, `term`, `deref`, `choice`, `fail`, `cut` | Real matching VM opcodes with routine metadata (`:2802–2887`).                                                                                               | Their primitive execution exists. Vagent still must supply the right cell-ref/value representation and choice/failure protocol.                                                                |
| Other `std:logic` exports such as `equal` predicates                         | Predicate/clause cells, rather than necessarily opcode entry points (`:2889` onward).                                                                        | Resolving a predicate is not permission to jump to it as an opcode. Clause selection/database semantics need the specified logic implementation.                                               |
| Graph-backed parts of `std:goal`                                             | Explicit opcode graphs, often with operand refs, output cells and produce-continuation cells; see `alt`, `succeed`, `scan`, `subject` (`:3527–3694`).        | They use a generator/continuation protocol, not automatic scalar Call/Return. Vagent should use its own statement-bounded lowering where semantics differ.                                     |
| Host-control `std:goal/every`, `bang`, `if`, `while`, `until`                | Nop entries made by `hostRoutine` (`:3698–3732`); `call()` dispatches their labels in C++ (`:2124–2187`).                                                    | #Call to these entries executes Nop, not the C++ control implementation. `goalBoundedFirst`/`goalResults` create additional cursors (`:3778–3828`), unsuitable as an inline vagent substitute. |
| Vicon scanning `tab`, `move`, `find`, `many`, `upto`, `match`, `any`, `bal`  | Not exported by the current `std:goal` builder. The spec already identifies this prerequisite.                                                               | Accepting an import or declaring the signatures does not implement them. Their bounded graph semantics remain work.                                                                            |
| `std:collections/list`, `map`, `grid`, `push_back`, `push_front`, `pop_back` | Exported Nop entries with registered argument/output cells (`:1845–1928`). C++ helpers such as `createList`/`pushBack` implement data operations separately. | The existing shapes/helpers can inform vagent's layout, but the exported entries are not working graph calls. The current `call()` code has no `#COLL_*` dispatch branch either.               |
| `std:agent`                                                                  | Specified new prelude, not an existing standard-library registration.                                                                                        | Its dispatcher, beliefs, intention operations and ordinary helpers need real compiled graphs; bootstrap cannot supply them by name alone.                                                      |

The test suite illustrates the split: math/string tests use the C++ `stdlib.call` API
(`tests/xuzz/vortex_stdlib_test.cpp:60–141`), whereas collection tests call helpers such as
`createList`/`pushBack` directly (`:272–277`). Those passing helper tests do not prove that
`#Call(std:collections/push_back)` works.

For comparison, `#ARRAY_*`, `#ZZ_*`, `#NAV_*` and some other names really are intercepted by
`VortexStdLib::call` (`vortex_stdlib.cpp:1982–2123`). They are still C++ API dispatch, not VM #Call
behavior; these examples do not establish a native bridge for vagent's own dependencies.

## The integration gap in the specification

Vagent §10 says its Vortex-source prelude reuses these packages
(`vagent-agent-language.md:1470–1475`), and §11.1 says qualified package symbols resolve against the
agent arena's stdlib at load (`:1540–1545`). Those statements are viable as dependency/layout
intent, but they are not a complete invocation contract. The implementation must distinguish:

- a graph-backed symbol whose body can be called after argument/result and Return adaptation;
- a generator symbol with its own continuation/choice protocol;
- a predicate/data cell that is not an opcode entry;
- a host-dispatched or currently unimplemented Nop stub.

The specification already acknowledges that std:goal's host `if`/`while` cannot be called from a
running cursor in its P1 rationale (§11.2). The same distinction must be applied consistently to all
imported symbols. A generic import-to-#Call translation would otherwise claim execution that the
current runtime does not supply.

## Concrete implementation path

1. **Define and enforce a graph calling convention.** Keep one VM cursor per agent (V3). Caller code
   evaluates arguments, spills live temporaries into activation frames, and transfers values through
   explicitly described parameter cells. A callable ordinary graph returns through #Return to an
   always-present continuation. Recursion cannot share unspilled static opcode outputs.
1. **Lower primitive builtins to actual Vortex operations.** Small graph-backed math/string/logic
   routines can become call-site primitive fragments or compiler-owned ABI wrappers. Preserve typed
   scalar bits, argument order, contracts and result representation. Do not mutate a shared module
   tail merely to append an ad hoc Return for one caller.
1. **Build the missing pure helpers as executable graphs.** Reuse the documented collections and
   term layouts, but emit real New/Link/Value/Splice/branch/loop operations and the specified Unify/
   Choice/Fail/Cut/ChoiceDepth behavior. Implement scanning and belief/prelude routines with the
   proper bounded statement/condition semantics. No exported Nop may count as implementation.
1. **Validate imports at the loader boundary.** Link only supported graph/ABI symbols. Refuse a
   noncallable predicate, missing prerequisite or host-only stub with a named linkage error rather
   than silently treating it as a successful unit/empty result. Resolving a name is necessary but
   insufficient. Artifact metadata must bind the supported ABI and dependency semantics.
1. **Keep external work on the existing staged boundary.** The graph writes typed outbox entries;
   after a successful turn, the adapter sends/spawns/starts native effects and later posts their
   completion as input (V1/V5/V6). Conditions and ordinary methods do not escape to device/native
   host dispatch. The adapter drives the VM and accounts for work; it does not interpret AST nodes.

This path requires new compiler/prelude/adapter work but does not itself require a host-reaching
opcode. If the implementation instead needs a generic VM native-call hook, that is an additional
explicitly priced core proposal and artifact/security contract. Recognizing specially labeled Nops
in an adapter step loop would be an instruction extension in substance, not an already-approved
mechanism or a way to avoid naming that proposal.

## Acceptance evidence needed in the next iterations

- A real ordinary graph call transfers arguments, returns its value, executes the caller's next
  instruction and restores the caller's stack depth. A nested/recursive call preserves spilled
  temporaries and leaves no second cursor or marks after the turn.
- An imported host-only stub cannot pass callable linkage and then return an empty/stale output.
  Tests must exercise graph execution, not only resolve or C++ helper invocation.
- Collection and scan graph operations under an outer choice point restore data/scan position on
  failure and retain successful statement results after the correct cut barrier.
- A pure condition cannot reach a C++ side effect or a nested host-driven VM cursor. Budgets apply
  to its actual work instead of an unrelated `call(..., 100)` default.
- A failed turn drains no external outbox work. Successful effects start only after commit, and
  their delayed results resume intentions through stable ids, not saved AST evaluation frames.

## Numeric kernel prerequisite

Scalar lowering uses the VM numeric instructions rather than a host evaluator or compiler-side
arithmetic execution. Int64 relational operations must compare both Int64 operands directly:
conversion to Double collapses adjacent integers above 2^53 and near the signed limits. Mixed
numeric and lexicographic string comparisons retain the existing VM rules.

Signed Add, Sub, Mul, unary Sub, Neg and Abs reject unrepresentable Int64 results. Div, Mod and
DivMod reject the Int64 minimum divided by minus one before performing either C++ operation. The
failure is an `ExecutionResult` with `success == false`, `Precondition`, the numeric opcode as
`failingClause` and an overflow diagnostic; destination cells are not written. Unary Sub of a Double
remains a Double. Legacy zero-divisor results are unchanged; vagent lowering must emit its own zero
guards.

Arithmetic failure in a primitive contract clause makes that clause false; it must not fall back to
truthiness of the opcode cell. The legacy `runPipeline` API returns no `ExecutionResult` and still
has no error channel to propagate a failed preprocessing/postprocessing operation. That separate
pipeline limitation must be resolved before relying on numeric failures there; this patch does not
introduce a new public pipeline API.

This kernel failure is not itself goal-directed recovery. The vagent adapter must recognize builtin
contract failure and backtrack the running cursor at the specified statement/condition boundary,
restoring the appropriate arena mark and advancing the remaining alternative. An unrecoverable
failure must reach the enclosing handler/plan policy. A correct first numeric kernel does not imply
that the actor adapter, complete scalar language, or full runtime already exists.

The architecture audit reports no runtime verification. Numeric regression tests accompany the
kernel changes; build/test results are recorded by the integrating agent. Source line numbers above
identify the audited state and may move as implementation proceeds.

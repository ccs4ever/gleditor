# vagent: activation frames and run-time cells (design §5.8, §5.6, §6.6)

Analysis only. Line numbers are `apps/common/xanadu/vortex/vortex_vm.cpp` unless noted. The claims
in section 1 were run as a throwaway gtest (seven cases, all passing, not committed).

## 1. How operands resolve, and what can reach a run-time cell

An operand is a **slot**: a scalar cell on the opcode's `+d.grab` wing (inputs) or `-d.grab` wing
(outputs), chained on `d.step`, whose Int64 value names the operand cell (`VortexCore::bindInput`,
`vortex_core.cpp` 366-377; `wingOf`, 340-352, turns an in-arena slot value into that cell and
otherwise falls back to the slot itself). `step` (1008-1063) runs any parameter pipeline on each
input, renders every operand cell to a value (`oldInputs`), executes the body, and writes the body's
outputs into the output cells, except for `New`, `Clone`, `Link` and `Break` (1049-1052).
`resolveCell` (48-63) lets the operand cell win over the value: opcodes that take a cell (`Link`
531, `Value` 590, `Splice`, `Clone`, `Deref` 934) act on the operand cell itself, and only fall back
to reading the value as a ref when there is no operand cell.

Consequences, each confirmed by a probe:

- **Obtaining a run-time ref works, four ways.** `Value` on an empty cell with length 0 returns a
  freshly minted cell (`vortex_core.cpp` 216-219), and `step` writes its ref to the output: this is
  the compiler's `alloc`. `Bind` outputs its new value cell's ref (710-733). `Deref` outputs a ref
  as an integer (934). A `Link` read through a parameter pipeline writes the neighbour's ref into
  the pipeline's parameter cell (`runPipeline`, 233-239, which excludes `New`/`Link` only when an
  output wing exists). `New` does not: its result is dropped, or it links a static output cell.
- **Using one directly does not work.** A register holding `x`'s ref, given to `Value`, is
  overwritten; `x` is untouched. No opcode follows a ref held in a value.
- **The only way to use one is a poke**: a prior `Value` that writes the register's value into the
  consuming opcode's slot. It costs one cycle per operand (probe: 2 cycles for one poked write) and
  edits the program graph at run time, so the program's slot cells carry transient refs.
- **`Resolve` cannot return a ref** (736-757): it returns the bound value, a copy. `Bind` prepends
  to the cursor's `+d.vars`, never unbinds, and its scope is the cursor, not a frame: a callee's
  `Bind` was still resolvable after `#Return`. `Resolve` walks the rank linearly from the newest.
- **The stack top is reachable.** If the adapter writes the cursor's ref into a static cell (it
  spawns the cursor, so it knows it), a poked pipeline `Link` read of `+d.stack` yields the frame
  `#Call` pushed (670-691). A `Link` with a poked frame and a freshly allocated cell then hangs that
  cell on the frame's `d.vars`. Probe: the whole prologue, call and return ran in 10 cycles,
  `#Return` popped the frame, and the vars rank was built. **So §5.8's frame layout is expressible
  with today's opcodes**; what is missing is a cheap way to use the refs, not a way to get them.

The compiler's header is therefore right about cost and wrong about possibility: frames were
reachable, but every access to a local would be a poke.

## 2. Whether the register scheme can suspend and resume

No, for three independent reasons.

1. **Registers are static cells shared by every intention running the same plan.** Two intentions on
   one plan interleave across turns; the second would overwrite the first's registers. Any
   suspension point must copy the live registers out and the resume must copy them back, so the
   register scheme is at best a cache over the real locals, which must live in the intention frame.
1. **Registers hold refs as Int64 values, and a checkpoint does not remap those.** `promote` and
   `checkpointView` walk links and remap `OpHandle` cells (`arena_manifold.cpp` 1790-1810,
   1941-1950; `mesh_value.cpp` 386-434); an Int64 is copied verbatim (`arena_manifold.cpp` ~1902).
   After resume a saved ref names some other cell. A spill must therefore store each ref-valued
   local as a **link** (name cell, `+d.values`, the record or list cell), and reload by reading that
   link back.
1. **Recursion spill by `#Bind` is unbounded and checkpoint-hostile**: it leaks past `#Return` and
   keeps depth-named cells in the cursor's scope forever.

Cost of making the scheme suspendable: per live local, per suspension, one link store (mint name
cell by `alloc`, two pokes, `Link`, a chain `Link`: about 8 cycles by the probe's counts) and per
resume one pipeline read (5 cycles: three pokes, `Link`, `Nop`) plus a register write. A plan with
six live locals at a `wait` costs about 80 cycles to suspend and 40 to resume, every time, plus a
compiler pass computing liveness at each suspension point. It is feasible and needs no core change,
but it keeps both copies of every local and doubles the compiler's local-variable code.

## 3. Options and the smallest core change

| Option                                                                 | Gives                                                 | Cost                                                                                  |
| ---------------------------------------------------------------------- | ----------------------------------------------------- | ------------------------------------------------------------------------------------- |
| A. No change: registers plus spill to the frame at suspension          | suspension and checkpoint                             | about 120 cycles per wait; two code paths                                             |
| B. No change: all locals on the frame, adapter-published cursor, pokes | §5.8 and §5.6 literally                               | 3 to 5 cycles per local access                                                        |
| C. `#FrameGet name` / `#FrameSet name v` (and a ref-returning variant) | O(1)-cycle access by name, in C++                     | 3 to 4 opcodes; the intention frame is not the stack top unless the adapter pushes it |
| D. Handle operands: `#Point` plus input slots that follow `OpHandle`   | pokes vanish for every opcode; refs survive promotion | one opcode and a few lines in `wingOf`                                                |

Rejected: a bare `#Frame` (yield stack top) shrinks nothing, since the probe shows the stack top is
already reachable and the cost is the poke. `#Resolve` returning refs would serve names but still
needs a poke to use the ref.

**Recommendation: D, as proposal P16.** It alone attacks the poke (the cost in B), makes locals
checkpoint-safe (the defect in A), keeps §5.8's d.vars/d.values layout, and needs no frame-specific
semantics, so intention frames (not on the stack) work identically to activation frames. `OpHandle`
already exists as the value kind that names another cell and is remapped on promotion.

| No. | Change                                                                                                                                                                                                                                                                                                                                                     | Where                                                                                                                                                                                                     | Why it cannot live in `std:agent` or the adapter                                                                                                          | Without it                                                                                                                                                                         | Used by                                                                                 |
| --- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------- |
| P16 | (a) `#Point out, ref`: make the output cell an `OpHandle` whose target is the cell the input's value names (the input is a ref as an Int64, as `Jump` reads an op ref). (b) On the **input** wing only, an operand slot that is an `OpHandle` cell resolves to its target; the output wing never follows, so a handle register can be rebound by `#Point`. | `vortex_vm.{hpp,cpp}`: enum entry appended after `ChoiceDepth`, one `case` of about 12 lines. `vortex_core.cpp` `wingOf`: about 6 lines. Opcode name tables in `vql/compiler.cpp` and `vpl/compiler.cpp`. | Operand resolution is inside `wingOf`/`step`; no Vortex or adapter code can change what a slot means. The adapter cannot do it per opcode without a poke. | Every use of a local that holds a ref is a poked write into the program graph; checkpoint cannot carry such a local; plan locals need a second copy at each suspension (option A). | compiler locals, call arguments, intention frames (§5.6, §6.6), `for` over lists (§5.9) |

Size: about 25 lines of engine code, 60 to 90 lines of test (handle followed on input, not on
output; `#Point` rebinding; checkpoint and resume of a handle local; no change to existing programs,
since no existing cell is an `OpHandle` operand). Open point to confirm when building: that
`asInt64` on an `OpHandle` slot is empty, so the existing fallback does not already misread it
(`wingOf` falls back to the slot itself). The compiler then drops its pokes, its register file and
its depth-keyed `#Bind` spills; a plan's suspension becomes "leave the frame as it is".

Until P16 is approved, option B (frames on `d.vars` via an adapter-published cursor cell and pokes)
is the correct no-change path for iteration 4 and is cheaper to migrate than A, because P16 only
turns each poke-plus-opcode pair into one opcode.

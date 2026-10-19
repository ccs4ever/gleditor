# Codebase section: shared mutable state in the Vortex VM

This is the code-grounded inventory for the multithreading study. Every claim below cites a file and
line in the `study/vortex-multithreading` worktree. It describes the state as it exists today, not a
proposed design. Where the briefing and the code agree, the citation is repeated so the synthesis
author can trust one source.

The one sentence that matters: **every opcode of every cursor mutates one `ArenaManifold`, held by
reference in exactly one place, with no lock anywhere in the VM, the core, the stdlib, or the
host.** That single arena is the GIL-analogue the owner fears, and it is not a figure of speech — it
is a `ArenaManifold &arena_` member (`vortex/vortex_core.hpp:256`) that the whole runtime shares.

## 1. Inventory of shared mutable state

### 1.1 The single `ArenaManifold` (the shared heap)

`VortexCore` holds exactly one arena by reference, plus one `home_` cell and one `SystemDimensions`
value:

```text
vortex/vortex_core.hpp:256   ArenaManifold &arena_;
vortex/vortex_core.hpp:257   CellRef home_{noCell};
vortex/vortex_core.hpp:258   SystemDimensions dims_{};
```

The host owns the concrete arena and threads it through every component by reference in its member
init list:

```text
vortex/vortex_host.cpp:77    : arena_(baseManifold), core_(arena_), vm_(core_), stdlib_(core_, vm_),
vortex/vortex_host.cpp:78      vqlEngine_(core_), vqlCompiler_(core_, vm_), vplEngine_(core_) {
```

So `core_`, `vm_`, `stdlib_`, and the compilers all share one `arena_`. The arena itself is
documented as single-owner, not thread-safe (`zigzag/arena_manifold.hpp:15`): "dense vectors only.
No ops. Dies with its owner."

Its mutable backing stores — any write to any of these from two threads is a data race:

```text
zigzag/arena_manifold.hpp:661   std::vector<CellSlot> slots_;
zigzag/arena_manifold.hpp:662   std::vector<DimLink> links_;
zigzag/arena_manifold.hpp:663   std::vector<xanadu::PrimediaSpan> content_;
zigzag/arena_manifold.hpp:665   std::string scratch_;
zigzag/arena_manifold.hpp:666   std::vector<TrailEntry> trail_;
```

plus the overlay/federation bookkeeping that mutates on first write to a base cell:

```text
zigzag/arena_manifold.hpp:675   std::unordered_map<CellRef, std::uint32_t> overlay_;
zigzag/arena_manifold.hpp:678   std::vector<CellRef> shadowOrder_;
zigzag/arena_manifold.hpp:680   std::size_t liveLinks_{0};
zigzag/arena_manifold.hpp:681   std::size_t liveContent_{0};
zigzag/arena_manifold.hpp:682   std::uint32_t outstandingMarks_{0};
zigzag/arena_manifold.hpp:696   std::vector<Floors> floors_;
```

The mutators — the write path that two cursors would contend on — are:

```text
zigzag/arena_manifold.hpp:518   CellRef makeCell();              (and the content/scalar overloads)
zigzag/arena_manifold.hpp:537   CellRef makeScalarCell(...);
zigzag/arena_manifold.hpp:542   xanadu::PrimediaSpan intern(...);   // appends to scratch_
zigzag/arena_manifold.hpp:546   ArenaResult setContent(...);
zigzag/arena_manifold.hpp:550   ArenaResult setValueBits(...);
zigzag/arena_manifold.hpp:564   ArenaResult link(...);              // maintains both ends
zigzag/arena_manifold.hpp:587   ArenaManifold *insertIntoRank(...);
zigzag/arena_manifold.hpp:593   Mark mark();                        // reads seven lengths
zigzag/arena_manifold.hpp:597   ArenaManifold *release(const Mark&); // truncates + replays trail
zigzag/arena_manifold.hpp:601   ArenaManifold *discard(const Mark&);
zigzag/arena_manifold.hpp:624   ArenaResult compact();              // moves every run
```

Even the read path is not pure under COW: `linkedMinting()` (`zigzag/arena_manifold.hpp:460`) and
the private `shadow()` (declared in the private section, described in the overlay doc comment at
`zigzag/arena_manifold.hpp:35-47`) *write* on first access to a base cell, so a "reader" cursor can
mutate `overlay_`, `shadowOrder_`, `slots_`, `links_`, and `content_` as a side effect of reading
through the base.

### 1.2 `VortexCore`'s structural primitives all funnel into the arena

Every named primitive on the core is a thin wrapper over arena writes (`vortex/vortex_core.cpp`):

```text
vortex/vortex_core.cpp:124   VortexCore::link(...)
vortex/vortex_core.cpp:165   VortexCore::value(...)
vortex/vortex_core.cpp:220   VortexCore::newCell(...)
vortex/vortex_core.cpp:235   VortexCore::breakLink(...)
vortex/vortex_core.cpp:454   VortexCore::pin(...)
vortex/vortex_core.cpp:509   VortexCore::collectGarbage()
vortex/vortex_core.cpp:515   VortexCore::getOrCreateMemoPin(...)
```

`splice`, `insert`, `append`, `erase`, and `mintDimension` are the same shape. There is no primitive
on the core that does not touch the shared arena.

### 1.3 `VortexVM`'s unsynchronized maps and the global choice stack

The VM adds four mutable members, none of them per-cursor-isolated and none synchronized
(`vortex/vortex_vm.hpp:197-200`):

```text
vortex/vortex_vm.hpp:196   VortexCore &core_;
vortex/vortex_vm.hpp:197   std::unordered_map<CellRef, OpcodeKind> opcodeMap_;
vortex/vortex_vm.hpp:198   std::unordered_map<CellRef, std::string> memoizedOps_;
vortex/vortex_vm.hpp:199   std::unordered_map<CellRef, CellRef> cursorPC_;
vortex/vortex_vm.hpp:200   std::vector<ChoicePoint> choiceStack_;
```

- `opcodeMap_` maps an opcode cell to its `OpcodeKind`. It is a program-wide table: shared by all
  cursors, written at assembly time, read on every step.
- `memoizedOps_` maps an opcode cell to its memo key. Program-wide, same sharing.
- `cursorPC_` is the per-cursor program counter — but it is **one map keyed by cursor**, not a
  per-cursor object. Two threads advancing two cursors both write this one map.
- `choiceStack_` is a **single global** vector of `ChoicePoint`, not per-cursor. This is the hardest
  structure for any real-threads design, discussed in §5.

### 1.4 The `+d.cursors` rank (the runnable set, in the arena)

Cursors are not C++ objects; they are cells linked onto `home`'s `+d.cursors` rank inside the arena.
`spawnCursor` links the new cursor cell onto the tail of that rank (`vortex/vortex_vm.cpp:127-141`),
and `activeCursors()` reads the rank back (`vortex/vortex_vm.cpp:143-146`). So the runnable set is
itself shared mutable arena state: spawning a cursor is an arena write, and enumerating cursors is
an arena read.

### 1.5 Memo pins and the memo cache rank

Memoization lives entirely in the arena on `d.cache` ranks hung off pin cells. `recordMemo`
(`vortex/vortex_core.cpp:535-554`) mints cells for inputs and outputs and splices a new entry at the
head of the pin's `+d.cache` rank; `lookupMemo` (`vortex/vortex_core.cpp:521-533`) walks that rank.
`getOrCreateMemoPin` (`vortex/vortex_core.cpp:515`) mints the pin on first use. All three are arena
reads/writes with no synchronization. The VM step calls them around opcode execution
(`vortex/vortex_vm.cpp:` memoization check at step phase 4, record at phase 8), so a memoized opcode
run concurrently by two cursors would double-mint and race the rank.

### 1.6 The GC cursor and `compact()`

A system GC cursor is spawned at host init (`vortex/vortex_host.cpp:86`):

```text
vortex/vortex_host.cpp:86    gcCursor_ = vm_.spawnCursor(*sweepOp, "sys:gc");
```

The scheduler periodically triggers GC (`vortex/vortex_host.cpp:140-143`), which calls
`stdlib_.gcSweep()` (`vortex/vortex_stdlib.cpp:3745`), which calls `core_.collectGarbage()`
(`vortex/vortex_core.cpp:509`), whose body is:

```text
vortex/vortex_core.cpp:509   std::size_t deadBefore = arena_.deadLinks();
                             zigzag::expectWritten(arena_.compact());
                             return deadBefore;
```

This is load-bearing for threading: `compact()` **moves every run** and is **refused while a mark is
outstanding** (`zigzag/arena_manifold.hpp:613-624`). A GC that runs while any cursor is
mid-backtrack (mark outstanding) is already refused today, serially; under real threads, GC and a
backtracking cursor cannot touch the arena at the same time at all, because `compact()` invalidates
every offset a `Mark` recorded.

## 2. Where each piece is read and written (file:line)

| State                | Written at                                                              | Read at                                 |
| -------------------- | ----------------------------------------------------------------------- | --------------------------------------- |
| `arena_` slots/links | all core primitives `vortex_core.cpp:124,165,220,235`                   | every `linked`/`render`/`asInt64` in VM |
| `arena_` scratch     | `intern` via `mintOpcode` `vortex_vm.cpp:71`                            | `textOf`/`scratchTextOf`                |
| `arena_` overlay     | `shadow()`/`linkedMinting` first write to base cell                     | every read-through of a base cell       |
| `opcodeMap_`         | `mintOpcode` `vortex_vm.cpp:68`; `setOpcodeKind` `:92`                  | `getOpcodeKind` `vortex_vm.cpp:95`      |
| `memoizedOps_`       | `enableMemoization` `vortex_vm.cpp:148`                                 | `isMemoized`/`getMemoKey` `:152,:156`   |
| `cursorPC_`          | `setCursorOpcode` `vortex_vm.cpp:119`                                   | `getCursorOpcode` `vortex_vm.cpp:107`   |
| `choiceStack_`       | `pushChoicePoint` `vortex_vm.cpp:164`; `backtrack` `:178`; `cut` `:197` | `choiceDepth`/`backtrack` scan          |
| `+d.cursors` rank    | `spawnCursor` `vortex_vm.cpp:127`                                       | `activeCursors` `vortex_vm.cpp:143`     |
| memo pins/`d.cache`  | `recordMemo` `vortex_core.cpp:535`; `getOrCreateMemoPin` `:515`         | `lookupMemo` `vortex_core.cpp:522`      |
| GC `compact()`       | `collectGarbage` `vortex_core.cpp:509` → `arena_.compact()`             | triggered `vortex_host.cpp:140`         |

The `step()` lifecycle (`vortex/vortex_vm.cpp:920`) touches most of these in one call: it reads
`cursorPC_` (phase 0), reads `opcodeMap_` (phase 0), reads/writes `arena_` through preconditions,
pipelines, snapshot, body, postconditions (phases 1–7), reads/writes the memo rank (phases 4 and 8),
and writes `cursorPC_` to advance (phase 9). A single step is therefore a wide critical section over
nearly all shared state.

## 3. The scheduler loop and the `spawnCursor`/`spawnThread` naming trap

The scheduler is cooperative, single-threaded, and lock-free because it never needs a lock —
everything runs on the calling thread (`vortex/vortex_vm.cpp:1076`):

```text
std::size_t VortexVM::stepScheduler() {
  std::size_t executed         = 0;
  std::vector<CellRef> cursors = activeCursors();      // reads +d.cursors
  for (CellRef cursor : cursors) {
    CellRef op = getCursorOpcode(cursor);              // reads cursorPC_
    if (op != noCell) {
      auto res = step(cursor);                          // the wide critical section
      if (res.success) executed++;
    }
  }
  return executed;
}
```

The host wraps it in a cycle-budget loop and folds GC in (`vortex/vortex_host.cpp:128-150`). There
is no preemption, no OS thread, no yield point other than "`step()` returned."

**The naming trap.** The word "thread" already exists in the API and already means "green cursor":

```text
vortex/vortex_host.hpp:174   bool spawnThread = false   // default param on compileAndAttachVQL
vortex/vortex_host.cpp:513   ..., bool spawnThread) {
vortex/vortex_host.cpp:526   if (spawnThread) {
vortex/vortex_host.cpp:527     vm_.spawnCursor(result.entryOpcode, "vql_exec");
```

`spawnThread == true` does nothing but call `spawnCursor`. Likewise `spawnCursor`'s cells are linked
onto a rank named `d.cursors` and named via `d.name` with labels like `"main"`, `"call"`,
`"sys:gc"`, `"vql_exec"` — the vocabulary of OS threads is already spent on green threads. A
real-threads design must either rename this surface or carefully redefine it; shipping an OS-thread
primitive called `spawnThread` beside a green-thread flag called `spawnThread` is a trap for every
future reader. The `SystemDimensions::name` comment even calls `d.name` the "Thread, pin, and store
naming rank" (`vortex/vortex_core.hpp:48`).

## 4. Which surfaces spawn cursors / are entry points

Every `spawnCursor` call site (verified by grep across `apps/common/xanadu`):

```text
vortex/vortex_vm.cpp:127     VortexVM::spawnCursor  (the definition)
vortex/vortex_host.cpp:86    "sys:gc"      — the GC cursor at host init
vortex/vortex_host.cpp:212   actionName    — keymap action routine dispatch
vortex/vortex_host.cpp:412   canonical     — stdlib op dispatch via action
vortex/vortex_host.cpp:527   "vql_exec"    — compileAndAttachVQL(spawnThread)
vortex/vortex_stdlib.cpp:2075 "call"       — std call, routine-bound path
vortex/vortex_stdlib.cpp:2093 "call"       — std call, direct-wing fallback
vpl/compiler.cpp:1121        "main"        — VPL program entry
vql/compiler.cpp:866         "main"        — VQL query entry
```

Notes on the stdlib functional surface (`vortex/vortex_stdlib.cpp`):

- `map` (`:2212`), `filter` (`:2232`), and `fold` (`:2257`) each take a `function_ref` and run
  **synchronously on the calling thread** over a `rank(...)` range. Their `CellRef fnOp` overloads
  (`:2224`, `:2248`, `:2269`) call `call(fnOp, ...)`, and `call` *spawns a cursor and runs it to
  completion inline* (`vortex/vortex_stdlib.cpp:2075`/`:2093` then `vm_.run(cursor, 100)`). So today
  `map`/`filter`/`fold` over a Vortex function are a sequence of inline cursor runs, each mutating
  the one arena. These are the briefing's "most natural fit for real parallelism" (VPL data-parallel
  arrays) and are the cleanest candidate surface — but only because the `function_ref` body is the
  parallel unit, not because the arena is ready for it.
- `call` is the universal invocation primitive and the single most frequent cursor spawner at
  runtime; it also binds input wings via `core_.value(...)` before spawning
  (`vortex/vortex_stdlib.cpp:2069-2073`), which is another arena write per call.

Frontend entry points:

- VQL: `vql/compiler.cpp:866` spawns `"main"`; `compileAndAttachVQL` optionally spawns `"vql_exec"`
  (`vortex/vortex_host.cpp:527`).
- VPL: `vpl/compiler.cpp:1121` spawns `"main"`.
- vprolog: **does not call `spawnCursor` at all.** Its compiler (`vprolog/compiler.cpp`) lowers
  clauses into the logic database over `VortexCore` (`compileClause`/`lowerTerm`,
  `vprolog/compiler.hpp:34-50`) and resolution runs on the VM's C++ logic opcodes (`Unify`,
  `Choice`, `Fail`, `Cut`, `Deref`, `MakeVar`, `MakeTerm`) in `executeOpcodeBody`
  (`vortex/vortex_vm.cpp` `Unify` at `:789`, `Choice` at `:878`, `Fail` at `:887`, `Cut` at `:899`)
  over the single global `choiceStack_` and the single arena trail. This is why vprolog collides
  hardest with any threading design: its parallelism (AND/OR search) is exactly the thing the global
  choice stack and global arena mark/release forbid.

## 5. Blunt assessment: what is safe to touch, what is load-bearing

**Load-bearing, do not casually change:**

- **Arena `mark()`/`release()`/`discard()`** (`zigzag/arena_manifold.hpp:593-601`). Undo is
  truncation of dense vectors to recorded lengths; the whole point is that a resolution engine can
  take a million choice points for free (the `Mark` doc comment,
  `zigzag/arena_manifold.hpp:157-171`). A `Mark` names offsets into the shared
  `slots_`/`links_`/`content_`/`trail_` vectors. **Two threads cannot hold independent marks on one
  arena**: thread B's `makeCell` grows the vectors thread A's mark named, so A's `release` would
  truncate B's cells. This is the structural reason the single arena is a GIL, not merely a
  lock-contention hotspot — the data model itself assumes one writer with a nested mark stack
  (`floors_`, `zigzag/arena_manifold.hpp:696`).

- **`ephemeralBit` and the R8 refusal machinery.** Every arena ref is `ephemeralBit | dense`
  (`refOf`, `zigzag/arena_manifold.hpp:428`), and `Store::setLink()` / `Manifold::applyStructure()`
  refuse an ephemeral ref so a leaked arena ref is caught without new code
  (`zigzag/arena_manifold.hpp:24-33`). A per-thread-arena design must preserve this: each worker
  arena still mints `ephemeralBit` refs, and nothing may persist them except `promote()`. Do not
  invent a second ephemeral tag space.

- **Copy-on-write overlay.** Reads of a base cell fall through; first write shadows the whole cell
  keeping the base `CellRef` as identity (`zigzag/arena_manifold.hpp:35-47`,
  `overlay_`/`shadowOrder_` at `:675`/`:678`). This is what makes "resolve against a document's
  clause database" cheap. It is also what makes a "reader" cursor a writer (§1.1). A multi-arena
  design can lean on this: give each worker its own arena over the **same immutable base
  `Manifold`** — reads are lock-free fall-through, writes shadow privately — but only if the base is
  genuinely never mutated during the parallel region (the header already demands this: base "must
  outlive this, and must not be mutated while it is being read through",
  `zigzag/arena_manifold.hpp:250-252`).

- **`promote()`** (`zigzag/arena_manifold.hpp:758`, the free function). The one road to persistence;
  it maps refs rather than trusting them and is budgeted (`PromotionBudget`). It walks only what is
  reachable from the answer root, so "promote the answer, not the search" is already the contract. A
  parallel design that fans out worker arenas can keep this unchanged if each worker promotes its
  own answer subgraph into the store under a lock, or returns the subgraph for the main thread to
  promote.

**Plausibly safe to touch (with care):**

- **`opcodeMap_` and `memoizedOps_`** are effectively write-once-at-assembly, read-many-at-run. If a
  threading design freezes program assembly before the parallel region, these become immutable
  shared reads and need no lock. That is the easy win.
- **`cursorPC_`** is logically per-cursor; the only reason it is shared is that it is one map keyed
  by cursor. Splitting it into per-worker PC state (or storing PC solely in the cursor cell's value
  bits, which `setCursorOpcode` already does as a mirror, `vortex/vortex_vm.cpp:119-124`) removes it
  as a contention point.
- **The stdlib `map`/`filter`/`fold` `function_ref` bodies** are the cleanest parallel surface: the
  per-element function is already isolated behind `function_ref`, and the ranges are materialized.
  The obstacle is not the surface, it is that the body currently calls `call` → `spawnCursor` →
  arena writes on the one shared arena.

**Hardest / least safe:**

- **`choiceStack_`** (single global) plus **global arena mark/release**. Concurrent backtracking by
  two OS threads over one arena is not expressible today, full stop (briefing §logic; confirmed by
  `pushChoicePoint`/`backtrack` taking `core_.arena().mark()`/`release()`,
  `vortex/vortex_vm.cpp:164-195`). Any parallel vprolog needs either per-worker arenas with
  per-worker choice stacks (OR-parallelism across independent arenas) or a redesign of the trail.
  This is the single biggest structural obstacle and should drive the staging plan.
- **GC `compact()` vs. outstanding marks** (§1.6). GC and backtracking are already mutually
  exclusive serially; under threads they must be coordinated, and `compact()` can never run
  concurrently with any marked arena.

## Open questions for the other experts

1. **Arena topology.** Is the recommended model one arena per OS worker over a shared immutable base
   `Manifold` (lock-free reads, private shadows, merge via `promote()`), or one shared arena behind
   a lock (serializes exactly the GIL we fear)? The data model (`mark`/`release` truncation,
   `floors_`) strongly favors per-worker arenas — does the runtime/concurrency expert agree, and
   what is the merge story for answers that must land in one store?
1. **Determinism.** VQL parallel scans/joins and VPL data-parallel `map` must define whether results
   stay ordered. `map`/`filter`/`fold` currently preserve rank order by construction
   (`chainCells(... | views::transform ...)`). What ordering contract does each frontend promise,
   and does `fold`'s left-associativity survive parallelization?
1. **Choice-stack ownership.** For parallel vprolog, do we commit to per-worker arenas + per-worker
   choice stacks (OR-parallelism only, no shared trail), and explicitly defer AND-parallelism? What
   happens to `cut`'s `cutBarrier` (`vortex/vortex_vm.cpp:197`) when choice points live on different
   workers?
1. **Primitive naming.** Given `spawnThread` already means "spawn green cursor"
   (`vortex/vortex_host.cpp:526`) and the rank is `d.cursors`, what do the OS-thread primitives get
   called at the C++ layer and in each language, so no reader confuses the two? (Language-surface
   expert.)
1. **GC under threads.** Who may call `compact()`, and when? Does GC become a stop-the-world barrier
   across all worker arenas, or does each worker GC its own arena independently?
1. **Config, not constants.** `schedulerCycleBudget` and `gcIntervalFrames` already come from
   `system://settings` (`vortex/vortex_host.cpp:128`, `:140`). What new knobs (worker count,
   parallel threshold) must live in system xanadocs rather than as literals, per the governance
   rule?

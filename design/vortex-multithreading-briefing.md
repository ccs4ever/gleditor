# Vortex multithreading study — ground-truth briefing

This briefing is the shared, code-verified factual basis for the multithreading study. Every
subagent must treat it as authoritative over any prior assumption. File and line references point at
`apps/common/xanadu/` in the `study/vortex-multithreading` worktree.

## The question under study

Vortex today has only *green threads*: cheap cursors minted into a manifold and advanced by a
cooperative scheduler. The owner is concerned this becomes a Python-GIL-style bottleneck. The study
must evaluate true (OS/hardware) multithreading in the Vortex VM, its downstream impact on the
frontends (VQL, VPL, vprolog) and the Vortex stdlib, and how each frontend would expose true thread
primitives to its language.

## Verified facts about the current model

### Execution core

- `VortexCore` (`vortex/vortex_core.hpp`) holds exactly one `ArenaManifold &arena_`, one
  `CellRef home_`, and one `SystemDimensions dims_`. **Every** structural primitive — `link()`,
  `value()`, `splice()`, `newCell()`, `breakLink()`, `pin()`, `collectGarbage()`, memo record/lookup
  — mutates that single shared arena. This single shared arena is the GIL-analogue: it is the one
  piece of state every cursor touches on nearly every opcode.

- `CellValue = std::variant<std::string, double, std::int64_t, bool>`.

### Virtual machine

- `VortexVM` (`vortex/vortex_vm.hpp`) owns a `VortexCore &core_` plus **unsynchronized mutable
  maps/vectors**:

  - `std::unordered_map<CellRef, OpcodeKind> opcodeMap_`
  - `std::unordered_map<CellRef, std::string> memoizedOps_`
  - `std::unordered_map<CellRef, CellRef> cursorPC_` (per-cursor program counter)
  - `std::vector<ChoicePoint> choiceStack_` — a **single global** choice-point stack, not
    per-cursor.

- The scheduler is cooperative and single-threaded. `VortexVM::stepScheduler()`
  (`vortex_vm.cpp:1076`) reads `activeCursors()` off home's `+d.cursors` rank and calls
  `step(cursor)` on each in a plain `for` loop on the calling thread. There is no preemption, no OS
  thread, no lock.

- `spawnCursor()` (`vortex_vm.cpp:127`) mints a cell, records its PC, links it onto the tail of the
  `+d.cursors` rank. A "cursor" is a green thread. Nothing about it is an OS thread.

- Backtracking (`pushChoicePoint`/`backtrack`, `vortex_vm.cpp`) snapshots and restores arena state
  through `core_.arena().mark()` and `core_.arena().release(mark)`. This is **global arena
  checkpoint/rollback** and is inherently serial: a mark names a point in the arena's dense vectors.

### Arena manifold (the shared heap)

- `ArenaManifold` (`zigzag/arena_manifold.hpp`) is "dense vectors only, no ops, dies with its
  owner." It is **not** thread-safe and is designed as a single-owner scratch space.
- It supports a **copy-on-write overlay** over a base `Manifold`: reads fall through to the base;
  the first write to a cell shadows it (copies the whole cell, its link run, its content run into
  the arena) keeping the base `CellRef` as its identity. The base is never mutated.
- `mark()`/`release()`/`discard()` are choice-point / undo-by-truncation / cut. `compact()` is
  refused while a mark is outstanding. `promote()` is the only path to persistence and maps refs.
- Arena refs carry an `ephemeralBit`; `Store::setLink()` and `Manifold::applyStructure()` refuse an
  ephemeral ref, so a leaked arena ref is caught by pre-existing R8 machinery.

### Host and frontends

- `VortexHost` (`vortex/vortex_host.hpp/.cpp`) wires the VM, stdlib, and the three frontend
  compilers (VQL, VPL, vprolog) over one core/arena. Note
  `compileAndAttachVQL(..., bool spawnThread)` (`vortex_host.cpp:513`): `spawnThread == true` only
  calls `vm_.spawnCursor(...)` — **the name "thread" already means "green cursor" in the code
  today.** A real-threads design must reckon with this existing naming.
- A system GC cursor is spawned (`vortex_host.cpp:86`, `"sys:gc"`); stdlib `call` spawns cursors
  (`vortex_stdlib.cpp:2075,2093`).
- VQL (`vql/`), VPL (`vpl/`), vprolog (`vprolog/`) each have lexer/parser/compiler that emit Vortex
  opcodes; `vql/compiler.cpp:866` and `vpl/compiler.cpp:1121` each `spawnCursor(entryOp, "main")`.
- vprolog runs on the VM's C++ logic opcodes (`Unify`, `Choice`, `Fail`, `Cut`, `ChoicePoint`) and
  the single global `choiceStack_`.

### Logic programming (vprolog / Vlog)

- Opcodes `Unify, IsVar, MakeVar, MakeTerm, Deref, Choice, Fail, Cut` operate over the arena trail
  and the single global `choiceStack_`. `ChoicePoint` captures `cursor`, `altOp`, `zigzag::Mark`,
  `stackFrame`, `cutBarrier`. Because the mark is a global arena checkpoint and the stack is global,
  concurrent backtracking by two OS threads over one arena is not expressible today.

## What "true thread primitives per frontend" must address

- VQL: query parallelism (parallel scans/joins over the manifold or multi-store arena), and whether
  results stay deterministic.
- VPL: data-parallel array operations (`map`/`filter`/`fold` already use `function_ref`) — the most
  natural fit for real parallelism.
- vprolog: parallel search (AND/OR parallelism), which collides hardest with the global choice stack
  and global arena mark/release.
- stdlib: which `map`/`filter`/`fold`/`call` surfaces become parallel, and what memoization
  (`getOrCreateMemoPin`/`recordMemo`) does under contention.

## Constraints the study must respect (from AGENTS.md / README)

- C++23, GNU Make, no CMake. clang-format 19, `make format`/`lint`/`check` gates.
- New C++ must justify why it is not Vortex (hardware/driver/allocator/raw-OS only).
- No sentinels in APIs (`std::optional`/`std::expected`); setters chain.
- On-disk/wire invariants are fixed: `sizeof(CompactOpNode) == 64`, 64 KiB Merkle pieces,
  append-only. A threading design must not break these.
- Configuration lives in system xanadocs, not magic numbers.

## Deliverable

A single design document, `design/vortex-multithreading-study.md`, in this worktree, synthesizing
the expert sections, with a clear recommendation, a staged plan, and an honest risk/pitfalls
section. It must be `make format`/`mdl`-clean (mdformat `--wrap 100`, fenced code, no bare headers).

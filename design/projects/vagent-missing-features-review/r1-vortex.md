# Round 1 critique: Vortex & Spool-Mesh Expert (design/vagent-metacircular-missing-features.md)

Paths below are relative to the repo root. Tally: 4 BLOCKING, 4 MAJOR, 4 MINOR.

## BLOCKING

### 1. (§4.2, lines 314–326, 341–365) Cross-Arena Raw CellRef Leaks in Swarm Messaging & Promotion

Full title: Cross-Arena Raw CellRef Leaks in Multi-Agent Swarm Messaging and Arena Promotion

**Problem:** Pattern B specifies a distributed swarm (`CompilerCoordinator`, `LexerWorker`,
`ParserWorker`, `CheckerWorker`, `EmitterWorker`) communicating via asynchronous performatives
(`tell AstConstructed(ast_root)`, `achieve ValidateAst(ast_root)`, `tell ImageLowered(image_root)`).
It then has `CompilerCoordinator` call `let ver = std:zigzag:promote(image_root)`. This violates the
foundational Spool-Mesh isolation model: actors share no mutable memory, each actor runs in an
isolated `WorkerContext` with its own `ArenaManifold`, and raw ephemeral `CellRef`s cannot be passed
across actor boundaries or promoted by a foreign actor.

**Evidence:**

- `apps/common/xanadu/vortex/mesh_actor.hpp:160–162`: "Actors share no mutable contexts; the
  executor loads verified immutable programs and runs the VM with grant limits."
- `apps/common/xanadu/identity/identity_layout.hpp:303`: "A process address survives transport
  without exporting an arena CellRef."
- `apps/common/xanadu/vagent/percept_codec.hpp:31–35`: `PerceptSpec` carries scalar `CellValue`
  primitives (ints, floats, bools, strings); it does not transport raw cross-arena `CellRef`s.
- `apps/common/xanadu/zigzag/arena_manifold.cpp:1738–1740`:
  `zigzag::promote(store, parent, from, root)` asserts `from.contains(root)`. If
  `CompilerCoordinator` invokes `promote(image_root)`, `image_root` was minted in `EmitterWorker`'s
  arena and is NOT present in `CompilerCoordinator`'s arena. The call immediately returns
  `std::nullopt`!

**Fix:** Specify that intermediate AST and bytecode structures passed between distributed actors
must be committed as immutable Store microversions (`MicroversionId` strings) or serialized
record/percept payloads. In Pattern B, `EmitterWorker` must execute `std:zigzag:promote(image_root)`
locally within its own arena and send the resulting `MicroversionId` string
(`tell ImageLowered(version)`) to `CompilerCoordinator`. Cross-actor transmission of un-promoted
ephemeral `CellRef` handles must be explicitly forbidden in the specification.

### 2. (§5.1, §5.2, lines 380–415; §7, line 502) Inverted spawn Lifecycle & Dangling Lineage Base

Full title: Inverted MeshActorRuntime::spawn Lifecycle, Dangling Lineage Base, & Turn Atomicity

**Problem:** Section 5.2 and Proposal P29 state that `std:actor:spawn_image` dispatches to
`MeshActorRuntime::spawn()` "with ActorGenesis", and claims step 6 runs "Executor Pump: run
Initialize handler and enter turn loop". This contradicts the actual C++ API, creates a dangling
pointer in `WorkerContext`, and violates turn atomicity:

1. `MeshActorRuntime::spawn()` takes `(identity, request, wallNow, lineage)`. `ActorGenesis` is NOT
   an input argument; it is constructed internally by `spawn()` and passed to `genesisHook_`
   (`mesh_actor.cpp:221`).
1. `WorkerContext` stores `baseManifold` as a non-owning raw pointer (`const Manifold* base`). If
   `spawn_image` reconstructs `Store::rebuildManifold(version)` as a local temporary, the spawned
   actor's `WorkerContext` is left holding a dangling pointer, causing use-after-free crashes.
1. An actor executing `spawn_image` is in the middle of its own turn. It cannot synchronously pump
   the child actor's turn loop; child activation must be queued via a P4 wake.

**Evidence:**

- `apps/common/xanadu/vortex/mesh_actor.hpp:218–222`:

  ```cpp
  std::expected<ActorPid, ActorError>
  spawn(const xanadu::identity::Fingerprint &identity,
        const ActorMessage &request,
        std::chrono::system_clock::time_point wallNow,
        std::optional<ActorLineage> lineage = std::nullopt);
  ```

- `apps/common/xanadu/vortex/worker_context.hpp:59–62`: `WorkerContext` takes
  `const Manifold *baseManifold = nullptr`.

- `apps/common/xanadu/vortex/mesh_actor.cpp:164–170`:
  `grant->programRef != request.programRef || grant->baseRef.empty()` triggers
  `ActorError::NotAuthorized`.

- `apps/vagent/main.cpp:258–263, 298–303`: The host runner manages the lifetime of `fold` (the
  rebuilt `Manifold`), passes `ActorLineage{fold.get(), &lineage}`, and queues the `Initialize`
  handler via `runtime.wake(*pid)`.

**Fix:** Rewrite §5.1, §5.2, and Proposal P29:

1. Replace "spawn() with ActorGenesis" with
   `MeshActorRuntime::spawn(identity, request, wallNow, ActorLineage{fold, store})`.
1. Explicitly specify that the `std::unique_ptr<Manifold>` produced by `Store::rebuildManifold` must
   be retained in an actor runtime image cache keyed by `MicroversionId` to ensure the lineage base
   outlives the child actor incarnation.
1. Specify that `spawn_image` queues a P4 local wake (`runtime.wake(childPid)`) and returns the
   `ActorPid` immediately; the child's `Initialize` handler runs cooperatively in subsequent runtime
   `pumpOne` cycles.

### 3. (§6.3, line 487; §7, line 503) Trail Rollback & Backtracking from #Choice/#Fail in match

Full title: Silent Destruction of Side Effects and Choice Stack Leaks from #Choice/#Fail Lowering

**Problem:** Section 6.3 states: "Guards evaluate within an isolated `#Choice` frame; failure
triggers backtrack to the next arm." Proposal P30 specifies lowering `match` to
`#Choice`/`#Branch`/`#Equal`. In `VortexVM`, `#Choice` pushes a `ChoicePoint` containing an arena
mark (`core_.arena().mark()`). If `#Cut` is not emitted when an arm succeeds, the choice point
remains on `choiceStack_` across function returns! Any subsequent `#Fail` in the program will
backtrack into the match's remaining arms, and `core_.arena().release(mark)` will **destroy all
arena cells minted in the arm body and subsequent execution**. Furthermore, if a guard has side
effects or allocates cells, `#Choice` backtracking will silently wipe them out.

**Evidence:**

- `apps/common/xanadu/vortex/vortex_vm.cpp:317–326`: `pushChoicePoint` captures mark
  (`core_.arena().mark()`).
- `apps/common/xanadu/vortex/vortex_vm.cpp:345`: `backtrack` calls `core_.arena().release(cp.mark)`.
- `apps/common/xanadu/zigzag/arena_manifold.hpp:730–732`: "Undo everything since @p m: replay its
  trail tail in reverse, then truncate. Cells minted under the mark are *gone*, not garbage."
- `apps/common/xanadu/vortex/vortex_vm.cpp:1713–1730`: Opcode `Return` pops `+d.stack` and restores
  `+d.vars`, but does NOT touch `choiceStack_`.
- `apps/common/xanadu/vortex/vortex_stdlib.cpp:3565`: Existing control flow uses `#Equal` and
  `#Branch` pairs without `#Choice`.

**Fix:** Specify that VAgent pattern matching compiles to deterministic decision trees using
`#Equal` and `#Branch` testing constructor tags and guard booleans, completely avoiding
`#Choice`/`#Fail`. If logic-style backtracking is ever desired, the specification MUST mandate that
`#ChoiceDepth` is recorded before evaluating the match and `#Cut` is unconditionally executed upon
entering the succeeding arm to discard remaining alternatives before returning or executing arm body
side effects.

### 4. (§3.2, line 215; §5.2, lines 395, 407–409) Missing d.mesh.relocation Marker for OpHandle

Full title: Missing d.mesh.relocation Marker Invariant for OpHandle Cells Surviving promote()

**Problem:** Lowering program ASTs into an executable program image requires emitting jump, branch,
and continuation targets, which are represented as `ValueKind::OpHandle` cells. `zigzag::promote()`
strictly refuses any ephemeral `OpHandle` cell unless it carries a relocation marker along
`+d.mesh.relocation -> "cell"`. The design does not specify `d.mesh.relocation` or the relocation
manifest in `emitter.vagent` or `std:zigzag:promote`, guaranteeing that `promote(programRoot)` will
unconditionally fail with `std::nullopt`!

**Evidence:**

- `apps/common/xanadu/zigzag/arena_manifold.cpp:1812–1824`:

  ```cpp
  if (from.valueKindOf(cell) == xanadu::ValueKind::OpHandle &&
      isEphemeral(cell) && !from.isProxy(cell)) {
    const auto target = from.handleTarget(cell);
    if (!target) return std::nullopt;
    if (isEphemeral(*target)) {
      if (!xanadu::isCellRelocationHandle(from, cell) ||
          !seen.contains(*target))
        return std::nullopt;
      deferred.push_back(cell);
      continue;
    }
    if (!store.getCompactOp(*target)) return std::nullopt;
  }
  ```

- `apps/common/xanadu/vagent/fragment.cpp:45–50`: Marks internal refs with `d.mesh.relocation`.

- `apps/common/xanadu/vagent/lineage.cpp:40–90`: "The dense half of the relocation contract: a
  `d.mesh.relocation` slot's Int64-carried referent never reaches... (root not an arena cell, a
  relocation cycle, or over budget)".

**Fix:** Explicitly specify the relocation contract in §3.2 and §5.2: `emitter.vagent` and
`fragment.vagent` must mark all intra-image opcode target cells along `+d.mesh.relocation -> "cell"`
so that `zigzag::promote()` recognizes them as relocatable ephemeral handles and rewrites their
op-index targets to persistent `CompactOpNode` indices during microversion sealing.

______________________________________________________________________

## MAJOR

### 1. (§3.2, lines 185, 215–219; §7, line 500) Missing Store Binding and Fallibility on promote()

Full title: std:zigzag:promote Lacks Mutable Store Binding and Sentinel-Free Fallibility

**Problem:** Section 3.2 defines `promote(root: CellRef) -> string` as returning a bare string.
First, `zigzag::promote(store, parent, arena, root)` requires a mutable `xanadu::Store &store`. In
`WorkerContext` and `VortexStdLib`, `boundStore_` is currently `const xanadu::Store*`
(`vortex_stdlib.hpp:515`). Second, promotion can fail (`budget.maxOps` exceeded, uncontained root,
marks outstanding, relocation cycle, or unattached store), returning `std::nullopt`. Returning a
bare `string` forces an empty-string sentinel, violating House Style Rule 3 and §8.3.

**Evidence:**

- `apps/common/xanadu/zigzag/arena_manifold.hpp:930–934`: `promote()` returns
  `std::optional<Promoted>`.
- `apps/common/xanadu/vortex/vortex_stdlib.hpp:515`: `const xanadu::Store *boundStore_{nullptr};`.
- `design/vagent-metacircular-missing-features.md:526–532`: Refuses sentinel values in surface APIs.

**Fix:**

1. Define `promote(root: CellRef, budget: Option[int] = none) -> Result[string, ZigzagError]`.
1. Update Proposal P27 to provide a mutable `Store* boundStore_` (or actor store sink) on
   `VortexStdLib` and return a typed `Result` representing promotion success or refusal reason.

### 2. (§2.2, lines 147–152; §7, line 499) Hallucinated consumeWork(bytes) & maxOutputBytes Cap

Full title: Hallucinated consumeWork(file_byte_size) and Conflating maxOutputBytes with Ingestion

**Problem:** Section 2.2 states: "`WorkerContext::arena.consumeWork(file_byte_size)` prevents memory
exhaustion" and caps input files at `ActorGrant::maxOutputBytes` (default 32 KiB). In C++,
`ArenaManifold::consumeWork()` takes **no arguments** and decrements operation cycles, not memory
bytes (`arena_manifold.hpp:486–492`). Memory allocation is checked by `scratchLimit_`
(`arena_manifold.hpp:480`). Furthermore, `ActorGrant::maxOutputBytes` is hard-capped at 32 KiB
(`mesh_actor.cpp:166`) and governs actor wire output replies. Bounding file ingestion to
`maxOutputBytes` would make it impossible for VAgent to compile any source file larger than 32 KiB!

**Evidence:**

- `apps/common/xanadu/zigzag/arena_manifold.hpp:486–492`: `void consumeWork() const;` takes 0 args.
- `apps/common/xanadu/vortex/mesh_actor.cpp:166`: `grant->maxOutputBytes > 32768` triggers
  `ActorError::NotAuthorized`.

**Fix:** Remove the hallucinated `consumeWork(file_byte_size)` invocation. Charge string allocation
against `arena.intern()` / `scratchLimit_`. Remove the `maxOutputBytes` restriction on input reading
and replace it with a dedicated file ingestion capability bound or `WorkerContext` scratch memory
limit.

### 3. (§3.2, lines 185–202; §7, line 500) Missing Fallibility & Pseudo-Type any on std:zigzag

Full title: Missing Fallibility and Pseudo-Type any on Proposed std:zigzag Primitives

**Problem:** Section 3.2 specifies `make_cell(text: string = "") -> CellRef`,
`text_of(cell: CellRef) -> string`, `set_value(cell: CellRef, value: any) -> CellRef`, and
`resolve_dim(name: string) -> DimRef`. In `ArenaManifold`:

- `makeCell` can fail if `allocationLimit_` or `scratchLimit_` is reached.
- `textOf` fails if `cell` is not contained in the arena (`contains(cell) == false`).
- `setValueBits` can fail if `cell` is invalid or a read-only base cell.
- `findOrMintDimension` can fail if dimension allocation limits are exceeded. All existing
  `std:zigzag` primitives in `VortexStdLib` return `Opt<CellRef>` (`vortex_stdlib.cpp:5036–5120`).
  Returning bare types forces sentinel values (`noCell`, `""`, `-1`), violating Rule 3. Also,
  `value: any` uses the forbidden pseudo-type `any`.

**Evidence:**

- `apps/common/xanadu/vortex/vortex_stdlib.cpp:5036–5120`: All `zz*` primitives return
  `std::optional`.
- `apps/common/xanadu/vagent/prelude/meta.vagent:15`: "`<any>` is not a type name a source file may
  write."

**Fix:** Update all signatures to return `Option[T]` or `Result[T, ZigzagError]`:

- `def make_cell(text: string = "") -> Option[CellRef]`
- `def text_of(cell: CellRef) -> Option[string]`
- `def set_value(cell: CellRef, value: CellValue) -> Option[CellRef]`
- `def resolve_dim(name: string) -> Option[DimRef]`

### 4. (§7, line 502) Layering Inversion: Exposing spawn_image in VortexStdLib without Host Shim

Full title: Layering Inversion: Exposing std:actor:spawn_image in VortexStdLib without Host Shim

**Problem:** Proposal P29 proposes placing `std:actor:spawn_image` in
`apps/common/xanadu/vortex/vortex_stdlib.cpp`. But `VortexStdLib` is an engine-level library used by
multiple non-actor tools (`vquery`, `vpl`, `vprolog`, `vortex_test`). It does not hold a reference
to `MeshActorRuntime` (`mesh_actor.hpp`). Injecting `MeshActorRuntime` directly into `VortexStdLib`
violates layer separation.

**Evidence:**

- `apps/common/xanadu/vortex/vortex_stdlib.hpp:64–545`: `VortexStdLib` has `core_`, `vm_`,
  `boundStore_`, but no `MeshActorRuntime` dependency.
- `apps/vagent/main.cpp:276–296`: Actor spawn is driven via `VagentAdapter` callback hooks.

**Fix:** Specify that `VortexStdLib` exposes `std:actor` via an injectable host callback hook
(`ActorSpawnHandler`), wired by `VagentAdapter` when running under `MeshActorRuntime`, returning
`ActorError::NotAuthorized` when running in a headless non-actor VM.

______________________________________________________________________

## MINOR

### 1. (§3.2, line 215; §9, line 548) Add Explicit budget Parameter to std:zigzag:promote

Section 9 Question 2 agrees that `promote` should accept an explicit `budget` parameter, but §3.2
omits it from the signature. Include `budget: Option[int] = none` in the signature, defaulting to
the actor's remaining `maxCells` quota.

### 2. (§3.2, line 198) Replace Forbidden any with CellValue Union in VAgent Surface

The document writes `value: any` for `set_value`. `any` is not a valid VAgent type. Replace with
`CellValue` (tagged union `Int | Float | Bool | Str | CellRef`).

### 3. (§3.2, line 215) Specify Default Microversion Parent for std:zigzag:promote

`zigzag::promote(store, parentVersion, arena, root)` takes `parentVersion`. The document does not
state how `parentVersion` is resolved. It should specify that `parentVersion` defaults to
`store.primaryCurrentVersion()` (or empty if fresh store).

### 4. (§1, line 18; §4, line 254; §6.3, line 487) Clarify VM Opcode Banding and Taxonomy

The document mentions "57 Vortex opcodes" in `emitter.vagent`. The count is consistent with
`OpcodeKind` (57 opcodes), but §6.3 references `#Choice`, `#Fail`, `#Equal`, `#Branch` while
omitting `#Cut` and `#ChoiceDepth`. Clarify that Band A and Band B opcodes adhere to `OpcodeKind` in
`vortex_vm.hpp:28–94`.

______________________________________________________________________

VERDICT: NOT CONVERGED

# Round 1 Critique: Vortex & Spool-Mesh Expert

**Reviewer**: Critic 2: Vortex & Spool-Mesh Expert (`vortex`)\
**Target Document**: `design/vagent-harmonization-and-missing-features-implementation-plan.md`\
**Companion Documents**:

- `design/vagent-metacircular-missing-features.md` (Proposals P26–P30, P36–P37)
- `design/vagent-language-harmonization.md` (Proposals P31–P35)
- `design/vagent-metacircular-compiler-implementation.md`

**Codebase Anchors Cross-Checked**:

- `apps/common/xanadu/vortex/vortex_core.hpp`, `vortex_core.cpp`
- `apps/common/xanadu/vortex/vortex_vm.hpp`, `vortex_vm.cpp`
- `apps/common/xanadu/vortex/vortex_stdlib.hpp`, `vortex_stdlib.cpp`
- `apps/common/xanadu/vortex/mesh_actor.hpp`
- `apps/common/xanadu/zigzag/arena_manifold.hpp`, `arena_manifold.cpp`
- `apps/common/xanadu/vagent/executor.cpp`, `decls.cpp`, `compiler.cpp`, `meta.cpp`, `meta.hpp`

______________________________________________________________________

## 1. Executive Summary & Verdict

The implementation plan establishes a compelling engineering blueprint for transitioning VAgent
into a self-hosting metacircular language. The foundational commitments to Nelsonian persistence
invariants, Spool-Mesh turn boundaries, and universal 1-based indexing are well aligned with the
accepted architecture.

However, a rigorous audit against Vortex VM execution mechanics, choice point lifecycle rules, and
the live `MeshActorRuntime`/`VagentExecutor` codebase reveals **two BLOCKING safety hazards** and
**two MAJOR specification gaps**:

1. **Choice Stack / Trail Desynchronization on Arm Rollback (BLOCKING)**: Section 5.2 (Track 4B)
   specifies calling `arena.release(m_arm)` on speculative arm failure, but omits cutting the VM
   choice stack back to the pre-arm barrier (`vm.cut(cp_arm)`). In Vortex VM, choice points contain
   arena marks and trail indices; releasing arena memory underneath un-cut choice points corrupts
   the VM choice stack and trail unwind invariants.

1. **Intention Scheduler Arm Deliberation & `bodyName` Desync Trap (BLOCKING)**: Section 5.2 targets
   `executor.cpp:3030–3090` (the delayed fault backoff ladder) rather than the execution failure
   site at lines 3240–3275, meaning arm failures will park for multiple turns rather than
   deliberating intra-turn. Crucially, switching to an auxiliary arm updates `plan` but neglects
   `bodyName` and `owner`; this triggers an immediate `liveName != bodyName` failure in
   `executor.cpp:3233`, faulting the intention with `"body_replaced"`.

1. **Phantom VM Opcode Names (MAJOR)**: Section 5.1 and Section 1 cite non-existent opcodes
   `#Equal` and `#ExtractField`. The real Vortex VM ISA (`vortex_vm.hpp:28–143`) uses
   `OpcodeKind::Eq`, `OpcodeKind::Find` (for record member lookup), and `OpcodeKind::Linked` (for
   rank navigation).

1. **Missing `std:map` Stdlib Registration & Non-String Keys (MAJOR)**: Section 2.3 adds C++ helper
   functions in `VortexStdLib`, but omits registering the `std:map` module and exporting its
   symbols in `VortexStdLib::bootstrap()`. Furthermore, `mapGet`/`mapSet` are typed only for
   `std::string_view key`, conflicting with Track 3E's mandate supporting `Int` and `CellRef` keys.

**VERDICT: REVISE** (2 BLOCKING, 2 MAJOR, 2 MINOR).

______________________________________________________________________

## 2. Detailed Technical Critique by Focus Area

### 2.1 VM Execution Semantics & Bytecode Graph Lowering

#### Phantom Opcode Names vs. Vortex VM ISA (Section 5.1 & Section 1)

Section 5.1 (Track 4A Deliverable 1) states:

> Lower `match ... with` into deterministic decision trees using `#Equal`, `#Branch`, and
> `#ExtractField` opcodes.

This wording also appears in Section 1 (Mermaid diagram), Section 7.1 (Test Matrix), Section 8
(Proposal Register), and Section 9 (Refused Alternatives).

**Codebase Reality**:
Inspection of `apps/common/xanadu/vortex/vortex_vm.hpp:28–143` confirms:

- The equality opcode is `OpcodeKind::Eq`, not `#Equal`.
- There is **no opcode named `ExtractField`** anywhere in the Vortex VM or the VAgent codebase.
- In Vortex bytecode, extracting a named record member is performed by `OpcodeKind::Find`
  (`apps/common/xanadu/vagent/compiler.cpp:1462–1471`), which walks the subject's `+d.step` rank for
  the child carrying a matching `+d.name`.
- Traversing structural Crum constructor arguments or tuple ranks uses `OpcodeKind::Linked`
  (`compiler.cpp:1473–1486`), navigating `+d.grab` (first argument) and `+d.step` (subsequent
  elements).

Lowering must target real, existing Vortex VM opcodes (`Eq`, `Branch`, `Find`, `Linked`).

#### Pattern Matching Determinism & Trail Safety

The mandate that pattern matching must compile to deterministic decision trees with **zero `#Choice`
and zero `#Fail` opcodes** is sound and essential.

In Vortex VM (`vortex_vm.cpp:331–377`), each `#Choice` allocates a `ChoicePoint` containing
`cp.mark = core_.arena().mark()`. If pattern matches emitted choice points, an uncut choice point
would leak across function/plan returns. Any subsequent goal failure or backtracking would unwind
the trail and erase legitimate side effects (such as cell allocations, field mutations, or staged
events) executed within the matched branch.

Deterministic branching using `OpcodeKind::Eq` and `OpcodeKind::Branch` guarantees zero entries are
pushed onto `vm.choiceStack_`, completely isolating pattern matching from trail rollbacks.

______________________________________________________________________

### 2.2 Spool-Mesh Turn Atomicity & Intra-Turn Speculation

#### Choice Stack Desynchronization on Arm Rollback (Section 5.2 Deliverable 3)

Section 5.2 (Track 4B Deliverable 3) states:

> Before executing an arm body, capture an arm-scoped arena mark: `m_arm = arena.mark()`. If the arm
> body fails prior to external commit (`#Fail`, deadline expiry, subgoal failure), call
> `arena.release(m_arm)` to roll back speculative cells and un-stage pending `+d.outbox` entries.

**The Fatal Flaw**:
`arena.release(m_arm)` only rewinds the `ArenaManifold` heap (cells, links, values, and trail). It
**does not touch the VM's `choiceStack_`**!

During arm execution, statements or goal-directed subgoals may push choice points (even with
statement-level `bounded()` sequences, a failure during evaluation leaves active choice points). If
the executor calls `arena.release(m_arm)` while choice points pushed *after* `m_arm` remain on
`vm.choiceStack_`:

1. The choice points on `choiceStack_` retain `cp.mark` values that now point past the truncated
   arena boundary (`arena_manifold.cpp:1612–1650`).
1. When `vortex_vm.cpp:360` later cuts or backtracks those choice points, it executes
   `core_.arena().discard(cp.mark)`. Because the mark was already truncated, this triggers an arena
   assertion or silent trail desynchronization.
1. This exact failure mode is why `executor.cpp:269–271` strictly cuts before release:

   ```cpp
   vm.cut(1);
   arena.release(m0);
   vm.backtrack(exec.cursor);
   ```

**Required Invariant**:
Before executing an arm body, the executor must capture *both* the arena mark and the choice depth:

```cpp
const auto m_arm  = arena.mark();
const auto cp_arm = vm.choiceStack().size();
```

On arm failure, the executor **must cut choice points before releasing the arena mark**:

```cpp
vm.cut(cp_arm);
arena.release(m_arm);
```

#### Outbox Un-staging Mechanics

In `apps/common/xanadu/vagent/executor.cpp:360–385`, `stageOutbox` links fresh cells along
`agent -> dims.outbox`. Because these entries and their links are created in `arena` after `m_arm`,
`arena.release(m_arm)` cleanly restores the `dims.outbox` rank tail and truncates the speculative
message cells. When the turn concludes, `executor.cpp:3375` traverses only committed outbox cells.
Un-staging via `arena.release(m_arm)` is topographically sound, provided choice points are cut
first.

______________________________________________________________________

### 2.3 BDI Intention Scheduler Deliberation Loop & State Consistency

#### The `bodyName` Desync Trap in Arm Reconsideration (Section 5.2 Deliverable 4)

Track 4B Deliverable 4 specifies:

> If an untried arm matches, update intention `plan`, reset `step := 0`, clear `faulted`, and set
> `ready := 1`.

**The Fatal Flaw**:
In `apps/common/xanadu/vagent/executor.cpp:3212–3236`, the executor verifies that the plan body
fragment has not been mutated or replaced since adoption:

```cpp
const auto bodyName = [&]() -> std::string {
  if (const auto rec = varsRecord(arena, core, intention, "bodyName", false))
    if (const auto v = arena.linked(*rec, core.dims().values, DimVector::POS))
      return std::string{arena.textOf(*v)};
  return {};
}();
...
std::string liveName;
if (const auto head = arena.linked(planCell, core.dims().spin, DimVector::POS))
  if (const auto name = arena.linked(*head, core.dims().name, DimVector::POS))
    liveName = std::string{arena.textOf(*name)};
if (liveName != bodyName) {
  faultIntention(intention, "body_replaced");
  continue;
}
```

When `decls.cpp` compiles a multi-arm plan:

- Primary arm 1 body fragment is labelled `PlanName:body`.
- Auxiliary arm 2 body fragment is labelled `PlanName$2:body`.

If the intention scheduler updates `plan` to arm 2 without updating `bodyName` on the intention
record, the executor will evaluate `liveName ("PlanName$2:body") != bodyName ("PlanName:body")` on
the very next step and immediately fault the intention with `"body_replaced"`!

Furthermore, if the new arm belongs to a mounted behavior, `owner` must also be updated.

**Required Action**:
Deliverable 4 must specify:

```cpp
meta.setIntentionField(intention, "plan", newClause);
meta.setIntentionField(intention, "bodyName", liveBodyName);
if (newOwner != noCell) meta.setIntentionField(intention, "owner", newOwner);
meta.setIntentionField(intention, "step", 0);
meta.setIntentionField(intention, "faulted", 0);
meta.setIntentionField(intention, "ready", 1);
```

#### Deliberation Hook Location: In-Turn vs. Delayed Ladder

The implementation plan references `executor.cpp:3030–3090` as the target for intention
reconsideration.

However, lines 3030–3090 represent the **delayed fault backoff ladder**, which only triggers after
`vagent.intention_backoff` turns have elapsed. The actual point of plan arm failure is at
`executor.cpp:3269`:

```cpp
if (!ok) {
  faultIntention(intention, "body");
  continue;
}
```

If the untried-arm deliberation loop is not integrated directly at line 3269, any arm failure will
call `faultIntention`, parking the intention for 2+ turns before reconsideration even begins!

**Required Action**:
At the arm execution failure site (`executor.cpp:3269`):

1. Immediately roll back speculative arm state: `vm.cut(cp_arm); arena.release(m_arm);`.
1. Append the failed clause to the intention's `tried` rank along `+d.step`.
1. Query `MetaRuntime::selectClause` with the updated `tried` set.
1. If an untried arm matches, re-arm the intention (`plan`, `bodyName`, `owner`, `step := 0`) and
   proceed to execute the new arm within the current turn (subject to `maxCycles` turn limits).
1. Only if `selectClause` returns `nullopt` does the executor fault the intention or escalate to
   `GoalFailed`.

______________________________________________________________________

### 2.4 Map Subscript Lowering & Module Registration

#### Missing `std:map` Module Registration in Phase 1C

Track 4C specifies lowering `map[key]` to `std:map:get(map, key)` and `map[key] := val` to
`std:map:set(map, key, val)`.

However, in Phase 1C (Section 2.3), the deliverables list adding C++ methods (`createMap`, `mapSet`,
`mapGet`, `mapHas`, `mapKeys`, `mapKeyCell`) to `VortexStdLib`, but **completely omits registering
`std:map` as a Vortex module**.

In `apps/common/xanadu/vortex/vortex_stdlib.cpp:1459–1920`, `VortexStdLib::bootstrap()` exports
module symbols into `core_.dims().stdlib` using:

```cpp
CellRef modMap = getOrCreateModule("std:map");
exportSymbol(modMap, "get", opGet);
exportSymbol(modMap, "set", opSet);
exportSymbol(modMap, "has", opHas);
exportSymbol(modMap, "keys", opKeys);
```

Without this deliverable in Phase 1C, the VAgent compiler and runtime will fail to resolve
`std:map:get` during bytecode assembly.

#### Non-String Key Ingestion (`Int`, `CellRef`)

Track 3E (Section 4.5) requires supporting key types $K \in \{\text{Str}, \text{Int},
\text{CellRef}\}$.

However, `VortexStdLib::mapGet` and `mapSet` in `vortex_stdlib.hpp:173–178` are currently declared
taking only `std::string_view key`:

```cpp
void mapSet(CellRef mapRoot, std::string_view key, const CellValue &val);
std::optional<CellValue> mapGet(CellRef mapRoot, std::string_view key) const;
```

If keys can be integers or cell references, `VortexStdLib` must either:

- Provide overloaded/templated methods taking `const CellValue &key` and comparing keys via
  `CellValue` ordering, OR
- Canonically serialize integer and `CellRef` keys into deterministic string representations (e.g.
  `"42"`, `"cell:1024"`) before BST comparison.

Phase 1C and Phase 4C must explicitly document how non-string keys are compared in the 2-degree BST.

#### Lowering Differentiation: Assertive `map[k]` vs. Soft `map[k]?`

Track 3B defines:

- `map.get(k)`: returns `Opt<V>`.
- `map[k]?`: soft unwrap, branches to `cells_->fail` on `none`.
- `map[k]` / `map[k]!`: assertive hard unwrap, raises `ActorError::ContractViolation` (or
  `KeyNotFound`) on `none`.

Track 4C Deliverables 1 and 3 detail the lowering for `map[k]?`, but do not specify the bytecode
lowering for assertive subscripting `map[k]`. The compiler must emit a check on the `Opt<V>` result
that triggers a contract violation fault if the key is missing.

______________________________________________________________________

### 2.5 Fuel Accounting & Turn Quotas

#### Fuel Accounting Precision via `consumeWork()`

In Section 2.3 and Section 5.3, the plan mandates calling `core_.arena().consumeWork()` during tree
descent and iteration.

In `apps/common/xanadu/zigzag/arena_manifold.hpp:486–491`:

```cpp
void consumeWork() const {
  if (workRemaining_) {
    if (*workRemaining_ == 0)
      throw std::runtime_error("Vortex evaluation work budget exhausted");
    --*workRemaining_;
  }
}
```

`consumeWork()` is `const` and throws `std::runtime_error`, which unwinds cleanly to the actor turn
boundary.

To avoid double-counting fuel, the implementation must designate **`mapKeyCell` as the single
canonical descent function** where `consumeWork()` is called on each `d.left` and `d.right` hop.
Since `mapGet`, `mapHas`, and `mapSet` delegate to `mapKeyCell`, descent fuel is deducted exactly
once per hop. Iteration over `+d.step` in `mapKeys` must also call `consumeWork()` once per key
cell.

______________________________________________________________________

## 3. Categorized Findings & Required Actions

### BLOCKING 1: VM Choice Stack Desynchronization on Arm Rollback

- **Location**: `design/vagent-harmonization-and-missing-features-implementation-plan.md:384–387`
  (Section 5.2, Track 4B, Deliverable 3).
- **Issue**: Calling `arena.release(m_arm)` without cutting choice points leaves stale choice points
  on `vm.choiceStack_`, corrupting trail and mark structures upon subsequent backtrack or cut.
- **Remedy**: Update Deliverable 3 to explicitly mandate recording
  `cp_arm = vm.choiceStack().size()` alongside `m_arm = arena.mark()`, and calling `vm.cut(cp_arm)`
  strictly before `arena.release(m_arm)` on arm failure.

### BLOCKING 2: Intention Scheduler Arm Deliberation & `bodyName` Desync Trap

- **Location**: `design/vagent-harmonization-and-missing-features-implementation-plan.md:388–392`
  (Section 5.2, Track 4B, Deliverable 4).
- **Issue**: Switching `plan` to an untried arm without updating `bodyName` triggers an immediate
  `liveName != bodyName` failure in `executor.cpp:3233`, aborting execution with `"body_replaced"`.
  Additionally, targeting `executor.cpp:3030–3090` delays deliberation across turns rather than
  speculating within the current turn.
- **Remedy**:
  1. Add explicit updates for `bodyName` (and `owner`) to Deliverable 4 when re-arming an intention.
  1. Target the arm failure site in `executor.cpp:3240–3275` so untried arms are deliberated
     intra-turn before falling back to the delayed fault ladder.

### MAJOR 1: Phantom Opcode Names vs. Vortex VM ISA

- **Location**: Section 1, 5.1, 7.1, 8, 9 (Plan lines 48, 359, 501, 528).
- **Issue**: References to `#Equal` and `#ExtractField` do not correspond to any opcodes in
  `vortex_vm.hpp`.
- **Remedy**: Replace `#Equal` with `OpcodeKind::Eq`. Replace `#ExtractField` with
  `OpcodeKind::Find` (for record fields) and `OpcodeKind::Linked` (for Crum/tuple argument
  traversal).

### MAJOR 2: Missing `std:map` Module Registration & Typed Key Handling

- **Location**: `design/vagent-harmonization-and-missing-features-implementation-plan.md:160–178`
  (Section 2.3, Track 1C).
- **Issue**: Phase 1C omits registering `modMap = getOrCreateModule("std:map")` in
  `VortexStdLib::bootstrap()`, and does not specify how non-string keys (`Int`, `CellRef`) are
  ingested by `VortexStdLib::mapGet`/`mapSet`.
- **Remedy**:
  1. Add a deliverable to Track 1C for exporting `std:map` module symbols (`get`, `set`, `has`,
     `keys`) in `VortexStdLib::bootstrap()`.
  1. Define `CellValue` key support or canonical string serialization for `Int` and `CellRef` keys
     in `mapSet`/`mapGet`.

### MINOR 1: Bytecode Lowering for Assertive Subscript `map[k]`

- **Location**: `design/vagent-harmonization-and-missing-features-implementation-plan.md:406–413`
  (Section 5.3, Track 4C).
- **Issue**: Track 4C specifies lowering for soft `map[k]?`, but omits the bytecode lowering for
  assertive hard subscript `map[k]`.
- **Remedy**: Add explicit lowering for `map[k]` verifying `Opt<V>` presence and raising
  `ActorError::ContractViolation` (or `KeyNotFound`) on `none`.

### MINOR 2: Fuel Accounting Deduplication in Map Descent

- **Location**: `design/vagent-harmonization-and-missing-features-implementation-plan.md:165–175`
  (Section 2.3, Track 1C).
- **Issue**: `consumeWork()` is specified in multiple functions (`mapSet`, `mapGet`, `mapHas`,
  `mapKeyCell`), risking double-deduction.
- **Remedy**: Clarify that `mapKeyCell` is the sole descent function invoking `consumeWork()`,
  while `mapGet`, `mapHas`, and `mapSet` delegate their descent traversal to `mapKeyCell`.

______________________________________________________________________

## 4. Final Disposition

The architectural foundations of the plan are robust, but execution safety within the Vortex VM and
Spool-Mesh actor turn framework requires addressing the choice stack rollback invariants and
intention record synchronization.

**VERDICT: REVISE** (0 CONVERGED, 2 BLOCKING, 2 MAJOR, 2 MINOR).

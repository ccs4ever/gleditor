# Round 1 critique: System Architect (`design/vagent-shared-program-and-live-upgrade.md`)

Paths below are relative to the repo root. Tally: 1 BLOCKING, 4 MAJOR, 1 MINOR.

## BLOCKING

### 1. (§3.2, line 327) Runaway COW-Shadowing on Opcode Outputs and Broken Turn Cleanup

Full title: Runaway Copy-On-Write Shadow Overhead on Opcode `-d.grab` Slots and Broken Turn-End
Cleanup

**Problem:** The specification places compiled opcode instruction cells and their `-d.grab` output
cells in the frozen base `programManifold`. During execution, the Vortex VM writes results into
opcode output cells. Because these output cells are base cells (`!isEphemeral(ref)`), every
instruction write triggers `ArenaManifold::shadow(outCell)`
(`apps/common/xanadu/zigzag/arena_manifold.cpp:1464`), which allocates a new `CellSlot` (64 bytes),
copies content spans and links, performs a heap node allocation in `overlay_`
(`std::unordered_map`), and pushes to `shadowOrder_`. For a standard turn executing 1,000–2,000
instructions, this shadows thousands of base cells, allocating hundreds of kilobytes of heap memory
per turn.

More critically, the proposed turn-end cleanup in §3.2 ("At the start of a turn, the executor
records an `ArenaManifold::Mark turnMark = arena.mark()` before running transient scratch
computations, or resets shadowed `-d.grab` output slots after the turn completes, so only `$agent`
state mutations and handle updates remain in `overlay_`") is architecturally impossible under the
current runtime:

1. Calling `arena.release(turnMark)` (`arena_manifold.cpp:1614–1700`) truncates `slots_`, `links_`,
   `content_`, and `overlay_` back to the mark floor. Doing so at turn end erases all legitimate
   mutable state mutations (field updates, belief assertions, active intentions) produced during
   that turn.
1. `ArenaManifold` provides no API to "reset" or "unshadow" individual cells.
1. As a result, shadowed `-d.grab` output cells accumulate permanently in `overlay_` across
   thousands of turns, destroying memory locality, bloating checkpoints, and causing severe memory
   leakage.

**Evidence:**

- Doc §1.3 (lines 111–112): "with copy-on-write shadowing (`ArenaManifold::shadow`, `overlay_`)
  whenever a base cell's value or links are mutated."

- Doc §2.2 (line 179): "`OUT`: Shadowed Opcode `-d.grab` Output Cells (Copy-on-write per turn,
  discarded at turn end)."

- Doc §2.3 (lines 264–268): "when an opcode in `programManifold` writes its result to its `-d.grab`
  output cell via `writeScalarValue(context.arena, *outputCell, ...)` (`core.cpp:104`),
  `ArenaManifold` shadows `*outputCell` into `context.arena.overlay_`."

- Doc §3.2 (lines 327–329): "the executor records an `ArenaManifold::Mark turnMark = arena.mark()`
  before running transient scratch computations, or resets shadowed `-d.grab` output slots after the
  turn completes..."

- Repo `apps/common/xanadu/zigzag/arena_manifold.cpp:1464–1503`: `ArenaManifold::shadow` performs a
  full copy into `slots_`, `links_`, `content_`, and allocates an entry in `overlay_`.

- Repo `apps/common/xanadu/zigzag/arena_manifold.cpp:1614–1700`: `ArenaManifold::release(m)` rolls
  back all slots and shadows minted since mark `m`; there is no selective unshadowing.

- Repo `apps/common/xanadu/vortex/vortex_vm.cpp:1118–1120`:

  ```cpp
  for (std::size_t i = 0; i < std::min(outCells.size(), newOutputs.size()); ++i) {
    core_.value(outCells[i], 0, -1, newOutputs[i]);
  }
  ```

**Fix:** Immutable opcodes in `programManifold` must not hold mutable output cells on `-d.grab`.
Instead, compiler code generation must route opcode outputs to execution-scoped registers in the
agent's ephemeral overlay (e.g., registers `r0..r1`, `a0..a4`, or activation-frame local slots on
`d.stack`), or bind outputs to a fixed per-agent ephemeral scratch ring. This keeps the base code
graph 100% read-only, eliminates COW-shadowing during instruction execution, and avoids the need for
an impossible turn-end unshadowing pass.

## MAJOR

### 2. (§4.3, line 441 & §5, line 563) Unjustified C++ State Reconciliation Violating `GEMINI.md`

Full title: Unjustified C++ State Reconciliation & Manifest Inspection Violating `GEMINI.md`

**Problem:** `GEMINI.md` mandates that "New C++ code must justify why it isn't being written in
Vortex." §5 claims in prose that "all structural schema inspection, field/belief/intention
reconciliation walks, and `on Upgrade` dispatch belong in Vortex cells and the `vagent`
compiler/runtime adapter (`std:agent` / `apps/common/xanadu/vagent/`). Only the low-level manifold
dimension seeding, frozen program store hosting, and `MeshActorRuntime` grant transition require
small, surgical C++ changes in `zigzag` and `vortex`."

However, Proposals P17.4 and P17.5 contradict this:

- Proposal P17.4 defines `StateReconciler` as a C++ `std::function`:
  `using StateReconciler = std::function<std::expected<CellRef, ...>(...)>;`
- Proposal P17.5 specifies `vagent::checkUpgradeCompatibility` as a C++ function in
  `agent_executor.cpp` doing AST/manifest comparisons.
- §6.2 step 3 describes `vagentReconciler` running in C++ to walk and rewrite the manifold.

The reflection manifest spine (`d.vagent.type = "vagent:program@1"`) is already represented as
structured cells and ranks in the ZigZag manifold (§2.2). Comparing fields, matching belief
predicates, and transforming intention records across two mounted program manifests is fundamentally
a relational join/query across two ZigZag spaces, which Vortex (via VQL, Vlog, or `std:agent`) is
uniquely designed to execute natively. Implementing this in C++ duplicates rank walking and adds
unjustified C++ complexity.

**Evidence:**

- Doc §5 (lines 563–568): "all structural schema inspection, field/belief/intention reconciliation
  walks, and `on Upgrade` dispatch belong in Vortex cells..."

- Doc §5 Proposal P17.4 (lines 654–659):

  ```cpp
  using StateReconciler = std::function<std::expected<zigzag::CellRef, RuntimeError>(
      WorkerContext &oldContext, zigzag::CellRef oldStateRoot,
      std::string_view oldProgramRef,
      WorkerContext &newContext, std::string_view newProgramRef,
      const UpgradePolicy &policy)>;
  ```

- Doc §5 Proposal P17.5 (lines 695–703): "Static Upgrade Compatibility Checker
  (`vagent::checkUpgradeCompatibility(oldProg, newProg)`): Compares the `vagent:program@1` manifest
  spines of `oldProg` and `newProg` ahead of deployment..."

- `GEMINI.md`: "New C++ code must justify why it isn't being written in Vortex."

**Fix:** Explicitly partition the architectural responsibilities:

1. **C++ Host / Actor Supervisor (`MeshActorRuntime::upgrade`)**: Only handles the turn-boundary
   quiescence check, grant cryptographic verification, and context swapping.
1. **Vortex Standard Library (`std:agent:reconcile` / `std:agent:upgrade`)**: Manifest comparison
   and state graph migration (§4.3) must be written in Vortex bytecode or Vlog/VQL rules operating
   over the two program manifests mounted via `ArenaManifold::attach(space)`. The C++ reconciler
   simply dispatches this Vortex reconciliation transaction, passing
   `(oldRoot, newManifest, oldManifest)` and receiving `newRoot`.

### 3. (§2.1, line 157 & §5, line 626) Missing Retention Bounds on Base Manifolds in `LocalHost`

Full title: Missing Retention Bounds and Lifetime Invariants on Cached Base Manifolds in `LocalHost`

**Problem:** Under §2.1 and §6.1–§6.2, `LocalHost` compiles and caches frozen `programManifold`
instances across successive versions ($V_1, V_2, \dots$). Proposal P17.3 introduces:

```cpp
using ProgramManifoldResolver =
    std::function<const zigzag::Manifold *(std::string_view programRef)>;
```

This design contains two severe architectural flaws:

1. **Unbounded Memory Retention**: There is no retention policy, capacity limit, or eviction
   mechanism on `LocalHost`. Over a long-running process where multiple program versions are
   deployed and upgraded, obsolete sealed stores and manifolds (and their mmap segments) remain
   pinned indefinitely, leaking memory.
1. **Dangling Raw Pointers**: Returning a raw pointer `const zigzag::Manifold *` provides no
   lifetime guarantees. `WorkerContext` and `ArenaManifold` store a raw pointer `base_`. If
   `LocalHost` ever evicts a program version, active or suspended agents will hold dangling
   pointers.

**Evidence:**

- Doc §2.1 (lines 157–160): "$V_1$'s `Manifold` snapshot and $V_2$'s `Manifold` snapshot coexist
  simultaneously in memory on the same `LocalHost` while some agents run $V_1$ and upgraded agents
  run $V_2$."

- Doc §5 Proposal P17.3 (lines 626–628):

  ```cpp
  using ProgramManifoldResolver =
      std::function<const zigzag::Manifold *(std::string_view programRef)>;
  ```

- Repo `apps/common/xanadu/vagent/program.hpp:74–156`: `LocalHost` currently owns only a single
  `CompiledProgram program_` with no cache or retention policy.

- Repo `apps/common/xanadu/vortex/worker_context.hpp:52`: `WorkerContext` takes and stores a raw
  `const Manifold *baseManifold`.

**Fix:**

1. Replace raw pointers with explicit shared ownership or a leased handle:

   ```cpp
   using ProgramManifoldResolver =
       std::function<std::shared_ptr<const zigzag::Manifold>(std::string_view)>;
   ```

   and update `WorkerContext` to accept `std::shared_ptr<const zigzag::Manifold>`.

1. Add an explicit, bounded LRU or ref-counted retention cache in `LocalHost::Options`
   (`std::size_t maxCachedPrograms{16}`), allowing unreferenced base manifolds to be unloaded once
   their active agent count reaches zero.

### 4. (§2.2, line 216 & §4.3.4, line 494) Magic Sentinel Zero for Unstarted Intentions

Full title: Magic Sentinel Zero for Unstarted Intentions Violating House Rules and Causing Collision

**Problem:** House Rule 3 explicitly mandates: "No Sentinels in APIs: Absent values and fallible
operations use `std::optional` or `std::expected`, never reserved sentinel integers or empty-string
conventions."

In §4.3.4 (line 494), the document specifies: "`step`: the suspension point ordinal
$s \in \mathbb{N}$ (`0` if not yet started, or the ordinal of the `achieve`, `Wait`, `Request`, or
action yield boundary where $I$ suspended)". In §4.3.4 (lines 507–508): "StepZero: Is $s == 0$ (not
yet started)?". However, §2.2 (line 216) states: "suspension points $s \in \{0, 1, \dots, K-1\}$".

This creates an immediate semantic collision and bug: ordinal `0` cannot simultaneously denote "not
yet started" and the first valid suspension point in a plan ($s=0$). If a plan yields at its first
suspension point ($s=0$), §4.3.4 misinterprets it as an unstarted intention and triggers replanning
from scratch!

**Evidence:**

- Doc §2.2 (line 216): "suspension points: ordered rank on `d.vagent.step` mapping each suspension
  ordinal $s \in \{0, 1, \dots, K-1\}$..."
- Doc §4.3.4 (line 494): "`step`: the suspension point ordinal $s \in \mathbb{N}$ (`0` if not yet
  started, or the ordinal of the achieve, Wait, Request, or action yield boundary where $I$
  suspended)..."
- Doc §4.3.4 (lines 507–508): "StepZero: Is $s == 0$ (not yet started)?"

**Fix:** Eliminate the sentinel integer `0`. An intention that has not yet started execution has NO
link along `d.vagent.step` (absence of link = `std::nullopt`). The `d.vagent.step` rank is linked
only upon reaching an actual suspension boundary ($s \ge 0$). In C++ and Vortex APIs, the suspension
ordinal must be represented as `std::optional<std::uint32_t>`.

### 5. (§5, line 660) Error Type Mismatch and Hallucinated C++ Signatures in Proposals

Full title: Error Type Inconsistency in Proposal P17.4 and Inaccurate Codebase Citations

**Problem:**

1. In Proposal P17.4, `MeshActorRuntime::upgrade` is declared to return
   `std::expected<ActorGrant, RuntimeError>`. However, `RuntimeError` is a Vortex VM execution error
   (`vortex_core.hpp`), whereas all existing supervisor methods on `MeshActorRuntime` (`spawn`,
   `resume`, `renew`, `checkpointView` in `mesh_actor.hpp:152, 160, 239`) uniformly return
   `std::expected<T, ActorError>`. Mixing VM execution error types into the distributed actor
   supervisor violates layer boundaries.
1. In §1.2 (lines 67–72), the document cites non-existent files and symbols: "The Vortex VM
   (`core.cpp:1735`, `1766`, `1831`, `1929`) reads the jump/call target via `readScalarValue` as a
   raw `uint64_t` and casts it to `CellRef`:" `core.cpp` does not exist in
   `apps/common/xanadu/vortex/` (the VM is `vortex_vm.cpp:655–694`), and `readScalarValue` does not
   exist (it is `toInt(inputs[0])`).
1. In §1.2 (line 90) and §5 Proposal P17.1 (line 578), the document claims `base->dimensionNames()`
   exists and that `ensureDimension` mints duplicate ephemeral dimensions. In reality,
   `dimensionNames()` does not exist (the real API in `manifold.hpp:472` is `dimensions()`), and
   `ArenaManifold::ensureDimension` (`arena_manifold.cpp:81–85`) already calls
   `dimensionNamed(name)` and reuses the base manifold's dimension cell if present.

**Evidence:**

- Doc §5 Proposal P17.4 (lines 660–663): `std::expected<ActorGrant, RuntimeError> upgrade(...)`

- Repo `apps/common/xanadu/vortex/mesh_actor.hpp:35, 152, 160, 239`: All methods return
  `ActorError`.

- Repo `apps/common/xanadu/vortex/vortex_vm.cpp:655–690`: Real jump/call implementation using
  `toInt`.

- Repo `apps/common/xanadu/zigzag/arena_manifold.cpp:81–85`:

  ```cpp
  DimRef ArenaManifold::ensureDimension(const std::string_view name) {
    const auto existing = dimensionNamed(name);
    if (noCell != existing) {
      return existing;
    }
  ```

**Fix:** Update Proposal P17.4 to return `std::expected<ActorGrant, ActorError>` (or introduce a
dedicated `ActorUpgradeError` within the `zigzag::vortex::mesh` namespace). Correct all file paths
to `vortex_vm.cpp` and method names to `dimensions()`.

## MINOR

### 6. (§1.1, line 40 & §2.2, line 178) Hardcoded Arbitrary Register and Scratch Capacities

Full title: Arbitrary Register and Scratch Allocator Ceilings Violating House Rule 4

**Problem:** §1.1 and §2.2 specify hardcoded fixed sizes: "the 64-entry dynamic allocation pool
(`pool_`)" and "`scratch_[0..31]`". House Rule 4 ("No Arbitrary Ceilings") requires that capacity
limits be justified and exposed as configurable settings rather than hardcoded small caps that force
user workarounds.

**Evidence:**

- Doc §1.1 (line 40): "the 64-entry dynamic allocation pool (`pool_`)"
- Doc §2.2 (line 178): "`scratch_[0..31]`, `pool_[0..63]`"

**Fix:** Allocate scratch and pool cells dynamically on the P16 activation frame, or parameterize
pool limits in `LocalHost::Options` / `ActorGrant`.

______________________________________________________________________

VERDICT: NOT CONVERGED

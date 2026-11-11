# Round 1 Critique — Systems Architect (`architect`)

**Review Target:** `design/vagent-metacircular-compiler-implementation.md`\
**Companion Documents:** `design/vagent-metacircular-compiler.md`,
`design/vagent-metaprogramming.md`, `design/vagent-metaprogramming-implementation.md`,
`design/vagent-implementation.md`\
**Reviewer:** Critic 4 — Systems Architect (`architect`)\
**Date:** 2026-11-07

______________________________________________________________________

## Findings

### [ARCHITECT-1] BLOCKING: Corrupted Citation and Truncation of `kMetaDimensionNames` (`P5`)

- **Location**: `design/vagent-metacircular-compiler-implementation.md:245-254`,
  `apps/common/xanadu/vagent/lineage.hpp:58-62`, `apps/common/xanadu/vagent/lineage.cpp:425-445`

- **Evidence**: Line 245 asserts:

  > "Extend `kMetaDimensionNames` (`apps/common/xanadu/vagent/lineage.hpp:58-62`) from 6 entries to
  > 8 entries by filing `"d.lineage"` (`P5`) and `"d.kind-registered"` (`P4`):
  >
  > ````cpp
  > inline constexpr std::array<std::string_view, 8> kMetaDimensionNames{
  >     "d.code",   "d.refs",    "d.ref",          "d.ref.for",
  >     "d.patch",  "d.patch.head", "d.lineage",  "d.kind-registered",
  > };
  > ```"
  > ````

  In the actual codebase on `main` (`apps/common/xanadu/vagent/lineage.hpp:58-62`),
  `kMetaDimensionNames` already has **12 entries**:

  ```cpp
  inline constexpr std::string_view kMetaDimensionNames[] = {
      "d.code",  "d.patch",      "d.weight",   "d.synapse",
      "d.entry", "d.patch-head", "d.validate", "d.refs",
      "d.ref",   "d.recipe",     "d.refFor",   "d.mesh.relocation",
  };
  ```

  Replacing `kMetaDimensionNames` with the proposed 8-entry array drops 6 load-bearing apparatus
  dimensions (`d.weight`, `d.synapse`, `d.entry`, `d.validate`, `d.recipe`, `d.mesh.relocation`),
  breaking `dimensionVocabulary` (`lineage.cpp:431`) and genesis dimension sweeps. Furthermore, the
  proposal misspells `"d.patch-head"` as `"d.patch.head"` and `"d.refFor"` as `"d.ref.for"`, which
  would cause dimension resolution mismatches across `MetaDimensions` (`meta.hpp:71-84`).
  Additionally, line 256 specifies `DimRef lineage{noDim};` where `noDim` does not exist in the
  codebase (the codebase uses `zigzag::noCell` for unlinked `DimRef`).

- **Fix**: Update §3 Stage 0 (line 245) to extend `kMetaDimensionNames` from 12 entries to 14
  entries by appending `"d.lineage"` and `"d.kind-registered"`:

  ```cpp
  inline constexpr std::string_view kMetaDimensionNames[] = {
      "d.code",     "d.patch",      "d.weight",   "d.synapse",
      "d.entry",    "d.patch-head", "d.validate", "d.refs",
      "d.ref",      "d.recipe",     "d.refFor",   "d.mesh.relocation",
      "d.lineage",  "d.kind-registered",
  };
  ```

  Replace `noDim` with `zigzag::noCell` in `MetaDimensions` (`meta.hpp:71-84`).

______________________________________________________________________

### [ARCHITECT-2] MAJOR: Hidden Dependency on Track A2 (`P12` `int_to_str`) in Stage 5 Emitter

- **Location**: `design/vagent-metacircular-compiler-implementation.md:196-207`, `:598-634`,
  `:680-698`

- **Evidence**: Stage 5 (`vagent/emitter.vagent`) must emit fragment descriptors, captured slots
  (`cap$N` per line 614 citing `fragment.cpp:412-420`), and serial labels (`src:<n>` per line 696).
  VAgent has no integer-to-string conversion primitive. The companion architecture document
  (`design/vagent-metacircular-compiler.md:493-496`) specifically warns:

  > "One string dependency arrives earlier than 'parser last' implies: head labels like
  > `reify:<src>:<serial>` (`meta.cpp:1709-1713`) need int→text that vagent lacks — either mint a
  > minimal int→text builtin or restructure the label as a name record + serial-int member. Decide
  > at the builder stage, not the parser stage."

  However, `design/vagent-metacircular-compiler-implementation.md` defers `int_to_str` and Track A2
  (`P12`) all the way to Stage 7 in Wave 3 (lines 204, 680-698). Consequently, Stage 5 cannot be
  completed or tested in VAgent without `int_to_str`.

- **Fix**: Move Track A2 (`P12` — scalar string/char primitives `int_to_str`, `str_len`, `byte_at`,
  `str_slice`) from Stage 7 into Wave 2 (landing at Barrier 2 before Wave 3 begins), or integrate it
  into Track A/C in Wave 1. This guarantees that `int_to_str` is live when Stage 5 starts emitting
  `cap$N` capture slots and serial labels.

______________________________________________________________________

### [ARCHITECT-3] MAJOR: Dual File Ownership Collision on `executor.cpp` in Wave 2 (Tracks E & F)

- **Location**: `design/vagent-metacircular-compiler-implementation.md:184-195`, `:209-222`,
  `:518-524`, `:558-572`

- **Evidence**: §2 provides an explicit File-Ownership Matrix for Wave 1 (Tracks A–D) guaranteeing
  zero TU collisions, but omits a File-Ownership Matrix for Wave 2. In Wave 2:

  - Track E ("Stage 3: Host-Service Compile Boundary", line 518) modifies
    `apps/common/xanadu/vagent/executor.cpp` to wire `compile_fragment` into `runHostStep`.
  - Concurrently, Track F ("Stage 4: Bootstrap Pair, Batched Join, R1/R2 Builders", line 558)
    modifies `apps/common/xanadu/vagent/executor.cpp` to wire `join_batch` into `runHostStep`.

  Executing Track E (`T_HOST`) and Track F (`T_BOOT`) concurrently in parallel worktrees as shown in
  the Mermaid topology causes a direct merge collision on `executor.cpp`.

- **Fix**: Define the Wave 2 File-Ownership Matrix in §2. Provide a single extension point in
  `executor.cpp` at Barrier 1: dispatch all `std:meta` host steps through a single method on
  `MetaRuntime` (`MetaRuntime::runMetaHostStep(CellRef stepCell)`). This restricts `executor.cpp`
  modifications to Track C in Wave 1, allowing Track E and Track F in Wave 2 to modify only
  `host_compile.cpp` and `meta.cpp` without touching `executor.cpp`.

______________________________________________________________________

### [ARCHITECT-4] MAJOR: Contradictory `OpcodeKind` Band Partitioning (`P9`) for VQL & Epistemic

- **Location**: `design/vagent-metacircular-compiler-implementation.md:89-102`, `:730-740`,
  `feature/vql-completion:apps/common/xanadu/vortex/vortex_vm.hpp:97-129`

- **Evidence**: §1.1 specifies:

  > "1. Partition `OpcodeKind` into two explicit, contiguous bands terminated by `static constexpr`
  > markers in `vortex_vm.hpp`:
  >
  > - Band A (Content-Decodable Scalar/Query Opcodes): `Nop` through `FieldSet`, plus `Point`,
  >   `Exp`, `Ln`, `Sqrt`, and the VQL query opcodes (`MatchContract..VqlMaskLinks`), bounded by
  >   `kLastContentDecodableOpcode`.
  > - Band B (Registration-Only Control/Eval Opcodes): `RunCall`, `RunResume`, `ScopedEvaluate`,
  >   `BeginMutationGroup`, `EndMutationGroup`, bounded by `kLastOpcodeKind`."

  On `feature/vql-completion` (`4143dcc0`), `BeginMutationGroup` and `EndMutationGroup` are ordinals
  26 and 27 in the 29-opcode sequence `MatchContract..VqlMaskLinks` (before `VqlMaskLinks` at
  ordinal 28). If Band A is contiguous and bounded by `kLastContentDecodableOpcode`,
  `BeginMutationGroup` and `EndMutationGroup` cannot remain inside `MatchContract..VqlMaskLinks`
  while simultaneously belonging to Band B. Furthermore, `feature/vql-completion` contains other
  control-flow and frame-manipulation opcodes in that 29-opcode range (`InvokeFrame`, `ReturnFrame`,
  `PushGuard`, `PopGuard`, `CheckGuards`, `OpenGuardScope`, `CloseGuardScope`). Leaving their band
  assignment unspecified creates an ambiguous security boundary where untrusted content-decoded
  integers could invoke frame and guard instructions.

- **Fix**: Specify the exact contiguous layout of `OpcodeKind` in `vortex_vm.hpp`:

1. **Band A (`0` to `kLastContentDecodableOpcode`):**
   - Scalar arithmetic & logic (`Add` .. `Nop`)
   - Scopes & assertions (`Bind`, `Resolve`, `Assert`)
   - Math & strings (`Abs` .. `ToUpper`)
   - Vlog logic extensions (`Unify` .. `ChoiceDepth`)
   - Persistent traversal & member ops (`Linked`, `Find`, `FieldSet`)
   - Epistemic mathematical & reference primitives (`Point`, `Exp`, `Ln`, `Sqrt`)
   - VQL query & stream opcodes (`MatchContract`, `RequireContract`, `CopyCellRefs`,
     `ClearCellRefs`, `BranchFalseCell`, `StreamClear`, `StreamAppend`, `StreamBegin`, `RankBegin`,
     `IteratorNext`, `StreamReverseBegin`, `VqlCompare`, `ResumeInit`, `ResumeSet`, `ProbeLink`,
     `VqlString`, `VqlAdd`, `VqlBoxScalar`, `ScopedChoice`, `VqlFieldProbe`, `VqlMaskLinks`)
1. **Band B (`kFirstRegistrationOnlyOpcode` to `kLastOpcodeKind`):**
   - VQL execution frames & guards (`InvokeFrame`, `ReturnFrame`, `PushGuard`, `PopGuard`,
     `CheckGuards`, `OpenGuardScope`, `CloseGuardScope`, `BeginMutationGroup`, `EndMutationGroup`)
   - VAgent metacircular & epistemic execution (`RunCall`, `RunResume`, `ScopedEvaluate`)
1. Ensure `isContentDecodableOpcode(int64_t raw)` checks
   `raw >= 0 && raw <= static_cast<int64_t>(kLastContentDecodableOpcode)`, and
   `isRegistrableOpcode(int64_t raw)` checks
   `raw >= 0 && raw <= static_cast<int64_t>(kLastOpcodeKind)`.

______________________________________________________________________

### [ARCHITECT-5] MAJOR: OR-Parallel Worker Ephemeral Cell Hazard in `bindProgramOpcodes` (`P6`)

- **Location**: `design/vagent-metacircular-compiler-implementation.md:478-483`,
  `design/vagent-metacircular-compiler.md:427-435`,
  `apps/common/xanadu/vortex/or_parallel.cpp:331-365`

- **Evidence**: In `or_parallel.cpp:345-350`, `executeSparkOnWorker` constructs
  `ArenaManifold workerArena{base};` where `base` is the persistent base manifold. Session-minted
  ephemeral cells from the parent actor arena are not present in `base`. If an agent compiles and
  registers a fragment at runtime (`register(h)`), head $h$ and its op cells are session-minted
  ephemeral cells (`zigzag::isEphemeral(h) == true`) until the next P16 checkpoint/promote.
  `design/vagent-metacircular-compiler.md:434-435` explicitly notes:

  > "Worker visibility: `workerArena` overlays the persistent base only, so session-minted
  > registered ops are invisible to sparks — v1 states `or_parallel` reaches durable-registered
  > graphs only (or spark export checks coverage)."

  However, `design/vagent-metacircular-compiler-implementation.md:478-483` instructs Track D to bind
  all `d.kind-registered` opcodes into worker VMs unconditionally: *"binds both image opcodes and
  `d.kind-registered` opcodes in $O(N_{\text{ops}})$ flat array iteration"*. If ephemeral op cells
  are registered into a worker VM whose `workerArena` does not contain them, worker sparks that
  encounter a `RunCall` to $h$ will fault or read uninitialized memory.

- **Fix**: In Track D (`genesis.hpp` / `or_parallel.cpp`), explicitly specify that
  `bindProgramOpcodes` filters the memoized opcode vector to bind only persistent base cells
  (`!zigzag::isEphemeral(op) && base->contains(op)`). Add a preflight validation in
  `exportSparkFromVictim` that refuses or marks unsparkable any choice point whose call graph
  references session-ephemeral registered heads, preserving the P17 worker memory-safety invariant.

______________________________________________________________________

### [ARCHITECT-6] MINOR: Missing Settings Plumbing for Fuel & Resource Bounds (`P3`, `P4`)

- **Location**: `design/vagent-metacircular-compiler-implementation.md:417-418`, `:762-763`,
  `apps/common/xanadu/vagent/meta.hpp:365, 429`
- **Evidence**: P3 and P4 specify bounding `validate`, `drainPendingRegisters`, and `registerHead`
  by `vagent.max_register_ops` (default 512), `vagent.max_pending_registers` (default 64),
  `vagent.max_registered_heads` (default 256), and `vagent.max_register_replay_failures` (default
  1). However, as documented in `apps/common/xanadu/vagent/meta.hpp:365, 429` (*"the verb has no
  settings channel"*), `MetaRuntime` has no settings reader. The implementation plan does not define
  how these configuration values reach `MetaRuntime`.
- **Fix**: Define a `MetaRuntimeLimits` struct in `meta.hpp` holding these four bounds with their
  default values. Declare the settings string constants in `apps/common/xanadu/system_docs.hpp`
  (`kVagentMaxRegisterOps`, `kVagentMaxPendingRegisters`, `kVagentMaxRegisteredHeads`,
  `kVagentMaxRegisterReplayFailures`). Have `VagentExecutor` and `runGenesis` initialize
  `MetaRuntimeLimits` via `settingReader_` and pass them into `MetaRuntime` via `MetaBindSet` or a
  `setLimits` method.

______________________________________________________________________

### [ARCHITECT-7] MINOR: Foreign Error Type in Porting `VortexCore::rawWingSlotsOf` (`P11`)

- **Location**: `design/vagent-metacircular-compiler-implementation.md:241-244`, `:770`,
  `origin/design/vagent-epistemic-arbitration:apps/common/xanadu/vortex/vortex_core.hpp:258`
- **Evidence**: P11 specifies adopting `VortexCore::rawWingSlotsOf(CellRef op)` in Stage 0 from
  `origin/design/vagent-epistemic-arbitration`. On `epistemic-arbitration`, `rawWingSlotsOf` has the
  signature `std::expected<std::vector<CellRef>, ScopedRefusal> rawWingSlotsOf(...)`, which depends
  on `enum class ScopedRefusal`. `ScopedRefusal` is defined in `epistemic_limits.hpp` on the
  epistemic branch and does not exist on `main`. Porting that signature directly into Stage 0 on
  `main` breaks compilation or prematurely introduces P19 epistemic types into the core VM.
- **Fix**: Define `VortexCore::rawWingSlotsOf` on `main` in Stage 0 with a decoupled signature:
  `std::vector<CellRef> rawWingSlotsOf(CellRef opcode, DimVector side) const`, implemented via
  `zigzag::rank(arena_, *first, dims_.step)`. When `epistemic-arbitration` merges in Step M3, adapt
  its callsite to this clean signature.

______________________________________________________________________

### [ARCHITECT-8] NIT: Unregistered Proposal for Stage 3 Host-Service `compile_fragment`

- **Location**: `design/vagent-metacircular-compiler-implementation.md:507-525`, `:754-771`
- **Evidence**: `brief-common.md` mandates that *"Every change to existing code must be tracked in
  the numbered Proposal Register (`P<k>`)"*. Stage 3 introduces a new host verb
  `compile_fragment(label, params, resultType, src)` requiring modifications to existing code in
  `apps/common/xanadu/vagent/decls.cpp`, `executor.cpp`, and `adapter.cpp`, but this change is
  omitted from the Proposal Register in §5 (`P1`–`P12`).
- **Fix**: Add `P13` to §5 covering `host_compile.hpp/.cpp`, `decls.cpp`, and `executor.cpp` for the
  Stage 3 host-service compilation boundary, pricing its cost and documenting the refused
  alternative (`eval(string)`).

______________________________________________________________________

SUMMARY: BLOCKING=1 MAJOR=4 MINOR=2 NIT=1\
VERDICT: NEEDS-REVISION

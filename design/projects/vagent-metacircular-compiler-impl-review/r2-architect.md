# Round 2 Critique — Systems Architect (`architect`)

**Review Target:** `design/vagent-metacircular-compiler-implementation.md`\
**Companion Documents:** `design/vagent-metacircular-compiler.md`,
`design/vagent-metaprogramming.md`, `design/vagent-metaprogramming-implementation.md`,
`design/vagent-implementation.md`\
**Reviewer:** Critic 4 — Systems Architect (`architect`)\
**Date:** 2026-11-07

______________________________________________________________________

## 1. Audit of Round 1 Disposition & Verifications

The revised implementation plan (`design/vagent-metacircular-compiler-implementation.md`, commit
`9d91bfdc` baseline, 882 lines) was audited against all Round 1 findings in the Systems Architecture
lane (`ARCHITECT-1` through `ARCHITECT-8`) and the system-level convergence criteria.

### 1.1 Resolution Verification of `ARCHITECT-1` through `ARCHITECT-8`

- **[ARCHITECT-1] BLOCKING — `kMetaDimensionNames` truncation in `lineage.hpp` (`P5`):** **VERIFIED
  RESOLVED.**

  - **Check**: §3 Stage 0 (lines 264–281) preserves all 12 existing apparatus dimensions verbatim
    (`"d.code"`, `"d.patch"`, `"d.weight"`, `"d.synapse"`, `"d.entry"`, `"d.patch-head"`,
    `"d.validate"`, `"d.refs"`, `"d.ref"`, `"d.recipe"`, `"d.refFor"`, `"d.mesh.relocation"`) and
    appends `"d.lineage"`, `"d.kind-registered"`, `"d.pending-reg"`, and `"d.ast.kinds"` for a
    complete 16-entry vocabulary array.
  - Nonexistent `noDim` is replaced with `zigzag::noCell` in `MetaDimensions` (`meta.hpp:72–85`,
    line 283).

- **[ARCHITECT-2] MAJOR — Hidden dependency on Track A2 (`P12` `int_to_str`) in Stage 5 Emitter:**
  **VERIFIED RESOLVED.**

  - **Check**: The execution topology diagram (lines 198–212) and File-Ownership Matrix (lines
    224–235) move Track A2 (`P12`: `str_len`, `byte_at`, `str_slice`, `int_to_str`, `str_to_int`)
    into Wave 2, completing at Barrier 2 alongside Stage 3 and Stage 4.
  - Stage 5 (`emitter.vagent`) and Stage 6 (`checker.vagent`) in Wave 3 have `int_to_str` available
    at kickoff for emitting `cap$N` captured slots and serial-keyed head labels.

- **[ARCHITECT-3] MAJOR — Dual file ownership collision on `executor.cpp` in Wave 2 (Tracks E &
  F):** **VERIFIED RESOLVED.**

  - **Check**: §2 now includes a complete Wave 1 & Wave 2 File-Ownership Matrix (lines 224–235).
  - Track C in Wave 1 exclusively owns `executor.cpp`, introducing the host-step delegation call
    `meta.runMetaHostStep(verb, step, intention)` (line 343) and post-commit drain
    `meta.drainPendingRegisters()` (line 345).
  - Track E (`host_compile.hpp/.cpp`, `adapter.cpp`) and Track F (`bootstrap.hpp/.cpp`,
    `r1_ast.hpp/.cpp`) in Wave 2 plug into `runMetaHostStep` without touching `executor.cpp`,
    guaranteeing 100% file-disjoint parallel execution across all worktrees.

- **[ARCHITECT-4] MAJOR — Contradictory `OpcodeKind` band partitioning (`P9`) for VQL & Epistemic
  Merges:** **VERIFIED RESOLVED.**

  - **Check**: §1.1 (lines 92–106) and §4 Step M2 (lines 838–844) explicitly partition `OpcodeKind`:
    - **Band A (`<= kLastContentDecodableOpcode`):** `Nop` through `FieldSet`, `Point`, `Exp`, `Ln`,
      `Sqrt`, `P12`'s five pure scalar opcodes (`StrLen`, `ByteAt`, `StrSlice`, `IntToStr`,
      `StrToInt`), and the 21 pure/data VQL opcodes from `feature/vql-completion` (`MatchContract`,
      `Navigate`, `Project`, `Filter`, `Join`, `Group`, `Order`, `Limit`, `Aggregate`, `Distinct`,
      `Union`, `Intersect`, `Except`, `Window`, `Coalesce`, `Case`, `Cast`, `VqlString`,
      `VqlConcat`, `VqlRegex`, `VqlMaskLinks`).
    - **Band B (`> kLastContentDecodableOpcode && <= kLastOpcodeKind`):** `RunCall`, `RunResume`,
      `ScopedEvaluate`, and the 8 VQL execution/frame/mutation opcodes (`InvokeFrame`,
      `ReturnFrame`, `ScopedChoice`, `PushGuard`, `PopGuard`, `BeginMutationGroup`,
      `EndMutationGroup`).
  - Range predicates `isContentDecodableOpcode` and `isRegistrableOpcode` enforce exact contiguous
    checks.

- **[ARCHITECT-5] MAJOR — OR-Parallel worker safety: phantom ephemeral cell hazard in
  `bindProgramOpcodes` (`P6`):** **VERIFIED RESOLVED.**

  - **Check**: §3 Track D (lines 547–557) specifies that the memoized opcode vector for OR-parallel
    worker VMs filters out ephemeral cells, binding only persistent base cells
    (`!zigzag::isEphemeral(op)`).
  - For `ShadowDeltaPrefix` sparks where `spark.prefixSnapshot` hydrated ephemeral cells into
    `workerArena`, ephemeral registered opcodes are bound only after verifying
    `workerCore.arena().contains(op)`.

- **[ARCHITECT-6] MINOR — Settings plumbing for fuel and resource bounds in `MetaRuntime` (`P3`,
  `P4`):** **VERIFIED RESOLVED.**

  - **Check**: Lines 286–288 introduce `MetaRuntimeLimits` (`maxRegisterOps = 512`,
    `maxPendingRegisters = 64`, `maxRegisteredHeads = 256`, `maxReplayFailures = 1`), populated from
    `VagentExecutor`'s settings reader and passed to `MetaRuntime`.
  - Lines 431, 477, and 541 enforce these bounds during `validate`, `registerCode`, and genesis
    replay.

- **[ARCHITECT-7] MINOR — Decoupled signature for `VortexCore::rawWingSlotsOf` (`P11`):** **VERIFIED
  RESOLVED.**

  - **Check**: Lines 260–262 adopt
    `std::vector<CellRef> VortexCore::rawWingSlotsOf(CellRef op, bool rightWing) const` without
    importing the foreign `ScopedRefusal` type onto `main`.

- **[ARCHITECT-8] NIT — Proposal Register completeness (`P13` and `P14`):** **VERIFIED RESOLVED.**

  - **Check**: §5 Proposal Register (lines 881–882) fully tracks `P13` (Stage 1 lowering &
    `runMetaHostStep` dispatch) and `P14` (Stage 3 `compile_fragment` host-service boundary),
    completing the numbered registry (`P1`–`P14`).

______________________________________________________________________

## 2. Structural & Architectural Invariants Verification

1. **Layering & Module Independence**: `VortexVM` remains 100% isolated from `vagent` symbols and
   headers; expression lowering uses native manifold constants and opcodes, while complex reflection
   and synthesis verbs lower to spine host steps dispatched via `runMetaHostStep`.
1. **2-Degree ZigZag Topology**:
   - `+d.lineage` forms a clean 2-degree rank (`twin_k → ... → twin_1 → sourceHead`) with zero
     ref-carrier cells and zero P16 relocation hooks (`ZIGZAG-2`).
   - `join_batch` argument cells are tuple-private, avoiding reciprocal `-d.grab` eviction on shared
     targets (`ZIGZAG-1`).
   - R1 AST uses `+d.grab` for first-child and `+d.step` for siblings, eliminating intra-tree
     dimension collisions (`ZIGZAG-4`).
1. **P15/P16/P17 Invariants & Universal Deferral**:
   - In-turn requests are buffered on the trailed `+d.pending-reg` rank, leaving `opcodeMap_`,
     `d.kind-registered`, and `d.code` untouched until the post-commit `drainPendingRegisters()`
     boundary at `marks == 0` (`WRITER-4`, `ZIGZAG-6`).
   - `EngineP16Ceremony::markedCells` unions `d.code`, `d.kind-registered`, and `installedCopies_`
     so runtime-minted code is relocated cleanly across checkpoints.
   - P17 `SharedProgramManifold` base overlay residency is unified with `LineageStore` prefix bands
     in `imageRef`.
1. **Zero-Sentinel Contract**: `byte_at` and `str_to_int` return `Opt<Int>`
   (`std::optional<int64_t>`), unlinked dimensions use `noCell`, and unknown values return
   `Opt::none`.

______________________________________________________________________

SUMMARY: BLOCKING=0 MAJOR=0 MINOR=0 NIT=0\
VERDICT: CONVERGED

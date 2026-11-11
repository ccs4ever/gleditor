# Common Brief: VAgent Metacircular Missing Features Review

## 1. Target Document

- Target: `design/vagent-metacircular-missing-features.md`
- Repository root:
  `/home/ccs4ever/.gemini/antigravity/worktrees/gleditor/metacircular_compiler_implementation_plan`

## 2. Relevant Sibling Design Docs & Source

- Sibling Design Docs:
  - `design/vagent-self-hosting-migration.md`
  - `design/vagent-agent-language.md`
  - `design/vagent-metacircular-compiler-implementation.md`
  - `design/vortex-hyperstructural-runtime.md`
  - `design/store-slice-convergence.md`
- Key Implementation Files:
  - `apps/common/xanadu/vortex/vortex_stdlib.hpp` & `vortex_stdlib.cpp` (especially
    `buildZigzagModule:4634–4760`, `zzStep:5036`, `zzInsert:5045`, `zzUnlink:5054`, `zzLink:5062`,
    `zzDelete:5071`, `zzCloneToChain:5087`, `zzDuplicate:5103`)
  - `apps/common/xanadu/vortex/vortex_core.hpp` & `vortex_core.cpp`
  - `apps/common/xanadu/vortex/mesh_actor.hpp` & `mesh_actor.cpp`
  - `apps/common/xanadu/zigzag/arena_manifold.hpp` (especially `promote:930–934`,
    `ArenaManifold::makeCell:427`)
  - `apps/common/xanadu/vagent/stdlib/*.vagent` (11 stdlib modules)
  - `apps/vagent/main.cpp`

## 3. Standing Architectural Rules (House Style)

1. **Argued, Not Asserted**: Every mechanism must be justified by citing exact repository files and
   line numbers (`file:line`), pricing costs, and recording refused alternatives.
1. **Cells and Dimensions Over Side Tables or Bit-Packing**: State and structure live on orthogonal
   ZigZag ranks (`d.*`), never in ad-hoc C++ tables or serialized strings.
1. **No Sentinels in APIs**: Absent values use `Option[T]` or `Result[T, E]`, never `noCell` or -1
   in surface APIs.
1. **No Arbitrary Ceilings**: Resource safety bounds must be priced and exposed via capability
   grants (`ActorGrant`).
1. **Vortex Over C++**: New C++ code must justify why it cannot be expressed in Vortex.
1. **Proposal Register**: Any changes to existing engine/core/runtime C++ code must be isolated in
   the numbered Proposal Register (`P26`–`P30`).
1. **Strict 2-Degree Invariant**: `A.pos(d) = B <=> B.neg(d) = A`. No fan-in on any
   dimension/direction.
1. **Nelsonian Persistence Invariant**: **NO arbitrary file writing to disk is exposed in the
   language.** Reading produces an ephemeral string, but the SOLE path to permanent persistence is
   Arena Promotion (`std:zigzag:promote` / `zigzag::promote`).

## 4. Output Format

Write your findings to `design/projects/vagent-missing-features-review/r1-<role>.md` adhering
strictly to:

- Tally: `<B> BLOCKING, <M> MAJOR, <m> MINOR`
- Section 2: `## BLOCKING`
- Section 3: `## MAJOR`
- Section 4: `## MINOR`
- Ending with `VERDICT: CONVERGED | NOT CONVERGED`
- Line length \<= 100 characters; valid Markdown.

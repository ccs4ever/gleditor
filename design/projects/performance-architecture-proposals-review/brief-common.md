# Common Brief: Performance Architecture Proposals Critique

- **Target Document**: `design/performance-architecture-proposals.md`
- **Repository Root**: `/data/git/gleditor-restore`
- **Excluded Path**: Do NOT read or inspect `/data/git/gleditor-restore/.worktrees/vql-completion`.
- **Relevant Sibling Docs & Source**:
  - `AGENTS.md`, `GEMINI.md`
  - `design/store-slice-convergence.md`, `design/vortex-hyperstructural-runtime.md`,
    `design/vql-performance-spike.md`
  - `apps/common/xanadu/vortex/vortex_vm.{hpp,cpp}`, `vortex_frames.cpp`, `vortex_closures.cpp`,
    `vortex_core.{hpp,cpp}`, `or_parallel.{hpp,cpp}`
  - `apps/common/xanadu/zigzag/manifold.{hpp,cpp}`, `arena_manifold.{hpp,cpp}`, `net_lens.hpp`,
    `dim_cap.hpp`
  - `apps/common/xanadu/segmented_ops_spool.{hpp,cpp}`, `lmdb_cache.hpp`, `store.{hpp,cpp}`

## Standing House Style Rules

1. **Argued, Not Asserted**: Every mechanism must cite exact repository `file:line`, price the cost
   (bytes, cycles, links, allocations), and record refused alternatives with technical reasons.
1. **Cells and Dimensions Over Side Tables or Bit-Packing**: State, structure, and metadata belong
   on orthogonal ZigZag ranks (`d.*`) unless justified; no bit-packed fields outside
   `CompactOpNode`.
1. **No Sentinels in APIs**: Absent values and fallible operations use `std::optional` or
   `std::expected`, never reserved sentinel integers.
1. **No Arbitrary Ceilings**: Bounds for resource safety must be priced and exposed as settings or
   grant limits.
1. **Vortex Over C++**: New C++ code must justify why it cannot be expressed in Vortex bytecode or
   stdlib.
1. **Proposal Register for Existing Code**: Any change to existing C++ engine, core, stdlib, or
   runtime files must be isolated in a numbered Proposal Register (`P1`, `P2`, ...) stating exact
   files touched, why it cannot live in caller/compiler, cost, and fallback.

## Output Format

Return your critique in the exact `r<round>-<role>.md` format:

- Header: `# Round <N> critique: <Role Name> (performance-architecture-proposals.md)`
- `Tally: <B> BLOCKING, <M> MAJOR, <m> MINOR.`
- Sections: `## BLOCKING`, `## MAJOR`, `## MINOR` (each numbered with
  `(§X.Y, line L) <Short title>`, `Full title:`, `**Problem:**`, `**Evidence:**`, `**Fix:**`).
- Final line: `VERDICT: CONVERGED` or `VERDICT: NOT CONVERGED`.

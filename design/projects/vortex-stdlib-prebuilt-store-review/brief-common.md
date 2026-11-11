# Round 1 common brief — vortex-stdlib-prebuilt-store

## Worktree Verification (Rounds 8+)

The terminal's default checkout `/data/git/gleditor-restore` is **not the target**. Use absolute
paths rooted in the worktree below for every read/search; relative paths without an explicit workdir
produced a wrong-tree Round 7 codebase report. Before reviewing, read the target's `vortex_vm.hpp`
and verify `kLastContentDecodableOpcode`, `kLastOpcodeKind`, and `isRegistrableOpcode` at
approximately lines 145-157. The target's `VortexStdLib::bootstrap` starts around 1507 and contains
21 module creations; the wrong checkout starts around 1459 with 17. If your reads disagree, report a
baseline mismatch instead of deleting target symbols. See `r7-disposition.md` for working-tree blob
hashes; HEAD alone is insufficient.

Focused follow-up reports contain only substantive BLOCKING/MAJOR findings plus at most five MINOR
items. Citation-only drift is MINOR unless it conceals an actual API or behavior mismatch.

Paths are relative to the repository root:

- Repo root:
  `/home/ccs4ever/.gemini/antigravity/worktrees/gleditor/metacircular_compiler_implementation_plan`
- Target document: `design/vortex-stdlib-prebuilt-store.md`
- Sibling design docs: `design/vortex-hyperstructural-runtime.md` (§7 stdlib),
  `design/store-slice-convergence.md` (R1–R15), `design/osmic-microversioning-and-dag.md`,
  `design/vagent-self-hosting-migration.md`,
  `design/system-xanadocs-customization-and-metasystem.md`
- Primary source: `apps/common/xanadu/vortex/vortex_stdlib.{hpp,cpp}`,
  `apps/common/xanadu/vortex/vortex_core.{hpp,cpp}`,
  `apps/common/xanadu/vortex/vortex_vm.{hpp,cpp}`,
  `apps/common/xanadu/vortex/vortex_host.{hpp,cpp}`, `apps/common/xanadu/vortex/worker_context.hpp`,
  `apps/common/xanadu/zigzag/arena_manifold.{hpp,cpp}`,
  `apps/common/xanadu/zigzag/manifold.{hpp,cpp}`,
  `apps/common/xanadu/zigzag/dimension_registry.{hpp,cpp}`, `apps/common/xanadu/store.{hpp,cpp}`,
  `apps/common/xanadu/ops.hpp`, `apps/common/xanadu/compact_op.hpp`,
  `apps/common/xanadu/user_permascroll.{hpp,cpp}`, `include/gleditor/paths.hpp`,
  `tests/xuzz/vortex_stdlib_test.cpp`, `Makefile` (install rules ~line 1842)

## House style rules (standing, every critic enforces)

1. **Argued, Not Asserted**: designs justify every mechanism by citing exact repository files and
   line numbers (`file:line`), pricing the cost (bytes, cycles, links, messages, segments), and
   recording refused alternatives with the technical reason they were rejected.
1. **Cells and Dimensions Over Side Tables or Bit-Packing**: state, structure, and metadata live on
   orthogonal ZigZag ranks (`d.*`), never in ad-hoc C++ side tables, JSON/bencode blobs stuffed into
   scalar cell text, or bit-packed integer fields outside `CompactOpNode`.
1. **No Sentinels in APIs**: absent values and fallible operations use `std::optional` or
   `std::expected`, never reserved sentinel integers or empty-string conventions.
1. **No Arbitrary Ceilings**: do not invent fixed small caps that force user workarounds; when a
   bound is required for resource safety, price it and expose it as a documented setting.
1. **Vortex Over C++**: new C++ code must justify why it cannot be expressed in Vortex bytecode or a
   Vortex standard library routine.
1. **Proposal Register for Existing Code**: any change to existing C++ engine, core, stdlib, or
   runtime files must be isolated in a numbered Proposal (`P<k>`) stating exact files touched, why
   the change cannot live in the caller/compiler, the cost, and the fallback if not approved.

## Severity levels

- `BLOCKING`: the design cannot work as specified, violates a hard invariant, or contradicts
  existing C++ code without a numbered proposal in the register.
- `MAJOR`: an implementer would build the wrong thing, hit an unspecified edge case, or be blocked;
  a layer boundary or C++-vs-Vortex rule violated; a proposal lacks justification, pricing, or
  fallback.
- `MINOR`: wording polish, formatting, minor redundancy, non-blocking clarifications.

## Required output format (return this text; the orchestrator files it)

```markdown
# Round 1 critique: <Role Name> (vortex-stdlib-prebuilt-store.md)

Paths below are relative to the repo root.
Tally: <B> BLOCKING, <M> MAJOR, <m> MINOR.

## BLOCKING

### 1. (§X.Y, line L) <Short title under 80 chars>

**Problem:** <Precise statement of the defect or contradiction.>

**Evidence:** <Exact quotes from the doc + `file:line` citations from the repository.>

**Fix:** <Concrete replacement wording, cell/dimension topology, or proposal.>

## MAJOR
...

## MINOR
...

VERDICT: CONVERGED | NOT CONVERGED
```

## Measured facts the doc cites (verified by the orchestrator, re-verify if you doubt them)

- `exportStandardLibraryToStore` + `Store::save` bound to a real `UserPermascroll` produces 8,652
  ops, folding to ~4,296 cells, 0 refused folds, ~605 KiB `ops.nodes` + ~48 KiB permascroll.
- The exported spool contains wing-slot Int64s holding ephemeral refs (e.g. `value=2147484863` =
  `0x800000BF` at op 10 in one run).
- On the reloaded fold: `home` and `dimsDimension` resolve, but `dimensionNamed("d.stdlib")` returns
  nothing (the `d.*` registry rank is not in the export).

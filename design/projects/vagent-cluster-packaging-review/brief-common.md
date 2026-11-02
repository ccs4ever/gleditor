# Common brief: critique of `design/vagent-cluster-packaging.md`

Repository root: `/data/git/gleditor`. Another session may be working in this checkout: you are
**read-only** except for writing your one assigned round file in
`design/projects/vagent-cluster-packaging-review/`. Do not edit, format, stage or commit anything
else. Do not run builds, tests or programs.

## Target

`design/vagent-cluster-packaging.md` (currently v0.2): a companion proposal for **vagent cluster
packaging** (`P21.1`–`P21.8`). It specifies how user agent programs—pure `vagent` source programs,
`vagent` source programs carrying native skills (`"source+native"`), and native-op `.vnprog`
programs (`"native"`), together with their per-architecture shared modules (`.so`/`.dylib`/`.dll`)
and bundled non-system native shared libraries (`--bundle-lib`)—are packaged into `.vnpkg`
artifacts, addressed by a single logical `programRef`, distributed across cluster nodes over
`MeshSwarmTransport` (inline QUIC lane 1 `SpoolSliceBundle` fast path + BitTorrent/Merkle
`SwarmContentSource` pull path), queued without blocking `MeshActorRuntime::pumpOne` via
`PendingProgramFetch`, verified into a node-local CAS (`<deployment>/packages/cas/`), staged into
`<staging_dir>/<author>-<device>-<epoch>/<serial>/lib/`, and pre-loaded in topological `DT_NEEDED`
order before Phase 2 sandbox lockdown.

## Sibling documents

- `design/vagent-native-agent.md` (v0.6, P20.1–P20.8)
- `design/vagent-shared-program-and-live-upgrade.md` (P17.1–P17.5)
- `design/vagent-agent-language.md` (v3.13)
- `design/vagent-implementation.md`
- `design/vortex-distribution.md`, `design/vortex-mesh-cli.md`,
  `design/vortex-transport-and-or-parallelism-study.md`
- `design/store-slice-convergence.md` (R1–R14)
- `design/logging-migration.md`

## Source to cross-check

- `apps/common/xanadu/vortex/` — `mesh_swarm.hpp`, `mesh_host.{hpp,cpp}`, `mesh_value.{hpp,cpp}`,
  `mesh_actor.{hpp,cpp}`, `mesh_cli.cpp`, `vortex_vm.{hpp,cpp}`, `vortex_core.{hpp,cpp}`
- `apps/common/xanadu/vagent/runtime/` — `agent_executor.{hpp,cpp}`, `program.{hpp,cpp}`,
  `compiler.{hpp,cpp}`, `wire.{hpp,cpp}`, `host_bindings.{hpp,cpp}`
- `apps/common/xanadu/zigzag/` — `manifold.{hpp,cpp}`, `arena_manifold.{hpp,cpp}`
- `apps/common/xanadu/` — `store.{hpp,cpp}`, `provenance.{hpp,cpp}`, `system_docs.cpp`
- `apps/vagent/main.cpp`, `Makefile`, `AGENTS.md`, `.claude/rules/architectural_governance.md`

## House rules every critic enforces

1. **Argued, not asserted:** cite `file:line`, price costs, record refused alternatives.
1. **Cells and dimensions over side tables or bit-packing.**
1. **No sentinels in APIs:** `std::optional`/`std::expected`, never reserved values (only at the
   wire or on-disk boundary).
1. **No arbitrary ceilings:** a required bound is priced and exposed as a setting or grant limit.
1. **Vortex over C++:** new C++ must justify why it is not Vortex (hardware/OS interfacing,
   allocator primitives, network transport framing and similar are valid reasons).
1. **Proposal register:** any change to existing engine/runtime/core files must be a numbered
   proposal with files, why it cannot live in new code, cost and fallback.
1. Markdown must satisfy `mdformat --wrap 100` (gfm, dollarmath, frontmatter) and `mdl` (100
   columns).

## Severity and format

- **BLOCKING:** cannot work as specified, violates a hard invariant, or contradicts existing code
  without a numbered proposal.
- **MAJOR:** an implementer would build the wrong thing or be blocked; a layer or C++-vs-Vortex rule
  is broken; a proposal lacks justification, price or fallback.
- **MINOR:** polish.

Verify every claim you make about the code by reading it; quote the doc and cite `file:line`. A
finding you could not verify must say so. Do not pad: a short accurate critique beats a long one.

Write exactly this shape to your assigned file:

```markdown
# Round <N> critique: <Role Name> (vagent-cluster-packaging.md)

Paths below are relative to the repo root.
Tally: <B> BLOCKING, <M> MAJOR, <m> MINOR.

## BLOCKING

### 1. (§X.Y) <Short title under 80 chars>

**Problem:** ...

**Evidence:** ...

**Fix:** ...

## MAJOR

## MINOR

VERDICT: CONVERGED | NOT CONVERGED
```

`CONVERGED` means zero BLOCKING and zero MAJOR remain in your lane.

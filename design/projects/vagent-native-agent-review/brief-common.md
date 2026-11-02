# Common brief: critique of `design/vagent-native-agent.md`

Repository root: `/data/git/gleditor`. Another session may be working in this checkout: you are
**read-only** except for writing your one assigned round file in
`design/projects/vagent-native-agent-review/`. Do not edit, format, stage or commit anything else.
Do not run builds, tests or programs.

## Target

`design/vagent-native-agent.md` (v0.1): a proposal for vagent **native agents**. A native module
builds the agent's program through recorded "native ops" (a C ABI, `vn_*`). The control plane
replays and verifies them into a Vortex graph and runs the agent's turns. Native code (FFI via
libffi, module-native skills, percept drivers) runs in a per-agent sandboxed process
(`vagent-native-host`) that may write only shared memory, its IPC socket and a log pipe, plus
descriptors handed over by authority grants. The first test is ncurses through libffi on a brokered
pty that the agent must request. Proposals P20.1–P20.6 touch existing code.

The owner's request it answers: "create a design doc for a native agent for vagent, fulfilling the
agent contract just as a pure vortex agent would (constructing its vortex graph with native ops) and
capable of accessing native libraries but blocked from most writes, by likely platform specific
mechanisms, except for shared memory and whatever else is required for ipc communication with the
control plane, that leverages vortex's multiprocess support to isolate the restricted native code.
The first test of this new feature should be accessing ncurses with libffi, which will require the
native agent to request write access to terminals."

Judge whether the design meets that request, as well as whether it is internally sound.

## Sibling documents

- `design/vagent-agent-language.md` (the specification; §§3–7, 10, 11, 13–15, 18 matter most)
- `design/vagent-runtime-grounding.md`, `design/vagent-implementation.md`
- `design/vagent-shared-program-and-live-upgrade.md` (P17)
- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md` (P19)
- `design/vagent-open-question-resolutions.md`, `design/vagent-module-checking-plan.md`
- `design/vortex-distribution.md`, `design/vortex-mesh-cli.md`,
  `design/vortex-transport-and-or-parallelism-study.md`, `design/vortex-multithreading-study.md`,
  `design/vortex-hyperstructural-runtime.md`, `design/vortex-scoped-evaluation-abi.md`
- `design/store-slice-convergence.md` (R1–R14)

## Source to cross-check

- `apps/common/xanadu/vagent/runtime/` — `agent_executor.{hpp,cpp}`, `host_bindings.{hpp,cpp}`,
  `program.{hpp,cpp}`, `compiler.{hpp,cpp}`, `runtime_abi.hpp`, `wire.{hpp,cpp}`, `prelude.cpp`
- `apps/common/xanadu/vortex/` — `mesh_actor.{hpp,cpp}`, `mesh_host.{hpp,cpp}`, `mesh_cli.cpp`,
  `mesh_value.hpp`, `vortex_vm.{hpp,cpp}`, `vortex_core.{hpp,cpp}`, `worker_context.hpp`
- `apps/common/xanadu/zigzag/arena_manifold.{hpp,cpp}`
- `apps/common/xanadu/provenance.cpp` (the `posix_spawn` precedent), `apps/vagent/main.cpp`,
  `system_docs.cpp`, `Makefile`
- `AGENTS.md`, `.claude/rules/architectural_governance.md`

## House rules every critic enforces

1. **Argued, not asserted:** cite `file:line`, price costs, record refused alternatives.
1. **Cells and dimensions over side tables or bit-packing.**
1. **No sentinels in APIs:** `std::optional`/`std::expected`, never reserved values (only at the
   wire or on-disk boundary).
1. **No arbitrary ceilings:** a required bound is priced and exposed as a setting or grant limit.
1. **Vortex over C++:** new C++ must justify why it is not Vortex (hardware/OS interfacing,
   allocator primitives and similar are valid reasons).
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
# Round <N> critique: <Role Name> (vagent-native-agent.md)

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

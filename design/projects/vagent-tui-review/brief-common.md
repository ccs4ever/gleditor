# Common Brief: Vortex Design Critique of Terminal UI Architecture & vagent TUI Dashboard

This review evaluates two companion design documents for the sovereign terminal stack in `gleditor`:

1. **`design/terminal-ui-architecture.md`**: Sovereign Monospace Presentation, Virtual Screen
   Buffers, and Reactive Event Loops (General, reusable terminal UI foundation).
1. **`design/vagent-tui-dashboard.md`**: Interactive Cluster Monitoring, Epistemic Arbitration
   Inspection, and Deductive Logic REPL (Domain-specific `vagent` dashboard and implementation
   plan).

Repository Root: `/data/git/gleditor`

______________________________________________________________________

## Relevant Sources and Prior Designs

### Source Code

- `apps/common/xanadu/vagent/tui/agent_tui.hpp` and `.cpp` (Current prototype TUI)
- `apps/common/xanadu/vagent/tui/ncurses_actor.hpp` and `.cpp` (Current terminal/screen actor)
- `apps/vagent/main.cpp` (`runTui` event loop and CLI dispatch)
- `apps/common/xanadu/vagent/runtime/agent_executor.hpp` and `.cpp` (`AgentExecutor`, `HostSink`,
  `AgentSettings`)
- `apps/common/xanadu/vortex/mesh_actor.hpp` and `.cpp` (`MeshActorRuntime`, `ActorPid`,
  `ActorRecord`)
- `apps/common/xanadu/zigzag/arena_manifold.hpp` and `.cpp` (Epistemic belief arenas and Vlog
  relations)
- `apps/common/xanadu/vprolog/compiler.hpp` and `.cpp` (`vprolog::Compiler` query compilation and
  execution)
- `tests/xuzz/vagent_tui_test.cpp` (Headless TUI tests)

### Sibling Design Documents

- `design/vagent-agent-language.md` (Agent grammar, directives, epistemic arbitration)
- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md` (P18/P19 epistemic
  arbitration)
- `design/vagent-cluster-packaging.md` (Cluster packages, multi-target deployment)
- `design/vagent-native-agent.md` (Sandboxed native host process)
- `design/vlog-logic-extension.md` (Vlog deductive engine and relation schema)

______________________________________________________________________

## Standing Design Rules & House Style

1. **Argued, Not Asserted**: Justify every mechanism citing exact repository files and lines
   (`file:line`).
1. **Cells and Dimensions Over Side Tables**: State and structure live on orthogonal ZigZag ranks
   (`d.*`).
1. **No Sentinels in APIs**: Absent values use `std::optional` or `std::expected`.
1. **No Arbitrary Ceilings**: Avoid hardcoded small caps; expose needed limits as settings.
1. **Vortex Over C++**: Justify why any proposed C++ cannot be expressed in Vortex bytecode or
   stdlib.
1. **Proposal Register**: Any changes to existing C++ files must be isolated in numbered proposals
   (`P22.1`–`P22.8`).
1. **Strict Headless Determinism**: 100% testable without a real display, terminal, or X11/Wayland.
1. **Markdown Hygiene**: Strict 100-column wrapping (`mdformat --wrap 100`) and `mdl -i` compliance.

______________________________________________________________________

## Review Deliverable Format

Each critic writes its evaluation to `design/projects/vagent-tui-review/r1-<role>.md`:

```markdown
# Round 1 critique: <Role Name> (terminal-ui-architecture.md & vagent-tui-dashboard.md)

Paths below are relative to the repo root.
Tally: <B> BLOCKING, <M> MAJOR, <m> MINOR.

## BLOCKING

### 1. (§X.Y, line L) <Short title under 80 chars>

**Problem:** <Precise statement of defect, contradiction, or missing invariant.>

**Evidence:** <Exact quotes from docs + `file:line` citations from codebase.>

**Fix:** <Concrete replacement wording, architecture, or proposal.>

## MAJOR
...

## MINOR
...

VERDICT: CONVERGED | NOT CONVERGED
```

Severity definitions:

- **`BLOCKING`**: Cannot work as specified, violates hard invariants (e.g. headless safety, memory
  safety, 2-degree rank invariant, API sentinel violations), or contradicts existing C++ code
  without a numbered proposal.
- **`MAJOR`**: Implementer would build the wrong thing, unhandled edge cases, layer boundary
  violations, unjustified C++ vs Vortex, or uncosted proposal.
- **`MINOR`**: Wording polish, table formatting, minor non-blocking clarifications.

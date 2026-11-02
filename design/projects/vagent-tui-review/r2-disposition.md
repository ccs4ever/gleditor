# Round 2 Disposition: Terminal UI Architecture & vagent TUI Dashboard

Date: 2026-11-02 Reviewers:

- System Architect (`architect`): 0 BLOCKING, 0 MAJOR, 4 MINOR — **VERDICT: CONVERGED**
- Technical Writer (`writer`): 3 BLOCKING, 4 MAJOR, 5 MINOR — NOT CONVERGED
- Vortex & Spool-Mesh Expert (`vortex`): 3 BLOCKING, 4 MAJOR, 2 MINOR — NOT CONVERGED
- Codebase Expert (`codebase`): 2 BLOCKING, 4 MAJOR, 3 MINOR — NOT CONVERGED
- Programming Language Design & Ergonomics Expert (`ergo`): 0 BLOCKING, 4 MAJOR, 3 MINOR — NOT
  CONVERGED
- Xanadu / ZigZag Purist (`zigzag`): 2 BLOCKING, 4 MAJOR, 3 MINOR — NOT CONVERGED

______________________________________________________________________

## Executive Summary & Core Architectural Adjudications

Round 2 achieved complete convergence from the **System Architect** (`VERDICT: CONVERGED`). The
remaining five critics isolated five sharp, actionable issues:

1. **`Envelope` / `HostSink` Fields and Wire Formatting (`vagent-tui-dashboard.md` §5)**:

   - *Critics*: `writer` (B3), `vortex` (B3), `codebase` (B2), `zigzag` (B1).
   - *Ruling*: **ACCEPTED**. In `apps/common/xanadu/vagent/runtime/wire.hpp:102–117`, `Envelope` has
     fields `from`, `type` (not `topic`), and `fields` of type `WireRecord` (not `payload`).
     Formatting is performed via `runtime::renderEmission(envelope)` or iterating
     `envelope.fields.names` and `values`. Furthermore, `LocalHost`'s pre-existing `hostSink_` is
     preserved by chaining calls rather than silently overwriting.

1. **Reactor Loop Clocks, Deadlines & Notification APIs (`terminal-ui-architecture.md` §5.1)**:

   - *Critics*: `writer` (B2), `vortex` (B2), `codebase` (B1, M3), `architect` (m4).
   - *Ruling*: **ACCEPTED**.
     - `AgentExecutor` deadlines use `std::chrono::system_clock::time_point`. Replaced steady_clock
       comparisons with system_clock.
     - Proposal `P22.9` now explicitly declares all three methods on `AgentExecutor`:
       `notificationDescriptor()`, `drainNotifications()`, and `earliestDeadline()`.
     - `runTuiLoop` calls `executor.tick(now)` alongside `executor.pumpUntilIdle()` to fire due
       timers.
     - Added `device.hasPendingInput()` to `TerminalDevice` and checked it before `::poll()` so
       injected inputs in tests wake immediately with `timeoutMs = 0`.
     - Handled `POLLHUP | POLLERR` by requesting graceful exit `tui.requestExit()` when stdin
       closes.

1. **Deductive REPL Solution Buffering & Pagination State**:

   - *Critics*: `vortex` (B1), `ergo` (M1), `zigzag` (M6).
   - *Ruling*: **ACCEPTED**. Resolved the pagination vs turn-atomicity contradiction: the scoped
     evaluation frame compiles the query and pre-resolves up to `maxQuerySolutions = 50` solutions
     into an in-memory `std::vector<std::string> solutionBuffer_` within the immediate mark/release
     scope (`arena.mark()` / `arena.release()`), then immediately releases the arena. The UI pages
     through `solutionBuffer_` 5 at a time (`Solutions 1–5 of N (Press ';' for more)`). This leaves
     zero choice points or marks on the live arena, prevents turn corruption, and enables
     instantaneous local pagination.

1. **Proposal Register Numbering & Attribution Harmonization**:

   - *Critics*: `writer` (B4, M5), `codebase` (M2).
   - *Ruling*: **ACCEPTED**. Harmonized Proposals `P22.1`–`P22.10` consistently across both
     documents:
     - `P22.1`: `TerminalDevice` Decoupled Driver (`apps/common/ui/tui/terminal_device.{hpp,cpp}`).
     - `P22.2`: Contiguous Flat `VirtualScreenBuffer`
       (`apps/common/ui/tui/screen_buffer.{hpp,cpp}`). Corrected pricing to $120 \\times 30 \\times
       20 \\text{ B} = 72 \\text{ KiB}$.
     - `P22.3`: Multiplexed Reactive Event Loop & Signal Handler (`apps/vagent/main.cpp`).
     - `P22.4`: Viewport Virtualization & Scroll Protocol (`apps/common/ui/tui/viewport.hpp`).
     - `P22.5`: `HostSink` Bounded Action Log Streaming & Chaining
       (`apps/common/xanadu/vagent/tui/agent_tui.{hpp,cpp}`, `apps/vagent/main.cpp`).
     - `P22.6`: Responsive Master-Detail Dual-Mode Presentation
       (`apps/common/xanadu/vagent/tui/agent_tui.cpp`).
     - `P22.7`: Scoped Evaluation Frame & Solution Buffer Pagination
       (`apps/common/xanadu/vagent/tui/agent_tui.cpp`).
     - `P22.8`: Command/Query History Draft Stashing & In-Line Line Editing
       (`apps/common/xanadu/vagent/tui/agent_tui.cpp`).
     - `P22.9`: `AgentExecutor` Notification Descriptor, Drain & Earliest Deadline
       (`apps/common/xanadu/vagent/runtime/agent_executor.{hpp,cpp}`).
     - `P22.10`: `system://settings` TUI Configuration Binding (`apps/vagent/main.cpp`,
       `apps/common/xanadu/system_docs.hpp`).
     - Mapped Roadmap Tasks 1.1–3.4 directly to numbered proposals.

1. **Metadata and Glyph Clarity**:

   - *Critics*: `zigzag` (B2, M4), `writer` (M1).
   - *Ruling*: **ACCEPTED**.
     - Removed phantom `ArenaManifold` from `rests on` in `terminal-ui-architecture.md` (low-level
       I/O does not depend on ZigZag).
     - Corrected block element `█` (U+2588) to single-width (`width == 1`).
     - Ensured standard GFM table formatting across all tables.

______________________________________________________________________

## Adjudication by Finding

### 1. Technical Writer (`writer`)

- **B1 (Compound argument traversal)**: RESOLVED in R1.
- **B2 (Hallucinated AgentExecutor APIs in P22.9)**: **ACCEPTED**. Added `earliestDeadline()` and
  `drainNotifications()` to Proposal `P22.9`.
- **B3 (Contradictory HostSink lambda snippet)**: **ACCEPTED**. Corrected to `envelope.type`,
  `envelope.fields`, and `renderEmission`.
- **B4 / M1 (Table formatting)**: **ACCEPTED**. Re-formatted all tables to standard GFM without
  escapes.
- **M5 (Proposal mapping to roadmap)**: **ACCEPTED**. Explicitly mapped Phase 1–3 tasks to
  `P22.1`–`P22.10`.
- **m1–m5 (Minor style and typography)**: **ACCEPTED**.

### 2. Vortex & Spool-Mesh Expert (`vortex`)

- **B1 (Vlog REPL pagination vs turn atomicity)**: **ACCEPTED**. Pre-resolve solutions into
  `solutionBuffer_` before releasing mark.
- **B2 (Reactive loop clocks & tick omission)**: **ACCEPTED**. Reconciled clocks with `system_clock`
  and added `executor.tick(now)`.
- **B3 (HostSink callback signature and fields)**: **ACCEPTED**. Reconciled with `Envelope` struct.
- **M1 (Actor lease expiration during manual step)**: **ACCEPTED**. Documented lease suspension or
  refresh during interactive debugging.
- **M2 (Advance clock execution)**: **ACCEPTED**. Mapped `advance` to
  `executor.advanceSimulatedTime()`.
- **M3 (Belief key formatting for numeric/boolean terms)**: **ACCEPTED**. Cites
  `vprolog::formatTerm`.
- **m1–m2 (Telemetry notation and step scope)**: **ACCEPTED**.

### 3. Codebase Expert (`codebase`)

- **B1 (Clock mismatch & missing methods in P22.9)**: **ACCEPTED**.
- **B2 (Envelope fields & WireRecord methods)**: **ACCEPTED**.
- **M1 (LocalHost hostSink\_ overwrite)**: **ACCEPTED**. Chained into previous sink.
- **M2 (Proposal P22.8 and pricing math)**: **ACCEPTED**.
- **M3 (TerminalDevice hasPendingInput)**: **ACCEPTED**. Added `hasPendingInput()`.
- **M4 (Stdin POLLHUP exit request)**: **ACCEPTED**. Handled via `tui.requestExit()`.
- **m1–m3 (Minor API nuances)**: **ACCEPTED**.

### 4. Programming Language Design & Ergonomics Expert (`ergo`)

- **M1 (Deductive REPL solution buffer)**: **ACCEPTED**.
- **M2 (Widget focus traversal order)**: **ACCEPTED**. Documented Tab cycle: Left Table $\\to$ Right
  Context $\\to$ Input box.
- **M3 (Mode indicator and keybindings)**: RESOLVED in R1.
- **M4 (Typed command error reporting)**: **ACCEPTED**. Formatted typed `ActorError` strings.
- **m1–m3 (Help text, prompt glyph, syntax help)**: **ACCEPTED**.

### 5. Xanadu / ZigZag Purist (`zigzag`)

- **B1 (HostSink fields and causal provenance)**: **ACCEPTED**. Retained `ordinal` and `floor` in
  log.
- **B2 (ArenaManifold in rests on header)**: **ACCEPTED**. Removed from low-level driver doc.
- **M1 (P19 epistemic traversal specification)**: **ACCEPTED**. Documented `d.epistemic` and
  `d.source` ranks.
- **M2 (Full block character width == 1)**: **ACCEPTED**. Corrected to single-width `width = 1`.
- **M3 (U-TUI-1 clarification)**: **ACCEPTED**.
- **m1–m3 (Positional syntax, settings bounds, generic loop)**: **ACCEPTED**.

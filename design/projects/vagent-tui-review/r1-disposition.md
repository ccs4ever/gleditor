# Round 1 Disposition: Terminal UI Architecture & vagent TUI Dashboard

Date: 2026-11-02 Reviewers:

- Technical Writer (`writer`): 4 BLOCKING, 6 MAJOR, 6 MINOR
- Vortex & Spool-Mesh Expert (`vortex`): 3 BLOCKING, 4 MAJOR, 3 MINOR
- Codebase Expert (`codebase`): 3 BLOCKING, 4 MAJOR, 3 MINOR
- System Architect (`architect`): 3 BLOCKING, 6 MAJOR, 4 MINOR
- Programming Language Design & Ergonomics Expert (`ergo`): 2 BLOCKING, 7 MAJOR, 4 MINOR
- Xanadu / ZigZag Purist (`zigzag`): 2 BLOCKING, 4 MAJOR, 4 MINOR

Total Findings: 17 BLOCKING, 31 MAJOR, 24 MINOR across all 6 critics. All six critics rendered
`VERDICT: NOT CONVERGED`.

______________________________________________________________________

## Executive Summary & Core Architectural Adjudications

The six critics converged on seven core systemic defects across `design/terminal-ui-architecture.md`
and `design/vagent-tui-dashboard.md`:

1. **Topological Argument Traversal (ZigZag 2-Degree Invariant)**:

   - *Critics*: `writer` (B1), `codebase` (B2), `zigzag` (B1), `ergo` (M4).
   - *Ruling*: **ACCEPTED**. Section 4.2 in `vagent-tui-dashboard.md` erroneously asserted
     traversing arguments along `ctx->core.dims().grab`. In the Vortex runtime ABI
     (`runtime_abi.hpp:23`, `vlog.cpp:33–55`, `compiler.cpp:1615`), `+d.grab` connects strictly to
     the *first* argument, and all subsequent argument cells chain along `+d.step`. Revised to
     specify exact head-to-tail rank stepping.

1. **Reactor Loop, Headless Determinism & Non-Existent Executor APIs**:

   - *Critics*: `writer` (B2), `vortex` (B2), `codebase` (B1, B3), `architect` (B1, B2).
   - *Ruling*: **ACCEPTED**. `runTuiLoop` called hallucinated methods (`notificationDescriptor`,
     `drainNotifications`, `earliestDeadline`) without an actor pump or signal handling, and looped
     100% CPU on headless EOF.
   - *Remediation*:
     - Added `P22.9` (`AgentExecutor::notificationDescriptor` / eventfd wakeup).
     - Specified self-pipe / signalfd multiplexing in `poll(2)` for clean terminal restoration on
       `SIGINT`/`SIGTERM`/`SIGWINCH`.
     - Specified headless detection (`isatty`) disabling `STDIN_FILENO` polling when EOF is reached.
     - Mandated `executor.pumpUntilIdle()` invocation when wakeups fire.

1. **Deductive Vlog REPL & Live Arena Isolation**:

   - *Critics*: `vortex` (B1), `ergo` (B2), `zigzag` (M5).
   - *Ruling*: **ACCEPTED**. Running queries on the live worker arena leaves marks/choice points
     that break turn atomicity, trigger `ArenaRefusal::MarksOutstanding` during `compact()`, and
     fail live upgrades.
   - *Remediation*: Scoped evaluation frame: queries execute within an immediate mark/release pair
     (`Mark m = arena.mark(); ... arena.release(m);`) or snapshot shadow arena, pre-resolving a
     bounded page of results (default 5) before returning to the reactor loop.

1. **Outbound Emission Streaming (`HostSink`) Signature and Wire Record**:

   - *Critics*: `vortex` (B3), `codebase` (B1), `architect` (B3), `zigzag` (M3).
   - *Ruling*: **ACCEPTED**. Reconciled `setHostSink` with actual signature
     `std::function<void(const Envelope &)>` in `agent_executor.hpp:301`. Replaced non-existent
     `WireRecord::toJson()` with `formatWireValue(envelope.payload)`.
   - Bounded action log in `AgentTui` using a ring buffer (capacity `maxActionLogEntries` default
     500\) to eliminate unbounded memory growth.

1. **Modal Input Isolation Ergonomics**:

   - *Critics*: `ergo` (B1, M1, M2), `codebase` (M3).
   - *Ruling*: **ACCEPTED**. Submitting a command with `Enter` now **remains in EDIT mode** so
     sequential commands and queries can be entered without `'s'` triggering `step`. Explicit exit
     from EDIT mode requires `Esc` (on empty input), `Tab`, or pane switching. Added visual `[EDIT]`
     vs `[NAV]` indicator on the status strip. Stashed uncommitted input drafts during history
     navigation.

1. **Flat Contiguous Screen Buffer Layout**:

   - *Critics*: `architect` (M6), `codebase` (M2).
   - *Ruling*: **ACCEPTED**. Replaced `std::vector<std::vector<ScreenCell>> grid_` with contiguous
     flat `std::vector<ScreenCell>` of `width * height`, eliminating 31 separate heap allocations
     and double pointer indirection.

1. **Proposal Register Consolidation (P22.1–P22.10)**:

   - *Critics*: `codebase` (M1), `architect` (M8), `writer` (M3).
   - *Ruling*: **ACCEPTED**. Re-indexed and unified proposals across both documents:
     - `P22.1`: `TerminalDevice` I/O driver (`apps/common/ui/tui/terminal_device.{hpp,cpp}`).
     - `P22.2`: Unicode `ScreenCell` & Flat `VirtualScreenBuffer`
       (`apps/common/ui/tui/screen_buffer.{hpp,cpp}`).
     - `P22.3`: Reactor `poll(2)` event loop & signal multiplexing (`apps/vagent/main.cpp`).
     - `P22.4`: Viewport Virtualization & Scroll Protocol (`apps/common/ui/tui/viewport.hpp`).
     - `P22.5`: `HostSink` bounded ring buffer & wire formatter
       (`apps/common/xanadu/vagent/tui/agent_tui.{hpp,cpp}`).
     - `P22.6`: Responsive Master-Detail dual-mode presentation
       (`apps/common/xanadu/vagent/tui/agent_tui.cpp`).
     - `P22.7`: Deductive REPL Scoped Evaluation Frame & pagination
       (`apps/common/xanadu/vagent/tui/agent_tui.cpp`).
     - `P22.8`: Command history draft stashing & line editing
       (`apps/common/xanadu/vagent/tui/agent_tui.cpp`).
     - `P22.9`: `AgentExecutor` notification eventfd descriptor
       (`apps/common/xanadu/vagent/runtime/agent_executor.{hpp,cpp}`).
     - `P22.10`: `system://settings` dynamic configuration binding for TUI (`apps/vagent/main.cpp`).

______________________________________________________________________

## Adjudication by Critic and Finding

### 1. Technical Writer (`writer`)

- **B1 (Compound argument traversal on dims().grab)**: **ACCEPTED**. Cites `runtime_abi.hpp:23` and
  `compiler.cpp:1615`. Corrected to first argument on `+d.grab`, subsequent on `+d.step`.
- **B2 (Hallucinated AgentExecutor APIs omitted from proposals)**: **ACCEPTED**. Added proposal
  `P22.9` and corrected loop description.
- **B3 (Broken and missing cross-references between companion documents)**: **ACCEPTED**. Reconciled
  companion section links and proposal references.
- **B4 (Missing P22 proposal registrations for core features)**: **ACCEPTED**. Added proposals
  `P22.6`–`P22.10`.
- **M1 (Ambiguous and inconsistent proposal numbering P22.4 vs P22.6/P22.7)**: **ACCEPTED**. Unified
  and disambiguated register numbering.
- **M2 (Status strip keyboard shortcut conflict F1 vs ?)**: **ACCEPTED**. Changed help overlay to
  `F10` / `?` (in NAV mode), reserving `F1` for Actor Registry.
- **M3 (ASCII wire mockup deviates from live runtime format)**: **ACCEPTED**. Updated mock to match
  `formatWireValue`.
- **M4 (Unspecified lifecycle for query and command history buffers)**: **ACCEPTED**. Documented
  ring buffer bounds (`maxHistoryEntries = 100`).
- **M5 (Unreferenced ArenaManifold in rests on header)**: **ACCEPTED**. Documented exact relation in
  scoped evaluation frame and belief traversal.
- **M6 (Missing EBNF grammar rules for new CLI commands)**: **ACCEPTED**. Added EBNF for
  `vagent tui` subcommands in `vagent-tui-dashboard.md` §4.3.
- **m1–m6 (Minor polish, links, table headers, style)**: **ACCEPTED**.

### 2. Vortex & Spool-Mesh Expert (`vortex`)

- **B1 (Deductive Vlog REPL mutates live arena and destroys turn atomicity)**: **ACCEPTED**.
  Specified scoped evaluation frame with immediate mark/release and isolated solution extraction.
- **B2 (Reactive loop relies on hallucinated APIs and omits turn pumping)**: **ACCEPTED**. Added
  `P22.9` and mandated `executor.pumpUntilIdle()` in loop.
- **B3 (HostSink callback signature and wire API contradict C++)**: **ACCEPTED**. Corrected to
  `std::function<void(const Envelope &)>` and `formatWireValue`.
- **M1 (Single-step and advance commands bypass actor turn boundaries)**: **ACCEPTED**. Clarified
  that `step` pumps one cycle per actor; `advance <duration>` advances simulated clock and
  dispatches expired timers.
- **M2 (Interactive REPL lacks execution fuel limit)**: **ACCEPTED**. Added query fuel limit
  (`maxQueryFuel = 10'000`) within the scoped evaluation frame.
- **M3 (Cross-arena raw CellRef leak in belief traversal)**: **ACCEPTED**. Mandated that `AgentTui`
  extracts string/scalar values and never retains raw `CellRef` across turns.
- **M4 (Pre-flight upgrade validation misses actor state compatibility)**: **ACCEPTED**. Clarified
  scope of `upgrade-check` (verifies schema equivalence and field mapping).
- **m1–m3 (Spool-Mesh clock, causal floor, term limit)**: **ACCEPTED**.

### 3. Codebase Expert (`codebase`)

- **B1 (Hallucinated AgentExecutor APIs in main.cpp)**: **ACCEPTED**. Resolved via `P22.9`.
- **B2 (2-Degree rank invariant violation in Vlog traversal)**: **ACCEPTED**. Resolved via
  `+d.step`.
- **B3 (Curses stdio starvation & headless hangs in poll)**: **ACCEPTED**. Added stdio buffer drain
  check before `poll(2)` and handle `isatty` EOF.
- **M1 (Proposal register accounting gaps)**: **ACCEPTED**. Full Proposal Register updated.
- **M2 (2D vector fragmentation in VirtualScreenBuffer)**: **ACCEPTED**. Replaced with 1D flat
  buffer.
- **M3 (Input draft loss on history navigation)**: **ACCEPTED**. Specified `draft_` buffer stash.
- **M4 (Overwriting LocalHost internal hostSink\_)**: **ACCEPTED**. Changed to chain/dispatch to
  existing sink if present.
- **m1–m3 (Typed std::expected for errors, optional notifyFd, escape delay)**: **ACCEPTED**.

### 4. System Architect (`architect`)

- **B1 (Reactor loop spins on stdin EOF and calls missing methods)**: **ACCEPTED**.
- **B2 (Terminal corruption on unhandled exit signals)**: **ACCEPTED**. Added signalfd/self-pipe
  cleanup.
- **B3 (Unbounded memory growth in action log stream)**: **ACCEPTED**. Added ring buffer capacity
  cap.
- **M1 (Generic terminal stack coupled to vagent)**: **ACCEPTED**. Decoupled device abstractions to
  `apps/common/ui/tui/` with clean interface.
- **M2 (Missing Vortex justification and keymap centralization)**: **ACCEPTED**. Added C++ vs Vortex
  justification in §1.1 and specified `system://keymap` integration.
- **M3 (Jagged heap allocation in VirtualScreenBuffer)**: **ACCEPTED**. Flattened `grid_`.
- **M4 (werase() violates differential rendering invariant T3)**: **ACCEPTED**. Specified curses
  diff-cache preservation (`werase` only on full dirty/resize, else character overwrites).
- **M5 (Duplicate proposal numbering P22.4/P22.6/7)**: **ACCEPTED**. Re-indexed.
- **M6 (runTui does not load system://settings)**: **ACCEPTED**. Added `P22.10` loading settings
  from `SystemStoreModel`.
- **m1–m4 (Wide character continuation, palette fallbacks, timing exposure, solution buffer)**:
  **ACCEPTED**.

### 5. Programming Language Design & Ergonomics Expert (`ergo`)

- **B1 (Modal input isolation: Enter dropping back to Navigation Mode)**: **ACCEPTED**. `Enter`
  executes and stays in EDIT mode.
- **B2 (Deductive REPL ergonomics & pagination state machine)**: **ACCEPTED**. Defined
  `QueryPaginationState` and bounded solver evaluation.
- **M1 (Ctrl+Q flow control collision)**: **ACCEPTED**. Changed quit hotkey to `Ctrl+C` or `q` (NAV
  mode).
- **M2 (Tab key semantic conflation)**: **ACCEPTED**. Established 2-tier hierarchy (`F1`–`F3` panes,
  `Tab` widgets).
- **M3 (Missing visual mode indicators)**: **ACCEPTED**. Added `[NAV]` / `[EDIT]` badge to status
  strip.
- **M4 (upgrade-check vs upgrade syntax consolidation)**: **ACCEPTED**. Consolidated into
  `upgrade <target> <programRef> [--check]`.
- **M5 (advance command ignores duration)**: **ACCEPTED**. Corrected clock advance dispatch.
- **M6 (Opaque command error diagnostics)**: **ACCEPTED**. Formatted typed error codes.
- **M7 (String coercion in parseWireValue)**: **ACCEPTED**. Preserved quoted string literals.
- **m1–m4 (History browsing indicators, inline help, multiline solutions, confirmation on kill)**:
  **ACCEPTED**.

### 6. Xanadu / ZigZag Purist (`zigzag`)

- **B1 (Compound argument traversal violates 2-degree rank invariant)**: **ACCEPTED**.
- **B2 (ArenaManifold unreferenced in terminal architecture)**: **ACCEPTED**. Specified manifold
  grounding for cell views and projection.
- **M1 (HostSink drops Spool-Mesh causal provenance)**: **ACCEPTED**. Formatted emission with
  originating node/actor epoch.
- **M2 (Omission of normative multi-rank ZigZag traversal for P19)**: **ACCEPTED**. Documented
  `d.epistemic` and `d.trust` traversal.
- **M3 (Deductive REPL flattens solutions immediately)**: **ACCEPTED**. Addressed in scoped
  evaluation frame.
- **M4 (Viewport virtualization duplicates manifold ranks)**: **DEFERRED (`U-TUI-1`)**. Caching row
  views for virtualized terminal display avoids repeated manifold rank walks on 60Hz redraws.
- **m1–m4 (Syntax consistency, BorderStyle T-junctions, settings table, Mermaid graph)**:
  **ACCEPTED**.

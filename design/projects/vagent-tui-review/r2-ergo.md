# Round 2 critique: PL & Ergonomics (terminal-ui-architecture.md & vagent-tui-dashboard.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 4 MAJOR, 3 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R1.1 (Submit in Edit Mode exits to Nav Mode): RESOLVED — Invariant T4 and §7.1 specify that
  `Enter` retains `EDIT MODE`.
- R1.2 (Unspecified Vlog query pagination protocol): PARTLY — Scoped evaluation frame and fuel
  bounds added, but solution caching lifecycle and `;` key modal dispatch remain contradictory (see
  MAJOR 1).
- R1.3 (Terminal flow control hazard in [^Q] Quit): RESOLVED — `[^Q]` removed; replaced by `q` in
  Navigation Mode and `Ctrl+C`.
- R1.4 (Collision between [F1] Registry and Help): RESOLVED — Help assigned to `F10` / `?`; `F1`
  dedicated to Actor Registry.
- R1.5 (Conflated pane, widget, and mode Tab key): PARTLY — Pane switching separated to `F1`–`F3`,
  but widget focus ring and traversal order across panels remain unspecified (see MAJOR 2).
- R1.6 (Missing mode indicator in UI chrome): RESOLVED — `[NAV]` / `[EDIT]` badge added to status
  line and §7.1.
- R1.7 (upgrade-check command undefined in C++): RESOLVED — Consolidated into
  `upgrade <target> <programRef> [--check]`.
- R1.8 (advance command silently ignores duration): RESOLVED — Simulated clock advance specified in
  §4.3 and loop.
- R1.9 (Opaque errors and feedback in commands): OPEN — Standardized output reporting and typed
  error codes omitted from v0.2 (see MAJOR 4).

______________________________________________________________________

## BLOCKING

(None. All previous BLOCKING architectural failures have been mitigated or downgraded to MAJOR
specification gaps.)

______________________________________________________________________

## MAJOR

### 1. (vagent-tui-dashboard.md §4.2, line 152) QueryPaginationState caching and ';' collision

Full title: QueryPaginationState solution caching lifecycle and ';' modal collision

**Problem:** In `design/vagent-tui-dashboard.md` §4.2 lines 152–156, the scoped evaluation frame and
pagination state machine exhibit two distinct ergonomic and lifecycle flaws:

1. **Solution Caching Lifecycle Defect:** Line 152 specifies: "Solutions are pre-resolved up to
   `pageSize = 5` into detached string representations before returning to the reactor loop", while
   line 155 specifies: "Pressing ';' in Navigation Mode fetches the next page without leaving
   outstanding choice points on the worker arena." In
   `apps/common/xanadu/vprolog/compiler.hpp:62-63`, `Compiler::solve` accepts `maxSolutions`, but
   has no offset/skip or generator resumption parameter:

   ```cpp
   std::vector<zigzag::vortex::LogicSolution>
   solve(const CompiledQuery &query, std::size_t maxSolutions = 100);
   ```

   If resolution is clamped to `pageSize = 5` and the scratch arena mark is immediately released
   (`arena.release(m)`), the choice points and backtracking trail on the arena are destroyed. When
   the user subsequently presses `;` to fetch solutions 6–10, where do they come from? The engine
   cannot resume the query. Re-running the query from scratch without an offset parameter cannot
   skip the first 5 solutions, and if background actor deliberation mutated beliefs in the interim,
   re-execution violates query consistency.

1. **Modal Input Collision on Pagination Key ';':** Under Invariant T4, submitting a query with
   `Enter` retains `EDIT MODE` on the query input field (`Query: ?-`). If the query produces
   multiple solution pages, an operator pressing `;` (the universal Prolog pagination key) will type
   a literal `;` character into `queryInput_` instead of paging solutions. Line 155 specifies that
   `;` is pressed in Navigation Mode, but `;` is neither listed in Navigation Mode hotkeys in
   `terminal-ui-architecture.md` §7.1 line 430 nor advertised on the status bar in
   `vagent-tui-dashboard.md` line 118. To page, the operator is forced to know to press `Esc` to
   enter Navigation Mode, press `;`, and then press `Tab` or click to re-enter Edit Mode.

**Evidence:**

- `design/vagent-tui-dashboard.md:152-156`:

  ```markdown
  - Solutions are pre-resolved up to `pageSize = 5` into detached string representations before
    returning to the reactor loop.
  - An explicit `QueryPaginationState` tracks `activeQuery`, `windowOffset`, and `pageSize`.
    Pressing `;` in Navigation Mode fetches the next page without leaving outstanding choice points
    on the worker arena.
  ```

- `design/terminal-ui-architecture.md:430`:
  `Hotkeys: [1-3], [F1-F3], [Tab], [s] step, [?] help, [Ctrl+C] quit` (omits `;`).

**Fix:**

1. Update `QueryPaginationState` to cache all solutions resolved during the scoped evaluation frame
   up to `maxSolutions = 100`:

   ```cpp
   struct QueryPaginationState {
     std::string activeQuery;
     std::vector<std::string> cachedSolutions;
     std::size_t windowOffset{0};
     std::size_t pageSize{5};

     [[nodiscard]] bool hasMore() const noexcept {
       return windowOffset + pageSize < cachedSolutions.size();
     }
     [[nodiscard]] std::span<const std::string> currentPage() const noexcept;
   };
   ```

   Evaluating the query runs `compiler.solve(compiledQuery, 100)` within `mark()`/`release()`,
   formats solutions into detached strings, populates `cachedSolutions`, and immediately releases
   the arena mark. Paging advances `windowOffset += pageSize` purely in memory with 0% CPU and zero
   arena re-execution.

1. Clarify pagination affordances in the REPL presentation: when `paginationState.hasMore()` is
   true, render a prompt in the results footer:
   `[Solutions 1–5 of 18: Press Esc then ';' for more]`. In Navigation Mode, explicitly bind `;`,
   `Space`, or `PageDown` to advance `windowOffset`.

### 2. (terminal-ui-architecture.md §7.1, line 448) Unspecified widget focus ring and Tab order

Full title: Unspecified widget focus ring and Tab traversal hierarchy

**Problem:** In `design/terminal-ui-architecture.md` §7.1 line 448, the document states: "The
operator exits `EDIT MODE` by pressing `Esc` on an empty line, pressing `Tab` to cycle focus away
from the input widget, or pressing a dedicated navigation function key (`F1`–`F3`)." While
separating pane switching to `F1`–`F3` resolves the collision between panel selection and text
editing, neither document formally specifies the widget focus ring (the ordered list of focusable
widgets within each view) or how `Tab` / `Shift-Tab` cycles through them.

In Master-Detail Split View (width $\ge 110$ cols), the screen displays:

- Left Column: Actor Registry table, Telemetry Card.
- Right Column: Beliefs Table, Vlog REPL (query input + results), Action Log, Command Input.

An implementer cannot determine:

1. Which widgets are focusable? (e.g. Is the read-only Action Log scrollable and focusable? Is the
   Telemetry Card focusable?)
1. What is the cyclic `Tab` traversal order? Does `Tab` jump between columns, or cycle through all
   focusable widgets on the entire screen?
1. When `Tab` focus lands on a text input widget (`Query:` or `command>`), does it automatically
   transition into `EDIT MODE`, or does it remain in `NAVIGATION MODE` until `Enter` or a character
   key is pressed? (If focusing an input immediately enters `EDIT MODE`, single-key navigation is
   disabled as soon as `Tab` reaches that widget).

**Evidence:**

- `design/terminal-ui-architecture.md:430, 448`: Lists `[Tab]` as a Navigation Mode hotkey and
  mentions `Tab` cycling focus away from input fields, but provides no widget focus list.
- `design/vagent-tui-dashboard.md:118`: Status bar shows `[Tab] Focus`, but does not specify target
  widgets.
- `design/projects/vagent-tui-review/r1-disposition.md:190-192`: Claimed to establish a 2-tier
  hierarchy, but omitted the widget ring specification.

**Fix:** Formally define the focus hierarchy and widget ring in `terminal-ui-architecture.md` §7.1
and `vagent-tui-dashboard.md` §3.1:

1. **Focusable Widgets Table**:
   - `ActorRegistryTable`: Focusable. `Up`/`Down` selects actor; `PageUp`/`PageDown` scrolls
     viewport.
   - `TelemetryCard`: Non-focusable (read-only summary).
   - `BeliefsTable`: Focusable. `Up`/`Down` selects belief; `PageUp`/`PageDown` scrolls viewport.
   - `VlogQueryInput`: Focusable input. Focus enters `EDIT MODE`; `Esc` returns to `NAV MODE`.
   - `ActionLogView`: Focusable log. `Up`/`Down`/`PageUp`/`PageDown` scrolls historical emissions.
   - `OrchestratorCommandInput`: Focusable input. Focus enters `EDIT MODE`; `Esc` returns to `NAV`.
1. **Focus Rings**:
   - **Stacked Tab View**: `Tab` / `Shift-Tab` cycles strictly within the active pane:
     - Pane 1 (Monitoring): `ActorRegistryTable` (sole focusable widget).
     - Pane 2 (Querying): `BeliefsTable` $\leftrightarrow$ `VlogQueryInput`.
     - Pane 3 (Orchestration): `ActionLogView` $\leftrightarrow$ `OrchestratorCommandInput`.
   - **Master-Detail Split View**: `Tab` cycles clockwise across all active interactive widgets:
     `ActorRegistryTable` $\rightarrow$ `BeliefsTable` $\rightarrow$ `VlogQueryInput` $\rightarrow$
     `ActionLogView` $\rightarrow$ `OrchestratorCommandInput` $\rightarrow$ `ActorRegistryTable`.
1. **Focus Visual Cue**: The active widget draws its outer box border in bold `Color::Cyan`, while
   inactive widgets draw in dim `Color::White`.

### 3. (vagent-tui-dashboard.md §4.3, line 161) Missing EBNF and `<jsonPayload>` tokenization

Full title: Missing command grammar (EBNF) and `<jsonPayload>` tokenization ambiguity

**Problem:** In `design/projects/vagent-tui-review/r1-disposition.md` line 124–125, the disposition
explicitly accepted finding M6: "Added EBNF for vagent tui subcommands in vagent-tui-dashboard.md
§4.3." However, `vagent-tui-dashboard.md` §4.3 in v0.2 contains no EBNF grammar rules.

Furthermore, the command signatures in §4.3 introduce three critical syntactic ambiguities:

1. **`<jsonPayload>` Parsing vs Space Tokenization:** The signatures for `percept` and `send` are
   specified as: `percept <target> <name> <jsonPayload>` `send <target> <eventType> <jsonPayload>`
   In `apps/common/xanadu/vagent/tui/agent_tui.cpp:27-47`, the command line tokenizer splits on
   whitespace (`std::isspace`). If an operator enters a standard JSON payload with whitespace (e.g.
   `send 002 ping {"latencyMs": 12, "node": "a"}`), the tokenizer breaks the payload into fragmented
   tokens (`{"latencyMs":`, `12,`, `"node":`, `"a"}`), causing parse failure.
1. **Payload Optionality:** The grammar does not specify whether `<jsonPayload>` is optional. Can an
   operator send a simple event or percept without fields (e.g. `send @selected ping` or
   `percept 002 heartbeat`), or must they pass an empty object `{}`?
1. **`step [<count>]` Target Slot:** §4.3 states: "`step [<count>]`: Advances single-step execution
   for selected or all actors." The syntax provides no target parameter. How does an operator
   distinguish between stepping only the selected actor vs stepping all actors? (e.g. `step 5` vs
   `step @selected 5`).

**Evidence:**

- `design/vagent-tui-dashboard.md:161-170`: Bullets list commands without EBNF.
- `apps/common/xanadu/vagent/tui/agent_tui.cpp:27-47, 593-632`: Tokenizer naively splits on spaces
  outside double quotes; `parseFields` expects `key=val` pairs, not JSON objects.

**Fix:** Insert formal EBNF grammar in `vagent-tui-dashboard.md` §4.3:

```ebnf
Command        ::= SpawnCmd | PerceptCmd | SendCmd | StepCmd | AdvanceCmd | UpgradeCmd | StopCmd ;
SpawnCmd       ::= "spawn" Space AgentType Space AgentName [ Space Payload ] ;
PerceptCmd     ::= "percept" Space Target Space PerceptType [ Space Payload ] ;
SendCmd        ::= "send" Space Target Space EventType [ Space Payload ] ;
StepCmd        ::= "step" [ Space Target ] [ Space TurnCount ] ;
AdvanceCmd     ::= "advance" Space Duration ;
UpgradeCmd     ::= "upgrade" Space Target Space ProgramRef [ Space "--check" ] ;
StopCmd        ::= "stop" Space Target ;

Target         ::= "@selected" | "all" | ShortPid | ActorName | FullPid ;
Payload        ::= JsonObject | KeyValueList ;
Duration       ::= Integer ( "ms" | "s" )? ;
TurnCount      ::= Integer ;
```

Specify that for commands accepting `Payload`, the tokenizer consumes all remaining characters on
the line after the preceding positional argument as a single raw string, allowing valid JSON
literals containing whitespace without requiring escaped quotes.

### 4. (vagent-tui-dashboard.md §4.3, line 161) Unspecified feedback and error diagnostics

Full title: Unspecified command execution feedback and typed error diagnostics

**Problem:** In `design/projects/vagent-tui-review/r1-disposition.md` line 197, the disposition
accepted finding M6: "Formatted typed error codes." However, neither `terminal-ui-architecture.md`
nor `vagent-tui-dashboard.md` specifies the output formatting rules for command results.

In `apps/common/xanadu/vagent/tui/agent_tui.cpp:588-668`:

- Success output is inconsistent and omits parameters: `percept` outputs `"Percept <type> sent"`
  (omits target and fields); `send` outputs `"Event <type> sent"`; `stop` outputs `"Actor stopped"`.
- Errors are opaque: `res.error()` strings or generic messages like `"Failed to stop actor"` conceal
  structured `ActorError` enumerations (e.g. `ActorError::MailboxFull`,
  `ActorError::BudgetExceeded`, `ActorError::UnregisteredProgram`).
- Numeric parsing in `step` silently ignores errors: `std::from_chars` failure leaves `turns = 1`
  without warning the operator when an invalid token is entered (e.g. `step foo`).

**Evidence:**

- `apps/common/xanadu/vagent/tui/agent_tui.cpp:607, 611, 622, 627-629, 664-667`.
- `design/vagent-tui-dashboard.md:161-175`: Specifies command verbs but defines zero feedback or
  error reporting behavior.

**Fix:** Specify standardized command execution feedback in `vagent-tui-dashboard.md` §4.3:

1. **Feedback Destination**: Every command executed from the prompt appends a formatted entry to the
   Action Log (`actionLog_`) and marks the screen dirty.
1. **Output Structure & Styling**:
   - **Success (`[OK]`)**: Bold Green badge, target identity, and operation summary:
     - `[OK] Spawned actor 'worker_2' (pid: 004) [echo.va]`
     - `[OK] Injected percept 'gps' into 002:echo {lat: 12.5, lon: 40.0}`
     - `[OK] Sent event 'Ping' to 003:ping_bot`
     - `[OK] Stepped 5 turn(s) across 3 active actor(s)`
     - `[OK] Advanced clock by 500ms (ran 2 turn(s))`
     - `[OK] Upgraded actor 002:echo to 'echo_v2.va'`
     - `[OK] Stopped actor 002:echo`
   - **Failure (`[ERROR]`)**: Bold Red badge, target identity, error category, and detail string:
     - `[ERROR] Spawn 'worker_2' failed: UnregisteredProgram ("Program 'echo.va' not loaded")`
     - `[ERROR] Send to 003:ping_bot failed: MailboxFull (capacity 100 exceeded)`
     - `[ERROR] Invalid duration '50x': expected integer with optional 'ms' or 's' suffix`
     - `[ERROR] Unknown target '009': actor not found`
1. **Numeric Validation**: Strict error checking on `step` and `advance`; reject invalid numeric
   arguments with an explicit syntax error instead of falling back to default values.

______________________________________________________________________

## MINOR

### 5. (vagent-tui-dashboard.md §7, line 267) Proposal Register divergence for P22.8

**Problem:** In `design/vagent-tui-dashboard.md` §7 line 267, Proposal `P22.8` is defined as
"Proportional Certainty Bar Gauges & Palette Safety". However, in
`design/projects/vagent-tui-review/r1-disposition.md` line 94, `P22.8` was recorded as "Command
history draft stashing & line editing". Command history draft stashing is described in §4.3 and
Phase 2.2, but has no numbered proposal in §7 of `vagent-tui-dashboard.md`.

**Fix:** Reconcile proposal numbering: keep `P22.8` as Command History Draft Stashing & Line
Editing, and index Proportional Certainty Bar Gauges as `P22.11` (or add draft stashing under
`P22.8` in §7).

### 6. (vagent-tui-dashboard.md §3.1, line 118) Mode-aware status bar dynamic rendering

**Problem:** The ASCII status bar in `design/vagent-tui-dashboard.md` §3.1 line 118 illustrates only
the `[NAV]` mode layout. While the `[NAV]` / `[EDIT]` badge is specified, the document does not show
how the displayed hotkeys dynamically update when `EDIT MODE` is active.

**Fix:** Specify the dynamic status bar format for both modes:

```
[NAV]  [F1] Registry [F2] Query [F3] Orchestrate │ [Tab] Focus │ [s] Step │ [F10] Help
[EDIT] [Enter] Submit │ [Esc] Exit to Nav │ [↑↓] History │ [←→] Cursor │ [Tab] Next
```

### 7. (vagent-tui-dashboard.md §3.1, line 113) Multiline solution formatting in split view

Full title: Multiline solution formatting in constrained views

**Problem:** In `design/vagent-tui-dashboard.md` §3.1 line 113, the REPL mockup shows a single-line
result: `Results: X = edge_node`. For queries binding multiple variables or compound structures
(`?- route(Start, End, Hops, Cost).`), joining bindings on a single comma-separated line causes
clipping in split view (~75 columns).

**Fix:** Specify that multiple variable bindings wrap onto subsequent lines indented by 2 spaces,
and that the REPL results container supports vertical scrolling via `replScrollOffset_`.

______________________________________________________________________

VERDICT: NOT CONVERGED

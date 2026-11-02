# Round 1 critique: Ergonomics Expert (terminal-ui-architecture.md & vagent-tui-dashboard.md)

Paths below are relative to the repo root. Tally: 2 BLOCKING, 7 MAJOR, 4 MINOR.

## BLOCKING

### 1. (terminal-ui-architecture.md §7.1, line 338) Submit in Edit Mode exits to Nav Mode

**Problem:** In `design/terminal-ui-architecture.md` §7.1, the modal transition diagram defines
`Submit (Enter)` as an edge transitioning from `EDIT MODE` to `NAVIGATION MODE`. When an operator
submits a command in the Orchestrator prompt or a query in the Vlog REPL, the input executes, the
buffer clears, and the application immediately drops the operator back into `NAVIGATION MODE`. If
the operator immediately types another command (e.g. `step 5`, `spawn Worker b`,
`send @selected ping`, or `status(X)`), their very first keystroke `'s'` is interpreted as a
Navigation Mode accelerator, instantly triggering `step(1)`. This re-introduces the exact
catastrophic single-key collision that modal separation was introduced to solve.

**Evidence:**

- `design/terminal-ui-architecture.md:337-338`:

  ```
              Focus Input Field     |     Esc (when input empty)
           (Tab, Enter, or Click)   |   or Submit (Enter)
                                    v
                    +--------------------------------+
                    |           EDIT MODE            |
  ```

- `apps/common/xanadu/vagent/tui/agent_tui.cpp:764-766`:

  ```cpp
        else if (event.ch == 's')
          step(1);
  ```

- Under this state machine, chaining two commands requires typing:
  `<Enter> spawn Worker a <Enter> <Enter> step 5 <Enter>`. Forgetting the second `<Enter>` causes
  `'s'` to step an active actor instead of typing into the command prompt.

**Fix:** Invert the exit condition. Submitting an input with `Enter` must execute the command or
query, append the result to the history/results log, clear the input line, and **remain in
`EDIT MODE`**. Leaving `EDIT MODE` must be an explicit, conscious modal departure: pressing `Esc`
when the input buffer is empty, pressing `Tab`/`Shift-Tab` to cycle focus to an adjacent panel, or
pressing an explicit pane selection hotkey (`F1`–`F3`, `Alt+1`–`Alt+3`).

### 2. (vagent-tui-dashboard.md §4.2, line 143) Unspecified Vlog query pagination protocol

**Problem:** `design/vagent-tui-dashboard.md` §4.2 specifies: "Solutions display with pagination:
`Solutions 1–5 of 18 (Press ';' for more)`." However, neither document specifies an input handling
state machine for solution pagination. In `EDIT MODE`, pressing `;` appends a semicolon character to
`queryInput_`. In `NAVIGATION MODE`, `;` is unmapped and ignored. Furthermore,
`vprolog::Compiler::solve()` (`apps/common/xanadu/vprolog/compiler.hpp:63`) executes synchronously
and eagerly to exhaustion (up to 100 solutions); in `agent_tui.cpp:294`, it is invoked directly on
the main thread. If a deductive logic query contains cyclic rules, unbounded terms, or deep search
spaces, this eager call freezes the entire TUI thread, violating Invariant T2 (Zero-CPU Quiescence /
Responsive Reactor).

**Evidence:**

- `design/vagent-tui-dashboard.md:143`:
  `Solutions display with pagination: Solutions 1–5 of 18 (Press ';' for more).`

- `apps/common/xanadu/vagent/tui/agent_tui.cpp:294-313`:

  ```cpp
  auto compiledQuery = compiler.compileQuery(query);
  auto solutions     = compiler.solve(compiledQuery);
  ```

  `queryVlog()` formats all solutions eagerly into `std::vector<std::string>` with zero state for
  offsets, pagination, or incremental backtracking.

- `apps/common/xanadu/vprolog/compiler.hpp:62-63`:

  ```cpp
  std::vector<zigzag::vortex::LogicSolution>
  solve(const CompiledQuery &query, std::size_t maxSolutions = 100);
  ```

**Fix:**

1. Define an explicit `QueryPaginationState` in `AgentTui` holding `activeQuery`, `cachedSolutions`,
   `windowOffset`, and `pageSize` (default 5).
1. When query solutions exceed the page size, transition the REPL into a transient
   `InputMode::QueryPagination` state:
   - `;` or `Space` or `n`: Displays the next page of solutions.
   - `Enter`, `.`, or `Esc`: Accepts the current solutions, dismisses pagination, and returns focus
     to the `?-` prompt ready for the next query.
   - `a`: Aborts resolution.
1. Replace the eager `Compiler::solve` call with a fuel-bounded evaluation or step-limited solver
   invocation to prevent freezing the UI thread on runaway deductive searches.

______________________________________________________________________

## MAJOR

### 3. (vagent-tui-dashboard.md §3.1, line 116) Terminal flow control hazard in [^Q] Quit

**Problem:** In `design/vagent-tui-dashboard.md` §3.1 line 116, the mockup status bar advertises
`[^Q] Quit`. In POSIX terminals, `Ctrl+Q` (`^Q`) is XON (software flow control), paired with
`Ctrl+S` (XOFF). When curses initializes under `cbreak()` (specified in
`design/terminal-ui-architecture.md:161`), `IXON` is left enabled in termios. Pressing `Ctrl+Q` is
frequently intercepted by the terminal line discipline or SSH client. Furthermore,
`AgentTui::handleInput` in `apps/common/xanadu/vagent/tui/agent_tui.cpp:767` only handles printable
`'q'`, not `Ctrl+Q`. Advertising an unhandled, terminal-unsafe control sequence degrades operator
ergonomics.

**Evidence:**

- `design/vagent-tui-dashboard.md:116`:

  ```
  [F1] Registry  [F2] Query  [F3] Orchestrate │ [Tab] Focus │ [↑↓] Select │ [s] Step │ [?] Help │ [^Q] Quit
  ```

- `design/terminal-ui-architecture.md:161`:
  `3. cbreak(): Disables line buffering while preserving terminal signal generation (Ctrl+C).`

- `apps/common/xanadu/vagent/tui/agent_tui.cpp:766-767`:

  ```cpp
        else if (event.ch == 'q')
          requestExit();
  ```

**Fix:** Standardize exit ergonomics:

1. In `NAVIGATION MODE`, the quit accelerator is `q` (and `F10`), displayed as `[q] Quit`.
1. In `EDIT MODE`, typing `q` appends the character. Exiting the entire application from an edit
   prompt is performed via the explicit command `quit` (or `exit`), or by pressing `Esc` to return
   to Navigation Mode and typing `q`.
1. If an emergency application-wide escape is required, handle `Ctrl+C` (SIGINT) gracefully via the
   reactor loop. Remove all references to `[^Q]` from the status bar and documentation.

### 4. (vagent-tui-dashboard.md §3.1, line 116) Collision between [F1] Registry and Help

**Problem:** In `design/vagent-tui-dashboard.md` §3.1 line 116, `[F1]` is bound to `Registry` (Pane
1). However, in §6 Phase 3.4 (line 201), the implementation plan specifies:
`Add modal Help overlay (? or F1)`. If `F1` opens the Help overlay, it cannot switch to Pane 1.
Moreover, binding `?` to Help in navigation mode poses a collision risk with Prolog query syntax:
standard Prolog queries begin with `?-` (e.g. `?- route(X).`). If modal state leaks or the user
types `?` while focusing the Query pane, Help will be triggered instead of the query prompt.

**Evidence:**

- `design/vagent-tui-dashboard.md:116`:
  `[F1] Registry  [F2] Query  [F3] Orchestrate │ [Tab] Focus │ ...`
- `design/vagent-tui-dashboard.md:201`: `P3_4: Add modal Help overlay (? or F1)`
- `apps/common/xanadu/vprolog/compiler.hpp:44-45`:
  `/// Compiles a query string (e.g. "?- append(X, Y, [a, b])." or "append(X, Y, [a, b]).").`

**Fix:** Resolve the collision by assigning distinct functional keys:

- Reserve `F1`–`F3` strictly for pane selection (`[F1] Registry`, `[F2] Query`, `[F3] Orchestrate`).
- Assign the Help overlay to `F12` (or `F10`), and allow `?` to toggle Help **only** in
  `NAVIGATION MODE` when no text input field is active.

### 5. (terminal-ui-architecture.md §7.1, line 337) Conflated pane, widget, and mode Tab key

**Problem:** The design conflates three distinct navigation concepts onto the `Tab` key without
defining a widget focus hierarchy:

1. `design/terminal-ui-architecture.md:337`: "Focus Input Field (`Tab`, `Enter`, or Click) ->
   `EDIT MODE`".
1. `design/terminal-ui-architecture.md:360`: "In Navigation Mode: `Tab` / `Shift-Tab` cycles focused
   widgets."
1. `design/vagent-tui-dashboard.md:116` & `apps/common/xanadu/vagent/tui/agent_tui.cpp:732-750`:
   `Tab` cycles **panes** (`Monitoring` -> `Querying` -> `Orchestrating`).

If `Tab` cycles panes, there is no keyboard mechanism to navigate between the Actor Table and
Telemetry Card in Monitoring, or between the Beliefs Table and Vlog REPL in Querying. Conversely, if
`Tab` cycles widgets within a pane, users on terminals without function keys lose the ability to
switch panes via `Tab`.

**Evidence:**

- `design/terminal-ui-architecture.md:83-84`:
  `Invariant T4: Navigation across panels, tabs, or widgets is governed by functional keys`

- `apps/common/xanadu/vagent/tui/agent_tui.cpp:732-749`:

  ```cpp
  if (event.code == KeyCode::Tab || event.ch == '\t') {
    if (event.shift) { ... }
    else {
      if (activePane_ == Pane::Monitoring) setPane(Pane::Querying);
      else if (activePane_ == Pane::Querying) setPane(Pane::Orchestrating);
      else setPane(Pane::Monitoring);
    }
    return;
  }
  ```

**Fix:** Formalize a clean 2-level navigation hierarchy:

1. **Pane Level**: Switched globally via `F1`–`F3`, `Alt+1`–`Alt+3`, or by number keys `1`–`3` while
   in `NAVIGATION MODE`.
1. **Widget Focus Level**: `Tab` / `Shift-Tab` cycles focus between focusable widgets inside the
   active pane (e.g. in Pane 2: Beliefs Table -> Query Input Field; in Pane 3: Action Log -> Command
   Input Field).
1. Focusing an input field automatically activates `EDIT MODE`, and pressing `Esc` on an empty input
   field unfocuses the widget back to `NAVIGATION MODE`.

### 6. (terminal-ui-architecture.md §7.1, line 362) Missing mode indicator in UI chrome

**Problem:** While `design/terminal-ui-architecture.md` §7.2 specifies that the hardware cursor is
hidden in Navigation Mode (`curs_set(0)`) and shown in Edit Mode (`curs_set(1)`), neither document
provides any visual mode badge in the UI chrome. Terminal emulators frequently render hardware
cursors faintly, use block styles that obscure text, or run over serial/SSH where cursor visibility
escape sequences are suppressed. Without a clear textual badge, the operator has no visual
affordance indicating whether typing `'s'` will step an agent or type a character into a field.
Furthermore, the status bar in `vagent-tui-dashboard.md:116` statically displays navigation hotkeys
(`[s] Step │ [?] Help`) even when the user is actively typing in `EDIT MODE`.

**Evidence:**

- `design/terminal-ui-architecture.md:362-369`: Cursor visibility toggled via `curs_set`, but zero
  UI mode rendering specified.
- `design/vagent-tui-dashboard.md:105-116`: Header and status bar mockups contain no mode indicator
  (`[NAV]` / `[EDIT]`).

**Fix:**

1. Render a prominent mode badge in the top-right header or bottom status bar:
   - `[NAV]` (Bold Green/Cyan reverse) when in Navigation Mode.
   - `[EDIT]` (Bold Yellow/Black reverse) when an input field is active.
1. Dynamically update the status bar depending on active mode:
   - In `NAV`: `[Tab] Focus Widget │ [↑↓] Navigate │ [s] Step │ [r] Refresh │ [q] Quit`
   - In `EDIT`: `[Enter] Submit │ [Esc] Exit to Nav │ [↑↓] History │ [←→] Move Cursor`

### 7. (vagent-tui-dashboard.md §4.3, line 152) upgrade-check command undefined in C++

**Problem:** `design/vagent-tui-dashboard.md` §4.3 specifies: "Pre-Flight Upgrade Validation:
Command: `upgrade-check <target> <programRef>` compares schemas via `compareUpgradeSchemas()` and
prints diffs without mutating state." However:

1. `upgrade-check` is completely missing from `AgentTui::executeCommand` in
   `apps/common/xanadu/vagent/tui/agent_tui.cpp:588-696`.
1. Proliferating hyphenated command verbs (`upgrade-check`, `spawn-check`) violates standard CLI
   ergonomics. Mutating commands should support dry-run flags
   (`upgrade <target> <programRef> --dry-run` or `--check`).
1. The format of the schema diff is undefined: does it output textual field diffs, colored ANSI
   badges, or a structured report in the Action Log? What occurs if an upgrade is deemed
   incompatible (e.g. unhandled epistemic dimensions)?

**Evidence:**

- `design/vagent-tui-dashboard.md:152-154`:
  `- Pre-Flight: Command upgrade-check <target> <programRef> compares schemas...`
- `apps/common/xanadu/vagent/tui/agent_tui.cpp:649-658`: Only `upgrade` is handled; `upgrade-check`
  results in `Unknown command: 'upgrade-check'`.

**Fix:**

1. Consolidate syntax to `upgrade <target> <programRef> [--check|--dry-run]`.
1. Define the schema comparison output format in the Action Log:
   `[UPGRADE CHECK: target=worker_1, newProgram=v2.va] Schema: COMPATIBLE (+2 beliefs, +1 event)`.
   If incompatible:
   `[UPGRADE CHECK: target=worker_1] REFUSED: Unhandled dimension 'd.belief.custom'`.
1. Add `upgrade-check` / `--check` handling to `executeCommand` in P22.6.

### 8. (vagent-tui-dashboard.md §4.3, line 147) `advance <ms>` silently ignores duration

**Problem:** In `design/vagent-tui-dashboard.md` §4 and
`apps/common/xanadu/vagent/tui/agent_tui.cpp:633`, the command `advance <duration>` is provided to
simulate clock advancement. In `agent_tui.cpp:502-508`, `AgentTui::advanceClock(ms)` logs
`"Advanced clock by {}ms, ran {} turn(s)"`, but calls `executor_.tick()` with **no arguments**!
`AgentExecutor::tick(time_point now)` accepts an explicit time, but `AgentTui` fails to pass it. An
operator who runs `advance 5s` expecting timers, deadlines, or belief staleness intervals (e.g.
`kObsStatus = "stale"`) to advance receives zero simulated time progression, while the UI falsely
reports that time was advanced.

**Evidence:**

- `design/vagent-tui-dashboard.md:147`: `Command: advance <duration>`

- `apps/common/xanadu/vagent/tui/agent_tui.cpp:502-505`:

  ```cpp
  std::size_t AgentTui::advanceClock(std::chrono::milliseconds ms) {
    executor_.tick();
    std::size_t ran = executor_.pumpUntilIdle();
    totalTurns_ += ran;
  ```

- `apps/common/xanadu/vagent/runtime/agent_executor.hpp:394-395`:

  ```cpp
  void tick();
  void tick(std::chrono::system_clock::time_point now);
  ```

**Fix:** Update `AgentTui` to maintain an internal simulated clock cursor:
`std::chrono::system_clock::time_point simulatedNow_{std::chrono::system_clock::now()};`. In
`advanceClock(ms)`, advance `simulatedNow_ += ms` and call `executor_.tick(simulatedNow_)`. This
ensures scheduled deadlines and delivery retries actually fire during simulated clock progression.

### 9. (vagent-tui-dashboard.md §4.3, line 147) Opaque errors and feedback in commands

**Problem:** The orchestration command line violates basic error ergonomics (Nielsen Usability
Heuristics #9: Help users recognize, diagnose, and recover from errors). In `agent_tui.cpp`:

- `spawn`: On failure, logs `"Spawn failed: actor error"`, discarding the underlying `ActorError`
  enum code (e.g. `ActorError::UnregisteredProgram`, `ActorError::BudgetExceeded`).
- `stop`: On failure, logs `"Failed to stop actor"`, concealing the cause.
- `percept`: On success, logs `"Percept <type> sent"`, omitting the target PID and field summary.
- `send`: On success, logs `"Event <type> sent"`, omitting the target PID and payload.
- `step`: If given an invalid token (e.g. `step foo`), `std::from_chars` fails silently and defaults
  to 1 turn without warning the user.

**Evidence:**

- `apps/common/xanadu/vagent/tui/agent_tui.cpp:449, 467, 479, 484, 564, 627-629`.

**Fix:** Standardize command feedback across all commands in `executeCommand`:

- Success output must echo the target, operation, and relevant parameters:
  `[OK] Injected percept 'sensor_gps' into 002:echo {lat: 12.5, lon: 40.0}`
- Failure output must format the exact error reason:
  `[ERROR] Spawn 'tracker_2' failed: UnregisteredProgram ("Program ref 'tracker.va' not found")`
- Numeric arguments must be strictly checked with diagnostic syntax errors:
  `[ERROR] Invalid turn count 'foo'; expected positive integer`.

______________________________________________________________________

## MINOR

### 10. (terminal-ui-architecture.md §3.1, line 84) Function key accessibility on macOS

**Problem:** `F1`–`F3` function keys are frequently intercepted by macOS (Mission Control, display
brightness) or laptop Fn lock keys. While `Alt+1`–`Alt+3` is supported, `Alt` keys on macOS (Option)
produce composed Unicode characters by default unless the terminal is explicitly configured with
"Use Option as Meta Key".

**Evidence:**

- `design/terminal-ui-architecture.md:84`; `design/vagent-tui-dashboard.md:116`.

**Fix:** Explicitly document and preserve number-key navigation (`1`, `2`, `3`) when in
`NAVIGATION MODE`, ensuring single-key pane switching remains universally accessible across all
hardware platforms.

### 11. (vagent-tui-dashboard.md §3.1, line 109) Multiline solution layout in split view

**Problem:** In `design/vagent-tui-dashboard.md` §3.1, the Vlog REPL box in Master-Detail split view
is shown with only 2 lines of text (`Query:` and `Results:`). Deductive logic queries commonly bind
multiple variables: `?- route(Start, End, Hops, Cost).` In `agent_tui.cpp:308`, all bindings are
joined onto a single line with `", "`. In split view (right column ~75 cols), complex bindings are
clipped by `TerminalDevice::print`.

**Evidence:**

- `design/vagent-tui-dashboard.md:109-111`; `apps/common/xanadu/vagent/tui/agent_tui.cpp:308`.

**Fix:** Specify that multiple variable bindings wrap onto subsequent lines indented by 4 spaces,
budget at least 5 lines for the REPL box in split view, and provide vertical scrolling
(`replScrollOffset_`) within the results container.

### 12. (vagent_tui_test.cpp:371) Existing test encodes broken empty-prompt shortcuts

**Problem:** The existing headless unit test in `tests/xuzz/vagent_tui_test.cpp:371-377` explicitly
relies on the old, broken behavior:
`tui.handleInput(KeyEvent{KeyCode::Char, '3'}); EXPECT_EQ(tui.activePane(), Pane::Orchestrating);`
tested while `queryInput_` is empty. When single-key shortcuts are removed from text-entry prompts,
this test will fail unless updated.

**Evidence:**

- `tests/xuzz/vagent_tui_test.cpp:371-377`.

**Fix:** In Proposal `P22.4`, note that `vagent_tui_test.cpp` must be updated to test modal
navigation: pressing `Esc` on an empty prompt to enter `NAVIGATION MODE`, followed by `'3'` or
`Tab`.

### 13. (agent_tui.cpp:49) Quoted string literals coerced to numbers in commands

**Problem:** In `apps/common/xanadu/vagent/tui/agent_tui.cpp:32-34, 49-72`, `tokenize()` strips
quotes unconditionally. `parseWireValue()` subsequently guesses the value type by attempting integer
and float conversions. If an operator types `send @selected update code="123"`,
`parseWireValue("123")` coerces it to `WireValue::integer(123)`. If the agent expected a string, an
ABI type error results. There is currently no syntax to force string typing for digit-only text.

**Evidence:**

- `apps/common/xanadu/vagent/tui/agent_tui.cpp:32-34, 49-72`.

**Fix:** Modify `tokenize()` and `parseFields()` to preserve quote delimiters or tag tokens as
`isQuotedString`, instructing `parseWireValue` to bypass numeric inference and return
`WireValue::text`.

______________________________________________________________________

VERDICT: NOT CONVERGED

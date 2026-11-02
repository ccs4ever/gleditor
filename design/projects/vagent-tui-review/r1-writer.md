# Round 1 critique: Technical Writer (terminal-ui-architecture.md & vagent-tui-dashboard.md)

Paths below are relative to the repo root. Tally: 4 BLOCKING, 6 MAJOR, 6 MINOR.

## BLOCKING

### 1. (vagent-tui-dashboard §4.2) 2-Degree Rank Invariant on dims().grab

Full title: Compound Argument Traversal Specifies Invalid Fan-In on dims().grab Violating Rank
Invariant

**Problem:** Section 4.2 asserts: "Compound Argument Traversal: Iterates all argument terms on
`ctx->core.dims().grab` instead of truncating to the first term, rendering complete tuples:
`target(x: 12.5, y: 40.0)`." In ZigZag topological physics and the Nelsonian rank invariant, each
cell has at most one neighbor along `+d` and one along `-d`. Two argument cells cannot both link to
the clause cell along `+d.grab`.

In the actual Vortex ABI (`apps/common/xanadu/vagent/runtime/runtime_abi.hpp:23`), a predicate
clause links its *first* argument on `+d.grab`, and subsequent terms are chained along `+d.step`.
Specifying traversal of multiple argument terms along `dims().grab` violates the 2-degree invariant,
contradicts the runtime ABI, and would cause an implementer to write code that either fails to find
arguments or causes rank eviction.

**Evidence:**

- `design/vagent-tui-dashboard.md:135`: "- **Compound Argument Traversal**: Iterates all argument
  terms on `ctx->core.dims().grab` instead of truncating to the first term..."
- `apps/common/xanadu/vagent/runtime/runtime_abi.hpp:23`: "a cell with its first argument on
  `+d.grab`, the rest chained on `+d.step`."
- `apps/common/xanadu/vagent/runtime/compiler.cpp:1615`:
  `emitAppendTail(D(a0_), core_.dims().grab, core_.dims().step, a1_);`
- `apps/common/xanadu/vagent/runtime/compiler.cpp:4294`: "// predicate name as content, the
  arguments on `+d.grab` and `d.step`, so"

**Fix:** Correct §4.2 to specify linking the first argument on `+d.grab` and following subsequent
argument terms along the `+d.step` rank.

______________________________________________________________________

### 2. (terminal-ui-architecture §5.1) Hallucinated AgentExecutor APIs

Full title: Hallucinated AgentExecutor Notification Descriptor and Deadline Methods Omitted from
Register

**Problem:** The multiplexed reactor loop in §5.1 relies on three non-existent methods on
`AgentExecutor`:

1. Line 237: `const int notifyFd = executor.notificationDescriptor(); // eventfd or pipe`
1. Line 251: `if (auto deadline = executor.earliestDeadline())`
1. Line 266: `executor.drainNotifications();`

None of these methods exist on `apps/common/xanadu/vagent/runtime/agent_executor.hpp`. Furthermore,
§10's Proposal Register (P22.1–P22.4) only names `apps/vagent/main.cpp` in P22.3, completely
omitting any proposal or modification touching `agent_executor.hpp` or `agent_executor.cpp` to
introduce descriptor wakeups or deadline tracking. Without these methods or a numbered proposal
defining their implementation, the reactor loop cannot compile or function as specified.

**Evidence:**

- `design/terminal-ui-architecture.md:237`:
  `const int notifyFd = executor.notificationDescriptor();`
- `design/terminal-ui-architecture.md:251`: `if (auto deadline = executor.earliestDeadline()) {`
- `design/terminal-ui-architecture.md:266`: `executor.drainNotifications();`
- `apps/common/xanadu/vagent/runtime/agent_executor.hpp:1–748`: Contains no declaration of
  `notificationDescriptor`, `earliestDeadline`, or `drainNotifications`.

**Fix:** Add a dedicated proposal touching `apps/common/xanadu/vagent/runtime/agent_executor.hpp`
and `.cpp` to implement a wake-up descriptor (`int notificationDescriptor() const noexcept` or
`std::optional<int>`),
`std::optional<std::chrono::steady_clock::time_point> earliestDeadline() const`, and
`void drainNotifications()`, or specify an in-process fallback using a bounded poll timeout.

______________________________________________________________________

### 3. (vagent-tui-dashboard §5) Contradictory HostSink Lambda Signature

Full title: HostSink Lambda Signature, WireRecord::toJson, and AgentTui Methods Contradict Existing
Types

**Problem:** Section 5 provides a code snippet installing a `HostSink` on `AgentExecutor`:

```cpp
executor.setHostSink([&tui](const identity::ActorPid &pid,
                            std::string_view eventType,
                            const runtime::WireRecord &payload) {
  auto now = std::chrono::system_clock::now();
  auto timestamp = std::format("{:%H:%M:%S}", now);
  std::string formatted = std::format("[{} EMIT from {}] {} {}",
                                      timestamp, pid.cursor,
                                      eventType, payload.toJson());
  tui.appendActionLog(std::move(formatted));
  tui.markDirty();
});
```

This snippet contradicts existing C++ types and interfaces across four distinct points:

1. `HostSink` in `apps/common/xanadu/vagent/runtime/agent_executor.hpp:301` is defined as:
   `using HostSink = std::function<void(const Envelope &)>;`. It accepts a single
   `const Envelope &`, not three distinct parameters.
1. `Envelope` (`apps/common/xanadu/vagent/runtime/wire.hpp:102–117`) carries `from` (string), `type`
   (string), and `fields` (`WireRecord`). It does not pass an `identity::ActorPid`.
1. `runtime::WireRecord` (`apps/common/xanadu/vagent/runtime/wire.hpp:57–65`) has no member
   `toJson()`. The codebase uses `renderFields(const WireRecord &)` (`apps/vagent/main.cpp:450`) or
   `renderEmission(const Envelope &)` (`apps/vagent/main.cpp:458`).
1. `AgentTui` (`apps/common/xanadu/vagent/tui/agent_tui.hpp:130–132, 175`) has no `appendActionLog`
   method (its `actionLog_` member is private), nor `markDirty()` or `isDirty()`.

Proposal P22.5 ("Connect `executor_.setHostSink` to `AgentTui::appendActionLog` in
`apps/vagent/main.cpp`") fails to account for modifying `HostSink`, `AgentTui`, or `WireRecord`.

**Evidence:**

- `design/vagent-tui-dashboard.md:163–174`: Snippet with 3-arg lambda, `payload.toJson()`,
  `tui.appendActionLog()`, and `tui.markDirty()`.

- `apps/common/xanadu/vagent/runtime/agent_executor.hpp:301`:
  `using HostSink = std::function<void(const Envelope &)>;`

- `apps/common/xanadu/vagent/runtime/wire.hpp:57–65`: `struct WireRecord` (no `toJson()` method).

- `apps/common/xanadu/vagent/runtime/wire.hpp:102–117`: `struct Envelope` (fields: `kind`, `type`,
  `from`, `ordinal`, `floor`, `fields`, `correlation`, `relaySpace`, `relayFor`).

- `apps/vagent/main.cpp:458–460`:

  ```cpp
  std::string renderEmission(const Envelope &event) {
    return event.type + ' ' + renderFields(event.fields);
  }
  ```

- `apps/common/xanadu/vagent/tui/agent_tui.hpp:130, 175`: `actionLog_` is private with only
  const-getter `actionLog()`.

**Fix:** Align the snippet and P22.5 with `using HostSink = std::function<void(const Envelope &)>`,
format the emission using `renderEmission(event)` or `renderFields(event.fields)`, and explicitly
propose adding `appendActionLog(std::string entry)` and `markDirty()` to `AgentTui`.

______________________________________________________________________

### 4. (Proposal Register §10 & §7) Unspecified Proposals P22.1–P22.8

Full title: Proposal Registers P22.1–P22.8 Omit Exact File Impact, Cost Pricing, and Fallback
Strategies

**Problem:** Standing Rule 6 (`brief-common.md:53–54` and SKILL.md §1) mandates: "Any change to
existing C++ engine, core, stdlib, or runtime files must be isolated in a numbered Proposal Register
(`P1`, `P2`, ...) stating the exact files touched, why the change cannot live in the
caller/compiler, the cost, and the fallback if not approved."

Proposals P22.1–P22.4 in `terminal-ui-architecture.md` §10 and P22.5–P22.8 in
`vagent-tui-dashboard.md` §7 are formatted as brief 1-line bullet points. They completely omit:

1. Exact repository files touched (`file:line`).
1. Architectural justification for why changes cannot live in the caller or compiler.
1. Resource pricing (bytes for `ScreenCell` codepoint expansion, CPU wakeups/cycles for the reactor
   loop, memory footprint of history ring buffers).
1. Concrete fallback strategies if the proposals are rejected by the codebase owner.

**Evidence:**

- `design/terminal-ui-architecture.md:404–416`: Bulleted list P22.1–P22.4 without file impacts
  (except P22.3), pricing, or fallbacks.
- `design/vagent-tui-dashboard.md:213–222`: Bulleted list P22.5–P22.8 without file impacts (except
  P22.5), pricing, or fallbacks.
- `SKILL.md:67–70`: Standing Rule 6 definition.

**Fix:** Expand §10 of `terminal-ui-architecture.md` and §7 of `vagent-tui-dashboard.md` into
structured proposals with `Files`, `Why`, `Price / Cost`, and `Fallback` subsections.

______________________________________________________________________

## MAJOR

### 1. (Both docs) Malformed GFM Tables Fail mdformat and CI Checks

Full title: Malformed Markdown Table Delimiters and Squashed Rows Across Both Documents Fail
mdformat

**Problem:** Both documents fail
`mdformat --extensions gfm --extensions dollarmath --extensions frontmatter --wrap 100 --check`. The
markdown table formatting is corrupt in multiple sections:

1. In `terminal-ui-architecture.md`:
   - Front metadata table (lines 3–12): Contains backslash-escaped delimiters `\------------------`
     and multiple rows collapsed into single lines with double pipes `| |`.
   - Section 8 Semantic Colors table (lines 376–384): Multiple rows collapsed onto single lines with
     double pipes `| |`.
   - Section 12 Open Questions table (lines 433–439): Squashed rows with double pipes `| |`.
1. In `vagent-tui-dashboard.md`:
   - Front metadata table (lines 3–14): Contains backslash-escaped delimiters `\------------------`
     and squashed rows.
   - Section 8 Settings table (lines 228–232): Squashed rows with double pipes `| |`.
   - Section 11 Open Questions table (lines 261–264): Squashed rows with double pipes `| |`.

These malformed tables fail the CI format gate (`make format-check`).

**Evidence:**

- Running the repository mdformat gate exits non-zero:

  ```sh
  mdformat --extensions gfm --extensions dollarmath --extensions frontmatter --wrap 100 --check \
    design/terminal-ui-architecture.md design/vagent-tui-dashboard.md
  ```

  Output: `Error: File "design/terminal-ui-architecture.md" is not formatted.`
  `Error: File "design/vagent-tui-dashboard.md" is not formatted.`

**Fix:** Reformat all tables into standard multi-line GFM tables with valid delimiter rows and clean
100-column wrapping.

______________________________________________________________________

### 2. (Both docs) Missing C++ vs. Vortex Architectural Justification

Full title: Absence of C++ vs. Vortex Architectural Justification Violates Standing Rule 5

**Problem:** Standing Rule 5 (`brief-common.md:51–52`, `AGENTS.md`, `GEMINI.md`) states: "New C++
code must justify why it isn't being written in Vortex." Both documents propose extensive C++
structures (`TerminalDevice`, `VirtualScreenBuffer`, `Viewport`, `AgentTui` additions) without
articulating why this stack cannot be implemented in Vortex bytecode or stdlib.

The documents must articulate the architectural boundary: low-level POSIX terminal I/O,
termios/ncurses C ABI integration, kernel descriptor multiplexing (`poll(2)`), and `SIGWINCH` signal
handling require OS facilities unavailable inside sandboxed Vortex actors, while the dashboard acts
as an external host diagnostic tool.

**Evidence:**

- Neither document contains a section or paragraph discussing why the TUI or terminal driver is
  implemented in C++ rather than Vortex.
- `GEMINI.md`: "New C++ code must justify why it isn't being written in Vortex."

**Fix:** Add an explicit paragraph in §1.1 or §3 explaining that raw POSIX terminal I/O,
termios/ncurses C ABI integration, kernel descriptor multiplexing (`poll(2)`), and `SIGWINCH` signal
handling require low-level OS interfaces that are not exposed to sandboxed Vortex actors, while the
dashboard acts as the external host control plane.

______________________________________________________________________

### 3. (terminal-ui-architecture §3 & §5.1) Ambiguous TerminalDevice Ownership

Full title: Ambiguous TerminalDevice Ownership and Double-Polling Between AgentTui and runTuiLoop

**Problem:** In `terminal-ui-architecture.md` §5.1, `runTuiLoop` takes both `TerminalDevice &device`
and `AgentTui &tui`. Line 244 calls `device.pollInput()`, and line 259 calls `tui.render()`.

However, in `apps/common/xanadu/vagent/tui/agent_tui.hpp:70–72, 127, 160`, `AgentTui` itself holds
`std::shared_ptr<NCursesActor> actor_`, calls `actor_->pollInput()` inside `tui.tick()`
(`agent_tui.cpp:862`), and calls `actor_->print(...)` during `render()`.

The document does not specify whether `AgentTui` is being refactored to take `TerminalDevice` by
non-owning reference, if `AgentTui` owns `TerminalDevice`, or if `tui.tick()` is superseded by
`runTuiLoop`. Passing both `device` and `tui` to `runTuiLoop` where both attempt to poll input
creates split ownership and undefined coordination.

**Evidence:**

- `design/terminal-ui-architecture.md:231`:
  `int runTuiLoop(TerminalDevice &device, AgentTui &tui, AgentExecutor &executor)`
- `apps/common/xanadu/vagent/tui/agent_tui.hpp:72`: `std::shared_ptr<NCursesActor> actor = nullptr`
- `apps/common/xanadu/vagent/tui/agent_tui.cpp:862`: `while (auto ev = actor_->pollInput())`

**Fix:** Clarify the ownership model: specify in P22.1/P22.4 that `AgentTui` accepts a
`TerminalDevice &` (or `std::shared_ptr<TerminalDevice>`), delegate all rendering and input draining
through `AgentTui`, and simplify `runTuiLoop` so that input is polled consistently in one place.

______________________________________________________________________

### 4. (terminal-ui-architecture §5.2) KeyCode::Resize Omitted from KeyCode Enum

Full title: KeyCode::Resize Referenced in Input Model is Absent from KeyCode Enum Definition

**Problem:** `terminal-ui-architecture.md` (lines 282, 400) and `vagent-tui-dashboard.md` (line 244)
rely on `KeyEvent{KeyCode::Resize}` to trigger relayout on terminal resizing.

However, `apps/common/xanadu/vagent/tui/ncurses_actor.hpp:20–48` does not include `Resize` in
`enum class KeyCode`. Neither document proposes adding `Resize` to `KeyCode` in their Proposal
Registers.

**Evidence:**

- `design/terminal-ui-architecture.md:282`: `4. Emits KeyEvent{KeyCode::Resize} to AgentTui...`
- `design/terminal-ui-architecture.md:400`:
  `4. Resizing behavior via synthetic KeyEvent{KeyCode::Resize}.`
- `design/vagent-tui-dashboard.md:244`: `Run synthetic resize event (KeyCode::Resize)...`
- `apps/common/xanadu/vagent/tui/ncurses_actor.hpp:20–48`: `enum class KeyCode` ends at `F12` with
  no `Resize`.

**Fix:** Explicitly include `KeyCode::Resize` in the proposal for `TerminalDevice` (P22.1) and
update `KeyCode` enum in `ncurses_actor.hpp`/`terminal_device.hpp`.

______________________________________________________________________

### 5. (vagent-tui-dashboard §6 & §7) Implementation Plan Disconnected from Register

Full title: Implementation Plan Tasks Disconnected from Numbered Proposal Register and Untracked
upgrade-check

**Problem:** The phased implementation plan in §6 uses numbering `1.1`–`1.3`, `2.1`–`2.3`,
`3.1`–`3.4`. §7 defines proposals `P22.5`–`P22.8`. Several planned tasks have no corresponding
proposals:

- Task 1.1 ("Remove single-key shortcuts from edit prompts") is not in P22.5–P22.8.
- Task 1.2 ("Implement werase + wnoutrefresh + doupdate diff rendering") is not in P22.5–P22.8 (nor
  P22.1–P22.4).
- Task 3.4 ("Add modal Help overlay (? or F1)") has no proposal.
- Command `upgrade-check` specified in §4.3 (line 153) is missing from both §6 and §7.

**Evidence:**

- `design/vagent-tui-dashboard.md:153`: `Command: upgrade-check <target> <programRef>...`
- `design/vagent-tui-dashboard.md:184–205`: Mermaid diagram phases.
- `design/vagent-tui-dashboard.md:213–222`: P22.5–P22.8.

**Fix:** Add explicit cross-references linking Phase 1–3 tasks to numbered proposals, and add a
proposal for `upgrade-check` and the modal Help overlay.

______________________________________________________________________

### 6. (Both docs) Complete Absence of file:line Evidential Citations

Full title: Zero file:line Evidential Citations Violates Standing Rule 1 (Argued, Not Asserted)

**Problem:** Standing Rule 1 (`brief-common.md:45–46`) requires: "Argued, Not Asserted: Justify
every mechanism citing exact repository files and lines (`file:line`)." Across all 717 lines of both
documents, there is not a single `file:line` citation.

Files are occasionally mentioned by name (`apps/vagent/main.cpp`), but never with line numbers or
code references. Claims regarding existing deficiencies (screen clears, sleep loops, input traps)
and types (`NCursesActor`, `AgentExecutor`, `AgentTui`, `HostSink`) are asserted without grounding
evidence.

**Evidence:**

- `design/terminal-ui-architecture.md`: 0 `file:line` citations.
- `design/vagent-tui-dashboard.md`: 0 `file:line` citations.
- `brief-common.md:45–46`: Rule 1 definition.

**Fix:** Add exact `file:line` citations throughout both documents when discussing existing defects
and proposed changes (e.g. citing `apps/vagent/main.cpp:1261–1264` for polling sleep loop,
`apps/common/xanadu/vagent/tui/agent_tui.cpp:780–804` for single-key collisions,
`apps/common/xanadu/vagent/runtime/agent_executor.hpp:301` for `HostSink`, etc.).

______________________________________________________________________

## MINOR

### 1. (vagent-tui-dashboard §3.1 & §10) Split Threshold Inconsistency (110 vs. 100)

Full title: Split Threshold Inconsistency Between Layout Specification (110) and Refused
Alternatives (\<100)

**Problem:** Section 3.1 (line 96) and §8 (line 231) define the split threshold as 110 columns
(`Width $\ge 110$ columns`, `vagent.tui.split_threshold = 110`). However, §10 Refused Alternative 3
(line 254) states: "narrow terminals (\<100 cols)".

**Evidence:**

- `design/vagent-tui-dashboard.md:96`: `(Width $\ge 110$ columns)`
- `design/vagent-tui-dashboard.md:231`: `vagent.tui.split_threshold | int64 | 110`
- `design/vagent-tui-dashboard.md:254`: `narrow terminals (<100 cols)`

**Fix:** Harmonize §10 to state `(<110 cols)`.

______________________________________________________________________

### 2. (vagent-tui-dashboard §3.1) Quit Key Discrepancy: [^Q] in Diagram vs. 'q' in Prose

Full title: Quit Keybinding Discrepancy: [^Q] Quit in Diagram Footer vs. 'q' = quit in Prose

**Problem:** Dashboard ASCII diagram footer (line 116) displays `[^Q] Quit` (Ctrl-Q). Prose in
`terminal-ui-architecture.md` lines 334 and 358 documents `'q' for quit` in Navigation Mode.

**Evidence:**

- `design/vagent-tui-dashboard.md:116`: `│ [s] Step │ [?] Help │ [^Q] Quit`
- `design/terminal-ui-architecture.md:334`: `'s' = step, 'q' = quit`
- `design/terminal-ui-architecture.md:358`:
  `Single-character accelerators (s for step, r for refresh, q for quit) are active.`

**Fix:** Clarify whether `q` is Navigation mode quit while `^Q` is global quit, and reflect this
clearly in both documents.

______________________________________________________________________

### 3. (vagent-tui-dashboard §4.2) Proportional Certainty Gauge Lacks ASCII Fallback

Full title: Proportional Certainty Gauge Specifies Unicode Block Elements Without ASCII Fallback

**Problem:** Section 4.2 specifies certainty gauges using Unicode block elements `█` (U+2588) and
`░` (U+2591) without specifying the fallback when `BorderStyle::Ascii` is selected or running in
non-UTF-8 terminals.

**Evidence:**

- `design/vagent-tui-dashboard.md:138`: `[████████░░] 85.0%`
- `design/terminal-ui-architecture.md:110`: `enum class BorderStyle { Ascii, ... }`

**Fix:** Document the ASCII fallback gauge (e.g. `[========..]` or `[########..]`) in §4.2.

______________________________________________________________________

### 4. (terminal-ui-architecture line 10) Header Lists Obsolete NCursesActor

Full title: Metadata Header Lists Obsolete NCursesActor Under rests on Rather Than TerminalDevice

**Problem:** `terminal-ui-architecture.md` line 10 lists `NCursesActor` under "rests on", even
though P22.1 refactors `NCursesActor` into `TerminalDevice`. `vagent-tui-dashboard.md` line 11
correctly lists `TerminalDevice`.

**Evidence:**

- `design/terminal-ui-architecture.md:10`:
  `rests on | VirtualScreenBuffer, NCursesActor, KeyEvent...`
- `design/vagent-tui-dashboard.md:11`: `rests on | AgentTui, TerminalDevice, VirtualScreenBuffer...`

**Fix:** Replace `NCursesActor` with `TerminalDevice` in `terminal-ui-architecture.md` line 10.

______________________________________________________________________

### 5. (terminal-ui-architecture §8) Actor Status RUNNING Omitted from Color Table

Full title: Actor Status RUNNING (Cyan) Omitted from Terminal UI Semantic Color Table

**Problem:** The semantic color table in §8 lists `QUIESCENT` (Green), `BUSY` (Yellow), and
`STOPPED` (Red), but omits `RUNNING`. In `vagent-tui-dashboard.md` §4.1 (line 129), `RUNNING` is
assigned to Cyan.

**Evidence:**

- `design/terminal-ui-architecture.md:378–382`: Color table lists `QUIESCENT`, `BUSY`, `STOPPED`.
- `design/vagent-tui-dashboard.md:129`:
  `green (QUIESCENT), cyan (RUNNING), yellow (BUSY), or red (STOPPED).`

**Fix:** Add `RUNNING` to the `Accent / Focus` row (`Color::Cyan`) in `terminal-ui-architecture.md`
§8.

______________________________________________________________________

### 6. (terminal-ui-architecture §11) Refused Alternative 3 Duplicates Dashboard Scope

Full title: Refused Alternative 3 (Split Panes) Duplicates Dashboard Scope in Architecture Doc

**Problem:** Refused Alternative 3 in `terminal-ui-architecture.md` §11 ("Hardcoded Split-Pane Fixed
Sizes") duplicates Refused Alternative 3 in `vagent-tui-dashboard.md` §10 ("Fixed Split Panes on
Narrow Terminals"). Split panes belong to the dashboard application layer, not the reusable terminal
device stack.

**Evidence:**

- `design/terminal-ui-architecture.md:426`: `3. Hardcoded Split-Pane Fixed Sizes...`
- `design/vagent-tui-dashboard.md:254`: `3. Fixed Split Panes on Narrow Terminals...`

**Fix:** Replace Refused Alternative 3 in `terminal-ui-architecture.md` with a device-level
alternative, such as "Direct Raw ANSI Escape Sequencing Without Curses Terminfo".

______________________________________________________________________

VERDICT: NOT CONVERGED

# Round 3 critique: Technical Writer (terminal-ui-architecture.md & vagent-tui-dashboard.md)

Paths below are relative to the repo root. Tally: 1 BLOCKING, 2 MAJOR, 5 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R2.B1 / R1.M1 (Corrupted GFM tables fail mdformat gate): OPEN (STILL BLOCKING) — Despite the R2
  disposition asserting that all tables were reformatted to standard GFM without escapes, all 6
  tables across both documents remain squashed on single lines with escaped delimiters `\---` and
  double pipes `| |`. `mdformat --check` on `terminal-ui-architecture.md` fails with exit code 1.
- R2.B2 / R1.B3 (Hallucinated Envelope fields in HostSink): RESOLVED — `vagent-tui-dashboard.md` §5
  now chains `prevSink`, accesses legitimate `Envelope` members (`from`, `ordinal`, `type`), and
  removes non-existent `payload`, `topic`, and `formatWireValue`.
- R2.B3 / R1.B2 (Omission of earliestDeadline in P22.9): RESOLVED — Proposal P22.9 in both documents
  explicitly enumerates `notificationDescriptor()`, `drainNotifications()`, and `earliestDeadline()`
  with `std::optional<std::chrono::system_clock::time_point>`, reconciling with `AgentExecutor`.
- R2.M1 (Missing formal EBNF command grammar in §4.3): RESOLVED — `vagent-tui-dashboard.md` §4.3
  replaces the informal `<jsonPayload>` placeholders with a formal EBNF grammar defining subcommands
  and `FieldEntry` (`key=val`) assignments.
- R2.M2 (Disconnected roadmap & dropped proposal): RESOLVED — Every task in §6 now maps to numbered
  proposals (`P22.2`–`P22.9`), and P22.8 was restored in §7 covering history, draft stashing, line
  editing, and modal help.
- R2.M3 / R1.M6 (Zero file:line evidential citations in terminal architecture): OPEN (STILL MAJOR) —
  `terminal-ui-architecture.md` still contains zero `file:line` citations across all 597 lines,
  violating Standing Rule 1.
- R2.M4 (Unspecified AgentTui dirty tracking and action log append): OPEN (STILL MAJOR) — While §5
  describes dirty flags and ring buffering, Proposal P22.5 omits method signatures for
  `appendActionLog(std::string)`, `isDirty() const noexcept`, `markDirty() noexcept`, and
  `clearDirty() noexcept` on `AgentTui`.

______________________________________________________________________

## BLOCKING

### 1. (Both docs) Corrupted GFM Tables Fail mdformat Gate and Break Markdown Formatting

Full title: Corrupted GFM Markdown Tables with Collapsed Inline Rows and Escaped Delimiters Remain
Unfixed Across Both Documents, Failing CI Gates

**Problem:** The Round 2 disposition explicitly asserted:

> "B4 / M1 (Table formatting): ACCEPTED. Re-formatted all tables to standard GFM without escapes."

However, neither document was updated. All six tables across both specifications remain severely
corrupted into single-line blobs with double pipes (`| |`) and escaped delimiter rows
(`\-----------------`).

Because tables are malformed, CommonMark/GFM parsers treat them as unformatted raw paragraph text
littered with literal backslashes and pipes. In `design/terminal-ui-architecture.md`, running the
repository's standard CI markdown formatting gate fails with exit code 1:

```sh
mdformat --extensions gfm --extensions dollarmath --extensions frontmatter --wrap 100 --check \
  design/terminal-ui-architecture.md
```

Output: `Error: File "/data/git/gleditor/design/terminal-ui-architecture.md" is not formatted.`

Because `make format-check` is a required blocking gate in continuous integration
(`.github/workflows/c-cpp.yml`), this regression prevents the design branch from passing CI.

**Evidence:**

1. In `design/terminal-ui-architecture.md`:

   - Metadata header table (lines 3–12):

     ```markdown
     | field | value | | ------------- | \---------------------------------------------------------------------------------------------------------------------------------------------------------------------- | | version | 0.3 | | status | Draft for review (Vortex Design Critique Round 3)...
     ```

   - Section 8 Semantic Colors table (lines 470–478):

     ```markdown
     | Token | ANSI 16 Equivalent | Attribute | Usage in Presentation | | :---------------------- | :\----------------- | :-------- | :----------------------------------------------------------- | | **Accent / Focus** | `Color::Cyan` | Bold | Active box border, active tab, selection pointer | | ...
     ```

   - Section 12 Open Questions table (lines 573–577):

     ```markdown
     | ID | Title | Question | Default Resolution | | :--- | :--- | :--- | :--- | | **U-T1** | SGR 1006 Mouse Tracking | Should mouse support be enabled by default? | Yes, enable `mousemask` when interactive; disable when headless or in dumb terminals. | | **U-T2** | TrueColor (24-bit RGB) | ...
     ```

1. In `design/vagent-tui-dashboard.md`:

   - Metadata header table (lines 3–14):

     ```markdown
     | field | value | | ------------- | \------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | | version | 0.3 | | status | Draft for review (Vortex Design Critique Round 3)...
     ```

   - Section 8 Settings table (lines 331–338):

     ```markdown
     | Key | Type | Default | Description | | :--- | :--- | :--- | :--- | | `vagent.tui.borders` | `string` | `"rounded"` | Border style (`"rounded"`, `"single"`, `"ascii"`) | | `vagent.tui.history_size` | ...
     ```

   - Section 11 Open Questions table (lines 369–373):

     ```markdown
     | ID | Title | Question | Default Resolution | | :--- | :--- | :--- | :--- | | **U-D1** | Live Actor Filter | Should `/` open an interactive actor search filter? | Phase 3 extension; default to unfiltered table. | | **U-TUI-1** | ...
     ```

**Fix:** Reformat every table in both documents into standard GFM tables where each row occupies its
own line, delimiter lines use clean unescaped hyphens (`| --- | --- |`), and `mdformat --check`
passes cleanly.

______________________________________________________________________

## MAJOR

### 1. (terminal-ui-architecture) Complete Absence of file:line Citations in Architecture Document

Full title: Terminal UI Architecture Document Contains Zero file:line Citations, Violating Standing
Rule 1

**Problem:** Standing Rule 1 (*Argued, Not Asserted*) requires that all architectural assertions and
system modifications cite precise repository references (`file:line`). While
`design/vagent-tui-dashboard.md` successfully incorporated 11 evidential citations in v0.3,
`design/terminal-ui-architecture.md` contains **zero `file:line` citations** across its entire 597
lines (`grep -nE '\.(cpp|hpp):[0-9]+' design/terminal-ui-architecture.md` yields 0 matches).

Key architectural claims and mechanisms remain completely unanchored to existing code:

1. The motivation for Invariant T2 (§1.1, §2) asserts that existing loops suffer from polling
   sleeps, but fails to cite the live 30ms sleep loop defect in `apps/vagent/main.cpp:1261–1264`.
1. The `KeyCode` enum (§3) replaces existing key handling, but fails to cite
   `apps/common/xanadu/vagent/tui/ncurses_actor.hpp:20–48` where `KeyCode` is currently defined.
1. The reactor loop (§5.1) invokes `executor.pumpUntilIdle()`, but fails to cite
   `apps/common/xanadu/vagent/runtime/agent_executor.hpp:446`.
1. Section 1.2 justifies C++ vs. Vortex by referencing `ArenaManifold` and `vprolog::Compiler`, but
   omits citations to `apps/common/xanadu/zigzag/arena_manifold.hpp:45` and
   `apps/common/xanadu/vprolog/compiler.hpp:35`.

**Evidence:**

- `design/terminal-ui-architecture.md:1–597`: 0 citations matching `\.(cpp|hpp):[0-9]+`.
- `AGENTS.md` and `design/projects/vagent-tui-review/brief-common.md:45–46`: Rule 1 mandate.

**Fix:** Add exact repository citations across `design/terminal-ui-architecture.md` in §1.1, §1.2,
§2, §3, §5.1, and §10.

______________________________________________________________________

### 2. (vagent-tui-dashboard §7 & terminal-ui-architecture §5.1) Unspecified AgentTui Signatures

Full title: Proposal P22.5 Omits Explicit Method Signatures for appendActionLog, isDirty, markDirty,
and clearDirty Required by HostSink and Reactor Loop

**Problem:** Both documents specify operational mechanisms that call new methods on `AgentTui`:

1. In `design/vagent-tui-dashboard.md` §5 (lines 221–222):

   ```cpp
   tui.appendActionLog(std::move(formatted));
   tui.markDirty();
   ```

1. In `design/terminal-ui-architecture.md` §5.1 (lines 314, 317, 366):

   ```cpp
   if (tui.isDirty()) {
     tui.render();
     device.refresh();
     tui.clearDirty();
   }
   ...
   tui.markDirty();
   ```

1. In `design/vagent-tui-dashboard.md` §5 (line 226): "`AgentTui` maintains dirty flags
   (`isDirty()`, `markDirty()`, `clearDirty()`) and pushes entries into a bounded circular buffer
   (capacity 500)."

However, in the live class definition (`apps/common/xanadu/vagent/tui/agent_tui.hpp:110–147`),
`AgentTui` contains **no dirty-tracking methods** and **no mutating action log method** (only a
const getter `const std::vector<std::string> &actionLog() const noexcept`).

Proposal P22.5 (§7, lines 269–279) lists `agent_tui.hpp` as a touched file, but fails to define the
method signatures:

```cpp
void appendActionLog(std::string entry);
[[nodiscard]] bool isDirty() const noexcept;
void markDirty() noexcept;
void clearDirty() noexcept;
```

Without explicit declarations in the proposal register, implementers lack specifications for dirty
flag propagation (e.g., whether `AgentTui` forwards dirty checks to `VirtualScreenBuffer` or tracks
state independently).

**Evidence:**

- `design/terminal-ui-architecture.md:314, 317, 366`.
- `design/vagent-tui-dashboard.md:221–222, 226`.
- `design/vagent-tui-dashboard.md:269–279` (Proposal P22.5).
- `apps/common/xanadu/vagent/tui/agent_tui.hpp:110–147`.

**Fix:** Amend Proposal P22.5 in `vagent-tui-dashboard.md` §7 to explicitly specify:

```cpp
void appendActionLog(std::string entry);
[[nodiscard]] bool isDirty() const noexcept;
void markDirty() noexcept;
void clearDirty() noexcept;
```

Documenting that `isDirty()` indicates pending damage across panes, and `appendActionLog` pushes to
the 500-entry ring buffer while calling `markDirty()`.

______________________________________________________________________

## MINOR

### 1. (vagent-tui-dashboard §5, l. 217–220) Duplicate Event Type in HostSink Formatted Log

**Problem:** Section 5 constructs `formatted` by embedding `envelope.type` in the bracket header
(`[{} #{}:{} {}]`) and appending `rendered`. In `apps/vagent/main.cpp:458–460`, `renderEmission`
already prefixes `event.type`:

```cpp
std::string renderEmission(const Envelope &event) {
  return event.type + ' ' + renderFields(event.fields);
}
```

This produces duplicate event type strings in the action log (e.g.,
`[11:24:02 #echo_agent:1 PingAck] PingAck { latencyMs: 12 }` rather than the mockup's
`[11:24:02 #echo_agent:1 PingAck] latencyMs=12` in §3.1 line 118).

**Evidence:**

- `design/vagent-tui-dashboard.md:118`: `[11:24:02 #echo_agent:1 PingAck] latencyMs=12`
- `design/vagent-tui-dashboard.md:218–220`:
  `std::format("[{} #{}:{} {}] {}", timestamp, envelope.from, envelope.ordinal, ...)`
- `apps/vagent/main.cpp:458–460`.

**Fix:** In §5 line 220, pass `renderFields(envelope.fields)` or adjust the format string to
`std::format("[{} #{}:{}] {}", timestamp, envelope.from, envelope.ordinal, rendered)` so that the
event type is not printed twice.

______________________________________________________________________

### 2. (terminal-ui-architecture §5.1, l. 292–294) Tight Coupling of Generic runTuiLoop to AgentTui

**Problem:** Section 1.1 establishes the decoupled architecture requirement:

> "Decoupled Architecture: Terminal drivers, screen buffers, and event multiplexing must be cleanly
> decoupled from domain-specific applications (`vagent`, `vquery`), enabling unified reuse across
> all CLI programs."

However, in §5.1, `runTuiLoop` is hardcoded to `AgentExecutor &executor` and `AgentTui &tui`:

```cpp
void runTuiLoop(TerminalDevice &device,
                AgentExecutor &executor,
                AgentTui &tui);
```

Placing domain-specific `AgentTui` and `AgentExecutor` references inside the generic terminal driver
architecture document violates this decoupling invariant.

**Evidence:**

- `design/terminal-ui-architecture.md:50–52` (§1.1).
- `design/terminal-ui-architecture.md:292–294` (§5.1).

**Fix:** Parameterize `runTuiLoop` as a template
`template <typename App, typename WakeupSource> void runTuiLoop(...)` or state in §5.1 that the
provided `runTuiLoop` snippet is the concrete specialization for `apps/vagent/main.cpp`.

______________________________________________________________________

### 3. (terminal-ui-architecture §5.1, l. 296, 369) Undeclared Free Functions in Reactor Loop

**Problem:** The reactor loop invokes `setupSignalDescriptor()` and
`handleSignalEvent(sigFd, device, tui)` without declaring their function signatures, header
location, or namespace.

**Evidence:**

- `design/terminal-ui-architecture.md:296`: `const int sigFd = setupSignalDescriptor();`
- `design/terminal-ui-architecture.md:369`: `handleSignalEvent(sigFd, device, tui);`

**Fix:** Add explicit static function declarations in §5.1 (e.g.
`static int setupSignalDescriptor() noexcept;` and
`static void handleSignalEvent(int sigFd, TerminalDevice &device, AgentTui &tui);`) or document them
as file-static helpers in `apps/vagent/main.cpp`.

______________________________________________________________________

### 4. (terminal-ui-architecture §11, l. 566) Refused Alternative 3 Duplicates Dashboard Scope

**Problem:** In `design/terminal-ui-architecture.md` §11, Refused Alternative 3 ("Hardcoded
Split-Pane Fixed Sizes") addresses multi-pane layout budgeting, duplicating Refused Alternative 3 in
`design/vagent-tui-dashboard.md` §10 ("Fixed Split Panes on Narrow Terminals"). Multi-pane split
budgeting is an application dashboard concern, not a low-level device driver concern.

**Evidence:**

- `design/terminal-ui-architecture.md:566–568`.
- `design/vagent-tui-dashboard.md:358–360`.

**Fix:** Replace Refused Alternative 3 in `terminal-ui-architecture.md` with a device-level
alternative, such as "Direct Escape String Emission via std::cout vs. Flat Screen Buffer Curses
Linkage".

______________________________________________________________________

### 5. (vagent-tui-dashboard §5, l. 217) Out-of-Namespace Qualification for renderEmission

**Problem:** Section 5 line 217 writes: `std::string rendered = runtime::renderEmission(envelope);`.
In the codebase (`apps/vagent/main.cpp:458`), `renderEmission` is an internal helper in the
anonymous namespace of `apps/vagent/main.cpp`, not in `namespace xanadu::vagent::runtime`. Calling
`runtime::renderEmission` would result in a compilation error.

**Evidence:**

- `design/vagent-tui-dashboard.md:217`: `runtime::renderEmission(envelope)`
- `apps/vagent/main.cpp:458`: `std::string renderEmission(const Envelope &event)` in anonymous
  namespace.

**Fix:** In §5 line 217, call `renderEmission(envelope)` directly as a local helper in
`apps/vagent/main.cpp`, or propose promoting `renderEmission` to
`apps/common/xanadu/vagent/runtime/wire.hpp` under `namespace xanadu::vagent::runtime`.

______________________________________________________________________

VERDICT: NOT CONVERGED

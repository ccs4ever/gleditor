# Round 2 critique: Technical Writer (terminal-ui-architecture.md & vagent-tui-dashboard.md)

Paths below are relative to the repo root. Tally: 3 BLOCKING, 4 MAJOR, 5 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R1.B1 (Compound argument traversal on dims().grab): RESOLVED — §4.2 now links the first argument
  on `+d.grab` and chains subsequent arguments along `+d.step` per `runtime_abi.hpp:23`.
- R1.B2 (Hallucinated AgentExecutor APIs omitted from register): PARTLY — Proposal P22.9 was added
  for `notificationDescriptor()`, but `earliestDeadline()` and `drainNotifications()` remain
  unspecified in the Proposal Register.
- R1.B3 (Contradictory HostSink lambda signature): PARTLY — Lambda signature was updated to
  `const Envelope &`, but snippet introduces non-existent fields (`payload`, `topic`) and wire
  helpers (`formatWireValue`), and `AgentTui` still lacks `appendActionLog`.
- R1.B4 (Unspecified proposals P22.1–P22.8): RESOLVED — Proposals P22.1–P22.10 are now structured
  with Files Touched, Rationale, Resource Pricing, and Fallback sections.
- R1.M1 (Malformed GFM tables fail mdformat and CI checks): OPEN (Escalated to BLOCKING) — Tables
  remain collapsed on single lines with escaped delimiters `\---` and double pipes `| |`, causing
  `mdformat --check` to fail with exit code 1.
- R1.M2 (Missing C++ vs. Vortex architectural justification): RESOLVED — Section 1.2 in
  `terminal-ui-architecture.md` provides explicit justification under Standing Rule 5, inherited by
  `vagent-tui-dashboard.md` §2.
- R1.M3 (Ambiguous TerminalDevice ownership): RESOLVED — Control flow is clarified; `TerminalDevice`
  manages raw terminal I/O while `runTuiLoop` centrally drains input and invokes
  `tui.handleInput()`.
- R1.M4 (KeyCode::Resize omitted from KeyCode enum): RESOLVED — `KeyCode::Resize` is now declared in
  `enum class KeyCode` in `terminal-ui-architecture.md` §3 (line 148).
- R1.M5 (Implementation plan tasks disconnected from register): OPEN (Still MAJOR) — Roadmap tasks
  1.1–3.4 lack proposal mappings, and P22.8 was reassigned to certainty gauges, dropping command
  history and line editing from the register.
- R1.M6 (Complete absence of file:line evidential citations): PARTLY — `vagent-tui-dashboard.md`
  added citations, but `terminal-ui-architecture.md` still contains zero `file:line` citations.

______________________________________________________________________

## BLOCKING

### 1. (Both docs) Corrupted GFM Tables Fail mdformat Gate

Full title: Corrupted Markdown Tables with Collapsed Inline Rows and Escaped Delimiters Fail
mdformat and CI Checks

**Problem:** Both documents contain malformed GFM tables where table rows are collapsed onto single
lines separated by double pipes `| |` and delimiter rows are prefixed with backslashes
`\-----------------`.

In `terminal-ui-architecture.md`:

1. Metadata header table (lines 3–12) is squashed into an inline block.
1. Section 8 Semantic Colors table (lines 462–470) has multiple rows collapsed into single lines.
1. Section 12 Open Questions table (lines 562–566) is squashed.

In `vagent-tui-dashboard.md`:

1. Metadata header table (lines 3–14) is squashed into an inline block.
1. Section 8 Settings table (lines 281–286) is squashed.
1. Section 11 Open Questions table (lines 317–321) is squashed.

Running the repository's CI format check (`make format-check` or `mdformat --check`) on
`terminal-ui-architecture.md` **fails with exit code 1**:
`Error: File "/data/git/gleditor/design/terminal-ui-architecture.md" is not formatted.` Because
`make format-check` is a blocking CI gate, this failure prevents CI from passing.

**Evidence:**

- Running mdformat check:

  ```sh
  mdformat --extensions gfm --extensions dollarmath --extensions frontmatter --wrap 100 --check \
    design/terminal-ui-architecture.md
  ```

  Output: `Error: File "design/terminal-ui-architecture.md" is not formatted.`

- `design/terminal-ui-architecture.md:462–463`:

  ```markdown
  | Token | ANSI 16 Equivalent | Attribute | Usage in Presentation | | :---------------------- |
  :----------------- | :-------- | :----------------------------------------------------------- | |
  **Accent / Focus** | `Color::Cyan` | Bold | Active box border, active tab, selection pointer | |
  ```

- `design/vagent-tui-dashboard.md:3–5`:

  ```markdown
  | field | value | | ------------- |
  \-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
  | | version | 0.2 | | status | Draft for review...
  ```

**Fix:** Reformat every table across both documents into standard multi-line GFM tables with one row
per line, standard `| --- | --- |` delimiter lines, and clean wrapping.

______________________________________________________________________

### 2. (vagent-tui-dashboard §5) Hallucinated Envelope Fields in HostSink

Full title: Hallucinated Envelope Member Names (payload, topic) and Non-Existent formatWireValue
Function in HostSink

**Problem:** Section 5 provides a code snippet for wiring `AgentExecutor::setHostSink`:

```cpp
executor.setHostSink([&tui](const runtime::Envelope &envelope) {
  auto now = std::chrono::system_clock::now();
  auto timestamp = std::format("{:%H:%M:%S}", now);
  std::string formattedPayload = runtime::formatWireValue(envelope.payload);
  std::string formatted = std::format("[{} EMIT from {}] {} {}",
                                      timestamp, envelope.from,
                                      envelope.topic, formattedPayload);
  tui.appendActionLog(std::move(formatted));
  tui.markDirty();
});
```

This snippet fails to compile against the live repository types:

1. `runtime::Envelope` (`apps/common/xanadu/vagent/runtime/wire.hpp:102–117`) defines fields:
   `kind`, `type`, `from`, `ordinal`, `floor`, `fields`, `correlation`, `relaySpace`, `relayFor`. It
   has **no member named `payload`** (the record payload is in `envelope.fields`), and **no member
   named `topic`** (the event type string is in `envelope.type`).
1. The function `runtime::formatWireValue` **does not exist** anywhere in the repository. The
   codebase uses `renderEmission(const Envelope &)` (`apps/vagent/main.cpp:458–460`) or
   `renderFields(const WireRecord &)` (`apps/vagent/main.cpp:450–452`).
1. `AgentTui` (`apps/common/xanadu/vagent/tui/agent_tui.hpp:130–132`) still lacks
   `appendActionLog(std::string)` and dirty tracking methods (`markDirty()`, `isDirty()`).

**Evidence:**

- `design/vagent-tui-dashboard.md:188–191`: Accesses `envelope.payload`, `envelope.topic`, and
  `runtime::formatWireValue`.

- `apps/common/xanadu/vagent/runtime/wire.hpp:102–117`:

  ```cpp
  struct Envelope {
    std::string kind;
    std::string type;
    std::string from;
    std::uint64_t ordinal{};
    std::string floor;
    WireRecord fields;
    ...
  };
  ```

- `apps/vagent/main.cpp:458–460`:

  ```cpp
  std::string renderEmission(const Envelope &event) {
    return event.type + ' ' + renderFields(event.fields);
  }
  ```

**Fix:** Correct the snippet to use actual member names and existing formatters:

```cpp
executor.setHostSink([&tui](const runtime::Envelope &envelope) {
  auto now = std::chrono::system_clock::now();
  auto timestamp = std::format("{:%H:%M:%S}", now);
  std::string formatted = std::format("[{} EMIT from {}] {}",
                                      timestamp, envelope.from,
                                      renderEmission(envelope));
  tui.appendActionLog(std::move(formatted));
  tui.markDirty();
});
```

Explicitly specify adding `appendActionLog(std::string)` and `markDirty()` to `AgentTui` in Proposal
P22.5.

______________________________________________________________________

### 3. (terminal-ui-architecture §5.1 & §10) Omission of earliestDeadline in P22.9

Full title: Incomplete Proposal P22.9 Omits Method Signatures for earliestDeadline() and
drainNotifications() Used in runTuiLoop

**Problem:** Section 5.1 specifies the reactor loop calling:

- Line 317: `if (auto deadline = executor.earliestDeadline())`
- Line 357: `executor.drainNotifications();`

Neither method exists on `AgentExecutor` (`apps/common/xanadu/vagent/runtime/agent_executor.hpp`).
Existing code provides only per-actor `std::optional<std::int64_t> nextDeadline(const ActorPid &)`
(lines 386–387).

While Proposal P22.9 in §10 was added for the `notificationDescriptor()` eventfd descriptor, it
completely omits:

1. `std::optional<std::chrono::steady_clock::time_point> earliestDeadline() const noexcept` (and how
   it scans/aggregates deadlines across active actors).
1. `void drainNotifications() noexcept` (performing the 8-byte `read(2)` from the eventfd to reset
   its readiness).

Without these two methods specified in P22.9, an implementer will create a wake-up descriptor but
leave `runTuiLoop` unable to compile or determine poll timeouts.

**Evidence:**

- `design/terminal-ui-architecture.md:317`: `if (auto deadline = executor.earliestDeadline())`
- `design/terminal-ui-architecture.md:357`: `executor.drainNotifications();`
- `design/terminal-ui-architecture.md:530–537`: Proposal P22.9 only mentions eventfd descriptor
  creation, omitting `earliestDeadline()` and `drainNotifications()`.
- `apps/common/xanadu/vagent/runtime/agent_executor.hpp:386–387`:
  `std::optional<std::int64_t> nextDeadline(const identity::ActorPid &pid) const;`

**Fix:** Expand Proposal P22.9 to explicitly define:

```cpp
[[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
earliestDeadline() const noexcept;
void drainNotifications() noexcept;
```

Detailing how `earliestDeadline()` computes the minimum deadline across all active actor intention
heaps and converts it to `steady_clock`.

______________________________________________________________________

## MAJOR

### 1. (vagent-tui-dashboard §4.3) Missing Formal EBNF Grammar for Commands

Full title: Missing Formal EBNF Grammar for Dashboard CLI Commands and Contradictory jsonPayload
Syntax

**Problem:** The Round 1 disposition asserted that formal EBNF grammar rules were added to §4.3 for
all `vagent tui` subcommands. However, §4.3 contains only informal angle-bracket bullet points:

- `percept <target> <name> <jsonPayload>`
- `send <target> <eventType> <jsonPayload>`

This introduces two severe issues:

1. The specification claims the payload is `<jsonPayload>`. In the actual implementation
   (`apps/common/xanadu/vagent/tui/agent_tui.cpp:604, 615`), `percept` and `send` parse
   space-separated `key=val` pairs (`parseFields`), **not JSON**. Passing raw JSON would break on
   whitespace tokenization.
1. Without formal EBNF, whitespace handling, string quoting, token escaping, and target identifier
   forms (`shortPid` vs. full PID vs. agent name) remain formally unspecified.

**Evidence:**

- `design/vagent-tui-dashboard.md:163–164`: Uses `<jsonPayload>` placeholder.
- `apps/common/xanadu/vagent/tui/agent_tui.cpp:604, 608`:
  `Usage: percept <target> <type> [key=val ...]` calling `parseFields(tokens, 3)`.
- `design/projects/vagent-tui-review/r1-disposition.md:124–125`:
  `M6 ... ACCEPTED. Added EBNF for vagent tui subcommands in vagent-tui-dashboard.md §4.3.`

**Fix:** Add a formal EBNF specification in §4.3 and align payload syntax with `key=val` fields:

```ebnf
Command        ::= SpawnCmd | PerceptCmd | SendCmd | StepCmd | AdvanceCmd | UpgradeCmd | StopCmd
SpawnCmd       ::= "spawn" ws+ AgentType ws+ AgentName (ws+ FieldAssignment)*
PerceptCmd     ::= "percept" ws+ Target ws+ PerceptName (ws+ FieldAssignment)*
SendCmd        ::= "send" ws+ Target ws+ EventType (ws+ FieldAssignment)*
StepCmd        ::= "step" (ws+ TurnCount)?
AdvanceCmd     ::= "advance" ws+ Duration
UpgradeCmd     ::= "upgrade" ws+ Target ws+ ProgramRef (ws+ "--check")?
StopCmd        ::= "stop" ws+ Target
FieldAssignment::= Ident "=" Value
Duration       ::= [0-9]+ ("ms" | "s")?
Target         ::= Ident | PidLiteral
TurnCount      ::= [0-9]+
```

______________________________________________________________________

### 2. (vagent-tui-dashboard §6 & §7) Disconnected Roadmap & Dropped Proposal

Full title: Roadmap Tasks Disconnected from Proposal Register and Command History Proposal Dropped
from Register

**Problem:**

1. Roadmap tasks in §6 (Tasks 1.1–1.3, 2.1–2.3, 3.1–3.4) contain no explicit cross-references to
   proposals P22.1–P22.10.
1. In the Round 1 disposition (`r1-disposition.md:94`), Proposal P22.8 was defined as: "`P22.8`:
   Command history draft stashing & line editing (`apps/common/xanadu/vagent/tui/agent_tui.cpp`)."
   However, in `vagent-tui-dashboard.md` §7 (line 266), P22.8 was overwritten with "Proportional
   Certainty Bar Gauges & Palette Safety". Consequently, **Command history draft stashing & line
   editing (Roadmap Task 2.2) has no proposal in the Proposal Register**.
1. Roadmap Task 1.1 (Modal state machine) and Task 3.4 (Help overlay) have no assigned proposals.

**Evidence:**

- `design/vagent-tui-dashboard.md:213–223`: Tasks 2.2, 3.4 listed without proposal references.
- `design/vagent-tui-dashboard.md:266–273`: P22.8 is "Proportional Certainty Bar Gauges".
- `design/projects/vagent-tui-review/r1-disposition.md:94`: P22.8 defined as "Command history draft
  stashing".

**Fix:** Re-index proposals to include all planned changes:

- Proposal P22.8: Command History Draft Stashing & Line Editing (`AgentTui`).
- Proposal P22.11: Proportional Certainty Bar Gauges (`AgentTui`). Explicitly annotate every Roadmap
  task in §6 with its corresponding proposal ID (e.g. `[P22.2]`, `[P22.8]`).

______________________________________________________________________

### 3. (terminal-ui-architecture) Zero file:line Evidential Citations

Full title: Complete Absence of file:line Evidential Citations in Terminal UI Architecture Document

**Problem:** Standing Rule 1 (Argued, Not Asserted) requires that all technical specifications
justify their mechanisms with exact repository citations (`file:line`). Across all 580 lines of
`terminal-ui-architecture.md`, there is **not a single `file:line` citation**.

While `vagent-tui-dashboard.md` added citations in its revision, `terminal-ui-architecture.md`
discusses existing sleep loops, key collision issues, and types (`AgentExecutor`, `AgentTui`,
`NCursesActor`) without grounding them in the codebase.

**Evidence:**

- `design/terminal-ui-architecture.md`: 0 matches for `grep -nE '\.(cpp|hpp):[0-9]+'`.
- `AGENTS.md` and `brief-common.md:45–46`: Rule 1 mandate.

**Fix:** Add exact repository citations in §1.1, §2, §5.1, and §10:

- `apps/vagent/main.cpp:1261–1264` (existing 30ms sleep loop defect).
- `apps/common/xanadu/vagent/tui/agent_tui.cpp:862` (curses input polling in tick).
- `apps/common/xanadu/vagent/tui/ncurses_actor.hpp:20–48` (existing `KeyCode` enum).
- `apps/common/xanadu/vagent/runtime/agent_executor.hpp:446` (`pumpUntilIdle`).

______________________________________________________________________

### 4. (terminal-ui-architecture §5.1) Unspecified AgentTui Dirty Tracking

Full title: Unspecified AgentTui Dirty Tracking Methods (isDirty, markDirty, clearDirty) Called in
Reactor Loop

**Problem:** In `terminal-ui-architecture.md` §5.1 (lines 308, 311, 358) and
`vagent-tui-dashboard.md` §5 (line 193), the reactor loop and host sink invoke:

- `tui.isDirty()`
- `tui.clearDirty()`
- `tui.markDirty()`

However, `AgentTui` (`apps/common/xanadu/vagent/tui/agent_tui.hpp`) has no such dirty tracking
methods. While `VirtualScreenBuffer` declares these methods (`terminal-ui-architecture.md:249–251`),
`AgentTui` does not expose or forward them, and neither Proposal P22.1 nor P22.5 proposes adding
dirty-flag management to `AgentTui`.

**Evidence:**

- `design/terminal-ui-architecture.md:308`: `if (tui.isDirty()) {`
- `design/terminal-ui-architecture.md:311`: `tui.clearDirty();`
- `design/terminal-ui-architecture.md:358`: `tui.markDirty();`
- `apps/common/xanadu/vagent/tui/agent_tui.hpp:1–184`: Zero occurrences of `Dirty`.

**Fix:** Propose adding `isDirty() const noexcept`, `markDirty() noexcept`, and
`clearDirty() noexcept` to `AgentTui` in Proposal P22.5, documenting that `AgentTui` maintains an
internal dirty flag or forwards to `screen_.isDirty()`.

______________________________________________________________________

## MINOR

### 1. (vagent-tui-dashboard §3.1) Layout Column Dimension Discrepancy (34 vs. 38 columns)

Full title: Left Column Width Discrepancy Between Prose (34 cols) and ASCII Diagram (38 cols)

**Problem:** Section 3.1 states: "Left Column (34 columns): Persistent Live Actor Registry".
However, in the ASCII mockup below it, the left box `┌─ [1] ACTOR REGISTRY (3/18) ────────┐` is
exactly 38 columns wide.

**Evidence:**

- `design/vagent-tui-dashboard.md:99`: `Left Column (34 columns)`
- `design/vagent-tui-dashboard.md:107`: Mockup box is 38 characters wide.

**Fix:** Adjust the ASCII diagram box to 34 columns or update prose to 38 columns.

______________________________________________________________________

### 2. (vagent-tui-dashboard §3.1 & §5) Action Log Emission Format Inconsistency

Full title: Mockup Action Log Entry Omits Sender Name Formatted by HostSink

**Problem:** In the §3.1 ASCII diagram (line 115), the action log entry reads:
`[11:24:02 EMIT] PingAck { latencyMs: 12 }`. In §5 (line 189), the emission formatter explicitly
includes the sender name: `[11:24:02 EMIT from echo_agent] PingAck { latencyMs: 12 }`.

**Evidence:**

- `design/vagent-tui-dashboard.md:115`: `[11:24:02 EMIT] PingAck { latencyMs: 12 }`
- `design/vagent-tui-dashboard.md:189`:
  `std::format("[{} EMIT from {}] ...", timestamp, envelope.from, ...)`

**Fix:** Update diagram line 115 to: `[11:24:02 EMIT from echo_agent] PingAck { latencyMs: 12 }`.

______________________________________________________________________

### 3. (vagent-tui-dashboard §4.2) Sentinel Terminology in Belief Traversal ("until noCell")

Full title: Sentinel Wording "until noCell" Violates Standing Rule 3

**Problem:** Section 4.2 states: "The renderer steps along +d.step until noCell". House Style Rule 3
forbids sentinels in APIs; `pos()` and `neg()` in `manifold.hpp` return `std::optional<CellRef>`.
The traversal stops when `!nextCell.has_value()`, not upon encountering a sentinel value.

**Evidence:**

- `design/vagent-tui-dashboard.md:141`: `until noCell`
- `apps/common/xanadu/zigzag/manifold.hpp:105`: `std::optional<CellRef> pos(...)`

**Fix:** Replace `until noCell` with `until std::nullopt`.

______________________________________________________________________

### 4. (terminal-ui-architecture §5.1) Free Function Signatures Omitted for Reactor Helpers

Full title: Undeclared Free Functions setupSignalDescriptor() and handleSignalEvent() in runTuiLoop

**Problem:** `runTuiLoop` references `setupSignalDescriptor()` and `handleSignalEvent()` without
specifying their namespace, parameters, or header declarations.

**Evidence:**

- `design/terminal-ui-architecture.md:292`: `const int sigFd = setupSignalDescriptor();`
- `design/terminal-ui-architecture.md:361`: `handleSignalEvent(sigFd, device, tui);`

**Fix:** Add static helper function declarations in §5.1 or document them as internal helpers in
`apps/vagent/main.cpp`.

______________________________________________________________________

### 5. (terminal-ui-architecture §11) Refused Alternative 3 Redundant with Dashboard Scope

Full title: Refused Alternative 3 in Architecture Document Duplicates Dashboard Application Concern

**Problem:** Refused Alternative 3 in `terminal-ui-architecture.md` §11 ("Hardcoded Split-Pane Fixed
Sizes") duplicates Refused Alternative 3 in `vagent-tui-dashboard.md` §10. Pane sizing is a
dashboard presentation concern rather than a reusable terminal device abstraction.

**Evidence:**

- `design/terminal-ui-architecture.md:555`: `3. Hardcoded Split-Pane Fixed Sizes...`
- `design/vagent-tui-dashboard.md:310`: `3. Fixed Split Panes on Narrow Terminals...`

**Fix:** Replace Refused Alternative 3 in `terminal-ui-architecture.md` with a device-level
alternative, such as "Direct Raw ANSI Escape Sequencing Without Curses Terminfo".

______________________________________________________________________

VERDICT: NOT CONVERGED

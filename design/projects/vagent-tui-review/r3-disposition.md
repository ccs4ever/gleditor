# Round 3 disposition: vagent-tui-review

Disposition of Round 3 critique findings across all six reviewers for
`design/terminal-ui-architecture.md` (v0.3) and `design/vagent-tui-dashboard.md` (v0.3).

Reviewers converged in R2/R3:

- **System Architect**: CONVERGED in R2 (0B, 0M, 4m).
- **Xanadu / ZigZag Purist**: CONVERGED in R3 (0B, 0M, 3m).
- **Programming Language Design & Ergonomics Expert**: CONVERGED in R3 (0B, 0M, 5m).

Reviewers requiring closure in Round 4:

- **Technical Writer**: 1 BLOCKING, 2 MAJOR, 5 MINOR.
- **Vortex & Spool-Mesh Expert**: 0 BLOCKING, 2 MAJOR, 1 MINOR.
- **Codebase Expert**: 0 BLOCKING, 2 MAJOR, 2 MINOR.

______________________________________________________________________

## 1. Technical Writer Findings

### R3-Writer-B1: GFM Table Formatting Violations (`mdformat --check` failure)

- **Ruling**: ACCEPTED.
- **Action**: Expand all single-line collapsed tables in both `design/terminal-ui-architecture.md`
  and `design/vagent-tui-dashboard.md` to standard multi-line GFM tables with one row per line and
  unescaped pipe delimiters. Verify with `python3 -m mdformat --wrap 100` and
  `python3 -m mdformat --check --wrap 100`.

### R3-Writer-M1: Missing Evidential `file:line` Citations in `terminal-ui-architecture.md`

- **Ruling**: ACCEPTED.
- **Action**: Add explicit `file:line` citations throughout `design/terminal-ui-architecture.md`
  referencing `apps/common/xanadu/vagent/tui/ncurses_actor.hpp:20–48`,
  `apps/vagent/main.cpp:240–270`, `apps/common/xanadu/vagent/runtime/agent_executor.hpp:232–350`,
  `apps/common/xanadu/vagent/runtime/wire.hpp:102–117`, and
  `apps/common/ui/slice/viewport.hpp:35–70`.

### R3-Writer-M2: `AgentTui` Dirty Tracking & Append Member Signatures in P22.5

- **Ruling**: ACCEPTED.
- **Action**: Formally enumerate member function signatures in Proposal `P22.5`:
  `void appendActionLog(std::string entry)`, `[[nodiscard]] bool isDirty() const noexcept`,
  `void markDirty() noexcept`, `void clearDirty() noexcept`.

### R3-Writer-m1 to m5: Minor Polish Items

- **Ruling**: ACCEPTED.
- **Action**: Adopt all minor polish items: consistent EBNF terminal quoting, section header
  formatting, and code block tags.

______________________________________________________________________

## 2. Vortex & Spool-Mesh Expert Findings

### R3-Vortex-M1: Proposal P22.9 Omission of `advanceSimulatedTime` and Lease Upkeep

- **Ruling**: ACCEPTED.
- **Action**: Add
  `std::size_t advanceSimulatedTime(std::chrono::milliseconds delta, std::size_t maxTurns = 100)` to
  Proposal `P22.9` and describe lease renewal upkeep across simulated time jumps.

### R3-Vortex-M2: Vlog Compound Argument Scalar Value Extraction

- **Ruling**: ACCEPTED.
- **Action**: In `design/vagent-tui-dashboard.md` §4.2, explicitly specify extraction of scalar
  arguments via `asDouble()`, `asInt64()`, or `vprolog::formatTerm()` rather than calling
  `textOf()`.

### R3-Vortex-m1: Explicit Declaration of `maxQuerySolutions`

- **Ruling**: ACCEPTED.
- **Action**: Formally define `static constexpr std::size_t maxQuerySolutions = 50;` in
  `vagent-tui-dashboard.md` §4.2.

______________________________________________________________________

## 3. Codebase Expert Findings

### R3-Codebase-M1: Missing `AgentExecutor::hostSink()` Accessor and `renderEmission` Qualification

- **Ruling**: ACCEPTED.
- **Action**:
  1. Add `[[nodiscard]] const HostSink &hostSink() const noexcept { return hostSink_; }` to
     `AgentExecutor` and list `apps/common/xanadu/vagent/runtime/agent_executor.hpp` in Proposal
     `P22.5`.
  1. Call `renderEmission(envelope)` directly without the invalid `runtime::` namespace prefix in
     `vagent-tui-dashboard.md` §5.

### R3-Codebase-M2: Omission of `device.isEof()` Check in `runTuiLoop`

- **Ruling**: ACCEPTED.
- **Action**: In `design/terminal-ui-architecture.md` §5.1, immediately check
  `if (device.isEof()) { stdinActive = false; tui.requestExit(); break; }` after draining input.

### R3-Codebase-m1: Negative Integer Sentinel for Notification Descriptor (Invariant T8)

- **Ruling**: ACCEPTED.
- **Action**: Change `notificationDescriptor()` to return `[[nodiscard]] std::optional<int>` instead
  of `int` with `-1` sentinel, updating §5.1, §10, and Proposal `P22.9`.

### R3-Codebase-m2: Unchecked `ESCDELAY` Environment Variable

- **Ruling**: ACCEPTED.
- **Action**: Guard curses `set_escdelay(25)` with
  `if (std::getenv("ESCDELAY") == nullptr) { set_escdelay(25); }`.

______________________________________________________________________

## 4. Plan for Round 4

Apply all remediations across `design/terminal-ui-architecture.md` and
`design/vagent-tui-dashboard.md` (bumping to v0.4), verify with formatters/linters, and re-evaluate
with the Technical Writer, Vortex Expert, and Codebase Expert for final convergence.

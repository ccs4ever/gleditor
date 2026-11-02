# Round 2 critique: System Architect (terminal-ui-architecture.md & vagent-tui-dashboard.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 4 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R1.1 (Reactor loop spins on stdin EOF and calls missing methods): RESOLVED — `poll(2)` loop in
  `terminal-ui-architecture.md` §5.1 handles `isatty(STDIN_FILENO)`, `POLLHUP`/`POLLERR`, indefinite
  quiescence (`timeoutMs = -1`), and formally specifies proposal `P22.9`.
- R1.2 (Terminal corruption on unhandled exit signals): RESOLVED — Signal multiplexing in §3.1 and
  §5.1 intercepts `SIGINT`, `SIGTERM`, and `SIGWINCH` via `signalfd`/self-pipe, ensuring that RAII
  terminal cleanup restores curses attributes before exit.
- R1.3 (Unbounded memory growth in action log stream): RESOLVED — Bounded ring buffer with
  `maxEntries = 500` ($O(1)$ eviction) specified in `vagent-tui-dashboard.md` §4.3, §5, §7
  (`P22.5`), and §8.
- R1.4 (Generic terminal stack coupled to vagent): RESOLVED — Low-level terminal abstractions
  (`TerminalDevice`, `VirtualScreenBuffer`, `Viewport`) cleanly relocated to `apps/common/ui/tui/`
  under namespace `xanadu::ui::tui`, free of actor and identity dependencies.
- R1.5 (Missing Vortex justification and keymap centralization): RESOLVED — Explicit architectural
  justification in §1.2 accounts for OS descriptor multiplexing and ncurses C ABI bindings in C++,
  while anchoring user actions in `system://keymap`.
- R1.6 (Jagged heap allocation in VirtualScreenBuffer): RESOLVED — Screen grid refactored into a
  single flat contiguous `std::vector<ScreenCell>` with $y \times \text{width} + x$ indexing in §4
  and proposal `P22.2`.
- R1.7 (werase() violates differential rendering invariant T3): RESOLVED — Full-window clears
  eliminated from the frame render path; `werase()` is restricted to initial presentation and
  `SIGWINCH` resize in §6.1 and task P1_2.
- R1.8 (Duplicate proposal numbering P22.4/P22.6/7): RESOLVED — Proposal Register unified into a
  consistent, disambiguated sequence (`P22.1`–`P22.10`) cross-referenced across both companion
  documents.
- R1.9 (runTui does not load system://settings): RESOLVED — Proposal `P22.10` added in
  `terminal-ui-architecture.md` §10 and settings table in `vagent-tui-dashboard.md` §8.

______________________________________________________________________

## BLOCKING

None.

______________________________________________________________________

## MAJOR

None.

______________________________________________________________________

## MINOR

### 1. (§4, line 245) Expose contiguous row span in VirtualScreenBuffer

**Problem:** In `terminal-ui-architecture.md` §4, `VirtualScreenBuffer` exposes `at(x, y)` and
`line(y)` (which allocates and formats a `std::string`), but lacks a direct zero-copy accessor for a
single row of cells.

**Evidence:**

From `design/terminal-ui-architecture.md:245-253`:

```cpp
[[nodiscard]] const ScreenCell &at(int x, int y) const;
...
[[nodiscard]] std::string line(int y) const;
```

**Fix:**

Add `std::span<const ScreenCell> row(int y) const` to `VirtualScreenBuffer`:

```cpp
[[nodiscard]] std::span<const ScreenCell> row(int y) const noexcept {
  return std::span<const ScreenCell>(&grid_[y * width_], width_);
}
```

This enables zero-allocation, row-level differential hashing or `std::equal` comparisons during the
curses refresh pass without repeated scalar cell lookup bounds checking.

### 2. (§10, line 530) Include aggregate earliestDeadline in Proposal P22.9

**Problem:** In `terminal-ui-architecture.md` §5.1, `runTuiLoop` calls `executor.earliestDeadline()`
to compute the quiescent `poll(2)` timeout. However, `AgentExecutor` currently exposes only
per-actor deadline queries (`nextDeadline(const identity::ActorPid &)` in `agent_executor.hpp:386`).
Proposal `P22.9` names `notificationDescriptor()`, but does not explicitly mention the aggregate
`earliestDeadline()` query method.

**Evidence:**

In `design/terminal-ui-architecture.md:317`:

```cpp
if (auto deadline = executor.earliestDeadline()) {
```

In `design/terminal-ui-architecture.md:530-536` (`P22.9`), the text specifies the notification
descriptor, but omits `earliestDeadline()`.

**Fix:**

Expand Proposal `P22.9` in §10 to explicitly list `earliestDeadline()` (signature
`std::optional<std::chrono::system_clock::time_point> earliestDeadline() const`) alongside
`notificationDescriptor()` and `drainNotifications()`.

### 3. (§10, line 538) Note system://keymap action resolution in Proposal P22.10

**Problem:** Section 1.2 correctly establishes that interactive keybindings dispatch through the
sovereign `system://keymap` system store. However, Proposal `P22.10` in §10 only mentions loading
configuration parameters from `system://settings`.

**Evidence:**

In `design/terminal-ui-architecture.md:69-70`: "...and interactive keybindings dispatch through the
`system://keymap` system store."

In `design/terminal-ui-architecture.md:538-545` (`P22.10`): "`system://settings` TUI Configuration
Binding ... Loads `vagent.tui.*` configuration parameters dynamically from the sovereign
`system://settings` store via `SystemStoreModel`."

**Fix:**

Update the title and description of `P22.10` to encompass both `system://settings` and
`system://keymap` resolution via `SystemStoreModel` (e.g., resolving key chords to `vagent:*` action
symbols, falling back to compiled-in defaults when running headless or unconfigured).

### 4. (§5.1, line 352) Handle stdin EOF when read() returns 0 in pollInput()

**Problem:** In `runTuiLoop`, stdin polling is disabled when `fds[i].revents & (POLLHUP | POLLERR)`
is observed. However, on certain POSIX TTY configurations or closed pseudo-terminals, `poll(2)`
returns `POLLIN`, and the subsequent `read(2)` inside curses returns 0 (EOF) without `POLLHUP` being
set in `revents`.

**Evidence:**

In `design/terminal-ui-architecture.md:353`:

```cpp
if (fds[i].fd == STDIN_FILENO && (fds[i].revents & (POLLHUP | POLLERR))) {
  stdinActive = false; // Disable stdin polling to prevent busy-spin
}
```

**Fix:**

Specify that `TerminalDevice::pollInput()` reports an EOF condition or provides an `isEof()` query.
If `pollInput()` encounters EOF on `STDIN_FILENO`, `stdinActive` is set to `false`, guaranteeing
that the loop never busy-spins on an unflagged EOF.

______________________________________________________________________

VERDICT: CONVERGED

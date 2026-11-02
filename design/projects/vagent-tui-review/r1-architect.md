# Round 1 critique: System Architect (terminal-ui-architecture.md & vagent-tui-dashboard.md)

Paths below are relative to the repo root. Tally: 3 BLOCKING, 6 MAJOR, 4 MINOR.

## BLOCKING

### 1. (§5.1, line 231) Reactor loop spins on stdin EOF and calls missing methods

**Problem:** The proposed `runTuiLoop` reactor loop cannot compile against the current codebase and
violates Invariant T2 ("Zero-CPU Quiescence") under standard headless/CI execution. It invokes two
methods on `AgentExecutor` (`notificationDescriptor()` and `drainNotifications()`) that do not exist
anywhere in the codebase and are not proposed in P22.1–P22.8. Furthermore, polling `STDIN_FILENO`
without handling `POLLHUP` or disabling the descriptor upon EOF causes `poll(2)` to return
immediately with zero wait time when stdin is `/dev/null` or closed (the standard headless CI test
environment), converting the reactor into an uncontrolled 100% CPU spin loop. In addition, clamping
quiescent timeouts to 250ms prevents true 0% CPU idle.

**Evidence:**

From `design/terminal-ui-architecture.md:237-239`:

```cpp
const int notifyFd = executor.notificationDescriptor(); // eventfd or pipe
fds[1].fd     = notifyFd;
fds[1].events = (notifyFd >= 0) ? POLLIN : 0;
```

From `design/terminal-ui-architecture.md:264-268`:

```cpp
int ret = ::poll(fds, (notifyFd >= 0) ? 2 : 1, timeoutMs);
if (ret > 0 && (fds[1].revents & POLLIN)) {
  executor.drainNotifications();
  tui.markDirty();
}
```

In `apps/common/xanadu/vagent/runtime/agent_executor.hpp:1-748`, `AgentExecutor` contains no member
named `notificationDescriptor` or `drainNotifications`. `git grep notificationDescriptor` and
`git grep drainNotifications` return 0 hits across the entire repository.

In `design/terminal-ui-architecture.md:250`, `int timeoutMs = 250; // Quiescent maximum heartbeat`
unconditionally forces a 4 Hz wakeup even when no actors or deadlines are active. When executed
headless in automated testing or container shells with closed stdin, `poll(2)` reports `POLLHUP` /
`POLLIN` with 0 bytes available, looping indefinitely without blocking.

**Fix:**

1. In `apps/common/xanadu/vagent/runtime/agent_executor.hpp`, formally introduce a proposal
   (`P22.3a` or augment `P22.3`) adding an event file descriptor (`eventfd(2)` on Linux, self-pipe
   fallback on POSIX) to `AgentExecutor` or `MeshActorRuntime`, with explicit `notificationFd()` and
   `drainNotifications()` methods.
1. Handle `POLLHUP`, `POLLERR`, and EOF in `runTuiLoop`. If `read()` on `STDIN_FILENO` returns 0 or
   error, set `fds[0].fd = -1` to deactivate stdin polling so headless execution remains quiescent.
1. When `executor.earliestDeadline()` is `std::nullopt`, pass `timeoutMs = -1` to `::poll(2)` so the
   process achieves true 0% CPU quiescence until an fd readiness event arrives.

### 2. (§3.1, line 154 & §5.1, line 231) Terminal corruption on unhandled signals

**Problem:** The TUI configures curses into raw/cbreak mode, disables local echo (`noecho()`), hides
the hardware cursor (`curs_set(0)`), and configures escape delays. However, `runTuiLoop` registers
no signal handling for `SIGINT` (`Ctrl+C`), `SIGTERM`, `SIGHUP`, or `SIGWINCH`. When an unhandled
`SIGINT` or `SIGTERM` terminates the process, `TerminalDevice::~TerminalDevice()` and `endwin()` are
bypassed by the OS default signal disposition, leaving the user's host terminal in a corrupted state
(invisible cursor, echo suppressed, terminal un-reset). Additionally, `SIGWINCH` delivery during
`poll(2)` causes an unhandled `EINTR` error return that is ignored by `runTuiLoop`.

**Evidence:**

From `design/terminal-ui-architecture.md:158-164`:

```cpp
initscr();
cbreak();
noecho();
keypad(stdscr, TRUE);
```

In `design/terminal-ui-architecture.md:231-273`, `runTuiLoop` contains no signal masking, no
`signalfd(2)`, no self-pipe, and no check for `ret < 0 && errno == EINTR`. If a user sends `SIGINT`
(`Ctrl+C`) or closes the terminal session, the process abruptly exits via default signal
termination, leaving the TTY in raw non-echo mode with cursor hidden.

**Fix:**

1. Integrate `signalfd(2)` on Linux (or a POSIX self-pipe) into the `poll(2)` reactor loop as a
   third monitored descriptor (`fds[2]`), masking `SIGINT`, `SIGTERM`, `SIGHUP`, and `SIGWINCH`.
1. When `fds[2]` triggers with `SIGINT` or `SIGTERM`, invoke `tui.requestExit()`, exiting the loop
   normally so RAII destructors call `endwin()` and restore terminal attributes.
1. When `fds[2]` triggers with `SIGWINCH`, invoke curses `resizeterm(0, 0)`, recalculate layout, and
   mark the screen buffer dirty without racing against asynchronous signal handlers. Handle
   `ret == -1 && errno == EINTR` cleanly as a loop continuation.

### 3. (§5, line 158 & §8, line 224) Unbounded memory growth in action log stream

**Problem:** Outbound emissions streamed from `executor.setHostSink` are appended directly to
`actionLog_` without any retention bound, capacity limit, or eviction policy. In an active cluster
generating high-throughput events or running over hours, `actionLog_` grows without bound, creating
a fatal memory leak that inevitably triggers an out-of-memory crash. While command and query
histories have a configured setting (`vagent.tui.history_size: 100`), `actionLog_` has neither a
setting nor a ring buffer bound.

**Evidence:**

From `design/vagent-tui-dashboard.md:164-175`:

```cpp
executor.setHostSink([&tui](const identity::ActorPid &pid,
                            std::string_view eventType,
                            const runtime::WireRecord &payload) {
  ...
  tui.appendActionLog(std::move(formatted));
  tui.markDirty();
});
```

In `apps/common/xanadu/vagent/tui/agent_tui.hpp:175`, `actionLog_` is an unbounded
`std::vector<std::string>`. In `apps/common/xanadu/vagent/tui/agent_tui.cpp:816`,
`actionLog_.push_back(...)` unconditionally appends entries.

In `design/vagent-tui-dashboard.md:228-233`, Section 8 defines `vagent.tui.history_size` for
command/query history, but completely omits any limit or retention policy for `actionLog_`.

**Fix:**

1. Convert `actionLog_` in `AgentTui` to a bounded ring buffer or `std::deque<std::string>` capped
   at a fixed capacity.
1. Introduce setting `vagent.tui.action_log_capacity` (default: 1000 lines) in `system://settings`
   and `AgentSettings`.
1. When `actionLog_.size() >= capacity`, evict the oldest entry in $O(1)$ before pushing new logs.

______________________________________________________________________

## MAJOR

### 4. (§3, line 96 & §5.1, line 231) Generic terminal stack coupled to vagent

**Problem:** `terminal-ui-architecture.md` purports to provide the general, reusable terminal UI
foundation for all terminal utilities across the repository (`vquery`, `vpl`, `vprolog`,
`vagent tui`). However, `runTuiLoop` in §5.1 directly couples to `AgentTui` and `AgentExecutor`.
Furthermore, the existing implementation (`NCursesActor`) resides under
`apps/common/xanadu/vagent/tui/ncurses_actor.hpp`, includes
`"common/xanadu/identity/identity_layout.hpp"`, and embeds `identity::ActorPid pid_{}` into the
device driver. Neither document specifies moving generic terminal infrastructure (`TerminalDevice`,
`VirtualScreenBuffer`, `Reactor`, `Viewport`, `KeyEvent`, `Style`) into a generic UI directory
(`apps/common/ui/tui/`). Consequently, non-vagent utilities cannot use the terminal stack without
introducing backward dependencies on `vagent` and `identity`.

**Evidence:**

In `design/terminal-ui-architecture.md:40-44`, the text cites `vquery`, `vpl`, `vprolog` as primary
consumers. Yet in `design/terminal-ui-architecture.md:231-232`, the reactor signature is:

```cpp
int runTuiLoop(TerminalDevice &device, AgentTui &tui,
               AgentExecutor &executor)
```

In `apps/common/xanadu/vagent/tui/ncurses_actor.hpp:16,134,146,177`, `NCursesActor` embeds
`identity::ActorPid`.

`AGENTS.md:195-201` states: "Everything xanalogical goes in apps/common/: apps/common/xanadu/ if it
needs no graphics device, apps/common/ui/ if it draws or takes input."

**Fix:**

1. Relocate generic terminal infrastructure to `apps/common/ui/tui/` under namespace
   `xanadu::ui::tui`:
   - `terminal_device.hpp` / `.cpp`
   - `virtual_screen_buffer.hpp` / `.cpp`
   - `reactor.hpp` / `.cpp`
   - `viewport.hpp`
   - `key_event.hpp`, `style.hpp`
1. Remove `ActorPid` and all domain-specific headers from `TerminalDevice`.
1. Decouple `Reactor` / `runTuiLoop` from `AgentTui` by defining an abstract event consumer or
   callback concept (`TuiApplication` with `handleInput()`, `render()`, `shouldExit()`,
   `isDirty()`), allowing `vquery`, `vpl`, `vprolog`, and `AgentTui` to reuse the identical reactor.

### 5. (§1.1, line 36) Missing Vortex justification and keymap centralization

**Problem:** Both documents completely fail to justify why the terminal UI engine and dashboard are
written in C++ rather than Vortex, violating repository standing rules. Furthermore, all keybindings
in `terminal-ui-architecture.md` §7 and `AgentTui::handleInput()` are hardcoded in C++ switch
branches rather than being integrated into the sovereign `system://keymap` system store using Vortex
function calls.

**Evidence:**

`GEMINI.md:4-5` establishes: "New C++ code must justify why it isn't being written in Vortex."

`GEMINI.md:3` specifies: "All key bindings across the application (`xuzz`, and compatibility
symlinks `xudu`/`zigzag`) are merged into the `system://keymap` system store, using Vortex function
calls for their actions."

`brief-common.md:51-52` reiterates Rule 5 ("Vortex Over C++: Justify why any proposed C++ cannot be
expressed in Vortex bytecode or stdlib").

The word "Vortex" appears 0 times in the substantive text of both documents. In
`apps/common/xanadu/vagent/tui/agent_tui.cpp:718-858`, hotkeys (`F1`–`F3`, `s`, `q`, `r`, `Tab`) are
hardcoded in C++.

**Fix:**

1. Add an explicit "C++ vs. Vortex Division of Labor" section to `terminal-ui-architecture.md`
   justifying C++ for:
   - Low-level POSIX terminal device control (`termios`, `ioctl`, `poll(2)`, `signalfd(2)`).
   - High-throughput differential Unicode cell diffing and memory-contiguous screen blitting.
   - Non-intrusive read-only introspection of `ArenaManifold` and in-process `vprolog::Compiler`
     execution without charging actor turns or mutating agent state.
1. Connect `AgentTui` input dispatch to `system://keymap`, binding key events to Vortex action
   symbols (e.g. `vagent:step`, `vagent:quit`, `vagent:focus_pane`) with fallback defaults for
   headless / unconfigured boots.

### 6. (§4, line 213) Jagged heap allocation in VirtualScreenBuffer grid\_

**Problem:** `VirtualScreenBuffer` stores its screen grid as
`std::vector<std::vector<ScreenCell>> grid_`. In C++, a vector-of-vectors performs $H$ separate heap
allocations (30 to 60+ individual allocations), forces double pointer indirection on every cell
lookup, causes cache line fragmentation during linear screen diffing passes, and inhibits contiguous
memory optimizations like `std::span` row views.

**Evidence:**

In `design/terminal-ui-architecture.md:213`:

```cpp
std::vector<std::vector<ScreenCell>> grid_;
```

In `apps/common/xanadu/vagent/tui/ncurses_actor.hpp:119`:

```cpp
std::vector<std::vector<ScreenCell>> grid_;
```

Diffing two 120x30 frames requires chasing 30 separate heap pointers across non-contiguous memory,
defeating cache prefetching.

**Fix:**

Replace the jagged vector with a flat contiguous buffer:

```cpp
int width_{120};
int height_{30};
std::vector<ScreenCell> cells_; // sized to width_ * height_
```

Index cells via `cells_[y * width_ + x]`. Provide `std::span<const ScreenCell> row(int y) const` for
efficient row-at-a-time SIMD/memcmp comparisons and contiguous differential diffing.

### 7. (§6, line 187) werase() violates differential rendering invariant T3

**Problem:** Invariant T3 explicitly dictates: "Only modified regions of the terminal are emitted to
the wire. Whole-window clears (wclear) are strictly forbidden during the render loop." However,
Phase 1 task P1_2 in `vagent-tui-dashboard.md` states: "Implement werase + wnoutrefresh + doupdate
diff rendering". In curses, `werase(win)` writes blanks to every character cell of the window buffer
before redrawing. Calling `werase()` every tick wipes curses' internal knowledge of the current
screen contents, forcing curses to redraw all non-blank cells and emitting unnecessary terminal
escape sequences over SSH.

**Evidence:**

In `design/terminal-ui-architecture.md:80-82`, Invariant T3 mandates: "Whole-window clears (wclear)
are strictly forbidden during the render loop."

In `design/vagent-tui-dashboard.md:187`, Phase 1 task P1_2 states: "1.2 Implement werase +
wnoutrefresh + doupdate diff rendering".

In ncurses, `werase(stdscr)` fills `stdscr` with space characters and sets all lines dirty,
defeating differential change tracking against `curscr`.

**Fix:**

Revise Phase 1 task P1_2: Remove `werase()`. Paint new cell glyphs and attributes directly over
existing window cells via `mvwadd_wch()` / `mvwaddch()`, clear only trailing characters on shortened
lines via `wclrtoeol()`, and rely on `wnoutrefresh()` + `doupdate()` to emit minimal diffs to the
wire.

### 8. (§10, line 416 & §7, line 218) Duplicate proposal numbering P22.4/P22.6/7

**Problem:** Proposal numbering between the two companion documents is fragmented and contradictory:

- `terminal-ui-architecture.md` §10 defines `P22.4`: "Standardize `Viewport` scrolling offsets and
  navigation history across `AgentTui`."
- `vagent-tui-dashboard.md` §7 defines `P22.6`: "Implement `Viewport` scrolling offsets in
  `AgentTui` with `PageUp`/`PageDown`/`Home`/`End`" and `P22.7`: "Add command and query history
  buffers to `AgentTui` with `Up`/`Down` navigation." This duplicates scope: P22.4 in the
  foundational document directly references `AgentTui` (a domain class) and collides with both P22.6
  and P22.7.

**Evidence:**

In `design/terminal-ui-architecture.md:416-417`:
`- P22.4: Standardize Viewport scrolling offsets and navigation history across AgentTui.`

In `design/vagent-tui-dashboard.md:218-220`:
`- P22.6: Implement Viewport scrolling offsets in AgentTui with PageUp/PageDown/Home/End.`
`- P22.7: Add command and query history buffers to AgentTui with Up/Down navigation.`

**Fix:**

Unify and clarify the proposal register:

- **`P22.4`** (in `terminal-ui-architecture.md`): Define the generic `Viewport` virtualization
  utility in `apps/common/ui/tui/viewport.hpp` with clamp, pageUp, pageDown, and visible indicator
  calculations.
- **`P22.6`** (in `vagent-tui-dashboard.md`): Adopt `Viewport` across `AgentTui` actor tables,
  belief inspections, and action logs.
- **`P22.7`** (in `vagent-tui-dashboard.md`): Add bounded circular history buffers for commands and
  queries in `AgentTui`.

### 9. (§8, line 224) runTui does not load system://settings

**Problem:** `vagent-tui-dashboard.md` §8 claims settings integrate into `system://settings`.
However, in `apps/vagent/main.cpp:12-14`, the codebase explicitly documents that `vagent` does not
open the user's system store, using built-in defaults instead. In `runTui`
(`apps/vagent/main.cpp:1197-1267`), no `SystemStoreModel` is instantiated, and no settings are
loaded. Neither document contains a proposal to bridge `runTui` to `system://settings` or parse CLI
flag overrides for these settings, leaving the configuration table in §8 as dead specification.

**Evidence:**

`design/vagent-tui-dashboard.md:226` states: "The dashboard configuration integrates into
`system://settings`".

`apps/vagent/main.cpp:13-14` notes: "this command does not open the user's system store, so a
changed setting does not reach `run` yet."

In `apps/vagent/main.cpp:1197-1267`, `runTui` instantiates `AgentTui` with default constructed
parameters without loading settings.

**Fix:**

Add a numbered proposal (e.g. `P22.9`) specifying how `runTui` loads `TuiSettings` from
`SystemStoreModel` when a store is available, and falls back to CLI flags (`--history-size`,
`--borders`) when running standalone or headless.

______________________________________________________________________

## MINOR

### 10. (§4, line 179) Wide character continuation cell underspecification

**Problem:** `ScreenCell` tracks `std::uint8_t width` (0, 1, or 2). When storing a wide character
(width 2, e.g. CJK or full-width symbols) at `(x, y)`, the cell at `(x + 1, y)` must be explicitly
designated as a continuation placeholder cell (`codepoint = 0`, `width = 0`). Without this rule,
string printing or dump rendering will overwrite or misalign trailing characters. Furthermore,
storing `char32_t codepoint` only accommodates single Unicode scalar values; composite grapheme
clusters with combining diacritics require Unicode Normalization Form C (NFC) on input.

**Evidence:**

In `design/terminal-ui-architecture.md:180-182`, `ScreenCell` defines `codepoint` and `width`. In
`design/terminal-ui-architecture.md:87-88`, Invariant T6 mentions "grapheme clusters (up to 4 bytes
or char32_t)".

**Fix:**

Clarify in §4 that `VirtualScreenBuffer::putChar` and `putString` automatically write a continuation
cell (`codepoint = 0`, `width = 0`) at `(x + 1, y)` for wide characters, and document that incoming
strings are normalized to Unicode NFC so composed graphemes fit into single `char32_t` scalar
values.

### 11. (§8, line 374) Palette table lacks monochrome and accessible fallbacks

**Problem:** The semantic color mapping table hardcodes ANSI colors (Cyan, Green, Yellow, Red,
White) without defining behavior for monochrome terminals (`TERM=dumb`, `COLORTERM` unset) or
accessibility modes (e.g. high-contrast).

**Evidence:**

In `design/terminal-ui-architecture.md:376-383`, colors are statically defined. In monochrome
environments, colors are unsupported, yet selection currently relies on color preservation.

**Fix:**

Specify that `TerminalDevice` queries `has_colors()`: when colors are unavailable, fall back to
attribute styling (reverse video for selection, underline for focus, bold for active status).

### 12. (§5.1, line 250 & §3.1, line 164) Magic timing numbers without settings

**Problem:** The reactor loop and curses initialization hardcode magic timing numbers:
`timeoutMs = 250` (quiescent heartbeat) and `set_escdelay(25)` (escape sequence recognition delay).
These numbers are not documented as settings or constants.

**Evidence:**

`design/terminal-ui-architecture.md:164` has `set_escdelay(25)`.
`design/terminal-ui-architecture.md:250` has `int timeoutMs = 250`.

**Fix:**

Define named constants `kDefaultEscapeDelayMs = 25` and `kQuiescentHeartbeatMs = 250`, and expose
them under `system://settings` keys `vagent.tui.escape_delay_ms` and `vagent.tui.heartbeat_ms`.

### 13. (§4.2, line 142) Eager unbounded Prolog query solution buffering

**Problem:** `AgentTui::queryVlog` calls `compiler.solve(compiledQuery)` which eagerly computes and
allocates all possible solutions into `std::vector<Solution>`. If an operator inputs a query with an
infinite or very large solution set, the REPL will hang or exhaust memory before paginating the
first 5 results.

**Evidence:**

`design/vagent-tui-dashboard.md:142-143` states: "Solutions display with pagination: Solutions 1–5
of 18".

`apps/common/xanadu/vagent/tui/agent_tui.cpp:294` executes:
`auto solutions = compiler.solve(compiledQuery);`, which eagerly computes the entire solution
vector.

**Fix:**

Specify that `queryVlog` uses an iterative solver cursor or passes a solution batch limit (e.g. 50
solutions max) to `vprolog::Compiler`, lazily fetching subsequent batches when the user requests
more.

______________________________________________________________________

VERDICT: NOT CONVERGED

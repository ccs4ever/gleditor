# Terminal UI Architecture: Virtual Screen Buffers and Reactive Event Loops

| field         | value                                                                                                                                                                  |
| :------------ | :--------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| version       | 0.5                                                                                                                                                                    |
| status        | Implemented (P22.1–P22.4, P22.9 landed; reactor loop and foundation in place); companion to [vagent TUI dashboard](vagent-tui-dashboard.md)                            |
| prerequisites | Modern C++23, zero-sentinel conventions (`std::optional`, `std::expected`), headless testing invariant                                                                 |
| companion to  | [vagent TUI dashboard](vagent-tui-dashboard.md), [vagent agent language](vagent-agent-language.md), [system settings](system-xanadocs-customization-and-metasystem.md) |
| rests on      | `VirtualScreenBuffer`, `TerminalDevice`, `KeyEvent`, `Style`, `MeshActorRuntime`, `AgentExecutor`                                                                      |
| first test    | a multi-pane TUI running under `VirtualScreenBuffer` handles `SIGWINCH` resize, differential redraws, and scrolling with zero X11/tty dependencies                     |

Paths below are under `apps/common/` unless stated otherwise.

______________________________________________________________________

## Contents

- [1. Motivation & Problem Statement](#1-motivation--problem-statement)
- [2. Architectural Invariants (T1–T8)](#2-architectural-invariants-t1t8)
- [3. Device Abstraction (`TerminalDevice`)](#3-device-abstraction-terminaldevice)
- [4. Virtual Screen Buffer (`VirtualScreenBuffer`)](#4-virtual-screen-buffer-virtualscreenbuffer)
- [5. Reactive Event Loop & Reactor Architecture](#5-reactive-event-loop--reactor-architecture)
- [6. Viewport Virtualization & Scrolling Protocol](#6-viewport-virtualization--scrolling-protocol)
- [7. Modality, Input Handling & Cursor Protocol](#7-modality-input-handling--cursor-protocol)
- [8. Color Semantics & Terminal Palette Safety](#8-color-semantics--terminal-palette-safety)
- [9. Headless Verification Contract](#9-headless-verification-contract)
- [10. Proposed Changes (P22.1–P22.4, P22.9, P22.10)](#10-proposed-changes-p221p224-p229-p2210)
- [11. Refused Alternatives](#11-refused-alternatives)
- [12. Open Questions](#12-open-questions)
- [Change History](#change-history)

______________________________________________________________________

## 1. Motivation & Problem Statement

### 1.1 Terminal User Interfaces in the Sovereign Hypermedia Stack

Across the repository, terminal applications (`vquery`, `vpl`, `vprolog`, and `vagent tui`) serve as
the primary diagnostic and operational interfaces for headless servers, developer workflows, and
automated testing. While `xuzz` provides the graphical GPU-rendered canvas for Xanadu hypertext and
ZigZag space, terminal environments demand equal architectural rigor:

1. **Monospace Physics as Grid Space**: Terminal cells form a discrete 2D integer coordinate space
   $(x, y)$ that mirrors ZigZag slice projections.
1. **Headless Determinism**: Invariant T1 dictates that 100% of terminal behaviors (box layout,
   scrolling, word wrap, color attributes, text entry, and command dispatch) must execute
   identically under headless test harnesses without a physical TTY, X11, or Wayland display.
   Current implementations bind directly to curses in production
   (`apps/common/xanadu/vagent/tui/ncurses_actor.hpp:20–48`) and execute naive 30ms sleep loops in
   `runTui` (`apps/vagent/main.cpp:240–270`), preventing clean headless automation.
1. **Decoupled Architecture**: Terminal drivers, screen buffers, and event multiplexing must be
   cleanly decoupled from domain-specific applications (`vagent`, `vquery`), enabling unified reuse
   across all CLI programs.

### 1.2 Justification for C++ vs. Vortex (Standing Rule 5)

Why is the low-level terminal layer implemented in C++ rather than Vortex bytecode?

- **Direct OS Descriptor Multiplexing**: Interfacing with POSIX terminal descriptors (`termios`,
  `cbreak`, `noecho`), `signalfd(2)`, and `poll(2)` requires low-level kernel system calls and
  precise descriptor lifecycle management that must execute outside the garbage-collected or
  arena-tracked bytecode memory model.
- **Curses ABI Binding**: `ncursesw` is a C library requiring direct pointer linkage and terminfo
  database queries.
- **Zero-Allocation Inner Loop**: Rendering 60 FPS differential terminal updates into a flat buffer
  demands cache-friendly contiguous memory layouts and zero runtime allocations.

Conversely, all high-level application state, Prolog queries, and keybindings are grounded in
Vortex: deductive queries execute via `vprolog::Compiler`, agent state resides in `ArenaManifold`,
and interactive keybindings dispatch through the `system://keymap` system store.

______________________________________________________________________

## 2. Architectural Invariants (T1–T8)

Any terminal UI implementation in this repository must satisfy eight hard invariants:

- **Invariant T1 (Headless Determinism)**: The full visual and behavioral surface of the UI can be
  driven, inspected, and verified in continuous integration using an in-memory `VirtualScreenBuffer`
  without opening a `/dev/tty` or linking a display server.
- **Invariant T2 (Zero-CPU Quiescence)**: When no keyboard input is pending, no network events
  arrive, and no actor deadlines expire, the terminal reactor loop blocks in kernel `poll(2)` with
  0% CPU utilization. Spin loops and polling sleeps (`std::this_thread::sleep_for`) are strictly
  prohibited.
- **Invariant T3 (Differential Damage Rendering)**: Redraw operations emit curses updates only for
  damaged cells (`wnoutrefresh` followed by a single `doupdate`). Calling `wclear()` or `werase()`
  on every frame is prohibited, as it destroys curses' differential cache and causes severe flicker
  over SSH connections.
- **Invariant T4 (Strict Modal Isolation)**: While any text input field is focused (in `EDIT MODE`),
  all single-character hotkeys (`'s'`, `'q'`, `'?'`, `'1'`, `'2'`, `'3'`) are treated strictly as
  text insertions. Global navigation hotkeys are active only in `NAVIGATION MODE`. Submitting with
  `Enter` retains `EDIT MODE` so sequential commands can be entered without accidental hotkey
  triggers.
- **Invariant T5 (Virtual Scrolling)**: Render paths must never iterate across unbounded data sets
  (e.g., full actor tables or complete belief graphs). Scroll offsets and viewport heights enforce
  fixed-bound rendering ($O(\\text{viewport})$ time and space).
- **Invariant T6 (Unicode Grapheme Awareness)**: Screen cells store Unicode codepoints (`char32_t`)
  with display width tracking. Zero-width combining characters attach to the preceding cell, and
  double-width characters reserve a trailing zero-width placeholder.
- **Invariant T7 (16-Color ANSI Safety)**: Color styling maps strictly to universal ANSI color pairs
  with transparent background fallbacks (`use_default_colors()`), ensuring high legibility across
  both dark and light terminal palettes, as well as dumb monochrome terminals.
- **Invariant T8 (Zero Sentinels)**: Terminal APIs use `std::optional` and `std::expected` for
  absent values and fallible operations. Magic integer sentinels (`-1`, `0xFFFF`) are forbidden.

______________________________________________________________________

## 3. Device Abstraction (`TerminalDevice`)

The low-level terminal interface is placed in `apps/common/ui/tui/terminal_device.{hpp,cpp}` under
namespace `xanadu::ui::tui`:

```cpp
namespace xanadu::ui::tui {

struct TerminalDimensions {
  int width{120};
  int height{30};
};

enum class BorderStyle {
  Ascii,          // +, -, |
  UnicodeSingle,  // ┌, ┬, ┐, ├, ┼, ┤, └, ┴, ┘, ─, │
  UnicodeRounded  // ╭, ┬, ╮, ├, ┼, ┤, ╰, ┴, ╯, ─, │
};

enum class KeyCode {
  None,
  Char,
  Enter,
  Backspace,
  Delete,
  Tab,
  Backtab,
  Escape,
  Up,
  Down,
  Left,
  Right,
  PageUp,
  PageDown,
  Home,
  End,
  F1,
  F2,
  F3,
  F10,
  F12,
  Resize
};

struct KeyEvent {
  KeyCode code{KeyCode::None};
  char32_t codepoint{0};
  bool ctrl{false};
  bool alt{false};
  bool shift{false};
};

class TerminalDevice {
public:
  struct Options {
    bool forceHeadless{false};
    TerminalDimensions defaultSize{120, 30};
    BorderStyle borders{BorderStyle::UnicodeRounded};
  };

  explicit TerminalDevice(Options options = {});
  virtual ~TerminalDevice();

  [[nodiscard]] bool isHeadless() const noexcept;
  [[nodiscard]] TerminalDimensions dimensions() const noexcept;

  void erase();
  void print(int x, int y, std::string_view text, Style style = {});
  void drawBox(int x, int y, int w, int h, std::string_view title = "",
               Style style = {});
  void drawHLine(int x, int y, int w, Style style = {});
  void drawVLine(int x, int y, int h, Style style = {});

  void setCursorVisible(bool visible);
  void setCursorPosition(int x, int y);

  void refresh();
  [[nodiscard]] std::optional<KeyEvent> pollInput();
  [[nodiscard]] bool hasPendingInput() const noexcept;
  [[nodiscard]] bool isEof() const noexcept;

  void injectInput(const KeyEvent &event);
  void injectInput(std::string_view text);

  [[nodiscard]] const VirtualScreenBuffer &screen() const noexcept;
  [[nodiscard]] VirtualScreenBuffer &screen() noexcept;

  void shutdown();
};

} // namespace xanadu::ui::tui
```

### 3.1 Curses Lifecycle & Clean Signal Restoration

When non-headless, initialization executes strictly in order:

1. `std::setlocale(LC_ALL, "")`: Enables wide-character and UTF-8 interpretation in `ncursesw`.
1. `initscr()`: Allocates `stdscr`.
1. `cbreak()`: Disables line buffering while preserving terminal signal generation (`Ctrl+C`).
1. `noecho()`: Disables automatic echoing of typed keys.
1. `keypad(stdscr, TRUE)`: Maps escape sequences to curses `KEY_*` constants.
1. `nodelay(stdscr, TRUE)`: Ensures non-blocking input retrieval during drain passes.
1. `if (std::getenv("ESCDELAY") == nullptr) set_escdelay(25)`: Clamps escape-key delay to 25ms
   unless an explicit user environment override is present, allowing distinct `Escape` detection
   without lagging Alt-key sequences.
1. `start_color()` + `use_default_colors()`: Maps color pairs to terminal transparent background
   (`-1`).

To ensure the terminal is never left in raw mode upon abnormal termination, `TerminalDevice`
registers a signal handler or self-pipe multiplexer for `SIGINT`, `SIGTERM`, and `SIGHUP` that
invokes `endwin()` and resets terminal attributes before exiting.

______________________________________________________________________

## 4. Virtual Screen Buffer (`VirtualScreenBuffer`)

To guarantee Invariants T1, T3, and T6, the in-memory screen buffer represents cells in a contiguous
flat vector of `ScreenCell` structs:

```cpp
namespace xanadu::ui::tui {

struct ScreenCell {
  char32_t codepoint{U' '};
  std::uint8_t width{1}; // 0 for combining/continuation, 1 for normal, 2 for wide
  Style style{};

  bool operator==(const ScreenCell &) const = default;
};

class VirtualScreenBuffer {
public:
  VirtualScreenBuffer(int width = 120, int height = 30);

  void resize(int width, int height);
  void clear();

  void put(int x, int y, char32_t cp, Style style = {});
  void putString(int x, int y, std::string_view text, Style style = {});

  [[nodiscard]] const ScreenCell &at(int x, int y) const;
  [[nodiscard]] std::span<const ScreenCell> row(int y) const;
  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int height() const noexcept { return height_; }

  [[nodiscard]] bool isDirty() const noexcept { return dirty_; }
  void clearDirty() noexcept { dirty_ = false; }
  void markDirty() noexcept { dirty_ = true; }

  [[nodiscard]] std::string line(int y) const;
  [[nodiscard]] bool contains(std::string_view text) const;
  [[nodiscard]] std::uint64_t frameCount() const noexcept { return frameCount_; }

private:
  int width_{120};
  int height_{30};
  std::vector<ScreenCell> grid_; // Flat contiguous vector: y * width_ + x
  bool dirty_{true};
  std::uint64_t frameCount_{0};
};

} // namespace xanadu::ui::tui
```

### 4.1 Flat Vector Performance & Grapheme Clusters

- **Contiguous Layout**: Storing cells in a single `std::vector<ScreenCell>` of size
  `width * height` eliminates per-row heap allocations, eliminates double-pointer indirection, and
  maximizes CPU L1/L2 cache line efficiency during screen blits.
- **Double-Width Characters**: Monospace block drawing elements (`█` U+2588, `░` U+2591) are
  standard single-width cells (`width == 1`). True double-width characters (`width == 2`, such as
  East Asian fullwidth glyphs and emoji) occupy two columns: the primary cell at $(x, y)$ records
  `width = 2`, and the adjacent cell at $(x + 1, y)$ is populated with a placeholder continuation
  cell (`codepoint = U' '`, `width = 0`). Overwriting either cell invalidates both.

______________________________________________________________________

## 5. Reactive Event Loop & Reactor Architecture

### 5.1 Multiplexed Reactor Loop (`poll(2)`)

Invariant T2 dictates zero CPU burn when quiescent. The event loop multiplexes descriptor sources
using kernel `poll(2)`, interacting directly with `AgentExecutor`
(`apps/common/xanadu/vagent/runtime/agent_executor.hpp:232–350`) and envelope dispatch
(`apps/common/xanadu/vagent/runtime/wire.hpp:102–117`):

```cpp
void runTuiLoop(TerminalDevice &device,
                AgentExecutor &executor,
                AgentTui &tui) {
  // Signal setup: multiplex SIGINT, SIGTERM, SIGWINCH via signalfd (Linux) or self-pipe
  const std::optional<int> sigFd = setupSignalDescriptor();
  const std::optional<int> notifyFd = executor.notificationDescriptor(); // eventfd from P22.9

  struct pollfd fds[3];
  bool stdinActive = !device.isHeadless() && isatty(STDIN_FILENO);

  while (!tui.shouldExit()) {
    // 1. Drain curses user-space stdio buffers and pending keys
    while (auto key = device.pollInput()) {
      tui.handleInput(*key);
    }
    if (device.isEof()) {
      stdinActive = false;
      tui.requestExit(); // Clean exit on stdin closure/EOF
      break;
    }

    // 2. Dispatch pending actor turns and timers
    auto now = std::chrono::system_clock::now();
    executor.tick(now);
    executor.pumpUntilIdle();

    // 3. Render damaged regions
    if (tui.isDirty()) {
      tui.render();
      device.refresh();
      tui.clearDirty();
    }

    // 4. Compute poll timeout from nearest actor timer deadline
    int timeoutMs = -1; // Block indefinitely if quiescent
    if (device.hasPendingInput()) {
      timeoutMs = 0; // Immediate wakeup for injected test inputs
    } else if (auto deadline = executor.earliestDeadline()) {
      if (*deadline <= now) {
        timeoutMs = 0; // Immediate pump
      } else {
        auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now);
        timeoutMs = static_cast<int>(std::min<int64_t>(diff.count(), 250));
      }
    }

    // 5. Configure descriptors
    int nfds = 0;
    if (stdinActive) {
      fds[nfds].fd = STDIN_FILENO;
      fds[nfds].events = POLLIN;
      nfds++;
    }
    if (notifyFd) {
      fds[nfds].fd = *notifyFd;
      fds[nfds].events = POLLIN;
      nfds++;
    }
    if (sigFd) {
      fds[nfds].fd = *sigFd;
      fds[nfds].events = POLLIN;
      nfds++;
    }

    // 6. Block until an event, signal, or deadline arrives
    int ret = ::poll(fds, nfds, timeoutMs);
    if (ret < 0) {
      if (errno == EINTR) continue;
      break;
    }

    // 7. Check for events and EOF on stdin
    for (int i = 0; i < nfds; ++i) {
      if (fds[i].fd == STDIN_FILENO && (fds[i].revents & (POLLHUP | POLLERR))) {
        stdinActive = false;
        tui.requestExit(); // Clean exit on stdin closure
      }
      if (notifyFd && fds[i].fd == *notifyFd && (fds[i].revents & POLLIN)) {
        executor.drainNotifications();
        tui.markDirty();
      }
      if (sigFd && fds[i].fd == *sigFd && (fds[i].revents & POLLIN)) {
        handleSignalEvent(*sigFd, device, tui);
      }
    }
  }
}
```

______________________________________________________________________

## 6. Viewport Virtualization & Scrolling Protocol

To enforce Invariant T5, all list and table presentations utilize a virtualized `Viewport` (modeled
after `apps/common/ui/slice/viewport.hpp:35–70`):

```cpp
namespace xanadu::ui::tui {

struct Viewport {
  int topIndex{0};
  int visibleRows{0};
  int totalRows{0};

  [[nodiscard]] int bottomIndex() const noexcept {
    return std::min(totalRows, topIndex + visibleRows);
  }

  void scrollTo(int index) noexcept {
    if (totalRows <= visibleRows) {
      topIndex = 0;
      return;
    }
    if (index < topIndex) {
      topIndex = std::max(0, index);
    } else if (index >= topIndex + visibleRows) {
      topIndex = std::min(totalRows - visibleRows, index - visibleRows + 1);
    }
  }

  void pageUp() noexcept {
    topIndex = std::max(0, topIndex - visibleRows);
  }

  void pageDown() noexcept {
    if (totalRows > visibleRows) {
      topIndex = std::min(totalRows - visibleRows, topIndex + visibleRows);
    }
  }
};

} // namespace xanadu::ui::tui
```

### 6.1 Incremental Redraw vs. Full Clear

`werase()` is called strictly upon terminal resize (`SIGWINCH`) or initial presentation. Incremental
per-frame updates position the cursor and overwrite characters directly in `stdscr`. This preserves
curses' internal line-hash and character-diff structures, minimizing escape sequences over SSH.

______________________________________________________________________

## 7. Modality, Input Handling & Cursor Protocol

### 7.1 Modal State Machine (Invariant T4)

To resolve the catastrophic `'s'` collision bug where typing commands triggered single-step turns,
input conforms to a strict state machine:

```
                  +-----------------------------------+
                  |          NAVIGATION MODE          |
                  |  Hotkeys: [1-3], [F1-F3], [Tab],  |
                  |  [s] step, [?] help, [Ctrl+C] quit|
                  +-----------------------------------+
                       |                         ^
        Focus Text Box |                         | Esc (on empty buffer)
        or Type Char   |                         | or Focus Navigation
                       v                         |
                  +-----------------------------------+
                  |             EDIT MODE             |
                  |  All keys insert text.            |
                  |  Enter executes and STAYS in edit.|
                  |  Up/Down: Command history.        |
                  +-----------------------------------+
```

- **Retention of Edit Mode**: Submitting a command with `Enter` executes the command and clears the
  input line, but **retains `EDIT MODE`**. This enables the operator to enter multiple sequential
  commands or queries without their next keystroke triggering global navigation actions.
- **Explicit Exit**: The operator exits `EDIT MODE` by pressing `Esc` on an empty line, pressing
  `Tab` to cycle focus away from the input widget, or pressing a dedicated navigation function key
  (`F1`–`F3`).
- **History Stash**: Navigating history with `Up` / `Down` stashes the uncommitted active input in
  `draft_`, restoring it when the user returns to the bottom of the history stack.
- **Visual Status Badge**: The global status line displays an explicit high-contrast mode indicator:
  `[NAV]` (Cyan) or `[EDIT]` (Yellow Bold).

______________________________________________________________________

## 8. Color Semantics & Terminal Palette Safety

All UI presentations map to semantic color tokens:

| Token                   | ANSI 16 Equivalent | Attribute | Usage in Presentation                                        |
| :---------------------- | :----------------- | :-------- | :----------------------------------------------------------- |
| **Accent / Focus**      | `Color::Cyan`      | Bold      | Active box border, active tab, selection pointer             |
| **Running / Active**    | `Color::Cyan`      | Normal    | `RUNNING` status badge, active telemetry                     |
| **Healthy / Success**   | `Color::Green`     | Normal    | `QUIESCENT`, `true.` deductive result, successful command    |
| **Attention / Pending** | `Color::Yellow`    | Normal    | `BUSY`, in-flight outbox, intermediate certainty (0.60–0.84) |
| **Critical / Failure**  | `Color::Red`       | Bold      | `STOPPED`, `false.` result, syntax error, failed delivery    |
| **Muted / Background**  | `Color::White`     | Dim       | Inactive box borders, ground facts, timestamps               |

Under selection, foreground colors are preserved: selection is indicated by a leading cursor pointer
(`❯`), bold text attribute, and reverse background styling. In monochrome terminals
(`has_colors() == false`), attributes degrade cleanly to bold and reverse video.

______________________________________________________________________

## 9. Headless Verification Contract

To preserve Invariant T1, all TUI implementations must verify their full functional surface in unit
tests using `TerminalDevice::Options{.forceHeadless = true}`.

Tests assert:

1. Frame counts and dirty flags via `screen().frameCount()` and `screen().isDirty()`.
1. Text inclusion and visual layout via `screen().contains(text)` and `screen().at(x, y)`.
1. Input processing via synthetic injection: `device.injectInput(event)` and `tui.tick()`.
1. Resizing behavior via synthetic `KeyEvent{KeyCode::Resize}`.

______________________________________________________________________

## 10. Proposed Changes (P22.1–P22.4, P22.9, P22.10)

The foundational terminal infrastructure changes are recorded in the Proposal Register:

### Proposal P22.1: `TerminalDevice` Decoupled Driver

- **Files Touched**: `apps/common/ui/tui/terminal_device.hpp`, `terminal_device.cpp`,
  `apps/common/xanadu/vagent/tui/ncurses_actor.hpp` (deprecated/aliased).
- **Rationale**: Decouples terminal initialization, terminfo queries, curses lifecycle, and
  `KEY_RESIZE` handling from agent-specific code into a reusable generic UI driver.
- **Resource Pricing**: 120 bytes struct overhead; 0 heap allocations during operation.
- **Fallback**: Keep ad-hoc curses initialization duplicated across `vagent` and sibling CLI tools.

### Proposal P22.2: Contiguous Flat `VirtualScreenBuffer`

- **Files Touched**: `apps/common/ui/tui/screen_buffer.hpp`, `screen_buffer.cpp`.
- **Rationale**: Replaces fragmented `std::vector<std::vector<ScreenCell>>` with a single flat
  contiguous vector of `ScreenCell` (`width * height`), optimizing cache locality and eliminating 31
  individual heap allocations per buffer.
- **Resource Pricing**: $120 \\times 30 \\times 20 \\text{ B} = 72 \\text{ KiB}$ contiguous heap
  buffer.
- **Fallback**: Retain vector-of-vectors with double-pointer indirection.

### Proposal P22.3: Multiplexed Reactive Event Loop

- **Files Touched**: `apps/vagent/main.cpp`.
- **Rationale**: Replaces `std::this_thread::sleep_for(30ms)` spin loop with kernel `poll(2)` over
  `STDIN_FILENO`, notification descriptors, and signal multiplexers.
- **Resource Pricing**: 0% CPU quiescence; wakeups occur in $\<100 \\ \\mu\\text{s}$.
- **Fallback**: Retain 30ms sleep loop with constant CPU overhead and screen strobing.

### Proposal P22.4: Viewport Virtualization & Scroll Protocol

- **Files Touched**: `apps/common/ui/tui/viewport.hpp`.
- **Rationale**: Standardizes bounded window calculation ($O(\\text{viewport})$) across all list
  widgets, preventing unbounded table iteration.
- **Resource Pricing**: 12 bytes per viewport instance; 0 heap allocations.
- **Fallback**: Clamp tables to hardcoded top-N items, hiding remaining elements.

### Proposal P22.9: `AgentExecutor` Descriptors, Drain, Deadlines & Clock Advance

- **Files Touched**: `apps/common/xanadu/vagent/runtime/agent_executor.hpp`, `agent_executor.cpp`.
- **Rationale**: Adds `[[nodiscard]] std::optional<int> notificationDescriptor() const;` (`eventfd`
  on Linux / pipe), `void drainNotifications();`,
  `[[nodiscard]] std::optional<std::chrono::system_clock::time_point> earliestDeadline() const;`,
  and
  `std::size_t advanceSimulatedTime(std::chrono::milliseconds delta, std::size_t maxTurns = 100);`.
  Enables kernel wakeups, precise deadline timeouts in the reactor loop, and lease upkeep across
  simulated time jumps.
- **Resource Pricing**: 1 OS file descriptor per executor; 8-byte eventfd write per wake.
- **Fallback**: Fall back to polling timeout clamped to nearest timer deadline.

### Proposal P22.10: `system://settings` TUI Configuration Binding

- **Files Touched**: `apps/vagent/main.cpp`, `apps/common/xanadu/system_docs.hpp`.
- **Rationale**: Loads `vagent.tui.*` configuration parameters dynamically from the sovereign
  `system://settings` store via `SystemStoreModel`.
- **Resource Pricing**: 0 allocations in steady state; 4 cell reads at startup.
- **Fallback**: Hardcode defaults in C++ structs.

______________________________________________________________________

## 11. Refused Alternatives

1. **Third-Party C++ TUI Frameworks (FTXUI, Notcurses, Termbox2)**: Refused to keep the repository
   zero-external-dependency, preserve strict C++23 standards, and ensure 100% headless testability
   integrated directly with `MeshActorRuntime`.
1. **ANSI Raw Terminal Driver (No Curses)**: Refused because ncursesw provides mature, battle-tested
   terminfo handling across thousands of legacy and modern terminal emulators.
1. **Hardcoded Split-Pane Fixed Sizes**: Refused because terminal dimensions vary widely from 80x24
   VT100s to 200x60 ultrawide monitors. Responsive column budgeting is required.

______________________________________________________________________

## 12. Open Questions

| ID       | Title                   | Question                                             | Default Resolution                                                                           | Status   |
| :------- | :---------------------- | :--------------------------------------------------- | :------------------------------------------------------------------------------------------- | :------- |
| **U-T1** | SGR 1006 Mouse Tracking | Should mouse support be enabled by default?          | Yes, enable `mousemask` when interactive; disable when headless or in dumb terminals.        | Resolved |
| **U-T2** | TrueColor (24-bit RGB)  | Should `TerminalDevice` support 24-bit direct color? | Defer; 16-color ANSI with bold/dim provides maximum universal portability across SSH/serial. | Resolved |

______________________________________________________________________

## Change History

- **v0.5 (2026-11-02)**: Implementation landed. The foundation lives in `apps/common/ui/tui/`
  (`tui_types.hpp`, `screen_buffer.{hpp,cpp}`, `viewport.hpp`, `terminal_device.{hpp,cpp}`) in
  namespace `xanadu::ui::tui`; `apps/common/xanadu/vagent/tui/ncurses_actor.hpp` is now a thin
  back-compat shim re-exporting the foundation types over `TerminalDevice`. P22.9's executor
  descriptors/deadlines/clock-advance were already present on `AgentExecutor`; the `poll(2)` reactor
  loop (P22.3) is implemented in `apps/vagent/main.cpp` `runTui`. Generic foundation tests are in
  `tests/xuzz/tui_foundation_test.cpp`.
- **v0.4 (2026-11-02)**: Addressed Round 3 Vortex Design Critique:
  - Formatted all GFM tables with standard multi-line layout and unescaped pipe delimiters.
  - Added evidential `file:line` citations throughout (§1.1, §5.1, §6).
  - Added `device.isEof()` check in `runTuiLoop` Step 1 to prevent reactor busy-spin on EOF (§5.1).
  - Updated `notificationDescriptor()` to return `std::optional<int>` (Invariant T8) in §5.1 and
    `P22.9`.
  - Guarded `set_escdelay(25)` with `if (std::getenv("ESCDELAY") == nullptr)` in §3.1.
  - Added `advanceSimulatedTime` and lease upkeep specification to `P22.9` (§10).
- **v0.3 (2026-11-02)**: Addressed Round 2 Vortex Design Critique:
  - Reconciled clock types to `std::chrono::system_clock::time_point` in reactor loop (§5.1).
  - Explicitly enumerated `notificationDescriptor()`, `drainNotifications()`, and
    `earliestDeadline()` in `P22.9`.
  - Added `hasPendingInput()` and `isEof()` to `TerminalDevice` (§3).
  - Corrected block character `█` width to single-width (`width == 1`) in §4.1.
  - Corrected `P22.2` buffer pricing to 72 KiB for 20-byte `ScreenCell`.
  - Removed phantom `ArenaManifold` dependency from header.
- **v0.2 (2026-11-02)**: Addressed Round 1 Vortex Design Critique:
  - Replaced vector-of-vectors with contiguous flat `VirtualScreenBuffer`.
  - Added explicit C++ vs Vortex architectural justification (§1.2).
  - Specified signal restoration and `isatty` EOF handling in reactor loop (§5.1).
  - Clarified modal isolation: `Enter` retains `EDIT MODE` (§7.1).
- **v0.1 (2026-11-02)**: Initial specification for Vortex Design Critique.

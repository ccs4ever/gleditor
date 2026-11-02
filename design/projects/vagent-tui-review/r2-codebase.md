# Round 2 critique: Codebase Expert (terminal-ui-architecture.md & vagent-tui-dashboard.md)

Paths below are relative to the repo root. Tally: 2 BLOCKING, 4 MAJOR, 3 MINOR.

## Status of previous BLOCKING/MAJOR findings

- **R1-B1 (Hallucinated `AgentExecutor` API Signatures)**: **PARTIALLY RESOLVED / STILL BLOCKING**.
  - *Resolved*: `AgentExecutor::notificationDescriptor` is now formally registered in Proposal
    `P22.9`. The `HostSink` callback signature in `vagent-tui-dashboard.md` §5 was updated to
    `std::function<void(const runtime::Envelope &)>`, matching `agent_executor.hpp:301`.
  - *Unresolved*: In `terminal-ui-architecture.md` §5.1 (line 316), `runTuiLoop` compares deadlines
    against `std::chrono::steady_clock::now()`, which collides with `AgentExecutor`'s system clock
    types and fails compilation. `earliestDeadline()` and `drainNotifications()` remain missing from
    `agent_executor.hpp` and are not enumerated in `P22.9`. In `vagent-tui-dashboard.md` §5 (lines
    188–191), the `HostSink` implementation accesses hallucinated members `envelope.payload`,
    `envelope.topic`, and non-existent function `runtime::formatWireValue`.
- **R1-B2 (2-Degree Rank Invariant Violation in Vlog Traversal)**: **RESOLVED**.
  - `vagent-tui-dashboard.md` §4.2 now correctly specifies head-to-first-argument traversal along
    `+d.grab`, followed by chaining subsequent argument cells along `+d.step` until `noCell`.
    Verified against `runtime_abi.hpp:23`, `apps/common/xanadu/vagent/runtime/compiler.cpp:1615`,
    and `apps/common/xanadu/zigzag/vlog.cpp:33–55`.
- **R1-B3 (Curses Stdio Starvation & Headless Hangs in `poll`)**: **PARTIALLY RESOLVED**.
  - *Resolved*: Added `isatty(STDIN_FILENO)` check to prevent polling stdin when headless, and sets
    `stdinActive = false` on `POLLHUP | POLLERR`. Drains `device.pollInput()` at loop head.
  - *Unresolved*: `device.hasPendingInput()` was omitted from `TerminalDevice`'s public interface in
    §3 and is not checked before `::poll()`. Injected inputs cause `poll(2)` to hang indefinitely
    when quiescent. On `POLLHUP`, the loop keeps running without requesting exit.
- **R1-M1 (Proposal Register Audit)**: **PARTIALLY RESOLVED**.
  - *Resolved*: Proposals consolidated into a unified register `P22.1`–`P22.10`.
  - *Unresolved*: `P22.8` was designated in R1 disposition as "Command history draft stashing & line
    editing", but was replaced in `vagent-tui-dashboard.md` §7 by "Proportional Certainty Bar Gauges
    & Palette Safety", leaving Command History unregistered. `P22.2` pricing calculation
    ($120 \times 30 \times 8 = 28.8 \text{ KiB}$) is physically impossible for an unpacked
    `ScreenCell` (20 bytes). `P22.1` omits file attributions for `ncurses_actor.cpp` and tests.
- **R1-M2 (2D Vector Allocation & Grapheme Truncation)**: **RESOLVED**.
  - `VirtualScreenBuffer` updated to 1D flat buffer `std::vector<ScreenCell> grid_` ($W \times H$).
    Wide character continuation cell semantics specified in §4.1.
- **R1-M3 (Input Draft Loss on History Navigation)**: **RESOLVED**.
  - Transient draft stashing in `draft_` buffer documented in `terminal-ui-architecture.md` §7.1 and
    `vagent-tui-dashboard.md` §4.3.
- **R1-M4 (Overwriting `LocalHost`'s Internal `hostSink_`)**: **UNRESOLVED**.
  - `vagent-tui-dashboard.md` §5 still specifies `executor.setHostSink([&tui](...))` directly
    without chaining to `LocalHost`'s internal sink (`program.cpp:350–351`), wiping out
    `host.emissions()`.

______________________________________________________________________

## BLOCKING

### 1. (terminal-ui-architecture §5.1) Clock Collision & Missing Deadline Query in `runTuiLoop`

**Problem:** `runTuiLoop` in `design/terminal-ui-architecture.md:316–324` cannot compile against
C++23 `<chrono>` or the live `AgentExecutor` interface:

1. In lines 316–321:

   ```cpp
   auto now = std::chrono::steady_clock::now();
   if (auto deadline = executor.earliestDeadline()) {
     if (*deadline <= now) { ... }
     else {
       auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now);
       ...
     }
   }
   ```

   In `apps/common/xanadu/vagent/runtime/agent_executor.hpp:386–399`, all time points and deadlines
   are evaluated against `std::chrono::system_clock::time_point` (or epoch milliseconds as
   `int64_t`). In C++23, `time_point<steady_clock>` and `time_point<system_clock>` are distinct,
   incompatible types; comparing (`*deadline <= now`) or subtracting (`*deadline - now`) causes a
   fatal compilation error (`invalid operands to binary expression`).

1. `AgentExecutor` has no method named `earliestDeadline()` (`agent_executor.hpp:1–748`). The
   existing deadline query is `nextDeadline(const identity::ActorPid &pid) const` (line 386). While
   R1 disposition agreed to add `earliestDeadline()` and `drainNotifications()`, Proposal `P22.9` in
   §10 (lines 530–537) strictly mentions providing an eventfd descriptor and completely omits both
   `earliestDeadline()` and `drainNotifications()` from its rationale and interface contract.

**Fix:**

1. Fix line 316 to sample `std::chrono::system_clock::now()`.
1. Expand Proposal `P22.9` to explicitly specify:
   - `[[nodiscard]] std::optional<std::chrono::system_clock::time_point> earliestDeadline() const;`
   - `void drainNotifications();` in `apps/common/xanadu/vagent/runtime/agent_executor.hpp`.

### 2. (vagent-tui-dashboard §5) Hallucinated `Envelope` Accessors and Formatter in `HostSink`

**Problem:** The proposed `HostSink` implementation in `design/vagent-tui-dashboard.md:185–195`
fails compilation because it accesses non-existent member variables on `runtime::Envelope` and calls
an undefined function:

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

1. In `apps/common/xanadu/vagent/runtime/wire.hpp:102–117`, `Envelope` has no member named
   `payload`. The field holding the event data is `envelope.fields` (of type `WireRecord`).
1. In `wire.hpp:102–117`, `Envelope` has no member named `topic`. The field holding the event type
   is `envelope.type` (e.g. `"PingAck"`), accompanied by `envelope.kind`.
1. `runtime::formatWireValue` does not exist anywhere in the repository (`git grep formatWireValue`
   yields 0 hits). In `apps/vagent/main.cpp:424–460`, the standard formatting routines are:
   - `std::string renderFields(const WireRecord &record);` (line 424)
   - `std::string renderEmission(const Envelope &event);` (line 458) which returns
     `event.type + ' ' + renderFields(event.fields)`.

**Fix:** Update `vagent-tui-dashboard.md` §5 to use live types and existing formatting functions:

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

______________________________________________________________________

## MAJOR

### 1. (vagent-tui-dashboard §5) Silent Clobbering of `LocalHost` Internal `hostSink_`

**Problem:** In `apps/vagent/main.cpp:1227`, `runTui` instantiates `LocalHost host(...)`. During its
construction (`apps/common/xanadu/vagent/runtime/program.cpp:350–351`), `LocalHost` registers its
own internal sink:

```cpp
executor_->install()
    ->setBindings(&bindings_)
    ->setHostSink(
        [this](const Envelope &event) { emissions_.push_back(event); });
```

If `apps/vagent/main.cpp` calls `executor.setHostSink([&tui](...))` directly as shown in §5, it
completely overwrites `LocalHost`'s internal sink. As a consequence, `host.emissions()` remains
permanently empty, silently breaking CLI telemetry, test verifications, and assertions.

**Fix:** Provide an additive emission observer pattern. Either:

1. Add `host.addEmissionObserver([&tui](const Envelope &envelope) { ... })` to `LocalHost`
   (`program.hpp:270`), matching `ClusterNode::setEmissionCallback` in `apps/vagent/main.cpp:1394`;
   or
1. In `runTui`, chain the existing sink by capturing `LocalHost`'s emission handler.

### 2. (vagent-tui-dashboard §7 & terminal §10) Proposal Register Divergence and Pricing Discrepancy

**Problem:** The Proposal Registers across both documents diverge from the agreed R1 disposition and
contain struct sizing inaccuracies:

1. **Missing Proposal for Command History Draft Stashing**: In `r1-disposition.md:94`, `P22.8` was
   adjudicated as `P22.8: Command history draft stashing & line editing`. In
   `design/vagent-tui-dashboard.md:267`, `P22.8` was redefined as
   `Proportional Certainty Bar Gauges & Palette Safety`. The command history draft stashing
   mechanism (described in §4.3 and Mermaid Phase 2.2) is now completely absent from the Proposal
   Register.

1. **Physically Impossible Struct Sizing in P22.2**: `terminal-ui-architecture.md:511` states:
   `- **Resource Pricing**: $120 \times 30 \times 8 = 28.8 \text{ KiB}$ contiguous heap buffer.` For
   this to equal 8 bytes per cell, `sizeof(ScreenCell)` must equal 8. However, in §4:

   ```cpp
   struct ScreenCell {
     char32_t codepoint{U' '}; // 4 bytes
     std::uint8_t width{1};    // 1 byte + 3 bytes padding
     Style style{};            // 12 bytes in live code (Color fg, bg + 4 bools)
   };
   ```

   In `apps/common/xanadu/vagent/tui/ncurses_actor.hpp:80–89`, `Style` is 12 bytes, making
   `sizeof(ScreenCell) == 20` bytes (aligned to 4). At $120 \times 30$, the buffer occupies
   $72 \text{ kB}$, not $28.8 \text{ KiB}$. To achieve 8 bytes, `ScreenCell` must be defined with
   packed bitfields (`uint32_t codepoint : 21`, `uint32_t width : 2`, packed attributes).

1. **Incomplete File Attributions in P22.1**: `P22.1` lists only headers, omitting
   `apps/common/xanadu/vagent/tui/ncurses_actor.cpp`, `apps/vagent/main.cpp` (which instantiates
   `NCursesActor`), and `tests/xuzz/vagent_tui_test.cpp`.

**Fix:**

1. Restore `P22.8` as Command History Draft Stashing & Line Editing; assign `P22.11` to Proportional
   Certainty Bar Gauges.
1. Correct the pricing in `P22.2` to reflect actual struct size ($72 \text{ kB}$), or specify a
   packed 8-byte layout for `ScreenCell`.
1. Add `ncurses_actor.cpp`, `main.cpp`, and `vagent_tui_test.cpp` to `P22.1`.

### 3. (terminal-ui-architecture §3 & §5.1) Reactor Hang on Synthetic/Injected Input

**Problem:** `runTuiLoop` does not check `device.hasPendingInput()` before entering `poll(2)`. In
§5.1:

```cpp
// 1. Drain curses user-space stdio buffers and pending keys
while (auto key = device.pollInput()) {
  tui.handleInput(*key);
}
// 2. Dispatch pending actor turns and timers
executor.pumpUntilIdle();
...
// 4. Compute poll timeout
int timeoutMs = -1;
...
// 6. Block until an event, signal, or deadline arrives
int ret = ::poll(fds, nfds, timeoutMs);
```

If an actor turn in `pumpUntilIdle()` or an external thread calls `device.injectInput(...)`, the
event is queued in `TerminalDevice::inputQueue_`. Because queued events exist in memory and generate
no descriptor readiness on `STDIN_FILENO`, and because `timeoutMs` defaults to `-1` when no actor
deadlines are pending, `poll(2)` blocks indefinitely in the kernel, ignoring the queued input.
Furthermore, `TerminalDevice`'s class declaration in §3 (lines 159–194) omitted `hasPendingInput()`,
even though `NCursesActor` implements it (`ncurses_actor.hpp:163`).

**Fix:**

1. Declare `[[nodiscard]] bool hasPendingInput() const noexcept;` in `TerminalDevice` (§3).
1. In `runTuiLoop` (§5.1), check `if (device.hasPendingInput()) timeoutMs = 0;` before calling
   `::poll(fds, nfds, timeoutMs)`.

### 4. (terminal-ui-architecture §5.1) Orphaned Hang on Stdin EOF/Hangup

**Problem:** In `terminal-ui-architecture.md:352–355`:

```cpp
if (fds[i].fd == STDIN_FILENO && (fds[i].revents & (POLLHUP | POLLERR))) {
  stdinActive = false; // Disable stdin polling to prevent busy-spin
}
```

When stdin receives `POLLHUP` (for example, when the controlling terminal emulator or SSH session is
severed), setting `stdinActive = false` removes `STDIN_FILENO` from subsequent `poll` calls. If no
background timers or notification descriptors are active, `nfds` drops to 0 or only signal fds, and
the process hangs indefinitely in `poll(2)` with `timeoutMs = -1`. An interactive TUI that loses its
input channel cannot be operated and should cleanly terminate.

**Fix:** On `POLLHUP | POLLERR` on `STDIN_FILENO`, request clean exit via `tui.requestExit()`.

______________________________________________________________________

## MINOR

### 1. (terminal-ui-architecture §5.1 & dashboard §5) Undefined Dirty Protocol on `AgentTui`

**Problem:** `runTuiLoop` calls `tui.isDirty()`, `tui.clearDirty()`, and `tui.markDirty()` (lines
308, 311, 358), and `HostSink` calls `tui.markDirty()` (`vagent-tui-dashboard.md:193`). In the live
codebase (`apps/common/xanadu/vagent/tui/agent_tui.hpp:115–160`), `AgentTui` has no dirty tracking
methods; only `VirtualScreenBuffer` maintains a `dirty_` flag.

**Fix:** Specify in Proposal `P22.5` that `AgentTui` forwards or maintains dirty state via:
`[[nodiscard]] bool isDirty() const noexcept;`, `void markDirty() noexcept;`, and
`void clearDirty() noexcept;`.

### 2. (terminal-ui-architecture §3 & §5.1) Negative Integer Sentinel for Notification Descriptor

**Problem:** `notificationDescriptor()` returns `int` with negative values indicating absence (line
333: `if (notifyFd >= 0)`). This violates Invariant T8 ("Zero Sentinels: Terminal APIs use
std::optional and std::expected for absent values").

**Fix:** Change the API signature to:
`[[nodiscard]] std::optional<int> notificationDescriptor() const;`.

### 3. (terminal-ui-architecture §3.1) Unchecked `ESCDELAY` Environment Variable

**Problem:** `set_escdelay(25)` is invoked unconditionally during non-headless curses initialization
(line 208). If a user or terminal configuration has set the `ESCDELAY` environment variable,
unconditionally calling `set_escdelay` overrides the user's explicit preference.

**Fix:** Guard with `if (std::getenv("ESCDELAY") == nullptr) { set_escdelay(25); }`.

______________________________________________________________________

VERDICT: NOT CONVERGED

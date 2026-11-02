# Round 3 critique: Codebase Expert (terminal-ui-architecture.md & vagent-tui-dashboard.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 2 MAJOR, 2 MINOR.

## Status of previous BLOCKING/MAJOR findings

- **R1-B1 / R2-B1 (Clock Collision & Missing Deadline Query in `runTuiLoop`)**: **RESOLVED**.
  `terminal-ui-architecture.md` §5.1 (lines 309–329) now samples `now` using
  `std::chrono::system_clock::now()`, passes it to `executor.tick(now)`, queries
  `executor.earliestDeadline()`, and evaluates deadlines against `system_clock::time_point`. All
  `<chrono>` types match `AgentExecutor` (`agent_executor.hpp:395`). Proposal `P22.9` in both
  documents explicitly enumerates `notificationDescriptor()`, `drainNotifications()`, and
  `earliestDeadline()`.
- **R1-B2 (2-Degree Rank Invariant Violation in Vlog Traversal)**: **RESOLVED**. Retained in
  `vagent-tui-dashboard.md` §4.2, stepping along `+d.grab` for head-to-first-argument and chaining
  subsequent argument cells along `+d.step` until `noCell`. Verified against `runtime_abi.hpp:23`
  and `compiler.cpp:1615`.
- **R1-B3 / R2-B3 (Curses Stdio Starvation & Headless Hangs in `poll`)**: **PARTIALLY RESOLVED /
  MAJOR**. `TerminalDevice::hasPendingInput()` was declared in §3 (line 185) and checked before
  `::poll()` in §5.1 (line 322) to force `timeoutMs = 0`, preventing hangs on injected inputs.
  `POLLHUP | POLLERR` on `STDIN_FILENO` requests exit via `tui.requestExit()` (line 362). However,
  while `TerminalDevice::isEof()` was declared in §3 (line 186), it is never queried anywhere in
  `runTuiLoop` (§5.1), which causes reactor busy-spins or hangs when stdin reaches EOF. (See MAJOR
  2).
- **R1-B1 / R2-B2 (Hallucinated `Envelope` Accessors and Formatter in `HostSink`)**: **RESOLVED**.
  The hallucinated members (`payload`, `topic`, `formatWireValue`) were replaced in
  `vagent-tui-dashboard.md` §5 (lines 217–220) with real `wire.hpp:102–117` members (`type`, `from`,
  `ordinal`, and `fields` formatted via `renderEmission`).
- **R1-M4 / R2-M1 (Silent Clobbering of `LocalHost` Internal `hostSink_`)**: **PARTIALLY RESOLVED /
  MAJOR**. `vagent-tui-dashboard.md` §5 now chains sinks
  (`[prevSink = executor.hostSink(), ...](...) { if (prevSink) prevSink(envelope); ... }`). However,
  `AgentExecutor` has no getter method `hostSink()` (`agent_executor.hpp:298–315` only provides
  `setHostSink`), so `executor.hostSink()` fails compilation. Furthermore, neither P22.5 nor P22.9
  declares adding `hostSink()` to `AgentExecutor`. In addition, `renderEmission` is qualified as
  `runtime::renderEmission`, whereas it is an internal helper at file scope in
  `apps/vagent/main.cpp:458`. (See MAJOR 1).
- **R1-M1 / R2-M2 (Proposal Register Audit & Pricing Discrepancy)**: **RESOLVED**. Proposal `P22.8`
  is restored as "Command/Query History Draft Stashing, In-Line Editing & Modal Help" in
  `vagent-tui-dashboard.md` §7 and roadmap §6. `P22.2` pricing is corrected to $120 \times
  30 \times 20 \text{ B} = 72 \text{ KiB}$ for 20-byte `ScreenCell` in `terminal-ui-architecture.md`
  §10. Proposals `P22.1`–`P22.10` are unified and consistent across both documents.
- **R2-M3 (Reactor Hang on Synthetic/Injected Input)**: **RESOLVED**. Covered by `hasPendingInput()`
  in §3 and §5.1.
- **R2-M4 (Orphaned Hang on Stdin EOF/Hangup)**: **RESOLVED**. Covered by `POLLHUP | POLLERR`
  handling in §5.1 line 362.
- **R2-m1 (Undefined Dirty Protocol on `AgentTui`)**: **RESOLVED**. Documented in §5 and `P22.5`
  touches `agent_tui.hpp`.
- **R2-m2 (Negative Integer Sentinel for Notification Descriptor)**: **UNRESOLVED / MINOR**.
  `notifyFd >= 0` sentinel still used instead of `std::optional<int>`. (See MINOR 1).
- **R2-m3 (Unchecked `ESCDELAY` Environment Variable)**: **UNRESOLVED / MINOR**. `set_escdelay(25)`
  still invoked unconditionally without checking `std::getenv("ESCDELAY")`. (See MINOR 2).

______________________________________________________________________

## BLOCKING

None.

______________________________________________________________________

## MAJOR

### 1. (vagent-tui-dashboard §5 & §7) Missing `AgentExecutor::hostSink()` Accessor & Sink Chaining

**Problem:** In `design/vagent-tui-dashboard.md` §5 (lines 211–223):

```cpp
executor.setHostSink([prevSink = executor.hostSink(), &tui](const runtime::Envelope &envelope) {
  if (prevSink) {
    prevSink(envelope);
  }
  auto now = std::chrono::system_clock::now();
  auto timestamp = std::format("{:%H:%M:%S}", now);
  std::string rendered = runtime::renderEmission(envelope);
  std::string formatted = std::format("[{} #{}:{} {}] {}",
                                      timestamp, envelope.from, envelope.ordinal,
                                      envelope.type, rendered);
  tui.appendActionLog(std::move(formatted));
  tui.markDirty();
});
```

This snippet fails compilation against the live codebase for two distinct reasons:

1. **Non-Existent Getter on `AgentExecutor`**: In `agent_executor.hpp:298–315`:

   ```cpp
   using HostSink = std::function<void(const Envelope &)>;
   AgentExecutor *setHostSink(HostSink sink) {
     hostSink_ = std::move(sink);
     return this;
   }
   ```

   `hostSink_` is stored as a private member variable (`agent_executor.hpp:734`). `AgentExecutor`
   exposes no getter method (`hostSink()`). Calling `prevSink = executor.hostSink()` results in a
   compilation error (`no member named 'hostSink' in 'xanadu::vagent::runtime::AgentExecutor'`).
   Furthermore, Proposal `P22.5` ("HostSink Bounded Action Log Streaming & Chaining") lists
   `Files Touched: agent_tui.hpp, agent_tui.cpp, apps/vagent/main.cpp`, omitting
   `agent_executor.hpp`. Proposal `P22.9` also omits declaring this accessor.

1. **Namespace Mismatch for `renderEmission`**: In `apps/vagent/main.cpp:458–460`, `renderEmission`
   is defined as a file-scope helper in the anonymous namespace `namespace {`:

   ```cpp
   std::string renderEmission(const Envelope &event) {
     return event.type + ' ' + renderFields(event.fields);
   }
   ```

   It is not declared in `wire.hpp` and does not reside in namespace `runtime`. Calling
   `runtime::renderEmission(envelope)` inside `apps/vagent/main.cpp` fails compilation with
   `no member named 'renderEmission' in namespace 'xanadu::vagent::runtime'`.

**Fix:**

1. Add `[[nodiscard]] const HostSink &hostSink() const noexcept { return hostSink_; }` to
   `AgentExecutor` (`apps/common/xanadu/vagent/runtime/agent_executor.hpp`), and add
   `agent_executor.hpp` to Proposal `P22.5` (or expand `P22.9`).
1. In `vagent-tui-dashboard.md` §5, invoke `renderEmission(envelope)` directly without the invalid
   `runtime::` prefix, or formally declare `renderEmission` in
   `apps/common/xanadu/vagent/runtime/wire.hpp`.

### 2. (terminal-ui-architecture §3 & §5.1) Omission of `device.isEof()` Check in `runTuiLoop`

**Problem:** In `design/terminal-ui-architecture.md` §3 (line 186), `TerminalDevice` was updated to
declare:

```cpp
[[nodiscard]] bool isEof() const noexcept;
```

This addition was documented in Change History v0.3 (line 587):
`- Added hasPendingInput() and isEof() to TerminalDevice (§3).`

However, inside `runTuiLoop` (§5.1, lines 292–374), `device.isEof()` is never queried:

```cpp
// 1. Drain curses user-space stdio buffers and pending keys
while (auto key = device.pollInput()) {
  tui.handleInput(*key);
}
```

When stdin encounters EOF (such as when input is redirected from `/dev/null` or a script, or when an
interactive terminal session closes without delivering an immediate `POLLHUP`), `device.pollInput()`
detects EOF on the underlying descriptor, sets internal `isEof_ = true`, and returns `std::nullopt`.

In Step 7 (line 360), `runTuiLoop` only tests for `fds[i].revents & (POLLHUP | POLLERR)`. Under
POSIX, an open descriptor at EOF remains permanently readable and generates `POLLIN` with 0 bytes
available. Because `runTuiLoop` never inspects `device.isEof()`, `stdinActive` remains `true`.

On the next loop iteration, `::poll(fds, nfds, timeoutMs)` wakes immediately with `POLLIN`,
`device.pollInput()` returns `std::nullopt`, and the loop spins continuously at 100% CPU, violating
Invariant T2 (Zero-CPU Quiescence) and hanging the process.

**Fix:** In Step 1 of `runTuiLoop` (§5.1), immediately query `device.isEof()` after draining input:

```cpp
while (auto key = device.pollInput()) {
  tui.handleInput(*key);
}
if (device.isEof()) {
  stdinActive = false;
  tui.requestExit();
  break;
}
```

______________________________________________________________________

## MINOR

### 1. (terminal-ui-architecture §5.1 & §10) Negative Sentinel for Notification Descriptor

**Problem:** In `terminal-ui-architecture.md:297` and `:340`, `notificationDescriptor()` returns
`int` and uses `-1` as a sentinel for descriptor absence (`if (notifyFd >= 0)`). This violates
Invariant T8 ("Zero Sentinels: Terminal APIs use std::optional and std::expected for absent
values").

**Fix:** Update `AgentExecutor::notificationDescriptor()` and Proposal `P22.9` to return
`[[nodiscard]] std::optional<int> notificationDescriptor() const;`, and update the loop to:
`if (auto notifyFd = executor.notificationDescriptor()) { fds[nfds].fd = *notifyFd; ... }`.

### 2. (terminal-ui-architecture §3.1) Unchecked `ESCDELAY` Environment Variable

**Problem:** `set_escdelay(25)` is invoked unconditionally during non-headless curses initialization
(line 210). If an operator or terminal environment explicitly defines the `ESCDELAY` environment
variable, unconditionally calling `set_escdelay` overrides their explicit configuration.

**Fix:** Guard with `if (std::getenv("ESCDELAY") == nullptr) { set_escdelay(25); }`.

______________________________________________________________________

VERDICT: NOT CONVERGED

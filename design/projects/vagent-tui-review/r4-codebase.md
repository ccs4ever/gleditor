# Round 4 critique: Codebase Expert (terminal-ui-architecture.md & vagent-tui-dashboard.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings

- **R1-B1 / R2-B1 (Clock Collision & Missing Deadline Query in `runTuiLoop`)**: **RESOLVED**.
  `terminal-ui-architecture.md` §5.1 samples `now` via `std::chrono::system_clock::now()`, passes it
  to `executor.tick(now)`, queries `executor.earliestDeadline()`, and bounds poll timeouts
  accordingly. All `<chrono>` types match `AgentExecutor` (`agent_executor.hpp:395`). Proposal
  `P22.9` across both specifications explicitly enumerates `notificationDescriptor()`,
  `drainNotifications()`, `earliestDeadline()`, and `advanceSimulatedTime()`.
- **R1-B2 (2-Degree Rank Invariant Violation in Vlog Traversal)**: **RESOLVED**.
  `vagent-tui-dashboard.md` §4.2 steps along `+d.grab` for head-to-first-argument and chains
  subsequent argument cells along `+d.step` until `noCell`, extracting scalar values via
  `asDouble()`, `asInt64()`, or `vprolog::formatTerm()` rather than calling `textOf()`. Verified
  against `runtime_abi.hpp:23` and `compiler.cpp:1615`.
- **R1-B3 / R2-B3 / R3-M2 (Curses Stdio Starvation, Headless Hangs & EOF Spin in `runTuiLoop`)**:
  **RESOLVED**. `TerminalDevice::hasPendingInput()` is checked in §5.1 before `::poll()` to force
  `timeoutMs = 0`, preventing hangs on injected inputs. `POLLHUP | POLLERR` on `STDIN_FILENO`
  requests clean exit. Furthermore, `device.isEof()` is now queried in Step 1 of `runTuiLoop`
  immediately after draining the input queue (`terminal-ui-architecture.md` §5.1 lines 311–315),
  breaking the reactor loop and preventing 100% CPU busy-spins on standard input EOF.
- **R1-B1 / R2-B2 (Hallucinated `Envelope` Accessors and Formatter in `HostSink`)**: **RESOLVED**.
  The previously hallucinated members (`payload`, `topic`, `formatWireValue`) remain replaced in
  `vagent-tui-dashboard.md` §5 with real `wire.hpp:102–117` members (`type`, `from`, `ordinal`, and
  `fields`), rendered via `renderEmission`.
- **R1-M4 / R2-M1 / R3-M1 (Missing `AgentExecutor::hostSink()` Accessor & `renderEmission` Scope)**:
  **RESOLVED**. In `vagent-tui-dashboard.md` §7, Proposal `P22.5` now explicitly touches
  `apps/common/xanadu/vagent/runtime/agent_executor.hpp` and declares the getter
  `[[nodiscard]] const HostSink &AgentExecutor::hostSink() const noexcept;`. In §5 (line 216),
  `renderEmission(envelope)` is invoked directly without the invalid `runtime::` namespace prefix,
  accurately matching the file-scope helper in `apps/vagent/main.cpp:458`.
- **R1-M1 / R2-M2 (Proposal Register Audit & Pricing Discrepancy)**: **RESOLVED**. Proposals `P22.1`
  through `P22.10` are complete, consistent, and cleanly partitioned between base terminal
  architecture (`terminal-ui-architecture.md` §10: `P22.1`–`P22.4`, `P22.9`, `P22.10`) and dashboard
  extensions (`vagent-tui-dashboard.md` §7: `P22.5`–`P22.10`). Shared proposals `P22.9` and `P22.10`
  match across touched files, method signatures, rationales, and fallback strategies. Buffer pricing
  is verified: $120 \times 30 \times 20 \text{ B} = 72 \text{ KiB}$ for `VirtualScreenBuffer`
  (`P22.2`), $500 \times 128 \text{ B} \approx 64 \text{ KiB}$ for `actionLog_` (`P22.5`), and 100
  entries ($\approx 8 \text{ KiB}$) for command history (`P22.8`).
- **R2-M3 (Reactor Hang on Synthetic/Injected Input)**: **RESOLVED**. Covered by `hasPendingInput()`
  in §3 and §5.1.
- **R2-M4 (Orphaned Hang on Stdin EOF/Hangup)**: **RESOLVED**. Covered by `POLLHUP | POLLERR`
  handling and `device.isEof()` check in §5.1.
- **R2-m1 (Undefined Dirty Protocol on `AgentTui`)**: **RESOLVED**. Formally declared in Proposal
  `P22.5` (`isDirty()`, `markDirty()`, `clearDirty()`).
- **R2-m2 / R3-m1 (Negative Integer Sentinel for Notification Descriptor)**: **RESOLVED**.
  `AgentExecutor::notificationDescriptor()` now returns `[[nodiscard]] std::optional<int>` in
  `terminal-ui-architecture.md` §5.1 line 301 and Proposal `P22.9` (both documents), strictly
  satisfying Invariant T8.
- **R2-m3 / R3-m2 (Unchecked `ESCDELAY` Environment Variable)**: **RESOLVED**.
  `terminal-ui-architecture.md` §3.1 line 211 now guards curses `set_escdelay(25)` with
  `if (std::getenv("ESCDELAY") == nullptr)`, preserving operator-defined escape delays.

______________________________________________________________________

## BLOCKING

None.

______________________________________________________________________

## MAJOR

None.

______________________________________________________________________

## MINOR

None.

______________________________________________________________________

VERDICT: CONVERGED

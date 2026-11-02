# Round 3 critique: Vortex Expert (terminal-ui-architecture.md & vagent-tui-dashboard.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 2 MAJOR, 1 MINOR.

## Status of previous BLOCKING/MAJOR findings

- **R2 B1 (`HostSink` implementation references non-existent fields and functions)**: **RESOLVED**.
  v0.3 §5 correctly accesses real `Envelope` members (`from`, `type`, `ordinal`, `fields`), formats
  via `runtime::renderEmission(envelope)`, and properly chains into `prevSink`. Proposal `P22.5`
  accurately specifies this signature and bounded ring-buffering.
- **R2 B2 (Reactor loop omits `executor.tick()`, freezing timers and deadlines)**: **RESOLVED**.
  `terminal-ui-architecture.md` §5.1 now calls `executor.tick(now)` before
  `executor.pumpUntilIdle()`. Deadlines and timestamps are unified under
  `std::chrono::system_clock::time_point`. Proposal `P22.9` now explicitly declares
  `earliestDeadline()`.
- **R2 B3 (Deductive REPL pagination contradicts scoped evaluation isolation)**: **RESOLVED**.
  `vagent-tui-dashboard.md` §4.2 and Proposal `P22.7` resolve the contradiction by specifying that
  up to `maxQuerySolutions = 50` solutions are pre-resolved into in-memory `solutionBuffer_` within
  an immediate mark/release scope (`Mark m = arena.mark(); ... arena.release(m);`). The mark is
  released immediately, leaving zero choice points or outstanding marks on the live arena.
  Subsequent pagination via `;` pages through `solutionBuffer_` locally.
- **R2 M4 (Interactive operator pauses trigger actor lease expiration)**: **PARTIALLY RESOLVED /
  SUBSUMED INTO MAJOR 1**. v0.3 §4.3 notes that during manual stepping, actor leases are refreshed
  to prevent `ActorError::NotAuthorized`. In `LocalHost`, `kLocalGrantLife` is 24 hours. However,
  advancing simulated time past grant expiry requires explicit lease upkeep in
  `advanceSimulatedTime`.
- **R2 M5 (`advance <durationMs>` lacks API backing on `AgentExecutor`)**: **UNRESOLVED / STILL
  MAJOR**. Although accepted in R2 disposition to map to `executor.advanceSimulatedTime()`,
  `advanceSimulatedTime(std::chrono::milliseconds delta)` was omitted from Proposal `P22.9` in both
  documents. `AgentExecutor` provides no clock manipulation method, leaving `advance <durationMs>`
  unimplementable. (See MAJOR 1).
- **R2 M6 (Unhandled scalar type extraction produces blank argument values)**: **UNRESOLVED / STILL
  MAJOR**. Although accepted in R2 disposition citing `vprolog::formatTerm`, §4.2 still omits term
  formatting and scalar extraction specification. In `ArenaManifold`, `textOf` returns `""` for
  scalar cells, so numeric arguments (`40.0`) evaluate to empty strings. (See MAJOR 2).
- **R2 M7 (Ambiguous single-stepping target semantics)**: **RESOLVED**. v0.3 §4.3 simplifies the
  grammar and text to `step [<count>]`, mapping directly to `executor.pumpUntilIdle(count)`.

______________________________________________________________________

## BLOCKING

None.

______________________________________________________________________

## MAJOR

### 1. (§4.3 line 190, §7 P22.9, TUI §10 P22.9) `advance <durationMs>` lacks API in P22.9

**Problem:** Section 4.3 defines the command:

> `advance <durationMs>`: Advances the simulated cluster clock by the specified milliseconds,
> dispatching expired timers.

In `apps/common/xanadu/vagent/tui/agent_tui.cpp:502–514`, `advanceClock(ms)` completely discards
`ms` because `AgentExecutor` provides no API for clock advancement. In the R2 disposition, this
finding was accepted with the ruling:

> "M2 (Advance clock execution): ACCEPTED. Mapped advance to executor.advanceSimulatedTime()."

However, in v0.3, Proposal `P22.9` in both `terminal-ui-architecture.md` §10 and
`vagent-tui-dashboard.md` §7 only declares `notificationDescriptor()`, `drainNotifications()`, and
`earliestDeadline()`. `advanceSimulatedTime(std::chrono::milliseconds delta)` was completely omitted
from Proposal `P22.9`. Without an explicit method on `AgentExecutor` (or clock controller),
`advance <durationMs>` cannot be implemented.

Furthermore, when simulated time advances, `AgentExecutor::tick(now)` and `upkeepLeases(now)` must
be called. If a simulated time jump exceeds the actor's lease duration (e.g. jumping 24+ hours in
one call), `upkeepLeases` terminates the actor with `orderlyStop(..., kCauseLease)` because
`now >= end`. Therefore, `advanceSimulatedTime` must advance time in bounded slices or refresh
leases during simulated time advancement so that actors are not terminated by artificial clock
jumps.

**Evidence:**

- `design/vagent-tui-dashboard.md:190`: `advance <durationMs>` command specification.
- `design/vagent-tui-dashboard.md:307–315`: Proposal `P22.9` omits `advanceSimulatedTime`.
- `design/terminal-ui-architecture.md:540–547`: Proposal `P22.9` omits `advanceSimulatedTime`.
- `apps/common/xanadu/vagent/tui/agent_tui.cpp:502–504`: `ms` is discarded.
- `apps/common/xanadu/vagent/runtime/agent_executor.cpp:3068–3076`: Unconditional stop on lease
  expiry.

**Fix:** Add
`std::size_t advanceSimulatedTime(std::chrono::milliseconds delta, std::size_t maxTurns = 100)` to
Proposal `P22.9` in both documents. Specify that when running with a manual/controllable
`AgentClock`, `advanceSimulatedTime` advances the virtual clock, upkeeps and renews leases, calls
`tick()`, and returns turns pumped via `pumpUntilIdle(maxTurns)`.

### 2. (§4.2 lines 141–145, §9 Probe 3) Unhandled scalar type extraction in compound arguments

**Problem:** Section 4.2 specifies reconstructing positional tuples such as `target(pos_y, 40.0)` by
stepping along `+d.step`. Verification Probe 3 similarly asserts that inserting compound fact
`target(pos_y, 40.0)` traverses both argument cells along `+d.step` and renders correctly.

However, the specification fails to describe how argument cell values are extracted or formatted. In
`apps/common/xanadu/vagent/tui/agent_tui.cpp:374`, argument values are extracted via
`arena.textOf(*first)`. In `apps/common/xanadu/zigzag/arena_manifold.cpp:1127–1132`,
`ArenaManifold::textOf` returns an empty string `""` for any cell whose `valueKindOf` is not
`ValueKind::Span`.

As a result, numeric literals (`40.0` as `ValueKind::Double`, integers as `ValueKind::Int64`) and
booleans (`ValueKind::Bool`) evaluate to `""`, rendering `target(pos_y, )` or `target(pos_y, _)`. In
the R2 disposition, this finding was accepted with the ruling:

> "M3 (Belief key formatting for numeric/boolean terms): ACCEPTED. Cites vprolog::formatTerm."

However, this citation and specification were omitted from §4.2 in v0.3.

**Evidence:**

- `design/vagent-tui-dashboard.md:141–145`: Traversal along `+d.step` specified, but value
  extraction omitted.
- `design/vagent-tui-dashboard.md:348–349`: Probe 3 tests `target(pos_y, 40.0)`.
- `apps/common/xanadu/zigzag/arena_manifold.cpp:1127–1132`: `textOf` returns `""` for scalar values.
- `apps/common/xanadu/vprolog/ast.hpp:184` & `parser.cpp:348`: `vprolog::formatTerm(const Term &)`.

**Fix:** Update §4.2 to specify that compound argument rendering inspects `arena.valueKindOf(cell)`
and extracts typed values via `arena.asDouble(cell)`, `arena.asInt64(cell)`, and
`arena.asBool(cell)`, or delegates term formatting to `vprolog::formatTerm`.

______________________________________________________________________

## MINOR

### 1. (§3.1 line 119, §4.1) Undefined telemetry cell notation (`104c/12e`)

**Problem:** Section 3.1 displays `Cells: 104c/12e` in the Selected Actor Telemetry card. Section
4.1 does not document what `c` and `e` represent.

**Fix:** Document in §4.1 that `c` denotes scratch cells (`ctx->arena.cellCount()`) and `e` denotes
epistemic cells managed under the epistemic ingestion budget (`executor.epistemicCells(pid)`).

______________________________________________________________________

VERDICT: NOT CONVERGED

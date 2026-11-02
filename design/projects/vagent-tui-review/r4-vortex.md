# Round 4 critique: Vortex Expert (terminal-ui-architecture.md & vagent-tui-dashboard.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 1 MINOR.

## Status of previous BLOCKING/MAJOR findings

- **R2 B1 (`HostSink` implementation references non-existent fields and functions)**: **RESOLVED**.
  v0.4 §5 correctly accesses real `Envelope` members (`from`, `type`, `ordinal`, `fields`), formats
  via `renderEmission(envelope)` (unprefixed), and chains into `prevSink` via `executor.hostSink()`.
  Proposal `P22.5` enumerates all dirty-tracking and host-sink signatures.
- **R2 B2 (Reactor loop omits `executor.tick()`, freezing timers and deadlines)**: **RESOLVED**.
  `terminal-ui-architecture.md` §5.1 calls `executor.tick(now)` and `executor.pumpUntilIdle()`.
  Deadlines and timestamps are unified under `std::chrono::system_clock::time_point`.
- **R2 B3 (Deductive REPL pagination contradicts scoped evaluation isolation)**: **RESOLVED**.
  `vagent-tui-dashboard.md` §4.2 and Proposal `P22.7` specify that up to `maxQuerySolutions = 50`
  solutions are pre-resolved into in-memory `solutionBuffer_` within an immediate mark/release scope
  (`Mark m = arena.mark(); ... arena.release(m);`). The mark is released immediately, leaving zero
  choice points or outstanding marks on the live arena. Subsequent pagination pages through
  `solutionBuffer_` locally without touching worker arenas.
- **R3 M1 / R2 M4–M5 (`advance <durationMs>` lacks API in P22.9 & lease upkeep)**: **RESOLVED**.
  Both `terminal-ui-architecture.md` §10 and `vagent-tui-dashboard.md` §7 formally declare
  `std::size_t advanceSimulatedTime(std::chrono::milliseconds delta, std::size_t maxTurns = 100);`
  in Proposal `P22.9`. The rationale specifies advancing the virtual clock, upkeeping and renewing
  leases, calling `tick()`, and pumping turns. Task 3.5 in `vagent-tui-dashboard.md` §6 tracks
  simulated clock advance and lease upkeep on the implementation roadmap.
- **R3 M2 / R2 M6 (Unhandled scalar type extraction in compound arguments)**: **RESOLVED**.
  `vagent-tui-dashboard.md` §4.2 specifies that compound argument rendering steps along `+d.step`
  until `pos(d) == std::nullopt`, extracting scalar arguments via `asDouble()`, `asInt64()`, or
  `vprolog::formatTerm()` rather than calling `textOf()`, reconstructing positional tuples like
  `target(pos_y, 40.0)`. Verification Probe 3 explicitly validates this path.
- **R2 M7 (Ambiguous single-stepping target semantics)**: **RESOLVED**. `vagent-tui-dashboard.md`
  §4.3 and EBNF specify `step [<count>]`, mapping to `executor.pumpUntilIdle(count)` with automatic
  lease refresh during manual inspection.

______________________________________________________________________

## BLOCKING

None.

______________________________________________________________________

## MAJOR

None.

______________________________________________________________________

## MINOR

### 1. (vagent-tui-dashboard.md §3.1 line 115, §4.1 line 133) Telemetry cell breakdown documentation

**Problem:** Section 3.1 displays `Cells: 104c/12e` in the Selected Actor Telemetry card mockup.
Section 4.1 describes the card as showing a "memory breakdown", but does not explicitly document
what `c` and `e` denote.

**Evidence:**

- `design/vagent-tui-dashboard.md:115`: `│ Fuel:   1.0G  Cells:   104c/12e    │`.
- `design/vagent-tui-dashboard.md:133`: Telemetry Card: Displays full PID, lifecycle statistics,
  memory breakdown, and next deadline.

**Fix:** Add an explicit parenthetical note in §4.1 clarifying that `c` denotes scratch cells
(`ctx->arena.cellCount()`) and `e` denotes epistemic cells tracked under the epistemic ingestion
budget (`executor.epistemicCells(pid)`).

______________________________________________________________________

VERDICT: CONVERGED

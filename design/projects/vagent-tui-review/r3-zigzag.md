# Round 3 critique: Xanadu / ZigZag Purist (terminal-ui-architecture.md & vagent-tui-dashboard.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 3 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R2.1 (HostSink accesses missing fields and breaks causality): RESOLVED — Chained `prevSink` and
  reconciled with real `Envelope` members (`from`, `ordinal`, `type`) and `renderEmission`.
- R2.2 (Phantom ArenaManifold prerequisite without projection): RESOLVED — Removed `ArenaManifold`
  from `rests on` in `terminal-ui-architecture.md:9–10`.
- R2.3 (Omission of multi-rank ZigZag traversal for P19 observations): RESOLVED — Documented
  `+d.epistemic`, `+d.source`, `+d.vars`/`+d.values`, and `+d.clone` to `$agent +d.trust` in §4.2.
- R2.4 (Double-width classification of █ corrupts gauges): RESOLVED — Corrected block drawing
  elements (`█`, `░`) to single-width (`width == 1`) in `terminal-ui-architecture.md` §4.1.
- R2.5 (Deferral contradicts Invariant T5 and quiescence): RESOLVED — Bounded viewport rendering and
  added `vagent.tui.inspect_depth` configuration setting.
- R2.6 (Deductive REPL severs pre-existing CellRef grounding): RESOLVED — Scoped evaluation frame
  with `solutionBuffer_` preserves turn atomicity, prevents mark leaks, and bounds fuel.

## BLOCKING

None.

## MAJOR

None.

## MINOR

### 1. (vagent-tui-dashboard.md §5, l. 218) Action log formatting omits causal floor

**Problem:** Section 5 includes `envelope.ordinal` in the action log format (`#{}:{}`), preserving
the emitter's monotonic sequence counter. However, `envelope.floor` is omitted from the format
string despite the R2 disposition stating that `ordinal` and `floor` were retained. When an agent
cluster operates across multiple nodes, `floor` represents the causal request horizon; omitting it
obscures causal dependencies between distributed actor turns.

**Evidence:**

- `design/vagent-tui-dashboard.md:218–220`:

  ```cpp
  std::string formatted = std::format("[{} #{}:{} {}] {}",
                                      timestamp, envelope.from, envelope.ordinal,
                                      envelope.type, rendered);
  ```

- `apps/common/xanadu/vagent/runtime/wire.hpp:109`:

  ```cpp
  /// The sender's floor as a full request id (§7.3).
  std::string floor;
  ```

**Fix:** Include `envelope.floor` when non-empty (e.g.,
`if (!envelope.floor.empty()) formatted += ...` or append `(floor: ...)`).

### 2. (vagent-tui-dashboard.md §5, l. 218) Duplicate event type in HostSink formatted log

**Problem:** Section 5 constructs `formatted` by embedding `envelope.type` in the bracket header
(`[{} #{}:{} {}]`) and appending `rendered`. In `apps/vagent/main.cpp:458`, `renderEmission` already
prefixes `event.type`:

```cpp
std::string renderEmission(const Envelope &event) {
  return event.type + ' ' + renderFields(event.fields);
}
```

This produces duplicate event type names in the log line (e.g.,
`[11:24:02 #echo_agent:1 PingAck] PingAck {latencyMs: 12}` instead of the mockup's
`[11:24:02 #echo_agent:1 PingAck] latencyMs=12` in §3.1 line 118).

**Evidence:**

- `design/vagent-tui-dashboard.md:118`: `[11:24:02 #echo_agent:1 PingAck] latencyMs=12`.
- `design/vagent-tui-dashboard.md:217–220`.
- `apps/vagent/main.cpp:458–460`.

**Fix:** Either omit `envelope.type` from the bracket header or pass `renderFields(envelope.fields)`
instead of `renderEmission(envelope)` to match §3.1 line 118.

### 3. (vagent-tui-dashboard.md §4.2, l. 160) Ground cell navigation from REPL solutions

**Problem:** Pre-resolving deductive query solutions into `std::vector<std::string> solutionBuffer_`
inside the immediate mark/release scope successfully preserves turn atomicity, prevents mark leaks,
and protects arena compaction. However, for variable bindings that ground to pre-existing cells
(created prior to `arena.mark()`), discarding the `CellRef` prevents direct navigation from the REPL
result to the corresponding node in the Beliefs inspector.

**Evidence:**

- `design/vagent-tui-dashboard.md:156–166`.

**Fix:** In a future extension, allow `solutionBuffer_` to retain
`std::optional<CellRef> groundCell` for bindings targeting pre-existing arena cells, enabling
`Enter` or `v` in Navigation Mode to focus that cell.

______________________________________________________________________

VERDICT: CONVERGED

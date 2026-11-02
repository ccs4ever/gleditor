# Round 2 critique: Vortex Expert (terminal-ui-architecture.md & vagent-tui-dashboard.md)

Paths below are relative to the repo root. Tally: 3 BLOCKING, 4 MAJOR, 2 MINOR.

## Status of previous BLOCKING/MAJOR findings

- **R1 B1 (Deductive Vlog REPL mutates live arena and destroys turn atomicity)**: **PARTIALLY
  RESOLVED / STILL BLOCKING**. v0.2 introduces a scoped evaluation frame (`P22.7`) with immediate
  mark/release (`Mark m = arena.mark(); ... arena.release(m);`) and a 10,000 instruction fuel limit.
  However, the pagination design (§4.2) is internally contradictory: it specifies pre-resolving only
  up to `pageSize = 5` solutions before calling `release(m)`, while displaying "Solutions 1–5 of 18"
  and claiming pressing `;` fetches subsequent pages. If only 5 solutions are resolved before the
  mark is released, the total count cannot be known, and pressing `;` would require re-running the
  query against a live arena that may have been mutated by background actor turns. (See BLOCKING 3).
- **R1 B2 (Reactive loop relies on hallucinated APIs and omits turn pumping)**: **PARTIALLY RESOLVED
  / STILL BLOCKING**. v0.2 added `executor.pumpUntilIdle()` to `runTuiLoop` and added Proposal
  `P22.9` for the notification descriptor. However, `runTuiLoop` completely omits `executor.tick()`,
  which is the sole engine entry point for firing due timers, expiring deadlines, and upkeeping
  deliveries. Furthermore, `executor.earliestDeadline()` is called in the loop body but was omitted
  from Proposal `P22.9` and does not exist on `AgentExecutor`. (See BLOCKING 2, MINOR 1).
- **R1 B3 (HostSink callback signature and wire API contradict C++)**: **UNRESOLVED / STILL
  BLOCKING**. While the callback signature was updated to `const runtime::Envelope &envelope`, the
  remediated code snippet in §5 lines 185–195 calls non-existent fields (`envelope.payload`,
  `envelope.topic`) and a non-existent function (`runtime::formatWireValue`), failing compilation
  against `wire.hpp`. (See BLOCKING 1).
- **R1 M4 (Interactive single-stepping triggers actor lease expiration)**: **UNRESOLVED**. v0.2
  contains zero mentions of actor leases or grant expiration. Pausing in the TUI will still kill
  actors via `ActorError::NotAuthorized` in `MeshActorRuntime::pumpOne`. (See MAJOR 1).
- **R1 M5 (advanceClock is a no-op for simulated time)**: **UNRESOLVED**. v0.2 adds
  `advance <durationMs>` to the CLI syntax, but `AgentExecutor` has no time advancement API, and no
  proposal in the Proposal Register covers virtual clock advancement. (See MAJOR 2).
- **R1 M6 (Compound argument traversal on dims().grab violates calling convention)**: **RESOLVED**.
  v0.2 §4.2 and Probe 3 correctly specify following `+d.grab` to the head argument and chaining
  subsequent positional arguments along `+d.step`.
- **R1 M7 (Raw CellRef leakage and unhandled scalar type formatting)**: **UNRESOLVED**. v0.2
  promises tuples like `target(x: 12.5, y: 40.0)` but omits the specification of scalar value
  extraction. In `arena_manifold.cpp`, `textOf` returns `""` for scalar cells. (See MAJOR 3).

______________________________________________________________________

## BLOCKING

### 1. (§5 lines 185–195) `HostSink` implementation references non-existent fields and functions

**Problem:** Section 5 specifies the host sink implementation for outbound emission streaming:

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

This code fails compilation against `apps/common/xanadu/vagent/runtime/wire.hpp` in three ways:

1. `envelope.payload`: `struct Envelope` (`wire.hpp:102–117`) has no `payload` member. The payload
   fields are stored in `envelope.fields` (which is a `WireRecord`).
1. `envelope.topic`: `struct Envelope` has no `topic` member. The event type identifier is carried
   in `envelope.type` (and any outer relay destination is in `envelope.relaySpace`).
1. `runtime::formatWireValue`: No function named `formatWireValue` exists in `wire.hpp`, `wire.cpp`,
   or anywhere in namespace `xanadu::vagent::runtime`. The canonical wire formatter in the tree is
   `renderFields(const WireRecord &record)` in `apps/vagent/main.cpp:424` (or `renderEmission` in
   `main.cpp:458`).

**Evidence:**

- `design/vagent-tui-dashboard.md:185–195`: Snippet above.

- `apps/common/xanadu/vagent/runtime/wire.hpp:102–117`:

  ```cpp
  struct Envelope {
    std::string kind;
    std::string type;
    std::string from;
    std::uint64_t ordinal{};
    std::string floor;
    WireRecord fields;
    std::optional<std::string> correlation;
    std::optional<std::string> relaySpace;
    std::vector<std::string> relayFor;
    bool operator==(const Envelope &) const = default;
  };
  ```

- `apps/vagent/main.cpp:424–460`: Canonical wire formatters `renderFields(const WireRecord &)` and
  `renderEmission(const Envelope &event)`.

**Fix:** Promote `renderFields` from `apps/vagent/main.cpp` into
`apps/common/xanadu/vagent/runtime/wire.hpp` as `runtime::renderFields(const WireRecord &)`. Update
§5 and Proposal `P22.5` to use actual members:

```cpp
executor.setHostSink([&tui](const runtime::Envelope &envelope) {
  auto now = std::chrono::system_clock::now();
  auto timestamp = std::format("{:%H:%M:%S}", now);
  std::string formattedFields = runtime::renderFields(envelope.fields);
  std::string formatted = std::format("[{} EMIT from {}] {} {}",
                                      timestamp, envelope.from,
                                      envelope.type, formattedFields);
  tui.appendActionLog(std::move(formatted));
  tui.markDirty();
});
```

### 2. (§5.1 lines 304–306) Reactor loop omits `executor.tick()`, freezing timers and deadlines

**Problem:** Section 5.1 specifies the reactor loop turn dispatch:

```cpp
    // 2. Dispatch pending actor turns and timers
    executor.pumpUntilIdle();
```

`AgentExecutor::pumpUntilIdle` (`apps/common/xanadu/vagent/runtime/agent_executor.cpp:3118–3143`)
only wakes uninitialized actors (`needsFirstTurn_`) and calls `runtime_.pumpOne(wallNow())` for
queued messages. It does **not** evaluate timers, does **not** fire due alarms, and does **not**
check deadlines.

In `AgentExecutor`, timer expiration (`fireDueTimers`), intention deadline expiration
(`expireDeadlines`), delivery upkeep and retries (`upkeepDeliveries`), and lease upkeep
(`upkeepLeases`) are exclusively executed inside `AgentExecutor::tick(wallNow())`
(`apps/common/xanadu/vagent/runtime/agent_executor.cpp:2979–3001`).

Because `runTuiLoop` never invokes `executor.tick()`, timers will **never** fire, overdue deliveries
will **never** retry, and waiting deadlines will **never** expire! Even when `poll(2)` wakes up
because a timer deadline elapsed in `timeoutMs`, calling `pumpUntilIdle()` will run 0 turns because
`tick()` was not called to promote the expired timer into a ready wake turn.

**Evidence:**

- `design/terminal-ui-architecture.md:304–306`:

  ```cpp
  // 2. Dispatch pending actor turns and timers
  executor.pumpUntilIdle();
  ```

- `apps/common/xanadu/vagent/runtime/agent_executor.cpp:2979–3001`:

  ```cpp
  void AgentExecutor::tick(std::chrono::system_clock::time_point now) {
    drainPosted();
    upkeepDeliveries(now);
    upkeepLeases(now);
    const auto nowMillis = ...;
    for (auto &[key, state] : states_) {
      if (!live(*state) || state->inTurn) continue;
      liftLimits(*state->context);
      (void)fireDueTimers(*state, nowMillis);
      (void)expireDeadlines(*state, nowMillis);
    }
    ...
  }
  ```

**Fix:** In `terminal-ui-architecture.md` §5.1, explicitly invoke `executor.tick()` before
`pumpUntilIdle()` in the reactor loop:

```cpp
    // 2. Upkeep deliveries, dispatch due timers, and pump actor turns
    executor.tick();
    executor.pumpUntilIdle();
```

### 3. (§4.2 lines 148–158) Deductive REPL pagination contradicts scoped evaluation isolation

**Problem:** Section 4.2 specifies:

> "Solutions are pre-resolved up to `pageSize = 5` into detached string representations before
> returning to the reactor loop. An explicit `QueryPaginationState` tracks `activeQuery`,
> `windowOffset`, and `pageSize`. Pressing `;` in Navigation Mode fetches the next page without
> leaving outstanding choice points on the worker arena."

This specification is architecturally contradictory:

1. If only 5 solutions are resolved before `arena.release(m)` is called, the system cannot report
   `Solutions 1–5 of 18` (as specified in §3.1 line 111 and R1 §4.2), because total solution count
   is unknown until evaluation exhausts the choice tree or hits the fuel cap.
1. When the user presses `;`, the previous query's choice point has already been destroyed by
   `release(m)`. To fetch the next page, the engine would have to re-parse, re-compile, and re-solve
   `activeQuery` from scratch.
1. Re-solving against the live actor arena between user turns violates turn atomicity and solution
   consistency: background actor execution or percept arrivals may have altered the belief graph,
   causing the second query to yield different or duplicated solutions. Furthermore,
   `vprolog::Compiler::solve` (`apps/common/xanadu/vprolog/compiler.cpp:233`) takes only
   `maxSolutions` and has no offset parameter.

**Evidence:**

- `design/vagent-tui-dashboard.md:148–158`: Scoped evaluation frame description.
- `design/vagent-tui-dashboard.md:111`: Mock displaying `Results: X = edge_node` and pagination
  indicator `Solutions 1–5 of 18 (Press ';' for more)`.
- `apps/common/xanadu/vprolog/compiler.cpp:233–238`: `Compiler::solve` has signature
  `solve(const CompiledQuery &query, std::size_t maxSolutions)`.

**Fix:** Specify that query execution resolves all available solutions up to a bounded cap (e.g.
`maxQuerySolutions = 100`, bounded by `maxQueryFuel = 10'000`) within the single `mark()` /
`release()` frame at query submission time. All resolved solutions are detached into formatted
strings inside `QueryPaginationState::solutions`. `arena.release(m)` is called immediately.
Subsequent pagination with `;` simply increments `windowOffset` and displays slices from the
detached in-memory buffer without touching or re-evaluating the actor's live arena.

______________________________________________________________________

## MAJOR

### 4. (§1.1 line 55, §4.1) Interactive operator pauses trigger actor lease expiration

**Problem:** Actors are bounded by `ActorGrant::expiresAt`
(`apps/common/xanadu/vortex/mesh_actor.hpp:58`). In production, leases expire after `leaseDuration`
(default 60s, `mesh_actor.hpp:81`).

In an interactive TUI session, human operators read tables, inspect beliefs, and formulate queries.
Real time elapses while the operator is idle in `poll(2)`. When the operator pauses for more than 60
seconds and then presses `'s'` or issues a command, `MeshActorRuntime::pumpOne` checks:

```cpp
if (wallNow >= actor.grant.expiresAt) {
  actor.stopped      = true;
  actor.leaseExpired = true;
  return std::unexpected(ActorError::NotAuthorized);
}
```

(`apps/common/xanadu/vortex/mesh_actor.cpp:597–601`). The actor is abruptly terminated simply
because the operator paused between keystrokes. Neither `terminal-ui-architecture.md` nor
`vagent-tui-dashboard.md` specifies how interactive sessions handle actor leases.

**Evidence:**

- `apps/common/xanadu/vortex/mesh_actor.hpp:81`: `std::chrono::milliseconds leaseDuration{60000};`.
- `apps/common/xanadu/vortex/mesh_actor.cpp:597–601`: Unconditional termination on lease expiry.
- Absence of lease management in `vagent-tui-dashboard.md`.

**Fix:** Specify in §4.1 that when launched in interactive TUI mode (`vagent tui`), `LocalHost`
provisions actors with extended interactive debug leases (e.g. `leaseDuration = 24h`), or
`AgentExecutor::tick()` automatically renews leases for active actors while under interactive
operator observation.

### 5. (§1.1 line 55, §4.3 line 166) `advance <durationMs>` lacks API backing on `AgentExecutor`

**Problem:** Section 4.3 adds the command:

> `advance <durationMs>`: Advances the simulated cluster clock by the specified milliseconds and
> triggers expired timer handlers.

In the implementation (`apps/common/xanadu/vagent/tui/agent_tui.cpp:502–514`), `advanceClock(ms)`
completely discards `ms`. `AgentExecutor`
(`apps/common/xanadu/vagent/runtime/agent_executor.hpp:232–472`) provides no API to advance
simulated time. While `LocalHost` has `advance(by, turns)` in `apps/vagent/main.cpp:1189`,
`AgentTui` is constructed with only `AgentExecutor &` and `MeshActorRuntime &`, neither of which
exposes clock manipulation.

Furthermore, no proposal in the Proposal Register (`P22.1`–`P22.10`) defines an API for simulated
clock advancement.

**Evidence:**

- `design/vagent-tui-dashboard.md:166`: `advance <durationMs>` command syntax.
- `apps/common/xanadu/vagent/tui/agent_tui.cpp:502–504`: `ms` is unused.
- `apps/common/xanadu/vagent/runtime/agent_executor.hpp`: No `advanceTime` method exists.

**Fix:** Add a proposal to the Proposal Register (e.g. extending `P22.9`) specifying
`AgentExecutor::advanceTime(std::chrono::milliseconds delta)` when running with a controllable
`AgentClock`. Update `AgentTui::advanceClock` to call `executor_.advanceTime(ms)` before calling
`executor_.tick()` and `executor_.pumpUntilIdle()`.

### 6. (§4.2 lines 138–142) Unhandled scalar type extraction produces blank argument values

**Problem:** Section 4.2 states:

> "reconstructing the complete tuple: `target(x: 12.5, y: 40.0)`."

In `apps/common/xanadu/vagent/tui/agent_tui.cpp:371–375`, argument cell values are extracted via
`arena.textOf(*first)`. In `apps/common/xanadu/zigzag/arena_manifold.cpp:1127–1147`,
`ArenaManifold::textOf` returns an empty string `""` for any cell whose `valueKindOf` is not
`ValueKind::Span`.

Numeric arguments such as `12.5` (`ValueKind::Double`) or `40` (`ValueKind::Int64`), as well as
booleans (`ValueKind::Bool`), will evaluate to `""`, rendering `target(x: , y: )` or `target(_)`.
The specification fails to describe how scalar values are extracted from argument cells.

**Evidence:**

- `apps/common/xanadu/zigzag/arena_manifold.cpp:1127–1132`:

  ```cpp
  std::string ArenaManifold::textOf(CellRef cell) const {
    if (valueKindOf(cell) != ValueKind::Span) return "";
    ...
  }
  ```

- `apps/common/xanadu/vagent/tui/agent_tui.cpp:371–375`: Uses `arena.textOf` unconditionally.

**Fix:** Specify in §4.2 that term traversal must inspect `arena.valueKindOf(cell)` and extract
typed values using `arena.asDouble(cell)`, `arena.asInt64(cell)`, and `arena.asBool(cell)`, or
delegate to `VortexStdLib::renderTerm` to ensure numeric and boolean arguments format correctly.

### 7. (§4.3 line 165) Ambiguous single-stepping target semantics

**Problem:** Section 4.3 specifies:

> `step [<count>]`: Advances single-step execution for selected or all actors.

In `apps/common/xanadu/vagent/tui/agent_tui.cpp:488–500`, `step(1)` invokes
`executor_.pumpUntilIdle(1)`. This advances whichever actor happens to be at the head of the global
round-robin queue, completely ignoring the selected actor in the Actor Registry (`selectedPid_`).

The specification is ambiguous as to whether `step` steps the currently selected actor or pumps the
cluster queue, and does not define how the user specifies which behavior they want.

**Evidence:**

- `design/vagent-tui-dashboard.md:165`: "for selected or all actors".
- `apps/common/xanadu/vagent/tui/agent_tui.cpp:488–491`: `step()` ignores `selectedPid_`.

**Fix:** Disambiguate the syntax and semantics in §4.3:

- `step [<count>]` (or hotkey `'s'`): If an actor is selected, steps that actor specifically via
  `runtime_.wake(*selectedPid_)`. If no actor is selected, pumps the global cluster queue.
- `step --all [<count>]` (or `pump [<count>]`): Explicitly advances the global cluster round-robin
  queue.

______________________________________________________________________

## MINOR

### 8. (§5.1 line 317, §10 P22.9) `AgentExecutor::earliestDeadline` omitted from proposal

**Problem:** Section 5.1 line 317 calls `if (auto deadline = executor.earliestDeadline())`.
`AgentExecutor` (`agent_executor.hpp:386`) currently only defines
`nextDeadline(const identity::ActorPid &pid) const`. Proposal `P22.9` defines
`notificationDescriptor()` and `drainNotifications()`, but omits `earliestDeadline()`.

**Evidence:** `design/terminal-ui-architecture.md:317, 530–537`.

**Fix:** Add `std::optional<std::chrono::steady_clock::time_point> earliestDeadline() const` to the
specification of Proposal `P22.9` in `terminal-ui-architecture.md` §10.

### 9. (§3.1 line 116, §4.1) Undefined telemetry cell notation (`104c/12e`)

**Problem:** The telemetry card mock displays `Cells: 104c/12e` without defining what `c` and `e`
stand for.

**Evidence:** `design/vagent-tui-dashboard.md:116`.

**Fix:** Document in §4.1 that `c` denotes scratch cells (`ctx->arena.cellCount()`) and `e` denotes
epistemic cells managed under the epistemic ingestion budget (`executor.epistemicCells(pid)`).

______________________________________________________________________

VERDICT: NOT CONVERGED

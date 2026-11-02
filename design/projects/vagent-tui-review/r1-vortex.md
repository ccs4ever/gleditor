# Round 1 critique: Vortex Expert (terminal-ui-architecture.md & vagent-tui-dashboard.md)

Paths below are relative to the repo root. Tally: 3 BLOCKING, 4 MAJOR, 3 MINOR.

## BLOCKING

### 1. (§1.1 line 51, §4.2 line 141) Deductive Vlog REPL mutates live arena

**Problem:** Section 1.1 item 3 specifies running deductive queries "directly against an actor's
live ZigZag memory arena to inspect relational goals without interrupting actor execution", with
multi-solution pagination (`Press ';' for more`).

In `apps/common/xanadu/vprolog/compiler.cpp:41–89, 186–204`, compiling a query via
`Compiler::compileQuery` calls `lowerTerm`, which directly mints cells (`makeCell`, `makeVar`,
`makeTerm`) and reciprocal links in `core_.arena()`. Without an explicit `takeMark()` and
`release()` pair, query lowering permanently pollutes the live actor's scratch arena, inflating
`cellCount()` and consuming the actor's `ActorGrant::maxCells` allocation budget.

More critically, holding query state suspended across user keystrokes for pagination (`;`) leaves an
active choice point (`Mark`) on the actor's live arena across event loop iterations. If the actor
executes a turn in the background (or via `'s'`), or invokes `compact()` (which runs every
`compactEvery{64}` turns in `AgentSettings`), `compact()` checks `outstandingMarks() != 0` and fails
with `ArenaRefusal::MarksOutstanding` (`apps/common/xanadu/zigzag/arena_manifold.cpp:701–709`). If
the user subsequently requests the next solution via `;`, the backtracking release
(`arena.release(m)`) truncates the arena's slots, link arrays, and trail back to the mark's
snapshot, silently destroying all state modifications made by the actor during its turn.
Furthermore, `MeshActorRuntime::upgrade()` refuses with `ActorError::NotQuiescent` if
`actor->context->arena.outstandingMarks() != 0`
(`apps/common/xanadu/vortex/mesh_actor.cpp:767–770`).

**Evidence:**

- `design/vagent-tui-dashboard.md:51–53`:

  > "3. **Execute Interactive Deductive Queries**: Formulate Vlog / Prolog queries directly against
  > an actor's live ZigZag memory arena to inspect relational goals without interrupting actor
  > execution."

- `design/vagent-tui-dashboard.md:141–144`:

  > "Vlog Deductive REPL: Up/Down recalls previous queries from queryHistory\_. Solutions display
  > with pagination: Solutions 1–5 of 18 (Press ';' for more)."

- `apps/common/xanadu/vprolog/compiler.cpp:186–190`:

  ```cpp
  for (const auto &term : queryTerms) {
    collectNames(term);
    zigzag::CellRef goalCell = lowerTerm(term, varMap);
    result.goals.push_back(goalCell);
  }
  ```

- `apps/common/xanadu/zigzag/arena_manifold.hpp:699–709`:

  ```cpp
  /**
   * @brief Discard dead runs, leaving every cell's runs contiguous.
   *
   * **Refused while a mark is outstanding**, and this is load-bearing rather
   * than cautious: compaction moves every run...
   * @return ArenaRefusal::MarksOutstanding if it refused.
   */
  ArenaResult compact();
  ```

- `apps/common/xanadu/vortex/mesh_actor.cpp:767–770`:

  ```cpp
  if (actor->running || !drained(pid) ||
      actor->context->vm.choiceDepth() != 0 ||
      actor->context->arena.outstandingMarks() != 0)
    return std::unexpected(ActorError::NotQuiescent);
  ```

**Fix:** Forbid running unisolated queries on live actor arenas. The specification must define a
Scoped Evaluation Frame for the Vlog REPL:

1. Batch execution with immediate release: The query runner takes `Mark m = arena.mark()`, compiles
   the query, resolves up to $N$ solutions into detached `LogicSolution::formatted` strings, and
   immediately calls `arena.release(m)` before returning to the event loop. No choice points or
   marks may survive across event loop iterations or user keystrokes.
1. Alternatively, require queries to execute on an isolated snapshot arena (hydrated via
   `ArenaManifold::exportPrefix` / `hydratePrefix` or an ephemeral `ArenaManifold` overlay with
   `const Manifold *base`), decoupling REPL execution completely from the actor's live
   `WorkerContext`.
1. Add a proposal in the Proposal Register to enforce fuel limits (`ScopedEvaluationLimits`) on
   interactive REPL queries.

### 2. (§5.1 lines 230–273) Reactive loop relies on hallucinated APIs and omits turn pumping

**Problem:** Section 5.1 specifies the central TUI event loop `runTuiLoop` multiplexing `poll(2)`
over `STDIN_FILENO` and `notifyFd = executor.notificationDescriptor()`. It calls
`executor.earliestDeadline()` to compute timeouts and `executor.drainNotifications()` when `POLLIN`
is signaled on `notifyFd`.

None of these three methods (`notificationDescriptor()`, `earliestDeadline()`,
`drainNotifications()`) exist in `AgentExecutor`
(`apps/common/xanadu/vagent/runtime/agent_executor.hpp:232–350`) or `MeshActorRuntime`
(`apps/common/xanadu/vortex/mesh_actor.hpp:134–286`). Neither `terminal-ui-architecture.md` §10 nor
`vagent-tui-dashboard.md` §7 provides a proposal (`P<k>`) to add notification descriptors or eventfd
signaling to `AgentExecutor`.

Crucially, `runTuiLoop` never calls `executor.pumpUntilIdle()` or `runtime.pumpOne()` anywhere in
its loop body! Even if `drainNotifications()` were implemented, draining notification descriptors
without pumping actor turns means background actors, mailbox messages, and timers will never
execute.

**Evidence:**

- `design/terminal-ui-architecture.md:237–268`:

  ```cpp
  const int notifyFd = executor.notificationDescriptor(); // eventfd or pipe
  fds[1].fd     = notifyFd;
  fds[1].events = (notifyFd >= 0) ? POLLIN : 0;
  ...
  if (auto deadline = executor.earliestDeadline()) { ... }
  ...
  int ret = ::poll(fds, (notifyFd >= 0) ? 2 : 1, timeoutMs);
  if (ret > 0 && (fds[1].revents & POLLIN)) {
    executor.drainNotifications();
    tui.markDirty();
  }
  ```

- `apps/common/xanadu/vagent/runtime/agent_executor.hpp:232–350`: Grepping `notificationDescriptor`,
  `earliestDeadline`, and `drainNotifications` yields 0 matches in the repository.

- `design/terminal-ui-architecture.md:404–416`: Proposals `P22.1`–`P22.4` do not cover any
  notification descriptor or eventfd mechanism for `AgentExecutor`.

**Fix:**

1. Revise `runTuiLoop` to call `executor.pumpUntilIdle(settings.readyPerMessage)` or
   `runtime.pumpOne(wallNow)` whenever the reactor wakes, ensuring actors make progress.
1. Replace the hallucinated `notificationDescriptor()` with an explicit proposal in the Proposal
   Register (e.g. `P22.9`) adding an `eventfd`/pipe mechanism to `AgentExecutor` that signals when
   an external message or timer is enqueued, or specify that single-threaded execution polls
   `STDIN_FILENO` with a timeout bounded by the nearest actor deadline.
1. Implement `earliestDeadline()` on `AgentExecutor` by querying active actor deadlines, and
   document it in the proposal.

### 3. (§5 lines 163–175, §7 line 215) `HostSink` callback signature and wire API contradict C++

**Problem:** Section 5 specifies installing a `HostSink` lambda on `executor`:

```cpp
executor.setHostSink([&tui](const identity::ActorPid &pid,
                            std::string_view eventType,
                            const runtime::WireRecord &payload) {
  ... pid.cursor ... payload.toJson() ...
});
```

and §7 `P22.5` states: "Connect `executor_.setHostSink` to `AgentTui::appendActionLog` in
`apps/vagent/main.cpp`".

This contradicts the codebase in three distinct ways:

1. `HostSink` is defined in `apps/common/xanadu/vagent/runtime/agent_executor.hpp:301` as
   `using HostSink = std::function<void(const Envelope &)>;`. It does not accept
   `(pid, eventType, payload)`.
1. `Envelope` (`apps/common/xanadu/vagent/runtime/wire.hpp:102–117`) carries `from` (the emitter
   string name), not an `identity::ActorPid`. Furthermore, `identity::ActorPid` has fields `node`,
   `name`, `epoch`, `index`, and has no `cursor` member.
1. `payload` is `WireRecord` (`apps/common/xanadu/vagent/runtime/wire.hpp:57–65`), which has no
   `toJson()` method.

The code in §5 cannot compile against the live tree.

**Evidence:**

- `design/vagent-tui-dashboard.md:163–175`:

  ```cpp
  executor.setHostSink([&tui](const identity::ActorPid &pid,
                              std::string_view eventType,
                              const runtime::WireRecord &payload) {
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::format("{:%H:%M:%S}", now);
    std::string formatted = std::format("[{} EMIT from {}] {} {}",
                                        timestamp, pid.cursor,
                                        eventType, payload.toJson());
    tui.appendActionLog(std::move(formatted));
    tui.markDirty();
  });
  ```

- `apps/common/xanadu/vagent/runtime/agent_executor.hpp:301–305`:

  ```cpp
  using HostSink = std::function<void(const Envelope &)>;
  AgentExecutor *setHostSink(HostSink sink) {
    hostSink_ = std::move(sink);
    return this;
  }
  ```

- `apps/common/xanadu/vagent/runtime/wire.hpp:57–65, 102–117`: `struct Envelope` holds `from`,
  `type`, `fields` (`WireRecord`). `WireRecord` provides `find` and `set`, but no `toJson()`.

**Fix:** Update §5 and `P22.5` to conform to the existing `HostSink` signature:

```cpp
executor.setHostSink([&tui](const runtime::Envelope &envelope) {
  auto now = std::chrono::system_clock::now();
  auto timestamp = std::format("{:%H:%M:%S}", now);
  std::string formatted = std::format("[{} EMIT from {}] {} {}",
                                      timestamp, envelope.from,
                                      envelope.type, encodeWireRecord(envelope.fields));
  tui.appendActionLog(std::move(formatted));
  tui.markDirty();
});
```

Specify a helper `encodeWireRecord` (or bencode/compact wire representation) rather than calling a
non-existent `toJson()` method.

## MAJOR

### 4. (§1.1 line 54, §4.3) Interactive single-stepping triggers actor lease expiration

**Problem:** Actors are bounded by `ActorGrant::expiresAt`
(`apps/common/xanadu/vortex/mesh_actor.hpp:58`). In production, leases expire after `leaseDuration`
(default 60s, `mesh_actor.hpp:81`).

During an interactive TUI session, human operators read tables, inspect beliefs, and ponder queries.
Real time elapses. If the cluster runs against `AgentClock::system()`, the lease will expire while
the TUI is open. The next step or pump will immediately fail in `MeshActorRuntime::pumpOne`:

```cpp
if (wallNow >= actor.grant.expiresAt) {
  actor.stopped      = true;
  actor.leaseExpired = true;
  return std::unexpected(ActorError::NotAuthorized);
}
```

(`apps/common/xanadu/vortex/mesh_actor.cpp:597–601`). The actor will be killed simply because the
user paused between keystrokes.

**Evidence:**

- `apps/common/xanadu/vortex/mesh_actor.cpp:597–601`:

  ```cpp
  if (wallNow >= actor.grant.expiresAt) {
    actor.stopped      = true;
    actor.leaseExpired = true;
    return std::unexpected(ActorError::NotAuthorized);
  }
  ```

- `apps/common/xanadu/vagent/runtime/agent_executor.hpp:140–145`: Default `AgentClock::system()`
  uses unfreezable system steady and wall clocks.

**Fix:** The TUI dashboard specification must explicitly define lease management for interactive
sessions: either (a) when `--interactive` / TUI mode is active, `LocalHost` must construct
`AgentClock` backed by a controllable virtual clock that freezes during idle periods, or (b)
`AgentExecutor` must automatically renew leases for active actors (`renew()`) at turn boundaries
while under operator observation.

### 5. (§1.1 line 54, referencing agent_tui.cpp:502) `advanceClock` is a no-op for simulated time

**Problem:** Section 1.1 promises that operators can "advance simulated clock time". In
`apps/common/xanadu/vagent/tui/agent_tui.cpp:502–514`:

```cpp
std::size_t AgentTui::advanceClock(std::chrono::milliseconds ms) {
  executor_.tick();
  std::size_t ran = executor_.pumpUntilIdle();
  totalTurns_ += ran;
  actionLog_.push_back(
      std::format("Advanced clock by {}ms, ran {} turn(s)", ms.count(), ran));
  ...
  return ran;
}
```

`ms` is completely ignored; it is never passed to `executor_` or `runtime_`. `AgentExecutor` has no
API to advance time on its clock. In `apps/vagent/main.cpp:1189`, CLI batch `--advance` uses
`host.advance(by, turns)`, which exists on `LocalHost`, but `AgentTui` is passed only
`AgentExecutor &` and `MeshActorRuntime &`, neither of which exposes time advancement.

**Evidence:**

- `design/vagent-tui-dashboard.md:54`: "advance simulated clock time"

- `apps/common/xanadu/vagent/tui/agent_tui.cpp:502–514`: `ms` is discarded.

- `apps/common/xanadu/vagent/runtime/agent_executor.hpp:232–350`: No `advance(milliseconds)` method
  exists.

**Fix:** Add a proposal to the Proposal Register extending `AgentExecutor` with
`advanceTime(std::chrono::milliseconds delta)` when running with a controllable `AgentClock`, and
wire `AgentTui::advanceClock` to invoke it before pumping turns.

### 6. (§4.2 lines 135–136) Compound argument traversal on `d.grab` violates calling convention

**Problem:** Section 4.2 claims: "Compound Argument Traversal: Iterates all argument terms on
`ctx->core.dims().grab` instead of truncating to the first term, rendering complete tuples:
`target(x: 12.5, y: 40.0)`."

In Vortex and Vlog, `d.grab` only links a term to its *first* argument (the grab head). Subsequent
arguments are chained along `+d.step` off the grab head (the Inputs Wing calling convention).
Traversing `d.grab` will find only the first argument. An implementer following this specification
will fail to traverse subsequent arguments.

**Evidence:**

- `design/vagent-tui-dashboard.md:135–136`:

  > "- **Compound Argument Traversal**: Iterates all argument terms on `ctx->core.dims().grab`
  > instead of truncating to the first term..."

- `apps/common/xanadu/zigzag/vlog.hpp:11–13`:

  > "A term is a cell. Its functor is the cell's content; its arguments hang off the Inputs Wing --
  > first on +d.grab, the rest chained on +d.step -- which is Vortex's ordinary calling
  > convention..."

- `apps/common/xanadu/zigzag/vlog.cpp:69–75`: `argumentsOf` walks `rank(m, *head, step)` from
  `m.linked(deref(ref), grab)`.

**Fix:** Correct the specification to describe the canonical Inputs Wing walk: follow `+d.grab` to
obtain the head argument, then walk the `+d.step` rank to enumerate subsequent positional arguments.

### 7. (§4.2 lines 135–144) Raw `CellRef` leakage and unhandled scalar type formatting

**Problem:** In `apps/common/xanadu/vagent/tui/agent_tui.cpp:371–375`, arguments are read via
`arena.textOf(*first)`. In `ArenaManifold`, `textOf` only reads text spans; it returns an empty
string `""` for scalar cells (`ValueKind::Int64`, `ValueKind::Double`, `ValueKind::Bool`). Numeric
arguments like `12.5` and `40.0` silently format as empty strings or `_`.

Additionally, when `vprolog::Compiler::solve` produces solutions, unbound variables format as
`_G<CellRef>` (e.g. `_G2147483658`), directly exposing the raw 32-bit arena dense index with
`ephemeralBit` (`1u << 31`) set.

**Evidence:**

- `apps/common/xanadu/vagent/tui/agent_tui.cpp:371–375`: `if (first) br.key = arena.textOf(*first);`

- `apps/common/xanadu/zigzag/arena_manifold.cpp:1127–1147`: `ArenaManifold::textOf` returns `""` for
  scalar cells.

- `apps/common/xanadu/vortex/vortex_stdlib.cpp:4557`: `return "_G" + std::to_string(*actual);`

**Fix:** Specify that belief key extraction and term rendering must use `VortexStdLib::renderTerm`
(or inspect `arena.valueKindOf(cell)` and extract typed scalar values via `asDouble`, `asInt64`,
`asBool`), preventing blank values and normalizing variable identifiers to logical indexes (`_G1`,
`_G2`) instead of raw bitmasks.

## MINOR

### 8. (§4 line 213) Fragmented heap allocation per row in `VirtualScreenBuffer`

**Problem:** `grid_` is declared as `std::vector<std::vector<ScreenCell>>`. This creates 30 separate
heap allocations and causes pointer indirection on cell access. A flat buffer
`std::vector<ScreenCell> grid_{static_cast<std::size_t>(width * height)}` indexed by
`y * width_ + x` is cache-friendly and avoids heap fragmentation.

**Evidence:** `design/terminal-ui-architecture.md:213`:
`std::vector<std::vector<ScreenCell>> grid_;`

**Fix:** Replace the vector-of-vectors with a flat 1D vector and coordinate indexer.

### 9. (§4.1, §4.3) Ambiguity between stepping selected actor vs cluster queue

**Problem:** The status bar provides `[s] Step`, but `AgentTui::step(1)` calls
`executor_.pumpUntilIdle(1)`, which advances whichever actor is at the head of the round-robin queue
(`lastServed_`), not necessarily the selected actor.

**Evidence:** `apps/common/xanadu/vagent/tui/agent_tui.cpp:488–500`: `step(1)` pumps the global
executor without checking `selectedPid_`.

**Fix:** Clarify whether `'s'` steps the currently selected actor (via targeted
`runtime_.wake(pid)`) or advances the global cluster by 1 turn.

### 10. (§3.1 line 114) Undefined telemetry cell notation (`104c/12e`)

**Problem:** The telemetry box displays `Cells: 104c/12e` without documenting what `c` and `e` stand
for.

**Evidence:** `design/vagent-tui-dashboard.md:114`: `│ Fuel: 1.0G Cells: 104c/12e │`

**Fix:** Clarify in the view specification that `c` denotes total arena scratch cells
(`ctx->arena.cellCount()`) and `e` denotes epistemic belief cells (`executor.epistemicCells(pid)`).

VERDICT: NOT CONVERGED

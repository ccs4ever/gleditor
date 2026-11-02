# Round 1 critique: Codebase Expert (terminal-ui-architecture.md & vagent-tui-dashboard.md)

Paths below are relative to the repo root. Tally: 3 BLOCKING, 4 MAJOR, 3 MINOR.

## BLOCKING

### 1. (§5.1 & vagent-tui-dashboard §5) Hallucinated `AgentExecutor` API Signatures

**Problem:** Multiple code snippets in both design documents cannot compile against the live
repository because they invoke hallucinated methods on `AgentExecutor`, provide incompatible lambda
signatures, rely on non-existent `WireRecord` methods, and call private or non-existent `AgentTui`
members. None of these changes are accounted for in the Proposal Register.

**Evidence:**

From `design/terminal-ui-architecture.md:237-239`:

```cpp
const int notifyFd = executor.notificationDescriptor(); // eventfd or pipe
fds[1].fd     = notifyFd;
fds[1].events = (notifyFd >= 0) ? POLLIN : 0;
```

And lines 265–267:

```cpp
if (ret > 0 && (fds[1].revents & POLLIN)) {
  executor.drainNotifications();
  tui.markDirty();
}
```

In `apps/common/xanadu/vagent/runtime/agent_executor.hpp:1-748`, `AgentExecutor` contains no member
named `notificationDescriptor` or `drainNotifications`. `git grep notificationDescriptor` returns
zero hits across the repository. Furthermore, `AgentTui` in
`apps/common/xanadu/vagent/tui/agent_tui.hpp` has no `markDirty()` member.

From `design/terminal-ui-architecture.md:251-255`:

```cpp
if (auto deadline = executor.earliestDeadline()) {
  auto now = std::chrono::steady_clock::now();
  auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now).count();
  timeoutMs = std::clamp<int>(static_cast<int>(delta), 0, 250);
}
```

`AgentExecutor` has no zero-argument `earliestDeadline()` method. The existing deadline API is
`nextDeadline(const identity::ActorPid &pid) const` (`agent_executor.hpp:386`), which requires a
target PID. Furthermore, `nextDeadline` returns milliseconds on the wall/system clock, whereas
`runTuiLoop` compares it against `std::chrono::steady_clock::now()`, mixing incompatible clocks.

From `design/vagent-tui-dashboard.md:163-175`:

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

1. In `apps/common/xanadu/vagent/runtime/agent_executor.hpp:301-302`, the signature of `HostSink`
   is: `using HostSink = std::function<void(const Envelope &)>;`. It accepts a single
   `const Envelope &`, not three separate parameters.
1. In `apps/common/xanadu/vagent/runtime/wire.hpp:102-117`, `Envelope` holds `std::string from`
   (emitter name), not an `ActorPid` cursor.
1. `WireRecord` (`wire.hpp:57-65`) has no member function `toJson()`. Emission formatting in the
   repo uses `renderFields(const WireRecord &)` (`apps/vagent/main.cpp:424`) or
   `renderEmission(const Envelope &)` (`main.cpp:458`).
1. `AgentTui` (`apps/common/xanadu/vagent/tui/agent_tui.hpp:130`) exposes `actionLog()` as a const
   reference only; it has no `appendActionLog()` method and no `markDirty()` method.

**Fix:**

1. Author an explicit proposal (`P22.3a` or augment `P22.3`) to add descriptor readiness
   (`eventfd(2)` on Linux, self-pipe on POSIX) and `drainNotifications()` to `AgentExecutor` or
   `LocalHost`.
1. Add an `earliestDeadline()` query to `AgentExecutor` that evaluates the minimum pending deadline
   across all active actors, returning `std::optional<std::chrono::system_clock::time_point>`. Fix
   `runTuiLoop` to compute duration delta using `std::chrono::system_clock::now()`.
1. Correct the `setHostSink` lambda signature to `[](const Envelope &event)` and format emissions
   using existing `renderEmission(event)`. Add `void appendActionLog(std::string entry)` and
   `void markDirty()` to `AgentTui`'s public interface, enumerating these additions in `P22.5`.

### 2. (vagent-tui-dashboard §4.2) 2-Degree Rank Invariant Violation in Vlog Traversal

**Problem:** §4.2 specifies compound argument traversal by iterating along `ctx->core.dims().grab`:
"Compound Argument Traversal: Iterates all argument terms on `ctx->core.dims().grab` instead of
truncating to the first term, rendering complete tuples: `target(x: 12.5, y: 40.0)`." In the
Nelsonian hyperstructure of Vlog (`apps/common/xanadu/zigzag/vlog.cpp:33–55`), `makeTerm` links the
head term to the *first* argument cell via `grab` (pos), but all subsequent arguments are chained
along `step` (`previous -> step (pos) -> actualArg`). Attempting to traverse multiple arguments
along `d.grab` violates the 2-degree rank invariant (a cell has at most one POS link and one NEG
link along any dimension). Iterating on `grab` will find at most one argument.

**Evidence:**

From `design/vagent-tui-dashboard.md:135-136`:
`Compound Argument Traversal: Iterates all argument terms on ctx->core.dims().grab`

In `apps/common/xanadu/zigzag/vlog.cpp:33-55`:

```cpp
CellRef Vlog::makeTerm(const std::string_view functor,
                       const std::span<const CellRef> args) {
  const auto term  = m.makeCell(functor);
  CellRef previous = noCell;
  std::vector<CellRef> seen;
  for (const CellRef arg : args) {
    ...
    if (noCell == previous) {
      zigzag::expectWritten(m.link(term, grab, DimVector::POS, actualArg));
    } else {
      zigzag::expectWritten(m.link(previous, step, DimVector::POS, actualArg));
    }
    previous = actualArg;
  }
  return term;
}
```

In `apps/common/xanadu/vprolog/compiler.cpp:80-85`:

```cpp
if (const auto *comp = term.asCompound()) {
  std::vector<zigzag::CellRef> loweredArgs;
  loweredArgs.reserve(comp->args.size());
  for (const auto &arg : comp->args) {
    loweredArgs.push_back(lowerTerm(arg, varMap));
  }
  return vlog_.makeTerm(comp->functor, loweredArgs);
}
```

**Fix:** Replace the wording in §4.2 with the correct hyperstructural rank traversal: Follow
`ctx->core.dims().grab` once from the head term cell to obtain `arg0`. If `arg0` exists, traverse
all subsequent argument cells along `ctx->core.dims().step` in the positive direction using
`zigzag::rankAfter(arena, arg0, ctx->core.dims().step)`. Format each cell's text or scalar value to
construct the complete argument tuple.

### 3. (terminal-ui-architecture §5.1) Curses Stdio Starvation & Headless Hangs in `poll`

**Problem:** `runTuiLoop` relies on `poll(fds, ...)` directly on `STDIN_FILENO` without coordinating
with curses' internal stdio buffering or checking `NCursesActor`'s pending queue.

1. When curses is initialized (`initscr()`, `cbreak()`, `keypad(stdscr, TRUE)`), `wgetch` drains
   bytes from the kernel into `libncurses`'s user-space buffers. If a user pastes text or generates
   a multi-byte escape sequence, `poll(STDIN_FILENO)` will report no readiness even though curses
   has unprocessed characters in its buffer. Furthermore, any synthetic events queued in
   `NCursesActor::inputQueue_` (`ncurses_actor.hpp:179`) via `injectInput()` will sit unconsumed
   while `poll()` blocks in the kernel.
1. In headless environments (such as CI test runs or container execution where stdin is `/dev/null`
   or closed), `isatty(STDIN_FILENO)` is false. Polling a closed or EOF descriptor causes `poll(2)`
   to immediately return `POLLHUP` on every iteration, converting `runTuiLoop` into a 100% CPU
   busy-spin loop. Alternatively, if stdin is an open but inactive pipe, `poll()` blocks
   indefinitely instead of executing deterministic headless steps.

**Evidence:**

From `design/terminal-ui-architecture.md:234-247`:

```cpp
fds[0].fd     = STDIN_FILENO;
fds[0].events = POLLIN;
...
while (auto ev = device.pollInput()) {
  tui.handleInput(*ev);
  inputHandled = true;
}
```

In `apps/common/xanadu/vagent/tui/ncurses_actor.hpp:179`: `std::deque<KeyEvent> inputQueue_;`

In `apps/common/xanadu/vagent/tui/ncurses_actor.cpp:293-298`: `pollInput()` drains `inputQueue_`
first. If `injectInput` is called in tests, `STDIN_FILENO` has no data.

In `apps/vagent/main.cpp:1255-1259`:

```cpp
if (headless || actor->isHeadless()) {
  tui.render();
  std::cout << tui.actor().screen().dump();
  return exitOk;
}
```

Headless mode currently bypasses the loop entirely because polling `STDIN_FILENO` is invalid
headless.

**Fix:**

1. In `runTuiLoop`, check `device.hasPendingInput()` before calling `::poll()`. If input is pending,
   set `timeoutMs = 0` so the loop does not block in the kernel.
1. If `device.isHeadless()` is true, or if `STDIN_FILENO` receives `POLLHUP` / `POLLERR`, set
   `fds[0].fd = -1` to permanently disable polling stdin.
1. In headless test modes, allow the loop to be stepped deterministically via `tui.tick()` without
   invoking kernel `poll(2)`.

______________________________________________________________________

## MAJOR

### 1. (terminal-ui-architecture §10 & vagent-tui-dashboard §7) Proposal Register Audit

**Problem:** Proposals P22.1–P22.8 lack file attribution, migration costs, and fallbacks required by
house style. Furthermore, major architectural modifications described in the text are completely
missing from the register, and several proposals duplicate each other across the two companion
documents.

**Evidence:**

1. **Unlisted Architecture - Dual-Mode Master-Detail Split Layout** (`vagent-tui-dashboard.md`
   §3.1): Requires a complete overhaul of `AgentTui::renderHeader`, `renderMonitoringPane`, and pane
   geometry in `apps/common/xanadu/vagent/tui/agent_tui.cpp` to support side-by-side columns at
   width $\ge 110$. No proposal number, cost, or file list is provided.
1. **Unlisted Command - Pre-Flight Upgrade Validation** (`vagent-tui-dashboard.md` §4.3): Introduces
   `upgrade-check <target> <programRef>` to `AgentTui::executeCommand` in `agent_tui.cpp:600-696`,
   but is not listed in P22.5–P22.8.
1. **Unlisted Class & Namespace Migration**: `P22.1` proposes refactoring `NCursesActor` into
   `TerminalDevice` under namespace `xanadu::tui` (`terminal-ui-architecture.md:102`), moving it out
   of `xanadu::vagent::tui`. It does not list the breaking changes across
   `apps/common/xanadu/vagent/tui/agent_tui.hpp`, `apps/common/xanadu/vagent/tui/ncurses_actor.hpp`,
   `apps/vagent/main.cpp`, and `tests/xuzz/vagent_tui_test.cpp`.
1. **Duplicate / Overlapping Proposals**: `P22.4` ("Standardize Viewport scrolling offsets and
   navigation history across AgentTui") in `terminal-ui-architecture.md:415-416` duplicates both
   `P22.6` ("Implement Viewport scrolling offsets...") and `P22.7` ("Add command and query history
   buffers...") in `vagent-tui-dashboard.md:217-220`.
1. **Unlisted Settings Integration**: `vagent-tui-dashboard.md` §8 specifies three
   `system://settings` keys (`vagent.tui.borders`, `vagent.tui.history_size`,
   `vagent.tui.split_threshold`), but no proposal covers loading these settings into `AgentTui` via
   `LocalHost::Options` or `AgentSettings`.

**Fix:** Consolidate the Proposal Register into a unified, properly costed specification:

- Retract `P22.4` in `terminal-ui-architecture.md` as redundant with `P22.6` and `P22.7`.
- Assign `P22.9` for Dual-Mode Master-Detail Split layout in `AgentTui`.
- Assign `P22.10` for `upgrade-check` CLI command in `AgentTui`.
- Assign `P22.11` for `system://settings` dynamic configuration loading.
- For each proposal, enumerate exact files touched, estimated line diffs, and fallback behaviors.

### 2. (terminal-ui-architecture §4) 2D Vector Allocation & Grapheme Truncation

**Problem:** `VirtualScreenBuffer` uses an inefficient 2D vector layout that causes memory
fragmentation and cache misses, while `ScreenCell` truncates Unicode grapheme clusters and lacks
wide character continuation semantics.

1. `std::vector<std::vector<ScreenCell>> grid_` performs $H + 1$ separate heap allocations. For a
   standard 120x30 buffer, this requires 31 distinct heap objects. Every cell read `grid_[y][x]`
   dereferences two pointers, degrading cache locality during high-rate differential damage sweeps.
1. Invariant T6 claims "Unicode Grapheme Awareness (up to 4 bytes or char32_t)". A `char32_t` is a
   single Unicode codepoint, not a grapheme cluster. Combining character sequences (e.g. `e` +
   `\u0301`), emojis with skin tone modifiers, and zero-width joiner (ZWJ) sequences require
   multiple codepoints and cannot fit in a single `char32_t`.
1. Wide characters (`width == 2`): In terminal monospace grids, an East Asian wide character
   occupies two columns. The design provides no continuation cell specification for column `x + 1`,
   leading to visual truncation or out-of-bounds writes when a wide character is printed at column
   $W - 1$.
1. `VirtualScreenBuffer::line(int y)` in `ncurses_actor.cpp:124` appends `cell.ch` directly to an
   `std::string`. If `codepoint` is `char32_t`, direct conversion truncates the codepoint to 8 bits
   unless properly encoded to UTF-8.

**Evidence:**

From `design/terminal-ui-architecture.md:179-185`:

```cpp
struct ScreenCell {
  char32_t codepoint{U' '};
  std::uint8_t width{1}; // 0 for combining, 1 for normal, 2 for wide
  Style style{};
  bool operator==(const ScreenCell &) const = default;
};
```

From `design/terminal-ui-architecture.md:213`: `std::vector<std::vector<ScreenCell>> grid_;`

From `apps/common/xanadu/vagent/tui/ncurses_actor.cpp:123-125`:

```cpp
for (const auto &cell : grid_[y]) {
  s.push_back(cell.ch);
}
```

**Fix:**

1. Replace `grid_` with a contiguous flat buffer: `std::vector<ScreenCell> cells_` of size
   $W \times H$, indexed via `y * width_ + x`.
1. Specify that when a wide character (`width == 2`) is written at column `x`, column `x + 1` is
   automatically filled with a continuation cell having `codepoint = 0` and `width = 0`. If
   `x == W - 1`, clip the character and replace it with a single space.
1. Update `VirtualScreenBuffer::line(int y)` to encode each `char32_t` codepoint into UTF-8 before
   appending to `std::string`.

### 3. (terminal-ui-architecture §7.1) Input Draft Loss on History Navigation

**Problem:** When a user navigates command or query history using `Up` / `Down` in Edit Mode, the
current input buffer (`commandInput_` or `queryInput_`) is immediately overwritten with the recalled
history entry. If the operator had typed a partial command and pressed `Up` to inspect previous
syntax, returning `Down` past the most recent entry fails to restore the in-progress draft,
permanently destroying uncommitted input.

**Evidence:**

From `design/terminal-ui-architecture.md:348-355`:
`Up / Down navigates input history (commandHistory_, queryHistory_).`

From `apps/common/xanadu/vagent/tui/agent_tui.cpp:812-858`: `commandInput_` is a single
`std::string`. No draft stashing or restoration mechanism exists.

**Fix:** Introduce transient draft storage in `AgentTui`:

- `std::string savedCommandDraft_` and `std::optional<std::size_t> commandHistoryIndex_`.
- `std::string savedQueryDraft_` and `std::optional<std::size_t> queryHistoryIndex_`. When `Up` is
  pressed from the active input line (`historyIndex == nullopt`), stash the current buffer into
  `savedDraft_`. When `Down` navigates back past index 0, restore `savedDraft_` to the active buffer
  and reset the index to `std::nullopt`.

### 4. (vagent-tui-dashboard §5, line 160) Overwriting `LocalHost`'s Internal `hostSink_`

**Problem:** In `apps/vagent/main.cpp:1227`, `runTui` instantiates `LocalHost host(...)`. During its
construction (`apps/common/xanadu/vagent/runtime/program.cpp:350–351`), `LocalHost` registers its
own host sink:

```cpp
executor_->install()
    ->setBindings(&bindings_)
    ->setHostSink(
        [this](const Envelope &event) { emissions_.push_back(event); });
```

If `main.cpp` calls `host.executor().setHostSink(...)` as proposed in §5, it completely overwrites
`LocalHost`'s internal sink. Any testing assertions, telemetry monitors, or CLI routines that query
`host.emissions()` will subsequently see an empty list, breaking telemetry assertions across the
test suite.

**Evidence:**

From `design/vagent-tui-dashboard.md:163`: `executor.setHostSink([&tui](...) { ... });`

In `apps/common/xanadu/vagent/runtime/program.cpp:350-351`: `LocalHost` relies on `setHostSink` to
populate its `emissions_` vector.

In `apps/common/xanadu/vagent/runtime/program.hpp:274-276`:
`[[nodiscard]] const std::vector<Envelope> &emissions() const noexcept { return emissions_; }`

**Fix:** Provide an additive observer or chaining callback mechanism. Either:

1. Allow `AgentExecutor::setHostSink` to chain or support multiple sinks; or
1. Add an emission listener API to `LocalHost` (e.g. `host.addEmissionObserver(...)`), matching the
   pattern used by `ClusterNode::setEmissionCallback` in `apps/vagent/main.cpp:1394`.

______________________________________________________________________

## MINOR

### 1. (vagent-tui-dashboard §4.2, lines 141–145) In-Band String Error Sentinels in `queryVlog`

**Problem:** `AgentTui::queryVlog` returns `std::vector<std::string>`, encoding runtime failures as
in-band string values such as `{"Error: No actor selected for query"}` or `{"false."}`. House Rule 3
("No Sentinels in APIs: Absent values use std::optional or std::expected") requires separating error
channels from valid data values.

**Evidence:**

From `apps/common/xanadu/vagent/tui/agent_tui.cpp:283`:
`if (!selectedPid_) return {"Error: No actor selected for query"};`

From `apps/common/xanadu/vagent/tui/agent_tui.cpp:315`:
`return {std::format("Query error: {}", e.what())};`

In `design/vagent-tui-dashboard.md:144-145`: Results formatting lists "Red for errors, Yellow for
false., Green for variable bindings" without specifying a typed return contract.

**Fix:** Refactor `AgentTui::queryVlog` to return
`std::expected<std::vector<std::string>, std::string>`, reserving the `std::unexpected` channel for
execution and compilation errors, and distinguishing empty solutions (`false.`) from query dispatch
failures.

### 2. (terminal-ui-architecture §3 & §5.1) In-Band Negative Integer Sentinels

**Problem:** In `runTuiLoop`, `notifyFd` uses `-1` as an in-band sentinel for an unconfigured
descriptor (`fds[1].events = (notifyFd >= 0) ? POLLIN : 0;`). In modern C++23 house style, optional
OS handles and coordinates should be represented using `std::optional<int>`.

**Evidence:**

From `design/terminal-ui-architecture.md:237`:
`const int notifyFd = executor.notificationDescriptor();`

From `design/terminal-ui-architecture.md:239`: `fds[1].events = (notifyFd >= 0) ? POLLIN : 0;`

**Fix:** Change the proposed notification descriptor API to return `std::optional<int>`.

### 3. (terminal-ui-architecture §3.1, line 154) Hardcoded Escape Delay Without Terminfo Check

**Problem:** `set_escdelay(25)` is invoked unconditionally during curses initialization. On certain
non-Linux terminal emulators or older ncurses builds where `ESCDELAY` is governed by the user's
environment, calling `set_escdelay` can trigger unexpected behavior or be rejected if terminfo
capabilities are missing.

**Evidence:**

From `design/terminal-ui-architecture.md:164`:
`set_escdelay(25): Clamps escape-key delay to 25ms...`

In `apps/common/xanadu/vagent/tui/ncurses_actor.cpp:183`: Currently calls `curs_set(0)` but does not
invoke `set_escdelay`.

**Fix:** Guard `set_escdelay(25)` with a check that `!isHeadless()` and verify that the `ESCDELAY`
environment variable is not already set by the user before overriding.

______________________________________________________________________

VERDICT: NOT CONVERGED

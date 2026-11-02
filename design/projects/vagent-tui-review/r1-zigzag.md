# Round 1 critique: Xanadu / ZigZag Purist (terminal-ui-architecture.md & vagent-tui-dashboard.md)

Paths below are relative to the repo root. Tally: 2 BLOCKING, 4 MAJOR, 4 MINOR.

## BLOCKING

### 1. (§4.2, line 135) Compound argument traversal on `d.grab` violates 2-degree invariant

**Problem:** Section 4.2 asserts: "Compound Argument Traversal: Iterates all argument terms on
`ctx->core.dims().grab` instead of truncating to the first term, rendering complete tuples:
`target(x: 12.5, y: 40.0)`." In ZigZag topology and Vlog
(`apps/common/xanadu/zigzag/vlog.cpp:64–78`,
`apps/common/xanadu/vagent/runtime/compiler.cpp:5823–5828`), `d.grab` connects a functor/term cell
to its *first* argument cell (`term --d.grab--> arg0`). Subsequent arguments are chained on `d.step`
(`arg0 --d.step--> arg1 --d.step--> arg2`).

If an implementer attempts to iterate "all argument terms on `d.grab`", they will either:

1. Attempt to link multiple cells to `term` on `+d.grab`, which is immediately evicted by
   `ArenaManifold::link` because ZigZag enforces `pos(d) = B <=> neg(d) = A` (the strict 2-degree
   rank invariant); or
1. Traverse `rankAfter(arena, term, grab)`, which will encounter at most one neighbor (`arg0`), or
   traverse `rankAfter(arena, arg0, grab)`, which treats nested composite subterms as sibling
   arguments, completely confounding term nesting with argument lists.

Furthermore, the example `target(x: 12.5, y: 40.0)` confuses positional term arguments with named
record fields. In `vagent`, arguments on `d.grab`/`d.step` are values; record attributes reside on
`d.vars` linked to value cells on `d.values`.

**Evidence:**

- `design/vagent-tui-dashboard.md:135–136`:

  > "- **Compound Argument Traversal**: Iterates all argument terms on `ctx->core.dims().grab`
  > instead of truncating to the first term, rendering complete tuples: `target(x: 12.5, y: 40.0)`."

- `apps/common/xanadu/zigzag/vlog.cpp:69–75`:

  ```cpp
  if (const auto head = m.linked(deref(ref), grab)) {
    for (const auto arg : rank(m, *head, step)) {
      if (std::ranges::contains(args, arg)) {
        break;
      }
      args.push_back(arg);
    }
  }
  ```

- `apps/common/xanadu/vagent/runtime/compiler.cpp:5823–5828`:

  ```cpp
  if (!prev) {
    linkOp(D(tr), core_.dims().grab, D(cell));
  } else {
    linkOp(D(*prev), core_.dims().step, D(cell));
  }
  ```

**Fix:** Correct the specification to use the canonical ZigZag Inputs Wing traversal already
implemented in `Vlog::argumentsOf`: the term cell links to its first argument via `+d.grab`, and
subsequent arguments are enumerated along `+d.step`. Clarify that positional terms are traversed on
`d.step`, whereas named key-value arguments are record fields traversed via `d.vars` and `d.values`.

### 2. (§0, line 11) Phantom dependency on `ArenaManifold` without projection interface

**Problem:** The metadata prerequisites block of `terminal-ui-architecture.md` explicitly lists
`ArenaManifold` under `rests on` (line 11). However, across all 447 lines of the document and
proposals `P22.1`–`P22.4`, `ArenaManifold` is never referenced, included, or used. Neither
`TerminalDevice` nor `VirtualScreenBuffer` exposes any interface or adapter to project an
`ArenaManifold` or `Manifold` into the virtual screen buffer.

In Nelsonian architecture, a monospace screen grid is the natural 2D visual projection of a manifold
slice along visual dimensions (`d.x`/`d.y` or `d.1`/`d.2`). Claiming that the terminal architecture
rests on `ArenaManifold` while specifying only an ad-hoc
`std::vector<std::vector<ScreenCell>> grid_` without any manifold projection protocol or cell
grounding creates a false architectural dependency and leaves the layer boundary between manifold
cells and screen cells unspecified.

**Evidence:**

- `design/terminal-ui-architecture.md:9–12`:
  > "| rests on | `VirtualScreenBuffer`, `NCursesActor`, `KeyEvent`, `Style`, `MeshActorRuntime`,
  > `AgentExecutor`, `ArenaManifold` |"
- Grepping `design/terminal-ui-architecture.md` for `ArenaManifold` matches only line 11. No
  proposal in Section 10 defines any manifold integration.

**Fix:** Either:

1. Define the formal projection interface mapping an `ArenaManifold` slice or view onto
   `VirtualScreenBuffer` (e.g. projecting cell contents and borders along specified spatial
   dimensions); or
1. Strike `ArenaManifold` from `rests on` in `terminal-ui-architecture.md`, documenting that
   `VirtualScreenBuffer` is strictly a character-level rendering sink, and place the manifold
   projection responsibility explicitly in the domain dashboard layer (`vagent-tui-dashboard.md`).

______________________________________________________________________

## MAJOR

### 3. (§5, line 160 & §7 P22.5) Causal and provenance destruction via `payload.toJson()`

**Problem:** In `vagent-tui-dashboard.md` §5 and proposal `P22.5`, `executor.setHostSink` is wired
to format outbound emissions as:

```cpp
std::string formatted = std::format("[{} EMIT from {}] {} {}",
                                    timestamp, pid.cursor,
                                    eventType, payload.toJson());
tui.appendActionLog(std::move(formatted));
```

This violates Standing Rule 2 ("never in ad-hoc C++ side tables, JSON/bencode blobs stuffed into
scalar cell text") and destroys Nelsonian transclusion and Spool-Mesh causal provenance:

1. Converting `payload` to JSON string strips its hyperstructural cell identity and prevents the
   operator from inspecting structured fields or transcluding payload data in the ZigZag space.
1. Truncating `pid` to `pid.cursor` discards the node identity, and formatting a local wall-clock
   timestamp (`%H:%M:%S`) destroys causal ordering (vector clocks and hypertime sequence).
1. Pushing flat strings to `std::vector<std::string> actionLog_` creates an ephemeral side table
   disconnected from the reader walk and `system://activity` store.

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

- Standing Rule 2 (`brief-common.md:47–48`): "State, structure, and metadata live on orthogonal
  ZigZag ranks (`d.*`), never in ad-hoc C++ side tables, JSON/bencode blobs stuffed into scalar cell
  text...".

- `AGENTS.md` ("Xuzz navigation workflow"): "Completed reader transitions form branching walks in
  the private `system://activity` store... See the R8 activity-store extension in
  `store-slice-convergence.md`."

**Fix:** Refactor `ActionLogEntry` in `AgentTui` to store structured event provenance: full
`identity::ActorPid` (node + cursor), event type, logical/causal sequence, and either the
`runtime::WireRecord` or a reference to the event cell graph in the manifold. When rendering to the
TUI, format the fields directly without intermediate JSON serialization. Document how action log
entries map to an activity rank or system store.

### 4. (§4.2, line 133) Omission of ZigZag traversal for P19 epistemic observations

**Problem:** Section 4.2 focuses heavily on ASCII gauge rendering (`[████████░░] 85.0%`) while
omitting the ZigZag traversal rules required to inspect real P19 epistemic observations and source
profiles (`design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md` §2.1).

Under P19:

- Predicates link to clauses on `d.clause`.
- Clauses link to support cells on `d.source`.
- Each support cell heads an observation rank on `d.epistemic`.
- Each observation record has field-name cells on `d.vars` linked to value cells on `d.values`.
- Source references are clone views posward of `profile_master` along `d.clone`.
- Source profiles live on `$agent +d.trust`.

The current code in `apps/common/xanadu/vagent/tui/agent_tui.cpp:340–361` reads unauthenticated
display aliases (`arena.textOf(support)`) and uses flat string key lookups. By omitting P19
topology, the dashboard cannot display authenticated source identities, cannot inspect domain
calibration records (alpha, beta, decay rate, weights), and cannot verify clone rank integrity.

**Evidence:**

- `design/vagent-tui-dashboard.md:105–109`: ASCII mock displays unauthenticated aliases `sensor_1`,
  `gps_mod`.
- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:104–107`: "Existing
  source-name text is a routing/display alias, not authenticated identity. P19.1/P19.5 bind ingress
  to a checked principal/incarnation and map the alias to that identity...".
- `apps/common/xanadu/vagent/tui/agent_tui.cpp:340`: `std::string src = arena.textOf(support);`.

**Fix:** Specify the multi-rank ZigZag traversal in §4.2:

1. Walk from `clause` to `support` along `d.source`.
1. Walk from `support` to `observation` along `d.epistemic`.
1. Walk observation fields along `d.vars` to read `d.values`.
1. Traverse `profile_field --d.values--> profile_view --d.clone(neg)--> profile_master` to resolve
   the authentic source identity and inspect its calibration record.

### 5. (§4.2, line 141) Deductive REPL discards cell identities and clone bindings

**Problem:** Section 4.2 specifies that the Vlog Deductive REPL formats query results into typed
strings: `Results: X = edge_node` and stores them in `std::vector<std::string> queryResults_`.

In Vlog (`apps/common/xanadu/zigzag/vlog.hpp:80–82, 107–111`), variable bindings are live clone rank
splices in the `ArenaManifold`. A bound value `X` is a `CellRef` to an existing or minted term,
scalar, or list in the arena. Flattening solutions immediately to `std::string` destroys cell
provenance and prevents the operator from:

1. Navigating into compound result terms or inspecting nested argument ranks on `d.grab`/`d.step`.
1. Following cell references to other entities in the arena.
1. Transcluding the query result cell into an orchestration command or another actor's input.

**Evidence:**

- `design/vagent-tui-dashboard.md:141–145`: "Typed result formatting: Green for variable bindings,
  Yellow for false., Red for errors."

- `apps/common/xanadu/vagent/tui/agent_tui.cpp:305–311`:

  ```cpp
  for (const auto &[var, val] : sol.formatted) {
    if (!line.empty()) line += ", ";
    line += var + " = " + val;
  }
  results.push_back(line);
  ```

**Fix:** Update the REPL specification to retain structured query solutions containing
variable-to-`CellRef` bindings alongside formatted strings. Allow the operator to select a result
binding and navigate directly into the bound term in the arena.

### 6. (§6 & §4.1, P22.4, P22.6) Viewport virtualization duplicates manifold ranks to side tables

**Problem:** Proposals `P22.4` and `P22.6` introduce `Viewport` offset indexing over `actorRows_`,
`cachedBeliefs_`, and `actionLog_`.

In `AgentTui::refreshActorTable()` and `AgentTui::inspectBeliefs()`, the entire collection of actors
(from `runtime_.actorPids()`) and beliefs (from `zigzag::rankAfter(arena, agentCell, dims.belief)`)
is copied into heap-allocated `std::vector<ActorRow>` and `std::vector<BeliefRecord>` structs on
every frame or selection change, regardless of how many rows fit in the visible viewport.

In ZigZag, an entity collection is an orthogonal rank. A viewport is naturally a window cursor
stepping $V$ cells posward/negward along dimension $d$. Re-copying hundreds of manifold cells and
strings into C++ side vectors on every tick burns memory and CPU cycles, violating mechanical
sympathy and ZigZag rank principles.

**Evidence:**

- `design/terminal-ui-architecture.md:293–317`: `struct Viewport { std::size_t offset; ... }`.
- `apps/common/xanadu/vagent/tui/agent_tui.cpp:177–278, 321–422`: full vector population.
- `design/vagent-tui-dashboard.md:217–218`:
  > "P22.6: Implement Viewport scrolling offsets in `AgentTui` with
  > `PageUp`/`PageDown`/`Home`/`End`."

**Fix:** Specify that viewport pagination for beliefs and manifold entities walks the `d.belief` and
`d.clause` ranks lazily from the current viewport cursor cell rather than regenerating all
`BeliefRecord`s into heap vectors on every tick. Price the allocation cost of full-vector copying
versus lazy rank walking in Section 7.

______________________________________________________________________

## MINOR

### 7. (§3.1, line 108 vs §4.2, line 136) Predicate argument syntax inconsistency

**Problem:** In Section 3.1, line 108 of the ASCII mock displays `target(pos_y, 40.0)`, while
Section 4.2, line 136 describes compound arguments as `target(x: 12.5, y: 40.0)`. The notation
between the mock and the prose is inconsistent.

**Evidence:**

- `design/vagent-tui-dashboard.md:108`: `│ target(pos_y, 40.0)  gps_mod   0.450 ...`
- `design/vagent-tui-dashboard.md:136`: `...rendering complete tuples: target(x: 12.5, y: 40.0).`

**Fix:** Harmonize the syntax across the document to match `vagent` predicate syntax (e.g.
`target(pos_y, 40.0)`).

### 8. (§3, line 109) `BorderStyle` lacks specification for T-junctions and cross intersections

**Problem:** `BorderStyle` defines `Ascii`, `UnicodeSingle`, and `UnicodeRounded`. While rounded
corners are specified (`╭, ╮, ╰, ╯`), terminal split panes (such as master-detail splits) require
T-junctions (`┬, ┴, ├, ┤`) and cross-intersections (`┼`), which are neither rounded nor explicitly
specified in the enum documentation.

**Evidence:**

- `design/terminal-ui-architecture.md:109–113`:

  ```cpp
  enum class BorderStyle {
    Ascii,          // +, -, |
    UnicodeSingle,  // ┌, ┐, └, ┘, ─, │
    UnicodeRounded  // ╭, ╮, ╰, ╯, ─, │
  };
  ```

**Fix:** Document the full 11-glyph box drawing set (horizontal, vertical, 4 corners, 4 T-junctions,
1 cross) for each `BorderStyle`.

### 9. (§8, line 228) Settings table lacks bounds for query solution count and inspect depth

**Problem:** The settings table in Section 8 lists `vagent.tui.borders`, `vagent.tui.history_size`,
and `vagent.tui.split_threshold`, but lacks a setting for maximum query solutions or maximum belief
inspection depth, which defaults to 18 in mock line 143.

**Evidence:**

- `design/vagent-tui-dashboard.md:228–232`: Settings table.
- `design/vagent-tui-dashboard.md:143`: `Solutions 1–5 of 18 (Press ';' for more)`.

**Fix:** Add `vagent.tui.max_query_solutions` (default 20) and `vagent.tui.max_inspect_depth` to the
settings table in Section 8.

### 10. (§6, line 187) Mermaid diagram references non-existent curses refresh proposal

**Problem:** The Mermaid flowchart in Section 6 contains node
`P1_2["1.2 Implement werase + wnoutrefresh + doupdate diff rendering"]`. However, Section 7
(Proposed Changes) lists `P22.1`–`P22.8`, none of which explicitly name `wnoutrefresh` or
`doupdate`, nor are they in `TerminalDevice`'s public interface in `terminal-ui-architecture.md` §3.

**Evidence:**

- `design/vagent-tui-dashboard.md:187`:
  `P1_2["1.2 Implement werase + wnoutrefresh + doupdate diff rendering"]`
- `design/terminal-ui-architecture.md:102–152`: `TerminalDevice` exposes `erase()` and `refresh()`.

**Fix:** Align the diagram node text with `TerminalDevice::refresh()` and differential damage
tracking in `VirtualScreenBuffer`.

______________________________________________________________________

VERDICT: NOT CONVERGED

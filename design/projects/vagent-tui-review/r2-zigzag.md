# Round 2 critique: Xanadu / ZigZag Purist (terminal-ui-architecture.md & vagent-tui-dashboard.md)

Paths below are relative to the repo root. Tally: 2 BLOCKING, 4 MAJOR, 3 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R1.1 (Compound argument traversal on d.grab violates 2-degree invariant): PARTLY — Corrected to
  chain along `+d.step`, but residual prose retains named record syntax for positional terms.
- R1.2 (Phantom dependency on ArenaManifold without projection interface): OPEN — Still listed in
  metadata header without any projection protocol or cell grounding in proposals P22.1–P22.4.
- R1.3 (Causal and provenance destruction via payload.toJson()): PARTLY — `toJson()` removed, but
  accesses non-existent `Envelope` fields and strips monotonic causal ordering (`ordinal`, `floor`).
- R1.4 (Omission of ZigZag traversal for P19 epistemic observations): OPEN — Completely omitted from
  v0.2 despite claim of acceptance in disposition.
- R1.5 (Deductive REPL discards cell identities and clone bindings): PARTLY — Scoped frame preserves
  turn atomicity, but pre-existing `CellRef` identities are still discarded.
- R1.6 (Viewport virtualization duplicates manifold ranks to side tables): OPEN — Deferred as
  `U-TUI-1`, but deferral premise contradicts Invariant T2 and violates Invariant T5.

______________________________________________________________________

## BLOCKING

### 1. (vagent-tui-dashboard.md §5, l. 185) HostSink accesses missing fields and breaks causality

**Problem:** In `vagent-tui-dashboard.md` §5 (lines 185–195) and Proposal P22.5,
`executor.setHostSink` attempts to format outbound actor emissions by accessing `envelope.payload`
and `envelope.topic`:

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

In `apps/common/xanadu/vagent/runtime/wire.hpp:102–117`, `struct Envelope` contains `kind`, `type`,
`from`, `ordinal`, `floor`, `fields` (`WireRecord`), `correlation`, `relaySpace`, and `relayFor`.
There is no `payload` field, no `topic` field, and no function `runtime::formatWireValue` exists in
the repository. This code will fail compilation against the engine ABI.

Furthermore, this implementation destroys Spool-Mesh causal provenance:

1. It replaces the emitter's monotonic emission sequence `envelope.ordinal` (stamped from
   `$ordinal`) and causal floor `envelope.floor` with a non-monotonic local wall-clock timestamp
   (`%H:%M:%S`), preventing causal verification and replay across distributed nodes.
1. Flattening emissions immediately into formatted strings inside an ephemeral
   `std::vector<std::string>` or ring buffer strips hyperstructural cell identity, precluding
   Nelsonian transclusion and severing integration with the `system://activity` store mandated by
   `AGENTS.md`.

**Evidence:**

- `design/vagent-tui-dashboard.md:185–195`: Non-existent `envelope.payload` and `envelope.topic`.

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

- `AGENTS.md` ("Xuzz navigation workflow"): "Completed reader transitions form branching walks in
  the private `system://activity` store... See the R8 activity-store extension in
  `store-slice-convergence.md`."

**Fix:** Refactor the emission handler to read real `Envelope` fields: `envelope.type` (the event
type), `envelope.from` (the emitter), `envelope.ordinal` (sequence), `envelope.floor` (causal
floor), and `envelope.fields` (the declared fields `WireRecord`). Store structured `ActionLogEntry`
records in `AgentTui` that preserve `(from, type, ordinal, floor, fields)`. Document how these
entries map to the activity walk in `system://activity`.

### 2. (terminal-ui-architecture.md §0, l. 11) Phantom ArenaManifold prerequisite without projection

**Problem:** `terminal-ui-architecture.md` metadata header (line 11) continues to list
`ArenaManifold` under `rests on`, and Section 1.1 line 45 asserts that monospace physics mirror
ZigZag slice projections.

Yet across all proposals (P22.1–P22.4, P22.9, P22.10) and buffer implementations, `ArenaManifold` is
never referenced, linked, or projected. `VirtualScreenBuffer` is specified strictly as a contiguous
1D flat vector of `ScreenCell` structs with no dimension projection mapping, no cell cursor
protocol, and no manifold grounding.

Claiming that a low-level curses terminal driver rests on `ArenaManifold` while leaving the
projection bridge completely unspecified creates a false architectural dependency and confuses layer
boundaries.

**Evidence:**

- `design/terminal-ui-architecture.md:9–12`:

  ```text
  | rests on | VirtualScreenBuffer, TerminalDevice, KeyEvent, Style,
               MeshActorRuntime, AgentExecutor, ArenaManifold |
  ```

- `design/terminal-ui-architecture.md:221–266`: `VirtualScreenBuffer` definition has zero manifold
  interaction.

- `design/terminal-ui-architecture.md:493–546`: Proposals P22.1–P22.10 contain no manifold
  integration.

**Fix:** Remove `ArenaManifold` (and actor runtime types `MeshActorRuntime`, `AgentExecutor`) from
`rests on` in `terminal-ui-architecture.md`. Document that `VirtualScreenBuffer` is an independent,
generic character-grid rendering sink (`apps/common/ui/tui/`). Place manifold projection
responsibility explicitly in the domain dashboard layer (`vagent-tui-dashboard.md`), specifying how
manifold cells and ranks project onto screen buffer coordinates.

______________________________________________________________________

## MAJOR

### 3. (vagent-tui-dashboard.md §4.2) Omission of multi-rank ZigZag traversal for P19 observations

**Problem:** Round 1 finding R1.4 was marked accepted in `r1-disposition.md`: "Documented
`d.epistemic` and `d.trust` traversal."

However, in `vagent-tui-dashboard.md` v0.2 §4.2, there is zero specification of the multi-rank
ZigZag traversal required to inspect P19 epistemic observations and source profiles
(`design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md` §2.1).

Without this specification, an implementer cannot know how to:

1. Walk from a belief `clause` cell to its `support` cell along `+d.source`.
1. Walk from `support` to observation records along `+d.epistemic`.
1. Walk observation field names on `+d.vars` to read values on `+d.values`
   (`kObsAcquiredUncertainty`, `kObsStatus`, `kObsIncarnation`).
1. Resolve the authentic source principal identity by following
   `profile_field --d.values--> profile_view --d.clone(neg)--> profile_master` on `$agent +d.trust`.

**Evidence:**

- `design/projects/vagent-tui-review/r1-disposition.md:209–210`: "ACCEPTED. Documented `d.epistemic`
  and `d.trust` traversal."
- `design/vagent-tui-dashboard.md:136–158`: Section 4.2 contains only Compound Argument Traversal,
  Proportional Certainty Bar, and Deductive REPL Scoped Evaluation Frame. No mention of `d.source`,
  `d.epistemic`, `d.trust`, or `d.clone`.
- `apps/common/xanadu/vagent/runtime/runtime_abi.hpp:633–647`: Profile definition on
  `$agent +d.trust`.

**Fix:** Incorporate the normative multi-rank ZigZag traversal into §4.2:

1. `clause --d.source--> support`.
1. `support --d.epistemic--> observation`.
1. Read fields via `fieldValue(arena, values, record, fieldName)`.
1. Traverse `profile` clone view negward along `d.clone` to `profile_master` on `$agent +d.trust` to
   display authenticated principal credentials and calibration state.

### 4. (terminal-ui-architecture.md §4.1, line 273) Double-width classification of █ corrupts gauges

**Problem:** Section 4.1 asserts: "When a wide Unicode character (`width == 2`, such as CJK or block
progress bars `█`) is placed at $(x, y)$, the primary cell records `width = 2`, and the adjacent
cell at $(x + 1, y)$ is populated with a placeholder continuation cell (`codepoint = U' '`,
`width = 0`)."

This is a factual typographic error. Under POSIX `wcwidth(0x2588)` and Unicode Standard Annex #11
(East Asian Width), Block Elements (U+2580..U+259F, including `█` U+2588 and `░` U+2591) are East
Asian Ambiguous / Neutral with a standard monospace display width of 1 column (`width == 1`).

If `VirtualScreenBuffer` treats `█` as `width == 2`:

1. The 10-glyph certainty gauge `[████████░░]` (12 columns) will be calculated as 20 columns (or 22
   with brackets), overflowing the 86-column right work surface and breaking split-pane table
   alignment.
1. The ASCII fallback `[========..]` is 12 columns wide. Switching between Unicode and ASCII border
   styles will cause disruptive dynamic column shifts.

**Evidence:**

- `design/terminal-ui-architecture.md:273–277`.
- `design/vagent-tui-dashboard.md:109`:
  `│ route(edge_node)     sensor_1  0.050   [█████████░] 95% curr│` (12 columns).
- `design/vagent-tui-dashboard.md:144`: ASCII fallback `[========..]` (12 columns).

**Fix:** Correct line 273 in `terminal-ui-architecture.md` §4.1: remove `block progress bars █` from
the wide character examples. Clarify that CJK ideographs and full-width forms are `width == 2`,
while block and shade elements (`█`, `░`) are single-width (`width == 1`).

### 5. (vagent-tui-dashboard.md §11 U-TUI-1) Deferral contradicts Invariant T5 and quiescence

**Problem:** Open question `U-TUI-1` in `vagent-tui-dashboard.md` §11 and `r1-disposition.md` defers
lazy manifold rank cursor stepping with the justification: "caching row views for terminal frames
avoids repeated manifold walks on 60Hz redraws."

This rationale has two critical defects:

1. It contradicts Invariant T2 and `runTuiLoop`: the terminal reactor has no 60Hz polling loop; it
   blocks in kernel `poll(2)` with 0% CPU quiescence when not dirty.
1. It directly violates Invariant T5 in `terminal-ui-architecture.md` ("Render paths must never
   iterate across unbounded data sets... fixed-bound rendering ($O(\text{viewport})$ time and
   space)").

In `apps/common/xanadu/vagent/tui/agent_tui.cpp:331–381`, every dirty frame iterates across ALL
beliefs on `zigzag::rankAfter(arena, agentCell, dims.belief)`, allocating and copying full
`std::vector<BeliefRecord>` heaps. For an agent with 10,000 beliefs, this performs an $O(N)$
allocation and rank walk on every turn.

In ZigZag, an entity collection is a rank. A viewport is naturally a cursor stepping $V$ cells
posward/negward along dimension $d$. Stepping $V$ cells ($V \approx 10..20$) from a cursor takes 20
pointer hops ($O(V)$), whereas eagerly materializing 10,000 C++ structs burns megabytes of heap
memory.

**Evidence:**

- `design/vagent-tui-dashboard.md:319–321` (`U-TUI-1`).
- `design/terminal-ui-architecture.md:93–95` (Invariant T5).
- `apps/common/xanadu/vagent/tui/agent_tui.cpp:331–381`.

**Fix:** Resolve `U-TUI-1` by specifying that `AgentTui` maintains viewport cursor `CellRef`s along
`d.belief` and `d.actor`, lazily materializing only the visible $V$ rows into the display cache
during redraw, satisfying Invariant T5.

### 6. (vagent-tui-dashboard.md §4.2) Deductive REPL severs pre-existing CellRef grounding

**Problem:** Proposal P22.7 and §4.2 establish an immediate mark/release frame (`arena.mark()` /
`arena.release()`) that successfully prevents trail pollution and preserves turn atomicity.

However, Section 4.2 states: "Solutions are pre-resolved up to `pageSize = 5` into detached string
representations before returning to the reactor loop."

While query-minted ephemeral terms must be released, variable bindings frequently bind to
*pre-existing ground cells* in the agent's arena (e.g. existing belief facts, entity nodes, or actor
pids that existed prior to `arena.mark()`).

Flattening all solutions immediately into detached strings without recording the underlying
`CellRef` (for pre-existing cells) severs Nelsonian hyperstructural provenance: the operator has no
way to select a query result and navigate into that cell in the Beliefs view, trace its orthogonal
ranks, or transclude it into an orchestration command.

**Evidence:**

- `design/vagent-tui-dashboard.md:148–158`.

- `apps/common/xanadu/vagent/tui/agent_tui.cpp:305–311`:

  ```cpp
  for (const auto &[var, val] : sol.formatted) {
    line += var + " = " + val;
  }
  results.push_back(line);
  ```

- Standing Rule 2 (`brief-common.md:47–48`): "State, structure, and metadata live on orthogonal
  ZigZag ranks (`d.*`), never in ad-hoc C++ side tables, JSON/bencode blobs stuffed into scalar cell
  text..."

**Fix:** Update `QueryPaginationState` to store structured solution bindings:

```cpp
struct SolutionBinding {
  std::string varName;
  std::string displayValue;
  std::optional<CellRef> groundCell;
};
```

For bindings to pre-existing arena cells, retain `groundCell`. In Navigation Mode, allow pressing a
key (`Enter` or `v`) on a query solution to focus that cell in the Beliefs inspector.

______________________________________________________________________

## MINOR

### 7. (vagent-tui-dashboard.md §4.2, line 142) Residual syntax inconsistency for compound arguments

**Problem:** In §3.1 line 110 (ASCII mock), compound predicate arguments are rendered as positional
tuples: `target(pos_y, 40.0)`. In §4.2 line 142 and §9 line 296, the text describes compound
arguments as `target(x: 12.5, y: 40.0)`. In the runtime ABI (`runtime_abi.hpp:13–25`), arguments
linked along `+d.step` are positional term values. Named fields are record attributes on
`d.vars`/`d.values`. Describing `+d.step` traversal as reconstructing `target(x: 12.5, y: 40.0)`
confuses positional terms with records.

**Fix:** Harmonize §4.2 line 142 and §9 line 296 to use positional term syntax:
`target(pos_y, 40.0)`.

### 8. (vagent-tui-dashboard.md §8) Settings table lacks bounds for query limit and inspect depth

**Problem:** The settings table in Section 8 lists `vagent.tui.borders`, `vagent.tui.history_size`,
`vagent.tui.split_threshold`, and `vagent.tui.max_log_entries`. However, it omits settings for
maximum query solutions (`vagent.tui.max_query_solutions`, default 20) and maximum belief inspection
depth (`vagent.tui.max_inspect_depth`), violating Standing Rule 4 ("No Arbitrary Ceilings").

**Fix:** Add `vagent.tui.max_query_solutions` (int64, default 20) and `vagent.tui.max_inspect_depth`
(int64, default 100) to the settings table in Section 8.

### 9. (terminal-ui-architecture.md §5.1) Domain types coupled into generic terminal loop signature

**Problem:** `runTuiLoop` is placed in `terminal-ui-architecture.md` §5.1, but its signature couples
to domain types: `void runTuiLoop(TerminalDevice &device, AgentExecutor &executor, AgentTui &tui)`.
Section 1.1 states that the terminal driver layer is decoupled and reusable across all CLI tools
(`vquery`, `vpl`, `vprolog`). Having `AgentExecutor` and `AgentTui` hardcoded in
`terminal-ui-architecture.md` breaks generic UI library decoupling.

**Fix:** Generalize `runTuiLoop` in `terminal-ui-architecture.md` to accept a generic
`TuiApplication` interface (exposing `isDirty()`, `render()`, `handleInput()`, `shouldExit()`) and
notification file descriptor, moving the concrete `AgentExecutor` binding to
`vagent-tui-dashboard.md` or `apps/vagent/main.cpp`.

______________________________________________________________________

VERDICT: NOT CONVERGED

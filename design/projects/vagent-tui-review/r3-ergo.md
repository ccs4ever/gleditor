# Round 3 critique: PL & Ergonomics (terminal-ui-architecture.md & vagent-tui-dashboard.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 5 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R1.1 (Submit in Edit Mode exits to Nav Mode): RESOLVED — Invariant T4 and §7.1 specify that
  `Enter` retains `EDIT MODE`.
- R1.2 / R2.M1 (QueryPaginationState caching & ';' collision): RESOLVED — §4.2 specifies
  pre-resolving up to `maxQuerySolutions = 50` solutions into an in-memory `solutionBuffer_` within
  an immediate mark/release frame (`Mark m = arena.mark(); ... arena.release(m);`). This guarantees
  zero outstanding marks on the worker arena, eliminating compaction locks and choice-point
  destruction. Paging through `solutionBuffer_` via `QueryPaginationState` (`windowOffset`,
  `pageSize = 5`) advances purely in memory with 0% CPU; pressing `;` in Navigation Mode is isolated
  from text entry.
- R1.3 (Terminal flow control hazard in [^Q] Quit): RESOLVED — `[^Q]` replaced by `q` in Navigation
  Mode and `Ctrl+C`.
- R1.4 (Collision between [F1] Registry and Help): RESOLVED — Help assigned to `F10` / `?`; `F1`
  dedicated to Actor Registry.
- R1.5 / R2.M2 (Unspecified widget focus ring and Tab traversal): RESOLVED / DOWNGRADED — R2
  disposition settled the canonical 3-tier cycle (`ActorRegistryTable` →
  `Right Context / Work Surface` → `Active Input Box` → `ActorRegistryTable`); incorporation into
  body prose noted in MINOR 1.
- R1.6 (Missing mode indicator in UI chrome): RESOLVED — `[NAV]` / `[EDIT]` badge added to status
  line and §7.1.
- R1.7 (upgrade-check command undefined in C++): RESOLVED — Consolidated into
  `upgrade <target> <programRef> [--check]`.
- R1.8 (advance command silently ignores duration): RESOLVED — Simulated clock advance specified in
  §4.3 and reactor loop.
- R1.9 / R2.M4 (Opaque command execution feedback and error diagnostics): RESOLVED / DOWNGRADED —
  Action log ring buffer bounded at 500 entries (`P22.5`); lease refresh preventing
  `ActorError::NotAuthorized` during manual stepping documented in §4.3; standard output styling
  noted in MINOR.
- R2.M3 (Missing command grammar EBNF and payload tokenization): RESOLVED — Formal EBNF grammar
  added in `vagent-tui-dashboard.md` §4.3; `<jsonPayload>` replaced with `( FieldEntry )*`
  (`Ident "=" ...`), eliminating whitespace splitting bugs and aligning with `parseFields` in
  `agent_tui.cpp`. Syntactic precision refinements noted in MINOR 2.

______________________________________________________________________

## BLOCKING

None.

______________________________________________________________________

## MAJOR

None.

______________________________________________________________________

## MINOR

### 1. (terminal-ui-architecture.md §7.1, line 448) Unspecified widget focus ring in text

Full title: Incorporate widget focus ring and Tab traversal hierarchy into specification text

**Problem:** The R2 disposition accepted finding M2 and established the canonical 3-tier cyclic
focus ring (`ActorRegistryTable` $\to$ `Right Context / Work Surface` $\to$ `Active Input Box` $\to$
`ActorRegistryTable`). While `terminal-ui-architecture.md` §7.1 notes that `Tab` cycles focus away
from the input widget and the status line shows `[Tab] Focus`, neither document's body text formally
incorporates this traversal ring or defines the behavior when `Tab` lands on an input field.

**Fix:** In `terminal-ui-architecture.md` §7.1 and `vagent-tui-dashboard.md` §3.1, explicitly
document:

1. **Focus Ring Order**: `Tab` cycles focus clockwise across active widgets:
   - Stacked Tab View: Active Pane Content $\leftrightarrow$ Pane Input Box.
   - Master-Detail Split View: `ActorRegistryTable` $\to$ `BeliefsTable` / `ActionLog` $\to$
     `VlogQueryInput` / `OrchestratorCommandInput` $\to$ `ActorRegistryTable`.
1. **Focus Activation**: Pressing `Tab` while in `NAVIGATION MODE` moves the active selection
   pointer / border highlight to the target widget without automatically entering `EDIT MODE`. The
   user enters `EDIT MODE` only upon typing a printable character or pressing `Enter`.

### 2. (vagent-tui-dashboard.md §4.3, line 170) EBNF target syntax and duration unit precision

Full title: EBNF target syntax, duration units, and field entry precision

**Problem:** The formal EBNF command grammar in `vagent-tui-dashboard.md` §4.3 specifies targets as
`Ident` and `AdvanceCmd` as `"advance" Integer`:

```ebnf
SpawnCmd      ::= "spawn" Ident Ident ;
PerceptCmd    ::= "percept" Ident Ident ( FieldEntry )* ;
SendCmd       ::= "send" Ident Ident ( FieldEntry )* ;
FieldEntry    ::= Ident "=" ( StringLiteral | NumberLiteral | BoolLiteral ) ;
AdvanceCmd    ::= "advance" Integer ;
```

This exhibits four small syntactic discrepancies with the underlying implementation in
`apps/common/xanadu/vagent/tui/agent_tui.cpp`:

1. `resolveTarget()` (lines 574–584) supports `@selected`, numeric short PIDs (`001`), and scoped
   PIDs (`node-a:002:echo_agent`), none of which match standard `Ident` rules.
1. `advance` (lines 637–644) and the §4.3 semantics accept duration units (`500ms`, `2s`), but the
   grammar only permits unadorned `Integer`.
1. `spawn` (lines 595–598) parses optional initial field entries (`parseFields(tokens, 3)`), but
   `SpawnCmd` in the EBNF omits `[ ( FieldEntry )* ]`.
1. `parseWireValue()` (lines 70–72) falls back to unquoted text, so bare identifier values like
   `status=ok` or `node=edge_node` are accepted, whereas `FieldEntry` requires a literal.

**Fix:** Refine the EBNF productions in §4.3:

```ebnf
Command       ::= SpawnCmd | PerceptCmd | SendCmd | StepCmd | AdvanceCmd | UpgradeCmd | StopCmd ;
SpawnCmd      ::= "spawn" AgentType Space AgentName [ Space FieldList ] ;
PerceptCmd    ::= "percept" Space Target Space PerceptType [ Space FieldList ] ;
SendCmd       ::= "send" Space Target Space EventType [ Space FieldList ] ;
StepCmd       ::= "step" [ Space TurnCount ] ;
AdvanceCmd    ::= "advance" Space Duration ;
UpgradeCmd    ::= "upgrade" Space Target Space ProgramRef [ Space "--check" ] ;
StopCmd       ::= "stop" Space Target ;

FieldList     ::= FieldEntry ( Space FieldEntry )* ;
FieldEntry    ::= Ident "=" ( Ident | StringLiteral | NumberLiteral | BoolLiteral ) ;
Target        ::= "@selected" | Ident | ShortPid | FullPid ;
Duration      ::= Integer ( "ms" | "s" )? ;
TurnCount     ::= Integer ;
```

### 3. (vagent-tui-dashboard.md §3.1, line 121) Mode-aware status bar dynamic rendering mockup

**Problem:** In `vagent-tui-dashboard.md` §3.1, the ASCII dashboard mockup illustrates only the
`[NAV]` mode status bar:

```
 [NAV] [F1] Registry [F2] Query [F3] Orchestrate │ [Tab] Focus │ [↑↓] Select │ [s] Step │ [F10] Help
```

While Invariant T4 and §7.1 mandate that `[EDIT]` mode activates when typing in input boxes, the
dashboard documentation does not illustrate the dynamic status bar prompt presented during text
editing.

**Fix:** Add the complementary `[EDIT]` mode status bar mockup to §3.1:

```
 [EDIT] [Enter] Execute │ [Esc] Exit to Nav │ [↑↓] History │ [←→] Edit │ [Tab] Next Widget
```

### 4. (vagent-tui-dashboard.md §3.1, line 116) REPL solution pagination prompt

Full title: REPL solution pagination footer affordance

**Problem:** In `vagent-tui-dashboard.md` §4.2 line 163, the document specifies: "Pressing ';' in
Navigation Mode advances `windowOffset` by 5 without touching the worker arena." However, under
Invariant T4, an operator who just submitted a query remains in `EDIT MODE`. If they press `;`
immediately, a semicolon is inserted into `queryInput_`. Without a visual affordance, operators may
not realize they must press `Esc` to return to Navigation Mode before pressing `;`.

**Fix:** In §3.1 and §4.2, specify that when `paginationState.hasMore()` is true, the REPL results
footer displays an explicit pagination guide:
`│ Results: X = edge_node  [Page 1/4: Press Esc then ';' for more]   │`

### 5. (vagent-tui-dashboard.md §3.1, line 116) Multiline solution formatting in split view

**Problem:** In `vagent-tui-dashboard.md` §3.1 line 116, the REPL mockup shows a single-variable
binding: `Results: X = edge_node`. For deductive queries binding multiple variables or compound
structures (`?- route(Start, End, Hops, Cost).`), displaying all bindings on a single line risks
truncation or unreadable horizontal clipping in Master-Detail Split View (~72 columns in the right
column).

**Fix:** Specify that multiple variable bindings wrap across indented subsequent lines in the REPL
result viewport, and that the result container supports vertical scrolling via `replScrollOffset_`
managed by a `Viewport` instance (`terminal-ui-architecture.md` §6).

______________________________________________________________________

VERDICT: CONVERGED

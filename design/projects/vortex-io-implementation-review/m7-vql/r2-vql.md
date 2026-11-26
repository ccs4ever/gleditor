# Round 2 critique: VQL Expert (M7-VQL gate), target be30c946

Tally: BLOCKING 0, MAJOR 1 (N1), MINOR 2. Read-only static review; nothing was built or run.

## r1 status

- r1 M1 (persistence): FIXED. `-o`, `--in-place` (batch, `main.cpp:668-672`, before any promote or
  write) and `:save` (`:842`) all refuse through `refuseDurableAction` with
  `VQL_IO_PERSIST_NEEDS_CAPTURE`, typed on stderr, nothing written; `runQuery` resets
  `resultsFromAction` on every successful query, so a later plain query is savable. A script whose
  final statement is not an action cannot hold action rows (later statements cannot name the action
  value), so the managed outcome cell never reaches `promote`/`resultRows`; the untested promote
  behaviour is moot. `vqueryc` only compiles and exports bytecode (no bytes, no capture) and has no
  persistence of results; acceptable (MINOR 1). Library persisters (`promoteAndAttachToStore`) take
  compiled programs, not runner rows.
- r1 MINORs: fixed ones verified (def refusal at `parser.cpp:398`, UTF-8 check correct at the buffer
  end, `:open` rest-of-line). Skipped ones (statement index, bindFile deadline, examples doc, stale
  `lastResults` after a failed later script) are not MAJOR.

## MAJOR

### N1. The reclaim classification ignores dynamic-view registry entries

`execution.cpp:632-637` keeps the program only when `sessionBindings` is non-empty; the comment
above it says "published bindings and dynamic views". Dynamic shape projections, `lens(...)` values
and shape values mint plain program cells (`compiler.cpp:861-869`, `compile_dynamic_view.cpp:75,95`)
and register them in the session registry (`compilation.dynamicViews = &session_->dynamicViews()`,
`execution.cpp:435`) with no `let`, so `sessionBindings` is empty.

Scenario: `effect { ... }` followed by a statement with a dynamic shape projection or a bare
`lens(get:..., put:...)`. `published` is false, `reclaimOnClose(cells)` keeps only the rows and what
they reach by links; the shape/lens handle cells and any view cell not in a row are freed with
Repair while `shapeHandles`/`cellLenses`/`viewCells` still name them. The slot is reused (P7's
bounded-arena tests rely on reuse), so a later query's `shapeHandles_.contains(cell)`
(`shape_eval.cpp:50,76,110`) or `viewCells` lookup (`dynamic_view.cpp:114`) can match an unrelated
cell. Fix: also keep the program (or erase those registry entries on reclaim) when the registry grew
during the run, the same predicate `noDynamicViewsChanged` already computes at `:611-614`; add a
regression with an effect script plus a lens/pivot statement followed by a plain query.

## MINOR

1. `vqueryc` accepts an effect script and exports its Submit bytecode with no note; harmless, but
   say so or refuse with `VQL_IO_QUERY_CONTEXT` there (`apps/vqueryc/main.cpp:108-155`).
1. `rolledBack` returns early when `current_ == nullptr` (close-time `abortScopedChoices`); this is
   safe only because every failure path then calls `retireAll(activation->admitted)` and `retire`
   orphans a rolled-back request. Add a one-line comment so the coupling survives refactors.

Checked and sound: per-activation `admitted`/`finished_`/`latest_` (retire-all on admit matches the
documented "until next admission" lifetime), `Stepping` nesting, destructor retiring live
activations, host-allocation exclusion so service-owned request cells are not freed, row keep-set
(rows plus link reach within the region), and the outcome/payload row built after `close()`.

VERDICT: NOT CONVERGED (one MAJOR, N1; the persistence fix is approved).

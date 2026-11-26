# M7-VQL gate, round 3: VQL expert (focused)

Target `183129f5`. Read-only static review. Tally: BLOCKING 0, MAJOR 0, MINOR 2.

## r2 N1: FIXED

- Soundness of the size comparison (`execution.cpp:495-499`): nothing erases from `viewCells`,
  `shapeHandles` or `cellLenses` except `DynamicViewRegistry::clear()` (`vql_engine.cpp:128`), so
  within one run they cannot shrink; "grow and shrink to equal" does not occur. Every insertion that
  names a program cell is keyed by a freshly minted cell (`compiler.cpp:869`,
  `compile_projection.cpp:274`, `compile_steps.cpp:332`, `compile_dynamic_view.cpp:77,95`), so it
  grows the map. `ownerViews` is always set beside a `viewCells` insert
  (`compile_projection.cpp:274-275`), and `namedLenses` copies a `cellLenses` record under a `let`,
  which `sessionBindings` already covers. `handles[...] =` at `compile_steps.cpp:332` mutates a
  shared record but is paired with a new `viewCells` key. Interleaved scripts only make the check
  more conservative.
- Replace-under-existing-key is possible only with a stale key (MINOR 1).
- No other session registry names program cells: watches, mounts and functors hold paths and names,
  not cells.

## MINOR

1. Stale registry keys make the size guard unsound in one case. Nothing truncates the registry when
   a run's cells are rolled back. A compile failure after a registration, or a runtime failure with
   an unchanged trail, goes through `release(preCompileMark)` (`execution.cpp:~470`, `:588`) and
   leaves keys naming freed slots. Slot reuse can then make a later `reg.viewCells[K] = ...`
   overwrite that key with no size change, so `keepIfNamed` is false and the reclaim frees a cell
   the registry names. Pre-existing, not N1. Fix: truncate the registry to the pre-compile sizes
   wherever the pre-compile mark is released. Not blocking.
1. The tests do not prove the fix. Per the fixer, three of four pass with the guard disabled,
   because a pivot or lens result is a row and the row keeps its cell. A proving test registers a
   view, shape or lens in an effect script whose rows do not reach it (a final scalar or `count`
   statement), then asserts every key in the three maps still satisfies `arena.contains`, and that a
   following plain script does not take those slots. Only
   `AnAbandonedScriptWithARegisteredViewKeepsItsCells` bites the guard today.

## Other diff checks

- `vqueryc` effect note (`apps/vqueryc/main.cpp`): stderr only, bytecode export unchanged. Fine.
- `rolledBack` across activations: searches the current activation, then all live ones; a miss is a
  DEBUG log. Orphaning is idempotent through `OrphanResult`. `orphan()` with `HasWaiters` drops the
  parked route and retries once, which matches the failure-path `retireAll`. No script-semantics
  regression found.
- `clearCursorState` is guarded by `arena.contains`. Fine.

VERDICT: APPROVED (converged: zero BLOCKING, zero MAJOR).

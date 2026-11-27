# Round 3 critique: Vortex Expert (M7-VQL-d code)

Target 57be2657 (fixes since round 2: 0fa2c336..57be2657). Tally: 0 BLOCKING, 2 MAJOR, 2 MINOR.
Focused round. Headless; probes built outside the repo (scratch `r3/probe3.cpp`, linked against the
repo objects), nothing in the tree changed.

## Previous findings

- **r2 M1 (join providers unregistered under a parked script): RESOLVED.** The undo is gone
  (`execution.cpp:522-532`); each begin re-registers on the shared VM, and the providers capture the
  VM reference and the walk only, never the executor, so a registration by an executor that has
  since died is safe. The walk is reset at position 0 and taken by `join_end`, and no park falls
  inside it. `VqlIoJoinTest.AParkedScriptKeepsItsJoinWhenAnotherScriptRunsAndEnds` pins it.
  `prose_hits` keeps the old shape (older code, noted in r2, not graded).
- **r2 minor 1 (handle held by another parked activation): FIXED as code, unreachable as a
  scenario.** `ownOutcome` now searches `live_` (`script_runner.cpp:1421-1429`). No test exists
  (the disposition says so); see MINOR 1.

## Checks in the lane that held

- No core file moved: `git diff 0e6a672d..57be2657 -- apps/common/xanadu/vortex apps/common/xanadu/zigzag`
  touches `capture_host`, `frontend_diagnostics`, `mesh_evaluator`, `owned_outcome` only;
  `VortexVM::closureValue` and `makeClosureValue` date from `df765484`, `closureValue` is `const`
  and public. VM and core neutrality hold.
- `closureEnvironments` enumerates through the public read API, follows captures with a `seen` set
  (cycles cannot loop), skips non-closures through the typed refusal, and only runs when the program
  compiled a lambda (`nextClosureLambdaId_`) or the session holds a closure. Closure values are only
  created by the lambda opcode, so there is no other source to miss. `heldByClosures` prunes to
  closures a binding or row reaches, with an incarnation check.
- Parked activations, the VM's own state: a parked A survives 50 rounds of plain B queries (join, a
  let, a lambda, a `%` on `d.1`) with its let and its read outcome intact, and a parked A holding a
  session closure keeps reading what the closure captured while B rebinds the captured name 20 times
  (`ParkedSurvivesPlainReclaims`, `ParkedWithClosureLetAndPlainClosureRuns`). The reason is sound:
  the region is the incarnation windows of B's own steps, the VM maps are keyed by the cells of the
  run that owns them and `pruneAssemblyState` runs after the delete, d.cursors neighbours are
  repaired by the deletion (P7), and A's mark was discarded at its park, so B's mark is the only
  one outstanding and a release or discard cannot cross A's cells. `compact()` is not involved;
  the reclaim is `deleteCell` only.
- `%` on `d.clone`, `d.name`, `d.vars`, `d.values`, `d.role`, `d.cache`, `d.stores`, `d.1` is kept
  by a following run in a plain query (`PercentOnSystemDims`): the system-dimension exception does
  not drop a user's structure in any case I could build.
- The 10^4 pins are honest for what they measure: the arena's `liveCells` and slot count after a
  warm-up, rows freed by the caller as a REPL would, three shapes. They also hold with a closure
  bound in the session (`BoundedWithAClosureInTheSession`, 2000 runs, mine). They say nothing about
  a script parked in between or about redefining a closure (M2).

## BLOCKING

None.

## MAJOR

### M1. A rebind by one script frees a cell a parked script still reads

**Problem.** `publishCompilerBindings` and `releaseOwned` free the cell a replaced `let` held when
no binding, row, mount, registry entry or closure names it and it has no link (`freeable`,
`vql_engine.cpp`). A script parked at an `await` is none of those: its compile-time bindings put the
session cell into the activation's slot, and nothing reports that to the engine. Section 8c's claim
that a freed cell is "unreachable from every root, by construction" is false for a live activation,
and the disposition's reason ("names are not visible to each other until one ends") covers a
parked script's own `let`s, not the session names it read when it began.

**Failure scenario.** `let $t := "abc"; return $t` (published, owned). Script A:
`effect { ... await($q) } as $r; return $t` parks. Script B: `let $t := "N"; return $t` completes and
frees the old cell. A resumes and returns an empty text instead of `abc`; with slot reuse it could
as easily return another value. Reproduced: `R3Probe.Rebind` (`ValueWorld`, two `run`s then
`finish`) gives `{""}` where the control that rebinds a different name (`Control_NoRebind`) gives
`{"abc"}`. `:forget` and `clearVariables` share the path.

**Evidence.** `vql_engine.cpp` `freeable` / `publishCompilerBindings` / `releaseOwned`
(roots: env_, ownedLets_, views, mounts, closures, `keep`), `script_runner.cpp` `live_` (several live
activations on one VM). Not new in this round (the free dates from round 1) and `freeable` was
rewritten without it, but round 3's redesign is exactly the "unreachable from every root" claim.

**Fix.** Make a live activation a root: while any activation is parked, either hold the cells of
its bindings (the runner already owns the activation list and can hand the engine the session cells
each parked script was compiled against) or queue the free until no activation is parked. Add the
two-script test above.

### M2. Redefining a closure leaks its whole program on every run

**Problem.** A program whose results or bindings hold a closure keeps its whole region
(`keepCompiledProgram`, `execution.cpp:~825`), because the closure's descriptor and frames are raw
references into it. When the closure is later replaced, dropped or `:forget`ten, nothing frees that
region: `closureEnvs_` records captured cells, not the region, and the table entry is simply
pruned. 8c lists the price as "a cell a live closure captured is not reclaimed until the closure is
dropped", which understates it: the program region is never reclaimed, and the growth is per run
with no bound, in an ordinary REPL pattern (redefine `$g`).

**Failure scenario.** `let $g := \($x) $x + "!"; return $g("a")` run 1000 times (rows freed by the
caller): live cells 1370 after warm-up, 167370 after 1000 runs, about 166 cells per run, unbounded.
Reproduced: `R3Probe.BoundedRedefiningAClosure`. The same figure applies to any plain program whose
result list reaches a closure, and the 10^4 pins do not include one.

**Fix.** Record the kept region per closure root in the engine (the cells the run allocated, by
incarnation) and free it, around whatever a binding or row still reaches, when the closure leaves
the reachable table; or state the unbounded price in the design and the pins as a refusal-free
known limit (not recommended: it is the leak this slice set out to remove).

## MINOR

1. **The `live_` search in `ownOutcome` is dead and, if reached, retires another activation's
   request.** The other activation's names are not visible while it is parked, so the shape cannot
   be written (the disposition says so), but the code, if ever reached, erases the entry from the
   parked activation's own `admitted` list and retires its request early, so that script's own
   later `await` of its handle finds nothing. Either delete it and refuse the await-as with a typed
   code, or keep the entry and copy.
2. **The registry-changed test is a count comparison** (`keepIfNamed`, `execution.cpp`): while a
   script is parked, another that removes one registry entry while this one adds one leaves the
   counts equal and the parked script's registered cell is reclaimed. Needs `dropFreed` and a
   registering script to interleave; narrow. Compare the registry's cells against the region (as
   the roots do) instead.

Observation, not graded: a second script is refused with `VQL_SCRIPT_FAILED: Vortex VM supports one
active stream cursor at a time` when it starts while a script that holds a `let` is parked, so the
interleavings above are narrower than the runner's `live_` list suggests; the refusal is typed.

VERDICT: NOT CONVERGED

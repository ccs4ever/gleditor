# Round 2 critique: Xanadu/ZigZag Purist (M7-VQL-c code)

Target `cb194c92`. Tally: 0 BLOCKING, 1 MAJOR, 3 MINOR.

Run against a `build/vquery` rebuilt at `cb194c92` (the earlier binary pre-dated the commit), headless,
XDG redirected into `/tmp/claude-1000/zz2`.

## Status of round 1 findings

- **M1, stale session-variable aliasing: PARTLY.** The round 1 scenario is fixed: two named reads in
  two scripts, then `return io_tag($a)`, `io_bytes($a)`, `io_payload($a)` answer
  `VQL_IO_STALE_BINDING` (typed, no `RelationNext expects ...` text). A stale receipt `$c1` after a
  second capture answers the same typed code for `io_captured` and `io_tag`. A script that fails
  or is abandoned publishes no variables (`vql_engine.cpp:157-173` swaps `env_` only on success), so
  there is nothing to track there. One route to the same aliasing remains: see N1.
- **M2, transient bytes persisted from the REPL: RESOLVED.** Round 1 scenario
  (`effect{read A} as $a; let $t := io_payload($a); return $t;` then `return $t;` then `:save`) now
  ends `VQL_IO_PERSIST_NEEDS_CAPTURE` on the `:save`, and `active.primedia` stays 0 bytes
  (it was 137 to 200). A later `:save` after a further script is refused the same way. Captured
  cells (`c1`, `c2`) append as before, and only then.

## BLOCKING

None.

## MAJOR

### N1. A variable copied from a handle by a plain `let` is not tracked, and still aliases the cell that reuses the slot

**Problem.** `noteBindings()` is called only from `finish()` (`script_runner.cpp:1787`), so `bound_`
holds exactly the variables that existed when a script last completed. A variable created afterwards
by a plain query is invisible to `reconcileBindings()` (`:1711-1731`), which only rewrites names
present in `bound_`. The freed slot is reused, and the copy names the stranger.

**Failure scenario** (reproduced):

```text
vql> effect { ...read A (0,4)... } as $ra; return io_tag($ra);        Complete
vql> let $y := $ra; return io_tag($y);                                 Complete
vql> effect { ...read B (500,4)... } as $rb; return io_tag($rb);       EndOfStream
vql> return io_tag($y);                                                EndOfStream   <- $y was Complete
vql> return io_tag($ra);       VQL_IO_STALE_BINDING (correct)
```

`$y` reports script 2's outcome. `io_bytes($y)` answered
`VQL_IO_STALE_BINDING: the bytes belonged to the script...` for the wrong record in a variant, and
`capture($y)` would take whatever outcome occupies the slot. This is round 1 M1 reached through an
ordinary one-line `let`, with the same silent wrong value.

**Evidence.** `script_runner.cpp:1787` (only call of `noteBindings`), `:1711-1731`
(`reconcileBindings` iterates `bound_` only), `:763, :861, :996, :1383` (the reconcile sites, each
preceded by `freeFinishedRecords()`), `vql_engine.cpp:413` (`env_[let.varName] = cells`).

**Fix.** Snapshot before the free, not after the script: call `noteBindings()` at the head of
`reconcileBindings()` callers' sequence (before `retireAll`/`freeFinishedRecords` at each of the four
sites, and in `run`/`resume`), so every variable naming a live I/O handle has its incarnation
recorded while the handle is still live. `noteBindings` already keeps the first-seen incarnation.
Add the test: `let $y := $ra` in a plain line, a second admitting script, `io_tag($y)`. No core
change.

## MINOR

### m1. A stale name given to `capture` answers a `Rejected`/`Unresolved` outcome, not the typed stale code

After a script that read and bound `$ra`, `effect { capture($ra) } as $c3; return io_tag($c3),
io_error($c3)` in the next script prints `Rejected`, `Unresolved`. The wrapper at `:578-584` types
only the relations; the capture action reads the marker as a text cell. No aliasing, but the author
is told nothing about why. `capture` should test the marker the way the readers do
(`VQL_IO_STALE_BINDING`). The consequence the owner accepted (cross-script capture is impossible
once the next admission frees the record) is correct as stated; the message is what is missing.

### m2. The stale marker is an in-band text cell, not an I/O cell

Round 1 asked whether one marker cell fits cells-and-dimensions purity and whether incarnation checks
need a core change. Answer: no core change (`arena.incarnationOf`/`matches` exist), and one runner-owned
cell is the cheapest honest design for a cell-only program interface (the alternative, an empty
binding, loses the diagnostic: round 1's "needle evaluated to empty"). Its blemish is the identity:
it is a plain text cell `stale handle` recognised by `CellRef` equality (`script_runner.cpp:182, :582`),
so `return $ra` prints and `:save` of it writes that text (known, not re-reported). A purer form is a
marker that is an I/O cell of its own tag, so `io_tag($ra)` could answer `Stale` and the rest of the
ioCellTag schema discriminates it with no pointer comparison. Not required for convergence.

### m3. Deferred round 1 MINORs: reasoning checked

m2 (observation as cells), m3 (one reachable-graph walk) and m6 (clone-rank assert) are fairly
deferred as design or refactor decisions. m4 (`--in-place` piece order) stands as a documented
limitation to write into the contract. m5 (two "captured cell" predicates) is described as "not a
defect": a non-local span with no quoted store reaching `writeResultSlice` throws an uncaught
`invalid_argument` (`result_slice.cpp:19-26`); I could not reach it from the REPL this round, so it
is unverified rather than upgraded. m1's collision fix (`owned_` release) is correct on reading.

VERDICT: NOT CONVERGED

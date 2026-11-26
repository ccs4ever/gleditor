# Round 3 critique: Vortex Expert (M7-VQL-c code)

Target `e2b23837` (round 2 fixes `cb194c92..e2b23837`). Tally: 0 BLOCKING, 0 MAJOR, 2 MINOR.

`build/vquery` and `build/xuzz_test` were current for the sources (make had nothing to rebuild). Ran
headless: the 50 `*Stale*`, `*Forget*` and `IoCellsTest` cases pass. I then drove `build/vquery` by
hand; no code or test was changed.

## Status of round 2

- M1 (stale marker used as write source): RESOLVED. The marker is now an `IoCellTag::StaleBinding`
  cell (`io_cells.cpp:775-780`, `makeStaleBindingCell`), tested by tag (`isStaleBinding`) at the
  write source (`sourceBytes`, `script_runner.cpp:1658-1660`, and the script pre-flight
  `staleName`), the capability name, the capture variable, `number()` and every provider. Driven by
  hand: `let $y := $a` then a later script rebinds, `$y` goes stale; a write script naming `$y`
  answers `VQL_IO_STALE_BINDING: $y: ...` and creates no file. The marker has a tag that is not
  `Outcome`, so a persisted row naming it takes the `VQL_IO_PERSIST_HANDLE` road.
- m1 (stale capability name generic message): RESOLVED (`capability()` calls `staleName` first).
- m2 (marker minted before the constructor can throw): RESOLVED (`mintStaleMarker` checks the Submit
  hook first).

## Checked for regressions, clean

- `namesFreedHandle` and `noteBindings`: every free of a record the session can name happens in
  `retireFinished()` (which now notes first, reconciles after) or in `endActivation`/timeout paths
  whose own `as` bindings the engine rolled back; `admit` reconciles before it allocates, so a
  reused slot is never mistaken for an unreconciled stale entry (no false refusal of a fresh
  handle). `noteBindings` keeps the first-seen incarnation by cell, so a stranger in a reused slot
  is not adopted. Hand run of read `$a`, `let $y := $a`, read `$a`, read `$b`: `$y` and the old `$a`
  stale, new `$a` and `$b` `Complete`.
- Destructor order (`~ScriptRunner`): records freed, then `reconcileBindings`, then
  `forgetStaleVariables`, then the marker; no name outlives a cell it names.
- `forget`/`eraseVariable` against parked activations, leases, owned records and request slots:
  `forget` touches only `exposures_`, `session_.variables()` and nothing the host holds; handles
  (capabilities, outcomes, receipts) are kept and a payload read through a surviving outcome after
  the script is a typed refusal (`io_payload: the bytes belonged to the script...`), so no text
  re-enters unexposed. A variable `forget` erases leaves a `bound_` entry that the next
  `noteBindings` rebuilds, and `reconcileBindings` over an erased name rebinds nothing. Hand run:
  `:forget` then `return io_payload($r)` is refused, `:save` after it is refused with no rows, and a
  second `:forget` says `Nothing to forget`.

## BLOCKING

None.

## MAJOR

None.

## MINOR

### m1. `noteBindings()` now runs on every relation provider call

`installProviders` calls `noteBindings()` at the head of every wrapped provider
(`script_runner.cpp:~640`), which rebuilds a `std::map` of vectors over all session variables with
an arena lookup per cell. A script that calls `io_tag`, `io_payload` and the like once per row over
a large set is now O(rows x variables) and allocates in the loop. Fix: record only when the variable
set changed (a generation counter on `VQLEngine::bindName`/`eraseVariable`), or note once per script
entry and after plain lines, which the other call sites already do.

### m2. `eraseVariable` leaves `putGetLenses_` for the erased name

`VQLEngine::eraseVariable` erases `env_` only (`vql_engine.cpp:124`), whereas `bindName` also erases
`putGetLenses_` and `functors_`. `forget` only reaches names in `env_`, so a functor or lens that
closes over uncaptured text is outside the owner's `:forget` contract as worded ("every variable
that is not a runtime handle"). I could not construct a leak (the one functor form I tried failed to
compile for an unrelated reason), so this is stated as unverified: confirm that no functor or lens
step embeds a payload cell, or erase those maps in `eraseVariable` too.

VERDICT: CONVERGED

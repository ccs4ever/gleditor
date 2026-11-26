# Round 1 critique: Vortex Expert (M7-VQL-d code)

Target 301b9a8d (diff 0e6a672d..301b9a8d). Tally: 0 BLOCKING, 2 MAJOR, 3 MINOR.

Checked and found sound: no core file changed (`vortex_core`, `vortex_vm`, `arena_manifold`, `store*`
are absent from the diff, so the VM stays language-neutral); `io_match_tag` lowers to one
`RelationNext`, one `Eq` and `BranchFalseCell` per arm, with no new opcode and no `Submit`; its
`ioActions_` site has no await opcode, so `execution.cpp:598` never parks on it and `:627` is
guarded by `failingClause`; a match after a parked read resumes the same cursor (no resubmit; tests
count one submission); `OwnedOutcome::summary` rolls back with `release(mark)` on refusal or throw
and the capture fallback catches both; timeout/abandon (`abandonAll`, `releaseRecords`) and
`~ScriptRunner` still retire the request that `retireRequest = false` leaves admitted. All 248
`VqlIo*` tests pass headless. Scratch probes were compiled outside the repo (`/tmp/claude-1000/r1v`).

## BLOCKING

None.

## MAJOR

### M1. `await($h) as $r` is an owned record only when `$h` was admitted by the same script

**Problem.** `ownOutcome` finds its entry in `current_->admitted` only (`script_runner.cpp:1383-1393`)
and otherwise answers `keep`. A handle bound by an earlier script (a detached read still running when
that script ended) sits in `finished_`, not `current_->admitted`, so the copy is silently skipped and
`$r` is the service's own outcome. Design §2 a2 and the roadmap say the name is a script-owned record.

**Failure scenario.** Script 1: a detached read `as $h`, which completes while the read still runs.
Script 2: `effect { await($h) } as $r; return io_tag($r)` finishes the read. `ownedRecordCount()` is 0,
so `$r` is the service's outcome cell. The next admission retires the finished request and frees that
outcome. Script 3, `detached ... as $g; return io_tag($r)`, then fails with the untyped
`VQL_SCRIPT_FAILED: RelationNext: io_tag: the argument is not an outcome`. A slot is reused, and no
typed stale refusal is given. Confirmed with a scratch gtest: `ownedRecordCount()` was 0 where 1 was
expected, and the script 3 failure text was as above. Every M7-VQL-d await test awaits within the
script that started the handle (`vql_io_values_test.cpp:571-684`), so none catches it.

**Evidence.** `script_runner.cpp:1383-1393` (entry lookup), `:343`/`:1809-1818` (`finished_` handling),
`vql_io_values_test.cpp:571`.

**Fix.** Look the entry up in `finished_` as well. A copy needs only the request's outcome, so
`retireRequest = false` leaves the entry where it is. Or refuse `await($h) as $r` on a handle this
script did not admit, with a typed code. Add a cross-script test.

### M2. A refused `await($h) as $r` leaks the Rejected outcome graph, and nothing pins it

**Problem.** The refusal-graph free in `ownOutcome` sits after the `if (!retireRequest) return`
(`script_runner.cpp:1448-1460`). The refused entry then stays admitted, and at script end `retire()`
takes its request-less branch (`:437-448`): `bridge.retire` only frees the submission cell and its
occurrence and view (`io_cells.cpp:873-903`). The Rejected outcome's field/state/progress cells go
nowhere, since `freeOutcomeGraph` is called nowhere else for it. The comment at `:1455` says exactly
this: the refusal has no request whose reaping takes its outcome along.

**Failure scenario.** With `refuseAdmission = true`, a script of detached read `as $h; await($h) as $r;
return io_tag($r)` run 1000 times grows `arena.liveCells()` by about 6000 (about 6 cells a run, one of
them the returned row the harness did not free). The plain detached refusal grows by 5 cells a run. A
host that returns a refusal value from its write bound or budget (`staged()` refusals, `BudgetExceeded`)
can hit this in a loop. Confirmed with a scratch gtest. The named-read path frees the same graph
explicitly and has a leak pin (`vql_io_owned_record_test.cpp:472`). The new await-as path has none for
refusals: its refusal test (`vql_io_values_test.cpp:635`) checks tags only, and its leak pin
(`:661-684`) never refuses. The 5-cell leak itself also exists for plain detached refusals before this
slice, so a root-cause fix belongs in `retire()`. This slice routes a second path through it and tests
the refusal without a pin.

**Evidence.** `script_runner.cpp:1448-1460`, `:437-448`, `io_cells.cpp:873-903`,
`vql_io_values_test.cpp:635,661`.

**Fix.** In `retire()`, for a request-less entry whose Rejected outcome is not owned by a record,
`freeOutcomeGraph` before `bridge.retire` (read the outcome from the submission first). Add a
refusal leak pin for await-as and for detached refusals.

## MINOR

1. **Double budget accounting.** `retireRequest = false` keeps the service's outcome and its
   `retain`ed chunk while each `await ... as` record charges `chunkBytes` again to `retainedBytes_`
   (`script_runner.cpp:1415-1430`), though the bytes are shared. With a budget of one chunk the second
   `as` declines its copy and silently falls back to the service outcome. A name's ownership then
   depends on how many times it was awaited. Charge a payload chunk once per lease, or document it.
1. **No statement position on run-time typed failures.** A `let` assert, a `join` bound or a binder
   relation failure is no `ioActions_` site, so `execution.cpp:621-629` leaves `statement` empty and
   `failureOf` returns `VQL_IO_*` without a position (`script_runner.cpp:1725-1741`). A multi-statement
   script cannot tell which `let` was empty.
1. **Capture-host summary fallback is not exercised end to end.** The design admits it (§5), and
   the fallback in `capture_host.cpp:138-165` is covered only at the `OwnedOutcome::summary`
   constructor. A failed capture step under arena pressure is untested, and the earlier generic
   refusal is gone, so a regression there would be silent. Slice f should add it as the design says.

VERDICT: NOT CONVERGED

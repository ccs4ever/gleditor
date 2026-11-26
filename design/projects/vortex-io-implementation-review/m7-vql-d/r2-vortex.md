# Round 2 critique: Vortex Expert (M7-VQL-d code)

Target 09635a6c (round 1 fixes 52d120d2..09635a6c). Tally: 0 BLOCKING, 1 MAJOR, 1 MINOR.

Previous findings:

- **M1 (cross-script `await($h) as $r` owned via `finished_`): RESOLVED.** `ownOutcome` falls back
  to `finished_` (`script_runner.cpp:1413-1420`). The entry's request is still admitted there
  (`retireAll(finished_)` keeps a non-terminal request, `:395`, `:1836`), so its outcome and lease
  are valid when copied. On a terminal outcome `retire(retired)` runs once and the entry is erased
  from the list it was found in; the activation's own `admitted` never held it, so nothing retires
  twice. `VqlIoAwaitRecordTest.AnAwaitOfAHandleAnEarlierScriptAdmittedIsOwnedToo` and the 26 tests
  of `VqlIoAwaitRecord*`, `JoinWorld*`, `VqlIoJoin*` pass headless. Residual: a handle admitted by
  another still-parked activation is in neither list, so it stays the service's outcome (see MINOR
  1).
- **M2 (refused-await / detached-refusal graph): RESOLVED.** `retire()`'s request-less branch reads
  the Rejected outcome from the submission, releases the submission, then calls `freeOutcomeGraph`
  (`script_runner.cpp:448-464`). The explicit free in `ownOutcome` is gone, so `freeOutcomeGraph`
  has one caller on this path. It is guarded by `arena.matches` (incarnation), so an arena rollback
  that already released the cells, or a reused slot, is a no-op. The record is a copy and shares no
  cell with the graph. The other callers are in `capture_host.cpp` and work on that host's own
  refusals. The 10^4 pins for both shapes pass.

Regressions checked in my lane and found sound: `join_step`/`join_end` hold no cursor across an
Await (rows and separator are compiled before `StreamBegin`, so no park or nested join can fall
inside the loop; position 0 resets the walk and `join_end` takes it); an error mid-walk leaves a
partial walk that the next position 0 resets. The engine's `let` cell freeing skips I/O cells
(`execution.cpp`, `ioCellTag`) and anything minted at or before the pre-compile mark, so a request,
lease, owned record or handle is never freed there. No core file changed (VM and core neutrality
holds).

## BLOCKING

None.

## MAJOR

### M1. Join providers registered per execution vanish under a parked script

**Problem.** `QueryExecutor::beginVortex` calls `vm_.setRelationProvider("join_step"/"join_end")`
with a walk of its own and queues an undo that sets both to `nullptr` (`execution.cpp:456-466`). The
undo runs when that run closes. The VM's table is keyed by relation name only, and `ScriptRunner`
allows several live activations on one `vm_` (`live_`, `script_runner.cpp:1291-1296`). A script that
is parked at an `await` and has a `join` still to run depends on a registration that any other
script's begin replaces and any other script's end erases. `resume()` never re-registers
(`script_runner.cpp:2174-2198`). `prose_hits` has the same shape, but `join` is a core aggregate
that used to compile to plain opcodes, so this slice made a previously safe function depend on it.

**Failure scenario.** Script A:
`detached read as $h; effect { await($h) } as $r; return join($rows, ",")` parks at the await.
Script B (`return join($rows, ";")`) is run and completes, and its undo removes both providers. A's
read completes and A resumes. A's `join` fails with
`VQL_SCRIPT_FAILED: RelationNext: no relation provider is registered for 'join_step'`. Reproduced
with a scratch gtest built outside the repo (`ProbeJoin`, in
`/tmp/claude-1000/.../scratchpad/probe.cpp`): B returns `a;b;c` and A fails with the text above. The
reverse order also breaks: A ending while B is parked removes B's providers. If B only begins while
A is parked and A then resumes, A uses B's walk, which is harmless only because the walk is reset at
position 0.

**Evidence.** `execution.cpp:456-466` (registration and undo), `vortex_vm.cpp:824-831,891-894`
(name-keyed table; missing provider fails), `script_runner.cpp:1291-1296,2174-2198` (concurrent
activations, no re-registration on resume). No test runs a second script while one is parked with a
join to come; every join test is single-script.

**Fix.** Register the two join relations once, where `ScriptRunner` registers its own providers
(`script_runner.cpp:720`), or lazily in the engine, so they live as long as the VM or runner and
need no undo. The walk can be keyed or passed through the relation's own state: it is reset at
position 0 and consumed by `join_end`, and a join has no park inside its loop, so one walk object
shared by all executions is safe. Alternatively re-register in `resume()`. Add the two-script test
above (the probe is a ready body). `prose_hits` deserves the same treatment, but that is older code
and not this slice's finding.

## MINOR

1. **A handle held by another parked activation is not owned.** `ownOutcome` searches
   `current_->admitted` and `finished_` only (`script_runner.cpp:1413-1420`). A handle admitted by
   another activation that is still parked is in that activation's own list, so `await($h) as $r`
   keeps the service's outcome (no record) and `$r` can dangle once that activation ends. This needs
   two concurrent scripts sharing a name, so it is narrow. Either search `live_` too, or refuse the
   await-as with a typed code and say so in the design.

VERDICT: NOT CONVERGED

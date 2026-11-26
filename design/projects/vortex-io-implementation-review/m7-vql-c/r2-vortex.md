# Round 2 critique: Vortex Expert (M7-VQL-c code)

Target `cb194c92` (round 1 fixes `dcce529b..cb194c92`). Tally: 0 BLOCKING, 1 MAJOR, 2 MINOR.

Ran headless: `VqlIo*` and the stale/capability e2e cases, 207 tests, all pass. The MAJOR below was
found and confirmed by driving `build/vquery` by hand, not by a test; no code or test was changed.

## Status of round 1

- M1 (parked capture whose token was dropped is never released; host freed late): RESOLVED.
  `~ScriptRunner` locks every live activation into `alive` before `parked_.clear()`
  (`script_runner.cpp:206-212`), and the close, stop, retire and capture-release loops run over that
  vector. `~FileReadHost` calls `CaptureHost::abandonAll()` after `requestShutdown` while `service_`
  is still a live member (`file_read_host.cpp:188-191`), so `reapRetiredRequest` runs through a live
  service; `~CaptureHost` is left with `freeRun(run, false)`, which touches no `IoService`. The
  release path still orphans before freeing and then `reapAbandoned`, so the pin is made. No
  `HasWaiters` case reaches here (`~ScriptRunner` cancelled the waiters). No new lock or thread
  access: everything stays owner-thread.

Round 1 MINORs m1 and m2 were deferred with reasons. m1 (a conservative pin costs a `:reconcile`,
never data) and m2 (the step's tag is lost, not its failure) are acceptably reasoned as polish. m3
and m4 are fixed (warning log, comment states the timeout exception).

## Checked, clean

- Same-request resume and abandon-and-pin: the diff does not touch them beyond the destructor
  ordering above; `AppendRequested` still pins a capture that dies after the append was requested.
- `sweepStepGraphs` freeing a never-admitted step (`capture_coordinator.cpp:718-722`) is guarded by
  `specInUse` and `freeStepGraph`; a graph a capture still names is kept.
- `owned_` collision (`script_runner.cpp:1250-1259`) returns the lease and budget before the erase.
- Session exposure set: `exposures_` lives in the runner, `assess()` is computed per call, so a later
  commit lifts the refusal. The set grows only with distinct ranges.
- Failed and timed-out scripts: a binding made by a failed script is rolled back by the engine
  (verified: `$a` is unbound after `VQL_IO_CAPTURE_SOURCE`), so `endActivation` not calling
  `noteBindings` leaves no dangling name.
- VortexCore and VortexVM are untouched (neutrality holds); the stale marker is minted with the
  runner, below any mark, and freed in the destructor.

## BLOCKING

None.

## MAJOR

### M1. A stale write source writes the literal text "stale handle" into the file

**Problem.** The stale marker is an untagged text cell. The relation providers refuse it, but the
write path does not go through a provider: `ScriptRunner::run` binds a write source from the
session variable (`script_runner.cpp:~1076`) and `sourceBytes` treats any untagged cell as "a text
cell: its text is the bytes" (`script_runner.cpp:~1580-1590`). A variable rebound to `staleMarker_`
therefore reads as the 12-byte text `stale handle`. Round 1 D's contract is a typed
`VQL_IO_STALE_BINDING` refusal at every reader; the write source is a reader, and its failure is a
durable file with the wrong bytes, which is worse than the aliasing it replaced only in that it is
now deterministic nonsense rather than a stranger's record.

**Failure scenario.** Reproduced against `build/vquery`:

1. `effect { ... read_bytes(0, 5) ... await($q) ... } as $a; return io_tag($a)` prints `Complete`.
1. A second read script (`as $b`) admits, retiring `$a`'s record; `$a` becomes the marker.
1. `effect { open(".../dst.txt") for write create } as $dst; effect { let $s := submit($dst,
   write_bytes(0, $a)); ... } as $w; return io_tag($w)` prints `Complete`.
1. `od -c dst.txt` shows `stale handle`.

The same marker is also an ordinary row for `return $a` and is persisted as that text by `:save`
(the persist gate checks handles by tag and the marker has none).

**Evidence.** `script_runner.cpp:582` (refusal only in providers), `:~1580-1590` (`sourceBytes`
untagged branch), `:1682` (`staleMarker()`), manual run above.

**Fix.** Refuse the marker where untagged cells are accepted: in `sourceBytes` (and the operand
`number()` path, which already refuses a non-number) test `source == staleMarker_` and return
`VQL_IO_STALE_BINDING`; do the same in `refuseDurableAction` so a persisted row naming the marker is
a typed refusal. Add a test that writes through a stale `$a` and asserts no file bytes and the
typed code.

## MINOR

### m1. A stale capability name is refused with a generic message

`capability()` (`script_runner.cpp:~1150-1165`) answers "is not a capability cell" for the marker, so
`submit($f, ...)` after `$f`'s capability was reaped is not `VQL_IO_STALE_BINDING`. Fix: test the
marker first and use the typed code. Same fix site as M1.

### m2. The marker is minted before the constructor can throw

`staleMarker_` is made in the member-initialiser list (`script_runner.cpp:182`); the
`hasSubmitHandler` throw that follows (`:~184`) leaks the cell, since no destructor runs. It is one
arena cell on a programming-error path. Fix: mint after the check, or free it before the throw.

VERDICT: NOT CONVERGED

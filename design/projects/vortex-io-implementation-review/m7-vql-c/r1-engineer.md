# Round 1 critique: Software Engineer (M7-VQL-c code)

Target: `dcce529b` (diff `4d132ccf..dcce529b`). Tally: 0 BLOCKING, 2 MAJOR, 7 MINOR.

Method: read of the write adapter, grant, capture host, runner lifetime code, vquery wiring and the
tests; the relevant gtest suites (`FileWriteHostTest.*`, `CrashPointTest.*`, `VqlIoCapture*`,
`VqlIoWrite*`: 94 tests) were run headless against the existing `build/xuzz_test` and pass. No
sanitizer build was made, so M1 is established by reading and is not reproduced.

## BLOCKING

None.

## MAJOR

### M1. `~CaptureHost` runs after `IoService` is destroyed and calls into it

**Problem.** `FileReadHost` declares `capture_` before `service_` so that the service's destructor
joins the workers while the coordinator they execute still exists. That makes `service_` die first.
`CaptureHost` keeps an `IoService &` and its destructor, which the header documents as the way runs
still held are abandoned, calls `service_.reapRetiredRequest(...)` for every run. That touches the
service's `requestByCell_` after the `IoService` was freed.

**Failure scenario.** Any `Run` still in `runs_` when the host is destroyed: a handle from
`FileReadHost::capture()->begin()` that was never `release`d, or a capture belonging to an
activation the runner no longer reaches. The second case happens when `ScriptRunner::live_` holds
only weak pointers, the activation was destroyed by a dropped token or an exception out of `run()`,
and so `releaseCaptures` never ran for it. At `~FileReadHost` the members are destroyed in reverse
order: `service_` is freed, then `~CaptureHost` -> `freeRun` ->
`service_.reapRetiredRequest(*run.appendRequest)` reads `requestByCell_` of the freed service. This
is a use-after-free during teardown. `appendRequest` is set for every run from `begin()` on, so
every unreleased run reaches the call. The destructor comment "the coordinator ... is never asked
anything" is also false for this reference.

**Evidence.**

- `apps/common/xanadu/vortex/file_read_host.hpp:219-222` (`capture_` then `service_`, destroyed in
  the opposite order).
- `apps/common/xanadu/vortex/capture_host.cpp:44-51` (destructor frees every run) and `:284`
  (`service_.reapRetiredRequest`).
- `apps/common/xanadu/vortex/io_service.cpp:699-704` (member access).
- `apps/common/xanadu/vql/script_runner.cpp:221-237` (releases captures only for activations that
  still lock), `:1070-1071` (`live_` is weak, and the activation has no destructor).
- No test destroys a `FileReadHost` holding a live run:
  `vql_io_capture_script_test.cpp:430,495,759,847` all assert `liveRuns()` is 0 or at most 1 while
  the host is alive.

**Fix.** Make the destructor independent of the service: either drop the `reapRetiredRequest` call
from `~CaptureHost` (the service is gone, so nothing is left to reap), or have `FileReadHost`'s
destructor call `capture_->abandonAll()`/reset `capture_` explicitly after `requestShutdown` and
before `service_` is destroyed, with workers joined by an explicit service shutdown step. Add a test
that begins a capture, never releases it, and destroys the host (run it under ASan).

### M2. `:reconcile ID promote N` promotes the journal's span instead of refusing the malformed command

**Problem.** The argument parser treats "no second number" as "no span given" whenever the stream
hit EOF, so a missing LEN is not distinguished from no arguments, and trailing tokens after START
LEN are ignored. The resulting `ReconcilePromote{}` with no span promotes the journal-named span
(`pin.span`), which is a durable identity decision that the coordinator journals before acting.

**Failure scenario.** The author types `:reconcile 3 promote 100`, intending a span but forgetting
the length. `iss >> start` consumes `100` and sets eofbit, `iss >> length` fails, `iss.eof()` is
true, so `decision = ReconcilePromote{}` and capture 3 is promoted to whatever span its journal
names. Verified with a standalone copy of the parse: `promote 100` and bare `promote` both yield
"journal-span"; `promote 5 6 7` yields an explicit span and drops `7`. A wrong decision here is
journaled and replayed on restart. The usage text promises `[START LEN]`, a pair.

**Evidence.** `apps/vquery/main.cpp:1180-1190`.

**Fix.** Parse the optional pair as a unit: after `promote`, require either end of input (no span)
or exactly two numbers then end of input; anything else prints the usage line. Add an e2e case for
`promote 100` and `promote 1 2 3` that expects the usage message and no journal record.

## MINOR

### m1. A write root with a trailing slash that does not exist yet denies every write, for ever

**Problem.** `weakly_canonical("/x/new/")` keeps an empty trailing element for a path that does not
exist (an existing directory loses it). `strictlyBeneath` then compares the empty element to the
file name and fails. Roots are canonicalised once at startup, so creating the directory later does
not help.

**Failure scenario.** `vquery --io-write-root out/ ...` before `out` exists. Every write open is
refused with `fileDetailNoWriteGrant` with no hint, even after `mkdir out`. Confirmed:
`weakly_canonical("/tmp/nonexist/")` has 4 path elements including the empty one.

**Evidence.** `apps/common/xanadu/vortex/file_read_host.cpp:55-65,67-72,118-135`. No test uses a
trailing-slash or not-yet-existing root.

**Fix.** Strip a trailing empty element (`root.has_filename() ? root : root.parent_path()`) in
`writeRootGrant`, and warn in vquery when a root cannot be canonicalised (the `ec` branch currently
drops it silently).

### m2. The path check and the open are not atomic for intermediate components

**Problem.** `O_NOFOLLOW` guards only the last component. The host canonicalises the locator on the
owner thread, the worker opens the same string later. A directory component under the root swapped
for a symlink between the two steps redirects the open outside the root.

**Failure scenario.** Another process (or a script with a second tool) with write access under the
root replaces `root/sub` by a symlink to `/etc` after the grant check and before the worker's
`open`. `open("root/sub/f", O_NOFOLLOW|...)` follows `sub` and writes `/etc/f` (an existing file for
`-w`; `O_EXCL` makes creation of a new one equally possible). The comment at the open claims the
check "cannot promise more than the path it saw", which is honest, but the contract's "path safety"
is then only best-effort.

**Evidence.** `apps/common/xanadu/vortex/file_endpoint_adapter.cpp:205-213`;
`apps/common/xanadu/vortex/file_read_host.cpp:154-176` (grant runs at admission).

**Fix.** Open under a held root descriptor with `openat2(RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS)`
(Linux 5.6), carrying the matched root with the grant; otherwise document the residual race next to
`--io-write-root`.

### m3. `~ScriptRunner` drops its own hold on parked activations before it uses them

**Problem.** `parked_.clear()` runs first and `parked_` holds a full token (a `shared_ptr` to the
activation). The close/retire/capture-release loops then use `live_`, which is weak. An activation
whose only owner was `parked_` is destroyed by the `clear()`, so its opened files are not closed,
its admitted requests are not retired, and its captures are not released.

**Failure scenario.** A caller that discarded the `Parked` result (an early `return` or exception
between `run` and the wait loop) leaves `parked_` as the sole owner. At runner destruction the file
descriptor and endpoint graph stay open, and any capture run stays in `CaptureHost::runs_`, which
feeds M1.

**Evidence.** `apps/common/xanadu/vql/script_runner.cpp:205-210` against `:221-237`.

**Fix.** Collect `std::vector<std::shared_ptr<Activation>>` from `live_` (and from `parked_` tokens)
before cancelling waiters and clearing, and run the loops over that vector.

### m4. A close refused at runner destruction is dropped, leaving the descriptor open

**Problem.** `reapClosing(true)` tries each `unclosed_` capability once and then discards it
(`if (force) continue;`). The comment says "an opened file's lifetime is the script's, so a refusal
never ends it", but at destruction a refusal ends it.

**Failure scenario.** A script ends while the service is at its request budget or the endpoint has a
close in flight; the close is refused and queued in `unclosed_`. The runner is destroyed (end of a
vquery session, or a REPL swapping runners) and the forced retry is refused again; the fd and the
endpoint graph live until process exit. In vquery that is the process, but for the library API it is
a leak per refused close.

**Evidence.** `apps/common/xanadu/vql/script_runner.cpp:434-451`.

**Fix.** At destruction, hand every still-unclosed capability to `ports_.reapWhenEnded`-style host
bookkeeping, or retry on the host's `pump` until the close is admitted.

### m5. The capture directory's own creation is not made durable before the journal's first intent

**Problem.** `CaptureJournal` syncs its parent directory, so the journal file's entry is durable in
`directory`, but `create_directories(directory)` never fsyncs `directory`'s own entry in its parent.

**Failure scenario.** First `capture` in a fresh profile, power loss after the append reached the
permascroll (its `Append` intent and the bytes were both fsynced). The whole `xuzz/capture/<key>`
directory entry may not have survived, so the intent record is gone while the bytes remain: recovery
sees an empty journal and reports the bytes as nothing, not as orphan bytes to reconcile. The loss
is limited to a diagnostic (no data corruption), hence MINOR.

**Evidence.** `apps/vquery/main.cpp:680`; `apps/common/xanadu/vortex/framed_log.cpp:104-111`.

**Fix.** After `create_directories`, fsync each newly created component's parent (or the first
existing ancestor) before building the stack.

### m6. A refused cleanup of a never-admitted capture step leaves its scaffolding for ever

**Problem.** `sweepStepGraphs` skips every step whose `request` is unset
(`if (!step.request) return false;`). Refusal paths free the step immediately with
`if (freeStepGraph(steps_.back())) pop_back()`, but when `freeStepGraph` returns false (an endpoint
still named by a view, or a thrown arena error) the entry stays with no request, so no later sweep
can free it, and `stepGraphs` never returns to zero.

**Failure scenario.** `appendRequest` is refused by admission, `reapIoEndpoint` fails once (for
example because the arena refused a delete), and the endpoint, capability and spec cell leak per
refused capture. The leak test (`ManyCaptureRunsLeaveTheArenaAndTheStacksBounded`) only exercises
the success path of the free.

**Evidence.** `apps/common/xanadu/vortex/capture_coordinator.cpp:1530-1556` (push, then pop only on
success), and `sweepStepGraphs` at the `if (!step.request) return false;` line.

**Fix.** Let the sweep retry unadmitted steps (free when `!request` and nothing pending names the
spec), so a failed immediate free is picked up on the next turn.

### m7. Test gap: no test proves the claims at the grant/open boundary and at host teardown

**Problem.** `FileWriteHostTest` covers `..`, a symlinked directory and a sibling prefix, but
nothing exercises (a) a root with a trailing slash or one that does not exist, (b) a symlink swapped
after the grant check (the claim in the `O_NOFOLLOW` comment), or (c) destroying the host with an
unreleased capture (M1). The crash tests simulate a crash with an exception thrown from the
journal's flush hooks inside one process, so they prove recovery from the journal's bytes, not that
the permascroll fsync precedes `Appended`.

**Evidence.** `tests/xuzz/io_file_write_host_test.cpp:182-290` (cases present);
`tests/xuzz/vql_io_capture_crash_test.cpp:26-50` (hook-based crash).

**Fix.** Add the three cases; for (b) open a descriptor race with a thread that swaps a link inside
the worker's open (or unit-test the adapter with a locator whose last component is a symlink and
assert `ELOOP`).

## VERDICT: NOT CONVERGED

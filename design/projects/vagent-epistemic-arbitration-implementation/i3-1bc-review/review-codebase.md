# I3-1b/1c review: vagent compiler and codebase persona

Scope: `git diff 00db472b..4ad3f53a` (25 files, I3-1b checked ingress and I3-1c advance/retire),
against i3-design sections 3-6, 8, 9, 11 and 13. Method: read the diff and the emitted-IR generators,
then built throwaway probes in /tmp/i3-1bc-codebase (no repo file touched; compiled with the repo's
flags and linked against the existing objects). Existing suite: `vagent_actor_test
--gtest_filter='VagentEpistemic*'` passes, 67 tests.

## Probe results

- **noCell rule:** `git diff -U0 00db472b..4ad3f53a -- apps tests | grep '^+.*noCell'` finds 0 added
  lines (137 removed). No new `noCell` use. Residual reserved values are listed under MINOR 1.
- **IR control flow:** every exit of `emitObserve`, `emitObserveRefusal` and `emitSourceChange` goes
  through `endObservation` (it writes `$turn_result`, then jumps to `haltOp_`). The tail of
  `emitObserve` falls from the last predicate into `malformed`; a program whose arbitrated predicate
  list is empty is covered by `observeEntry` being absent. `rd` negward in `head`, `recordOf`,
  `unlinkFromRank`; the `nextV` read before the unlink in the retire walk is right; a record
  whose support was already dropped (`rd(sup, source, neg)` zero) takes `detached` correctly. I found
  no register or label defect.
- **Fuel (cycles per turn, `SPDLOG_LEVEL=vortex.agent=trace`):** ingestion rises from 9.8k to a
  plateau of 12,325 as the window and history fill. With 4 keys x 4 history, advance 8,385 and
  retire 13,956. All far under the 10M budget; the loops are bounded by the settings.
  A cycle-starved turn (`maxInstructions = 3000`) is covered under MAJOR 2.
- **Cell leak (new, see MAJOR 1):** 400 committed observations from one principal, one key:
  `arena.cellCount()` goes 12,615 -> 277,862, a constant 667 slots per observation, while
  `epistemicCells()` stays at 383. A percept turn adds 19.
- **Retire/advance:** 24 observations over 4 keys, then bind at incarnation 2, then `retireSource`:
  reachable epistemic cells 1019 -> 23 (a bare tombstone profile). Behaves as designed.

## BLOCKING

None.

## MAJOR

1. **Physical slot price is 6.7x the design's, and `ArenaExhausted` is never produced.**
   i3-design section 9 prices an ingestion at "about 100 slots" and sizes the deployment note
   (248 days at 1 Hz for 2^31, two days for 2^24) on it. Measured: 667 slots per committed
   observation (trace above). The 2^24 allowance lasts about 7 hours at 1 Hz instead of two days; a
   10 Hz sensor group (the cases 5.2 defers to an upstream estimator) fills it in 42 minutes.
   Section 9 also promises that "ingestion refuses `ArenaExhausted`, with the host action stated".
   `grep ArenaExhausted` finds only `outcomeClassOf` (agent_executor.cpp:3007) and no producer: when
   the allocation limit is hit the turn fails and reports `IngestionFault` (MAJOR 2's path), so the
   host is told "program defect" instead of "checkpoint and resume".
   `TheEpistemicCellCountPlateausAtTheHistoryBound` (ingress_test.cpp:600) measures reachable cells
   only, so it proves the logical bound and is silent on this.
   Where the slots go: `callSetField(rec, name, nullopt, vc); mov(D(vc), v)` (the `set` lambda at
   compiler.cpp:~4990, `bookkeeping` at ~4910-4960, `emitObserveRefusal` at 4462-4466) calls
   `rtSetField`, which on an existing field runs `alloc(vc); linkOp(np, values, vc)`
   (compiler.cpp:1560-1566) and orphans the old value cell, plus `callCopy(obs, rec)` clones the whole
   observation record (~25 fields) and the dedup entry, view and stream cells are minted per event.
   Remedy: (a) have the overwrite sites write into the cell `rtFind` already returned
   (`mov(D(r0_), v)`, as the stream update at compiler.cpp:4956 does) so a scalar overwrite costs no
   slot; build `rec` from the fields ingestion needs instead of copying the signed metadata twice;
   (b) re-measure and put the real number in section 9; (c) add the `ArenaExhausted` producer
   (compare `arena.cellCount()` with the allowance before the turn, as the design says) with a test.

2. **Fuel and cycle exhaustion are classified `NeedsHostAction`, not `Retryable`; the retry classes
   are dead code.** Design section 8 (i3-design.md:235-236) lists "turn fuel or cycle exhaustion" as
   Retryable and says a turn that set no `$turn_result` is `IngestionFault`. `deliverHost`
   (agent_executor.cpp ~1620) derives the code only from `outcome.result`; a failed turn leaves
   `result` empty and the cause ("cycles", "fuel", `epistemic.*`) in `outcome.cause`, which it never
   reads. Probe (`maxInstructions = 3000`, one valid observation): outcome `Refused`, code
   `IngestionFault`, class `NeedsHostAction`. So the `"cycles"`/`"fuel"` strings in `outcomeClassOf`
   (3005) and in `Ingress::chargeable` (702) can never match, and a starved turn is told apart from
   a runtime defect only by the log trace. Related: `UnboundIncarnation` is `Terminal` in the design
   (line 232) but `NeedsHostAction` in `outcomeClassOf` (3007), which the `OutcomeClass` comment
   ("bind an incarnation") supports; one of the two is wrong and no test pins it.
   Remedy: map `outcome.cause` (`kFailCycles`, `kFailFuel`) to the retryable codes in `deliverHost`
   and decide `UnboundIncarnation` once, in the design and in the code, with a test.

## MINOR

1. **Reserved values for "absent" outside the wire (the owner's rule).** None is `noCell`, but they
   are the same defect:
   - agent_executor.cpp `retireSource`: `std::uint64_t highest = 0;` is sent as the retire's
     incarnation when the principal has no binding, so `emitSourceChange` mints a tombstone with
     incarnation 0 for any 64-hex string the host names (`absent` branch, compiler.cpp ~4560). Use
     `std::optional<std::uint64_t>` and refuse (or no-op) a retire of a principal with no binding and
     no profile. As written, tombstones are unbounded (tombstones are exempt from `max_sources`, and
     the profile walk in every ingestion is linear in them).
   - `Ingress::Logged{eventId = {}}` for the stale-binding revocation entry (deliverHost ~1655):
     an empty event ID stands for "no event". `ingestOutcome` cannot match it only because `observe`
     rejects empty IDs; make the key `std::optional<std::string>`.
   - `IngestOutcome::code` is "empty unless Refused" and `Ingress::Charge::code` starts empty; the
     former should be `std::optional<std::string>` (the kind already says whether it applies).
   - compiler.hpp `FrameBody`: `noCell` replaced by `{}`, i.e. the same 0 default for `frame`,
     `make`, `enter` and `result`. `emitFrameBody` fills all four, so it is harmless today, but the
     fields are still "CellRef 0 means unset". `optional<CellRef>` or no default if they are always
     set. (Inside the emitted IR the `zero_` argument and the empty-string `$turn_result` clear are
     the VM's own wire and are fine.)
2. **Queue counters and `Pending` entries leak on dropped posts.** `drainPosted` drops an
   observation when `!live(state) || state.inTurn` (agent_executor.cpp ~3344) before it decrements
   `queued`/`queuedBy`; `forgetIngress` covers only stops through the executor (`fatal`,
   `orderlyStop`, `retire`). An actor stopped by the runtime directly keeps its bindings and a full
   queue count. The same happens for `hostEvents.clear()` at 2726 and 2882 when `wake` fails: the
   queued ingestion is discarded, the log entry stays `Pending` for ever and the host retries
   nothing. Remedy: decrement before the liveness checks and log a `Refused` (Retryable) entry for
   events discarded this way.
3. **The outcome log holds half its configured size.** `observe` logs a `Pending` entry on accept,
   and `deliverHost` appends a second entry for the result, so `epistemic_outcome_log = 256` keeps
   128 events, and under backpressure the `Pending` entries evict resolved ones that a retrying host
   is polling. Update the `Pending` entry in place, or log one entry per event.
4. **No diagnostics on the new paths.** One `GLEDITOR_LOG_TRACE` ("turn ran N cycles") is added.
   Refusals at ingress, drops in `drainPosted`, binding refusals and revocations log nothing, so a
   host that sees `UnknownSource` cannot tell expired from revoked from never bound. Add
   `GLEDITOR_LOG_DEBUG(kLog, ...)` calls (category `vortex.agent` exists; a dedicated
   `vagent.ingress` would name the subsystem better). Do not log tuple values.
5. **Setters that return void.** AGENTS.md: a function that changes an object and has nothing else
   to return gives back a pointer. `forgetIngress`, `postRefusals` (private) and `Ingress::log` /
   `forget` return void. Low value on private helpers; `forgetIngress` and `postRefusals` are the
   ones to chain if the rule is applied strictly.
6. **`observe` is not exception-safe in its counters.** `++queued; ++queuedBy;` precede `log` and
   `posts_->push`; a `bad_alloc` in either leaves the counters raised with nothing queued. Increment
   after the push, or use a small guard. The ingress mutex is also held across `encodeValue` and
   `sha256Hex`, serialising every producer thread on the hash; build the record before locking
   (the binding snapshot is the only thing that needs the lock).
7. **Stats pollution.** A `Duplicate` turn is undone as a refusal, so it bumps
   `refused_Duplicate`; a bookkeeping turn with no profile bumps `refused_NoProfile` although the
   adapter ignores it. Neither is wrong, but `stat()` readers will see refusals that are routine.
   The dedup trim removes one entry per push, so lowering `vagent.epistemic_dedup` on a live store
   would not shrink the window until it is rewritten (settings are fixed per executor today).
8. **Why-not-Vortex comment.** `epistemicCellsWithin` (agent_executor.cpp ~2931) walks the arena
   in C++ for admission. It is host admission against a host setting and the walk is bounded by
   `ceiling + 1`, which is a defensible reason, but no comment says why it is not a `rtCount`
   routine in the turn. One sentence would settle the review question (the same applies to
   `ingress_->bindings` scans in `bindSource`/`retireSource`, which are host authorization and fine).
9. **Test quality.** (a) `EpistemicView` (epistemic_invariants.hpp) and the `VagentEpistemicIngress`
   fixture (`profiles()`, `listLength()`, `value()`, `history()`, `supports()`) each walk the same
   trust, source and epistemic ranks; the fixture does not use the shared view, so a rank layout
   change needs two edits. (b) Both do `const_cast<ArenaManifold &>(ctx().arena)` to call
   `registerDimensions`; a const lookup overload would remove it. (c) `ASequenceThatDoesNot...` and
   `EachPrincipalGetsOneProfile...` etc. are good, but nothing tests the two findings above
   (cycle-starved class, `ArenaExhausted`).
10. **Fuzzer.** About 2 executions per second (known) means the 5 corpus scripts exercise the
    state machine but libFuzzer will not find much in a CI minute; consider reusing one actor across
    inputs (reset by `retireSource` plus a higher incarnation) so the executions/second is useful.
    Placement is right: test-support headers sit in tests/xuzz, the fuzzer includes
    `"../xuzz/epistemic_invariants.hpp"`, and the Makefile `fuzz-target` line and the `fuzz`
    prerequisite list are correct (`make fuzz` builds `fuzz_epistemic_ingress`).

Verdict: FAIL

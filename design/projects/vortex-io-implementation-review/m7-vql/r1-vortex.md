# Round 1 critique: Vortex Expert (Vortex I/O M7-VQL code)

Tally: 0 BLOCKING, 3 MAJOR, 5 MINOR. Target 8711aa1c, scope ef74939f..8711aa1c.

## BLOCKING

None.

## MAJOR

### M1. Submit under backtracking orphans an admitted request that nothing can retire

**Problem.** `stepSubmit` mints the submission sum and request cells in the arena and admits the
request in the service. `VortexVM::backtrack` then does `arena.release(cp.mark)`, which truncates
every cell created after the choice point, including those cells. The request stays in the
`IoService` index and in its request budget. No host path holds a handle to it. `ScriptRunner`
tracks only the latest `submission_`, and the plan says "the request stays with the host to retire"
without naming a retirer.

**Failure scenario.** A Vortex program runs `Choice; Submit; ...; Fail` in a retry loop. Each pass
admits a request and then truncates the cells that name it. After
`FileReadHostOptions::defaultRequests` (16) iterations the service refuses all admission, for the
runner and for every other script on that host. The only way out is a restart. The owner decision "a
new execution creates a new operation" is honoured, but the new operation leaks permanently. Whether
a worker completion published into the truncated cells is handled safely is not shown by any test.

`VortexSubmitOpcodeTest.BacktrackingOverASubmitSubmitsAgainOnReexecution` asserts only
`host.calls == 2`. It never checks the service's live-request count or the state of the abandoned
request after rollback, so it does not prove the claimed safety.

**Evidence.**

- `apps/common/xanadu/vortex/vortex_vm.cpp:1465` (`release(cp.mark)`)
- `apps/common/xanadu/vortex/vortex_vm.cpp:4081-4083` (the hook call)
- `tests/xuzz/vortex_io_vm_continuation_test.cpp:685-715`
- `design/vortex-io-implementation-plan.md` P6 bullet "The abandoned submission cells go with the
  arena rollback"
- `apps/common/xanadu/vortex/file_read_host.hpp` (`defaultRequests` = 16)

**Fix.** Pick one:

- (a) Refuse `Submit` while any choice point is live that was created before it. That is a typed
  step failure, like the mutation-group refusal. It is consistent with the plan §14 "Prolog
  speculative effects" refusal.
- (b) Keep the submission and request cells out of the rolled-back region, or reclaim them from a
  host-side list of orphaned requests that the owner reaps.

Add a test that counts live requests and budget after Fail. Option (a) needs a VM change, so it
needs owner approval under a new P-number.

### M2. A second parked script leaks the first one's request, and `submission_` mis-binds for it

**Problem.** `ScriptRunner` keeps a single `submission_` for the runner, but `ParkedProgram` and
`parked_` allow several parked scripts. The arena-mark change was made precisely because "several
scripts can be parked at once". The runner state was not generalised to match.

**Failure scenario.**

1. Script A parks on its Await with `submission_ = A`.
1. Script B starts. Its `admit` runs `retireSubmission(A)`, finds A's request awaited in `parked_`,
   and only `release`s the sum. It then sets `submission_ = B`.
1. A is woken and completes. Nothing ever retires A's request, because only the latest `submission_`
   is retired, by the next action or the destructor. Each interleaved script therefore leaks one
   request until the budget of 16 is hit.
1. If A parks again, `settle` does `bridge.match(*submission_)`. That returns B's submission, so A's
   token gets B's request, and `parked_` stores B's request under A's continuation.

No test parks two scripts at once. Only `parkedCount() == 1` is exercised.

**Evidence.**

- `apps/common/xanadu/vql/script_runner.cpp:233-234` (`retireSubmission` then overwrite)
- `apps/common/xanadu/vql/script_runner.cpp:255-258` (`settle` matches `submission_`)
- `apps/common/xanadu/vql/execution.cpp:403-418` (mark cut "because several scripts can be parked")
- `tests/xuzz/vql_io_script_test.cpp:377,389`

**Fix.** Key the submission per activation or per continuation. For example, put it in `Activation`
and set it from `admit` via the cursor argument the hook already receives, which the lambda
currently drops (`script_runner.cpp:54`). Retire per activation when it finishes, fails or is
abandoned. Add a two-script interleave test. If concurrent scripts are not meant to be supported,
refuse a second `run` while `parkedCount() > 0` with a typed diagnostic. Today the code claims
concurrency and breaks under it.

### M3. Every I/O script action permanently leaks its compiled program cells

**Problem.** A parked run calls `settleMark(true)`, which is `discard(preCompileMark)`, and from
then on `markHeld` is false. Completion after resume, failure after resume and an abandoned wake all
skip any release. An action that never parks takes the "mutated" branch (new result cells) and also
`discard`s. The compile cells, the spec term, the submission, arm and payload cells and the cursor
therefore stay for the life of the arena. `FileReadHost::stageRead` does use `freeCell` for a
refused stage, so the P4 primitive is known and available, but the compiled program's cells are
never freed or compacted.

**Failure scenario.** A long `vquery` session runs `effect` scripts in a loop. Arena cell count and
`VortexVM` maps (`opcodeMap_`, `cursorPC_`) grow without bound by one compiled program per action.
This violates the "bounded memory, no unbounded growth" standing constraint.

The M7-VQL notes call this a deliberate trade-off, but they give no price and no retention bound.

**Evidence.**

- `apps/common/xanadu/vql/execution.cpp:403-418`
- `apps/common/xanadu/vql/execution.cpp:429-431`
- `apps/common/xanadu/vql/execution.cpp:504`
- `design/vortex-io-implementation-plan.md` M7-VQL notes

**Fix.** Choose one:

- Track the program's cells in `ProgramRun` (the compile cells are the contiguous cell range from
  `preCompileMark.cellCount` to the count at park) and `freeCell` them in `ProgramRun::close()` when
  no result row refers to them.
- Replace the mark with a per-script scratch region.
- If neither is feasible this milestone, record the growth rate and a bound in the plan and add a
  retention test. Leaving it unpriced is not acceptable.

This is not an R8 violation in itself, because nothing is minted to the store. It is the growth that
breaks the standing constraint.

## MINOR

1. **Submit hook is a per-VM singleton that `ScriptRunner` silently overwrites.**
   - `script_runner.cpp:53` overwrites any installed handler. The destructor at `:59` clears
     whichever handler is current, so destroying runner 1 disables runner 2's scripts.
   - This is consistent with Await's one IoHost per VM and is not an arbitrary ceiling.
   - It is documented but unenforced. Refuse construction when `vm_.hasSubmitHandler()`, or return
     the prior handler and restore it.
1. **`stepSubmit` does not validate the host's arm.**
   - The arm is a `uint8_t` written as an Int64, and the VM checks neither that it is 0 or 1 nor
     that it matches the sum cell's real arm. It also does not check the payload's tag, only
     `arena.contains`.
   - The ordinal as a bare Int64 is acceptable. It is a derived projection, not a reserved value:
     the sum cell is the value and the arm is only a branch input, so it is not a sentinel in the
     AGENTS.md sense. A buggy host could still send `BranchFalseCell` the wrong way.
   - Add a VM-neutral validation or a host-side assertion, with a test.
   - Evidence: `vortex_vm.cpp:4081-4112`.
1. **A failed Submit after a successful host admission can leave a request behind.**
   - `ScriptRunner::admit` calls `bridge.submit` and then `bridge.match`. If `match` fails, the
     exception fires with the request admitted and `submission_` unset.
   - The VM's post-hook validation (stale sum or payload) fails the same way.
   - The comment "a failed step never leaves an admitted request behind" is true only of pre-hook
     checks. Retire in the failure path, or narrow the comment.
   - Evidence: `script_runner.cpp:226-230`, `vortex_vm.cpp:4090-4096`.
1. **`Submit` `executeOpcodeBody` message and the `step` dispatch call `getCursorOpcode` and
   `getOpcodeKind` twice per step** (`vortex_vm.cpp:3910-3915`). This is a hot-path micro-cost and
   could be one lookup.
1. **VM includes I/O schema headers.**
   - `ioCellTag`, `CapabilityCell` and `SubmissionCell` appear in the VM source. That is the
     existing Await pattern, and the P6 text claims the VM "names no I/O schema". Only the hook
     payload `VmSubmission` is decoded.
   - No test enforces the neutrality claim. Add an include or symbol check, or soften the wording.
1. **`ScriptRunner` remaining C++ is adequately justified now, but the doc text lags.**
   - The runner does native-handle decoding (`vlog.argumentsOf`, stage, bridge, payload read), which
     is host and I/O interfacing.
   - The header should say that and drop the stale "no Submit opcode" framing. The remaining
     Vortex-writable piece is the retire policy (M2).
1. **`vqueryc` compiles unbound names to a `$name` text cell** (`compile_io_effect.cpp`). It is a
   value-level failure at Submit, which is typed and fine, but the diagnostic reads "not a live
   CapabilityCell". Say "unbound capability $name".
1. **File-read owner and worker discipline is sound.**
   - `FileReadHost` stage and bridge calls are owner-only (`requireOwner` in `IoHost`).
   - The `freeCell` on a refused stage is correct.
   - Route lifetime tokens (`routeLife` weak_ptr, per-wake expiry check, `unrouteRunner` in the
     destructor) and waiter reaping are correct under the ordering I traced. No finding.
   - Only the effect-script resume lacks a test that destroys a runner mid-batch from another
     runner's wake.

VERDICT: NOT CONVERGED

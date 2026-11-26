# Round 1 disposition: M7-VQL gate (Vortex I/O)

Reports: [`r1-vortex.md`](r1-vortex.md) (3 MAJOR, 5 MINOR), [`r1-vql.md`](r1-vql.md) (1 MAJOR, 7
MINOR). Target `8711aa1c`. Every finding was checked against the code first.

## MAJOR findings

| id        | verdict                 | fix and regression test                                                                                                                                                                                                                                  |
| --------- | ----------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| vortex M1 | CONFIRMED, fixed        | Rollback notice plus orphaning (below). `VortexSubmitOpcodeTest.ARetryLoopOverASubmitLeavesTheServiceAndArenaBounded`: six Choice/Submit/Fail passes, six notices, arena cell count unchanged, budget whole after the requests end.                      |
| vortex M2 | CONFIRMED, fixed        | Submissions are per activation (`Activation::admitted`), the hook finds its activation through the stepping scope. `SeveralParkedScriptsKeepTheirOwnRequestsInEitherOrder` (five scripts, both resume orders), `AnAbandonedActivationRetiresItsRequest`. |
| vortex M3 | CONFIRMED, **deferred** | Reclamation needs a core API (proposal P7). The price is measured and pinned: `AnEffectScriptRunRetainsAPricedConstantNumberOfCells` (37-40 cells per run). Not bounded; recorded in the plan with its rate.                                             |
| vql M1    | CONFIRMED, fixed        | `-o`, `--in-place`, `:save` refuse action rows with `VQL_IO_PERSIST_NEEDS_CAPTURE`; e2e `ActionRowsAreNotWrittenByOutputStoreInPlaceOrSave`, `AQueryAfterAnActionIsSavableAgain`. Capture routing is deferred (no capture command exists in the CLI).    |

### Backtracking semantics (vortex M1)

A new execution creates a new operation, so backtracking recalls nothing. The VM now tells the host
before the cells of an admitted Submit are rolled back (`setSubmitRollbackHandler`, called newest
first from every choice-mark release: backtrack, scoped abort, failed mutation group).
`ScriptRunner` answers with `IoHost::orphanRequest`: the request is not cancelled, it runs to its
terminal, and the service then forgets it without publishing an outcome cell, freeing the budget.
Refused: refusing Submit under an older live choice point (it contradicts the owner rule), pinning
the cells outside the trail (the arena has no such region), cancel-on-rollback (a recall). The first
part is a small extension of the approved P6 (`vortex_vm`); `IoService` is not a core file.

### vortex M3 detail

A parked program cuts its pre-compile mark because marks end in order and several scripts park at
once. Releasing the compile cells later needs each cell's ranks. With only the known system
dimensions, a closed-region free reclaimed 2-3 of 27 compile cells (the rest sit in spin/grab/step
ranks shared with cells outside the program), so the attempt was dropped rather than risk cutting a
foreign rank. **Proposal P7 (needs approval):** `ArenaManifold::detachAll(CellRef)`, which unlinks a
cell from every rank it sits in while joining its neighbours, so `freeCell` can reclaim a program's
region on completion; then re-run the 10^4 test as a bound.

## MINOR findings

| finding                                              | disposition                                                                                                           |
| ---------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------- |
| vortex 1 submit hook overwritten / dtor clears other | Fixed: a second runner on a VM throws `std::logic_error`; test `ASecondRunnerOnTheSameVmIsRefused`.                   |
| vortex 2 arm ordinal not validated                   | Fixed in the VM (`submissionArmLimit`); test `AnUnknownArmOrdinalIsATypedStepFailure`. Payload tag check not added.   |
| vortex 3 request left after failure post-admission   | Fixed: `admit` retires an unreadable submission; failed and abandoned activations retire their actions.               |
| vortex 4 double opcode lookup                        | Skipped (micro-cost, no measurement).                                                                                 |
| vortex 5 VM-neutrality test                          | Skipped: the existing source-scan test covers the stepAwait..stepImpl text; a Submit-specific scan is not added.      |
| vortex 6, 7 header text, diagnostic wording          | Skipped.                                                                                                              |
| vortex 8 runner destroyed mid-batch test             | Skipped.                                                                                                              |
| vql 1 `def submit`                                   | Fixed in the parser; `AReservedNameCannotBeDefinedEither`.                                                            |
| vql 2 section 4.13 multiplicity wording              | Fixed.                                                                                                                |
| vql 3 pre-flight narrower than documented            | Docs corrected (plan and 4.13); the pre-flight itself is unchanged.                                                   |
| vql 4 statement index on all `VQL_SCRIPT_FAILED`     | Skipped.                                                                                                              |
| vql 5 payload failure silent, encoding               | Fixed: unreadable payload and non-UTF-8 bytes are `VQL_SCRIPT_FAILED`, stated in 4.13 (no dedicated unit test).       |
| vql 6 `:open` spaces                                 | Fixed; e2e `ReplOpensAPathWithSpaces`. `bindFile` deadline and `lastResults` naming retired cells: skipped, recorded. |
| vql 7 examples doc runnable, `--dump-ast`            | Skipped.                                                                                                              |

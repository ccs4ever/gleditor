# Round 2 disposition (M7-VQL-c gate)

Target `cb194c92`. Four lanes: VQL 0/0/1 (CONVERGED), Vortex 0/1/2, engineer 0/0/3 (CONVERGED),
zigzag 0/1/3. Both MAJORs were reproduced by the critics and are confirmed by the new tests failing
against the old `vquery`. No core file changed (no proposal `P10+`). Headless env throughout.

## MAJORs

### Vortex M1: a stale write source wrote "stale handle" into the file: fixed

The marker was an untagged text cell, so `sourceBytes` took it for text. It is now an
`IoCellTag::StaleBinding` cell (`makeStaleBindingCell`, runtime-only like every I/O cell), the form
zigzag m2 preferred, so no untagged-text consumer can mistake it for a value. Refused by tag with
`VQL_IO_STALE_BINDING` at:

- a write's source, both the script pre-flight (`staleName`) and `sourceBytes`;
- a `submit` capability name (`capability()`), a capture's variable (pre-flight and `captureBegin`),
  and an offset or length operand (`number()`);
- a persisted row: the marker has a tag that is not `Outcome`, so `refuseDurableAction` answers
  `VQL_IO_PERSIST_HANDLE`;
- every relation provider (now by tag, not by cell identity).

Tests (fail-before against the old `vquery`: all failed):
`AStaleNameAsAWriteSourceIsRefusedAndWritesNothing`, `AStaleNameIsNeverAPersistableRow`,
`AStaleNameGivenToCaptureOrSubmitIsATypedRefusal`, and the unit
`IoCellsTest.theStaleBindingCellHasATagNoReaderTakesForText`.

### Zigzag N1: a plain `let $y := $ra` aliased the reused slot: fixed

`noteBindings()` ran only in `finish()`. It now runs before every retirement (`retireFinished()`
replaces the three `retireAll; freeFinishedRecords; reconcileBindings` sequences), at `run` and
`resume`, in the head of every relation provider, and after each plain REPL line (the driver calls
it, `ScriptRunner::noteBindings` is public). The provider wrapper also refuses an argument whose
recorded incarnation no longer matches (`namesFreedHandle`), so a slot freed and reused by a pump
inside one script cannot alias either (engineer m1), and rebinds the names in the same pass.

Tests (fail-before: both failed, `$y` answered `EndOfStream` for the stranger):
`ACopyMadeByAPlainLetGoesStaleWithItsHandle`, and `ACopyMadeByALetWithNoQueryAfterItStillGoesStale`
(no provider call between the `let` and the next script, so only the `run()` entry note catches it).
A mid-script pump free needs a service reap timed inside one script and has no deterministic test;
the incarnation check in the wrapper is covered by reading.

## Owner decision: `:forget` (2026-11-26)

`ScriptRunner::forget()` and the REPL command `:forget`.

Semantics chosen: when any exposure is uncovered, **unbind every session variable that is not a
runtime I/O handle** and remove the **uncovered** exposures (covered ones stay, they are durable).
Printed: `Forgot N uncaptured payload(s); unbound $a $b`. The last result rows are cleared, since
they may be built from the dropped text, so `:save` needs a fresh query. With every exposure covered
it prints `Nothing to forget` and changes nothing.

Why all non-handle variables and not "those bound at or after the first uncovered exposure": the
rule is coverage, not taint, so neither the runner nor the engine knows which variable carries
uncaptured text; a `let` can mix covered and uncovered pieces through a pure function. Variables
carry no ordering the runner can trust (a name can be rebound), so a "since first exposure" cut
would be a guess, and a wrong guess lets uncaptured bytes into a store. Handles (capabilities,
outcomes, receipts) hold no text; reading a payload through one exposes it afresh and the rule
counts it again, so keeping them cannot leak. Price: a covered text variable the author wanted is
unbound too and is rebuilt by a `let`. Needs `VQLEngine::eraseVariable` (new, not a core file).

Tests: `ForgetLiftsTheRefusalAndUnbindsTheUncapturedText` (uncovered exposure refuses `:save`;
`:forget` prints `unbound $t`; a fresh plain query then saves; `$t` no longer prints the payload),
`ForgetNeverReachesWhatACaptureCovered` (captured data and its variable survive, `:save` works),
`ForgetKeepsCapabilitiesAndOutcomeHandles`, `ForgetWithNothingReadSaysSo`. Fail-before: all but the
vacuous handles case failed (`:forget` unknown).

Docs: `vql-query-language.md` §4.13 and change row 16.15; contract §4.7 (new bullet), §18.8, §20
(Q9, owner decision 2026-11-26); REPL help line.

## MINORs

- engineer m3 and vortex m2: the marker is minted by `mintStaleMarker`, after the Submit-hook check
  (no leak on the throw). At `~ScriptRunner` any variable still naming a freed handle is rebound and
  every variable naming the marker is unbound (`forgetStaleVariables`) before the marker is freed,
  so the longer-lived `VQLEngine` never holds a name for a freed cell. Fixed. No test: it needs a
  second runner on one engine and a destructor; reasoned in the code comment.
- vortex m1, zigzag m1: stale names given to `submit` and `capture` answer the typed code
  (`AStaleNameGivenToCaptureOrSubmitIsATypedRefusal`).
- vql m1: a plain-line stale refusal prints from the code onward (`VQL_IO_STALE_BINDING: ...`); the
  engine's `RelationNext:` prefix is dropped. Asserted by the same tests (`RelationNext` absent).
- vql note, orphaned doc comment above `parseReconcile`: moved to `captureReasonName`.
- engineer m2: stated in contract §4.7 (one `PayloadRange` per distinct exposed range, linear
  `expose` and check, not coalesced because coverage is asked per range, `:forget` drops the
  uncovered part). Coalescing deferred.
- zigzag m3 (`--in-place` piece order): still recorded as a limitation, not a defect; zigzag m5
  (uncaught `invalid_argument` from `writeResultSlice`) could not be reached by the critic and is
  left unverified.

## Verification

`xuzz_test` in full (5182 passed) and `zigzag_test` in full (269 passed), clang-format-19 on every
changed C++ file, mdformat and mdl on the changed markdown.

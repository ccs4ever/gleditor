# Round 1 disposition (M7-VQL-c gate)

Target `dcce529b`. Four lanes: VQL 0/1/4, Vortex 0/1/4, engineer 0/2/7, zigzag 0/2/7. Every BLOCKING
and MAJOR was checked against the code. All four consolidated MAJORs are confirmed and fixed;
nothing needed a core file change (no proposal `P10+`). Headless env throughout.

## MAJORs

### A. Teardown use-after-free and lost captures (vortex M1, engineer M1 and m3): fixed

Confirmed by reading and by a failing test: with the caller's token dropped, `parked_` was the
activation's only owner, `parked_.clear()` destroyed it before the `live_` weak pointers were
locked, and the capture stayed in `CaptureHost::runs_` with no pin (`liveRuns=1 pinned=0`).

- `~ScriptRunner` now locks every live activation into a local vector before it clears `parked_` and
  runs its close, stop, retire and capture-release loops over that vector.
- `FileReadHost::~FileReadHost` calls the new `CaptureHost::abandonAll()` while the service is
  alive. `~CaptureHost` then holds no runs, and a run still present drops its cells only
  (`freeRun(run, serviceAlive)`), never touching `IoService`, so the dangling `service_` reference
  cannot be used.
- Tests: `ADroppedParkedTokenStillPinsAtRunnerDestruction` (fail-before: pins 0, liveRuns 1; after:
  pins 1, liveRuns 0). `DestroyingTheHostWithALiveCaptureIsSafe` is a canary for the member order;
  no ASan or valgrind is installed here, so it cannot fail without a sanitizer (said plainly).
- Refused close at destruction (engineer m4): deferred. `reapWhenEnded` takes the close request and
  endpoint of an admitted close; a refused close has neither, so there is nothing to hand over, and
  retrying needs a host pump the destructor does not have.

### B. `:reconcile` parsing (engineer M2, vql m1): fixed

Confirmed: `promote 100`, bare `promote` and `promote 1 2 3` all decided. `parseReconcile` takes the
whole line: `release`, `promote`, or `promote START LEN`, numbers parsed with `from_chars` with
nothing left over; anything else prints the usage line and decides nothing. Test
`ReconcileRefusesAHalfTypedOrOverlongDecision` (fail-before: 1 usage line of 4).

### C. The persist gate is per line (vql M1, zigzag M2): fixed, owner rule Q2

Confirmed with the REPL: `let $t := io_payload($r)` then `return $t` then `:save` wrote the bytes; a
capability row passed on a plain line and in batch `--io-file src=f -o out -e 'return $src'`.

- The exposure set is the runner's and accumulates across every script, and across plain queries
  that reach the runner's relation providers (`expose` no longer needs a current activation).
  `persistability()` is computed at the call, so a capture committed later lifts the refusal for the
  ranges it covers. Derived text stays allowed once covered.
- `refuseDurableAction` has no `resultsFromAction` early return: handle rows are refused on any line
  (before the coverage check), then the whole-session verdict is checked.
- Consequence, now stated in the contract and §4.13: an exposure whose read was retired before it
  was captured can never be covered, so it refuses persistence for the rest of the session. The
  author captures inside the script that reads, or restarts. `AQueryAfterAnActionIsSavableAgain`
  encoded the old per-line rule; it became `AQueryAfterAnActionIsRefusedWhileItsExposureIsUncovered`
  (owner-directed change), and six `VqlIoPersistabilityTest` cases were re-pointed at the session
  rule (the "covered" half now runs in a fresh world).
- Tests: `ExposedTextCarriedInASessionVariableIsNotPersistedLater`,
  `ExposedTextBecomesPersistableOnceACaptureCoversIt`,
  `ACapabilityRowIsRefusedOnAPlainLineAndInBatch`,
  `TextCarriedInAVariableStaysExposedAcrossScripts`. Fail-before against the old `vquery`: all three
  e2e cases failed.
- Docs: contract §4.7 ("Session scope") and §18.7; `vql-query-language.md` §4.13 and change row
  16.14.

### D. Stale session variables alias later records (zigzag M1, vql m3, m4, m7): fixed, typed refusal

Confirmed in the REPL: `return io_tag($a)` printed the second script's outcome, and a stale `$c`
produced `RelationNext expects ...`.

Choice: the typed-stale refusal, not keeping records alive. Keeping a record alive while any
variable names it is unbounded (a name may be kept for ever) and would hold payload leases and
request slots the retention budget exists to free; the arena's incarnation facility makes the
refusal exact.

- `ScriptRunner::noteBindings()` records the incarnation of every runtime I/O handle a session
  variable names when a script ends. `reconcileBindings()` runs at `run`, `resume` and after each
  admission's reaping; a variable whose handle died is rebound to a runner-owned marker cell, so a
  reused slot is never read under the old name.
- Every relation the runner installs refuses the marker with the registered `VQL_IO_STALE_BINDING`
  (typed in a script, code-prefixed text in a plain query). It covers `io_tag`, `io_bytes`,
  `io_payload`, `io_captured`, `capture`, `await`, `cancel`, so the internal `RelationNext` text is
  gone (vql m7).
- vql m4: `io_bytes`/`io_payload` of a finished script's record whose lease went back is
  `VQL_IO_STALE_BINDING` (`OwnedOutcome::payloadReleased`), not `0` or nothing.
- vql m3: `await($h) as $r` still names the service outcome and is not copied to a record; its slot
  is freed at the next admission like any other, and the same check now makes its stale name typed.
  Making it an owned record would also retire `$h`'s request, which is a behaviour change, so it is
  deferred (§19 note).
- Tests: `VqlIoStaleBindingTest` (three cases; fail-before: no typed code, `io_bytes` read nothing
  silently), `AStaleOutcomeNameIsATypedRefusalNotAStrangersRecord`,
  `AStaleReceiptNameIsATypedRefusal`.
- The marker is one cell minted with the runner and freed with it; minting it lazily moved the leak
  tests' slot high-water mark by one.

## MINORs

Fixed (6):

- engineer m1: a trailing-slash write root that does not exist yet (`ARootWithATrailingSlash...`,
  fail-before confirmed).
- engineer m5: the capture directory's newly created components have their parent entry fsynced
  before the journal's first intent.
- engineer m6: `sweepStepGraphs` frees a never-admitted step no capture still names, so a failed
  immediate free is retried.
- zigzag m1: `owned_` collision releases the old entry's lease and budget instead of
  `insert_or_assign` dropping it (no test: needs a VM rollback that reuses a record slot).
- vql m2: the `$name is not bound` text now says a `let` of the same script is not visible to its
  actions.
- vortex m3 and m4: `CaptureHost::release` logs a `HasWaiters` refusal, and its comment states the
  timeout exception.

Deferred, with reasons:

- vortex m1 (pin only when the append may have run): a conservative pin costs a `:reconcile`; the
  decision needs the §11 phase analysis, not a quick edit. vortex m2 (outcome of a failed step copy
  under budget pressure): needs a payload-less detached outcome constructor.
- engineer m2 (`openat2(RESOLVE_BENEATH)`): documented residual race; a larger change for a later
  gate. engineer m4: see A. engineer m7: grant-race test depends on m2.
- zigzag m2 (observation as cells), m3 (one reachable-graph walk), m4 (`--in-place` piece order), m5
  (one captured-cell predicate), m6 (assert no clone rank): design or refactor decisions, not
  defects at this target.
- vql m2 resolution (script-local `let` as a write source) and the vql note on `capture` being
  contextual: recorded for §19 and §3.

## Verification

`xuzz_test` and `zigzag_test` in full (see the commit message for counts), clang-format-19 on every
changed C++ file, mdformat and mdl on the changed markdown.

Round 2 should look at: the stale-marker rebinding (a variable written back through
`VQLEngine::setVariable`), the session-scope poison consequence, and the destructor order.

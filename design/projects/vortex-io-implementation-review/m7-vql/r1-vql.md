# Round 1 critique: VQL Expert (Vortex I/O M7-VQL, language correctness and fit)

Target: `8711aa1c` (diff `ef74939f..8711aa1c`, VQL paths). Read-only static review; nothing was
built or run. Tally: BLOCKING 0, MAJOR 1, MINOR 7.

## What was checked and holds

- Grammar: `effect` is recognised only as identifier-then-`{` at statement start
  (`parser.cpp:2235`). No existing test, sample, stdlib or app source uses `effect`, `submit`,
  `await` or `read_bytes` as a VQL call or identifier (the `effect` hits elsewhere are
  `WeaveStatement`/evidence workflow/vagent, separate grammars). The fixed shape is unambiguous: it
  is a new `Statement` alternative, both arms are required, and the matched/awaited/kept variables
  must be the ones just bound (`parser.cpp:2281-2358`).
- Static placement: top-level only through `nextBodyIsScript_`/`effectsAllowed_`
  (`parser.cpp:121, 148-160`). Nested `def`, `watch`, perspective and lambda bodies all go through
  `parseScriptBody` with the flag off (`:243,290,407,2012`) and are refused with
  `VQL_IO_QUERY_CONTEXT` (`:173`). The `for`/`let`/`where`/`if` action position is refused in
  `parseActionClause` (`:1304`). Every call site of a function-like name funnels through
  `parseFunctionInvocation`, which refuses the three names (`:1675`). The other `parseQuery` callers
  (`vql_engine.cpp:216,233`, `contract_codec.cpp:341`, `cell_path.cpp:152`) either run
  `QueryExecutor::execute`, which refuses a script with an action (`execution.cpp:128`), or accept
  only a path. Perspectives and watches cannot hold an action.
- Multiplicity: one `Submit` per action site in straight-line bytecode, after the preceding stretch
  has already looped (`compile_io_effect.cpp`); resume continues the same cursor (`execution.cpp`
  `resume`/`advance`). The tests prove once-not-per-row (`vql_io_script_test.cpp:300`), ordering,
  N-runs-N-submits, and same-request resume.
- Terminal outcomes are rows: a refusal takes the `refused` arm and the script goes on; every tag is
  an outcome row; `QueryError`/empty-rows are not used for them (`script_runner.cpp` `finish`; tests
  `:400,:424`).
- Whole-script argument validation does not break VQL semantics: a script already compiles as one
  program, so a later-statement compile error already precedes an earlier statement's execution, and
  scripts without an action never reach the runner.
- Parity risk from the `QueryExecutor` refactor (`ProgramRun`, `begin`/`advance`): the non-action
  path keeps the same mark/trail/contract-coordinator/prose-hits unwinding order on success, failure
  and exception; I found no divergence by reading.

## BLOCKING

None.

## MAJOR

### M1. The CLI persistence paths (`-o`, `--in-place`, `:save`) were not audited and can durably store file bytes

**Problem.** The M7-VQL gate requires "vquery/vqueryc CLI host I/O audited", and the standing rule
is that input is transient unless explicitly captured (plan §2, `CaptureCoordinator`). An action's
value is the outcome cell plus a payload text cell minted with `arena().makeCell(text)`
(`script_runner.cpp` `finish`, the `rows.push_back(...makeCell(text))` line). In `vquery` those rows
become `lastResults` (`main.cpp:575-596`), and the same run can then be written by `--in-place`
(`zigzag::promote` of every ephemeral result into the primary store, `main.cpp:656-675`), by `-o`
(`writeResultSlice(outStore, resultRows(...))`, `:690-697`, appending to the author's permascroll
through `Store(permascroll)`), and by `:save`. None of the three consults the capture journal,
source interval or range, and the plan's CLI-audit note (plan, "CLI audit" bullet) lists only the
read path.

**Failure scenario.**
`vquery --io-file k=/path/secret.txt --in-place -e 'effect { let $s := submit($k, read_bytes(0, 4096)); match $s { admitted($q) => await($q), refused($e) => $e } }'`.
The payload text cell is an ordinary ephemeral text cell, so `promote` copies its bytes into the
primary store and the author's permascroll (a store that is later publishable). The data is now
permanent with no `CaptureReceipt`, no provenance and no "explicit capture" decision, contradicting
"No automatic capture occurs" (frontend-examples doc, VQL section: "the VQL snippet does not make it
permanent"). The outcome cell, a managed I/O cell, is also a `promote`/`resultRows` input and its
behaviour there is untested. I could not run this to confirm what `promote` does with the managed
cell, so the outcome-cell half is unverified; the payload-cell half follows from the code above.

**Evidence.** `apps/vquery/main.cpp:575-596,656-675,690-697`;
`apps/common/xanadu/vql/script_runner.cpp` `finish` (payload `makeCell`); no test in
`vquery_e2e_test.cpp:356-451` combines an action with `-o`/`--in-place`/`:save`.

**Fix.** Either (a) refuse, with a typed code, to persist rows of a script whose final statement is
an action (`-o`, `--in-place`, `:save`) until a real capture route exists, or (b) record in the plan
that `--in-place`/`-o` are the explicit capture and route them through `CaptureCoordinator` with
source provenance. Add an e2e test that pins whichever is chosen, and add the persistence paths to
the CLI audit bullet.

## MINOR

1. **A reserved name can still be defined, but then never called.** `def submit(...)` parses
   (`parser.cpp:387-410`; only `parseFunctionInvocation` refuses, `:1675`), so a user gets a clean
   definition and a `VQL_IO_QUERY_CONTEXT` refusal at every call. The compatibility change is
   documented (plan note, 16.2), but refuse the name at `def` with the same code. The only
   regression test is `def submit_all` (`vql_io_script_test.cpp:167`).
1. **§4.13 contradicts itself.** The "Multiplicity" paragraph still says "The runner cuts the script
   at its actions; every stretch ... runs through the Vortex VM", while "Bytecode form" says "The
   whole script is one program" (`design/vql-query-language.md`, §4.13). Strike the first-cut
   wording; 16.2/16.3 history already carries it.
1. **Pre-flight validation is narrower than documented.** The plan and §4.13 say a bad later action
   "fails the script before an earlier statement runs". `ScriptRunner::run` checks only
   capability-ness and non-negative integers (`script_runner.cpp:99-110`). The retention ceiling and
   file-capability-ness are checked at stage time inside the VM (`file_read_host.cpp:146-156`), so
   `effect{...read_bytes(0, 1)}; effect{...read_bytes(0, 999999999)}` submits the first read and
   then fails the script with `VQL_IO_EFFECT_ARGUMENT`. Harmless (reads are idempotent) but either
   extend the pre-flight to the staged-read bounds or correct the sentence. The test named
   "...BeforeSubmit" (`:802`) covers only a single action.
1. **`VQL_SCRIPT_FAILED` carries a position only for failures at an action.** §4.13 says a failing
   query stretch reports "with the statement index"; `advance` maps `failingClause` only over
   `ioActions` (`execution.cpp`, failure branch), so a runtime failure in an ordinary statement has
   no `position`, and the QueryError category survives only inside the message text
   (`script_runner.cpp` `settle`). Compile-time failures do get an index (`failingStatement_`).
1. **Payload read failures are silent.** If `payloads.retain`/`read` fails, `finish` returns the
   outcome row alone, indistinguishable from a payload-less tag (`script_runner.cpp` `finish`, the
   `if (lease)`/`if (bytes)` fall-throughs). Raw bytes are also narrowed into a text cell, so binary
   data and digit-only files take the text-inference path. Return a typed diagnostic (or a row
   naming the failure) for a `Complete` outcome whose payload cannot be read, and state the
   text-cell encoding in §4.13.
1. **vquery UX edges.** (a) `bindFile` loops until the open completes with no deadline of its own or
   cancel (`main.cpp:517-531`); a stuck endpoint hangs `:open`/`--io-file` (plan notes only that
   reads have no timeout). (b) `:open NAME PATH` splits on whitespace so a path with spaces is
   silently truncated (`main.cpp:792-800`). (c) One shared `VortexVM` replaces the per-query VM for
   every plain query (`main.cpp:493`; the plan justifies it for the write guard), so cross-query VM
   state now persists; add one regression that runs a failing query and then a plain query on the
   same session. (d) A later script's `admit` retires the previous action's request
   (`script_runner.cpp` `admit`), but `lastResults` keeps the old rows until the new script
   succeeds, so `:save` after a script that fails post-admit can name retired cells.
1. **Deferred surface is honest but thin for the examples journey.** An action's value renders as
   its tag word only ("Complete\\nworld", e2e `:381`), so a `Failed` reason, progress or uncertainty
   is not visible in `vquery`; later statements cannot name the action value or its bytes; there is
   no outcome-tag match (frontend-examples doc VQL block). None of this breaks the M7-VQL acceptance
   list, so it is not blocking, but the examples doc should say its VQL block is not yet runnable as
   written, and `--dump-ast` of an effect-first script shows the default empty `expr`
   (`parser.cpp:128-138`) rather than the action.

VERDICT: NOT CONVERGED (one MAJOR, M1; all language-fit, placement, multiplicity and outcome-value
requirements otherwise hold. Resolve M1 and I will approve the gate.)

# Round 1 critique: VQL Expert (M7-VQL-c code)

Target `dcce529b` (diff `4d132ccf..dcce529b`). Tally: 0 BLOCKING, 1 MAJOR, 4 MINOR.

Lane covered: surface syntax and parse (`open ... for write|append [create]`, `write_bytes`,
`append_bytes`, `sync`, `capture`), compile lowering (`compile_io_effect.cpp`: the capture loop,
`io_own_outcome`, `io_open` mode), `ScriptRunner` providers and pre-flight, the persist coverage
rule, and the vquery CLI/REPL. Checked clean (built `build/vquery` at the target, headless):
`capture($r, off, len)` boundaries (`0,0`, `11,0`, `12,0`, `0,99` are `Rejected`; `5,6`, `0,11` are
receipts); the capture loop never re-runs `io_capture_begin` (it re-enters at the `Await`); a
`capture` of a text `$t` is a typed `VQL_IO_EFFECT_ARGUMENT`; `return io_payload($r)` and
`return $r` of a read are refused by `-o` (the existing tests hold); `sync`/`write_bytes`/
`append_bytes` are refused in query context by name; `:open -w|-wc|-a|-ac` flag parsing; the
`--io-write-root`/`--io-write-bytes`/`--capture-dir` flags are in `reorderArgs`.

## BLOCKING

None.

## MAJOR

### M1. The persist rule is checked only for the script that ran an action

**Problem.** `refuseDurableAction` begins with `if (!resultsFromAction) return false;`
(`apps/vquery/main.cpp:766`), and `resultsFromAction = ranAction` where `ranAction` is only
`hasIoEffect(ast)` of the *current* line (`main.cpp:814`, `:848`). `persistability_` is likewise a
fact about "the most recent finished script" (`script_runner.cpp:1680`), and `expose()` records into
`current_->exposures` of one activation. Nothing carries an exposure, or the fact that a row is a
runtime handle, across scripts, although the REPL carries bindings across them (`as $r`, `let`
publish session variables: `execution.cpp:660-664`) and `:open` / `--io-file` bind capabilities with
`engine.setVariable` before any script runs. A plain query (no `effect`) takes the `QueryExecutor`
branch (`main.cpp:826-838`) and is never checked, and an action script is checked only against its
own exposures.

**Failure scenario** (all reproduced with `build/vquery`, headless, nothing captured):

1. Derived input text. REPL: `:open src f.txt`, then
   `effect { ...read_bytes(0, 64)... } as $r; let $t := io_payload($r); return 1`, then `return $t`,
   then `:save out.store`. Output: `hello world`, `Saved 1 result rows`; `out.store` exists and
   holds the file's bytes typed in. The script that exposed the payload had an uncovered exposure,
   but it was not the script whose rows were saved. This is exactly the "input is transient unless
   explicitly captured" rule (design §4.7) the rule exists to enforce, and
   `ActionRowsAreNotWrittenByOutputStoreInPlaceOrSave` pins the same data as refused one statement
   earlier.
1. The same through an action script: script 1 as above publishes `$t`; script 2 is
   `effect { open("g") } as $f; return $t`. Script 2's `persistability_` has zero exposures, so it
   is "vacuously covered" and `:save` accepts the file's bytes.
1. The new `VQL_IO_PERSIST_HANDLE` refusal (`main.cpp:773-784`) sits inside the same gated branch.
   `:open src f.txt`, `return $src`, `:save o4.store` writes the row `io capability`. After a
   capture script, `return $c` then `:save` writes `io capture receipt`. Batch form:
   `vquery --io-file src=f.txt -o out.store -e 'return $src'` exits 0 and writes `io capability`
   (the test `PersistingTheReceiptItselfIsATypedRefusal` covers only the receipt returned by the
   same script that made it).

**Evidence.** `apps/vquery/main.cpp:764-801` (`resultsFromAction`, the early return), `:814` /
`:848` (set from the current line), `:826-838` (plain queries skip the runner), `:914` and `:1251`
(the two call sites); `script_runner.cpp:1606-1610` (`expose` writes only `current_->exposures`),
`:1680` (one verdict per finish), `script_runner.hpp` `persistability_` (single optional,
overwritten by the next script); `execution.cpp:660-664` (published session bindings). Design §4.7
says "`persistability()` answers for the most recent finished script", and the exposed text outlives
that script as a session variable.

**Fix.** Make the verdict a property of the session, not of the line: (a) have the runner keep a
cumulative set of uncovered exposures (or a taint on the session variables that were derived while
an uncovered exposure was live) and answer `persistability()` over everything whose binding is still
alive, resetting only when the covering capture commits; (b) in `refuseDurableAction`, drop the
`resultsFromAction` early return for the handle check (a capability, request or receipt row is never
persistable whatever ran last) and run the exposure check for every query once the runner exists (a
session that never built a runner has nothing exposed). Add tests for scenarios 1-3 and the batch
`--io-file ... -o -e 'return $src'` form.

## MINOR

### m1. `:reconcile` silently reinterprets a half-typed or over-long irreversible decision

**Problem.** After `iss >> start >> length` fails the code takes `else if (iss.eof())` as "no span
given" (`main.cpp:1183-1188`). For `:reconcile 3 promote 5` the first extraction succeeds, the
second hits EOF (failbit and eofbit), so the decision becomes `promote` with no span and the `5` is
dropped. Trailing tokens after `release` or after a full `promote S L` are also ignored. **Failure
scenario.** An author who meant `promote 5 11` and forgot the length promotes the journal's own span
(or whatever the coordinator infers), a durable and irreversible choice, with no usage line.
`ReconcileNamesItsFormWhenGivenOnlyHalfOfIt` covers `:reconcile 1`, not this half. **Fix.** Parse
the remainder as a whole: no tokens, or exactly two integers, and nothing left over; otherwise print
the usage line.

### m2. A byte source or offset bound by a `let` of the same script is refused as "not bound"

**Problem.** The pre-flight accepts a `$name` only if an earlier *action* of the script named it
(`named`) or it is already a session variable (`script_runner.cpp:1040-1054`, same for offsets in
`operand`, `:1107-1111`). A `let` earlier in the same script is neither, so `let $t := "hello"`
followed by an `open(".../out") for write create` and a `write_bytes(0, $t)` fails with
`VQL_IO_EFFECT_ARGUMENT: $t is not bound to exactly one cell to write` (reproduced; also with
`let $t := io_payload($r)`, and with the `let` after the open). Design §3/§4.2 offers
`write_bytes(off, $text)` and calls a `$variable` "bound to one cell (the same rule the read offset
uses)". The read offset has the same limit, so this is consistent, but the message is false (the
variable is bound) and the first-use journey "write a report built in the script" is unreachable in
one script: the author must end the script, so the text becomes a session variable that no longer
belongs to any exposure check (see M1). **Fix.** Say "is bound only inside this script; bind it in
an earlier script or use `as`" in the message, and record the limit in §19 deferrals; or resolve
script-local `let` cells at the Submit.

### m3. A named `await`/`cancel` outcome dangles as a session binding after the next admission

**Problem.** Only `Kind::Read` is copied to an owned record (`compile_io_effect.cpp:305`, recorded
as C1b deviation 1). `effect { await($h) } as $r` therefore binds the service's outcome cell, which
`retireAll(finished_)` frees at the next admission (`script_runner.cpp:1255`, `:1090`). **Failure
scenario** (reproduced): REPL: a detached read
`as $h; effect { await($h) } as $r; return io_tag($r)` prints `Complete`; run any other read; then
`return io_tag($r)` fails with
`RelationNext expects found, position and limit outputs plus one answer slot per argument` and
`return io_bytes($r)` with `io_bytes: the argument is not an outcome` (the freed slot was reused). A
reader cannot tell this is a stale name. **Fix.** At minimum make `liveOutcome`/`io_tag` of a dead
or reused cell the same typed message (`$r no longer names a live outcome`); better, copy the
awaited outcome into a record when the request has no other outstanding handle.

### m4. `io_bytes($r)` of a record carried to a later script answers `0`

**Problem.** `gated()` supplies a record's payload only while its lease is held
(`script_runner.cpp:1144-1156`); the lease is returned at script end (`finish`, `:1650`). Later,
`io_bytes` falls through to `outcomeField(..., "transferredBytes")`, which the design itself records
as `0` for a backend that streams no progress (§18.2 deviation 3). **Failure scenario**
(reproduced): `read_bytes(0, 64)` of an 11-byte file `as $r; return io_bytes($r)` prints `11`; in
the REPL the next line `return io_bytes($r)` prints `0`, and `return io_payload($r)` prints nothing
at all. Complete read, no bytes, no refusal: a silent wrong value where the contract (§4.6, "a
capture of a retired outcome is `Rejected(Unresolved)`") wants a typed answer. **Fix.** Copy the
payload length into the record's `Progress` rank at copy time, and make `io_payload` of a record
whose lease is gone a typed failure rather than "no row".

## Notes

- The REPL `:open -w|-wc|-a|-ac` flags only ask; the host grant decides (verified: with no
  `--io-write-root` the open is `Open failed`), and no script path widens rights after the open
  (`rightsOf` is the only mapping, `script_runner.cpp:37-49`).
- `capture` is contextual (parse-time `peekToken` of `(` inside `effect {`), not reserved as design
  §3 says ("become reserved call names"); `sync` is reserved and refused as a `def` name. The code
  is defensible; update §3.

VERDICT: NOT CONVERGED

# M7-VQL-c: VQL write and capture

Status: design note; stage C1 (adapter and host, §18) is built, the rest is not. It specifies the
third M7-VQL slice of the [Vortex I/O implementation plan](vortex-io-implementation-plan.md): script
**write effects** to a file endpoint, and **capture**, the explicit act that makes a script's input
durable so results built from it may be persisted. It extends
[`vql-query-language.md`](vql-query-language.md) §4.13 and changes no on-disk format and no core
file (§14 lists the proposals considered and refused).

Where it lives. The plan's milestone table has rows for `M7-VQL` and no row for the `-b` or `-c`
slices; slices are recorded as "implementation notes and deliberate deferrals" under the M7-VQL
block (plan lines 1214 and 1329). This note follows that convention and carries the design; the
plan's M7-VQL block gains a short "M7-VQL-c (design)" pointer to it, and VQL's change history gains
a row 16.9.

## 1. Scope

**In.** (The owned-record form of §12.1 is in, by owner ruling.)

1. **Write effects.** Writing bytes to a file endpoint opened for writing, through the existing
   `FileEndpointAdapter`/`FileSourceAdapter` write path and `IoService`: `write_bytes` (positional),
   `append_bytes` (O_APPEND), and `sync` (a separate durable-sync request). Opening for write is a
   capability grant the invoking human makes, never something a script grants itself.
1. **Capture.** `effect { capture($r) } as $c` routes a read outcome's retained payload through
   `CaptureCoordinator` (plan §8.2): append the uncovered bytes to the author's one permascroll,
   record the journal, publish the receipt and spans into a capture store. Its value is a receipt
   (or the refusing outcome) and, through `io_captured($c)`, cells whose content *is* the permanent
   span.
1. **Lifting `VQL_IO_PERSIST_NEEDS_CAPTURE`** for a script whose every exposed payload range is
   covered by a committed capture, and only then (§4.7).
1. Their host wiring in `vquery` (write roots, capture directory, `:captures`, `:reconcile`), typed
   refusals, crash ordering, bounds, tests and stages.

**Out (deferred, §19).** `message` and frame modes; per-row or looped effects (still
`VQL_IO_QUERY_CONTEXT`); detached or cancellable capture handles; `truncate`; compare-and-swap
writes; streaming copy of more than one write bound; capture of a *write*'s bytes (there is nothing
transient in a write: it is script-authored); a settings xanadoc (M8); non-file endpoints; writes
from `vqueryc`, which is compile-only and stays so.

**Standing rules this design keeps.** Input is transient unless explicitly captured (plan §2, §8.2).
Capture is never implicit during read or render. No automatic retry after `Unknown`, no resubmission
of an admitted request, no blocking on the evaluation thread, no synchronous fallback (plan §14).
Admission refusal is a value, not an error. Nothing here is a sentinel: absence is `std::optional`,
refusal is `std::expected`.

## 2. What exists, and what is missing

| Fact                                                                                                                      | Where                                                                                                                                                      |
| ------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------- |
| The file adapter already pwrites: `FileOpSpec::Kind::Write`, frozen `writeBytes`, `stageAt`                               | `file_source_adapter.hpp:152-160`, `file_source_adapter.cpp:196-201`, `:325-380`                                                                           |
| Write outcomes: `Complete{transferred}`, `Cancelled{uncertain}` on stop, `Failed{BackendFailure}`                         | `file_source_adapter.cpp:364-380`                                                                                                                          |
| The request decoder knows `io.file.write` and declares **no payload bound** for it                                        | `file_endpoint_adapter.cpp:373-390`                                                                                                                        |
| An open's rights come from the **host's grant**, and `FileReadHost` grants read only                                      | `file_read_host.cpp:84-92`, `openSpec` at `:111`                                                                                                           |
| `fileRightWrite` opens `O_RDWR`; there is no create, append or sync                                                       | `file_source_adapter.hpp:146`, `file_endpoint_adapter.cpp:185`                                                                                             |
| A write's frozen bytes live in the adapter's staging table, not in the service budget                                     | prepared command is the 8-byte key (`file_source_adapter.cpp:271`); cost `io_service.cpp:259`; table cap 64 MiB / 4096 ops `file_source_adapter.cpp:22-30` |
| Lanes: writes on one endpoint run one at a time, FIFO; reads and writes of one endpoint never overlap                     | `io_service.cpp:290-345`                                                                                                                                   |
| `CaptureCoordinator` is complete and journal-ordered, but has **no caller outside tests**                                 | `capture_coordinator.hpp`; users: `tests/xuzz/io_capture_test.cpp` only                                                                                    |
| Coordinator drives its own `service_.submit` and returns a `SubmitResult`; it mints its own capability per append         | `capture_coordinator.cpp:1400-1520`                                                                                                                        |
| An abandoned capture is classified by `sweepDead`: dead cell in `AppendRequested` pins, earlier frees                     | `capture_coordinator.cpp:670-715`                                                                                                                          |
| Capture's bounds: 128 intervals, 1 MiB append, 1024 pieces, 32 pending, 8192 slots, 4 MiB journal                         | `capture_constants.hpp` (`capture_defaults`)                                                                                                               |
| The refusal to lift: any script holding an action refuses `-o`, `--in-place`, `:save`                                     | `vquery/main.cpp:614-625`, `:726`, `:917`; code at `frontend_diagnostics.hpp:58`                                                                           |
| Why: `promote()` spools scratch bytes into the permascroll, so an uncaptured payload would be typed in with no provenance | `arena_manifold.hpp:1273-1279`; review `m7-vql-b/r1-vql.md` M1                                                                                             |
| A cell may quote a permanent span: `ArenaManifold::makeCell(const PrimediaSpan &)`                                        | `arena_manifold.hpp:927` region                                                                                                                            |
| Script providers: `io_tag`, `io_payload`, `io_bytes`, `io_error`, `io_detail`, handle arm, open                           | `script_runner.cpp:447-530`                                                                                                                                |
| Lowering of every action, one result slot, Submit once, Await                                                             | `compile_io_effect.cpp:100-219`                                                                                                                            |

Three findings from this reading shape the design.

1. **A write's bytes are not charged to the service.** The prepared command is only the staging key,
   so `AdmissionCost::commandBytes` is 8 whatever the write size. The only bound is the adapter's 64
   MiB aggregate, which is sixteen times the service's `budgetBytes` (4 MiB, `file_read_host.hpp`
   defaults). Writes must be charged where reads are (§10, stage C1).
1. **There is no authority model for writing.** `FileReadHost`'s grant lambda hard-codes read. A
   script that could ask for `for write` would be asking itself for authority. The grant is a
   host-supplied policy (§4.1).
1. **Capture is a three-request protocol, not one request.** `prepare` (owner, synchronous) then an
   append request, then one or two durable-step requests (`commit`, `continueCommit`), each awaited.
   The Submit opcode admits exactly one request through a capability and a description cell, so
   capture cannot be one `submit`. It is lowered as the existing relation-provider-then-`Await`
   pattern that `cancel` and `open` already use (§6).

## 3. Surface syntax

All new forms are top-level script statements, refused with `VQL_IO_QUERY_CONTEXT` wherever a
`submit` is (as an action of `for`, `let`, `where`, `if`, in `def`, lambda, `watch`,
`with perspective`, and as a call in any expression). Per row or per loop iteration stays refused
until a later gate. `write_bytes`, `append_bytes`, `sync` and `capture` become reserved call names
beside `submit`, `await` and `read_bytes`.

```vql
-- grant: the invoking program decides whether a path may be opened for write
effect { open("out/report.txt") for write create } as $o;

-- a write is a submit, exactly as a read is, so within, handles and match all apply
effect {
  let $s := submit($o, write_bytes(0, $text)) within 2 s;
  match $s { admitted($q) => await($q), refused($e) => $e }
} as $w;

-- appending to a log
effect { open("out/run.log") for append } as $l;
effect {
  let $s := submit($l, append_bytes($line));
  match $s { admitted($q) => await($q), refused($e) => $e }
} as $a;

-- a write is not durable until a separate sync says so
effect {
  let $s := submit($o, sync());
  match $s { admitted($q) => await($q), refused($e) => $e }
} as $d;

-- capture: explicit, and its own action
effect { open("data/in.csv") } as $f;
effect {
  let $s := submit($f, read_bytes(0, 65536));
  match $s { admitted($q) => await($q), refused($e) => $e }
} as $r;
effect { capture($r) } as $c;
return io_captured($c)
```

Grammar additions, in the notation of §4.13's grammar block:

```text
OpenEffect     := "open" "(" PathOperand ")" [ "for" OpenMode ]
OpenMode       := "write" [ "create" ] | "append" [ "create" ]
RequestDesc    := ... | "write_bytes" "(" Operand "," ByteSource ")"
                      | "append_bytes" "(" ByteSource ")"
                      | "sync" "(" ")"
ByteSource     := StringLiteral | "$" Name        (* a text cell, or an outcome *)
CaptureEffect  := "capture" "(" "$" Name [ "," Operand "," Operand ] ")"
```

Choices, each argued:

- **Writes reuse `submit`.** A write is a request on a capability exactly as a read is, so it gets
  `within`, `as`, the detached form (`admitted($q) => $q`), `await($h)` and `cancel($h)` for free
  from M7-VQL-b with no new statement kind. Refused: a `write(...)` sugar statement. It would
  duplicate the match and admission shape and invite a second multiplicity rule.
- **`capture` is its own statement**, like `open`, `await` and `cancel`, because it is not one
  request (§2 finding 3) and so has no `submit` form. It takes an outcome, not text: capturing the
  text a script printed would capture the text's scratch bytes, which are not the file's bytes and
  have no source observation to key overlap on (plan §9.3).
- **`ByteSource` is a text cell or an outcome.** A text cell is UTF-8 and may be a literal, a
  `$variable` bound to one cell (the same rule the read offset uses), or `io_payload($r)`. An
  *outcome* source writes its retained payload through the shared status gate, so a binary read is
  copied to a file byte for byte, which the text route cannot do (non-UTF-8 payload text is
  `VQL_SCRIPT_FAILED` today). Refused: `from_hex(...)` and a `bytes` literal; neither is needed to
  prove the contract and both add a lexer surface.
- **`for write [create]` and `for append [create]`** are modes of `open`, not of the write, because
  authority belongs to the capability, the capability to the open (plan §3.3), and rights are fixed
  when the endpoint is minted. A script cannot widen them later.
- **`create` means `O_CREAT|O_EXCL`**: fail rather than clobber. There is no truncate (§19) and no
  create-or-open: both destroy or hide data, and neither is needed to write a report.
- **`sync()` is a request description with no operands**, an own request with its own outcome, as
  the plan requires ("flush and durable sync are separate backend-specific requests", plan §7.1,
  redesign lines 700-712, the buffered-write and flush paragraphs).

## 4. Semantics

### 4.1 Authority: the write grant

`FileReadHostOptions` gains `writeGrant`, a function from the canonical locator to the rights it may
be opened with, empty by default (deny). `FileReadHost`'s `OpenGrant` lambda
(`file_read_host.cpp:84`) calls it: with no `writeGrant`, or when it returns read only, an open
`for write` or `for append` completes as a **`Rejected`** outcome (`IoErrorCode::Unsupported` detail
`1`, "no write grant"), a value the script sees, nothing started. `vquery` builds the policy from
`--io-write-root DIR` (repeatable): a locator is writable iff its canonical form
(`weakly_canonical`, so `..` and symlinked parents resolve first) lies under a named root. With no
`--io-write-root`, no script can write anywhere: the safe default, and the property a reviewer can
check by reading one flag.

The grant is checked on the owner thread at admission (a pure path computation, no I/O), so the
refusal costs no worker. A path that passes admission but is a symlink swapped in before the worker
opens it is outside what a path policy can promise; the open uses `O_NOFOLLOW` on the final
component for write modes (stage C1) and the file must be a regular file, as
`file_endpoint_adapter.cpp:185` already enforces. Host-bound capabilities (`--io-file`, `:open`)
stay read-only; `:open -w` and `:open -a` (§13) take the same grant.

### 4.2 `write_bytes(offset, source)`

One `pwrite` loop at a checked 64-bit offset over bytes frozen at submit (plan §7.1: "`WriteBytes`
freezes one buffer version at submit"). The runner's Submit hook (`ScriptRunner::admit`) resolves
the description: the offset operand as for a read, the byte source to an owned
`std::vector<std::byte>` (text cell: `arena.textOf` with the permascroll `SpanReader`; outcome: the
gated payload range, read through `PayloadResolver` and released), then
`FileEndpointAdapter::stageAt` with `FileOpSpec::Kind::Write`. **The frozen buffer is complete
before `submit` returns**, so no worker can observe a half-built one (plan §3.3 "output freezing
completes before a worker can observe it"). An empty source is a valid zero-byte write: `Complete`
with `transferred = 0`, never an error.

Outcomes, all values (plan §3.2 "Write/send/flush/sync/close" row; no payload):

| Terminal                            | Meaning for the script                                                                           |
| ----------------------------------- | ------------------------------------------------------------------------------------------------ |
| `Complete`                          | every byte was handed to the kernel. Not durable: see `sync`.                                    |
| `Failed{BackendFailure, errno}`     | the pwrite failed after `io_bytes($w)` bytes landed; the file holds that prefix                  |
| `Cancelled{uncertain}` / `TimedOut` | stopped; `io_bytes($w)` bytes are known written, and `io_uncertain($w)` says the rest is unknown |
| `Unknown`                           | the backend cannot say; **never** an invitation to resend                                        |
| `Rejected`                          | admission refused (no grant, read-only capability, bound, budget); nothing started               |

`Partial` is not produced by this adapter today and the script reads it as any other tag if a later
backend does. `Conflict`/`Stale` appear only with a generation guard, which this slice does not
expose (§19).

### 4.3 `append_bytes(source)`

The capability was opened `for append`, so the descriptor carries `O_APPEND` and the worker uses
`::write`, not `::pwrite`: on Linux `pwrite` to an `O_APPEND` descriptor ignores its offset, and
relying on that is a documented defect, so the adapter must not. The endpoint's rights bit decides;
a `write_bytes` on an append capability, or `append_bytes` on a positional one, is `Rejected`
(`Unsupported`, detail `2`). The request carries no offset, so the resulting position is not known
to the script and is not reported (a `Complete` append reports `io_bytes`, not an offset). Atomicity
of one append against other writers is the kernel's `O_APPEND` guarantee for a single `write` that
the loop completes in one call; a short write continuing in a second call is reported through
`io_bytes` and may interleave with an external appender, which is stated, not hidden.

### 4.4 `sync()`

`fdatasync` on a worker (`FileOpSpec::Kind::Sync`), ordered by the lane rule after every earlier
admitted write on the endpoint (`io_service.cpp:290-345`), so `Complete` of a `sync` means every
earlier *completed* write on that capability reached stable storage, per the backend's promise.
Linux `fdatasync` on a regular file; a filesystem that cannot (some FUSE mounts) reports
`Failed{BackendFailure}`; a backend that can make no promise is `Rejected(Unsupported)`. It carries
the same outcome set as `write` and the same `within`.

### 4.5 `capture($r)` and `capture($r, off, len)`

`$r` must be an outcome whose terminal tag exposes a payload (`tagMayCarryPayload`,
`frontend_bridge.hpp`: `Complete`, `Partial`, `Failed`, `Cancelled`, `TimedOut` that retained
bytes). The selection is the whole retained payload, or the sub-range `[off, off+len)` of it. One
range per capture (the coordinator's `maxIntervals` of 128 is for a human's gesture over runs and is
not needed to copy one read; a list form is deferred).

Value, with `io_tag` naming it:

| Result                       | Value of the action (`$c`)                                        | `io_tag($c)`     |
| ---------------------------- | ----------------------------------------------------------------- | ---------------- |
| Committed                    | the capture's receipt cell                                        | `CaptureReceipt` |
| Refused before any request   | the `Rejected` outcome the coordinator minted (`service_.refuse`) | `Rejected`       |
| A step's request ended badly | that step's own terminal outcome (`Failed`, `Cancelled`, ...)     | its tag          |

This is the M7-VQL-b precedent (`io_tag` "also names what a script holds that is not an outcome":
`Capability`, `Request`). It is why no synthetic `Complete` outcome is minted: a capture is not one
request, so a fabricated outcome would claim progress counters no request has. A script
distinguishes success by `io_tag($c)` and reads the error through `io_error`/`io_detail` as for any
outcome.

`io_captured($c)` answers, for a receipt, the cells that are the capture's content (§12): one arena
cell per translated piece of the selection in source order, each quoting its permanent span; for
anything that is not a receipt, an empty result (the "else none" rule). Only these cells are what a
script may persist as the captured bytes.

### 4.6 What a capture does not do

- It does not read the file again. It captures the bytes the outcome retained: the exact observation
  (plan §9.3). A later read of the same path is a new observation and captures again.
- It does not capture on its own initiative: reading, rendering and `io_payload` never capture.
- It does not retain the transient chunk: translations do not pin bytes (plan §8.2). The payload
  must still be leased when `capture` runs, which it is while the script holds `$r` (a script's own
  requests stay admitted until it ends, M7-VQL-b), so a capture must happen inside the script that
  read. A `capture` of a retired or never-leased outcome is `Rejected(Unresolved)`.

### 4.7 The persistence rule

Today the rule is positional and conservative: any script that held an action refuses `-o`,
`--in-place` and `:save` (`vquery/main.cpp:614-625`). M7-VQL-b's gate widened it from "ends in an
action" to "held an action" because `io_payload` rows end in a `return`. It stays the default.

New: each run keeps an **exposure set**, the `PayloadRange` of every payload the script turned into
text, whether as an action's final row (`script_runner.cpp:934`, `payloadText`) or through
`io_payload` (`script_runner.cpp:503`). The runner records the range, not the text. When the script
finishes (before its requests are retired, so leases are still live), the runner evaluates, for each
range, **`CaptureCoordinator::translationFor(range)` covers all of it**, and records one bit:
`persistable` iff every range is covered (an empty set is vacuously covered: a script that read
nothing it exposed is as persistable as a script with no action, which it already was by being
refused only when it held an action; here it becomes accurate instead of blunt).

`ScriptRunner::persistability()` answers `Persistable` or `NeedsCapture{uncovered}` for the most
recent finished script; `vquery` consults it where it consults `resultsFromAction` now. A
`NeedsCapture` run keeps the exact current refusal (`VQL_IO_PERSIST_NEEDS_CAPTURE`) with its message
extended by the count of uncovered exposures.

Argued:

- **Coverage of exposed ranges, not flow tracking.** Tracking which rows derive from which payload
  is impossible once a pure function (`concat`, `substr`) mints a fresh cell: a flag on the payload
  cell does not follow. Coverage by range is sound at the granularity that matters: *every byte the
  script saw as text is, in the author's permascroll with provenance, because the author said so.* A
  row derived from covered input is a computation over captured data, which the author may persist.
- **Derived or duplicated text is allowed.** `return io_payload($r)` after capturing `$r` persists
  the text again as typed bytes (a second copy, no transclusion link). Price: those bytes occur
  twice in the permascroll. The captured cell from `io_captured($c)` is the right row and
  transcludes the span; the rule does not force it because refusing would turn "persist what I
  printed" into an error after the author already paid for the capture.
- **Partial coverage refuses.** Capturing bytes `[0,10)` and exposing `[0,4096)` is `NeedsCapture`:
  the uncovered 4086 bytes were never captured.
- **Evaluated at script end, not at persist time.** Translations may be evicted later (the
  coordinator evicts committed translations least recently hit first,
  `capture_coordinator.cpp:769-792`); a decision made at the end of the script is a fact about that
  run. The persisted rows quote permanent spans or carry derived text; neither depends on the
  translation map afterwards.

## 5. The permascroll policy

1. **Captured bytes go into the author's one permascroll, and nowhere else.** The sink is
   `UserPermascrollCaptureSink` over the permascroll `vquery` already holds (`vquery/main.cpp:446`:
   the registry's default user scroll, or `--permascroll`). No second scroll, no capture-specific
   store of bytes. A `Store` holds no primedia (`AGENTS.md`), so the capture store (§5.3) holds
   spans and receipts only.
1. **Transient input stays transient.** Reading never appends. The only road to the permascroll in
   this slice is `capture`; `-o`, `--in-place` and `:save` of a `Persistable` run append only the
   derived text the run itself produced, as they do for any query. The exposure rule (§4.7) is what
   keeps an uncaptured payload from reaching them.
1. **Provenance is structure, not a note.** The capture store (a `CapturePublisher` over a dedicated
   `Store`) holds, per capture, a receipt with `d.io.range` pieces whose content is the permanent
   span and `d.io.provenance` scalars naming source kind, source, generation, occurrence and source
   offset (`capture_publication.hpp`), keyed by `(journalId, CaptureId)`. A captured cell in a
   result quotes the same span, so the Spanfilade finds the receipt from the span: the permanent
   address is the link. No `d.source` text is written, and the range cells carry no serialised
   provenance.
1. **Local capture invents no remote signature** (plan §8.2); a file's bytes captured by this author
   are this author's bytes about that file, no more.
1. **Permanence is the price.** Append-only bytes cannot be rolled back. A `Rejected`/failed capture
   after the permascroll append leaves unreachable orphan bytes that are never guessed into identity
   (plan §8.2); `:captures` reports `orphanBytes` from the recovery report. Re-capturing an
   unchanged file after a restart appends again: a transient origin does not survive a process
   (`io_capture_test.cpp`, `AnOpenIntentOnATransientSourceDoesNotPinTheNextSession`), and content
   equality is refused as a substitute for origin (redesign §9.3).
1. **Where the capture store lives.** `--capture-dir DIR`, default
   `$XDG_DATA_HOME/xuzz/capture/<permascroll key>/` beside `$XDG_DATA_HOME/xuzz/permascroll/<key>/`,
   holding `capture.journal`, `ops.nodes`, `store.tables`. A dedicated store, not the primary store:
   `CapturePublisher::publish` lands on `store.latest()` and requires a single writer
   (`capture_publication.hpp` comment), and a user's document store is opened by other programs.
   Refused: publishing into the primary store (couples a read tool's session to a document's write
   lock and makes `--in-place` and capture race).

## 6. Execution design

### 6.1 Writes: no new opcode, no new relation

`write_bytes`, `append_bytes` and `sync` lower exactly as `read_bytes` does
(`compile_io_effect.cpp:100-150`): `MakeTerm` builds the description (`write_bytes(offset, src)`
where `src` is a text cell or an outcome cell; `append_bytes(src)`; `sync()`), `Submit` admits it,
`BranchFalseCell` on the arm ordinal selects `refused` or `await`. The VM sees a capability cell and
a description cell and nothing language- or I/O-specific. The runner's Submit hook decodes the new
functors beside `read_bytes` and stages through the file endpoint adapter (§4.2). The detached form,
`within`, `as`, `await($h)` and `cancel($h)` need no change. `io_uncertain($r)` is one more provider
beside `io_error` (`script_runner.cpp:473`), reading the outcome's `uncertain` detail through
`FrontendBridge::outcomeField`, which already exposes it.

### 6.2 Capture: provider, Await, loop

The coordinator's protocol is `prepare` then `appendRequest` then await, then `commit` returning
`Committed | Refused | CommitPending{request}`, then `continueCommit` after each durable step until
`Committed`. The lowering is a bounded loop over the existing opcodes (`Jump`, `Branch`,
`BranchFalseCell`, `Await`, relation calls; `vortex_vm.hpp:110-146`), reusing the "provider answers
arm 0 request or arm 1 outcome" shape that `handleArm` already has (`script_runner.cpp:532`):

```text
begin:   cap  = relate(captureBeginRelation, [$r, off, len])    # prepare + appendRequest
loop:    arm  = relate(captureArmRelation, [cap])               # 0: a request to await, 1: done
         BranchFalseCell arm -> done
         out  = Await(request-of(cap))                          # parks; resume re-enters here
         cap  = relate(captureStepRelation, [cap, out])         # commit / continueCommit
         Jump loop
done:    result = value-of(cap)                                 # receipt, or the outcome that ended it
```

Each provider runs once per pass on the owner thread. A resume continues at the `Await` and so never
re-runs `captureBeginRelation`: the capture is prepared and its append requested exactly once (the
same property as `Submit`, P6). `captureStepRelation` calls `commit` the first time and
`continueCommit` afterwards; it returns "request" for `CommitPending`, "done" for `Committed`, and
"done with the refusing outcome" for `Refused` or a non-`Complete` step outcome (the coordinator
already refuses `commit` for a non-successful append, plan §8.2). The coordinator's protocol has one
append and at most two durable steps (Appended record, then snapshot plus `Committed`); the provider
counts passes and refuses a fifth with `BackendFailure` detail `3`, so a coordinator regression
fails by value instead of looping. The capture handle (`CaptureCell`) and its coordinator-side
pending state are *host* state; the script holds the receipt, never the capture cell, so no native
`CaptureId` enters a language value (`auditLanguageValue` already checks the `Capture` tag).

The runner's parked token for a capture is the same `FrontendToken`: `request` is the step's
request, `position` the statement index. A capture is one statement; its sub-requests are the
runner's, as a read's request is.

C++ justification (AGENTS.md "new C++ must justify why it is not Vortex"): the providers wrap the
coordinator, whose work is journal fsyncs, permascroll appends and a Store snapshot: OS-facing and
off-thread by construction, the same class as `FileReadHost`. The *control flow* of capture, the
loop and the branch, is Vortex bytecode.

### 6.3 Roads not needing a core change

| Need                                  | Met by                                                                                             |
| ------------------------------------- | -------------------------------------------------------------------------------------------------- |
| Build a cell quoting a permanent span | `ArenaManifold::makeCell(const PrimediaSpan &)` (exists)                                           |
| Several awaits in one statement       | `Await` + `Jump`/`Branch` (exist); capture is a loop of them                                       |
| Keep a capture's multi-step state     | `CaptureCoordinator::pending_` (host); the cell is a handle                                        |
| Refuse an uncaptured persist          | runner-held exposure set + `translationFor` (host)                                                 |
| Charge a write's bytes to the service | carry the bytes in `PreparedOperation::bytes` (adapter; `io_service.cpp:259` already charges that) |
| Say what spans a selection became     | one coordinator accessor, `spansFor(range) -> std::vector<PrimediaSpan>` (stage C3)                |

## 7. Typed refusals

Two layers, as in M7-VQL: a static or script-level diagnostic when the script cannot proceed, and a
`Rejected` outcome value when the service refuses admission. Nothing is a default, an empty row or
an exception.

| Code or value                                            | When                                                                                       |
| -------------------------------------------------------- | ------------------------------------------------------------------------------------------ |
| `VQL_IO_QUERY_CONTEXT` (diagnostic, at parse)            | any new form where `submit` is already refused                                             |
| `VQL_IO_EFFECT_ARGUMENT` (diagnostic, before any submit) | unusable capability, offset, byte source (not a text cell or outcome), bad `capture` range |
| `VQL_IO_WRITE_SOURCE` (diagnostic; new)                  | the byte source is an outcome that exposes no payload, or text that cannot be read         |
| `VQL_IO_CAPTURE_UNAVAILABLE` (diagnostic; new)           | the host built no capture stack (`vqueryc`, a test host, an unwritable capture directory)  |
| `VQL_IO_CAPTURE_SOURCE` (diagnostic; new)                | `capture` of a value that is not an outcome                                                |
| `VQL_IO_PERSIST_NEEDS_CAPTURE` (diagnostic, kept)        | persist of a run with uncovered exposures; message gains the count                         |
| `Rejected(Unsupported, 1)` (value)                       | open `for write`/`append` with no grant                                                    |
| `Rejected(Unsupported, 2)` (value)                       | write mode mismatch, or a read-only capability                                             |
| `Rejected(BudgetExceeded)` (value)                       | the frozen write exceeds the write bound or the service's command/reserved bytes           |
| `Rejected(InvalidCapability / Stale)` (value)            | closed or foreign capability                                                               |
| `Rejected(Unresolved)` (value)                           | `capture` of a payload whose lease is gone                                                 |
| `Rejected(BudgetExceeded, 1/2)`, `PersistenceRefusal`    | the coordinator's own refusals (selection too large, journal full or unwritable)           |
| `Rejected(Conflict)` (value)                             | capture overlaps a pinned, unreconciled interval; names `:captures` in the diagnostic text |
| `VQL_SCRIPT_FAILED` / `VQL_SCRIPT_TIMEOUT` (kept)        | the script's own stretch fails, or its deadline passes while parked                        |

Messages name the code, never document or payload bytes (AGENTS.md logging rule); capture logs use
`vortex.io.capture`, writes `vortex.io.file`.

## 8. Crash ordering

**Write.** No journal, no durable intent: a script is not durable, and a write is an effect on the
world the plan declines to re-drive ("Unknown never authorizes resend"; "automatic retry after
unknown" is refused, plan §14). The order is: freeze at submit, admit, worker `pwrite`, outcome. A
crash at any point leaves the file holding some prefix of the bytes (possibly none, possibly all);
nothing records which, and nothing resumes (a restarted `vquery` has no parked script). Re-running a
script is a new write. A positional write of the same bytes at the same offset is idempotent in
*effect*; an append is not, and the doc states that rather than hiding it. `sync` is how a script
learns that a `Complete` write survived power loss; a script that needs the answer awaits `sync`
before it proceeds.

**Capture.** Exactly the plan's §8.2 order, already implemented and tested (`io_capture_test.cpp`),
now reachable from a script:

1. `prepare` (owner, in memory): leases frozen, interval map and journal budget reserved.
1. Append request: the worker writes and fsyncs the `Append` intent, *then* appends to the
   permascroll (P5).
1. The worker's step syncs the permascroll; the fsynced `Appended` record follows.
1. `continueCommit`: publish the receipt and spans into the capture `Store` in memory, take a
   snapshot (O(new operations)).
1. A worker writes and fsyncs the snapshot (`ops.nodes`, `store.tables`), then the fsynced
   `Committed` record.

A crash after step 2's intent and before the `Appended` record pins the intervals and reports orphan
bytes; after `Appended` and before `Committed`, recovery's `resumePublications` publishes exactly
once (`CrashBetweenAppendedAndPublicationPublishesExactlyOnce`). `vquery` runs
`CaptureCoordinator::recover()` before the first capture of a session (lazy, §13), on the owner
thread, bounded by the journal budget of 4 MiB; this is the plan's stated exception (recover,
reconcile and `resumePublications` "run at open or on an explicit owner decision", plan M6 notes),
not a commit's happy path. Recovery never resubmits an append.

## 9. Idempotency and same-request resume

- A `Submit` runs once per reached action; a resume continues at the parked `Await` and never
  re-executes it (P6). Unchanged for writes.
- A capture's `captureBeginRelation` runs once (§6.2); each later step's request is awaited by the
  same token mechanism; `FrontendBridge::resume` re-awaits the same request after a wake.
- A duplicate wake, a re-poll or a runner `resume` after the request ended returns the stored
  outcome; none resubmits.
- **No silent retry** of a `Rejected` write (the script may submit another action: a *new*
  operation, as the plan's multiplicity rule says), of an `Unknown` write, of a failed capture step,
  or of a capture refused for a pinned overlap. `Unknown` is a value the script reads.
- A capture that failed after the append and was never committed is **not** re-appended by a second
  `capture($r)` in the same session: the coordinator's translation map and pins (not the script)
  decide, and an overlap with a pinned interval is `Conflict`. This is the plan's "never silently
  duplicates an ambiguous overlap".
- Re-running a whole script is a new run and is not idempotent for appends, nor for a capture of a
  fresh observation (§5.5).

## 10. Backpressure and bounds, priced

No bound here is new policy. Each is an existing budget or a consequence of one, and each refuses by
value rather than blocking or queueing without limit.

| Bound                     | Value and source                                                                               | Price and behaviour at the limit                                                                                                                                 |
| ------------------------- | ---------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| One write's bytes         | `FileReadHostOptions::writeBytes`, default `defaultPayloadBytes` (1 MiB); `--io-write-bytes N` | one frozen buffer in memory per request; over it: `Rejected(BudgetExceeded)`, nothing staged                                                                     |
| Bytes of writes in flight | the service's command budget (`maxCommandBytes`, `maxReservedBytes`, 4 MiB `budgetBytes`)      | today **uncharged** (§2 finding 1); after C1 four 1 MiB writes in flight, the fifth is `Rejected(BudgetExceeded)`                                                |
| Staged ops / staged bytes | adapter table 4096 ops, 64 MiB (`file_source_adapter.cpp:22-30`)                               | a backstop behind the service budget; unchanged                                                                                                                  |
| Requests in flight        | `maxRequests` (16), per-endpoint 8 (`file_read_host.hpp` defaults)                             | refused `BudgetExceeded`; a script's own requests stay admitted until it ends, so a long script of many writes holds a slot each (reaped as terminal, see below) |
| One capture's bytes       | `capture_defaults::maxAppendBytes` 1 MiB                                                       | equals the read bound, so every single read fits one capture; a larger selection is `Rejected(BudgetExceeded, 2)`                                                |
| Captures live at once     | `maxPendingCaptures` 32, `maxMapEntries` 8192, journal 4 MiB                                   | refused before any append (plan §8.2); the coordinator reaps resolved state and evicts cold translations first                                                   |
| Exposure set              | at most one range per request the script holds, so at most `maxRequests`                       | no new ceiling: bounded by an existing one                                                                                                                       |
| Owner-thread work         | capture prepare is O(selected intervals); the durable steps are on workers                     | no blocking I/O on the owner (plan §14)                                                                                                                          |

**The 1 MiB write bound and large copies.** A script cannot write more than the bound in one action
and has no loop, so copying a large file takes one statement per chunk. The memory rule "no ceilings
users work around" applies, and the honest answer is: the bound is a configured budget (a flag now,
`system://settings` in M8), refusals name it, and the work-around is bounded by the number of
statements the author is willing to write. The real fix is a host-side `copy` effect that streams
between two capabilities in bounded chunks with progress and one terminal outcome; it needs its own
design (chunk accounting, partial-failure contract) and is deferred (§19) rather than smuggled in
here.

**Slots held by a long script.** M7-VQL-b keeps every request a script admits until the script ends,
so a script that names its outcomes is capped at `maxRequests` (16) of them. The owner ruled that
cap out: an outcome bound with `as` is copied into a script-owned **owned record** (§12.1) and its
service request is retired at once, so the slot is held only while the request runs. The cost moves
from slots to the record's retained payload bytes, which are budgeted (§12.1).

## 11. Timeout, cancel and `~ScriptRunner`

- **`within N ms`** on a write or `sync` is M7-VQL-b's request deadline: the service stops the
  backend and publishes `TimedOut` with the backend's `uncertain` flag; a completion already
  delivered wins the race. For a write, a timeout is *uncertain*: some prefix may be on disk
  (`file_source_adapter.cpp:364-370` reports `Cancelled{uncertain=true}` after a partial pwrite).
- **Script timeout** abandons a parked script (`VQL_SCRIPT_TIMEOUT`) exactly as before: the awaited
  request is asked to stop, waiter and route dropped, requests released, files the script opened
  closed.
- **`cancel($h)`** on a detached write is M7-VQL-b's cancel. A request stopped before its worker
  started leaves the file untouched (`Cancelled`, `uncertain=false`); a started one may have written
  a prefix (`uncertain=true`). `cancel` of a *capture* is not offered (detached capture is
  deferred).
- **Timeout during a capture** abandons the script: the awaited step's request is asked to stop, and
  the runner releases the capture cell. `sweepDead` then classifies it: a capture past
  `AppendRequested` is **retired uncertain and its intervals pinned** until reconciled
  (`capture_coordinator.cpp:670-715`). Consequences the author sees: a later capture overlapping
  those bytes is `Rejected(Conflict)` until `:reconcile` releases or promotes it. This is the honest
  behaviour of a two-resource transaction interrupted between its resources. Refused: shielding a
  capture from the script timeout (it turns the timeout into a promise the runner cannot keep when
  the disk hangs, and a hung worker would block the session) and silently re-driving the capture to
  completion in the background (it makes an abandoned script finish its side effect, the opposite of
  abandonment).
- **`~ScriptRunner`** does what it does for reads: cancels waiters, drops routes, releases the
  script's cells. For a capture mid-flight it does **not** cancel the durable step (cancelling an
  fsync mid-record manufactures an uncertain pin for no gain); it orphans the step's request and
  releases the capture cell, leaving `sweepDead` to pin or free by phase. An append or write request
  still running is asked to stop before it is orphaned (the existing rule for reads). Closes of
  files the script opened are admitted as always and never waited for. A recovered journal after
  process death reconciles the rest (§8).

## 12. Representation: cells and dimensions

Nothing is a side table the script can see, and nothing is serialised text in a value cell. The
existing graph conventions are reused (`io_cells.hpp` dimensions `Request`, `Outcome`, `Range`,
`Provenance`, `Payload`).

- **Write request.** A request cell as for a read; its spec is a `write_bytes` term whose operand is
  a *link* to the source cell (a text cell, or an outcome), not a copy of its bytes into the term.
  The frozen bytes live in the adapter (native state, "native tables validated against incarnation,
  never rebuilt from cells", plan §3.2) and are not a cell: they are a lease, and binary payloads
  have no cell form.
- **Write outcome.** An `Outcome` cell with its tag and `Progress` rank; `transferredBytes` and
  `uncertain` are the existing scalar fields. No new dimension, no payload rank (a write retains
  none).
- **Capture.** `CaptureCell` (owner-only, host state), whose `Request` rank holds the current step
  request (a clone view, so an await's waiter cannot evict it), exactly as the coordinator mints
  today (`capture_coordinator.cpp:1500-1515`). It never reaches a language value.
- **Receipt.** The existing `CaptureReceipt` cell: `captureId`, `journalSeq` and the span's fields
  as the coordinator's `field(label, value)` pairs mint them today
  (`capture_coordinator.cpp:1747-1790`). That is the owner-facing mirror and is *not* what the
  script reads.
- **The captured cells (`io_captured`).** Per translated piece of the selection, in source order, an
  arena cell minted with `makeCell(PrimediaSpan)` (content is the permanent span, no scratch), and
  the receipt links them on the `Range` rank (the same dimension name the Store's published receipt
  uses, `d.io.range`). Piece-level provenance is *not* repeated in the arena: it is the published
  receipt's `d.io.provenance` scalars in the capture Store, found by address (§5.3). The head cell
  of the rank is the receipt; walking the rank is how a program enumerates.
- **Exposure set.** Runner state (`std::vector<PayloadRange>` per activation) with no cell form: it
  is a host fact about leases, not data. It answers one question for one host call.

### 12.1 The owned record (Q5, owner ruling)

**Problem.** `as $r` binds the service's outcome cell, which lives in the request's graph; to keep
`$r` readable the runner keeps the request admitted until the script ends. That caps named outcomes
at the request budget and makes a payload-bearing outcome pin the request's completion reserve.

**Design.** When a statement with `as $r` reaches a terminal outcome, the runner calls
`OwnedOutcome::copy(outcome)` (host code in `apps/common/xanadu/vortex/`, beside `FrontendBridge`),
then retires the request through `FrontendBridge::retire` immediately and binds `$r` to the record.

- **Cells.** One arena cell tagged `IoCellTag::Outcome` (the same tag, so every existing reader of
  an outcome treats it alike) carrying the *same* ranks the service's outcome has: the tag and
  scalar fields on the field rank (`errorCode`, `stableDetail`, `cause`, `uncertain`), and a
  `Progress` rank cell with the progress fields (`acceptedBytes`, `transferredBytes`,
  `consumedBytes`, `sourceSequence`, `phase`). They are copied by value into cells the script owns,
  so retiring the request frees nothing the record reads. The cell has no `Request`, `Queue`,
  `Waiter`, `Clone` or `Lease` rank and so no way back to the service. A record is marked by a
  single `Owned` field so `io_tag($r)` and `auditLanguageValue` can tell it apart; nothing else
  differs.
- **Payload.** A payload has no cell form (§12 above), so the record has no `Payload` rank. The
  runner holds host state keyed by `(record cell, incarnation)`: `{PayloadRange, PayloadLease}`. The
  lease is taken with `PayloadResolver::retain(range)` *before* the request is retired, so the bytes
  stay resolvable when the service releases its own. The state is validated against the cell's
  incarnation and dies with it (plan §3.2: native tables are never rebuilt from cells).
- **Readers.** `io_tag`, `io_error`, `io_detail`, `io_bytes` and `io_uncertain` read through
  `FrontendBridge::outcomeField`, which reads the field and progress ranks, so they work unchanged
  on a record. `io_payload` and the shared status gate consult the runner's owned-record table
  first, apply the same `tagMayCarryPayload` rule to the record's tag, and read through the record's
  lease; for a service outcome they behave as today. `await($r)` and `cancel($r)` of a record are
  the existing "a handle that is already an outcome is itself the value" case: a record is an
  outcome.
- **Budget and ownership.** The record's lease pins its whole physical chunk (plan §8.1, charged at
  physical size). The runner keeps a total of those bytes against
  `FileReadHostOptions::retainedPayloadBytes`, default `budgetBytes` (4 MiB): retention can never
  exceed what the service itself would have held for the same requests, so it adds no new ceiling.
  When a copy would pass it, the runner **does not copy**: that outcome stays the service's, its
  request stays admitted (today's behaviour, slot and all), and the failure is logged under
  `vortex.io.script`. No payload is dropped and no refusal is invented; the cap applies only past
  the retained-bytes budget. Leases are released, in order, when the script ends, fails, is
  abandoned or its runner is destroyed (`endActivation` and the destructor), with the record's cells
  freed by the shared cell deletion (P7). An unnamed outcome is not copied and keeps its current
  retirement rule.
- **`capture($r)` on a record.** The provider resolves `$r` to a `PayloadRange` from the
  owned-record table (else the gate, for a service outcome) and calls `CaptureCoordinator::prepare`,
  which takes its own lease (`retain`) on the range. The record's lease is what keeps the bytes
  resolvable until then, so capture works after the service request is gone. Capture of a record
  whose payload was not copied (over budget) is the unchanged path.
- **The coverage rule survives the copy.** The exposure set holds `PayloadRange` values, which name
  an origin interval (`PayloadRef` plus offset and length), not a request or a lease. The record
  keeps the same range, so an `io_payload($r)` exposure records it and `translationFor(range)` at
  script end answers as before, whether the service outcome or its record was read.
- **Refused alternative: keep a text cell of the payload in the record.** It is not binary-safe, it
  would spool into the permascroll on `promote`, and `capture` takes ranges, not text. Refused
  alternative: copy only the scalar fields and drop the payload. It is the silent loss the plan
  forbids (a `Complete` read whose bytes vanish when named).

Refused: representing the exposure set as cells (it would be reachable from rows, and the rows
persist; the audit forbids owner-only tags in language values for exactly this reason,
`frontend_bridge.hpp: auditLanguageValue`); mirroring `d.io.provenance` into the arena per piece
(duplicates the Store's published, sealed fact with an unsealed ephemeral copy that can disagree);
storing the write's bytes as a text cell (not binary-safe, and it would spool into the permascroll
on `promote`).

## 13. vquery integration

| Surface                                    | Behaviour                                                                                                 |
| ------------------------------------------ | --------------------------------------------------------------------------------------------------------- |
| `--io-write-root DIR` (repeatable)         | roots a script may open `for write`/`append`; none given means no writes                                  |
| `--io-write-bytes N`                       | one write's bound (default 1 MiB)                                                                         |
| `--capture-dir DIR`                        | capture store, journal and publisher; default §5.6                                                        |
| `:open -w NAME PATH`, `:open -a NAME PATH` | bind a write or append capability for the session, under the same grant; the path still takes spaces      |
| `:captures`                                | pinned captures with their reasons, the recovery report's `orphanBytes`, `stateCounts()`                  |
| `:reconcile ID release\|promote`           | the owner decision `CaptureCoordinator::reconcile` takes; `promote` may need `SPAN`, so it names the form |
| persistence                                | §4.7: `-o`, `--in-place`, `:save` consult `ScriptRunner::persistability()`                                |

`vquery` builds the capture stack lazily, when a script containing `capture` is about to run (the
runner pre-flights `hasCaptureEffect`), so a plain query or a read-only script starts no journal,
opens no Store and runs no recovery; this mirrors the lazy `ioHost()`. Construction is: open or
create the capture `Store` over the permascroll, `CapturePublisher`, `CaptureCoordinator`, then
`recover()`, then `setPublisher`, then `resumePublications()`. A failure of any step is
`VQL_IO_CAPTURE_UNAVAILABLE` naming the step, never a degraded capture. `vqueryc` compiles all the
new forms (capability and byte-source variables with no binding become text cells naming them, as
now) and runs none.

The coordinator's `translationFor` and `spansFor` read the translation map the coordinator owns on
the owner thread; the runner and the coordinator are both owner-thread objects, so the persistence
decision needs no lock.

## 14. Core change proposals

**None is required.** The slice is host and front-end work in `apps/common/xanadu/vortex/` and
`apps/common/xanadu/vql/`, plus `apps/vquery/`. The `store.hpp`, `ops.hpp`, `compact_op.hpp`,
`store_tables.hpp`, `segmented_ops_spool`, `manifold`, `arena_manifold`, `vortex_core` and
`vortex_vm` files are untouched, and VortexCore and VortexVM stay language-neutral (the new
providers are host relations; the loop is ordinary bytecode). P1 to P7 are taken; the two numbers
below are reserved for proposals this design *considered and does not adopt*, so a reviewer can see
the cost of the road not taken.

**P8 (considered, refused): a persist-taint flag on arena cells.** `ArenaManifold` would carry a
"transient-origin text" bit, `promote()` would refuse a reachable flagged cell
(`PromotionRefusal::RuntimeOnlyState` already refuses runtime-only cells), and the rule of §4.7
would become "no flagged cell in the result". Justification: it would let a script that captured
*nothing* still persist its non-payload rows precisely. Minimal alternative adopted: the
range-coverage rule (§4.7), host-side. Why refused: the flag does not propagate through pure
functions (`concat`, `substr` mint fresh unflagged cells), so it launders exactly the cases the rule
exists to stop; making it propagate means a flag on every cell-minting path in the engine, a core
change with a performance price on the hot arena for a feature one front end uses.

**P9 (considered, refused): a durable write-intent journal** (a second journal beside
`capture.journal`) so a crash mid-write could be reconciled as "unknown, here is the prefix".
Justification: at-most-once evidence across restarts. Minimal alternative adopted: none needed. A
script is not durable, a restarted program resumes nothing, and the actor profile already has a
purpose-built attempt journal (`io_actor_journal.hpp`) for the one frontend that does resume. A new
on-disk format would also be an R11 version bump for a path no frontend can resume.

If implementation finds that `FileOpSpec` needs a `Sync` kind or the adapter a create flag, those
are `file_source_adapter` / `file_endpoint_adapter` edits (adapter files, in scope, not the core
list).

## 15. Refused alternatives, collected

| Alternative                                                            | Why refused                                                                                       |
| ---------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------- |
| Capture implicitly when persisting (`-o` captures)                     | plan §14 refuses implicit capture; it makes a serialising flag an irreversible permascroll append |
| Auto-capture every read                                                | permanent growth per read; defeats "transient unless explicitly captured"                         |
| Capture text instead of an outcome                                     | text is scratch bytes with no source observation, so overlap identity (plan §9.3) cannot be kept  |
| One `Submit` for capture                                               | capture is three requests (§2); a Submit admits one                                               |
| A new `Capture` opcode                                                 | a core change for policy; relation providers plus `Await` already express it                      |
| Taint flag on cells (P8)                                               | launders through pure functions (§14)                                                             |
| Content-hash dedup of re-captures                                      | content equality is not origin (redesign §9.3)                                                    |
| Publish captures into the primary store                                | single-writer requirement; races with the document's own writer                                   |
| Writes granted by the script (`open ... for write` allowed by default) | a script would grant itself authority; the grant is the invoking human's flag                     |
| Truncate, create-or-open, `O_TRUNC`                                    | destroy or hide data; not needed to write a report                                                |
| `pwrite` on an `O_APPEND` descriptor                                   | offset is ignored on Linux, a documented defect                                                   |
| Auto-retry a timed-out or `Unknown` write                              | plan §14; a retried non-idempotent write duplicates                                               |
| Shield capture from the script timeout / finish it in the background   | makes the timeout a lie, or an abandoned script keep acting (§11)                                 |
| Synthetic `Complete` outcome for a capture                             | a capture is not one request; progress counters would be invented                                 |
| Detached capture handles in this slice                                 | a handle needs the runner to drive the coordinator's steps after the script moves on              |
| Write bytes as text only                                               | not binary-safe; copy-a-file would be impossible                                                  |
| A write-intent journal (P9)                                            | nothing can resume to use it (§14)                                                                |

## 16. Acceptance criteria

1. **Writes are values and honour the contract.** For each of `Complete`, `Failed`, `Cancelled`,
   `TimedOut` (uncertain), `Unknown` (fake backend) and `Rejected`, the script sees the outcome as a
   value, `io_tag`/`io_error`/`io_bytes`/`io_uncertain` read it, no payload is exposed, and none is
   a `QueryError` or an empty result.
1. **No authority from the script.** With no `--io-write-root`, every `open ... for write|append` is
   a `Rejected` value and no file is created or modified; a path outside every root, or `..` or a
   symlink escaping a root, is `Rejected`; a read-only capability cannot be written.
1. **Frozen at submit, charged at admission.** Mutating the source cell after `submit` does not
   change the bytes written; a write over the bound, or over the service's reserved bytes, is
   `Rejected(BudgetExceeded)` with nothing staged; reserved bytes return to the budget when the
   request is reaped.
1. **Ordering.** Two detached writes on one capability run in admission order; a `sync` ordered
   after them reports `Complete` only after both are `Complete`; a read and a write on one endpoint
   never overlap.
1. **Multiplicity and resume.** A write submits once per reached action, a Pending write parks and
   the same request is re-awaited, a rolled-back (backtracked) submit orphans the request and never
   cancels it, and a duplicate wake submits nothing.
1. **Capture is explicit and journal-ordered.** A read that is not captured appends nothing to the
   permascroll (permascroll size unchanged). `capture($r)` appends exactly the uncovered bytes once,
   the journal order is `Append`, `Appended`, `Committed`, the receipt is in the capture Store, and
   `io_captured` cells quote the permanent span, whose bytes equal the retained payload.
1. **Overlap reuses.** Capturing `[0,10)` then `[5,15)` of one observation appends only `[10,15)`; a
   second read of the same file is a new observation and appends again.
1. **Capture failure is a value.** A non-successful append never publishes translations (`commit`
   refuses it); an admission refusal, a full journal and an unresolved lease each give the typed
   `Rejected` of §7, and the script continues.
1. **Persistence is gated by coverage.** A run whose exposed ranges are all covered persists through
   `-o`, `--in-place` and `:save`, and the persisted captured rows quote the permanent span
   (`readResultSlice` shows it transcluded, not retyped); a run with any uncovered exposure,
   including partial coverage, keeps `VQL_IO_PERSIST_NEEDS_CAPTURE` and writes nothing; a run with
   no exposure persists.
1. **Crash and abandon.** The five crash points of §8 reproduce the existing recovery results
   through a script-driven capture; a script timeout or runner destruction mid-capture pins by phase
   (`:captures` lists it), never resubmits, and `:reconcile` releases it so a later capture
   proceeds.
1. **Leaks.** After 10^4 write runs and 10^3 capture runs the arena, the service's accounting and
   the coordinator's `stateCounts()` are bounded (as `ManyEffectScriptRunsLeaveTheArenaBounded` pins
   for reads).
1. **No core change and no blocking.** Core files are byte-identical; no `read`, `write`, `fsync` or
   `fdatasync` runs on the evaluation thread outside the documented recover/reconcile exception;
   `auditLanguageValue` finds no native id in a write, capture or receipt value; the M7a conformance
   harness passes unchanged.
1. **Format.** `make format-check` and `make lint` pass; this note passes mdformat and mdl.
1. **Owned records.** A script that names 100 write or read outcomes in a row is not refused for
   slots and the service holds at most the requests then running; each named outcome's tag, fields,
   error, detail, uncertainty and byte count read after its request was retired, and a named payload
   reads through `io_payload` after retirement; `capture($r)` on a record appends the same bytes as
   on a service outcome; the coverage rule gives the same answer for a record and a service outcome.
1. **Retention is bounded and degrades by value.** Named payloads past `retainedPayloadBytes` are
   not copied, stay the service's requests, and still read correctly; every lease is released at
   script end, failure, abandonment and runner destruction (the resolver's lease count returns to
   its baseline), and 10^4 named-outcome runs leave the arena bounded.

## 17. Test plan

All headless; `tests/xuzz/` only (`xuzz_test` links the engine and no graphics device). Tests sit
with their code once: file-write adapter tests are extended where `FileEndpointAdapter` tests
already live; capture tests extend `io_capture_test.cpp` only for what scripts add; no library
battery is repeated.

| Test group (new file or extension)                       | Proves                                                                                                                                                                                                            |
| -------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `vql_io_write_test.cpp` (parser, compiler)               | grammar, reserved names, `VQL_IO_QUERY_CONTEXT` for every new form in every refused context, `vqueryc` compiles                                                                                                   |
| `vql_io_write_test.cpp` (runner, fake backend)           | all outcomes of criterion 1, resume, multiplicity, rollback orphan, `within`, detached and `cancel`                                                                                                               |
| `vql_io_write_test.cpp` (real files, temp dir)           | criteria 2, 3, 4 with `FileReadHost`: grant roots, symlink and `..` escape, bound and budget charging, ordering, `sync` ordering, `O_APPEND` atomic append, `create` exclusivity                                  |
| `io_file_adapter_test` extension                         | write bytes travel in the prepared command (C1); `Sync` kind; `O_NOFOLLOW`; create flag                                                                                                                           |
| `vql_io_capture_test.cpp` (script over real coordinator) | criteria 6, 7, 8: journal order, overlap, refusals as values, `io_captured` span equality                                                                                                                         |
| `vql_io_capture_test.cpp` (persistence)                  | criterion 9 through `ScriptRunner::persistability()` and a `Store` round trip; partial coverage; derived text                                                                                                     |
| `vql_io_capture_crash_test.cpp`                          | criterion 10: the coordinator's crash points injected through a script; timeout and `~ScriptRunner` mid-capture; reconcile then retry                                                                             |
| `vquery_e2e_test.cpp` extension                          | `--io-write-root`, `--capture-dir`, `:open -w`, `:captures`, `:reconcile`, `-o` / `--in-place` / `:save` over covered and uncovered runs, the existing `ActionRowsAreNotWrittenBy...` test updated                |
| `VqlScriptConformance` extension                         | the M7a harness over a write action (criterion 12)                                                                                                                                                                |
| `vql_io_owned_record_test.cpp`                           | criteria 14 and 15 (the last two of §16): 100 named outcomes, field reads after retirement, payload through a record, capture and coverage over a record, the over-budget fallback, lease balance on every ending |
| arena-bound tests                                        | criterion 11, mirroring `ManyEffectScriptRunsLeaveTheArenaBounded`                                                                                                                                                |

A UX pass follows the `ux-validation` skill: a journey "read a CSV, capture it, derive a result,
save it, close vquery, reopen and see the slice quote the capture" and a journey "write a report,
sync, read it back", each reachable from the REPL without a flag-only step beyond the two grants.

## 18. Stages

Each stage lands tested and committed alone; the gate is the plan's (a VQL expert and a Vortex
expert, reports under `design/projects/vortex-io-implementation-review/m7-vql-c/`).

| Stage | Content                                                                                                                                                                                                                                                                                              | Gate                         |
| ----- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------- |
| C1    | Adapter and host: write bytes carried in the prepared command and charged; `FileOpSpec::Sync`; rights bits for append and create; `O_NOFOLLOW`; `FileReadHostOptions::writeGrant` and `writeBytes`; owner-side staging for `Write`                                                                   | adapter tests, criteria 2-4  |
| C1b   | Owned records (§12.1): `OwnedOutcome::copy`, the runner's owned-record table with leases and `retainedPayloadBytes`, early `retire` of a named request, `io_payload` and the gate reading records, `auditLanguageValue` knowing the `Owned` field; independent of writes and testable on reads alone | criteria 14, 15              |
| C2    | VQL writes: grammar, reserved names, compile, `ScriptRunner::admit` decodes `write_bytes`/`append_bytes`/`sync`, `open ... for`, `io_uncertain`, `vquery --io-write-root`/`--io-write-bytes`/`:open -w -a`                                                                                           | criteria 1, 5, 12 for writes |
| C3    | Capture core wiring: `FileReadHost`-sibling capture host (`CaptureHost` over `CaptureCoordinator`, `CapturePublisher`, `UserPermascrollCaptureSink`), `CaptureCoordinator::spansFor`, providers, bytecode loop, `capture`, `io_captured`, `io_tag` for receipts                                      | criteria 6-8, 12             |
| C4    | Persistence: exposure set, `ScriptRunner::persistability()`, `vquery` consults it, captured cells through `resultRows`/`writeResultSlice` (quote path for cells with no `store`), `--capture-dir`                                                                                                    | criterion 9                  |
| C5    | Recovery surface: lazy `recover()`/`resumePublications()`, `:captures`, `:reconcile`, timeout and destructor behaviour, crash tests, leak tests, UX journeys, docs (§4.13, plan notes, `vql-conformance.md`)                                                                                         | criteria 10, 11, 13          |

C1b needs only M7-VQL-b and may land before or alongside C1; C3's `capture($r)` and C4's exposure
set read records, so both follow C1b. C2 and C3 are independent after C1 and may be built in either
order or in parallel by two agents. C4 needs C3. Sequencing writes first gives the gate a smaller
review surface and a real use of `sync` and `within` before the larger capture protocol arrives.

Two implementation checks the stages must make before relying on this note: (a) that `resultRows`
and `writeResultSlice` can carry a cell whose content is already a permascroll span but which names
no loaded store (`QuotedSpans::store` is a `const Store *`; a captured cell needs the author's
permascroll as the quoting scroll, which `Store(permascroll)` already names), a vquery-local change
if it does not; (b) that the lane rule orders two writes on one capability FIFO as read at
`io_service.cpp:290-345`.

### 18.1 Stage C1 as built

C1 is the adapter and host half only: no grammar, runner or `vquery` change. What landed, with the
places it differs from the text above:

- **Write bytes in the command.** `FileSourceAdapter::prepare` moves a `Write` or `Append` op's
  bytes out of the staging table into the prepared command (`[stage key][bytes]`), so the service's
  `commandBytes` charge, the per-request `maxCommandBytes` refusal and the aggregate reservation all
  see them. The bytes leave the staged-byte ledger at that moment (they are charged once, by the
  service). A second `prepare` of the same key is `InvalidRequest`, never an empty write.
- **Rights.** `fileRightAppend` (bit 2) and `fileRightCreate` (bit 3) beside read and write. Append
  opens `O_RDWR|O_APPEND`; create adds `O_CREAT|O_EXCL` with mode `0600`; any write mode adds
  `O_NOFOLLOW`. Write plus append, and create without either, are `Rejected(Unsupported, 2)` at open
  prepare.
- **Kinds.** `FileOpSpec::Kind::Append` (`::write`, no offset) and `Kind::Sync` (`fdatasync`, rides
  the write lane so it runs after earlier writes). A mode mismatch (positional write on an append
  capability, append on a positional one, any write or sync on a read-only one) is
  `Rejected(Unsupported, 2)` at request prepare (`fileDetailWrongMode`); this changes the detail of
  the existing read-only-write refusal from 0 to 2.
- **Host.** `FileReadHostOptions::writeGrant` (a `WriteGrant`, empty means deny) and `writeBytes`
  (default 1 MiB); `writeRootGrant(roots, rights)` is the root-containment policy `vquery` will
  build from `--io-write-root`. `FileReadHost::openSpec/beginOpen` take rights; `stageWrite`,
  `stageAppend`, `stageSync` stage the request. The grant sees the canonical locator
  (`weakly_canonical`, so `..` and symlinked directories resolve first; an embedded NUL is never
  granted) and a denied open is `Rejected(Unsupported, 1)` (`fileDetailNoWriteGrant`). For a write
  open the host also puts the canonical path in the `OpenSpec`, so the worker opens the path the
  grant judged.

Deviations and findings, recorded because the code disagreed with the note:

1. **The backend byte bound is not a write bound.** The scheduler sizes `BackendBudget::maxBytes`
   from the request's completion and payload envelopes, which a write declares as 256 diagnostic
   bytes, so the existing `maxBytes` check in the write path failed every write over 256 bytes
   (`BudgetExceeded`). The check is removed for writes; the command budget is the bound that
   applies. Latent before C1 because no caller wrote more than a few bytes.
1. **Writes reported zero bytes moved.** The service publishes the progress it receives through
   `CompletionSink::progress`, not the one inside the terminal result, so `transferredBytes` of a
   write outcome was always 0. The adapter now sends the write's progress before the terminal
   candidate, which is what `io_bytes($w)` (§4.2) reads. Reads are unchanged.
1. **Four 1 MiB writes do not fit 4 MiB.** Each command is `1 MiB + 8` bytes (the stage key rides
   with the bytes), so the fifth-write example in §10 is the fourth in practice; the criterion is "a
   write the budget cannot hold is `Rejected(BudgetExceeded)`", which the tests pin at a 64 KiB
   budget.
1. **Check (b) holds.** Two writes admitted back to back on one capability run in admission order
   and a `sync` admitted after them finishes after both
   (`SyncCompletesAfterTheWritesAdmittedBeforeIt`).
1. **A symlinked parent swapped in after the grant** is still outside what a path check can promise;
   `O_NOFOLLOW` covers the final component only. Stated, not closed.

## 19. Deferrals

- `message` and frame modes; per-row, per-loop and `for`-body effects (until an explicit gate).
- Detached capture handles and `cancel` of a capture; a list form `capture($a, $b)`;
  `capture ... within`, which needs a deadline parameter on `CaptureCoordinator::appendRequest`.
- `truncate`, create-or-open, `rename`, `unlink`, directory operations.
- Generation guards (`if unchanged`) giving `Stale`/`Conflict` on a write; the adapter has
  `expectedGeneration` already, the script surface does not.
- A streaming `copy` effect between two capabilities in bounded chunks (§10).
- A pure aggregate that joins many rows into one text cell so a script can write a query's rows.
- Writes through `message`-mode endpoints, sockets, and any non-file endpoint.
- `system://settings` entries for the write and capture bounds (M8); a UI for `:reconcile`.
- Linking a result row to the capture receipt as an xanalink rather than by shared permascroll
  address.
- Remote (swarm) publication of a capture receipt; local capture invents no remote signature.

## 20. Owner decisions (2026-11-26)

The open questions of the first draft are answered; each ruling is applied above.

- **Q1, write roots.** Repeatable `vquery --io-write-root DIR`, default deny (§4.1).
- **Q2, derived text.** Allowed: once every exposed payload range is covered by a committed capture,
  derived or duplicated text persists (§4.7).
- **Q3, capture store.** Dedicated, beside the permascroll (§5.6).
- **Q4, large writes.** The 1 MiB per-action bound is accepted; the streaming `copy` effect is
  deferred (§10, §19).
- **Q5, named outcomes.** Added: an outcome bound with `as` is copied into a script-owned record and
  its service slot reaped at once, removing the 16-request cap (§10, §12.1, stage C1b).
- **Q6, timeout or `~ScriptRunner` mid-capture.** Abandon and pin (§11).
- **Q7, `sync()`.** In scope (§4.4).
- **Q8, append mode.** `O_APPEND` in scope (§4.3).

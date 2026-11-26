# M7-VQL remaining slices: roadmap

Status: roadmap, no code. It finishes the planning of the M7-VQL milestone of the
[Vortex I/O implementation plan](vortex-io-implementation-plan.md). The milestone has so far shipped
as four gated slices: M7-VQL (read one file, one action per script), M7-VQL-b (named outcomes,
handles, `cancel`, deadlines, script-level `open`) and M7-VQL-c (writes and explicit capture); the
plan records them as "implementation notes and deliberate deferrals" under the milestone row (plan
lines 1214, 1329 and 1421), and the design of the last is
[`vortex-io-m7-vql-c.md`](vortex-io-m7-vql-c.md). The owner asked for "the rest of the M7-VQL
sub-headings, in iterations" and no list of them exists. This note makes the list: it collects every
deferral, decides which belong to the VQL frontend and which belong elsewhere, orders the rest into
slices `M7-VQL-d` to `M7-VQL-m`, and defines when the milestone is complete.

Read with [`vql-query-language.md`](vql-query-language.md) §4.13 (the surface as shipped), the
plan's §13 milestone table and §14 refusals, and [`vortex-io-redesign.md`](vortex-io-redesign.md) §4
"Shared effect contract". It changes no code, no on-disk format and no core file. Where a slice
might need a core file it is named in §5 as a candidate proposal, with the minimal alternative, and
nothing proceeds without its own approval.

## Owner decisions (2026-11-26)

The owner answered the ten questions of §7. Eight take the recommended answer; Q2 does not. The
slices below were adjusted where a ruling changes them (only slice e).

| #      | ruling                                                                                                                                                                  | effect on the text                                                                                      |
| ------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------- |
| Q1     | `match` is exhaustive, `_` allowed, a missing tag is a compile diagnostic (as recommended)                                                                              | slice d as drafted                                                                                      |
| Q2     | **Yes**: a script may close any capability it can name, host-bound ones (`--io-file`, `:open`) included. `VQL_IO_CLOSE_NOT_OWNED` is dropped; `:close NAME` still added | slice e rewritten; the host-bound name goes stale with the typed stale-binding diagnostic after a close |
| Q3     | A detached capture alive at script end: wait, bounded by the script timeout; on timeout abandon and pin; no background completion (as recommended)                      | slice f as drafted                                                                                      |
| Q4     | Destructive verbs granted per verb on the root, `--io-write-root DIR[:verbs]`, default today's set (as recommended)                                                     | slice h as drafted                                                                                      |
| Q5     | `mkdir` and `list` in, `rmdir` out; an oversize line is `Overflow` with the cursor unmoved and the bytes retained; framing language-neutral, VQL first (as recommended) | slices h, j as drafted                                                                                  |
| Q6     | No process endpoints in M7-VQL (as recommended)                                                                                                                         | slice k as drafted                                                                                      |
| Q7/Q8  | No loop iteration ceiling; a generation guard names an explicit prior read outcome (as recommended)                                                                     | slices l, g as drafted                                                                                  |
| Q9/Q10 | Spell `stable` now with a typed refusal; the `ux-validation` journeys are required at exit (as recommended)                                                             | slices k, m as drafted                                                                                  |

## 1. What "complete against the contract" means

The M7-VQL row (plan line 790) is narrow: an effect in script grammar only, `ExecutionBlock`
completion or parking, once per reached action, placement refusal, "real `read_bytes`/file endpoints
need the M4/M5 gate". The redesign's contract for a VQL frontend is wider. The clauses a finished
frontend must satisfy, and where each stands:

| contract clause                                                                            | source                                                           | status                                                   | slice |
| ------------------------------------------------------------------------------------------ | ---------------------------------------------------------------- | -------------------------------------------------------- | ----- |
| Submit is explicit; every evaluation admits a new request or a typed refusal               | redesign §4 (line 240); plan:1318                                | built (P6 `Submit`)                                      | done  |
| Await observes a retained request, never resubmits                                         | redesign §4                                                      | built                                                    | done  |
| Render, filters, projections are pure and cannot submit, capture, cancel                   | redesign §4; plan:790                                            | built (`VQL_IO_QUERY_CONTEXT`)                           | done  |
| Static placement diagnostic with stated multiplicity                                       | redesign §4                                                      | built for "once per reached script action"; no loop form | l     |
| Typed outcome is a value; EOF distinct; partial and uncertain flags                        | redesign §4; plan:203-212                                        | built (`io_tag` and four field functions)                | d     |
| Recovering from an outcome in the language's own control model                             | redesign §4 "each bridge defines how its control model recovers" | by path functions only; no match                         | d     |
| Cancellation of a language wait distinct from cancelling the request                       | redesign §4                                                      | built for reads and writes; absent for capture           | f     |
| Raw reads, framed reads, messages, cursor modes                                            | plan §7.1 (plan:543); redesign §8.5                              | raw only; frames and messages have no adapter or schema  | j, k  |
| A strict consistency request refuses when the backend cannot meet it                       | redesign §8.5                                                    | the file adapter refuses; VQL has no way to ask          | k     |
| Writes carry an expected generation; stale and conflict are typed                          | redesign §8.5; plan §7.2 (plan:568)                              | adapter has it; the script surface does not              | g     |
| Capture is explicit, journal-ordered, never implicit                                       | redesign §9; plan §8.2                                           | built for one read, one range, no deadline, no handle    | f     |
| One I/O stack per frontend, no blocking fallback, no frontend names in `VortexCore`/the VM | redesign §11.2 (line 1084)                                       | built; every slice re-proves it                          | all   |

Everything below is one of those clauses made reachable from a script, or a hardening item a gate
recorded and deferred.

## 2. Inventory and classification

Class **a** is in M7-VQL (frontend completeness for VQL against the contract). Class **b** belongs
to another milestone, named, and is out of scope here. Class **c** is refused or not needed, with
the reason. "Where" cites the first place the item was deferred.

### 2.1 Class a: in scope

| id  | item                                                                                        | where deferred                                                                                                        | slice |
| --- | ------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------- | ----- |
| a1  | Match an outcome's tag and fields in script syntax (`outcome(complete($payload, ..), ..)`)  | plan:1411 ("Deferred in M7-VQL-b"); vql §4.13 "only this fixed shape is accepted"                                     | d     |
| a2  | `await($h) as $r` copied to an owned record                                                 | `m7-vql-c.md` §19 "Gate round 1 additions" (line 1346); `r2-disposition.md`, "vql m3"                                 | d     |
| a3  | A script-local `let` as a write source, offset or length                                    | `m7-vql-c.md` §19 (line 1346); `m7-vql-c/r1-disposition.md` ("vql m2 resolution")                                     | d     |
| a4  | A pure aggregate joining many rows into one text cell, so a script can write a query's rows | `m7-vql-c.md` §19 (line 1339)                                                                                         | d     |
| a5  | Payload-less detached outcome constructor (a failed step copy under budget pressure)        | `m7-vql-c/r1-disposition.md` ("vortex m2")                                                                            | d     |
| a6  | Close a capability early from the script; close any it can name, host-bound included        | plan:1416 ("Deferred in M7-VQL-b")                                                                                    | e     |
| a7  | A refused close at runner destruction is not retried                                        | `m7-vql-c.md` §19 (line 1346); `m7-vql-c/r1-disposition.md:24` (engineer m4)                                          | e     |
| a8  | `within` on `open`; `within` in a unit finer than milliseconds                              | plan:1417                                                                                                             | e     |
| a9  | A deterministic end-to-end timeout test with a real, blocking file                          | plan:1413                                                                                                             | e     |
| a10 | Detached capture handles; `cancel` of a capture; list form `capture($a, $b)`                | `m7-vql-c.md` §19 (line 1333); plan:1441 ("Deferred: the other deferrals of §19")                                     | f     |
| a11 | `capture ... within` (a deadline parameter on `CaptureCoordinator::appendRequest`)          | `m7-vql-c.md` §19 (line 1334); coordinator `appendRequest` at `capture_coordinator.hpp:496`                           | f     |
| a12 | Pin a timed-out capture only when its append may have run                                   | `m7-vql-c/r1-disposition.md` ("vortex m1")                                                                            | f     |
| a13 | Coalesce the exposed payload ranges                                                         | `m7-vql-c/r2-disposition.md` ("engineer m2")                                                                          | f     |
| a14 | One reachable-graph walk, one captured-cell predicate, assert no clone rank                 | `m7-vql-c/r1-disposition.md` ("zigzag m3, m5, m6")                                                                    | f     |
| a15 | `openat2(RESOLVE_BENEATH)` containment of a write path; the grant-race test                 | `m7-vql-c/r1-disposition.md` ("engineer m2, m7")                                                                      | g     |
| a16 | Generation guards (`if unchanged`) giving `Stale`/`Conflict` on a write                     | `m7-vql-c.md` §19 (line 1336); `FileSourceAdapter` already takes `expectedGeneration` (`file_source_adapter.hpp:193`) | g     |
| a17 | `truncate`, create-or-open, `rename`, `unlink`, directory operations                        | `m7-vql-c.md` §19 (line 1335); `m7-vql-c.md` §15 refused `O_TRUNC` for the slice                                      | h     |
| a18 | A streaming `copy` between two capabilities in bounded chunks                               | `m7-vql-c.md` §19 (line 1338), §10, owner Q4 (line 1351+)                                                             | i     |
| a19 | Frame mode (line and fixed-length frames) and the cursor modes over a file                  | plan:1318, 1419; plan:874-879 ("deferred wholesale until the consumer milestone")                                     | j     |
| a20 | A way to ask for a `stable` read, refused typed when the adapter cannot prove it            | plan:1318; redesign §8.5; `SourceConsistency` at `io_cells.hpp:352`                                                   | k     |
| a21 | Reads and writes on host-granted non-file endpoints (in-memory stream, connected socket)    | plan:1318 ("any action other than a bounded read of one file capability"); `m7-vql-c.md` §19                          | k     |
| a22 | Per-row, per-loop and `for`-body effects                                                    | plan:1361-1364; plan:790 ("no per-row fan-out until an explicit later gate"); `m7-vql-c.md` §19                       | l     |
| a23 | Constants and the `vqueryc`/`vquery` host audit for every new surface; UX journey pass      | plan:790 acceptance ("vquery/vqueryc CLI host I/O audited"); AGENTS.md "UX validation workflow"                       | m     |

### 2.2 Class b: owned elsewhere, out of scope

| item                                                                                                              | where deferred                                                                | owner                                                                                                                                                           |
| ----------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `system://settings` entries for the script timeout, write and capture bounds, request budgets                     | `m7-vql-c.md` §19 (line 1341); `defaultScriptTimeout` at `vquery/main.cpp:68` | **M8** (budgets, config, release); this roadmap only lists the constants (slice m)                                                                              |
| A UI for `:reconcile` and `:captures`                                                                             | `m7-vql-c.md` §19 (line 1341)                                                 | xuzz UI work (`xuzz-ui-design`); the REPL commands stay the contract                                                                                            |
| Linking a result row to the capture receipt as an xanalink rather than a shared address                           | `m7-vql-c.md` §19 (line 1342)                                                 | the xudu-zigzag bridge ("federated cross-store link source is explicitly deferred")                                                                             |
| Remote (swarm) publication of a capture receipt                                                                   | `m7-vql-c.md` §19 (line 1344)                                                 | swarm and publication (`publication.hpp`, transcopyright)                                                                                                       |
| A real stable-snapshot adapter (git blob, copy-on-write snapshot)                                                 | `FileSourceAdapter` offers `Live` only (plan:870); no adapter uses `Stable`   | **M5 follow-up** (file/media): VQL only supplies the request surface (a20)                                                                                      |
| Message mode and message endpoints; `NetStreamRing`, `JournalLens`, `SwarmLens` adapters                          | plan "M4 known gaps" (plan:821-823); redesign §4 message rows                 | **M4 follow-up** (adapters); VQL gets message mode when an adapter states oversize semantics (plan:213)                                                         |
| Making `ProcessIoAdapter` killable mid-read; the `popen` limit                                                    | `source_adapters.hpp:159-190`; plan M4 notes                                  | **M4 follow-up**; see owner question Q6 for why VQL does not expose it                                                                                          |
| The VQL lens bridge (generator and gesture sources)                                                               | redesign §4 table, VQL row ([`vql-lens-bridge.md`](vql-lens-bridge.md))       | lens work, not the I/O frontend                                                                                                                                 |
| VPL, vprolog, Vlace, VIcon and VAgent effect surfaces                                                             | plan §13 `M7-VPL` ... `M7-VAgent`                                             | their own milestones; the slices here keep the shared pieces language-neutral so they can reuse them                                                            |
| Making the I/O service restartable or the actor journal resumable                                                 | `m7-vql-c.md` §14 (P9)                                                        | M3 actor profile; a script is not durable                                                                                                                       |
| `zigzag m2` copying the `d.io.source` observation as cells                                                        | `m7-vql-c/r1-disposition.md` ("zigzag m2")                                    | design question for the I/O cell schema (M0 owner); no VQL consumer needs it before slice f ends, which revisits it only if capture of a copied record needs it |
| `--in-place` piece order (zigzag m4); the unreached `writeResultSlice` `invalid_argument` (zigzag m5, unverified) | `m7-vql-c/r2-disposition.md`                                                  | `vquery` persistence of results, owned by the existing `--in-place` path, not by effects                                                                        |

### 2.3 Class c: refused or not needed

| item                                                                    | where                                           | reason                                                                                                                                               |
| ----------------------------------------------------------------------- | ----------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- |
| Capture a write's bytes                                                 | `m7-vql-c.md` §1 "Out"                          | a write is script-authored, so nothing in it is transient; capturing it would manufacture an origin for text the author already holds                |
| Writes from `vqueryc`                                                   | `m7-vql-c.md` §1 "Out"                          | `vqueryc` is compile-only and opens no endpoint; it keeps compiling effect scripts so the bytecode stays inspectable                                 |
| A write-intent journal (P9)                                             | `m7-vql-c.md` §14                               | nothing can resume to use it; R11 version bump for a path no frontend resumes                                                                        |
| A persist-taint flag on arena cells (P8)                                | `m7-vql-c.md` §14                               | does not propagate through pure functions, so it launders; a core change for one frontend                                                            |
| A `Cancel` opcode, a `Capture` opcode, a `Copy` opcode                  | plan:1359 (cancel); `m7-vql-c.md` §15 (capture) | cancellation and capture are host policy; relation providers and `Await` already express them                                                        |
| Automatic retry after `Unknown`; implicit capture; synchronous fallback | plan §14                                        | standing first-release refusals; every slice below keeps them                                                                                        |
| Process endpoints reachable from VQL in this milestone                  | owner question Q6                               | the adapter's `popen` read cannot be stopped, so one hung child starves the default two-worker pool; refused until the adapter is killable (class b) |
| `rmdir` and recursive removal; overwrite-by-rename; `O_TRUNC` on open   | slice h                                         | each destroys more than the author named in one verb; `unlink` of one file and `truncate` to a named length say exactly what is lost                 |
| Bounded-concurrency fan-out of effects (`each ... parallel N`)          | slice l                                         | VPL's domain (equal-shape sums, one waiter); a VQL loop is sequential, which keeps one waiter per activation and the budgets simple                  |
| An iteration count ceiling on a loop                                    | slice l, owner question Q7                      | a ceiling the author must work around; the brakes are the script timeout, the grants and the request budget                                          |

## 3. Ordering: dependency and risk

```text
d values  ->  e lifecycle  ->  f capture completion
                    \                   \
                     g containment+guards -> h namespace ops -> i copy
                                                    \
                       j frames+cursor      k other endpoints+stable
                                  \              /
                                   l loops (last: multiplies everything above)
                                          \
                                           m closure and audit
```

The rules behind the order, each priced:

1. **Frontend-only slices before service slices.** d adds no world effect at all: it names, matches
   and joins values. It lands first because every later slice's gate wants to write its acceptance
   tests as `match`, and because it is the cheapest slice to reverse if the surface is wrong.
1. **Lifecycle before new destruction.** e (closing) precedes h (rename, unlink, truncate) so a
   script that opens a file, works and closes it exists before scripts that destroy.
1. **Containment before new path verbs.** g's `openat2(RESOLVE_BENEATH)` lands before h and i add
   more calls that take a path. The residue the engineer lane recorded (a symlink swapped between
   grant check and open) is tolerable for `open for write create`; it is not for `rename`/`unlink`,
   which act on the name and not on a held descriptor.
1. **Capture completion beside, not after, the file verbs.** f touches `CaptureCoordinator`, the
   journal and the permascroll; h and i touch the file adapter. They share nothing but the runner,
   so they can be gated in either order; f is placed first because its a12 (pin only if the append
   may have run) removes a manual `:reconcile` the author pays today.
1. **Loops last.** l turns "once per reached action" into "once per row". Every earlier slice's leak
   pin (10^4 runs bounded) becomes a per-iteration obligation; building l on unfinished d..k would
   let a leak in any of them hide behind the loop's own tests. This is the highest-risk slice and
   the one the plan reserves "an explicit later gate" for.
1. **Frames and endpoints are the largest service work and independent of loops.** j adds cell
   schema and adapter work that VPL and Vlace will reuse; k adds grants. Neither needs l. They sit
   between h/i and l so l's gate can exercise a framed read per iteration, the natural use ("one row
   per line").

Sizing check: each slice below holds one new idea, touches a bounded set of files, and its
acceptance fits one gate iteration of two to four critics (the M7-VQL-c gate took three rounds with
four critics for a slice with writes, capture, journals and a REPL, which is the upper bound; none
below is that large except f and l, which carry a split rule).

## 4. Cross-cutting rules for every slice

- **Contextual verbs.** A new effect verb (`close`, `copy`, `truncate`, `rename`, `unlink`, `mkdir`,
  `list`, `read_frame`) is recognised only as the head of a request inside `effect { ... }` or after
  `submit(`; it is not added to the reserved call names. M7-VQL reserved `submit`, `await` and
  `read_bytes` and M7-VQL-c added `write_bytes`, `append_bytes` and `sync`, which already breaks a
  user `def` of those names (vql §4.13 "Where it may stand"); `capture` stayed contextual because a
  test `def pure capture()` exists (`m7-vql-c.md` line 1066). The aggregate `join` of slice d is an
  ordinary function name, checked against a user `def` of the same name at parse time.
- **No core file.** Each slice lists "Core change?" below. A gate finding that needs one stops and
  asks; the candidates are §5, the refused ones remain P8 and P9.
- **Same acceptance harness.** `VqlScriptConformance` (the M7a checks) passes unchanged, a leak pin
  of 10^4 runs bounds the arena, the service's requests and `stepGraphs`, and every refusal is a
  typed code in the built-in registry, never free text. Each slice adds change-history rows 16.10
  onward to `vql-query-language.md` and a section to this roadmap's successor notes (the slice
  design note `vortex-io-m7-vql-<letter>.md` when it needs more than a section of §4.13).
- **Named constants only.** Every new bound is a named `constexpr` with its rationale beside the
  existing `FileReadHostOptions` constants (`file_read_host.hpp:63-96`), and slice m collects them
  for M8. No sentinel: absence is `std::optional`, refusal is `std::expected`.
- **Headless.** Tests run with the repository's headless environment and the `build/xdg/` redirect;
  nothing opens a window. Real file tests use a temp directory under `build/`.
- **Gate shape.** Reports under `design/projects/vortex-io-implementation-review/m7-vql-<letter>/`
  in the existing format (`brief.md`, `r<N>-<role>.md`, `r<N>-disposition.md`, `README.md`). The VQL
  and Vortex experts are always present; the engineer joins when a slice touches durable formats,
  concurrency, the permascroll or paths; the zigzag expert joins when it touches cells, owned
  records or the capture store.

## 5. Candidate core proposals

Every slice is expected to need no core file (`store.hpp`, `ops.hpp`, `compact_op.hpp`,
`store_tables.hpp`, `segmented_ops_spool`, `manifold`, `arena_manifold`, `vortex_core`,
`vortex_vm`). The capture loop of M7-VQL-c was built from `Jump`, `Branch`, `BranchFalseCell`,
`Await` and relation providers (`m7-vql-c.md` §6.2), which is the evidence that a loop does not need
the VM changed. One proposal is reserved, conditional on a spike:

**P10 (candidate, conditional): a VM relation-iteration cursor that survives `Await`** (slice l).
`vortex_vm.{hpp,cpp}`. Justification: a script loop over *materialised* rows runs its body once per
row, and the body parks. If the runner cannot keep the iteration index and the row set across a park
by compiling per iteration, the VM would need an iteration cursor as a first-class resumable thing.
Minimal alternative (the plan of record): **host-driven iteration**. The runner evaluates the query
stretch to completion, then re-enters the compiled body once per row with the row bound to the loop
variable, exactly as it resumes a parked action today; no opcode is added. Price of the alternative:
one body compilation per iteration unless the compiler lowers the body once with the loop variable
as a session variable the runner rebinds (the cheaper reading, and the one the spike S-l measures).
Refused for P10: fail-driven iteration with `Choice`/`Fail`, because `Fail` restores arena cells and
so frees each iteration's outcome and the P6 rollback handler would orphan each admitted request
(plan:1471-1490); the loop's results would have to be host-held, which is the host-driven design
again.

No other slice reserves a number. P8 and P9 stay refused (`m7-vql-c.md` §14).

## 6. The slices

### 6.1 M7-VQL-d: values (name, match, join)

**Scope.** Everything that lets a script use an outcome and its text without a new world effect: a1
outcome matching, a2 owned `await($h) as $r`, a3 `let` as a source, a4 `join`, a5 the payload-less
detached outcome constructor that a2 needs.

**Surface (sketch).**

```vql
effect { submit($src, read_bytes(0, 4096)) ... } as $r;
match $r {
  complete($p)         => ... ,
  end_of_stream        => ... ,
  partial($p, $why)    => ... ,
  failed($code, $d)    => ... ,
  _                    => ...
}
let $text := join(for $x in people return $x.name, "\n");
effect { open("out") for write create } as $o;
effect { submit($o, write_bytes(0, $text)) ... }       // a `let` as a source (a3)
```

`match` over an outcome is a pure statement: each arm is a row-yielding expression, the arm is
chosen by a branch on the tag ordinal the provider already answers (`io_tag`), and the fields bind
through the existing `io_payload`, `io_error`, `io_detail` providers. A pattern naming a tag whose
variant cannot occur for that operation (a `Complete` pattern is fine on a read; `end_of_stream` on
a write) is a compile-time diagnostic from the operation table at plan:203-212, and a `match`
without `_` must name every tag the operation can end with, so forgetting `EndOfStream` is a static
error, not a runtime surprise. `join(expr, sep)` is a pure aggregate over rows lowered like `count`
(`compile_path_functions.cpp:70-132`), folding the existing text `+` (vql §4.12, line 2153) with a
separator, bounded by the write bound (`FileReadHostOptions::writeBytes`) and typed `VQL_IO_BOUND`
(new code) when it would exceed it.

**Why a1 is worth building rather than leaving to path functions.** M7-VQL-b closed the *access*
half with `io_tag`/`io_payload`. What remains is the control half the redesign asks of each bridge
("each bridge defines how its control model recovers from that typed value", redesign §4). With only
path functions, a script that forgets one of the eleven tags silently yields an empty result ("else
none" is an empty result by design, vql §4.13). The price is a grammar addition and one
exhaustiveness check; the alternative, nothing, leaves every later slice's examples unable to branch
on an outcome.

**Depends.** M7-VQL-b and -c (shipped).

**Acceptance.**

1. `match` selects by tag and binds fields; a refusal outcome and every terminal tag has a test.
1. A `match` that misses a tag the operation can produce is a compile-time diagnostic naming the
   tag.
1. `await($h) as $r` is a record: `$h` stays awaitable and `$r` survives `$h`'s request retirement
   (the behaviour change m7-vql-c r1 refused for lack of this slice's constructor).
1. A `let` bound in the script is accepted as a write source, offset or length; a stale marker is
   still `VQL_IO_STALE_BINDING`.
1. `join` over 10^4 rows equals the fold of `+` and is refused over the write bound with a typed
   code.
1. 10^4 named-outcome runs stay bounded (the criterion-15 pin, extended to `match`).

**Core change?** No. `BranchFalseCell` exists (P6 reuses it); `join` is the text `+` fold, so no new
opcode. If `join` cannot be a fold without per-row allocation measured as a problem, the fallback is
a relation provider (host C++) with the justification the AGENTS.md rule wants (allocation
primitives), and that is the first place to look at the gate.

**Owner questions.** Q1 below (match form).

**Gate.** VQL, Vortex, zigzag (a2 creates owned records, cells). Round budget: two.

### 6.2 M7-VQL-e: lifecycle (close, deadlines on open, the real timeout test)

**Scope.** a6 closing, a7 the refused close at destruction, a8 `within` on `open` and units finer
than a millisecond, a9 the real-file timeout test.

**Surface.** `effect { close($f) } as $c`: the same `submit`/`await` machinery, a close request on
the capability's endpoint, `Complete` or the typed refusal as a value. A capability the script
opened may be closed early; the runner then skips its end-of-script close for that capability (today
every ending admits one, plan:1365-1372). `open(...) within 250 ms` sets the open's
`OpenSpec::deadline` per request instead of the host-wide `openDeadline` (`file_read_host.hpp:81`).
`within 500 us` and `within 2 s` join `ms`/`s`; the clock keeps millisecond ticks internally and
rounds a finer deadline up, stated, never down.

**Closing any capability the script can name (a6, ruled 2026-11-26, Q2).** The owner ruled that a
script may close any capability it can name, host-bound ones (`--io-file`, `:open`) included. This
is **not** the draft recommendation, which was to refuse with `VQL_IO_CLOSE_NOT_OWNED`; that code is
dropped. The consequence the slice must define rather than leave open: closing a host-bound
capability must leave the host and REPL binding in a defined state. The ruling is that the **session
name stays bound and goes stale**: the next use of it ends with the typed stale-binding diagnostic
(`VQL_IO_STALE_BINDING`, the same code a stale `let` marker already earns), never a silent
rebinding, never a dangling descriptor. The host's own record of the capability (the grant, the
path, the mode) is retired with the close, so `:open` of the same name afterwards mints a fresh
capability. The REPL also gets `:close NAME`, the same close path from the human's seat, so the
human has what the script has. The price the owner accepted: a script can turn a session grant into
a one-shot grant. The mitigation is only visibility: the close is an ordinary journal-visible
request, and a `:close`/script close of a host-bound name is logged by name.

**a7, refused close at destruction.** The destructor has no host pump (`r1-disposition.md:24`), so a
refused close has nothing to retry with. Resolution: when the runner dies with a close the service
refused, it hands the endpoint to a host-owned "pending close" list that `FileReadHost::pump`
retries under the existing `maxCloseAttempts`, and the host's own destructor makes its final attempt
before the workers join. The runner stays thread-free; the cost is one list in the host.

**a9, the real timeout test.** A regular file never blocks, so expiry is proved on an injected clock
today (plan:1413). A FIFO whose writer never opens blocks the worker's `open`/`read`, which makes
`within` and the script timeout testable against a real descriptor; the test sits in `tests/xuzz/`
with the temp directory under `build/`, headless, with a bounded real wait.

**Depends.** d (the examples `match` on `Complete`/`TimedOut`).

**Acceptance.** Early close admits one close request and the script-end path does not duplicate it;
closing a host-bound name succeeds and leaves the session name stale (`VQL_IO_STALE_BINDING` on the
next use), and `:close NAME` does the same from the REPL; a refused close at destruction is retried
and ends with no descriptor left (count open fds before and after); `within` on `open` ends a
blocked open as `TimedOut` on a FIFO; a sub-millisecond `within` rounds up, never down.

**Core change?** No.

**Owner questions.** Q2 (ruled: any capability may be closed).

**Gate.** VQL, Vortex, engineer (descriptor lifetime, FIFO test validity, close ordering).

### 6.3 M7-VQL-f: capture completion

**Scope.** a10 detached capture handles, `cancel` of a capture and the list form; a11
`capture within`; a12 pin only if the append may have run; a13 coalesced exposure ranges; a14 the
three zigzag refactors.

**Surface.**

```vql
effect { capture($r) => $q } as $h;       // detached: the handle, or the refusing outcome
effect { cancel($h) } as $c;              // asks the coordinator to stop, then waits
effect { capture($a, $b) within 5 s } as $c;   // list form, one deadline for the whole capture
```

**Why detached capture needs a rule, not just code.** A read's request is the service's; a capture's
steps are the *runner's*, because `CaptureCoordinator` is driven by the relation loop around `Await`
(`m7-vql-c.md` §6.2). A detached capture means someone drives the next step after the script has
moved on, and that someone is the runner, so a capture would keep acting after the script's last
statement. M7-VQL-c refused exactly that ("an abandoned script finishing its side effect", §11). The
ruling this slice proposes (Q3) is **structured completion**: a script that holds an unawaited
detached capture at its last statement parks (bounded by the script timeout) until each such capture
reaches a terminal state, and a timeout then abandons and pins as §11 already does. Refused
alternatives: abandon-and-pin at the script's end (makes every unawaited `=> $q` capture cost a
`:reconcile`); background completion past the script (the refusal above).

**a11, `within`.** `CaptureCoordinator::appendRequest` takes no deadline today
(`capture_coordinator.hpp:496`); the slice adds an optional deadline parameter and passes it to
`IoService::submit(..., within)` for the append, so the existing expiry (`stopRequest` then
`TimedOut`) applies. A timed-out capture past `AppendRequested` is still pinned uncertain (§11).

**a12.** Today a capture retired after `AppendRequested` pins its intervals even if the append was
never admitted. The slice records, per capture, the phase of the last admitted step and pins only if
the append request was admitted (it may have run); a capture timed out before the append was
admitted frees without a pin. The price is a phase flag in the coordinator and a crash-ordering test
per phase (`m7-vql-c.md` §8 lists the five points); the saving is a `:reconcile` the author no
longer has to type for a capture that never touched the permascroll.

**a13, a14.** Coalescing is an optimisation for a script that exposes many adjacent ranges; it is
done only if a measured case (a 10^4-range exposure) shows the linear check costs (the plan's rule:
no performance claim without data). The three refactors are mechanical and share the gate.

**Depends.** d (match on the capture receipt), e (a clean end of script with open captures).

**Acceptance.** Detached capture returns a handle and the script's own end waits for it; `cancel` of
a capture before the append is admitted leaves no pin and no permascroll growth; after the append
admission it pins and `:captures` lists it; a list capture appends each uncovered range once under
one deadline; a deadline expiring at each of the five crash points matches §8's table; the 10^3
capture-run leak pin holds for detached runs.

**Core change?** No (`CaptureCoordinator`, `capture_host`, `capture_journal` are not core-list
files). If a12 needs a journal field it is a version bump of the capture journal under R11 ("bump
the version, don't carry a shim"), still not a core file.

**Owner questions.** Q3.

**Gate.** VQL, Vortex, engineer (journal ordering, fsync), zigzag (capture store, owned records).
Split rule: if the round-1 finding count shows f exceeds one iteration, split into f (detached,
cancel, list, within) and f2 (a12, a13, a14, which change no surface).

### 6.4 M7-VQL-g: containment and guards

**Scope.** a15 `openat2(RESOLVE_BENEATH)` and the grant-race test; a16 generation guards.

**Surface.**

```vql
effect { submit($o, write_bytes(0, $text) if unchanged($r)) } as $w;
// $r: a prior read's outcome; its observation's generation is the expectation
```

**a15.** The grant check resolves the path with `weakly_canonical` and checks the prefix
(`file_read_host.cpp:50-63`); a symlink swapped between that check and the worker's `open` escapes
the root (engineer m2, "documented residual race"). `openat2` with
`RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS`-class flags relative to a held root descriptor removes the
race for every verb that takes a path; the adapter keeps a root descriptor per grant. Price: Linux
5.6+; the fallback on older kernels or other platforms is the current check, announced once through
`GLEDITOR_LOG_DEBUG` and recorded as the weaker guarantee. Refused alternative: keep the race and
document it, which is fine for `open for write create` and not for slice h.

**a16.** `if unchanged($r)` names a read outcome whose observation carries a source generation
(`ReadObservationRecord`, `io_cells.hpp`); the write is admitted with that `expectedGeneration`. A
mismatch known at admission is `Stale`, one found at execute is `Conflict` (redesign §8.5; the
adapter already implements both). Recommended (Q8): the guard names an *outcome*, never "whatever
the file was last seen as", so the expectation is visible in the text and cannot be changed by an
intervening read. An adapter that cannot verify a generation refuses the guard (`prepare` already
does, `source_adapters.hpp:103`).

**Depends.** e (a guarded write of a file the script opened and closed).

**Acceptance.** A swap test: a thread replaces a directory with a symlink between grant check and
open and the write never lands outside the root (this is a15's pin; it fails before the change); a
guarded write after an external modification is `Stale` or `Conflict` per phase, the file is
untouched, and `Unknown` after a partial write invalidates the generation (redesign §8.5).

**Core change?** No.

**Owner questions.** Q8.

**Gate.** VQL, Vortex, engineer (race tests, kernel fallback).

### 6.5 M7-VQL-h: file namespace operations

**Scope.** a17: `truncate`, create-or-open, `rename`, `unlink`, `mkdir` and directory `list`.

**Surface.**

```vql
effect { open("log.txt") for write create_or_open } as $o;      // O_CREAT without O_EXCL
effect { submit($o, truncate(0)) } as $t;                        // to a named length
effect { rename("a.tmp", "a.txt") } as $r;                       // within one write root
effect { unlink("a.tmp") } as $u;                                // one file
effect { mkdir("out") } as $m;
effect { list("out") } as $l;                                    // bounded, one outcome, entries as text
```

**Pricing.** M7-VQL-c refused `O_TRUNC`, create-or-open and these verbs because each destroys or
hides data (`m7-vql-c.md` §15, line 717-738). This slice admits them under conditions that restore
the property that made the refusal right, which is that the author names exactly what is lost:

- **Per-verb grant** (Q4): `--io-write-root DIR[:verbs]`, default verbs the present set (`write`,
  `append`, `create` exclusive). A destructive verb must be named by the human: `truncate`,
  `rename`, `unlink`, `mkdir`, `create_or_open`. A script cannot grant itself one.
- `rename` stays inside one write root and one filesystem and uses `renameat2` with
  `RENAME_NOREPLACE`, so it never overwrites silently; an overwrite is `unlink` then `rename`, two
  named effects.
- `truncate(n)` takes a length, never "to the file's current position"; `O_TRUNC` on open stays
  refused.
- `rmdir` and recursive removal are class c (§2.3): one verb that destroys a tree is the opposite of
  naming what is lost.
- `list` is a read of a directory: one bounded outcome whose payload is the entries as text, over
  the existing payload bound; an overflow is `Overflow`, never a truncated listing presented as
  complete (plan §14 on oversized messages).

All five run on workers through the file adapter, resolve through the g containment, and are single
requests with the usual outcomes (`Complete`, `Failed` with the errno detail, `Conflict`, `Unknown`
after an uncertain partial). A name-acting verb is idempotent only by being refused the second time
(`unlink` of a missing file is `Failed(NotFound)`, a value), and an `Unknown` outcome is never
retried (plan §14).

**Depends.** g (containment), e (close ordering, so a rename of an open file is defined: it is
allowed, the descriptor is unchanged).

**Acceptance.** Each verb has a granted and an ungranted test (ungranted is `Rejected` before
anything starts); a rename never overwrites; every verb is refused outside the root, by symlink and
by `..`; a crash test around `rename` (the destination is whole or absent); a leak pin of 10^4 runs
per verb class.

**Core change?** No.

**Owner questions.** Q4, Q5 (directories).

**Gate.** VQL, Vortex, engineer.

### 6.6 M7-VQL-i: streaming copy

**Scope.** a18: `copy($src, $dst)` moving bytes between two capabilities in bounded chunks, so a
file larger than the one-request write bound (1 MiB, owner Q4 of M7-VQL-c) can be moved.

**Surface and design.** `effect { copy($src, $dst) [from off] [length n] } as $c` is a host-side
coordinator with the same shape as capture: a relation loop around `Await` reading one chunk
(`read_bytes(off, chunk)`), writing it (`write_bytes`/`append_bytes`), and asking again, each step
an ordinary admitted request that obeys the request budget, with the script's `within` as the
overall deadline. Chunk size is a named constant (default the read payload bound). The result is one
outcome: `Complete` with `transferredBytes` equal to the length, or `Partial` carrying how many
bytes reached the destination and a typed cause (the read failed, the write failed, the source
changed). A source change between chunks is detected by the g generation guard applied across chunks
(`Conflict`), because a copy of a file that changed mid-copy is not a copy of any version.

**Why a coordinator and not a script loop.** A loop of `submit` pairs in script text is slice l's
loop, which does not exist yet; and a copy must not leave a half-written destination named as
`Complete`. Chosen: the coordinator, whose state is host state (like `CaptureHost`) and whose steps
are visible requests. Refused: a `Copy` opcode (host policy); a destination write of the whole
source in one request (the very bound the slice exists to pass).

**Depends.** g (generation guard), h (so `copy` can create-or-open its destination and a script can
`rename` it into place, the usual atomic-replace idiom).

**Acceptance.** A source ten times the bound copies in ten chunks with ten admitted writes; a
mid-copy source change is `Conflict` with `transferredBytes` equal to what was written; a timeout
mid-copy is `TimedOut` with `uncertain` and the destination length equal to the reported prefix; a
failed destination write leaves no waiting request; 10^3 copy runs leak nothing.

**Core change?** No.

**Gate.** VQL, Vortex, engineer.

### 6.7 M7-VQL-j: frames and cursors

**Scope.** a19: line and fixed-length frame reads over a file endpoint, and the cursor modes that
make them safe.

**Surface.**

```vql
effect { open("data.csv") for read frames line max 4096 } as $f;
effect { submit($f, read_frame()) } as $r;                // one frame per request
effect { submit($f, reset()) } as $z;
```

The contract is plan §7.1 (plan:543-566): `ReadFrame(mode, maxBytes)` may read ahead, and the unread
suffix stays leased by the endpoint for the next frame; mixing a raw read, a frame read, a seek and
a reset must advance one declared cursor or return `CursorModeConflict`; oversize is a declared
policy; a decoder holds bounded state; chunk boundaries may split a delimiter or a code point. The
plan deferred all of it "until the consumer milestone that needs it" (plan:874-879). VQL is the
first consumer: the natural VQL use is one row per line.

**Scope of the service work (the real cost).** `BufferPolicy` is `Raw`/`Fixed`/`Line` here;
`Message` is class b (§2.2). `OversizePolicy` recommended (Q5): a line longer than `max` ends as
`Overflow`, the cursor stays at the start of that line and nothing is consumed, so no data is lost
silently; the author can read it raw with a larger bound. Encoding is UTF-8 with the
split-code-point rule stated; invalid UTF-8 in a frame is `Failed` with a detail, never replacement
characters. The cursor is service state on the endpoint (an endpoint field, not a cell schema change
beyond what `IoDimension` already carries; if a new dimension is needed it is added in
`io_cells.hpp` under the M0 schema owner's rule and listed in the gate brief). Mixing a raw and a
frame read on one endpoint without a `reset` is `CursorModeConflict`.

**Shared with VPL.** M7-VPL refuses message and frame modes with `IO_UNSUPPORTED_MODE` until "their
adapter gate" (plan:724, 791). This slice is that gate for frames over a file; it is written
language-neutral (`FileReadHost`, `FileSourceAdapter`), and VPL consumes it later. The owner
question (Q5) is whether VQL builds it first, which the recommendation says yes to.

**Depends.** e (frame cursor state dies with the endpoint's close), d (matching `EndOfStream` is how
a script ends a frame loop; without l it is one frame per statement, which is the honest limit of
this slice).

**Acceptance.** Line frames over a file split at the right place across a chunk boundary and across
a code point; an oversize line is `Overflow` with the cursor unmoved; raw-after-frame without
`reset` is `CursorModeConflict`; `EndOfStream` ends the file; a zero-length line is a valid frame,
not EOF; cancel mid-read leaves the cursor either advanced past the frame or not, and the outcome
says which (`uncertain` set for the former only when the backend cannot say).

**Core change?** No (`io_cells`, `io_service`, the file adapter are not core-list files). If the
endpoint cursor needs a persisted field, it does not: endpoints are transient and non-persistable
(plan:760-765).

**Gate.** VQL, Vortex, engineer (decoder state, cancel semantics), zigzag (any new cell dimension).

### 6.8 M7-VQL-k: other endpoints and the consistency request

**Scope.** a21 host-granted non-file endpoints; a20 asking for a stable read.

**Surface.** Capabilities for non-file endpoints are bound by the human, as `--io-file` is:
`--io-stream NAME` (an in-memory stream the host fills, mainly for tests and piping) and
`--io-net NAME=fd:N` or a connected socket the host opens from `HOST:PORT` under
`--io-net NAME=HOST:PORT` (the adapter does not own connection setup,
`source_adapters.hpp:194-213`). Scripts use the same `submit`/`read_bytes`/`write_bytes` on them;
the verbs not meaningful for a stream (offset-bearing reads past the first, `truncate`) are refused
by the adapter's `validateSpec` as a value. `open(...) consistency stable` is accepted by the
grammar and passes `ioConsistencyStable` in the `OpenSpec`; `FileSourceAdapter` refuses it before
admission today (plan:870), and the refusal is the outcome the script sees, which is exactly the
redesign's "a strict request refuses when the backend cannot meet its requested consistency" (§8.5).

**Why the request surface is in scope though no adapter satisfies it.** The contract has two halves,
the frontend can *ask* and the backend can *answer*; M5 follow-up owns the answer. Building the ask
now means the first adapter that can prove a snapshot needs no VQL change, and the refusal path
(typed, nothing started) is exercised on every build. Refused: wait until an adapter exists (the
frontend would then change under a different milestone's gate, with nothing in VQL's own acceptance
covering it).

**Process endpoints.** Not exposed (class c, Q6). The `ProcessIoAdapter` comment documents that a
child that never closes stdout blocks `pclose`, and a cancelled read "may still run the child to
completion" (`source_adapters.hpp:159-190`). `FileReadHostOptions::workers` is 2 by default; two
hung children would starve every request of the session, including closes. The right fix is an
adapter that can kill its child (class b); until then a script that wants a command's output reads a
file the human produced.

**Depends.** j (a socket and a file share the framed-read verbs), g (grants naming roots now name
endpoints).

**Acceptance.** A stream endpoint reads and writes with typed outcomes; a socketpair-backed net
endpoint round-trips bytes and ends as `EndOfStream` when the peer closes; a net read cancelled
mid-wait stops; `consistency stable` on a file is a typed refusal with nothing started; the grant is
required (no flag, no endpoint, same default-deny as writes); the conformance harness passes with a
non-file adapter.

**Core change?** No.

**Owner questions.** Q6, Q9.

**Gate.** VQL, Vortex, engineer (descriptor ownership, fd lifetime, the single-flight mutex).

### 6.9 M7-VQL-l: per-row and loop effects

**Scope.** a22: an explicit script-level loop whose body holds effects. Plan:790 reserves this for
"an explicit later gate"; this slice is the design for that gate and carries a spike.

**Surface.**

```vql
let $names := for $x in files return $x.path;            // a pure stretch, completes first
each $p in $names {
  effect { open($p) } as $f;
  effect { submit($f, read_bytes(0, 4096)) ... } as $r;
  effect { close($f) };
  return io_tag($r), io_bytes($r)
}
```

**Rules.** The query that feeds the loop is a pure stretch and **completes before the first
iteration**; its rows are materialised, so the iteration count is a fact fixed before any effect
(this is the static multiplicity the redesign's shared contract asks each bridge to state, redesign
§4: "each bridge identifies ... their evaluation multiplicity"). The body runs once per row,
sequentially, one iteration at a time: one waiter per activation, the same parking and same-request
resume, a `Pending` per iteration. `for` stays row traversal and an effect in a `for` body stays
`VQL_IO_QUERY_CONTEXT` (the loop is `each`, a new statement, so no existing query changes meaning).
The body's results append in order; the script's value is the concatenation of the iterations'
final-statement rows, which is the same as running the script N times today without the N process
starts.

**Brakes (Q7).** No iteration-count ceiling, which would be a limit an author works around by
splitting scripts. The brakes are the ones that already exist and each is exercised in the
acceptance: the per-script timeout (`VQL_SCRIPT_TIMEOUT`) abandons a loop mid-way, with the
iterations so far reported; the destructive verbs need a human grant (h); the per-iteration cleanup
keeps the arena bounded (below); and the iteration's requests obey the same request budget. Refused:
a fan-out form (`parallel N`), class c.

**Per-iteration cleanup is the hard acceptance.** Each iteration's script-owned records, request
handles, opened capabilities (closed at the end of the iteration unless bound outside), captures and
the compiled body must be freed when the iteration ends, with the shared cell deletion that P7
already supplies. This is exactly where d..k's leak pins are worth having.

**Spike S-l (first stage of the slice).** Measure the two lowerings of §5 on 10^4 iterations of a
read: (1) body compiled once, loop variable rebound by the runner; (2) body compiled per iteration.
Pick (1) unless it cannot keep the iteration across a park without a VM change, in which case P10 is
raised with this measurement as its justification (the plan's rule: no optimisation without a
number).

**Depends.** d (matching outcomes per iteration), e (close per iteration), f (captures in a loop), h
and i (the verbs a loop usually repeats), j (the common loop reads one frame per iteration).

**Acceptance.** The iteration count equals the row count of the pure stretch and is stated by `:asm`
and `:effects` (a new REPL listing of the script's static effect sites and their multiplicity, which
makes the multiplicity visible before running); 10^4 iterations of open/read/close leave the arena,
service requests and descriptors at their start values; a timeout at iteration k reports k-1
complete iterations and the timeout, and closes what was open; `VQL_IO_QUERY_CONTEXT` still refuses
an effect inside `for`, `let`, `where`, `if`, a lambda and a `def`; the M7a conformance harness
passes unchanged.

**Core change?** No expected; P10 only if S-l says so (§5).

**Owner questions.** Q7.

**Gate.** VQL, Vortex, engineer (bounds, leaks, timeout-mid-loop), zigzag (arena reclamation per
iteration). Two rounds budgeted; this is the slice most likely to need three.

### 6.10 M7-VQL-m: closure and audit

**Scope.** a23: no new world effect. It makes the milestone checkable.

- **Host audit.** `vquery` and `vqueryc` re-audited for every new surface: no read, write or open on
  the evaluation thread, every persistence path (`-o`, `--in-place`, `:save`, `--capture-dir`) still
  gated by coverage, `vqueryc` still compile-only, `:effects` and `:asm` showing the new forms.
- **Constants for M8.** One table listing every named bound the slices added, with rationale and
  value, ready for `defaultSettingSpecs()` in `system_docs.cpp` (AGENTS.md: "adding a setting means
  adding a `defaultSettingSpecs()` entry, which is easy to skip"). M8 owns adding them; this slice
  delivers the list so M8 cannot miss one.
- **UX validation.** Run the [`ux-validation`](../.claude/skills/ux-validation/SKILL.md) skill on
  `vquery` against a new journey in `design/ux_workflow_real_work.md`: read a file, query it, write
  a report, capture and persist, recover a pinned capture with `:reconcile`, close and resume. A
  step reachable only by a flag, script or file is a finding, not a pass (AGENTS.md).
- **Documentation.** `vql-query-language.md` §4.13 reorganised from a changelog of slices into one
  surface description; the plan's M7-VQL block collapsed to a status row and a list of the gate
  records.

**Acceptance.** The exit criterion of §8.

**Gate.** VQL, Vortex. No engineer or zigzag lane: nothing durable or concurrent changes.

## 7. Owner questions

Each has a recommended answer; none blocks the slices before its own.

| #   | slice | question                                                                                                                                                        | recommended answer                                                                                                                                                                |
| --- | ----- | --------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Q1  | d     | The outcome `match` syntax: exhaustive with `_`, or free-form with an implicit fallthrough?                                                                     | Exhaustive, `_` allowed, missing tag is a compile diagnostic. A silent fallthrough is the "forgot `EndOfStream`" bug the match exists to end.                                     |
| Q2  | e     | May a script close a host-bound capability?                                                                                                                     | Ruled 2026-11-26: yes, any capability it can name; `VQL_IO_CLOSE_NOT_OWNED` dropped; REPL `:close NAME` still added; the session name goes stale.                                 |
| Q3  | f     | A script ends while a detached capture is still running: wait, abandon-and-pin, or background-complete?                                                         | Wait (structured completion), bounded by the script timeout; the timeout path then abandons and pins as today. Background completion is refused.                                  |
| Q4  | h     | Grant granularity for destructive verbs                                                                                                                         | Per verb on the write root (`DIR[:verbs]`); default the present set. A script cannot grant, and an ungranted verb is `Rejected` before work starts.                               |
| Q5  | h, j  | Directories (`mkdir`, `list` in; `rmdir`, recursive removal out); line oversize policy (`Overflow`, cursor unmoved); who builds framing (VQL first, VPL reuses) | As stated: `mkdir` and `list` in, `rmdir` refused; `Overflow` retains rather than drains; VQL builds frames over a file first and VPL consumes them.                              |
| Q6  | k     | Process endpoints from VQL                                                                                                                                      | Not in M7-VQL: the adapter cannot stop its child and two hung children starve the two-worker pool. Reopen when M4's follow-up makes it killable. Net and in-memory stream are in. |
| Q7  | l     | A bound on loop iterations                                                                                                                                      | None: a count ceiling is a limit authors work around. The brakes are the script timeout, the verb grants, the request budget and per-iteration cleanup.                           |
| Q8  | g     | What a generation guard names                                                                                                                                   | A prior read's outcome (`if unchanged($r)`), never an implicit "last seen"; an adapter that cannot verify refuses the guard.                                                      |
| Q9  | k     | Spell the stable request in the same slice that cannot satisfy it                                                                                               | Yes: the ask is VQL's contract half, the answer is M5 follow-up's; the typed refusal is exercised from day one.                                                                   |
| Q10 | m     | Must the UX journeys pass at exit, or is the unit-level acceptance enough?                                                                                      | They must pass: AGENTS.md makes a step reachable only by flag or script a finding.                                                                                                |

## 8. M7-VQL complete

M7-VQL is complete when **all** of these hold at the commit that closes slice m:

1. **Every contract clause of §1 is "built"**, each with a test that fails when the clause breaks:
   the table's status column has no "absent" cell, and the two class-b dependencies (message mode, a
   stable adapter) are the only items it names as not reachable, with their owning milestone.
1. **Every class a item of §2.1 is closed, or re-classified in this document** with a dated ruling.
   Nothing is left as "deferred" without an owner.
1. **Every slice d..m passed its gate** with zero BLOCKING and zero MAJOR from every lane, reports
   under `m7-vql-<letter>/`.
1. **The M7a conformance harness passes unchanged**, and the shared effect contract of redesign §4
   (submit explicit, await never resubmits, render pure, typed outcomes, static multiplicity) is
   proved by tests that cover each of the new request kinds and the loop.
1. **No core file changed** except through a recorded, approved proposal (P10 at most), and
   `VortexCore` and the VM name no I/O schema and no VQL.
1. **Leak and bound pins**: 10^4 runs of each new action class bounded; 10^4 loop iterations
   bounded; no descriptor, request slot, step graph or capture run left after the runner dies, in
   any ending (finish, fail, abandon, timeout, destruction).
1. **No blocking and no implicit capture**: nothing on the evaluation thread touches a descriptor
   (audited), and a script that read anything persists only through a committed capture.
1. **`make check` is clean** (format, lint, tidy) and the full `xuzz_test`, `zigzag_test` and
   `gleditor_test` pass headless.
1. **The `ux-validation` journey of slice m passes** with no step reachable only by flag, script or
   file.
1. **M8 has its inputs**: the constants table exists and `vql-query-language.md` §4.13 describes one
   surface, not a sequence of slices.

What "complete" does not claim: that VQL can reach message endpoints, process endpoints, a
stable-snapshot backend or the network beyond a host-granted socket. Those are other milestones'
adapters (§2.2); the frontend is complete when it can ask for them and take a typed refusal.

## 9. Refused alternatives, collected

| Alternative                                                | Why refused                                                                                                                                   |
| ---------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| One large "M7-VQL-d" with everything                       | M7-VQL-c took four critics and three rounds for writes and capture alone; a slice mixing a loop and a framing schema cannot be gated honestly |
| Loops first, because they are the most asked-for           | every earlier slice's leak and multiplicity bug would hide behind the loop's own tests                                                        |
| Fail-driven (Choice/Fail) iteration for the loop           | `Fail` frees the iteration's cells and the P6 rollback handler orphans each admitted request; results would be host-held anyway               |
| A loop construct inside `for` (row traversal with effects) | it is exactly the incidental fan-out the redesign forbids; `each` is explicit and over materialised rows                                      |
| Defer the `stable` request until an adapter exists         | the first adapter would need a VQL change under another milestone's gate                                                                      |
| Expose `ProcessIoAdapter` with a human-granted command     | grant does not fix the unstoppable `popen` read; two hung children starve the pool                                                            |
| Refuse a script's close of a host-bound capability         | the draft recommendation; the owner ruled against it (2026-11-26, Q2): the script may close what it can name                                  |
| Reserve every new verb as a call name                      | breaks user `def`s (`capture` precedent); a contextual head is enough                                                                         |
| A core "iterate" opcode up front                           | measurement first (S-l); the capture loop already shows existing opcodes suffice                                                              |
| Add the settings entries here                              | M8 owns `system://settings`; this roadmap hands over the list                                                                                 |
| Unify the roadmap into `vortex-io-m7-vql-c.md` §19         | §19 is the record of one gated slice; a roadmap across slices needs its own page, so §19 is only cited                                        |

## 10. Change history

| version | date       | change                                                                                                                                                                             |
| ------- | ---------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 0.1     | 2026-11-26 | First draft: inventory of 23 in-scope items and 11 out-of-scope owners, slices d to m, ten owner questions, one conditional proposal                                               |
| 0.2     | 2026-11-26 | Owner decisions recorded: Q2 ruled yes (a script may close any capability it can name; `VQL_IO_CLOSE_NOT_OWNED` dropped; host-bound names go stale), the other nine as recommended |

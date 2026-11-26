# Round 1 critique: Xanadu/ZigZag Purist (M7-VQL-c code)

Target `dcce529b` (diff `4d132ccf..dcce529b`). Tally: 0 BLOCKING, 2 MAJOR, 7 MINOR.

Confirmed by running `build/vquery` headless (XDG redirected into the scratchpad). The binary was
built 06:30 on the same day as the target commit and was not rebuilt.

What held up, so the next round need not re-read it: the captured cells are one `makeCell(span)` per
piece chained on the receipt's `Range` rank (`capture_host.cpp:162-170`; rank degree 1 each way, and
the receipt's field rank is `Occurrence`, so no collision); `-o` transcludes (the result store's op
9 is `make cell span=[0,2)` into the capture's own bytes, no retyping); `--in-place` writes the span
itself (`vquery_e2e_test.cpp:931-955` checks `spanStart == 0`); the owned record is a detached
`Outcome` cell with field and `Progress` ranks and no `Request`/`Queue`/`Waiter`/`Clone`/`Lease`
rank (`owned_outcome.cpp:56-122`); record, receipt and refusal deletion go through
`ArenaManifold::deleteCell` leaf first (`owned_outcome.cpp:151-153`, `capture_host.cpp:206-244`).

## BLOCKING

None.

## MAJOR

### M1. A session variable naming a freed record or receipt aliases the cell that reuses the slot

**Problem.** `ScriptRunner` frees a finished script's owned records and capture receipts at the next
admission (`freeFinishedRecords`, `script_runner.cpp:1253-1262`; called from `captureBegin` at
`:857` and `openPath` at `:1050`; `admit` does the same). `as $r` and `capture(...) as $c` have
already published the cell into `VQLEngine::env_` as a bare `CellRef` (`vql_engine.cpp:154-175`),
with no incarnation, and nothing unbinds it when the cell is freed. The arena reuses freed slots, so
the variable then names an unrelated live cell. The readers (`liveCell`, `liveOutcome`,
`capturedCell`) only test "is this slot live and an Outcome/Receipt", which the reused slot passes.

**Failure scenario** (reproduced). REPL, two files `A` ("AAAA") and `B`:

```text
vql> <read A as $a; return io_tag($a)>          Complete
vql> <read B at offset 100 as $b; return io_tag($b)>   EndOfStream
vql> return io_tag($a);                          EndOfStream   <- $a was Complete
```

`$a` now reports script 2's outcome. The same holds for every field reader (`io_error`, `io_detail`,
`io_bytes`, `io_uncertain`), and `capture($a)` in a third script would capture whatever outcome
occupies the slot (the pre-check at `script_runner.cpp:1004-1010` only asks that `$a` is bound to
one cell). For receipts, `return io_captured($c1)` after a second capture ran failed with the
internal text
`RelationNext expects found, position and limit outputs plus one answer slot per argument`
(reproduced), where a typed stale-handle diagnostic is wanted; when the slot holds a receipt of the
later capture it would return that capture's bytes under the earlier name. Wrong value, silently: a
stale raw `CellRef` dereference.

**Evidence.** `script_runner.cpp:1253-1262` (free), `:857-858`, `:1050-1051` (frees on the next
admission), `vql_engine.cpp:154-175` (bindings are raw refs), `script_runner.cpp:1004-1010`
(cross-script `$name` accepted), `capture_host.cpp:206-244` (receipt freed by `release`). The
design's own text (§12.1: "a record outlives its script only until the next admission") describes
the free, but not what happens to the name.

**Fix.** When a finished record or capture is freed, also remove every session binding that names it
(`VQLEngine` already owns `setVariable`; add an erase-by-cell, or drop the binding where
`finishedRecords_`/`finishedCaptures_` are filled), or keep the binding's `(cell, incarnation)` and
refuse a mismatch with a stale-handle diagnostic (`tokenStale`). Add a REPL-sequence test: two named
reads in two scripts, then `io_tag($first)`.

### M2. Transient input is typed into the permascroll through a session variable

**Problem.** The coverage gate is evaluated only for the script that ran an action
(`main.cpp:823,848`: `resultsFromAction = ranAction`; `refuseDurableAction` returns false when it is
false, `:766`). A script's `let $t := io_payload($a)` publishes the payload text as a session
binding. A later plain query that returns `$t` is not an action script, so `:save`, `-o` and
`--in-place` accept it. `ScriptRunner::persistability()` is never consulted. This is the plan's hard
rule ("input is transient unless explicitly captured", §5 item 2: "reading never appends") and
design §4.7's soundness claim ("every byte the script saw as text is, in the author's permascroll
with provenance, because the author said so") broken from the REPL.

**Failure scenario** (reproduced; permascroll 137 bytes before, 200 after):

```text
vql> effect { read A } as $a; let $t := io_payload($a); return $t;     AAAA
vql> return $t;                                                         AAAA
vql> :save <path>                       Saved 1 result rows
```

`strings active.primedia` then shows `AAAA` appended after the capture-store names: the uncaptured
bytes of `A` are now permanent, with no capture receipt and no coverage check. The batch form (`-e`
of one script) is correctly refused; the REPL's two-line form is not.

**Evidence.** `apps/vquery/main.cpp:764-768, 823, 848`; `script_runner.cpp:1672-1699`
(`persistability_` is per most recent finished script and nothing ties it to later rows);
`vql_engine.cpp:154-175` (the binding outlives the script). Acceptance criteria in §16 test the
single-script cases (`PartialCoverageIsRefusedAndWritesNothing`,
`DerivedTextPersistsOnlyOnceThe SourceIsCovered`) only.

**Fix.** Track taint on the binding, not on the script: when an action script finishes with
uncovered exposures, remember that the session variables it published (and `let` bindings) came from
it, and refuse a later `-o`/`--in-place`/`:save` whose rows reach one, or simply keep the verdict
sticky until the session is cleared. At minimum `refuseDurableAction` must refuse when
`ioRunner->persistability()` is uncovered and any row cell is a session-bound cell of that run.

## MINOR

### m1. `owned_` is keyed by `CellRef` only; `insert_or_assign` drops a lease

`script_runner.cpp:1223` (`owned_.insert_or_assign(cell, ...)`), `script_runner.hpp:445`. The design
says the table is keyed `(cell, incarnation)` (§12.1). If a record's cell is rolled back by the VM
and the slot reused by a later record in the same activation, the old `OwnedOutcome` is destroyed by
the assignment without `releasePayload`, and `retainedBytes_` (`:1222`) stays charged for it
forever: the retention budget shrinks until the runner dies. Key by the pair, or `emplace` and
release on collision.

### m2. The read observation is copied into a native struct while the service keeps it as cells

`owned_outcome.hpp:35-42,44-48`. The service mints the observation as a `d.io.source` cell graph;
the record copies locator, generation and extent into C++ strings (`observation`,
`ownedObservation()` at `script_runner.hpp:265-276`), charges them to the byte budget, and `capture`
has two readers (`script_runner.cpp:860-863`). This is the side table the owner rule "solve with
cells and dimensions" refuses. A `Source` rank on the record (copied by value, freed by the same
leaf-first deletion) would remove the second reader and the extra accounting. Design §12.1 says the
record has no `Source` rank, so this is a design deviation to decide, not a defect.

### m3. Four hand-rolled "collect the reachable graph" walks, and the old reaper still unlinks

`owned_outcome.cpp:134-146`, `capture_host.cpp:206-215` and `:251-275` copy the same BFS that
`freeOwnedGraph` (`io_cells.cpp:244-292`) already has; only `freeOwnedGraph` honours `heldByViews`,
and it still uses `unlink` loops plus `freeCell` rather than P7's `deleteCell`, while the new
`reapIoEndpoint` (`io_cells.cpp:326-391`) extends it. The new code correctly deletes through
`deleteCell`, so no new unlink loop was added, but there are now two reclamation algorithms with
different view rules. Factor one `deleteReachable(arena, root, dims, mode)` in
`zigzag/cell_deletion` and call it from all five sites.

### m4. `--in-place` loses a multi-piece capture's order and mints an arena cell per row

`apps/vquery/main.cpp:945-952`. Each captured piece becomes an unconnected store cell; the `Range`
rank that ordered them in the arena is not carried into the store, so a selection captured over two
appends reads back as unrelated cells (order survives only as operation order), and
`arena.makeCell(content[0])` leaves one extra ephemeral cell per row until the arena dies. Promote
the pieces linked on a dimension (a `d.result` rank, as `writeResultSlice` does for `-o`), or
document that `--in-place` writes pieces, not a sequence.

### m5. The two persistence paths use different "is a captured cell" predicates

`--in-place` requires a `Range`-NEG neighbour (`main.cpp:945-948`); `resultRows` quotes any cell
with non-scratch content and no `quotedContent` against the capture store (`:152-166`). A non-local
span with no quoted store reaches `writeResultSlice`, which throws `invalid_argument` ("quotes a
scroll this store cannot name", `result_slice.cpp:19-26`), uncaught in `main`. Use the one predicate
(receipt-range membership) in both.

### m6. `reapIoEndpoint` and `freeOutcomeGraph` ignore a view of their root from another cell

`owned_outcome.cpp:129-157` frees a record or refusal without `heldByViews`; the contract is "has no
clone rank", enforced by comment only. A capture's `Request`-rank view of a `Rejected` outcome does
not exist today, but a later caller passing a service outcome that a capture clones would free the
master under a live view. Assert the rank is empty and refuse otherwise.

### m7. Stale-handle diagnostics are internal VM text

See M1's `RelationNext expects ...` message: `capturedCells` answers `nullopt` for a non-receipt
(`script_runner.cpp:947-958`) but the stale `$c1` took a different road to a VM error. Whatever fix
M1 takes, a stale `io_captured`/`io_tag` operand should be a `VQL_*`/`tokenStale` diagnostic.

VERDICT: NOT CONVERGED

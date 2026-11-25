# Round 1 critique: System Architect (Vortex I/O M1–M6 code)

Tally: 0 BLOCKING, 5 MAJOR, 5 MINOR. Target 757cd55b.

Lane checks that came back clean: no `apps/common/xanadu/vortex/` file in the diff includes
`apps/common/ui/`, `apps/xuzz/` or a device-needing library header (only `<gleditor/logging.hpp>`
and `<gleditor/cpp26.hpp>`). `src/` and `include/` are untouched by the diff. Tests sit in
`tests/xuzz/` beside engine code. Adapter hooks on `IoAdapter` (`abandonStaged`, `openSettled`,
`io_protocol.hpp:293,301`) are generic and do not leak a specific adapter.

## BLOCKING

None.

## MAJOR

### A1. Capture state is never reaped: committed slots and the journal exhaust permanently

**Problem.** `translations_` only gains Committed slots (`coalesceOrigin` merges only adjacent
same-origin slots). `CaptureJournal` is append-only with a byte budget and no compaction or
checkpoint. Both caps are hard refusals, and restart does not clear them: `recover()` rebuilds the
same slots from the same journal.

**Failure scenario.** Each new file generation or stream occurrence is a distinct `PayloadRef`
origin, so slots never coalesce across origins. After 8192 live slots, `prepare()` fails
`BudgetExceeded` forever (`capture_coordinator.cpp:862`). The same happens when the 4 MiB journal
fills (`capture_journal.cpp:599`). The plan states source chunks are not pinned by the map
(`design/vortex-io-implementation-plan.md:981`), so a Committed slot for an origin whose leases
have died can never be hit again. It is dead weight that is still counted. The user's only
workaround is deleting `capture.journal`, which loses the idempotence and orphan evidence the
journal exists for. This is the "ceiling users work around" the owner rejected.

**Evidence.** `capture_constants.hpp:23,32,38` (8192 slots, 4 MiB); `capture_coordinator.cpp:862`;
`capture_journal.cpp:599`; the only erasures are `capture_coordinator.cpp:624,738,1357,1782`, which
cover Pending, Pinned and coalesce, never retired Committed.

**Fix.** Reap Committed slots whose origin generation/occurrence is retired (the resolver knows),
and add a journal snapshot/compaction: a `Checkpoint` record that carries live translations,
written to a new file and renamed, with a journal version bump per the format rule. Until then,
document the exhaustion as a first-release refusal and expose a count in the report.

### A2. `commit()` blocks the owner thread on three fsyncs and a whole-store save

**Problem.** The plan's §14 and the standing constraint forbid any path that blocks the
evaluation/UI thread. The plan's deferral notes only that the whole-store save is costly
(`design/vortex-io-implementation-plan.md:981`), not that it runs on the owner thread.

**Failure scenario.** The owner thread calls `commit()`, which runs `syncSink()` (permascroll
fsync), `journal.appendRecord` for Appended (fsync), `Store::save` of the whole store, and a second
fsynced Committed record. On a large store, or a slow disk, the UI/eval thread stalls for the sum
of these, and the `save` term grows with the store. `appendRequest` also does a journal fsync on
the owner thread (`capture_coordinator.cpp:577` via `appendJournal`).

**Evidence.** `capture_coordinator.cpp:1204` (`syncSink`), `:1216` (Appended write), `:1232–1235`
(`publish` + `save`), `capture_publication.cpp:197–199` (`store_.save`).

**Fix.** Move the durable steps (permascroll sync, journal writes, save) into a worker-turn
adapter operation with the owner only minting cells, or record this as a named deviation from §14
with a bound. An incremental per-capture save needs a core change; raise it as a proposal (`P<k>`)
and do not assume it.

### A3. Two divergent copies of the journal machinery; the durable-effect journal has no integrity check

**Problem.** `writeAll`, `FdGuard`, the torn-tail scan/repair with ftruncate, the fsync/EINTR loop
and the append-with-rollback logic are written twice. A third `writeAll` exists in
`mesh_epoch.cpp`. Only `capture_journal` has a checksum (FNV-1a-64 per record,
`capture_journal.cpp:43–57,143,178`). `io_actor_journal` has none.

**Failure scenario.** A bit flip in a fully present `ActorIoJournal` record's `intentId` or `kind`
passes `journalPrefixEnd`, which checks only length arithmetic and `kind <= Checkpoint`
(`io_actor_journal.cpp:96–115`). A Receipt for intent 7 can then read as a receipt for intent 9.
Recovery concludes that a durable real-world effect was done (or not done), and "never resends"
is decided on corrupt data. The two copies have already diverged, so every future fix must be made
twice.

**Evidence.** `io_actor_journal.cpp:53–66,100–107,160–330` vs `capture_journal.cpp:282–310,474–730`;
`mesh_epoch.cpp` (`writeAll`).

**Fix.** Extract one engine-level framed-append-log helper (open/recover tail/append+fsync+rollback,
with the checksum) used by both journals, and bump `kActorJournalVersion` to add the checksum.

### A4. Sentinels in the API, one of which disables a bound

**Problem.** `StagedLensSourceAdapter::stage()` and the stream `stage()` return `0` for "table
full", with `key == 0` skipped when minting. `ActorIoJournal(path, maxBytes)`, `CaptureJournal` and
`CaptureCoordinator::journalMaxBytes` treat `0` as "unbounded".

**Failure scenario.** A caller that forgets to compare against 0 stages a spec under key 0, or
looks it up and gets a silent miss. A mistyped or default-zero `maxBytes` removes the journal bound
entirely, contradicting "memory is bounded; overflow explicit". Both are sentinels outside any wire
format.

**Evidence.** `lens_adapters.hpp:102`, `lens_adapters.cpp:59–62`; `source_adapters.hpp:71`,
`source_adapters.cpp:64–66`; `io_actor_journal.cpp:238`; `capture_journal.cpp:599`;
`capture_coordinator.hpp` doc "0 = unbounded".

**Fix.** `stage()` returns `std::expected<std::uint32_t, IoErrorInfo>`; drop the 0 convention and
require a positive `maxBytes` (`std::optional` if unbounded is ever legitimate).

### A5. `ActorIoJournal` documents a compaction that does not exist and refuses permanently when full

**Problem.** The header says checkpointed records "may retire (be dropped on the next compaction)"
(`io_actor_journal.hpp:140–142`). No compaction exists. `checkpoint()` only appends.

**Failure scenario.** Every intent costs at least admission + receipt + checkpoint records. When
`writtenBytes_ + framed > maxBytes_`, `reserve()` returns `BudgetExceeded` for every later
`recordAdmission` (`io_actor_journal.cpp:238`). A long-running durable-effect actor can never
submit again, with the retirement the header promises unreachable. This is a deferral described
falsely, plus the same permanent-exhaustion shape as A1.

**Evidence.** `io_actor_journal.hpp:140–142`; `io_actor_journal.cpp:238,330`; no compaction path
anywhere in the file.

**Fix.** Implement compaction (rewrite to a temp file with the live suffix after the latest
checkpoint, fsync, rename, fsync the directory) or correct the header and the plan to say the
journal is single-lifetime and refuses at the bound.

## MINOR

1. **Scattered naked limits, no path to `system://settings`.** Local `4096` caps are repeated in
   `lens_adapters.cpp:16`, `source_adapters.cpp:18`, `file_source_adapter.cpp:22`,
   `capture_coordinator.hpp:190`. Others are `64 MiB` (`file_source_adapter.cpp:30`), 4096
   observations and interned generations (`file_source_adapter.hpp:318,327`), 256 decode leases
   (`:333`) and the function-local 1024 tombstones (`io_service.cpp:611`). `capture_constants.hpp`
   covers capture only, and `IoBudgetSnapshot` has no defaults. M8 needs one `IoLimits`-style
   header with rationale per value.
1. **`closingEndpoints_` eviction is by key, not age.** `erase(begin())` removes the lowest
   `CellRef` (`io_service.cpp:613–615`). With 1025 entries and a recycled low CellRef, the entry
   just inserted for an in-flight close can be the one erased. A second `closeEndpoint` then
   passes the Conflict pre-check while the first is still active. Use insertion-ordered eviction
   and never evict an in-flight entry.
1. **Four copies of the staging table.** `lens_adapters.cpp:59–84`, `source_adapters.cpp:64–83`,
   `file_source_adapter.cpp:130–135` and `capture_coordinator.cpp:290` each re-implement
   stage/stageAt/abandon with a cap. Promote one `StagingTable<Spec>` template.
1. **The translation map is a second source of truth.** Committed translations are published as
   Structure (receipt, `d.io.range`, provenance chains) yet `translations_` is rebuilt from the
   journal, not from the Store. The two can disagree after a lost Store (the code already needs
   `PublicationIncomplete`). The `std::map` side table also sits against the "cells and dimensions
   over side tables" rule. Justify it as a cache of the published ranks or derive it from them.
1. **File read observations are evicted at 4096** (`file_source_adapter.cpp:535–536`), so a
   capture of an older read becomes `Unresolved` with no reaping signal. This is a ceiling with no
   settings knob, and the plan's deferral list does not mention it.

VERDICT: NOT CONVERGED

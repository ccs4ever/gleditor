# Round 2 critique: System Architect (Vortex I/O M1-M6 code)

Target dbe8e40c. Tally: 0 BLOCKING, 1 MAJOR (R2-1), 1 MINOR. A2 is OPEN (P3 approved) and not
counted.

## Round-1 MAJOR status

- A1 (capture state never reaped): PARTLY FIXED. Committed slots of unreachable transient origins
  and retired captures' journal records are now reaped (`capture_coordinator.cpp:583`, `:605`; test
  `CaptureStateIsReapedSoALongRunPastTheOldCeilingsWorks`). The durable-origin residue is R2-1
  below.
- A2 (`commit()` blocks owner): OPEN, P3 approved, pending. Recorded as a named deviation in the
  plan; not counted against convergence.
- A3 (duplicated journal machinery, no integrity check): FIXED. `framed_log.{hpp,cpp}` holds
  framing, FNV checksum, scan, tail repair, append/rollback, directory sync, `replaceRecords`; both
  journals use it and `io_actor_journal` is v2 with checksum. No `writeAll`/`fsync`/`ftruncate`
  remains in either journal (grep). The only other `writeAll` is `mesh_epoch.cpp:50`, a text
  hash-chain record file untouched by this diff and outside M1-M6; not a finding.
- A4 (sentinels): FIXED. `std::optional` stage keys and byte budgets; 0 holds nothing
  (`ABoundOfZeroHoldsNothingAndNulloptIsUnbounded`).
- A5 (actor journal compaction false/permanent refuse): FIXED. `compact()` implemented and tested;
  full now means full of unreleased records.

## Lane checks (clean)

- Layering: `framed_log.hpp` includes only std, `<gleditor/cpp26.hpp>` and `io_error.hpp`;
  `framed_log.cpp` adds `<gleditor/logging.hpp>`. No `apps/common/ui/`, `apps/xuzz/` or device
  header anywhere in the diff. The engine has no UI dependency.
- DRY: both journals share one layer; no second copy remains in the diff.
- Reaping of IoService/io_cells: closing tombstones evict oldest-settled only
  (`io_service.cpp:20,631-637`), retired requests are reaped from the rank walks
  (`io_cells.cpp:216`), resolver sources are weak with amortised sweep. No unbounded path found.
- Configuration: the capture bounds are named in `capture_constants.hpp` with rationale and an M8
  pointer; consistent with the plan's recorded deferral.

## MAJOR

### R2-1. Durable-origin capture state is never retired: a long session hits a permanent ceiling

**Problem.** `reapUnreachableSlots` skips every durable origin because `sourceReachable` returns
`true` for `PayloadSourceKind::Durable` unconditionally (`payload_resolver.cpp:441`), so Committed
durable slots are never erased. `compactJournal` keeps every capture whose Appended record has a
Durable interval (`capture_coordinator.cpp:626-631,640-644`). The plan states this as "the one thing
never retired" and relies on the journal bound to refuse
(`design/vortex-io-implementation-plan.md:1027-1029`). Durable slots coalesce only when adjacent, so
distinct ranges accumulate.

**Failure scenario.** A long-running session (or a script) captures many distinct, non-adjacent
ranges of durable text (per-line or per-quotation capture of a large document). After 8192 such
slots `prepare()` takes the `BudgetExceeded` path at `capture_coordinator.cpp:959-966` and
`reapUnreachableSlots` frees nothing. At roughly 4 MiB of retained durable-capture journal,
`journal_.hold` fails and `compactJournal` returns `BudgetExceeded` with detail 5 ("nothing
retires", `:660-664`). Every later capture refuses, restart rebuilds the same state from the same
journal, and the only workaround is deleting `capture.journal`, which discards the idempotence and
orphan evidence. This is the same ceiling shape A1 named, now confined to durable origins. Dropping
such a translation is safe by the plan's own argument: the map is a dedup cache and "a lost cache
costs a repeated append, never a wrong reference" (plan `:1020`).

**Evidence.** `payload_resolver.cpp:441`;
`capture_coordinator.cpp:583-603,626-644,959-966, 994-1003`; `capture_constants.hpp:34,40`; plan
`:1027-1029`.

**Fix.** Bound durable retention by recency instead of refusing: when reap and compaction free
nothing, evict the least-recently-hit committed durable translations (drop their whole captures'
journal records in the same compaction, since the journal record and slot must agree), counting
evictions in the report. Alternatively record the refusal as a user-visible, named first-release
limit with a count in the coordinator report, but a refusal that only a file delete clears is the
workaround the owner rejected.

## MINOR

1. `compactJournal` returns `BudgetExceeded` detail 5 when nothing retires, which `prepare()` then
   reports as the journal being full; the report gives no count of retained durable captures, so a
   user cannot tell which bound is binding. Expose live slot and retained-capture counts.

VERDICT: NOT CONVERGED

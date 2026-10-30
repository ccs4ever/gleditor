# I3-1b/1c review, round 3: vagent compiler and codebase persona

Scope: review head `08425c60` (round-2 fix commit; diff `d48e85f8..08425c60`: compiler.cpp,
agent_executor.{hpp,cpp}, epistemic_ingress.hpp, runtime_abi.hpp, ingress tests), against i3-design
section 14 and the owner's decision on open question 1. Method: read the diff and the new advance
IR; re-ran the whole `vagent_actor_test` (697 tests pass, 14 s; the binary and `compiler.o` are
newer than the sources); rebuilt my round-2 probes in /tmp/i3-1bc-codebase (`probe2.cpp`) against
the current objects. No repository file was touched.

## Probe results

- **Round-2 MAJOR, the wedge (probe `Wedge`):** one principal, 4 new keys per boot, 160 boots,
  default limits, a second honest principal bound throughout. Epistemic cells: 1,571 at boot 40,
  1,571 at boot 80, 1,491 at boot 120, 1,571 at boot 160 (round 2: 2,911 at boot 10 and the
  24,576 ceiling at boot 86). One ingestion takes 3.5 to 3.8 ms at boot 2, 40, 80, 110 and 160
  (round 2: 4.2 ms rising to 42 ms). VM cycles for any turn peak at 12,500 (an advance turn); the
  ingestion turns sit at 11.8k, which is the same as the small-state figure in this build, not the
  80k of round 2. No refusal of either principal anywhere in the run. **Fixed.**
- **Slots and cycles per ingestion:** unchanged from round 2 (242 slots, about 4.2k cycles on a
  small state); the 11.8k now seen includes the settle of the program's handlers in this probe.
- **noCell and sentinels:** `git diff -U0 d48e85f8..08425c60 -- apps tests` adds none. The new
  absences are `optional` or the VM's own 0 from `rd`/`rtFind` (allowed).

## BLOCKING

None.

## MAJOR

None.

## Review of the new code

- **Advance IR (compiler.cpp ~4680-4750).** Pass 1 retires the predecessor's current records as
  before. Pass 2 walks the profile's clone ring: for each record, `head(rec, d.epistemic)` is its
  support; `rd(sup, d.source, neg)` is zero for a head that is not a support (the profile after a
  move, or an already dropped support), which skips it, so a support whose chain moved on the first
  of its records is not processed again for the others. For a live support it detaches the whole
  chain (`breakOp(sup, epistemic)`), appends it to the profile's own `d.epistemic` tail, then
  drops the support through `rtBDrop` with the same argument order as the retire path, then
  `rtSettle_` raises `-pred` once at the end. Every register is a reg of the turn, `rec` and `v`
  are reused after pass 1 finished with them, and the chain is detached before `rtBDrop`, so the
  drop cannot see the records. I found no label or register defect. The 160-boot probe and
  `ReplacementBootsWithNewKeysStayBounded` exercise it.
- **Trim.** Counts the profile chain, and while `n > retained` rescans from the head for the first
  unpinned record (pin absent counts as "keep", a defensive reading), unlinks its clone view and
  itself. Terminates (each pass removes one or ends at the first scan miss). The chain holds at
  most `retained` plus one boot's incoming records (at most `keys_per_source * history`), so the
  rescan is short. Order across supports is by support, not by acquisition time, so "oldest first"
  is approximate when one boot held several keys; harmless for a bound, worth a word in section 14
  if I4 will read the chain as a time series.
- **Cost of the move.** `-pred` handlers run once per dropped support inside the advance turn; each
  activation allocates its frames, which is the known compiler-wide leak (about 85 slots), now also
  paid by advances (up to `keys_per_source` of them per boot). Bounded, but it adds to the slot
  clock of a rebooting device: roughly 340 slots per boot on top of the ingestions.
- **`epistemicCellsWithin`.** Now also admits each profile's retired chain, so admission counts what
  the routine keeps; the why-not-Vortex comment (host admission between halt and commit, bounded by
  the ceiling) is present and adequate. The walk is O(cells) per committed ingestion, 24k cells at
  worst, now only reachable by legitimately many sources, no longer by one rebooting device.
- **`readAt` guard.** `branchIfZero(f, malformed)` inside the advance loop; a short record ends the
  turn `IngestionFault`, never reads cell 0. Correct. (`recordOf` in the source-change IR still has
  no zero guard, MINOR 2.)
- **Field-order check in `observe`.** Compares `r.names[i]` with `kObservationOrder[i]` for all 23
  entries on every observation, after the record is built. Cheap and loud, and there is no
  out-of-range read because all 23 names are set unconditionally just above. Two quibbles in MINOR 1.
- **`pushControl`.** Inserts behind the source changes already waiting and ahead of everything
  else, under the post box mutex; both callers hold the ingress mutex, and `observe` pushes under
  the same mutex, so a bind and the observations it authorizes cannot swap. `putBack` (the
  drain's kept items) still inserts at the front, so a kept change stays ahead of one pushed while
  the drain ran. In `drainPosted` the same rule orders the host events. The test
  `SourceChangesInOnePumpRunInCallOrder` covers retire-then-bind. Correct.
- **Ingress changes.** `release` erases a counter that reaches zero and ignores an absent key, so a
  late release after `forget` is harmless; `observe` reads counters through a non-inserting lookup
  and only creates entries when it queues, so a refused observation leaves no entry. The retire floor
  is created only when a binding or a floor is known (the sentinel 0 default is gone). A revive refused
  by the budget now revokes its bindings without raising the floor (right: the arena's tombstone keeps
  its own), so rebinding the same incarnation after the host makes room works. The new refusal rule
  (`retry` when the held entry is not `Refused`, or is `Refused` with the same code; log only if absent
  or `Refused`) keeps a queued, committed or duplicate entry intact and charges a repeated identical
  refusal once. `UnknownActor` is checked before anything is stored.
- **Resume.** `restoreState` copies the profile's `d.epistemic` chain with the shared map, so the
  retired history and its clone views come across once.

## MINOR

1. **Smaller points on the order check and the sentinel-like defaults.** (a) A mismatch is a build
   defect, yet it refuses `InvalidObservation`, which `Ingress::chargeable` charges to the source.
   Refuse with a host-class code (it already has `IngestionFault` for exactly this) so a code
   defect cannot lower a device's standing. (b) The pin is still a runtime check; an array built
   from `kObservationOrder` or a unit test would pin it at build. (c) Carried from round 2:
   `IngestOutcome::code` and `Ingress::Charge::code` are "empty means none" strings (a charge
   always has a code, so make it required); `FrameBody` fields default to cell 0 (compiler.hpp:70-76);
   `ArbitrationPolicy::maxUncertaintyMs{}` defaults to 0.
2. **`recordOf` has no zero guard** (compiler.cpp, emitSourceChange). A clone view whose record is
   gone (a pinned record of a dropped support, once I3-4 sets pins) walks from cell 0 in the retire
   and advance passes. Unreachable today; add `branchIfZero(name, next)` before I3-4.
3. **No test resumes after an advance that left a retired chain.** The resume tests cover profiles,
   histories, tombstones and the floor, and `restoreState` now copies the profile's own
   `d.epistemic`, but none checkpoints between an advance and the next ingestion. A short test
   (advance, checkpoint, resume, then a retire turn and a count of cells) would pin the new chain.
4. **`catch (const std::exception &)` still maps every exception to fuel** (recorded in section 14
   as predating the slice; it can still send a host to grow a grant for a logic error).
5. **Design/code on exhaustion** (section 14, line 643: "no retire, advance or bookkeeping turn is
   attempted") is still not what `deliverHost` does: only ingestions are gated. The code is the
   better behaviour; amend the sentence.
6. **Setters returning `void`** (`forgetIngress`, `postRefusals`, `Ingress::log/forget/release/
   unbind`) and the **stats pollution** by `refused_Duplicate`/`refused_NoProfile` are unchanged.
7. **Fuzzer** still launches one actor per input (known; not re-raised).

## Round-2 findings, status

| Round 2 | Status |
| ------- | ------ |
| MAJOR 1, wedge from dead supports | fixed (probe above) |
| 1 sentinel defaults | partly: the floor default is gone; the rest in MINOR 1c |
| 2 two unpinned orders / `readAt` guard | fixed in runtime (guard, order check); build-time pin remains (MINOR 1b) |
| 3 exception to fuel | recorded, open (MINOR 4) |
| 4 refused revive leaves binding | fixed |
| 5 why-not-Vortex comment | fixed |
| 6 `max_uncertainty` default | recorded in section 14 |
| 7 design vs code on exhaustion | open (MINOR 5) |
| 8 `recordOf` guard | open (MINOR 2) |
| 9 floor map growth | fixed |
| 10 void setters, 11 stats | open (MINOR 6) |
| 12 fuzzer | known |

Verdict: PASS

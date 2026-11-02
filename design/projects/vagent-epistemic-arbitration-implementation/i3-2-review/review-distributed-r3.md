# I3-2 design review, round 3: distributed-systems expert (persona distributed)

Scope: `i3-2-design.md` revision 3 (uncommitted, 1,136 lines), `i3-2-review/findings-r2.md`,
`decisions.md` sections 1 and 3, and my round-2 report `review-distributed-r2.md`. Checked against
P19 v1.5 sections 2.1, 4, 5.1 and 8.2 and against the code at HEAD `8a4089d9` plus the uncommitted
T0 change. Line numbers are the working tree's (`agent_executor.cpp:N`, `compiler.cpp:N`), which T0
shifts by about 60 lines from the revision's HEAD numbers. I read the code and ran no new
experiment; the arithmetic is in `/tmp/p19-distributed/calc3.py`. Nothing in the repository was
edited except this file.

## Earlier findings

My round-2 findings were M1-M8 and m1-m9. The lead's rulings on them are in `decisions.md` (F9, F17,
F20, F23 and F28 among them). Revision 3 resolves most of them in the text; M4 is resolved in shape
but its mechanism has a new defect (M1 below).

| Finding                                     | Source F# | Status    | Where and why                                                                                                                                                                                                                                                                                                                 |
| ------------------------------------------- | --------- | --------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| M1 discard drops the whole batch            | F9        | Resolved  | 4.1: one settle turn per top-level event; a failing event is discarded alone; `SettleReport` names it; `stat()` counts it. Residue: m9.                                                                                                                                                                                       |
| M2 settle and discard must run inline       | F10       | Resolved  | 4.1: inline in `deliverHost`, gate with `slotReserve`, executor-side discard, one publication after the loop, `CheckpointRefusal::SettlePending`. The unlink is right: `d.pending` is one chain on `dims.pending` (`compiler.cpp:1809`, `5691-5702`) and `ArenaManifold::link` evicts the old end (`arena_manifold.hpp:625`). |
| M3 reset epoch bumped at call time          | F19       | Resolved  | 5.4: `ResetMark {requested, committed}`, stamped under the ingress mutex with the push, set at commit, checked in `drainPosted` and again in `deliverHost`. Equivalent to the host sequence number I proposed. Residue: m8. New interaction with the wake cap: M2.                                                            |
| M4 superseded reading returns on retransmit | F20       | Open      | 6.4 writes a stream sequence floor in the arena, as I asked, so the cut survives restart and resume. The floor's source is unsound: M1.                                                                                                                                                                                       |
| M5 health actions not idempotent            | F17       | Resolved  | 5.3, 6.3: `expectResetEpoch`, `NoChange` without a turn, ticket (instance, counter), tri-state outcome, pending map never evicted, `SettleFailed` reported as a committed change. Residue: m6, m4.                                                                                                                            |
| M6 "one predicate" and the other sites      | F7, F25   | Resolved  | 5.4 site table, `isControl` and `owesAccounting`, `chargeable` exclusions, explicit `outcomeClassOf`. I checked each named site against the code.                                                                                                                                                                             |
| M7 host latch mirror                        | F23       | Resolved  | Lead ruling A (no mirror). 6.5 and 10 state what it costs: one turn per flooded reading, a quiescence block until the flooder is retired or unbound. The arena check is first in the turn. Residue: m14.                                                                                                                      |
| M8 grant authority not rechecked            | F14, F15  | Resolved  | 5.2, 5.3: bind-time refusal, turn-time recheck by token digest in `deliverHost`, `revokeHealthGrant` returns a count, linearisation point written. Residue: m3, m11, m12.                                                                                                                                                     |
| m1 fairness of control items                | F16       | Part open | The health cap exists (`controlPerWake`); source changes stay uncapped and the cap counts health turns, not settle turns: m1 below and M2.                                                                                                                                                                                    |
| m2 quiescence                               | m23       | Resolved  | 6.5 says a flooder blocks checkpoint and upgrade until retired or unbound.                                                                                                                                                                                                                                                    |
| m3 steady clock across suspend              | m24       | Resolved  | 5.2 records it; the lifetime is bounded at 24 hours.                                                                                                                                                                                                                                                                          |
| m4 durability of a safety action            | F24, m25  | Resolved  | 6.7 table, `checkpointed`, `NoChange` replay, resume fencing recorded as out of scope (decision). Residue: m13.                                                                                                                                                                                                               |
| m5 token source                             | F13       | Resolved  | 5.2: `RAND_bytes`, fail closed, digest and `CRYPTO_memcmp`.                                                                                                                                                                                                                                                                   |
| m6 wedge on the ingestion path              | F11       | Decided   | A4, accepted residual. The wording of A4 is inaccurate: m10.                                                                                                                                                                                                                                                                  |
| m7 audit of refused actions                 | m26       | Resolved  | 6.5: refusals reach the sink; `actions` holds accepted actions. Residue: m5.                                                                                                                                                                                                                                                  |
| m8 extra turns                              | m15       | Resolved  | 4.1 notes `$turn_count`.                                                                                                                                                                                                                                                                                                      |
| m9 epoch maps in `forget`                   | m16       | Resolved  | 5.4 erases `ResetMark` and `admittedSeq` in `forget`, not `unbind`; the cost is host memory growth: m11.                                                                                                                                                                                                                      |

## What is sound

- The slice split is the right one for my discipline. 2b removes the wedge before any action can
  need it. 2c1 puts the whole host surface (grants, ordering, rechecks, receipts, bounds) behind a
  stub that changes no state, so the authorization code is reviewable before it can hurt anything.
- The settle loop terminates for the reason given: each iteration removes one top-level event,
  settled or discarded, and only a settle turn adds events, which that turn consumes or undoes.
  Running it inline keeps "before any other item for the actor" true without a queue position.
- The discard cannot depend on the budget that just failed. Unlinking the head from the same chain
  that holds the queue is a pure arena edit with no cell minted.
- The generation scheme for reset is transactional by construction: nothing is superseded until a
  reset commits, a failed or revoked reset supersedes nothing, and the stamp and the push share a
  critical section.
- The site table in 5.4 names every place that tests an item's kind. The deny-list trap in
  `chargeable` (`agent_executor.cpp:902-910`) is closed with named exclusions.
- Receipts carry the executor's instance, pending items sit in their own map and are never evicted,
  and `healthOutcome` has three answers instead of one `nullopt`.
- A reserved lane for isolate and reset inside a bounded pending queue, with a per-grant limit, is
  the correct shape against a degrade-only holder.

## BLOCKING

None.

## MAJOR

### M1. The reset floor comes from every passing reading, not from the readings in flight

Problem. 5.4 and 6.4 write each stream's sequence floor from `Ingress::admittedSeq`, "raised in
`observe` for every reading that passes its checks". Those checks accept any Int64 sequence
(`agent_executor.cpp:3630-3640`; the stream order is decided later, in the arena). A reading that
passes `observe` and is then refused by the arena (`ClockEpochRegression`, a mapping regression,
`ObservationTimeMismatch`) still raised the mark, and the refused turn is undone, so the stream's
`lastSeq` does not move (`compiler.cpp:5361-5376`; the stream is written at commit only, `:5389`).
The floor then holds a sequence the arena never accepted.

Evidence and scenario. A device with a corrupted counter sends readings whose sequence is huge and
whose clock epoch has regressed. Each is a health refusal, `f` rises, the source reads SUSPECT, and
the operator resets it, which is the remedy P19 5.1 names. The reset writes `floors[predicate]` =
the huge value. Every honest reading afterwards has a lower sequence and refuses `StaleSequence`
(Terminal, and not a health refusal, so the source reads NOMINAL while delivering nothing). A reset
keeps stream floors by design (P19 2.1; 6.4), so it cannot undo this. The only exit is a retire
(`compiler.cpp:5138` clears the streams) and a rebind at a higher incarnation, which A3 says some
devices cannot sign; the retire also drops the dedup window.

Consequence. The remedy for a faulting source can lock the source out. The map also grows without
bound while it survives `unbind` (m11).

Required fix. The floor must cover only what the cut is for: readings queued or running for the
principal at the call. Committed readings are already in the stream, and a reading the arena refused
refuses the same way again. Track, under the ingress mutex, a bounded per-(principal, predicate)
multiset of in-flight sequences (at most `ingressQueuePerSource`), inserted when `observe` queues
the reading and removed in `finishIngest`, `abandon`, `dropHostEvents` and `orderlyStop`. The reset
call snapshots its maximum. Delete `admittedSeq` from the survives-unbind list. `wouldChange(Reset)`
(6.3) must return "would change" while any reading is in flight, or a reset meant to cut the queue
is skipped as `NoChange`. Tests: a refused reading with a huge sequence, then a reset, then an
honest reading commits; the floor equals the in-flight maximum; the multiset is empty at idle; a
reset of a clean profile with a reading queued supersedes it.

### M2. The control cap, the interleave claim and post-call survival cannot all hold

Problem. 5.3 says the cap on health turns defers the rest "at the front", and that "a batch of 100
isolates takes seven wakes, and an ingestion wake runs between them". The 2c1 test table repeats it
("a 100-source batch over several wakes with an ingestion between"). 5.4 and the 2c2 test say a
reading queued after a reset call is not superseded.

Evidence. `settle` (`agent_executor.cpp:1989-2005`) walks one ordered batch. A deferral inserts
`batch[k..end)` at the front and `break`s; the ingestions sit behind the deferred controls, so the
next wake runs the controls first again. The outer round loop would also continue within the same
call unless the cap breaks it as `ingested == ingestPerWake` does. Ingestions therefore do not run
between the wakes, and the stated test cannot pass. If the implementation is changed so that they do
(run up to `ingestPerWake` ingestions ahead of the deferred controls), a reading queued after the
reset call can reach the arena before the reset turn and be dropped by it, which breaks the survival
test. The two promises exclude each other.

The cap also meters the wrong quantity. At the defaults a change is `1 + 4P = 13` turns (P = 3), and
"each started change finishes its settle loop", so a capped wake is 16 x 13 = 208 turns (0.89
million cycles at 4.3k each). If every settle turn burns the default 10 million instruction grant,
that is 1.92 billion instructions in one wake, which the cap does not bound. Source changes
(`bindSource`, `retireSource`) are not capped at all.

Consequence. A tested promise is unimplementable as written, and the fairness mechanism does not
bound the work that matters.

Required fix. Decide, and write the rule into 5.3 and 5.4. Recommended: strict control priority. The
cap yields to other actors only (`pumpUntilIdle` already round-robins the actors), the cap breaks
the round loop, and the interleave sentence and test are deleted. Count every turn of a change,
settle turns included, toward `controlPerWake`, checked before a change starts, and apply it to
source changes as well. The alternative (interleave) needs the survival guarantee weakened to
"queued before the call never survives" and its test changed to match. Tests: a cap trip defers the
ingest tail with the controls; a 100-isolate batch finishes in the number of wakes the budget
implies; a reading queued after a reset call that is deferred past a wake is (or is not) dropped, as
decided.

### M3. Close-on-accept and the generation key split one charge with no note identity

Problem. 3.2 closes a principal's open `Charge` when `observe` queues an accept, "so the next
refusal opens a new charge and a note queued after the accept". 5.4 keys `charges` by (actor,
principal, generation). A principal can then have several open charges at once, but the code holds
one entry per (pid, principal) (`agent_executor.cpp:764`), `charge()` is `try_emplace`, and the note
finds its charge by that key and erases it on conversion (`:3937-3940`, `abandon` at `:3863`).

Evidence. Two natural implementations both fail. Erasing the open charge when an accept is queued
leaves the already-posted note nothing to find: `if (found == end) continue` (`:3938`) drops its
counts, so refusals are lost. Leaving the entry open merges the next refusal into it, so the "keep
queue order exactly" claim and the ML gate's order test (refusal, queued accept, refusal) fail.

Consequence. The order guarantee that decision F2 rests on has no mechanism, and the natural
mechanisms lose counts or break the order.

Required fix. Give a charge an identity: key by (pid, principal, generation, serial), and let the
note carry (generation, serial). Or move the counts into the note item at close and keep the map for
the open charge only. State the bound: one open charge per queued accept plus one, at most
`ingressQueuePerSource + 1` = 9 per principal at the defaults. Say what `abandon`, `dropHostEvents`
and a superseded note do with a closed charge. Tests: the three orderings, a closed charge whose
note is abandoned, and a closed charge superseded by a reset.

### M4. A relay can replay old refused events past the outcome log and count them again

Problem. 3.2 counts a refusal once per (incarnation, event ID) through `Logged::counted`, so that "a
hostile relay could not make its victim SUSPECT" (3.1). `Ingress::log` keeps `outcome_log` entries
per source (64) and drops the oldest (`agent_executor.cpp:782-794`). A refused event leaves nothing
in the arena (the dedup window of 16 holds committed events, and host-decided refusals never reach
the arena), so once its log entry is evicted the same signed event counts again. The design calls
this "the bound on the error".

Evidence. That bound is right for accident and wrong for the adversary the design names. A relay
holding at least 65 (= 64 + 1) distinct signed-bad events of the victim can replay them in rotation:
each replay finds its log entry gone and counts. `f` stays above the threshold for as long as the
relay keeps replaying (calculated in `calc3.py`).

Consequence. The defence stops at 64 events. The SUSPECT state, and in I4 the inflation keyed on it,
can be held by a third party.

Required fix. Make "counted" independent of the evictable log. Option A: keep, per (source,
incarnation, predicate), the highest sequence counted or committed, and count a signed-basis refusal
only above it. A reordered bad event below it is under-counted, which errs toward trust and is
bounded by the queue. Option B: a separately sized ring of counted event IDs. Option C: record the
limit as accepted with the attack named, as an owner decision. Test: replay of 65 or more distinct
old bad events after the first pass moves neither `R` nor `N`.

## MINOR

- m1. Source changes (`bindSource`, `retireSource`) are still uncapped control items, and each is
  now `1 + n` turns (my round-2 m1, part open). Count them toward `controlPerWake` (see M2).
- m2. State each new method's thread contract. `observe` is any-thread, `bindSource` is "from the
  thread that pumps" (`agent_executor.hpp:343-352`). `Reset` requires `expectResetEpoch`, which only
  `sourceHealth` supplies, and `sourceHealth` is pump-thread only (3.5). A network operator thread
  cannot form a valid reset. Either say all five methods and the reads are pump-thread, or publish a
  mutex-guarded `SourceHealth` snapshot refreshed at the end of each health, ingestion and
  bookkeeping turn.
- m3. State the lock order for the reset critical section (Ingress mutex, then `PostBox` mutex,
  which `observe` already follows) and define what `revokeHealthGrant` counts. Items already in
  `hostEvents` are owner-thread data, so the count can only be pending-map entries not yet started;
  say that it is a lower bound and that a turn past its check completes.
- m4. A batch is one pending slot and many principals. Say that it expands into per-principal
  `HostEvent`s at drain, that the slot is released by the last member to finish, that
  `dropHostEvents` and `orderlyStop` finish the members that did not run, and what `healthOutcome`
  shows while it is part-done (`Pending` until the last member).
- m5. The audit sink runs inline on the pump thread. State that it is called with no executor lock
  held and may not call a mutating method; that a slow sink delays every actor; that a failure is
  counted (a `SourceHealth` field and a `stat` counter), not only logged, because an ignored failure
  leaves a committed isolate with no durable record; and that delivery is at least once, keyed by
  (instance, `action_seq`), since a resume can replay.
- m6. `Clear {}` is not idempotent: a retry after another operator added a reason removes it. Either
  carry `expect_action_seq` or list the exception beside the table of idempotent kinds.
- m7. 6.4 creates a stream where the predicate has none: give its clock epoch and mapping version
  (the minimum, 0 and 0, so no later epoch is refused). Add that `retire` clears the streams
  (`compiler.cpp:5138`), so the cut does not survive retire and rebind.
- m8. A note already converted to a `HostEvent` has left `charges`, so a reset that supersedes it
  does not add its counts to `dropped_content` and `dropped_other` (m17 in `findings-r2.md`). Scan
  `hostEvents` at the reset's turn on the owner thread, or accept the lifetime-counter undercount
  and say so.
- m9. A correct handler on the same event as a failing one loses that event (one turn per top-level
  event). 4.1 says siblings keep theirs, which is true across predicates only. State it and test two
  handlers on one predicate.
- m10. A4 and 4.1 say "an operator isolate or reset unwedges" an ingestion wedge. An isolate latches
  the source and refuses its readings, so the key cannot update; a reset, a retire or an advance
  drops the supports and lets it. A4 is a proposed spec amendment, so the sentence must be right.
- m11. `Ingress::everBound`, `admittedSeq` and `ResetMark` survive `unbind` and are erased with the
  actor, so they grow with every distinct principal ever bound on a long-lived executor (rotating
  device keys). `everBound` is also stricter than needed, since `bindSource` already refuses a live
  issuer. Bound them, or derive the check from current bindings and profiles. `admittedSeq` goes
  away with M1.
- m12. A queued request should hold the token's digest, not the token, so a heap dump of a queued
  item does not leak a live credential.
- m13. `checkpointed` means a checkpoint value was produced, not that it was stored. Say so, or
  rename it.
- m14. Add to the 2c2 isolate test an assertion that `cellCount()` is unchanged by a reading refused
  while latched; the no-mirror decision depends on "allocates no cell".
- m15. Design text that T0 has overtaken. Section 1 and the 5.4 `putBack` row say `putBack` signals;
  after T0's review fixes it is silent, and `drainPosted` abandons the posts of an actor stopped
  before its first turn (`agent_executor.cpp:3881-3887`). The abandon branch must also finish a
  queued health item. Section 13 says "both overloads refuse populated roots until a translator is
  registered"; the reconciler overload refuses always and does not compare policy (T0 report m5). T0
  shifts line numbers by about 60, not 38.
- m16. 3.2's pass credit posts a bookkeeping note per run of dropped readings under overload. The
  runs coalesce and are bounded by the queued accepts, but say so next to the cost figure.

## Questions for the owner

1. M2: should ingestion interleave with a long control batch (the claim in 5.3), or does control
   take strict priority and yield only to other actors? I recommend strict priority.
1. M4: is a relay's replay of old refused events an accepted limit, or does the counted set need a
   bound that does not depend on the outcome log?

## Verdict per slice

- 2a: FAIL (M3, M4). The estimator, the stamp and the settings are not in my discipline and I have
  no objection to them.
- 2b: PASS. The contract, the inline loop, the discard, the gates, the single publication and
  `SettlePending` are sound; m9 and m10 are text fixes.
- 2c1: FAIL (M1 for the `admittedSeq` host state it introduces, M2).
- 2c2: FAIL (M1 on the floor write; it depends on 2c1's fix).

Tally: 0 BLOCKING, 4 MAJOR, 16 MINOR.

Verdict: FAIL

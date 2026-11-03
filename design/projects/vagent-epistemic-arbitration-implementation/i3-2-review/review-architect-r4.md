# I3-2 design review, round 4: System architect (persona architect)

Scope: `i3-2-design.md` revision 4 (commit `d710e7a3`, 1,839 lines), `decisions.md` sections 1 (R3-1
to R3-13) and 3 (A1 to A11), my round-3 report `review-architect-r3.md`, against P19 v1.5 sections
2.1, 2.2, 4, 5.1 and 8.2, the owner's decisions Q1 to Q3, and `remaining-plan.md` section 4.2.
Confirmation round: each round-3 finding is checked, then the sections revision 4 changed
(`git diff 1140853d d710e7a3`). Code at HEAD `d710e7a3` (T0 `4a912c69`, E1 `0dc6bbad`). Scripts are
under `/tmp/p19-architect/` (`r4calc.py`).

## Earlier findings

| Finding                                      | Status   | Why                                                                                                                                                                                                                                                     |
| -------------------------------------------- | -------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| M1 reserve charged to tombstones             | Resolved | R3-1 is implemented in 6.6: ordinary admission sums the reserve over live profiles only, a tombstone is charged its cells, and health actions on a tombstone draw on the `cells / 16` headroom. The `profiles_cap` is gone. See m1 below for a residue. |
| m1 reset floor above what the stream accepts | Resolved | 5.4 replaces `admittedSeq` with the in-flight multiset snapshot, and 6.4 caps the floor at `lastSeq + healthSequenceGap`. Both defeat the lockout I described.                                                                                          |
| m2 `retired_by` sized three ways             | Resolved | 6.1 makes it a named field after the 24 positional ones, absent meaning `superseded`, at most 32 cells a profile, outside `healthBound`, paid from the headroom. One figure now.                                                                        |
| m3 settle turns and handler cost             | Resolved | 4.3 prices a change at `(1 + 4P)` turns times the grant; 5.3 counts every turn before the change starts. I recomputed 44 turns and 4.4 x 10^8 instructions a wake, 3.1 wakes for 100 bare isolates and 40.6 for 13-turn changes (`r4calc.py`).          |
| m4 thread contracts                          | Resolved | 5.1 states that every method but `observe` runs on the pumping thread and how an operator gets `resetEpoch`. 6.5 states the sink contract.                                                                                                              |
| m5 `everBound` unbounded                     | Resolved | 5.2 deletes the set and walks live bindings and profiles at the call, with the recorded cross-executor limit. The walk reads other actors' arenas on the pumping thread, which is safe because `agent_executor.cpp` starts no threads.                  |
| m6 `wouldChange(Reset)`                      | Resolved | 6.3 lists the conditions, including "no reading in flight", and says a `NoChange` reset moves neither `requested` nor `committed`.                                                                                                                      |
| m7 test and measurement gaps                 | Resolved | 3.8 re-measures `AFullEpistemicBudgetStillCheckpoints`; 6.8 tests both `upgrade` overloads, reports the IR footprint and changes the 18-principal tests to 16.                                                                                          |
| m8 citations and `kProgramFormat`            | Resolved | `kIngestOrder` is described as a local constant (`compiler.cpp:4079` at HEAD, checked). Section 7 gives the bump rule and the reason for one bump.                                                                                                      |
| m9 settings domains                          | Resolved | R3-2 turns the capacity relations into a construction WARN plus admission arithmetic, and 6.6 names the joint effect (`healthBound` 1,550 times 128 sources).                                                                                           |
| m10 once-per-event guarantee                 | Resolved | The mark replaces the log (3.2, R3-5); the restart bound is stated and the persisted mark is an owner question.                                                                                                                                         |
| m11 capacity below `max_sources`             | Resolved | 6.6 prints the table. I recomputed it: 129, 90, 69, 47, 29 and 16 live sources at 0, 1, 2, 4, 8 and 16 records. It says plainly that 128 sources is not supported at the defaults.                                                                      |

The owner question of round 3 (raise `cells`, lower `max_sources` or reserve by need) is carried to
O7 and section 14 without a decision. That is acceptable: the design no longer claims 128 sources.

## What is sound

- **Citations moved with E1.** The `compiler.cpp` lines I spot-checked at HEAD (3744-3753 retire
  with no profile, 4079 `kIngestOrder`, 4483 and 4500 `rtSettle_`, 4146, 4184, 4190, 4369) and the
  `agent_executor.cpp` lines (1873 `admit`, 1882 slot gate, 1945, 1989, 3314, 3408, 4948, 4956) all
  match. Section 1 says the `compiler.cpp` numbers are about 1,185 lower than revision 3's.
- **Reserve arithmetic.** `healthBound = 14 + 16R + 20A = 142`, the 4.1 slot reserve (2^30 / 64 =
  16,777,216, and the sum stays below 2^31), the tombstone figures (7 cells bare, 82 latched, 3,510
  and 299 to fill the ceiling, 18 latched actions in the headroom) and the 11.7 years to the ceiling
  all recompute.
- **Settle design.** The termination argument, the executor-side discard as the recovery path only,
  and one publication after the loop hold. A settle turn undone on failure restores `d.pending` to
  `[h, r...]`, so unlinking the head leaves the remainder intact.
- **Reset ordering.** `requested` moves only when `wouldChange` accepts, `committed` only on a
  commit, and an item is superseded iff its generation is below `committed`. I walked two queued
  resets with a reading between them: each reading is superseded by exactly the reset that follows
  it.
- **Latch cache.** It is a negative cache with a test per transition, and the arena stays the
  authority. A retire keeps the latch (a tombstone holds it), which matches A1 and 6.1.
- **C++ versus Vortex (section 8).** Each C++ item names the AGENTS.md category. The estimator mark
  and ring are decided in `observe` before the arena sees the event, so they cannot be IR.
- **Estimator bookkeeping.** Marks are bounded by bindings: `observe` checks the incarnation against
  the binding before any health refusal (`agent_executor.cpp:3626-3629`), so a refusal cannot create
  a mark for an incarnation the binding does not name.

## BLOCKING

None.

## MAJOR

None.

## MINOR

- **m1. The "bounded only by the cell ceiling, as at HEAD" sentence is not quite true.** 6.6 says
  tombstones are bounded by the cell ceiling and the reserve cannot be exhausted by them. At HEAD
  ordinary admission stops ingestion at the ceiling, but a retire is not admitted
  (`agent_executor.cpp:1873` attaches `admit` to ingestion and bookkeeping only) and a retire of a
  principal with no profile allocates a 7-cell tombstone with no count or ceiling check
  (`compiler.cpp:3744-3753`). `bindSource` limits no principals. A host that binds and retires
  without end passes the hard ceiling (3,731 bare tombstones, `r4calc.py`) and then every exempt
  entry, an isolate included, refuses `EpistemicBudget`, and past 4,097 the checkpoint limit of
  28,672 is exceeded. That is a buggy or hostile embedder, not the sensor threat model, and no
  operator remedy exists short of resume. State it in 6.6 (the host owns the bind rate), or admit a
  retire of an unprofiled principal against the hard ceiling and refuse it with a typed code.
- **m2. The headroom has five payers and no sum.** `cells / 16` (1,536) pays for exempt isolates,
  creation past `max_sources`, actions on tombstones, `retired_by` (up to 32 a profile), reset-made
  streams (24 each) and the isolate entry on a tombstone. Advances write `retired_by` with no
  admission at all. I bound the unadmitted `retired_by` stamping at 2 cells per retired record, and
  a retired record costs 82 ordinary cells already counted, so about 600 cells at the ceiling. That
  leaves roughly 900 for the rest, about 11 latched actions rather than the 18 in 6.6. Put the
  shared figure in 6.6 and let the 2c2 checkpoint test fill the headroom with all five payers.
- **m3. "Bare" tombstone cost mixes two figures.** 6.6 says 128 tombstones are "1,280 cells bare
  (5%)". A bare tombstone is 7 cells (896, 3.6%); 1,280 is 128 retired live profiles at about 10
  cells. Say which is meant.
- **m4. The files paragraph is stale about the Makefile.** Section 1 (line 66) says "The Makefile
  source lists and the `vagent_actor_test` object list name each new file". Both are `find` based
  (`Makefile:636-642`, `:1170`, where the actor test takes `tests/xuzz/vagent_%`), so a new `.cpp`
  needs no edit. The real constraint is the opposite one: `vagent_runtime_test` removes every
  `apps/common/xanadu/vagent/*.cpp` object (`Makefile:694`), so the health unit tests must be in
  `vagent_actor_test`, as the planned file name already is. Replace the sentence.
- **m5. The emitter form disagrees with the plan.** Section 1 and 3.8 make new emitters out-of-class
  `Gen` members declared in `compiler_gen.hpp`. `remaining-plan.md:93` still says emitters are free
  functions taking `Gen &`, and its section 4.2 table lists the header as written by "E1 only". The
  design is right (the lazy-routine mechanism already stores `void (Gen::*)()`,
  `compiler_gen.hpp:416-423`). My E1 review (`e1-review/review-architect.md`) gives the reasons;
  change the plan, not the design.
- **m6. New labels shift every agent's cell numbers.** `newLabel()` mints a `Nop` op
  (`compiler_gen.hpp:598`), so a label added to the block of `run()` that mints `rtBDrop_`,
  `rtSettle_` and the others (`compiler.cpp:6279-6283`) moves every later `CellRef` of every agent,
  arbitrating or not. Say, for `rtHealthUpdate_`, `rtHealth_`, `rtSourceDrop_`, `settleEntry` and
  `healthEntry`, whether each is minted for every agent or only when an arbitrated predicate exists
  (`run()` already branches on that for `observeEntry`). The second keeps non-arbitrating agents
  byte-identical, which is what makes E1's IR dump a regression oracle for them in 2a to 2c2. 3.8
  and 6.8 report "the IR footprint of an empty arbitrating agent" only.

## Questions for the owner

None new. O7 and the section 14 capacity question stand.

Tally: 0 BLOCKING, 0 MAJOR, 6 MINOR.

Per slice:

- 2a: PASS
- 2b: PASS (section 4 read as design text only)
- 2c1: PASS
- 2c2: PASS (m1 and m2 are text fixes to 6.6)

Verdict: PASS

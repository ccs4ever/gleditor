# I3-1 revision 2 review: compiler and codebase persona

Base: apps/common/xanadu/vagent/runtime. Round-1 items accepted as resolved on paper: entry
mechanism (`TurnSpec::entry` and `turnFields` exist, agent_executor.cpp 559-579), keyword, split,
settings struct, `rtSettle_`, static refusal (`EBeliefChange` has exactly Believe/Forget/Revise,
ast.hpp 213), binding mutex. The findings below are what the new text still gets wrong or leaves
unimplementable.

## Findings

1. MAJOR: refusal codes cannot leave an undone turn as specified. `runTurnOnce` reports a failed
   Vortex turn as `kFailStatement` (agent_executor.cpp 1672-1676) and calls `arena.release(m0)`
   (1699), which discards every cell the routine wrote. `$answer` is read only for `readOnly` turns
   and only on success (1679-1688). So `StaleSequence`, `ConflictingEvent`, `EpistemicBudget`, and
   the `Duplicate` success (a committed turn with nothing written) are indistinguishable in
   `TurnOutcome`; the outcome log and the bookkeeping write (which needs the code) have no source.
   Fix, to be stated in §5: `rtObserve` writes a typed result code to a reserved turn field before
   `failNow()`, and `runTurnOnce` reads it before the release, on both failure and success, for
   specs flagged `reportsResult`. Add `std::optional<std::string> result` to `TurnOutcome`. This is
   a small but real edit to the shared turn path, so it needs its own test and the core-change
   approval note.

1. MAJOR: the checkpoint-budget arithmetic is wrong in two directions. The limit is
   `ValueLimits::operations` = 65536 applied to every cell and dimension cell reachable from the
   root over all dimensions (mesh_value.cpp 395-415, 448), not "epistemic cells". Hence:

   - §8's 61,000 omits the dedup windows: 64 (id, digest) pairs per principal at roughly 6 cells
     each is about 49,000 more at 128 principals. Together with histories the default cannot fit
     before the baseline agent graph is counted.
   - The record list in §5.5 has about 35 fields; at 2+ cells each plus views and lists, 60 is low
     (90-110). With several keys per principal, 8 records per support multiplies by key count.
   - The admission rule needs a counter. Counting reachable cells at each ingestion is O(graph).
     Specify a per-actor counter field updated in `rtObserve` (record, profile, window, excision all
     adjust it by named per-structure constants), seeded at restore from the actual count, and a
     reservation for the non-epistemic baseline. Reduce defaults (dedup 16, history 4) or raise
     `valueOperations` with a stated argument. Add a test that a full-budget actor checkpoints and
     resumes.

1. MAJOR: identity-token support names break existing source semantics. A support's text is now
   `<principal>#<incarnation>` (§3), but `believes … from "<alias>"` and the `d.source` match
   (`Eq "src?"`, compiler.cpp 4066-4085) compare raw text. A program query `from "sensor:gps"` on an
   arbitrated predicate silently matches nothing. Either the checker refuses `from <source>` on
   arbitrated predicates until I4 (`EpistemicIngressOnly`-style), or the design defines alias
   resolution through the grant table inside Vortex (which would then need grants in the arena,
   contradicting §3). Choose the former for I3-1a.

1. MAJOR: incarnation change double counts. §3 says a new incarnation creates a profile and "retires
   the predecessor's current records", but supports are per `<principal>#<incarnation>`, so the old
   incarnation's supports remain on their clauses as votes until dropped, and nothing in §5 drops
   them. Retiring records is not removing support (`rtBSet`/`rtBDrop` semantics). Specify:
   `rtObserveSet`, for the key being ingested, also drops the same principal's other-incarnation
   support (transferring its history, as for own-source change); for the principal's other keys,
   either a bounded sweep of the arbitrated predicate's clauses (cost O(clauses x sources), state
   the fuel) or lazy handling at next ingestion with an explicit eligibility rule for
   non-live-incarnation supports. "Only the live and previous incarnation profiles are kept" also
   needs the excision of their supports.

1. MAJOR: `restoreState` reusing `UpgradeGraph` is plausible but not drop-in. The class takes
   `const ArenaManifold &from` and `ArenaManifold &into` (agent_executor.cpp 263-266); restore works
   in one arena where the saved graph and the new agent cell coexist, so `from` and `into` alias
   while `copy()` calls `into_.makeCell()` and reads `from_.contentOf(...)` spans. Spans and slots
   may be invalidated by growth; it also resolves dimensions by a role-pair list (`dims_`, 330) and
   throws "unsupported owned upgrade dimension" for any unlisted name, and "unowned clone edge"
   (357-361) for a clone edge to an unmapped cell. Restore must therefore pass saved-to-new
   dimension pairs by name (as `old(name)` does, 3395-3400), include `d.epistemic`, `d.trust`,
   `d.vars`, `d.values`, `d.grab`, `d.step` and `d.clone`, seed `bind()` with the policy master and
   with the agent cell so the profile master is the entry point, and take the copy-then-link
   ordering into account. Require a spike (a restore of a two-source fixture) before I3-1c starts,
   and a test with the same arena aliasing. Note that restore currently has no `d.clone` handling at
   all.

1. MAJOR: the post-refusal bookkeeping write is under-specified and partly impossible. It is "a
   second, minimal write for a bound source", but (a) it needs the refusal code (finding 1); (b) it
   needs its own entry (`rtReject`) and its own `TurnSpec`, which is subject to the same fuel grant
   and may itself fail; (c) refusals returned by `observe` before queueing (`InvalidObservation`,
   `ObservationFrameMismatch`, `IngressBackpressure`) have no turn at all, so a device sending
   malformed data still looks silent to I3-3, contradicting the stated goal; (d) there is no profile
   to write for `EpistemicBudget` or an unbound source. State which refusals bookkeep, route the
   pre-queue ones through the same queue (bounded, via a cheaper bookkeeping item), and say that
   backpressure refusals do not write.

1. MINOR: `ActorClock::time_point` does not exist. The executor uses `clock_.steady()` and
   `wallNow()` (agent_executor.cpp 705, 1091); name the type actually used for lease expiry.

1. MINOR: "upgrade schema carries the full typed policy and its hash" is new work, not wiring.
   `EpistemicPolicySchema` (upgrade_schema.hpp 40) has two fields and is never populated or
   compared; the only hash in the schema is the plan `astHash` (upgrade_schema.cpp 589). Add the
   policy fields, a canonical byte encoding for the hash (integers in ms, no float printing), the
   `UpgradeChangeKind` entry and its name, and tests. Include a version-bump note, as
   `PlanHashVersion` does.

1. MINOR: dedup and history lists add per-turn fuel. A newest-first 64-entry window with text
   comparisons on event IDs is a 64-iteration Vortex loop plus excision of the oldest entry, plus
   the 128-profile linear find in §5.3. State the fuel estimate against the actor grant's
   `maxInstructions`, and keep the profile find O(principals) with text `Eq` on the token only.

1. MINOR: the static refusal must also cover `believes … from` of finding 3 and any compiled path
   that reaches `rtBSet` with a predicate by name (e.g. initial facts in `init`). Add a compiler
   `assert` that `rtBSet`'s only entries for an arbitrated predicate are from `rtObserveSet`, so a
   later feature cannot bypass the checker.

1. MINOR: tests. Name the new Makefile entries (VAGENT_RUNTIME_TEST_OBJS list, Makefile near 684)
   and files: parser, checker, `vagent_epistemic_ingress_test.cpp`, `vagent_resume_test.cpp` and
   `vagent_upgrade_schema_test.cpp` additions; plus the fuel-exhaustion test needs a small
   `maxInstructions` grant and a cell-count spy.

## Implementation plan changes versus round 1

- I3-1a: add the `from <alias>` refusal; policy hash with encoding; no runtime change.
- I3-1b: first land the `TurnOutcome::result` mechanism (finding 1) as its own commit with its own
  approval; then grants/observe/PostBox kind/outcome log; then `rtFindClause`, `rtFindSrc`,
  `rtObserveSet`, `observeEntry`, `rtReject`; incarnation handling per finding 4.
- I3-1c: budget counter and admission first (finding 2), then the restore spike (finding 5), then
  the copier.

## Verdict on sizing

The three-slice split is right. I3-1b remains the largest (about 1200-1600 lines with tests);
consider splitting the incarnation/profile-chain logic (§3 incarnation paragraph, finding 4) into an
I3-1b2.

## Tally

BLOCKING 0, MAJOR 6, MINOR 5.

VERDICT: FAIL

# Remaining-work plan review, round 1: System architect (persona architect)

Scope: `remaining-plan.md` (draft 1) and the `traceability.md` rows in the architect's area (1-12,
2.1-33, U-1 to U-3, X-1, X-12, X-17, X-20, 1-10, 8.2-0a, 9-5), against spec sections 2.1, 2.2, 6.1
to 6.3, 8 and 8.2, code at HEAD `3ae8d384`. Script: `/tmp/p19-architect/scan.py` (per-slice file
lists and sizes). Paths below are relative to `apps/common/xanadu/vagent/runtime/` unless stated.

## What is sound

- Order. Designs before slices, E1 before every slice that edits ingestion IR, I3-2c1's version cell
  before I4-1, and I4-7 before I3-4a are real dependencies. None is missing that I found.
- E1 is feasible and cheap. `class Gen` is declared in one block (`compiler.cpp:414-1278`, about 865
  lines) and its members are already defined out of class (`void Gen::emitObserve`, `:5164`). The
  move is a relocation, not a rewrite, which supports the lead's lean: minimal E1, no bulk move.
- Standing rules: the budget-parity rule, the settings rule and "no shim, bump the version" match
  `AGENTS.md`.
- O1(b) is cheap and honest at HEAD. The runtime compiler rejects `GoalDecl` and `PlanDecl`
  (`compiler.cpp:7495-7499`), so a compiled actor cannot hold an intention today. The refusal costs
  nothing now and becomes a real boundary only when P17 lands.
- The T0 defect is real. Only the shared-pointer overload checks populated roots, the policy diff
  and quiescence (`agent_executor.cpp:5053-5116`, `:5315-5319`). The `CompiledProgram&` overload
  (`:5419-5470`) checks none of them.

## BLOCKING

None.

## MAJOR

### M1. The plan's policy-encoding versions are already used at HEAD

Problem: I4-1 is to add `vagent-policy-2` and I5-3 `vagent-policy-3` (slices I4-1, I5-3; risk
table). Evidence: `canonicalPolicy` already writes `vagent-policy-2` (`epistemic_policy.cpp:343`,
commit `0124b0d2`; `i3-design.md:701`). Consequence: a new shape written under an existing tag. An
old checkpoint then fails as a hash mismatch, not a "refused by version number", which is the
failure the AGENTS rule exists to prevent. Required fix: I4-1 writes `vagent-policy-3`, I5-3 `-4`.
State the rule in the standing rules: the tag is bumped by the slice that changes the encoding, and
a test asserts the tag string and that the previous tag is refused by name.

### M2. No version of the owned-state shape exists, and the plan assumes one

Problem: T1 promises "typed refusals for an unknown shape version", and I4-1 "freezes the whole
record shape". Neither has anything to check. Evidence: the only versions are `kProgramFormat`
(`program.hpp:42`, the compiled program), `kObservationVersion` (`runtime_abi.hpp:579`, one record)
and the policy tag. `restoreState` clones whatever it finds with `cloneGraph`
(`agent_executor.cpp:4773-4840`). Spec 8.1 and the policy tag do not cover profile, health or
calibration shape. The "state version cell" of I3-2c1 (X-12) is a change counter for
`StaleEpistemicQuery`, not a shape version, and the plan uses "version" for both. The calibration
shape cannot be frozen in I4-1 either: weights and normalizer length depend on the feature count,
which arrives with policy v3 in I5-3. Consequence: with the same source (same program ref), a
compiler change to a record's fields resumes old cells into routines that read different field
names, silently. Required fix: add a separate `epistemic_abi` shape version, written at the agent's
`d.epistemic` head in I3-2c1, bumped by any slice that changes a record shape, checked by
`restoreState` and the translator by number. Name the change counter differently ("epoch counter").
Replace "frozen" in I4-1 with "shaped for the fields known at I4-1; vectors are separate cells whose
length is a policy value".

### M3. Two upgrade copiers exist; T0 unifies checks, T1 assumes one shared copier

Problem: T0 makes "one function" for the preconditions. T1 then claims the translator "reuses the
copier of `restoreState`" so "resume and upgrade cannot diverge". Evidence: the two overloads copy
state by different code. The shared-pointer overload has its own graph copier and the only call to
the translator seam (`agent_executor.cpp:5330`). The `CompiledProgram&` overload copies through
`makeStateReconciler` (`reconciler.cpp:1036`, `agent_executor.cpp:5475`), which has no reference to
`d.trust` or `d.epistemic`. `LocalHost::upgrade(pid, CompiledProgram)` (`program.cpp:397`) and
therefore `vagent`/cluster use the second. `restoreState` copies inside one arena with old-to-new
dimension pairs (`cloneGraph(arena, ...)`, `:240`); upgrade copies from a source manifold into a new
context. The two also differ on purpose: resume clears `last_receive`/`last_contact` because the
clock instance changed (`:4781-4790`) and throws on a policy-hash change (`:4838`); upgrade keeps
the clock and refuses on a schema diff. Consequence: a translator wired to one path leaves the other
path either unguarded again after T0, or untranslated. The property test "resume and upgrade produce
the same digest" is false as written. Required fix: (1) T0 removes one overload's copier, preferably
`LocalHost::upgrade(pid, CompiledProgram)` forwarding to the shared-pointer path, so a populated
actor has one road; (2) add a T1 design section (T1 has none; it is the highest-risk slice in my
area) that names the shared routine's signature (source arena, destination arena, dimension map,
reference map), the list of deliberate differences, and defines the digest as equal modulo them; (3)
T0 introduces the typed refusal for a populated intention rank, not only T2 (today it is
`ExecutionFailed`, `:5073`).

### M4. O3 conflates two token lifetimes and contradicts the plan's own query test

Problem: O3(b) hangs "the query token" on the agent's `d.epistemic` rank, bounded by retention. I4-6
requires that a query never enqueues anything and that "repeated queries leave the arena size flat";
I4-7 requires a ranked continuation that survives a mark release. Evidence: spec 6.2 says the
generator's owner holds IDs, scores and position "under its enclosing lifetime" and releases them on
exhaustion; spec 6.1 says selection tokens are "reconstructed outside the inner mark from stable
observation IDs". Spec 6.2 also says to select in a pure guard, commit, then act. There are two
things: a ranked continuation (lifetime: the handler activation, option c) and a verification token
(lifetime: until the verification horizon, needed only if the plan acts on the selection). A token
minted at every query makes reads write durable state, grows with query rate, and puts an eviction
policy (the retention bound) in conflict with `ExpiredVerificationTarget`. Consequence: I4-6's flat
arena test and I4-7's design cannot both hold. Required fix: split O3. Continuations: activation
scope. Verification tokens: minted at the commit point where the plan acts on a selection (or where
a verifier requests one), on the agent's `d.epistemic`, bounded and charged. Owner approves the
amendment to 2.1 (an intention cannot anchor tokens until P17). I4-7 gets two sub-tests, and I3-4a's
dependency becomes "commit-point token", not all of I4-7.

### M5. The translator parity rule is not applied by the slices, and T2 duplicates it

Problem: the standing rule says a slice that adds an owned structure extends the translator and adds
a resume and upgrade test, "without it the slice is not accepted". Evidence (`scan.py`): of the
slices that add owned structures after T1, only I4-7, I5-3 and I6-2 list `epistemic_translate.cpp`.
I3-3b (clock snapshots), I4-1 (calibration), I4-4 (groups), I3-4b (journal), I4-8 (verdict cells),
I5-1 (ledger), I5-2 (defeaters) and I5-5 (epochs) do not, yet T2 closes the rows for them (U-4 to
U-11, 3.1-15, T-12) and lists "calibration, groups, tokens, journals, ledgers, defeaters". T1 also
translates the retired chain, which I4-1 re-homes (X-6), and the state-version cell, which M2
splits. Consequence: either the rule is not enforced (T2 becomes the 10-structure bulk change that
option D rejected), or the file lists are wrong and rows are double-assigned. Required fix: add
`epistemic_translate.cpp` and a populated-state resume and upgrade test to every slice above; reduce
T2 to typed migrations (epoch, schema), the intention refusal, the full-bound lifecycle test and the
audit; move rows to the slice that adds the structure; exclude the retired chain from T1.

### M6. `compiler_epistemic.cpp` becomes the next monolith, and E1's header step is under-specified

Problem: E1 stops the growth of two files and then sends all growth to one. Evidence: after E1, 21
slices name `compiler_epistemic.cpp` (code plus tests sum about 26,900 lines of the 40,000 estimate)
and 12 name `agent_executor_epistemic.cpp` (about 15,200). The 400-line growth rule covers only the
two old files. `runtime_abi.hpp` (887 lines) is edited by about 15 slices. `Gen` and its helpers sit
in an anonymous namespace (`compiler.cpp:92-7579`); a header cannot export them from there, and the
helpers at `:92-413` (type tags, constants, `Bail`) are needed by both files. The move range
`:4308-5735` includes non-epistemic helpers (`mintPredicates`, `breakOp`, `predOf`, `planTerm`
starts at `:5735`) and the belief routines `believe`/`forget` also use. Consequence: the same review
and conflict cost reappears in a file named for a feature. Required fix: E1 states the header
contents (helpers and `Gen` in a named `detail` namespace, no anonymous namespace in a header),
fixes the move boundary by function name, and sets the structure for later slices: one file per
concern (`epistemic_health`, `epistemic_scoring`, `epistemic_query`, `epistemic_verify`,
`epistemic_ledger`), the 400-line rule applied to `compiler_epistemic.cpp` and to `runtime_abi.hpp`
(one ABI header per concern, included by a thin `runtime_abi.hpp`). Keep the lead's lean: no bulk
move beyond that.

### M7. O2's recommendation rests on a false premise (X-17)

Problem: X-17 and O2 say "actors never load `vortex_stdlib`", so option (b) "adds a loader".
Evidence: every actor context is a `WorkerContext` (`vortex/mesh_actor.cpp:197`), which holds a
`VortexStdLib` (`vortex/worker_context.hpp:66`) whose constructor calls `bootstrap()`
(`vortex/vortex_stdlib.cpp:1448-1451`), and that creates `std:math` with `exp`, `ln`, `sqrt`
(`:1477`, `:1511-1520`). The modules are already in each actor's arena; the vagent compiler just
does not call them. Consequence: the owner decides on a cost that does not exist. Recommendation (a)
can still be right, for these reasons: emission is conditional on `arbitrated` predicates (as
`emitObserve` is, `compiler.cpp:7531-7540`), the numerics need agent-private ABI constants, and
loading scoring graphs into every actor's arena would cost cells for non-epistemic actors. Required
fix: restate O2 with the true facts for the owner; amend X-17. Add a design constraint to I4-D: each
numeric routine is a pure scalar routine with a fixed call ABI (input and output cells), built
through a small emitter library, and each is run from a unit test in a bare `WorkerContext` without
an agent, as `vortex_scoped_evaluation_test` does. That makes the 14,000 lines of IR emitters
reviewable and lets them be promoted to a stdlib module without rework. The rule "New C++ must
justify why it is not Vortex" (AGENTS) is met only if this justification is written in I4-D.

### M8. The definition of done permits "P19 is implemented" with no shipped path that feeds it

Problem: nothing outside `AgentExecutor` calls `bindSource` or `observe` (grep over `apps/`: none;
matrix X-3). I3-3a delivers an adapter class and tests against fakes (O4(a)). No slice wires it to a
program a user can run (`vagent run`, `vagent/cluster`) or defines the binding policy in
`system://settings`; the journeys of section 8.5 are tests that call the API. Consequence: F2 can
pass while no shipped binary can ingest a signed reading, query, or verify, and the final record
will read as an implemented feature. Also, DoD item 2 rewrites spec 8.2 gate 0 ("complete the
independent full-vagent upgrade prerequisite before claiming P19 upgrade support") to O1(b); the
claim needs the quiescence precondition too, because `upgrade` refuses with pending posts, flights
or requests (`agent_executor.cpp:5070-5080`), and an actor under continuous ingestion may be rarely
quiescent. Required fix: add to section 8 either (a) a wiring slice (`vagent run` binding and
adapter options, a settings entry, one headless end-to-end journey from a signed reading to a ranked
answer through the CLI), or (b) an explicit statement that P19 is delivered as a runtime and library
capability and that CLI integration is outside this plan, with the owner's approval. Word the F2
claim as: "P19 is implemented for supported agents (no active intention, no native skill state),
quiescent upgrade only, deferred rows as listed".

## MINOR

- **I4-D spike.** The query frame ABI is settled in I4-D, but the first emission from the actor
  compiler is in I4-6. Add a throwaway spike (one trivial `ScopedEvaluate` from the actor compiler,
  run headless) before I4-D is reviewed, so the design is judged against something that runs.
- **Emitter seam.** New files that add `Gen` members must edit the private header each time. Prefer
  free emitter functions taking `Gen&` from I4 on, so the header stops changing.
- **C++ ingress state.** Bindings, alias map and incarnation floors live in C++ `Ingress` and are
  not checkpointed (matrix 1-10). I3-2c2 and I3-3a should state, for each, what a resume loses and
  why a replay cannot then be accepted (the profile's last sequence is in the arena; say so, and
  test it).
- **Row counts.** The plan closes 357 rows and the matrix has 354 open; the difference is the
  Informational rows. State the two numbers in section 4.2 so the totals can be audited.
- **DoD item 3.** "`make check` is clean on the touched files" should be the whole tree or the full
  diff from HEAD `3ae8d384`; "clean on touched files" lets a lint regression elsewhere through.
  Record that the swarm netns tests need the `veth` module (AGENTS), so the three gtest `[ PASSED ]`
  lines are the evidence in an environment without it.
- **Adapter reuse (I3-3a).** Use the existing `DeviceDelegation`/`StandardCryptoEngine` X.509 path;
  do not add a second verifier. State this in I3-3D.

## Answers to the ordering questions and owner decisions

| Item                          | Answer              | Why                                                                                                                                                                                                                                 |
| ----------------------------- | ------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| A. Verification before tokens | Agree, option 2     | Needs M4's split: only the commit-point token gates I3-4a, not all of I4-7.                                                                                                                                                         |
| B. Labels in I5-1             | Agree, option 3     | One mutation path (spec 5.1). The ML persona judges the claim boundary.                                                                                                                                                             |
| C. Calibration and groups     | Agree with change   | The record cannot be "frozen" at I4-1 (M2); freeze the fields known then plus a shape version. I4-4 may move after I4-9 if a slice is needed.                                                                                       |
| D. Translator timing          | Agree, option 3     | Conditional on M3 (one upgrade road, a T1 design section) and M5 (parity enforced in the slice file lists).                                                                                                                         |
| E. Upgrade claim              | Agree, option 2     | See O1 and M8.                                                                                                                                                                                                                      |
| O1                            | Agree, (b)          | Owner must still approve the amended gate 0; the claim is scoped to supported agents and quiescent upgrade.                                                                                                                         |
| O2                            | Owner must decide   | Recommendation (a) is plausible but rests on a wrong premise (M7). The decision amends the approved P19.2/P19.3 file lists, so the lead cannot take it alone.                                                                       |
| O3                            | Disagree as framed  | Split continuation (activation scope) from verification token (commit point on the agent's `d.epistemic`), M4. Owner approves the 2.1 amendment.                                                                                    |
| O4                            | Agree, (a), with M8 | A reference adapter reusing the existing delegation code; wiring or an explicit non-goal is needed.                                                                                                                                 |
| O5                            | Agree               | The lead's lean also holds: accumulate the amendments in one normative record that each design cites, fold into v1.6 at F1, and do not block E0. List the record in the README.                                                     |
| O6                            | Owner must decide   | `t_star` and the trusted reference are semantics the approved spec leaves open; a lead ruling would write spec content. Recommendation is acceptable from the architecture side (a grant with an issuer outside the bound sources). |
| O7                            | Agree, (a)          | Measure earlier than I4-11: put the cell cost of the I4-1 record shape in I4-D, since a late finding that the default holds 18 principals would force rework.                                                                       |
| O8                            | Agree               | Bounded arena audit list plus host log.                                                                                                                                                                                             |

Lead's leanings: E1 minimal (agree, with M6's header and boundary specifics); amendments accumulate
to v1.6 at F1 (agree); T0's two defects first (agree; T0 should also carry M3's overload collapse).

## Process economy

- Keep E0 small, as a record, not blocking.
- Do not merge E1 into T0: E1's review is "a pure move" and a mixed diff defeats the moved-line
  check.
- I4-2 and I4-3 share reviewers and file; they are 2,300 lines together and exceed the 1,500-line
  cap, so keep them separate.
- Missing gate: a translator audit at the end of each iteration (I3, I4, I5), by script, listing
  every structure the iteration added and the translator test that covers it. It is cheap and
  replaces most of T2's discovery work (M5).

Tally: 0 BLOCKING, 8 MAJOR, 6 MINOR.

Verdict: FAIL

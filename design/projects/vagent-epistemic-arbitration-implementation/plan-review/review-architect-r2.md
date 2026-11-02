# Remaining-work plan review, round 2: System architect (persona architect)

Scope: `remaining-plan.md` draft 2 (uncommitted), `traceability.md` (targets for draft 2),
`decisions.md` section 2 (O1-O10, E1, T0, D1-D3), my round-1 report. Checked against spec sections
2.1, 2.2, 6.1 to 6.3, 8, 8.2, code at HEAD `8a4089d9` (identical to `3ae8d384` under `apps/`,
`tests/`, `Makefile`; the working tree holds uncommitted edits to `agent_executor.{hpp,cpp}` and
`vagent_epistemic_ingress_test.cpp`). Paths are relative to `apps/common/xanadu/` unless stated.
Scripts: `/tmp/p19-architect/r2check.py` (slice row lists against the matrix),
`/tmp/p19-architect/r2sizes.py` (file-size estimate). Copies of HEAD sources are in
`/tmp/p19-architect/r2/`.

## Earlier findings

| Round-1 finding                  | Status              | Why                                                                                                                                                                                                                                                                                                                                        |
| -------------------------------- | ------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| M1 policy tags already used      | Resolved            | Section 2 "Version rules" and I4-1a: `vagent-policy-3` once, `-2` refused by name; `-4` only if I5-D finds a missing key. `epistemic_policy.cpp:343` still writes `-2`, so `-3` is free. The channel for "refused by name" is a new finding (M1 below).                                                                                    |
| M2 no owned-state shape version  | Resolved in intent  | Section 2 separates policy tag, shape version, state counter; I3-2c1 writes the first two cells; "frozen" became the envelope (section 3 C). New finding M3 below covers how those cells interact with the upgrade guards. `i3-2-design.md` rev 3 has no text on either cell yet (grep: none), so I3-2D must add it.                       |
| M3 two upgrade copiers           | Resolved by ruling  | Lead ruling T0: share the guards, leave the copiers; T1-D decides collapse. I accept the split. Residual items for T1-D are minor m4. The `CompiledProgram&` overload still copies through `makeStateReconciler` (`agent_executor.cpp:5459`) and never consults `ownedStateTranslator_` (the only use is `:5330`).                         |
| M4 O3 conflates two lifetimes    | Resolved            | I4-7a (continuation, activation scope) and I4-7b (commit-point token on `d.epistemic`) match spec 6.1 and 6.2. I4-6's flat-arena test and I4-7a hold together because a read writes nothing. I3-4a depends on I4-7b only (checked in section 4.1 and in the I3-4a text). Residual: m9.                                                     |
| M5 translator parity unenforced  | Resolved, with gaps | Every structure-adding slice I listed now names `epistemic_translate_<area>.cpp`; U-7, U-8, U-21, 3.1-15, U-22, T-12 moved. The matrix and the plan agree on all 354 rows (script: 354 listed, no duplicates, 0 target mismatches, 468 matrix rows parsed). Gaps m3, m6.                                                                   |
| M6 next monolith / E1 header     | Resolved            | Section 4.2 names files per area; E1 names the `codegen` namespace and the ranges. I verified them: `class Gen` is `compiler.cpp:414-1278`, the helper block is `:92-413`, and the only top-level definitions in `:1278-7579` are `Gen::` members and two `static` helpers (`isCompoundStmt :3321`, `hasDynamicCall :6376`). Residual: m1. |
| M7 O2 false premise              | Resolved            | Plan section 7.1 and X-17 restate the `WorkerContext` fact; ruling O2 follows the spec file list; the design constraints (pure scalar routine, fixed ABI, bare-`WorkerContext` test, written C++ justification) are in section 2 and I4-D2.                                                                                                |
| M8 no shipped path; gate 0 claim | Partly resolved     | W1 and W2 exist, 7.3 keeps the explicit non-goal alternative, and the F2 claim is worded as I asked. The gate 0 point stands, and the lead's "no amendment is needed" is wrong (M2 below). W1's settings carrier is m8.                                                                                                                    |
| Minor: I4-D spike                | Resolved            | S0 in section 4.4.                                                                                                                                                                                                                                                                                                                         |
| Minor: emitter seam              | Resolved in text    | Free emitter functions from E1 on (section 2); see m1 for the header detail.                                                                                                                                                                                                                                                               |
| Minor: C++ ingress state         | Resolved            | "Host state against arena state" rule; I3-2c2 tests a replay after resume.                                                                                                                                                                                                                                                                 |
| Minor: row counts                | Resolved, verified  | Section 4.3 gives both numbers; 354 by script (section 4.5 lists sum to 354).                                                                                                                                                                                                                                                              |
| Minor: DoD item 3, veth          | Resolved            | Section 8 item 3.                                                                                                                                                                                                                                                                                                                          |
| Minor: adapter reuse             | Resolved            | I3-3a reuses `DeviceDelegation`/`StandardCryptoEngine`.                                                                                                                                                                                                                                                                                    |

## What is sound

- The order. The new dependencies are correct: I5-1a before I4-6 and I3-4c, I4-7b before I3-4a,
  I3-3b before I4-6, T1-D before T1. I found no missing edge.
- Section 4.2. A rough estimate (45% of each slice's lines in its test files, 55% in its source
  files, split evenly over the files the slice names, files over 1,500 lines excluded) puts the
  largest area file at about 1,700 lines (`vagent_epistemic_query_test.cpp`, 1,665;
  `agent_executor_health.cpp`, 1,661). Concentrating all source of the health slices in
  `epistemic_emit_health.cpp` gives about 3,000, the edge of the cap; the 400-line rule per slice
  and a line count at acceptance cover that.
- The build seams the plan relies on exist. `COMMON_XANADU_SRCS` and `XUZZ_TEST_SRCS` use `find`
  (`Makefile:636,642`), and `tests/xuzz/vagent_*` goes into `vagent_actor_test` automatically
  (`:1170`). The W1 test shells out to `build/vagent` (`tests/xuzz/vagent_cli_test.cpp:36`), which
  is headless-safe.
- T1-D is the right place for the copier decision, provided it also takes the items in m4. The
  interim state is safe after T0: a populated actor is refused on both roads.
- I4-7a/I4-7b, I5-1a before I4-3, and the O3 split are coherent with spec 6.1 and 6.2.

## BLOCKING

None.

## MAJOR

### M1. There is no channel for the typed refusals that T0, I3-2c1, I4-1a and T1 promise

Problem: the plan promises "a typed, tested error" or a refusal "by number" for: the populated
intention rank (T0), a policy-hash mismatch on resume (T0, X-9), another shape version (I3-2c1),
another policy tag "naming both tags" (I4-1a), an unknown shape or no translator (T1). None of those
slices lists a file that defines a vehicle for it.

Evidence:

- `AgentExecutor::resume` and both `upgrade` overloads return `std::expected<..., ActorError>`
  (`agent_executor.hpp:448-450`). `ActorError` is a closed 13-value enum in
  `vortex/mesh_actor.hpp:30-46` with no payload; adding policy-specific enumerators puts the P19
  vocabulary in the mesh layer, and the enum is compared in cluster tests
  (`vagent_cluster_packaging_test.cpp:848,1004`).
- On resume, `restoreState` runs inside `prepare()` on the new actor's first turn: `resume()`
  returns the pid after `runtime_.resume` (`agent_executor.cpp:5044-5050`), and `execute` calls
  `prepare(..., execution.stateRoot)` later (`:1460`). `restoreState` throws `std::runtime_error`
  for a changed policy (`:4838`); `prepare` catches it, logs at DEBUG and returns false
  (`:1399-1402`), and `execute` returns `fatal(state, kCauseInit)` (`:1461`). So a host that calls
  `resume` gets a live pid and then a dead actor with no typed reason.
- The only typed-refusal precedent is `CheckpointRefusal` plus `toString` (`:174-186`) for
  `checkpoint()`.

Consequence: every "refuses by number" test in the plan has nothing to assert against except a debug
log. The refusals either get invented per slice (several incompatible vehicles) or are tested
through the fatal cause string. A policy-tag or shape-version mismatch on resume is the exact
failure the AGENTS rule ("refuse loudly, by number") exists to prevent, and today it looks like a
successful resume.

Required fix: T0 (the first slice that needs it) defines one refusal vocabulary and says where each
check runs. Options to state and choose between in T0's text: (a) an executor-level
`EpistemicRefusal` enum with `toString` and the offending numbers, returned by a checked pre-flight
that `resume` runs before `runtime_.resume` (the shape version, policy tag and policy hashes are
readable from the checkpoint's sealed state, or the checkpoint header gains them); (b) a typed
`Refusal` side record that `fatal(kCauseInit)` carries and `AgentExecutor` exposes
(`lastRefusal(pid)`), with `resume()` documented as admission only. Either way list the files it
touches (`agent_executor.hpp`, `program.hpp` for `LocalHost`, `cluster_node.cpp`, `agent_tui.cpp`
callers) and add a test per refusal that asserts the typed value, not a log line. I3-2c1, I4-1a and
T1 then cite that vocabulary.

### M2. O1(b) does contradict spec 8.2 gate 0, and "no amendment is needed" is wrong

Problem: `decisions.md` O1 and plan section 7.1, 8 item 2 say O1(b) needs no amendment and that
"gate 0 reads as O1(b) decides". The spec text is: "Complete the independent full-vagent upgrade
prerequisite before claiming P19 upgrade support"
(`vagent-dynamic-source-weighting-and-epistemic- arbitration.md:966-968`, and row 1028 "Full vagent
upgrade pipeline plus P19 translation"). O1(b) claims P19 upgrade support for supported agents
without that prerequisite (the P17 pipeline cannot lower or resume intentions:
`agent_executor.cpp:5081`, `compiler.cpp:7495-7499`).

Evidence: the spec does say at line 200 that the shared routine "does not claim an automatic full
P17 upgrade exists", which supports building the translator without P17. It does not support the F2
sentence "P19 is implemented for supported agents ... quiescent upgrade only", which is a claim of
P19 upgrade support.

Consequence: the final record claims something the approved text forbids, and the owner has not
ratified the scope. Decisions section 3 has no row for it, so F1's v1.6 will not carry it.

Required fix: add a proposed amendment row A5 to `decisions.md` section 3: "8.2 gate 0: P19 upgrade
support is claimed for agents without an active intention, quiescent upgrade only; full-vagent
upgrade support remains owed to P17". Reword plan 7.1 O1, section 3 E and section 8 item 2 to say "a
lead ruling within the spec, plus proposed amendment A5". Keep the T0 intention refusal, which makes
the boundary real.

### M3. The shape-version and state-counter cells collide with the populated-root guards

Problem: I3-2c1 writes "two cells at the head of the agent's `d.epistemic` rank". T0 refuses an
upgrade when `d.trust` or `d.epistemic` is populated, as the shared-pointer overload does today
(`agent_executor.cpp:5315-5319`:
`!ownedStateTranslator_ && (linked(agentCell, trust) || linked(agentCell, epistemic))`).

Evidence: at HEAD nothing links the agent cell's `d.epistemic` (the `compiler.cpp` uses of
`dims_.epistemic` are on predicates, profiles and supports: `:4371`, `:5033-5664`), so the
`d.epistemic` half of the guard is vacuous today. I3-2c1 makes it live.

Consequence: depending on how the cells are minted, either (a) every compiled agent, including those
with no arbitrated predicate, becomes "populated" and cannot upgrade between I3-2c1 and T1, a
regression across `vagent_upgrade_test.cpp` (1,101 lines); or (b) an arbitrating agent with no
profiles yet is refused for a header no translator needs to move. The plan does not say, and T1's
"unknown shape version refuses" has no defined equal-version case for the header itself (the new
program mints its own header, so the translator can only compare, not copy).

Required fix: I3-2D and T0 state, in one sentence each: (1) the two cells are minted only for an
agent whose program declares an arbitrated predicate (so unrelated agents pay nothing and keep
upgrading); (2) the guard is a single function `hasTranslatableEpistemicState` that ignores the two
header cells and is conditional on a translator being registered for that shape version, so I3-2c1
changes the function, not every call site; (3) `restoreState` and the translator compare the saved
header with the freshly minted one by number and never copy it; (4) the state counter is not carried
across resume or upgrade (continuations die there). Add a test: a non-arbitrating agent upgrades
unchanged after I3-2c1, and an arbitrating agent with no profile upgrades.

## MINOR

- **m1. E1 header details.** The 865-line class body contains inline member bodies (for example
  `mkOp`, `store`, `mov` at `compiler.cpp:568-770`), so "no emitter body moves" should read "no
  out-of-class body moves". `tyOf` (`:128`) and `collectCallees` (`:403`) are non-template functions
  and need `inline` in a header. Choosing "the members emitters use" to make public before the
  emitters exist will force a header edit per area; make the whole class a `struct` in the private
  `codegen` header instead. Give `Gen` one per-area state slot (a pointer to a struct each area
  owns) so an emitter needing per-compile state does not edit the header either.
- **m2. Citation drift.** The plan cites the intention refusal at `agent_executor.cpp:5073`; at HEAD
  it is `:5081`. E0's "refresh stale citations" can cover it.
- **m3. Translator coverage oracle.** `tools/check-translator-coverage.py` "lists every structure an
  iteration added" but no input defines a structure. T1-D should define the oracle: each per-area
  ABI header exports a list of owned dimensions and record kinds, each translator test registers the
  ones it covers, and the script diffs the two lists and also greps `ensureDimension` names against
  the first list so a structure that nobody declared is caught.
- **m4. T1-D requirements.** (a) Acceptance criterion: no public upgrade entry point accepts a state
  another refuses; if the copiers stay separate, `LocalHost::upgrade(pid, CompiledProgram)`
  (`program.cpp:399`; `cluster_node.cpp:154` builds a `LocalHost`, and the TUI calls the executor's
  shared-pointer overload at `agent_tui.cpp:649`) either forwards or installs the translator,
  because F2 claims upgrade for supported agents and `LocalHost` is the shipped host type. (b)
  Quiescence: the current check refuses with any pending post (`posts_->pendingFor(pid)`,
  `agent_executor.cpp: 5065-5066`), so an actor under steady ingestion is rarely quiescent. T1-D
  names the operator path to quiesce (pause admission for a pid, drain, upgrade, resume) or states
  that upgrade requires the adapter to be paused. (c) The parity rule's "both copiers" means resume
  and upgrade; say which upgrade road each test drives.
- **m5. Writers of alpha and beta.** I5-1a says `ledgerApply` is "the only writer". I4-1b writes
  them at creation and at reset ("reset returns it to the prior"). Reword to "the only writer after
  creation", and add to I5-1a that a reset routes through `ledgerApply` (clears the ledger,
  recomputes the prior) with a test, since I4-1b's reset predates the ledger.
- **m6. Parity gaps.** I3-3c changes liveness at resume and upgrade (deadline rebase, "a resume
  never restarts a full grace period") but its Files list has no `restoreState` or translator line;
  I3-3d adds origin-tagged reasons on health records. Add a both-roads populated-state test line to
  each, as the rule requires.
- **m7. Makefile and test binaries.** "The Makefile finds sources by `find` and needs no edit" holds
  for sources and `tests/xuzz`. The new fuzzers (`fuzz_epistemic_verification` in I3-4c,
  `fuzz_epistemic_query` in I4-11, the adapter fuzzer) need `$(eval $(call fuzz-target,...))` lines
  and an entry in the `fuzz:` list (`Makefile:1046-1058`). The bare-`WorkerContext` numerics test
  `vortex_stdlib_epistemic_test.cpp` lands in `xuzz_test`, not `vagent_runtime_test`, whose object
  list is explicit (`:692`); state which binary runs it and add `Makefile` to those slices' files.
- **m8. W1 settings carrier.** Settings entries are flat typed tuples
  (`SettingSchemaShape{expectedTypes, defaultValues}`, `system_docs.hpp:780`), so an
  alias-to-principal table cannot be one setting. I3-3D should decide the carrier (a system xanadoc
  dimension, or a file path named by a setting) before W1, and W1 should go through a design section
  of I3-3D or V-D since it carries security decisions at 900 lines.
- **m9. Verifier-requested tokens.** I4-7b mints a token "or where a verifier asks for one". The
  I3-3a message union has observation, verification, drift, reference; there is no token-request
  message. I4-D2 defines it or drops the clause.
- **m10. Optional economy.** The numerics slices (I4-2, I4-3, I4-8's gate routines, I4-9 fusion,
  I5-4) write only `vortex/vortex_stdlib_epistemic_*.cpp` and their tests. Once I4-D1 and I4-D2 are
  accepted the pure-routine halves could run in a second worktree beside I3-3, shortening the single
  sequential chain of 42 slices. The plan's "strictly sequential" rule can keep, or relax, that; the
  lead decides.

## Answers

1. **Can the I3-2 slices start?** Not yet, and for reasons already in the plan: I3-2D must be
   accepted first, then T0, then E1. Three things must change before T0 and I3-2D are written: M1
   (refusal vocabulary, T0), M3 (header cells and guard, T0 and I3-2D), and M2 (record A5 now; it
   does not block I3-2a but blocks the F2 wording). `i3-2-design.md` revision 3 has no text on the
   shape version or the state counter, so I3-2c1's design is not yet written.
1. **Can the designs start?** I4-D1 can start now (spec and I1 record only). I3-3D can start now; it
   should settle m8 and the relay-marking source. T1-D can start drafting now but cannot be accepted
   before I3-2D (it needs the shape-version rule), and it takes M1 if T0 does not, plus m3 and m4.
1. **Section 4.2 and the 3,000-line cap.** Holds under the estimate above; worst case is
   `epistemic_emit_health.cpp`.
1. **T1-D as the place for the copiers.** Yes, with m4.
1. **I3-4a dependency after the token split.** Only I4-7b (and I3-3a, I3-2c2, V-D). Correct.

Tally: 0 BLOCKING, 3 MAJOR, 10 MINOR.

Verdict: FAIL

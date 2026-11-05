# T1 validation

T1 lands in the slices of its [accepted design](t1-design.md) (revision 2). Each slice records what
was built, what it fixed, its tests and its limits here.

## T1a: the refusal channel and the shared pre-flight

Source baseline: `8d80e07f`. Implemented from the design without a per-slice review. No persisted
layout changes; T1b1, T1b2 and T1c are not started.

- **Added.** `epistemic_refusal.{hpp,cpp}`: `TransferRoute`, `TransferCode` (only the codes T1a can
  produce), `RefusalDetail` and `TransferRefusal`, whose only constructor is `make`; texts are cut
  at 128 bytes on a UTF-8 boundary. `epistemic_preflight.{hpp,cpp}`: `discoverDimensions` (moved out
  of `restoreState`), `countOwnedCells` (the walk `epistemicCellsWithin` now delegates to),
  `checkResumePolicy`, `checkResumeBudget`, `ResumeLimits` and the apply-time
  `TransferRefusalError`. `upgrade_schema.*` gained `checkPredicateTransfer`,
  `predicateSignatureText` and `sameUpgradeSchema`.
- **Executor.** `resume` and both `upgrade` overloads return `TransferRefusal`. `preflightResume`
  and `preflightUpgrade` are public dry runs, and both overloads call the one `preflightUpgrade`
  (the road only switches the road-specific checks). `quiescence(pid)` returns a `QuiescenceReport`
  of counts. `setResumeGrantProbe` and `lastRefusal(pid)` are the grant probe and the apply-time
  backstop. Resume decides, after `fetchValue` and before `trustNode`, issue and `runtime_.resume`:
  missing program (`Runtime{NotAuthorized}`), policy and removal against the saved masters, then
  `PeakCopyBudget` and `WorkBudget` from the probe, so no pid exists on a refusal. `LocalHost`
  builds the authorizer and the probe from one `grantLimits`.
- **Order of checks.** Program, quiescence, active intention (schema road), policy, signature,
  removal, other schema refusals, populated state, budgets.
- **Reconciler road.** The by-value `LocalHost::upgrade` stays on it. It now compares policy (any
  predicate) and signature and removal (arbitrated or bound predicates), refuses populated state
  with `RouteCarriesNoEpistemicState` naming the other overload, runs the pre-flight before
  `retainProgram`, and refuses another schema under a retained reference with
  `ProgramReferenceReused`.
- **Fixes.** A changed policy under one explicit reference no longer yields a pid for an actor that
  dies in `init`. A bound, empty agent could be upgraded to a program that drops its predicate on
  either road, and the reconciler road never compared policy; both are refused. `agent_tui` prints
  `toString(refusal)` instead of an integer.
- **Tests.** New `tests/xuzz/vagent_epistemic_refusal_test.cpp` (20 cases): vocabulary, changed
  policy before a pid, no trace for five resume refusals, missing program decided first, residue
  after a runtime refusal, budgets (probe, settings, no probe, exact allowance), the apply-time
  backstop and `lastRefusal`, both dry runs, every schema-road code, the budgets from the grant,
  reconciler policy, removal on both roads, plain predicate still droppable, entry points agree,
  same reference, registries after a pre-flight refusal, and an ask in flight still carried by
  value. `AgentTuiTest.TheTuiPrintsTheTypedReason`. Existing T0 assertions edited to
  `.error().actor` or the code.
- **Result.** `vagent_actor_test`: 981 passed. `xuzz_test`: 3150 passed. Full `make` exits 0 and
  `make fuzz_epistemic_ingress` builds.
- **Limits.**
  - Issued-capability count and the trusted-node set are not observable through `LocalHost`, so the
    "no trace" and residue tests check pid, actor count, pump and the retained program only.
  - A populated schema-road upgrade with no translator is still `Runtime{ExecutionFailed}`;
    `NoTranslator` and the shape guard belong to T1b1 and T1c.
  - `PredicateIncompatible` on resume (the signature half) needs the master's `signature` field from
    T1b1; resume refuses policy and removal only.
  - The resume work and cell estimate (14 charges a cell) is the design's figure; T1c measures it.
  - `lastRefusal` entries are erased by `retire`; the executor has no `forget`.
  - A resume of a program that is not a `CompiledProgram` skips the pre-flight and relies on the
    `restoreState` backstop.

## T1b1: the shape header and the guard

Source baseline: `e0834640`. Implemented from the design without a per-slice review. This slice
defines shape 1, so nothing persisted before it is supported: a checkpoint holding P19 state and no
header refuses by number. No stored sample holds P19 state, so no fixture was regenerated.

- **Added.** `epistemic_shape.{hpp,cpp}`: `kEpistemicShapeVersion` (1), `EpistemicStateProbe`,
  `probeEpistemicState`, `hasTranslatableEpistemicState`, `TranslatorRegistry`, `programShape` and
  `checkShape`. `epistemic_emit_shape.cpp`: `Gen::mintShapeHeader`, one call at the end of
  `mintPredicates`. `TransferCode::UnknownShapeVersion` and `PolicyEncodingChanged`.
  `kPolicyEncoding` (the tag `canonicalPolicy` starts with), `canonicalPredicateSignature` and
  `predicateSignatures` in `upgrade_schema.*`.
- **The header.** Three cells (`EpistemicShape`, a `version` label, an Int64 scalar) at the head of
  the agent cell's `d.epistemic` rank, minted for a unit with an arbitrated predicate and for no
  other. Nothing copies the saved one: the executor reads its number and compares it with
  `programShape`, and the new program's instantiation has its own.
- **The master.** `mintPolicyMaster` writes `encoding` (the tag) and `signature` before `strategy`.
  Both are part of shape 1.
- **The guard.** `populated` is a profile on `d.trust`, tombstones included, or anything on
  `d.epistemic` that is not the header. `hasTranslatableEpistemicState` is the only place that
  decides, and `preflightUpgrade` uses it in place of the `linked(trust) || linked(epistemic)` test,
  so an arbitrating agent with no source yet upgrades on both roads without a translator.
- **Resume.** `checkResumeState` runs before the policy check: `ActiveIntention`, then
  `UnknownShapeVersion`. `checkResumePolicy` then reads per predicate: removal,
  `PolicyEncodingChanged` (the saved tag against `kPolicyEncoding`), `PolicyChanged`, and
  `PredicateIncompatible` from the master's `signature` against the schema's, which T1a could not
  do.
- **Intentions.** An arbitrating agent (header present, or the new program arbitrates) now refuses
  an active intention on resume and on the reconciler road as well. The schema road refuses any
  agent's, as before. A non-arbitrating agent resumes and reconciles with an intention.
- **Tests.** New `tests/xuzz/vagent_epistemic_shape_test.cpp` (12 cases): header only for an
  arbitrating program; the master's two fields; every row of the guard table; shape 2 and an
  unreadable header refusing on the schema road, the reconciler road and resume, with the numbers;
  an arbitrating agent with no profile upgrading on both roads and resuming; a non-arbitrating agent
  upgrading unchanged; the header cell being the program's own after resume and after upgrade;
  `PolicyEncodingChanged{saved, current}`; `PredicateIncompatible` on resume from the signature;
  `ActiveIntention` on every road and the non-arbitrating exception. The vocabulary test lists the
  two new codes.
- **Result.** With T1b1 alone, `vagent_actor_test`: 993 passed (981 before). `xuzz_test` passed
  apart from one benchmark timing under build load, which passed alone. The final counts for both
  slices are under T1b2.
- **Deviations.**
  - The stored signature is `vagent-sig-1 at(key Str, Float)`, the existing display text under a
    version tag, not the design's NUL-separated fields. One text then serves the comparison and the
    refusal that prints it, and it names name, arity, ordered types and key positions.
  - `Gen` does not decide a signature. `Compiler::setPredicateSignatures` hands it the ones the
    program's `UpgradeSchema` produced, so a type cannot be spelled two ways; a compile made outside
    `CompiledProgram` writes no `signature` field.
  - The new program's shape is read from its schema (`programShape`), which the compiler mirrors,
    not from its cells.
  - `TranslatorRegistry` has one fact: a registered seam carries shape 1. T1c replaces it with
    per-shape entries.
  - A saved shape the new program does not have, because it dropped every arbitrated predicate, is
    left to `PredicateRemoved`, which names the predicate, rather than reported as
    `UnknownShapeVersion`. The design's order would have hidden the removal behind the number.
- **Limits.**
  - The encoding is compared on resume only. On the upgrade roads the old arena's masters were made
    by this build, so the tag cannot differ before T1c's routine reads masters back.
  - `NoTranslator` is still `Runtime{ExecutionFailed}`; it belongs to T1c.

## T1b2: the admission gate and the health commit scope

Source baseline: `e0834640` plus T1b1. Implemented from the design without a per-slice review. No
persisted layout changes and no cells.

- **Lock order.** Ingress, then `healthMutex_`, then the post box. A re-grep of
  `agent_executor*.cpp` found no site that takes the ingress mutex while holding either of the other
  two; the order is written above `healthMutex_`.
- **The gate.** `Ingress::admission[pid]` holds `pausedUntil` and `swapping`. `acquireSwap` takes
  both mutexes in order and, in one scope, refuses `NotQuiescent{admission}` if `requirePaused` is
  not met or a swap is in progress, runs `quiescenceLocked`, runs `checkPredicateTransfer` against
  the bindings read under the held lock, and only then sets `swapping`. `SwapGate` is move-only,
  built only there, and clears the flag in its destructor. Both upgrade roads hold it from after the
  pre-flight to the end of the function, so the post-commit tail runs under it; the tail is caught
  and logged at WARN and the call returns success. `HostLocks` is the proof of locks that
  `quiescenceLocked` takes, so the advisory report and the gate share one computation.
- **The report.** `QuiescenceReport` gained `pendingHealth`, `inFlight`, `queued`, `mailbox`,
  `arbitrating` and `admission`. `firstBusy` orders them as the design's table does. The mailbox
  refuses only an arbitrating agent, on both roads, with its count; other agents are as before.
- **Admission tests.** `observe` tests in the first section (an early-out placed before the latch
  cache, so a closed door counts no babbler) and again at the top of the second section, in the
  scope of `posts_->push`; the refusal is not logged, charged or noted. `bindSource` and
  `retireSource` test at the top of their existing scope. `unbindSource` is ungated.
  `AdmissionClosed` is appended to `EpistemicRefusal` and is `Retryable`.
- **The health commit scope.** `healthAction` and `healthActionBatch` copy the grant's id and issuer
  out of the first scope (no pointer survives it), then take ingress and `healthMutex_` and, in
  order: admission, the grant found again by id, back-pressure, receipt, ticket, reset generation
  and charge, push. Every refusal precedes the ticket and nothing after it refuses.
- **Operator path.** `pauseAdmission(pid, ttl)` returns `std::expected<AgentExecutor *, ...>` and
  only extends; `resumeAdmission(pid)` is idempotent; `forgetIngress` drops the record.
  `LocalHost::upgradeQuiescing` pauses over its timeout, pumps until `quiet()`, upgrades with
  `requirePaused`, and reopens admission whatever happened; it refuses `NotQuiescent` if a pump runs
  no turn or the host's steady clock passes the deadline. `vagent.admission_pause_ms` (default
  30,000, range 1,000 to 600,000) is `AgentSettings::admissionPause`, a `defaultSettingSpecs()`
  entry, and in the expected list of `system_docs_test.cpp`.
- **Seam.** `setSwapObserver` with `AfterGrantMatch`, `BetweenObserveSections`, `AfterCommit` and
  `DuringDrain`; one test of an empty function in production.
- **Tests.** New `tests/xuzz/vagent_epistemic_admission_test.cpp` (18 cases): a post before the gate
  is counted and one raced between `observe`'s sections is refused, unlogged and uncharged (bind,
  retire refused, unbind not); the pause refuses every admitting call and queued readings still run;
  a health action and a batch raced at `AfterGrantMatch` leak no ticket, with more refusals than the
  per-grant cap; a queued ticket in the report and a checkpoint refused with it; `queued` and
  `inFlight` empty after a drain; charges, the outcome ring and the audit numbering across a swap;
  the tail under the gate (`Swapping`, latch cache already cleared, posts refused); admission
  reopening after a refusal, a throwing callback, a commit and a throwing tail; the pause kept
  across an upgrade; the mailbox for an arbitrating agent and not for a plain one; the pause lapsing
  by itself, never shortening, and renewed across a drain longer than the default ttl;
  `requirePaused` after a lapse; `upgradeQuiescing` draining and reopening on a refusal. Two tests
  use real threads parked on `std::latch` at `BetweenObserveSections` and `AfterGrantMatch` while
  the swap runs, and release them at `DuringDrain`, so the producer's second section meets
  `swapping` deterministically. `VagentSettingsTest.TheAdmissionPauseLoadsWithinItsRange`.
- **Result.** `vagent_actor_test`: 1012 passed. `xuzz_test`: 3181 passed (it compiles every
  `tests/xuzz` file, so it holds the 31 new cases too). Full `make` exits 0 and
  `make fuzz_epistemic_ingress` builds. The 30 new shape and admission cases passed 50 times in a
  row under `--gtest_repeat=50`, and the two real-thread cases 200 times.
- **Deviations.**
  - `Ingress::Admission` has no `pauseOpenedBy`: `requirePaused` only needs to know the pause is in
    force, and `upgradeQuiescing` always reopens what it opened.
  - The non-thread race tests close the door with `pauseAdmission` at the observer point, not a
    swap: the check is one function for both, and the threaded tests cover the real `swapping` flag.
  - The `pendingHealth` count is tested in the report, but `NotQuiescent{pendingHealth}` as the
    first refusal cannot be reached: the same ticket's item is in the post box, which the report
    names first. A ticket outliving its item exists only for an incomplete batch.
  - There is no test that places a bind between the pre-flight and the gate: the two are adjacent
    and no observer point sits between them. The comparison runs again under the locks, and T1a's
    tests cover its result.
  - `LocalHost::upgradeQuiescing` refuses on a pump that ran no turn instead of waiting for the
    deadline, since the host has one thread.
- **Limits.**
  - The mesh mailbox is counted, not closed; `ClusterNode::receive` sharing the pump's thread is
    still the I3-4 review's finding.
  - A percept posted during a swap is consumed by the new program, as the design records.
  - Nothing here drives `upgradeQuiescing` from the `vagent` command line; that is W1's.

## T1c: the translation routine, the full-key matcher and the registry

Source baseline: T1b2. Implemented from the design without a per-slice review. No persisted layout
changes.

- **Added.** `epistemic_translate.{hpp,cpp}`: `planTranslation` (read-only, the pre-flight),
  `applyTranslation` (the only writer, four passes over one old-to-new map), `PredicateKey`,
  `decodePolicy`, `PredicateMatcher<KeyDigest>`, `checkPredicateKeys`, `epistemicStateDigest` and
  the default translator. `epistemic_translate_registry.{hpp,cpp}`: `OwnedStructure` (14 entries,
  each with a `populate` hook), `notOwnedDimensions`, `hostState` and the three `owns*` queries.
  `tools/check-translator-coverage.py` and the make target `test/translator-coverage`.
  `TransferCode` gained `NoTranslator`, `UnregisteredOwnedStructure`, `ReplayStateUnavailable` and
  `StateInconsistent`.
- **The passes.** Pass 0 is `planTranslation`: shape and translator, then every predicate key
  decoded from the graph, then the profiles (token, fields and dimensions against the registry,
  incarnation, streams, dedup entries, clone chain), then every record (dimensions, fields, and that
  its `profile` value is a clone occurrence of some profile), then the cell, scratch and work
  budgets. Pass 1 copies each profile, its name cells, field values and clone chain, and appends it
  to `d.trust`. Pass 2 copies each record to the mapped support or profile in its old order. Pass 3
  checks that every edge of every cell the routine made has both ends mapped and the new cell holds
  the same edge, and that it copied what the plan counted. Pass 4 clears `last_receive` and
  `last_contact` on a resume. Stored value bits move as they are.
- **The three copiers.** `restoreState` no longer copies profiles, compares policy or follows
  `d.epistemic` and `d.clone`; it copies clauses with their supports and calls the routine through
  `translateEpistemic`. The schema road's `UpgradeGraph` already skipped both dimensions; it now
  always admits them and hands the seam its map. The reconciler road is unchanged: populated state
  stays `RouteCarriesNoEpistemicState`.
- **The refusal that lifted.** A populated schema-road upgrade, which T0 and T1a refused, now
  upgrades with the default translator and keeps its profiles, latches, health records and
  histories. The tests that pinned the refusal now pin the success, and the reconciler road's
  refusal. The seam's signature is
  `(TranslationPlan, ApplyRequest) -> expected<TranslationReport, TransferRefusal>`; an empty one
  gives `NoTranslator`. No README text named the refusal.
- **Measured work.** On a 22,156-cell resume the routine charged 328,857 instructions (14.84 a
  cell); on a 1,062-cell in-place upgrade, 15,648 (14.73). The design's 14 was low, so
  `kTransferWorkPerCell` is 16. A full 26,112-cell graph is 417,792, 4.2% of the default grant and
  42% of the TUI's 1,000,000, under the half-grant rule. Pre-flight wall time on the full resume was
  about 1.7 s, almost all of it the checkpoint decode in `fetchImage`; the plan itself runs twice on
  resume (pre-flight and `restoreState`) and spends no grant.
- **Tests.** New `tests/xuzz/vagent_epistemic_translate_test.cpp` (18 cases): the registry's
  consistency; policy decode and round trip with every optional present and absent; the forced
  collision; every registered structure populated on a live profile and compared by digest across
  the source, a resume and an in-place upgrade; the latch and a reason on both roads; liveness
  stamps by disposition; a `-0.0` on the upgrade road; 24 seeded random ingestion runs of up to 40
  events (readings, rebinds, retire, isolate, clear); a failing translator rolling back; no
  translator on both roads; the apply-time resume failure with its stored refusal; a record without
  a profile; a missing `streams`; an unregistered dimension and an unregistered profile field; D3
  (an unchanged policy keeps the binding, a changed one is refused and the binding still accepts);
  the full-budget resume with its measurement; a measured in-place upgrade; and the coverage script
  failing on an uncovered field constant and on an uncovered dimension.
- **Result.** `vagent_actor_test`: 1031 passed. `xuzz_test`: 3200 passed (one benchmark timing
  flaked once under build load and passed alone). Full `make` exits 0 and
  `make fuzz_epistemic_ingress` builds.
- **Deviations.**
  - The seam takes the plan as well as the request, and the request carries `OwnedGraphDims` for the
    old graph (optional refs) and a plain `DimBinding` for the new one, since the old graph may lack
    a dimension and the new one cannot.
  - The registry lists live in `epistemic_translate_registry.cpp`, not in the ABI headers: those
    hold constants only and `populate` needs the wire layer. The script reads the constants' names
    out of the registry file. It treats `kProfile*`, `kObs*`, `kReason*` and `kAction*` constants as
    fields except the content values (statuses, health states, reason kinds and origins,
    `retired_by` causes).
  - The `health` and `health_fraction` stamps are record fields, not profile health scalars as the
    design's table lists them; the registry has them under `record.fields`.
  - Required replay state is the incarnation always, and `streams` and `dedup` unless the profile is
    a tombstone. An advance writes fresh windows and a retirement clears them with `retired` set, so
    requiring them of a tombstone refused real state.
  - `Migratable` and the `Incompatible{part}` outcome type are not declared; the matcher answers
    with an index or nothing and `checkPredicateKeys` classifies a miss as T1a's codes. The matcher
    takes its digest as a template parameter, as designed.
  - `NoTranslator` is decided in the plan, so the pre-flight, `restoreState` and the schema road
    agree; the old `Runtime{ExecutionFailed}` for that case is gone.
  - The reconciler road gets its policy, signature and removal comparison from T1a's
    `checkPredicateTransfer`, not from the graph decode; only resume and the schema road decode.
  - A checkpoint is encoded with canonical doubles and types numeric-looking text, so `-0.0` reaches
    a resume as `0.0` and a field named `"0"` comes back as a number. The routine moves whatever
    bits it is handed; the digest hashes names as text for that reason. `DoublesCrossByBitPattern`
    tests the upgrade road, which reads the live arena.
  - The destination arena interns each distinct text once. An upgrade grant caps the new arena's
    scratch at 32 KiB (`MeshActorRuntime::upgrade`), which the design's cell bound did not account
    for: at 22,156 cells the clauses and supports fill it before the translator runs, and the swap
    fails with `Runtime{ResourceLimit}`, leaving the actor untouched, where a resume of the same
    image succeeds. The plan refuses earlier, as `PeakCopyBudget{subject = "scratch bytes"}`, when
    the distinct texts alone exceed the grant. The full-budget test therefore resumes at the bound
    and upgrades a smaller graph.
- **Limits.**
  - The in-place road's real ceiling is the 32 KiB scratch, not 26,112 cells. Raising it is a change
    to the mesh runtime that needs its own approval; an operator whose agent is that large resumes
    instead.
  - Pass 3 verifies edges only; tokens and intentions are copied there by later slices.
  - The matcher decodes the policy but the feature schema, calibration and model epochs of the full
    key arrive with I4-1b and I5-3.
  - `TheCoverageScriptFailsOnAnUncoveredFieldOrDimension` runs the script from the repository root
    and skips elsewhere.
  - There is no test that fails the `on Upgrade` hook after the routine ran with populated state;
    the existing hook-failure test covers the rollback for an agent without it.
  - Resume of a program that is not a `CompiledProgram` carries no P19 state and refuses populated
    state as `NoTranslator`.

### T1 as a whole

Complete: the typed refusal channel on resume and both upgrade roads (T1a), the shape header and the
guard (T1b1), the admission gate, the quiescence report and the operator path (T1b2), and the
routine, the full-key matcher, the digest and the owned-structure registry with its script (T1c). A
populated agent resumes and upgrades in place with its profiles, histories, latches and health
records, and every cause of refusal has a code.

Knowingly open for the M1 review: the 32 KiB scratch ceiling on the in-place road; the reconciler
road still refuses populated state rather than carrying it; no `Migratable` outcome, no token or
intention copy and no continuation rules (T2 and later slices); the `ClusterNode::receive` thread
question for the mesh mailbox (I3-4); the resume pre-flight reads the whole image, so its cost is
the decode time; and the registry's `kHostState` text is prose, checked only by being the single
source of the user guide's table.

Lead verification (2026-11-05): full build, `vagent_actor_test` 1031 and `xuzz_test` 3200 pass.
One full run, which overlapped another process using the same `build/xdg` state, failed
`Translate.ResumeAndUpgradeAgreeOverRandomIngestion`; it then passed 10 repeats alone, 4 shuffled
runs of the 897 vagent and epistemic tests and a clean full run. The failure text was not captured.
It is carried into the M1 review as an unexplained intermittent.

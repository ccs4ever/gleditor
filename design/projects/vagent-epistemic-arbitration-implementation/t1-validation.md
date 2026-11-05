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

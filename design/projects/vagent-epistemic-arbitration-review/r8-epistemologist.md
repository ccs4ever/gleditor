# Round 8 critique: Epistemologist (P19)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 1 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R7 reported no open epistemic findings against its focused v1.4 candidate. That review preceded
  the integrated runtime baseline and does not establish upgrade/reconciliation conformance; no
  prior R7 blocker remains to disposition.

## BLOCKING

None.

## MAJOR

### 1. (§2.1, lines 134–139) Upgrade migration can carry facts across changed predicate meaning

**Problem:** P19 correctly requires versioned calibration/model meaning and exact target-key checks,
but its migration instructions do not constrain the integrated reconciler's existing belief
migration. `restoreState` finds destination predicates by name alone and copies every old clause
under the match. It does not compare arity, argument types, key layout, frame/unit schema, or a
predicate semantic epoch. If a program upgrade reuses a predicate name with changed meaning, old
observations and their source support remain available to the new program as if they belonged to its
new declaration. This undermines P19's versioned meaning and can make otherwise exact model-epoch
checks moot for belief facts. The risk is not hypothetical: P17 migration is already implemented in
the R8 baseline, while the P19.1 proposal names runtime ABI/executor/reconciler integration but does
not state a compatibility rule for carrying beliefs across upgrades.

**Evidence:** P19 says “A schema/model version change invalidates incompatible weights,
normalization state and cached features explicitly” and that “P19.1 reconciliation migrates profiles
first, then observations and source clone occurrences” (`r8-input-v1.4.txt:134–139`). The integrated
migration instead says predicates are matched by name and clauses copied
(`apps/common/xanadu/vagent/runtime/agent_executor.cpp:3233–3247`): it obtains fresh predicates,
compares only `text(arena, candidate) == name`, then clones every old clause beneath that predicate.
The belief compiler derives argument types and key count from the new declaration
(`apps/common/xanadu/vagent/runtime/compiler.cpp:3945–3978`), so matching names do not establish
semantic compatibility. Existing resume coverage verifies that belief clauses survive
checkpoint/resume (`tests/xuzz/vagent_resume_test.cpp:130–147, 182–198`); upgrade tests exercise the
upgrade hook and field lifecycle, but do not test predicate-schema changes
(`tests/xuzz/vagent_upgrade_test.cpp:243–287`).

**Fix:** Extend P19.1's migration contract: persist a stable predicate signature/schema epoch with
every assertion and observation; migrate support only when the old and new signatures, key layout,
frame/unit schema, and relevant model/calibration keys are compatible. On incompatible or absent
declarations, retain the old versioned evidence as inspectable retired state or explicitly expire
it; never attach it to the new predicate solely by name. Define behavior for unchanged predicates
whose dependent domain/model epoch changes, and add upgrade tests for same-name arity/type/key/frame
changes, compatible changes, and source/evidence identity preservation. Price the retained retired
state and migration work under existing actor budgets.

## MINOR

None.

## R8 integration distinction

The integrated runtime implements source-aware ground-belief coexistence by coalescing an identical
ground term into one clause with a `d.source` rank of asserting names; it does not implement P19's
observation ledger or authenticated observation identities. The compiler comments and code make the
representation explicit (`apps/common/xanadu/vagent/runtime/compiler.cpp:3945–3955, 4056–4105`), and
the tell adapter takes the implicit source from `envelope.from`
(`apps/common/xanadu/vagent/runtime/compiler.cpp:4690–4750`). P19's stable event IDs, authenticated
identity/incarnation, keyed calibration records, active-label revisions, retained defeaters, and
exact-epoch completion checks remain proposed future state (`r8-input-v1.4.txt:81–110, 625–691`);
the current name-based support must not be treated as satisfying those guarantees. The R8 brief
identifies the bounded actor frontend as distinct from the runtime frontend and says not to import
absent P18 semantics by assumption. I found no actor SLD execution in the reviewed paths; the
current belief implementation and tests exercise ground facts and matching, not P19 arbitration or
rule-proof provenance. These are explicit prerequisites/extensions rather than additional findings
here.

VERDICT: NOT CONVERGED

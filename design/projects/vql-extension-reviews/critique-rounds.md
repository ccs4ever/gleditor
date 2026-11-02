# Iterated critique of the VQL improvements

The user requested up to five rounds with the original five Sol medium reviewers, stopping only when
a complete round yields no new actionable findings and all five accept the improvement set. Reviews
examine the uncommitted V16.1 repairs, performance spike and full workflow proposal on top of
`7617d3f0`. A proposed contract is not an implemented feature.

The coordinator runs the five original reviewers in batches within the three-subagent concurrency
limit. Each later round receives the shared findings and disposition from the preceding round.
Silence or an unavailable reviewer does not establish agreement.

## Round 1

All five original reviewers completed their independent critique. Reports:
[Purist](round-1-purist.md), [writer](round-1-writer.md), [Vortex](round-1-vortex.md),
[language](round-1-language.md), [workflow user](round-1-workflow.md).

| ID  | Finding                                                                            | Disposition                                                                                                                       |
| --- | ---------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------- |
| P1  | Bulk shape assignment silently drops reciprocal relationships.                     | Fixed: preflight both endpoint claims; reject incompatible batch before masking; regression.                                      |
| P2  | Nested shape RHS fabricates Boolean content.                                       | Fixed: reject unsupported shape values before edits, including hidden function results; regression; empty-mask fixture corrected. |
| P3  | Private perspective diagram exceeds valence and promises global private discovery. | Replaced with observer-local proxies, owned reciprocal edges, stable external references and covered discovery.                   |
| W1  | Claimed checked/inferred effects are unbuilt.                                      | Corrected qualifier status and examples; future effect enforcement remains proposed.                                              |
| W2  | Claimed lazy traversal/early `[1]` termination is unbuilt.                         | Distinguished proposed lowering from vector materialization and postfilter ranges.                                                |
| W3  | Cache section still promises implemented reachability GC.                          | Explicit proposed lifetime policy; removed stale example references and current reclamation claim.                                |
| W4  | Historical baseline and remaining grammar status were stale.                       | Historical issue register and baseline labeled; empty shapes no longer listed as outstanding.                                     |
| W5  | Snapshot roots lack durable structural correspondence.                             | Added proposed queryable source/result mapping, source observation/version and clone authority; no current lineage API claimed.   |
| V1  | Exceptions leak UDF/lambda lexical bindings.                                       | Fixed: RAII lexical binding/functor restoration and regressions.                                                                  |
| V2  | Compiled bare creation bypasses per-New reuse guard.                               | Preflight refusal before emission and regressions; dimension-attached creation retained.                                          |
| V3  | Logical capability scope omits resolved target changes.                            | Pin connection/adapter/policy editions; recheck immediately before I/O/redirect; refuse ungranted credential forwarding.          |
| L1  | Empty path invokes downstream UDF/lambda once.                                     | Fixed: terminate empty downstream paths/functors while retaining contextual top-level calls; regression.                          |
| L2  | Empty final statement resurrects earlier result.                                   | Fixed: each final statement result replaces the earlier result even when empty; regression.                                       |
| L3  | Earlier parameter shadows later explicit argument evaluation.                      | Fixed: caller-scope explicit cell/functor capture, defaults see earlier callee parameters; regressions.                           |
| L4  | Excess UDF arguments silently disappear.                                           | Fixed: preflight excess-arity refusal; regression. Missing-argument matching policy retained.                                     |
| U1  | Cancellation discards late evidence of external effects.                           | Fence stale continuation/arena writes; retain authenticated late receipts without resuming downstream work.                       |
| U2  | All pages fetched conflates coherent collection coverage.                          | Proposed scan record and stability/mixed-time policy, with restart and changing-page acceptance cases.                            |
| U3  | Offline merged drafts can inherit stale dispatch authorization.                    | Merged intent is a new revision; preserve alternatives and require exact-result authorization.                                    |

Round 1 did not quiesce. Larger workflow features above remain design contracts; only prerequisite
execution defects are implementation changes.

## Round 2

R1 source fixes built successfully; 141 VQL tests passed headlessly. Reports:
[Purist](round-2-purist.md), [writer](round-2-writer.md), [Vortex](round-2-vortex.md),
[language](round-2-language.md), [workflow user](round-2-workflow.md).

| ID  | Finding                                                                    | Disposition                                                                                                                                                          |
| --- | -------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| P4  | Spec says all perspective syntax refuses, but direct arena prototype runs. | Documented tested transient overlays/fallback separately from unbuilt durable private proxies; matrix updated.                                                       |
| V4  | Exceptions leak `activePerspective_` from blocks/qualified steps.          | Fixed: scoped RAII restores named perspective on nested/block/qualified-step success and failure; 29 semantic/perspective tests pass.                                |
| U4  | Two recovering executors can submit the same pending attempt.              | Proposed authoritative admission domain, atomic executor claim, unavailable-owner policy and reconciliation after lease expiry; concurrent recovery acceptance case. |
| L5  | Unknown scalar-context calls fabricate empty text instead of refusing.     | Fixed: scalar unknown-name/arity refusal and nested/UDF/assignment regressions; 145 VQL tests pass.                                                                  |

Writer identified no new actionable findings in its documentation domain. Round 2 has not quiesced.

## Round 3

The writer, Purist, Vortex and workflow user report no new actionable findings in their domains. The
language reviewer found one remaining composition path, so this round did not quiesce.

| ID  | Finding                                                                                                                        | Disposition                                                                                                                                                                           |
| --- | ------------------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| L6  | A macro group invokes later inner steps after its stream becomes empty, reviving results or running a body without candidates. | Added inner-step termination guard and regressions for nested macros, lambdas, functions, root selectors and repetition forms; verified: build and all 146 VQL tests pass headlessly. |

Reports: [Purist](round-3-purist.md), [writer](round-3-writer.md), [Vortex](round-3-vortex.md),
[workflow user](round-3-workflow.md). Language report pending.

## Round 4

**Quiescent: all five original Sol medium reviewers reported no new material actionable findings.**
Stopped after four rounds under the user's early-stopping rule; round five was not needed.

| Reviewer                   | Final acceptance               |
| -------------------------- | ------------------------------ |
| ZigZag/Xanadu Purist       | [Round 4](round-4-purist.md)   |
| Technical writer           | [Round 4](round-4-writer.md)   |
| Vortex expert              | [Round 4](round-4-vortex.md)   |
| Language/ergonomics expert | [Round 4](round-4-language.md) |
| Workflow user              | [Round 4](round-4-workflow.md) |

Acceptance covers the prerequisite repairs and completeness of the proposed extension contracts at
the reviewed scope. It does not implement or certify the external integrations. Explicit remaining
design choices in proposal §9, exact-value/effect/strict-validation work, reusable compiled
allocation frames and transient perspective limitations remain visible rather than treated as
completed features.

## Final verification

- `make -j$(nproc) xuzz_test vquery vqueryc vql-spike vql-spike-abi` succeeded.
- All **264 tests from 28 suites** passed with filter `VQL*:VQuery*:Vortex*:*Lens*`, headless and
  with isolated XDG data/config/cache. This includes all 146 VQL tests.
- The rebuilt spike, 1000 cells/two hop dimensions, three warmups and 21 repetitions, passed output,
  allocation-probe and zero arena-growth checks.
  [Post-critique raw run](../../measurements/vql-spike-post-critique-2026-10-21.txt).
- Changed C++ and Markdown formatting/lint, new local links and whitespace are checked separately
  from repository-wide gates. The earlier full format/lint attempts hit pre-existing untouched UI
  formatting and font-literal policy failures; those are not passing gates.

No external service action was dispatched. Changes remain uncommitted.

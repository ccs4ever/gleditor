# Round 4 — Vortex/runtime critique

**Quiescent at the reviewed scope: no new material actionable findings.** I accept the combined
prerequisite repairs and proposed host/workflow architecture. This review does not certify a
complete language, unbuilt integrations, or tests I did not run.

Read the accumulated R1–R3 findings/dispositions, L6 report, current macro evaluator/regression,
compiler preflight/append guard, prior lexical/perspective scope fixes, and revised async
admission/replay contracts. L6's inner-step loop now stops on an empty stream; its regression covers
direct/nested macro groups, downstream UDF/lambda allocation, attempted root regrounding and
repetition. Optional/zero-repeat identity behavior is explicitly preserved. The reviewer reports all
146 VQL tests passing; I did not rerun them this round.

The runtime boundaries remain coherent: exceptions restore lexical maps and ambient perspectives;
unsupported compilation is rejected before emission; compiled creation stays a bounded
dimension-attached subset rather than reusable allocation frames. These repairs do not promise
rollback of arena edits preceding a later failure, a complete static effect system or strict
external-action validation.

The proposed host boundary preserves context ownership and arena lifetime through owned completion
values, stable serialized identities, bounded demand and source leases. Cancellation fences stale
continuations while retaining late authenticated effect receipts. Exact intent/configuration
revisions, resolved-target checks and authoritative atomic executor admission prevent accidental
privilege transfer or ordinary concurrent redispatch. Lost receipts and expired leases retain
unknown-outcome semantics; message/effect-boundary checkpoints replay recorded inputs and re-admit
current authority.

C++ enforcement and protocol/OS adapters remain separate from reusable Vortex orchestration policy.
Controlled crash/cancellation/revocation/concurrent-recovery tests and interface-based evidence
journeys remain future acceptance obligations, rather than delivered service claims. No additional
runtime requirement is needed solely to extend the review.

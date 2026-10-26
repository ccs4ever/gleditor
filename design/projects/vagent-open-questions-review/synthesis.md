# Synthesis: revise before incorporating

All three Luna reviewers support the overall direction and the deliberate first-release limits. None
reported a BLOCKING finding. The technical writer and language expert say revise before
incorporating; the Vortex expert identifies concrete missing contracts in the proposed extensions.
Their agreement is evidence about the recommendations, not approval or implementation validation.

## Changes needed before incorporation

1. **U-A1: specify roots and lifetimes, not just a graph copy.** Inventory the actor's separate
   `cursor` and `stateRoot`, VM roots, program/library registrations, arena attachments and borrowed
   span readers, and adapter/callback references. Rebuild or map them all before installation. A
   state-only copy can leave a valid-looking actor with a cursor into its discarded context. Define
   the equivalence test using identical scripted inputs, a manual clock and mock effect outcomes,
   comparing semantic state and staged output after each turn. Add a distinct reclamation entry to
   §1's mechanism table while retaining the existing checkpoint/migration entry: local context
   replacement and portable checkpointing are different mechanisms.

1. **U-A3: specify durable content identity and lease lifecycle.** Pinning a C++ reader is not
   enough across checkpoint/resume or context replacement. State must retain a stable verified
   content reference; define how readers are reacquired, transferred and released, and what a
   missing source means after restoration. Retain typed handles and asynchronous bounded fetch as
   the direction; keep the lifecycle an explicit prerequisite to adoption.

1. **U-A9: define StoreWriter before treating delegated writes as settled.** The specification names
   the capacity but does not define its surface or complete authority contract. Specify target
   identity, allowed operations, grant issuer and signer, version preconditions, conflict results,
   provenance, and when revocation affects staged or in-flight writes. Clarifying R8's proposed
   interpretation can happen early; permitting such writes remains a pending semantic decision. For
   writes whose completion may be lost across a crash or retry, define stable operation ids and
   reconciliation or atomic deduplication with the write. Mesh message redelivery alone is not
   evidence that receiver deduplication fails; it also does not establish crash-safe, exactly-once
   StoreWriter effects. The future effect protocol must state its actual guarantee.

1. **U-A8: name the replay unit.** A per-agent trace can replay that agent's recorded inputs without
   reproducing a node's entire schedule. Whole-node equivalence additionally needs the actual
   cross-actor execution order and relevant host/lifecycle decisions. State which guarantee is
   offered; do not require global recording for a deliberately per-agent debugging facility.

1. **U-A5 and U-A11: preserve existing ordering and finish future arbitration.** The current
   `continue(i)` pseudocode checks dropping before success, so describe that existing precedence
   rather than present it as wholly unresolved. Keep FIFO wire delivery. Before adding host
   priority, specify mailbox/host-delivery/ready-intention arbitration under P4, overload bounds and
   fairness. Neither handler priority nor urgent ingress interrupts a running turn.

1. **U-A13: demonstrate the library approach.** A worked Contract-Net example should show
   conversation-id allocation, per-participant correlation, conversation state, timeout,
   cancellation and late/duplicate messages using existing constructs. This validates library
   usability while dedicated protocol syntax remains deferred.

1. **Make decision status and incorporation edits uniform.** For each U-A, separate the recommended
   first-release boundary, the semantic design still required, and the sections changed now versus
   only if an extension is adopted. Existing approval of P1–P14 covers none of the new proposals.
   Avoid calling every U-A1/U-A3/U-A8 detail a core change in advance: price changes to existing
   core only where implementation grounding shows that new adapter/compiler code is insufficient.

## Consolidated dispositions

These dispositions assess proposal readiness; they do not adopt the recommendations.

| Question | Disposition after critique                                                         |
| -------- | ---------------------------------------------------------------------------------- |
| U-A1     | Revise root/dependency inventory, scripted equivalence gate and mechanism status.  |
| U-A2     | Carry forward single-owner release boundary; defer cross-owner admission.          |
| U-A3     | Carry forward handle direction; specify lease lifecycle before adoption.           |
| U-A4     | Carry forward hosted relay; defer hostless transport semantics.                    |
| U-A5     | Carry forward single-minded default; state existing drop-first order.              |
| U-A6     | Carry forward canonical naming direction; defer family migration.                  |
| U-A7     | Carry forward restricted suspension and useful rewrite diagnostics.                |
| U-A8     | Carry forward debug replay direction; specify replay unit; defer durable recovery. |
| U-A9     | Revise full write/authority contract; retain R8 interpretation as pending.         |
| U-A10    | Carry forward deterministic harness; use it for controlled acceptance traces.      |
| U-A11    | Carry forward FIFO correction; specify P4 arbitration before host priority.        |
| U-A12    | Retain fixed selectors initially; defer bounded plan-ranking extension.            |
| U-A13    | Defer syntax; demonstrate typed library conversation semantics first.              |
| U-A14    | Defer optimization until private baseline measurements and isolation tests.        |

## Recommended next revision

First clarify first-release boundaries, existing drop/success order, and the harness contract. Then
revise reclamation roots/tests, content leases and replay scope. Write a standalone StoreWriter
contract before changing the R8 persistence rule. Add the protocol example and uniform per-question
incorporation instructions. Measure the private-program baseline before pursuing shared programs.

The individual critiques contain the original file/line references and reviewer-specific reasoning.
This synthesis narrows two claims: context replacement supplements checkpointing rather than
replacing its §1 entry, and retry-safe StoreWriter behavior requires a future effect contract rather
than an inference that current message deduplication is broken.

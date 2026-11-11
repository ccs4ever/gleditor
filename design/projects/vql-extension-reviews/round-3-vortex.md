# Round 3 — Vortex/runtime critique

**Quiescent at the reviewed scope: no new material actionable findings.** I accept the combined
prerequisite repairs and the proposed workflow/runtime boundary, subject to the coordinator's final
build/test results. This is not certification of unbuilt integrations, a full language
implementation, or arbitrary runtime behavior.

Reviewed the R1/R2 shared ledger and peer findings, revised execution/conformance boundaries,
current lexical/perspective guards, L5 scalar-call refusal and regressions, and the complete
host/action/replay ownership contracts. No executions or edits this round.

V1 is addressed with RAII restoration of both cell and functor environments across argument/default
evaluation, body execution and return checks. Explicit argument snapshots preserve caller bindings;
defaults deliberately use earlier callee parameters. V4 now scopes the ambient perspective in both
block and qualified-step execution; its inspected regression checks exceptional/nested restoration
and subsequent canonical editing. I personally ran 29 focused semantic/perspective tests when
implementing V4, before the later L5 change; that result must not be represented as verification of
the newer source.

V2's compiler preflight rejects bare creation before arena/opcode/cursor emission. The retained
dimension-attached creation subset and per-New append guard are clearly bounded rather than
advertised as reusable allocation frames. L5 now throws a named scalar-function diagnostic instead
of manufacturing empty text; explicit contains/count arity checks precede argument evaluation. Its
newly added nested, UDF, arity and assignment tests are inspected evidence only until the running
build/tests complete. Deliberate empty-variable scalar rendering remains separate from
unknown-function refusal.

The advanced design now states the required runtime invariants coherently: owner-thread arena
mutation, owned values/stable serialized identities across workers, bounded demand and lifetime
leases, stale-continuation fencing with a durable late-receipt inbox, effective-target/credential
checks immediately before I/O, and exact intent/connection/adapter/policy revisions. Authoritative
atomic dispatch admission prevents independent ordinary execution by concurrent recoverers; a lease
timeout does not prove nonexecution. Replay consumes retained inputs/receipts without dispatch;
recovery checkpoints message/effect boundaries and re-admits authority. External outcomes can remain
unknown.

Those mechanisms remain proposed host/runtime work. Their acceptance still requires the stated
controlled-source, crash, cancellation, revocation, concurrent-recovery and offline-reopen tests.
C++ owns enforcement and OS/protocol boundaries; reusable Vortex routines own orchestration policy.
No additional mechanism is needed solely to prolong this review.

# Workflow-user critique, round 2

Reviewed the full updated workflow proposal, shared R1 ledger and all four peer R1 reports. R1 U1–U3
are addressed at the proposal level: late receipts survive cancelled continuations, page scans
expose coherence separately from transport completion, and merged drafts require authorization for
their new revision. Resolved-target checks and snapshot correspondence also close the stated related
gaps. The six requested source domains and annotatable pending/error/gap states remain covered.

One new actionable contract remains; this round is **not quiescent** from the workflow perspective.

## U4 — Concurrent recovery can dispatch one logical attempt twice

**Evidence:** §4.2 requires a durable dispatch record before execution and associates retries with
an intent. §4.3 re-admits capabilities on restart and allows scheduled runs to run concurrently.
Neither specifies how two hosts/processes recovering the same workflow coordinate ownership of one
pending attempt. Exact intent authorization permits an operation; it does not establish which
dispatcher owns it. The new offline-merge rule protects changed drafts, but two devices can agree on
the identical revision and still both send its queued mail or run its command.

**Concrete fix:** declare the dispatch-admission domain and execution ownership for each
workflow/run. Within a shared authoritative dispatch journal, an attempt's transition from pending
to admitted must be atomic and identify the executor/generation. A second recovery observes that
claim and reconciles rather than independently submitting. Where devices have divergent offline
journals or cannot obtain the owning authority, default to awaiting coordination; explicitly
independent submissions must have distinct attempts and user-visible duplication risk. A
timeout/expired lease does not prove the first executor never reached the service, so taking over
must follow the existing unknown-outcome policy. If admission storage is unavailable before the
dispatch record becomes durable, fail before external I/O rather than executing unaudited work.

This is a local/shared-workflow admission contract, not a promise of external exactly-once delivery.
Remote execution followed by a lost receipt still uses §4.2's reconciliation/idempotency rules.

**Acceptance:** queue one exact mail intent, reopen the same run simultaneously on two authorized
executors, and release both at the admission boundary. Only one may begin the ordinary attempt.
Disconnect its owner immediately after remote acceptance; the other must show ownership/outcome
uncertainty and must not resend merely because the lease expires. A separately requested intentional
second send remains possible as another explicit attempt.

No implementation or live service behavior is certified by this review. Other implementation-level
R1 findings remain with their original reviewers.

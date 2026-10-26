# vagent v3.8 incorporation review — codebase expert, R4

## Verdict: CONVERGED

I reviewed the frozen R4 input (`r4-input.txt`; byte-identical to
`design/vagent-agent-language.md`), the prior incorporation findings and fixes, all U-A1–U-A14
resolutions, and relevant Vortex, ZigZag and mesh runtime code. I found no actionable incorporation
errors. The target remains a specification, not an implementation; no deferred extension is required
for approval here.

## Closure of earlier findings and R4 additions

- The status map rows are well formed. R1’s §14 determinism correction remains: the full selected
  work item and turn time are inputs, with program, runtime/scheduler, grant/limits and settings
  fixed. Ready/deadline cell contents are distinguished from external scheduling.
- The §6.5 source-local selection rules are separated from cross-source P4 arbitration. §11.1
  requires a deterministic, versioned baseline policy, finite starvation-free service for admitted
  host work, ready intentions and due deadlines under mailbox load, and a finite service bound in
  actor-service turns rather than a wall-clock guarantee. §18.11 correctly keeps this as ordinary P4
  work while deferring urgency changes. The current `pumpOne` dequeues the mailbox head before
  calling the executor, as stated; runtime fairness claims remain conditional on the proposed P7/P8
  work.
- The §12.6 rename to `plan wait_event` fixes the R3 reserved-keyword parse error. The yield prose
  now says that `yield` makes an intention ready without promising an external message’s precedence.
- The twenty-three isolated §12.6 units are consistent with the specified grammar and semantics. In
  particular, the new `for`/`while` example uses a list and a pure loop guard and has the stated
  final value; bounded `return` and method contracts in the helper example match §§8.2–8.3 and the
  existing Vortex contract machinery; and the scan example’s `tab`, `match` and `upto` calls match
  the vicon scanning design. These scanning functions are explicitly a vicon prerequisite: the
  vagent header says Vortex `std:goal` currently registers only `scan`, `subject` and `pos`, and the
  example imports the additional functions as specified-but-unbuilt. It does not claim they are
  currently exported or executable. The examples state the separate-unit/harness assumptions and do
  not overclaim wire ordering, implementation or device guarantees.

## Compact U-A1–U-A14 disposition

| Question | Incorporation disposition                                                                                                                                                                                                                                                                                            |
| -------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| U-A1     | Same-pid context replacement remains deferred and distinct from checkpoint/resume. Root/dependency inventory and transactional replacement requirements match the privately owned, nonmovable `WorkerContext` and `MeshActorRuntime` checkpoint refusal conditions.                                                  |
| U-A2     | Single-owner admission remains the initial boundary; the runtime checks authenticated owner identity for `ActorSend` and `ActorMonitor`. Cross-owner capabilities and relay authority remain deferred.                                                                                                               |
| U-A3     | Inline envelope limits remain; typed immutable handles and resolution are deferred with source identity, leases and bounded failure requirements. The actor runtime’s `ActorDeliverWriteSet` path is correlated to a pending delivery and is not represented as authority to apply the graph to agent or user state. |
| U-A4     | Hosted relay remains initial; its bottleneck/host-failure costs remain. Topic discovery is not treated as membership or authority.                                                                                                                                                                                   |
| U-A5     | Single-minded commitment remains initial, with drop-before-success precedence. Blind/open-minded modifiers are deferred.                                                                                                                                                                                             |
| U-A6     | Colon-separated names are scoped to vagent logical modules; family convergence is a separate migration and preserves existing VQL/sibling/import/store/content/loader distinctions.                                                                                                                                  |
| U-A7     | Restricted suspension positions remain initial. Reified continuations and their changed undo semantics are deferred.                                                                                                                                                                                                 |
| U-A8     | Per-agent captured replay is distinct from single-turn determinism, whole-node equivalence and crash recovery. Complete turn inputs, pinned semantics, stable semantic comparisons and suppressed real effects are required.                                                                                         |
| U-A9     | StoreWriter is unavailable initially; scoped authority, commit/retry recovery and delegated authorship under R8 remain separate decisions.                                                                                                                                                                           |
| U-A10    | The host-side deterministic harness is specified but owed. One-turn and run-until-idle drivers, semantic inspection, staged outbox visibility and separate runtime integration coverage are consistent with the existing code boundary.                                                                              |
| U-A11    | FIFO wire delivery remains. The adapter cannot reorder a message already dequeued by `pumpOne`; finite-progress host/ready/timer arbitration is baseline P4 work, and a fast lane remains deferred.                                                                                                                  |
| U-A12    | Selection stays fixed initially. A later selector is limited to ranking eligible untried plans and must retain deterministic fallback and isolation.                                                                                                                                                                 |
| U-A13    | Typed library protocols precede grammar. The Contract-Net example remains explicitly scoped to one node/owner and an injected clock, with complete source and Correlation binding prerequisites stated.                                                                                                              |
| U-A14    | Per-agent private programs remain initial. Existing arena/worker base support is not confused with actor-runtime installation; shared caching, ABI identity and source pinning remain measured future work.                                                                                                          |

Runtime API claims, lifecycle and root lists, module/stdlib boundaries, envelope limits, P4/FIFO
status and numeric costs reviewed here are consistent with the code and the specification-only
status. No deferred proposal inherits P1–P14 approval.

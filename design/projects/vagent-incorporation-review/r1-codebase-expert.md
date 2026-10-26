# vagent v3.8 incorporation review — codebase expert

## Verdict: BLOCKING

I reviewed the frozen v3.8 input (`r1-input.txt`; byte-identical to the target), its diff from
`79e1fa68`, the converged U-A1–U-A14 resolution note, and the cited Vortex, ZigZag and mesh runtime
paths. I found one actionable mismatch in the determinism claim. It is a documentation correction;
it does not require implementing deferred work. The known malformed §1 table rows are excluded from
this independent review per the parent’s note.

## MINOR — §14 omits scheduler selection from the claimed function input

At [design/vagent-agent-language.md:2099-2101](../../vagent-agent-language.md), §14 says a turn is a
function of its input and state, then defines input as “the message or host delivery and the turn
time.” But §4.2 says wake turns may run a ready intention, a due timer, `Initialize`, or a host
delivery ([lines 316-318](../../vagent-agent-language.md)); §11.1 leaves the adapter’s arbitration
among mailbox work, host deliveries, ready intentions and deadlines as required P4 design work
([lines 1566-1572](../../vagent-agent-language.md)). Thus a message and timestamp do not identify
the selected work for every turn, and selection can depend on scheduler policy/position while the
agent state is otherwise the same. The new U-A8 section itself recognizes this omitted input by
requiring traces to record “timer/wake selection” and external lifecycle decisions
([lines 2375-2379](../../vagent-agent-language.md)).

**Correction:** qualify §14’s determinism claim: define the full selected turn item (message, host
delivery/percept/effect result, timer, ready intention, initialization, or lifecycle decision), its
time, and the applicable fixed runtime/scheduler semantics as the turn input. State that this is
single-turn determinism conditional on those inputs, rather than implying message/host-delivery
input alone suffices. Keep §18.8’s larger trace requirement for captured replay.

## Compact U-A1–U-A14 disposition

| Question | Incorporation disposition                                                                                                                                                                                                                                                                                                                                                              |
| -------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| U-A1     | Correctly distinguishes quiescent checkpoint/resume from deferred same-pid context replacement; names the separate cursor/state root, VM/core/library dependencies, source leases, adapter state, atomic swap, and copy costs. Runtime code confirms nonmovable `WorkerContext`, private actor ownership, and checkpoint refusal with a running actor, mailbox, marks or choice depth. |
| U-A2     | Correctly keeps admission single-owner initially; `MeshActorRuntime::receive` enforces owner identity for `ActorSend`/`ActorMonitor`. Deferred capability/relay authority is not described as built.                                                                                                                                                                                   |
| U-A3     | Correctly retains the 32 KiB inline ceiling and defers the typed immutable handle/resolver. The lease, unavailable-source and no-implicit-apply requirements are scoped as future prerequisites.                                                                                                                                                                                       |
| U-A4     | Correctly retains host relay; host failure and bottleneck remain in the costs. No swarm discovery-as-authority claim.                                                                                                                                                                                                                                                                  |
| U-A5     | Correctly retains single-minded commitment and drop-before-success precedence; blind/open-minded behavior remains deferred.                                                                                                                                                                                                                                                            |
| U-A6     | Correctly scopes colon-separated names to logical module naming and keeps VQL `::`, sibling imports, store/content identities and loader paths outside the vagent decision.                                                                                                                                                                                                            |
| U-A7     | Correctly keeps restricted suspension positions and identifies continuation reification/undo semantics as later language work.                                                                                                                                                                                                                                                         |
| U-A8     | Correctly separates per-agent captured replay from single-turn determinism, whole-node timing and crash recovery; requires complete inputs, pinned semantics, output digests and suppressed external effects. §14 needs the scheduler-input correction above.                                                                                                                          |
| U-A9     | Correctly makes StoreWriter unavailable initially, retains R8 unchanged, and identifies authorization, commit, expiry, retry and crash reconciliation as extension prerequisites.                                                                                                                                                                                                      |
| U-A10    | Correctly specifies a host-side harness first, separates one-turn and run-until-idle drivers, requires semantic assertions and mesh integration tests, and does not require a second parser or durable trace.                                                                                                                                                                          |
| U-A11    | Correctly states FIFO wire delivery and that the adapter cannot reorder a message already dequeued by `pumpOne`; treats host/ready/timer arbitration and the bounded wake quota as baseline P4 work, not a deferred urgency extension.                                                                                                                                                 |
| U-A12    | Correctly keeps selection policies fixed initially and constrains a future selector to pure, bounded ranking over eligible untried plans.                                                                                                                                                                                                                                              |
| U-A13    | Correctly treats typed library protocols as the first step and grammar as deferred. §12.5 explicitly limits its example to same-node/same-owner injected-clock assumptions and calls out its incomplete source/correlation binding.                                                                                                                                                    |
| U-A14    | Correctly keeps per-agent private programs as baseline, acknowledges existing arena/worker base support without claiming actor-runtime installation, and defers a measured shared-base proposal with ABI and source pinning requirements.                                                                                                                                              |

No other actionable incorporation errors were found in the reviewed runtime API claims,
lifecycle/root lists, module and stdlib boundaries, envelope limits, FIFO/P4 status, or stated
numeric costs. Deferred proposals remain identified as unimplemented and do not inherit P1–P14
approval.

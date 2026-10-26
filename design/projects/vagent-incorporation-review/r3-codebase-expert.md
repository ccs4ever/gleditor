# vagent v3.8 incorporation review — codebase expert, R3

## Verdict: BLOCKING

I reviewed the frozen R3 input (`r3-input.txt`; byte-identical to
`design/vagent-agent-language.md`), the prior incorporation findings and fixes, the converged
U-A1–U-A14 recommendations, and the relevant Vortex, ZigZag and mesh runtime code. I found one MINOR
actionable syntax error in the new examples, which the parent independently identified for R4. I
found no additional independent incorporation issue. The specification remains explicitly
unimplemented; this review does not require implementing deferred proposals.

## MINOR — §12.6 uses a reserved keyword as a plan identifier

At [design/vagent-agent-language.md:2307](../../vagent-agent-language.md), the example declares
`plan event for WaitForEvent`. `event` is a reserved keyword (the lexical keyword list at §8.1,
lines 1036–1040), while `PlanDecl` requires an `Ident` for its name (§8.9, lines 1285–1286). This
unit therefore does not parse as written. Rename the plan to a non-keyword identifier such as
`wait_event` and adjust any prose that names it. This is the known R3 issue already queued for
repair in R4.

## Closure of prior findings and R3 additions

- The §1 three-column status map now has complete, correctly aligned rows for reclamation, large
  values, StoreWriter, testing, captured replay and shared programs.
- §14 now conditions single-turn determinism on the full selected input and pinned
  program/runtime/scheduler/grant/limits/settings. It expressly says a message or timestamp alone
  does not identify every wake and separates the claim from lifetime replay. This closes R1.
- §6.5 now limits its fixed ordering claims to within-source rules and explicitly leaves
  cross-source selection to P4. §11.1 requires the adapter’s P4 policy to be deterministic and
  versioned, to give admitted host work, ready intentions and due deadlines starvation-free service
  under sustained mailbox load, and to state a positive finite service bound using bounded
  queues/population. It correctly distinguishes actor-service turns from wall-clock guarantees.
  §18.11 identifies that as required baseline P4 work, not deferred fast-lane work.
- §5.6 now distinguishes restored ready ordering and deadline contents, which are derived from
  cells, from external delivery scheduling, which depends on the versioned P4 policy. This removes
  the inference that restoring cell data recreates an external arrival schedule.
- §12.6 labels its 20 snippets as independent specification examples, explains their harness
  assumptions, and states that they are not verified by an existing compiler. Apart from the
  reserved plan identifier finding above, I checked their syntax against §8.9 and the surrounding
  binding, failure, suspension, handler, goal and capacity rules. Their described outcomes match the
  specification: reversible `<-` is undone on failed alternatives; handler failure rolls back its
  writes/outbox; ordinary plan failure preserves earlier statements; belief and event awaits differ;
  `yield` permits but does not force message precedence; effect calls suspend plans; handler
  priority only orders handlers of a selected event; and stop/terminate use the stated orderly
  cleanup path. No snippet claims a transport order or guarantee that the spec denies.

## Compact U-A1–U-A14 disposition

| Question | Incorporation disposition                                                                                                                                                                                                                                                                                                                                                     |
| -------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| U-A1     | Same-pid replacement remains deferred and distinct from checkpoint/resume. The root/dependency inventory and old-context-preserving swap requirements match the private `MeshActorRuntime::Actor` context and nonmovable `WorkerContext`; current checkpoint refusal conditions support the stated quiescence distinction.                                                    |
| U-A2     | Single-owner admission remains initial. `MeshActorRuntime::receive` checks authenticated owner identity for `ActorSend`/`ActorMonitor`; proposed cross-owner capabilities and relay authority are not claimed as built.                                                                                                                                                       |
| U-A3     | Initial inline limits remain; handles/resolution are deferred with source identity, lease and failure requirements. In `MeshActorRuntime::receive`, `ActorDeliverWriteSet` is accepted only as a correlated pending reply (the separate monitor-exit exception applies only to `ActorDown`); the text does not grant authority to apply a received graph to agent/user state. |
| U-A4     | Hosted relay remains the initial space design. Host bottleneck/failure costs remain, and BEP 46 discovery is not treated as membership or authority.                                                                                                                                                                                                                          |
| U-A5     | Single-minded commitment remains fixed, with drop checked before success. Blind/open-minded modifiers are deferred.                                                                                                                                                                                                                                                           |
| U-A6     | Colon-separated names are the vagent logical-name target convention; family migration is separate and does not silently reinterpret VQL syntax, sibling imports, filesystem paths or store/content identities.                                                                                                                                                                |
| U-A7     | Suspension remains restricted to stated positions; continuation/undo behavior remains a separate later language change.                                                                                                                                                                                                                                                       |
| U-A8     | Per-agent captured replay is deferred and distinct from single-turn determinism, whole-node timing and crash recovery. The complete turn inputs, pinned semantics, semantic digests and suppression of real effects are specified.                                                                                                                                            |
| U-A9     | StoreWriter is unavailable initially. The R8 interpretation remains separate; authority, operation scope, atomic commit and retry/recovery requirements are not presented as approved implementation.                                                                                                                                                                         |
| U-A10    | The host-side deterministic harness is specified and owed; it separates one-turn and run-until-idle use, state/outbox inspection and runtime integration coverage. Durable replay and a second parser are not prerequisites.                                                                                                                                                  |
| U-A11    | FIFO wire delivery remains. The adapter’s inability to reorder a dequeued mailbox head matches `pumpOne`; host/ready/timer service is identified as finite-progress P4 baseline work, while urgency/priority changes remain deferred.                                                                                                                                         |
| U-A12    | Initial selection remains fixed. A future pure, bounded plan selector can only rank the already eligible untried set and must preserve isolation/fallback behavior.                                                                                                                                                                                                           |
| U-A13    | Typed library protocols precede any grammar. The Contract-Net transition example states its same-node/same-owner clock and no-migration scope, and identifies missing complete source/Correlation binding as a prerequisite.                                                                                                                                                  |
| U-A14    | Private per-agent programs remain baseline. Arena/worker immutable-base support is accurately separated from actor-runtime installation; shared caching, ABI identity and source pinning remain a measured future proposal.                                                                                                                                                   |

The current runtime claims, resource limits, lifecycle/root inventory, P4/FIFO status,
standard-library/module boundaries and numeric costs reviewed here are consistent with the code and
with the document’s specification-only status. No deferred proposal inherits P1–P14 approval.

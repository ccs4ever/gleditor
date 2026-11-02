# Remaining-work plan review, round 2: distributed-systems expert (persona distributed)

Scope: `remaining-plan.md` draft 2 (uncommitted working tree), `traceability.md` targets,
`decisions.md` section 2 (O1-O10, E1, T0, D1-D3) and my round-1 report
`plan-review/review-distributed.md`. Read against spec 6.3 (lines 780-897), 2.1 and 5.1. Code at
HEAD `8a4089d9`; `agent_executor.cpp` line numbers are `git show HEAD:` of that file, which is
unchanged by the three commits since `3ae8d384` except for documents. The working tree's uncommitted
`agent_executor.*` edits (X-27) are not cited.

Scratch: `/tmp/p19-distributed/floor_sim.py` (the floor simulation in M1).

## Earlier findings

| Round 1                                     | Draft 2                                                                                                                                                                                            | Verdict                         | Why                                                                                                                                                                                     |
| ------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| M1 floor and idempotency key                | I3-4b1 (event ID = issuer, incarnation, sequence; arena floor; fixed decision order), I3-4b2 (issuer side), `VerificationTooOld` terminal                                                          | **Partly resolved**             | Key, arena home, decision order, cached-before-expiry and the issuer owner are all fixed. What a floor *is* (how it advances) is still the draft-1 gap in a new form; see M1 below.     |
| M2 host-state classes                       | I3-4a and I3-4b1 class (a) transport refusals, class (b) inputs copied into the journal at intake; resume-with-no-grants test                                                                      | **Resolved**                    | The two classes and the test are what I asked for. One residual: create floor and journal state only at intake commit (m3).                                                             |
| M3 stable identity, rollback, double resume | Arena identity (name plus lineage ID) in I3-4b1; DECISION 2 with options (a) to (c); host lineage lease                                                                                            | **Partly resolved**             | Identity is resolved. Decision 2 states the right three options, but the lease that all three rely on has no owner, no mechanism and no test (M2 below).                                |
| M4 control items overtake                   | I3-2c2: per-pid ordinal, contract, quotas, tests (reset between two readings, isolate behind a held item, wake after `putBack`); arena epoch stays in I3-2c1                                       | **Resolved**                    | The two jobs are separated as I asked and both outcomes are tested. One contradiction in the wording is m2.                                                                             |
| M5 quiescence                               | T1 rewritten (no turn, no un-drained host event, no staged outbox, no unreplied send, no flight; journals are carried, not blocking); U-22 to I3-4b1, T-12 to I3-4c; race test; processing trigger | **Resolved, with a wrong hook** | The rule is now right and journals travel with their slice. The trigger named for upgrade does not fire on upgrade (m1). The mesh mailbox is a third queue the rule does not name (m4). |
| M6 signed bytes                             | I3-3a and I3-3D list audience, domain separation, canonical bytes, relay from transport, gateway analysis, certificate validity, tagged union                                                      | **Resolved**                    | All seven properties are listed. The tagged union must carry a common signed header including the floor (M1 asks for the field; no new finding).                                        |
| M7 no-op outcome                            | `RecordedNotApplied` deleted; I5-1a lands before I3-4c; real write set through `ledgerApply`                                                                                                       | **Resolved**                    | The reorder is correct and the dependency edge is in 4.1.                                                                                                                               |
| Minor 1 edges                               | I3-4a on I3-3a; I3-4c on I3-3c                                                                                                                                                                     | Resolved                        | In section 4.1.                                                                                                                                                                         |
| Minor 2 audit location                      | 8.P5-2 written in V-D, pinned in I3-4b1; citations to `:1715`, `:1755`                                                                                                                             | Resolved                        |                                                                                                                                                                                         |
| Minor 3 `putBack` wake                      | T0 covers `pushControl` and `putBack` with a blocked-reactor test                                                                                                                                  | Resolved                        |                                                                                                                                                                                         |
| Minor 4 charges                             | I3-2D states host charges are dropped as acceptable                                                                                                                                                | Resolved                        |                                                                                                                                                                                         |
| Minor 5 U-22, T-12                          | Re-pointed to I3-4b1, I3-4c                                                                                                                                                                        | Resolved                        |                                                                                                                                                                                         |
| Minor 6 control quotas                      | I3-2c2                                                                                                                                                                                             | Resolved                        |                                                                                                                                                                                         |
| Minor 7 token retention                     | I4-7b, I3-4b2, I4-D2 state retention at least the pending-record life                                                                                                                              | **Partly resolved**             | The inequality is stated; the unit, the eviction rule and where the setting is defined are not (m5).                                                                                    |
| Process economy                             | One review round for I3-4b1/b2/c; one-page message and crash table in V-D; crash matrix in I3-4c                                                                                                   | **Partly resolved**             | The matrix is named but its fault model is undefined under the checkpoint rule (M3).                                                                                                    |

## What is sound

- The I3-4 split is in the right dependency order. The receiver journal (b1) comes before the issuer
  (b2) and the processing turn (c); I5-1a before I3-4c gives the processing turn a real, atomic
  write set.
- Option A narrowed (I4-7b alone gates I3-4a) is correct for my area: verification arrives in a
  later turn, so the anchor must be arena state, and a token minted at the commit point survives
  resume.
- The host/arena split in section 2 is the right one, and it matches the code. `Ingress` bindings,
  aliases and floors, the mesh `Actor::floors` (`mesh_actor.cpp:74`, advanced at `:512-518`) and the
  agent-level floors are host memory; `ActorContinuation` (`mesh_actor.hpp:117-123`) carries only
  `completed`. The plan's statement that no existing floor is checkpointed is true.
- Stable identity is needed: `resume` assigns `ActorPid{node_, content, epoch_, nextSpawn_}`
  (`mesh_actor.cpp:330`) and rewrites remembered responses to it (`:337-343`).
- T1's refusal list now matches the upgrade guard at HEAD (`agent_executor.cpp:5068-5076`), plus the
  populated-intention refusal (`:5081`), which T0 types.
- Decision 2's recommendation (c) is cheap, honest, and consistent: a rolled-back arena loses the
  journal, the cached outcome and the label together, so the state stays self-consistent. What is
  lost is an acknowledged label, not integrity.
- W1 and W2 are in the right place (after the transport slices they exercise) and their gates
  include DIST and SEC.

## BLOCKING

None.

## MAJOR

### M1. The replay floor is "below a stored number"

Problem. I3-4b1 says the floor is "arena state per authenticated issuer" over (incarnation,
sequence) so that "below the floor is decidable and bounded". It never says how the floor
*advances*. The only reading available from the text and from the observation precedent
(`epistemic_ingress.hpp:26,54`, one incarnation and a monotone sequence per principal) is
highest-seen. That works for a sensor that never retransmits. It fails for verification, where spec
6.3 (line 830) requires retry: a request that was lost is retried after a later event from the same
issuer has been accepted, and the retry is then "below the floor" with no cached outcome. The plan
says the issuer treats `VerificationTooOld` as a terminal outcome of unknown commit state and does
not retry (I3-4b2). So a lost packet plus one more verification becomes a permanent,
operator-visible failure.

Evidence. `/tmp/p19-distributed/floor_sim.py` (one issuer, sequences 1..20,000, a request or reply
lost with probability p, retry after 3 time units, mean one-way delay 0.2; "highest-seen" refuses a
retry of a never-committed event once a higher sequence was accepted; the count includes first-send
reordering as well as retries):

```text
loss  issue gap  retry timeout  events ending TooOld, highest-seen floor
0.01     5.0         3.0             0.01 %
0.01     1.0         3.0             1.36 %
0.01     0.2         3.0            20.12 %
0.05     1.0         3.0             5.34 %
0.10     1.0         3.0            10.08 %
0.10     0.2         3.0            26.07 %
```

Under an issuer-declared low-watermark (below) the figure is zero in every row. The existing mesh
already has this shape: `Envelope.floor` is the sender's lowest outstanding request
(`AgentExecutor::floorFor`, `agent_executor.cpp:2908-2914`), not the highest sent.

Consequence. 6.3-23 to 6.3-26 hold only for a quiet issuer. The terminal-TooOld rule then turns
ordinary loss into lost labels with no recourse. Spec line 894 also requires a floor to outlive the
expiry of the cached outcome, so the floor cannot be a by-product of the cache.

Required fix. State in the I3-4b1 text, and as a "must settle" item of V-D, that:

1. The floor is an **issuer-declared low-watermark**: each signed verification carries the lowest
   sequence the issuer will still retransmit (everything below is terminal at the issuer). The
   receiver keeps a cached outcome for every sequence at or above the watermark and refuses below it
   with `VerificationTooOld`. The receiver raises the stored watermark only to a signed value; it
   never infers one.
1. The window (watermark to highest accepted) is bounded by a setting and by a cell price per entry.
   When it is full the refusal is a retryable class (a) error (`VerificationWindowFull`); nothing is
   evicted, because eviction makes a retry look too old.
1. The issuer's incarnation is monotone and refused below the stored one, as for observations. An
   issuer with a counter that can regress (an actor restored from a checkpoint, a host that lost its
   counter) must take a fresh incarnation on resume; this is amendment A3 applied to issuers. Reuse
   of an old (incarnation, sequence) with a different payload then fails safe as
   `DuplicateVerificationPayload`; say so.
1. Floor and journal state is created only when intake commits (m3), not when an authenticated but
   ungranted issuer is first seen.
1. **I3-3D** (not only V-D) fixes the signed message header common to every kind in the tagged
   union: issuer, incarnation, sequence, low-watermark, audience, expiry. Otherwise I3-4a changes
   the accepted wire format, which the plan promises it will not.
1. Tests: retry of an uncommitted event after a later sequence committed (the failing row above);
   at, inside and below the watermark; window full; incarnation regression; the watermark raised by
   an unsigned or replayed header (refused).

### M2. Decision 2 relies on a "lineage lease" that nothing implements, and (a) is misdescribed

Problem. Section 7.2 and the M3 resolution say "a double resume is a fork that the host prevents
with a lineage lease", and the recommendation makes the lease the one safeguard in option (c). There
is no lease mechanism in the tree. `MeshActorRuntime::resume` refuses a second resume only when the
same node's epoch has not advanced (`mesh_actor.cpp:248-250`) or the same request key is reused
(`:268-275`); a different request ID on the same node, or any other node, yields a second live actor
from the same checkpoint (`agent_executor.cpp:5038`, `resumes_++`). The failover scenario in the
decision text is a second node, where no check exists. A lease without fencing is also unsafe: host
A may still be alive across a partition while host B resumes, and A keeps acknowledging
verifications that B will never see.

Option (a) as written ("release the outcome only after a checkpoint covers it") cannot be done with
the staged-response design: `checkpoint` refuses while an outbox entry or an unanswered send exists
(`agent_executor.cpp:4937-4956`, `HostDeliveryPending`, `SendsIncomplete`). It is feasible only if
the outcome is released from the journal's cached copy after the checkpoint, not from the outbox,
and only a host can trigger a checkpoint (`checkpoint(pid)` needs the host's identity); the cost
line omits both.

Consequence. The lead's ruling D2 ("a host lineage lease that refuses a second live resume") has no
slice that builds it and no test that proves it. I3-4c is told to "implement decision 2" and test "a
double resume", with nothing specified. Acceptance would test an absent mechanism.

Required fix. Decide and write in the plan which of these I3-4c delivers, with the test:

1. **A local refusal** in `AgentExecutor::resume`: a checkpoint content hash that already has a live
   actor in this executor is refused with a typed error (cheap, testable, covers the in-process
   fork). The test is a double resume of one checkpoint.
1. **Cross-host fork prevention is an operator precondition**, stated in the user guide and in the
   F2 limits, not claimed as implemented. If the lead wants it enforced, option (b) with a lineage
   generation is the mechanism (the issuer ignores outcomes from a lower generation); it needs a
   receiver-to-issuer report and a bounded issuer-side retention rule, which should be priced in the
   option row.
1. Correct the option (a) row: the cost is a host-driven checkpoint on the verification path and a
   cached-copy release rule, and the staged response must live in the journal, not the outbox.
1. Add to decision 2 the failure it omits: rollback of the *issuer* (an actor restored from an older
   checkpoint resends from a regressed counter). M1 item 3 covers it; cite it here.

### M3. The I3-4c crash-point matrix has no fault model

Problem. I3-4c's acceptance is "a crash-point matrix (after intake, after processing, after the
response is staged, after it is sent) run against resume and upgrade". A checkpoint is refused while
anything is staged or pending, and upgrade is refused for the same reasons
(`agent_executor.cpp:4937- 4956`, `5068-5076`). So a checkpoint or an upgrade can never contain
"response staged but unsent". Either those rows run against something else, or they test the
refusal; the plan does not say which. In the other direction, what a restart from the last
checkpoint contains for each point depends on decision 2: under (c) every post-checkpoint point
collapses to the same state (journal entry absent, issuer pending depends on whether the outcome
reached it).

Consequence. The slice's main acceptance gate can pass with tests that do not exercise a crash, or
fail depending on which decision-2 option is chosen after the tests are written.

Required fix. V-D's one-page table, and the I3-4c test list, must define two fault models and give
the expected state per row and per decision-2 option:

1. **In-turn abort**: the processing turn is aborted at each statement boundary of its write set
   (record, ledger entry, cached outcome, staged response) by fault injection, and the enclosing
   mark rolls everything back together. Expected: journal still pending, no outcome, no label.
1. **Restart from the last checkpoint**: after intake, after processing, after the response was
   sent. Expected state for the actor (journal present or absent) and the issuer (pending record
   present or removed), and the observable result of the issuer's next retransmit.
1. For upgrade, the matrix rows are "refused as not quiescent" for staged-response points and "state
   carried" for committed points; list them.

## MINOR

1. **Wrong trigger after upgrade (I3-4c).** The text says "`needsFirstTurn_` is the hook" for the
   processing trigger after resume or upgrade. The upgrade tail does push the pid
   (`agent_executor.cpp:5492`), but `pumpUntilIdle` skips any pid whose state is prepared and
   `initialized` (`:3228`), and an upgraded actor keeps `initialized`. The hook works for resume
   only. Name a separate trigger (a scan of the arena queue at the end of `upgrade` and at first
   resume) and test an idle upgraded actor with a queued entry.
1. **Contradiction in I3-2c2.** "A reset does not overtake readings of its source admitted earlier"
   conflicts with "`putBack` keeps control items first". A held, earlier reading of the source would
   then run after the reset. State the precedence: the ordinal decides for a reset against readings
   of its own source; control-first applies to everything else. Keep both tests.
1. **Floor and journal creation.** Create per-issuer state only when intake commits (see M1 item 4).
   An authenticated issuer with no grant must leave no cell.
1. **Mesh mailbox is a third queue.** T1-D lists host posts and `hostEvents`, but the upgrade guard
   deliberately allows a non-empty mesh mailbox (`runtime_.pending(pid) != 0`, `:5112-5118`) unless
   an event layout changed. A verification intake message in that mailbox is consumed by the new
   program. T1-D must say that intake handling is program-independent, or add the mailbox to the
   refusal list for verification kinds.
1. **Token retention (I4-7b, I4-D2).** State the retention unit (turns or state-counter steps, not
   wall time, so replay is deterministic), that eviction never precedes expiry, and that at the cap
   minting refuses with a typed error rather than evicting. Define the pending-record life setting
   in I4-D2 and validate `retention >= pending life + retransmit bound` at settings load. At I4-7b
   the pending record does not exist yet (it arrives at I3-4b2), so the retention test there can
   only check the setting.
1. **Host state across in-process upgrade.** T1 says grants, mirror and ordinals "are not carried".
   An in-process upgrade keeps the pid (`State` is copied under the same pid,
   `agent_executor.cpp:5133`), so `Ingress` keyed by pid survives; only resume loses it. Say which
   happens where and test both, so an operator is not told to re-grant after an upgrade that did not
   need it.
1. **W1/W2 transport.** `vagent run` has no mesh peer for a file or stdin reading. State what
   carries `relayedBy` in the CLI (an in-process loopback peer, or "none, relay unsupported in W1")
   so the "relay marking from transport only" test is not silently dropped; and name the
   fault-injection hook W2 uses for "a reply is lost".

## Answers to the round-2 questions

1. **Is the floor over (issuer, incarnation, sequence) enough?** Not as a highest-seen number. It
   needs the issuer-declared low-watermark, a bounded window and an incarnation rule (M1). The key
   itself is right.
1. **Do the three options of Decision 2 cover the failure?** They cover rollback of the receiver.
   They omit rollback of the issuer, and none of them has a lease mechanism behind it. I agree with
   the recommendation (c) once the local refusal is specified and the cross-host precondition is
   stated (M2). Cross-host float determinism (D1) does not touch this.
1. **Is the quiescence rule now complete?** Yes for the host-side items, with the mailbox to be
   named (m4). The processing trigger it relies on is mis-specified for upgrade (m1).
1. **Does I3-4c cover the crash points?** Not yet. It names four points without a fault model (M3).

## Can the next work start on this plan

- **I3-2a, I3-2b, I3-2c1, I3-2c2: yes.** Nothing in my findings gates them. I3-2D should take m2
  (the `putBack` precedence) into its text.
- **I4-D1: yes.** No distributed content.
- **T1-D: yes**, with m4 (mailbox) and m6 (which host state survives an in-process upgrade) added to
  its list of things to settle.
- **I3-3D: yes**, but its brief must include the common signed header of M1 item 5 (issuer,
  incarnation, sequence, low-watermark, audience, expiry), because the tagged union is accepted at
  I3-3a and I3-4a promises not to change it. Add that sentence to the I3-3D row in section 4.4.
- **I4-D2: yes**, with m5 (retention unit, eviction, setting).
- **V-D, I3-4b1, I3-4b2, I3-4c: not until M1 to M3 are written into the plan** as "must settle" and
  acceptance text. They are plan edits of a paragraph each, not redesigns.

## Questions for the owner

None. Decision 2 remains the lead's (D2); my recommendation is (c) with the local refusal of M2 item
1 and the cross-host precondition stated, and (b) only if the owner wants cross-host failover
claimed.

Tally: 0 BLOCKING, 3 MAJOR, 7 MINOR.

Verdict: FAIL

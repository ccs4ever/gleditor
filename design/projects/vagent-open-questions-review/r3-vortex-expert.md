# R3 Vortex runtime review: vagent open questions

## Verdict: CONVERGED WITH ONE MINOR CLARIFICATION

R3 closes the previous actionable findings without implying that deferred mechanisms are implemented
or approved. U-A1 now inventories actor, VM, arena, and adapter roots/dependencies and defines
atomic replacement and rollback. U-A9 leaves StoreWriter unavailable pending an owner decision on
R8, and requires durable retry/commit reconciliation for a future crash-safe duplicate-prevention
claim. This is a requirement on the future StoreWriter boundary, not evidence that current
MeshActorRuntime duplicate suppression fails. U-A8 separates per-agent replay from whole-node timing
and explicitly requires global scheduling and host/transport capture for the latter. The R2
CFP-expiry finding is addressed with explicit event deadlines, a shared nondecreasing one-node
clock, and no-migration scope. R3 also gives concrete cancellation, work-intention drop,
late-result/effect, expiry, and bounded tombstone rules.

One minor validation bound remains for the worked protocol: an `Accept` can carry a `result_until`
at or before the participant's current time while satisfying the written upper bound. Exact
reference and correction follow.

## Finding

1. **[MINOR] Require a future `result_until` when accepting work.** R3 says `Accept` carries
   `result_until` “no later than `expires_at`” and requires an unexpired offer (proposal,
   `design/vagent-open-question-resolutions.md:359–368`, particularly lines 362–368). It does not
   require `now < result_until`. A coordinator can therefore send an `Accept` with an unexpired
   `offer_until` but a `result_until <= Clock.now()`; the participant meets the written acceptance
   checks and adopts work that is already past its execution deadline. **Correction:** require
   `now < result_until <= expires_at` when creating the `Accept`, and require `now < result_until`
   when the participant accepts; otherwise close/refuse the offer without adopting the work goal.
   This fits the explicitly shared, nondecreasing clock constraint
   (`design/vagent-open-question-resolutions.md:338–343`) and the rule that execution deadline drops
   the work goal (`design/vagent-open-question-resolutions.md:400–402`).

## Review of R3 runtime and lifecycle claims

- **U-A1 reclamation:** The prerequisite inventory now expressly includes actor cursor/state root,
  VM/core/stdlib roots and output shadows, base/attachments/readers/stores, and adapter
  continuations/callbacks; it forbids stale cursor maps and keeps the old context until validated
  installation (`design/vagent-open-question-resolutions.md:26–60`). This matches nonmovable context
  members (`apps/common/xanadu/vortex/worker_context.hpp:25–67`), external actor roots
  (`apps/common/xanadu/vortex/mesh_actor.cpp:31–49`;
  `apps/common/xanadu/vortex/mesh_actor.hpp:78–84`), and borrowed arena dependencies
  (`apps/common/xanadu/zigzag/arena_manifold.hpp:247–270`, `741–803`). The initial boundary
  accurately leaves checkpoint/resume as the available path and busy-agent memory unbounded until
  the extension exists.
- **U-A3 payload lifecycle:** The handle remains an extension; stable content identity, source
  reacquisition, unresolved-view state, shared lease lifetime, and pin acquisition before swap are
  all spelled out (`design/vagent-open-question-resolutions.md:77–114`). These are consistent with
  the current `ReceivedValue` content/source ownership contract
  (`apps/common/xanadu/vortex/mesh_value.hpp:96–120`). The current `ActorDeliverWriteSet` path
  remains correlated to a tracked delivery (`apps/common/xanadu/vortex/mesh_actor.cpp:422–449`), not
  unsolicited bulk ingress.
- **U-A8 deterministic replay:** Per-agent replay is explicitly ordered and excludes allocation
  refs/resource representation; whole-node replay separately requires global actor order and
  host/transport choices (`design/vagent-open-question-resolutions.md:178–204`). This fits the
  current actor-map/mailbox selection in `pumpOne`
  (`apps/common/xanadu/vortex/mesh_actor.cpp:459–476`) without claiming the proposed fair-pump
  behavior exists.
- **U-A9 StoreWriter:** The first-release boundary expressly disables StoreWriter and leaves the R8
  authorship interpretation pending (`design/vagent-open-question-resolutions.md:206–243`). The
  future stable operation id, atomic dedup/commit or recoverable equivalent, and
  lost-completion/restart tests address the crash window. Current mesh request deduplication is
  in-memory (`apps/common/xanadu/vortex/mesh_actor.cpp:328–353`) and request retry is separate
  (`mesh_actor.cpp:513–542`); R3 does not mistake either for crash-safe StoreWriter reconciliation.
- **U-A11 scheduler:** R3 makes ordinary P4 arbitration and wake-budget handling required adapter
  detail while deferring urgency changes (`design/vagent-open-question-resolutions.md:264–287`,
  incorporation map lines 439–440). This matches the spec’s message/wake distinction, including
  timers and host deliveries as wake turns (`design/vagent-agent-language.md:300–305`), and the
  adapter’s described work (`design/vagent-agent-language.md:1523–1529`). The mesh `pumpOne` remains
  mailbox-only (`apps/common/xanadu/vortex/mesh_actor.cpp:459–505`), consistent with the correction.
- **U-A13 worked protocol:** The example now fixes one node/owner, a shared nondecreasing injected
  clock, and no migration; carries explicit `expires_at`, `collect_until`, `offer_until`, and
  `result_until`; bounds and validates control events; and handles CFP/reject/cancel races, late
  effects/results, tombstones, record quotas, and loss-detection limits
  (`design/vagent-open-question-resolutions.md:338–419`). Its `occurrence.source` check is supported
  by the vagent event’s implicit source field (`design/vagent-agent-language.md:549–558`).
  Cancellation drops the tracked work intention and stages best-effort effect cancellation, while
  the completion handler checks conversation state; this matches the language’s boundary semantics
  for effects and cancellation (`design/vagent-agent-language.md:520–527`, `751–775`). The
  clock/no-migration scope avoids making distributed clock claims.
- **Incorporation map:** All fourteen questions have distinct “Now” and “Later” outcomes, and the
  text states these are proposals only (`design/vagent-open-question-resolutions.md:421–452`). No
  revision silently changes P1–P14 approval or R8.

## U-A1–U-A14 dispositions

| U-A | Disposition           | Review note                                                                                    |
| --- | --------------------- | ---------------------------------------------------------------------------------------------- |
| 1   | **Accept**            | Root/dependency inventory, atomic replacement, and initial checkpoint-only boundary are clear. |
| 2   | **Accept**            | Keeps first release single-owner and separates cross-owner authority.                          |
| 3   | **Accept**            | Typed handle and resolver remain deferred; source/lease lifecycle is specified.                |
| 4   | **Accept**            | Retains host relay and identifies later membership/partition questions.                        |
| 5   | **Accept**            | Keeps existing drop-first precedence and defers modifiers.                                     |
| 6   | **Accept**            | Limits colon naming recommendation to logical module names and stages migration.               |
| 7   | **Accept**            | Keeps current suspension positions; continuation semantics remain a separate extension.        |
| 8   | **Accept**            | Distinguishes per-agent replay from whole-node replay and durable recovery.                    |
| 9   | **Accept**            | StoreWriter remains unimplemented pending R8 and durable commit/recovery contract.             |
| 10  | **Accept**            | Harness assertions use semantic state and preserve staged outbox order.                        |
| 11  | **Accept**            | Requires baseline arbitration before any urgency extension.                                    |
| 12  | **Accept**            | Keeps current selector policy and constrains any later plan selector.                          |
| 13  | **Accept with minor** | Lifecycle/expiry model is sound; require `now < result_until` at acceptance.                   |
| 14  | **Accept**            | Shared immutable program remains a measured optimization with ownership prerequisites.         |

## Incorporation order

R3’s order is sound: define the U-A10 harness and initial boundaries; then specify U-A1 reclamation,
U-A3 content lifetimes, U-A11 ordinary P4 arbitration, and U-A8 per-agent replay. Treat U-A9’s R8
interpretation and StoreWriter contract separately. Measure before adopting U-A14; demonstrate
U-A13’s library and a need for U-A12 before adding syntax or selectors. Add the `result_until`
lower-bound check before adopting the Contract-Net library model.

This is a specification review only. No builds or tests were run, and no implementation or owner
approval is implied.

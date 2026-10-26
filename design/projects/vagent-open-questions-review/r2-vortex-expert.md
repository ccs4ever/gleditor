# R2 Vortex runtime review: vagent open questions

## Verdict: CONVERGED

R2 closes the three prior review findings without implying that the deferred mechanisms are
implemented or approved. U-A1 now inventories actor, VM, arena, and adapter roots/dependencies and
defines atomic replacement and rollback. U-A9 explicitly leaves StoreWriter unavailable pending an
owner decision on R8, and correctly requires durable retry/commit reconciliation for any future
crash-safe duplicate-prevention claim. This is a requirement on the future StoreWriter boundary; it
is not evidence that current MeshActorRuntime duplicate suppression fails. U-A8 now distinguishes
per-agent stream replay from whole-node timing and names global actor ordering and host/transport
decisions as prerequisites for a later whole-node mode.

I found no blocking or major issue in R2. One minor protocol detail should be explicit before the
worked Contract-Net example is treated as implementation-ready.

## Findings

1. **[MINOR] Carry the collection expiry in `CallForProposal`.** The worked protocol requires
   participants to reject expired requests even if their cancellation/rejection tombstone has
   expired (proposal, lines 372–376), but its event field description only says each event carries a
   conversation ID, with participant-specific events carrying an expected participant (lines
   350–354). A participant cannot independently enforce the collection deadline on a delayed CFP
   unless that absolute expiry (or an equivalent authenticated deadline) is part of the CFP. This
   matters because the host relay/runtime does not promise message ordering (vagent spec,
   `design/vagent-agent-language.md:953–954`). **Correction:** include the collection expiry in
   `CallForProposal`, have the participant compare it to turn time before accepting/responding, and
   retain the tombstone only as duplicate/cancellation state through the retry horizon. This does
   not change the recommendation’s same-owner scope or its claim boundaries.

## Closure of prior findings and runtime review

- **U-A1 root/dependency replacement:** Closed. The proposal enumerates the separately stored actor
  cursor and state root, VM/core/stdlib references and outputs, arena attachments and readers,
  source leases, adapter caches, pending completions and callback tokens (lines 31–45). It also
  requires zero marks/no running executor, a complete ref map, old-context retention until validated
  installation, and semantic paired-run testing (lines 20–60). This matches the present nonmovable
  `WorkerContext` members (`apps/common/xanadu/vortex/worker_context.hpp:25–67`), the external actor
  roots (`apps/common/xanadu/vortex/mesh_actor.cpp:31–49`;
  `apps/common/xanadu/vortex/mesh_actor.hpp:78–84`), and the arena’s borrowed base/store/reader and
  attachment metadata (`apps/common/xanadu/zigzag/arena_manifold.hpp:247–270`, `741–803`). The
  initial boundary now correctly keeps checkpoint/resume as the available path and admits busy-agent
  memory remains unbounded until a separate reclamation extension exists (proposal lines 47–53).
- **U-A3 payload lifecycle:** Scoped as an extension; the stable value is content identity/schema,
  with process-local leases reacquired and validated on resume, unresolved values not exposed as
  readable, and pins retained until all users are gone (proposal lines 77–114). That is consistent
  with current `ReceivedValue` ownership and its source lifetime contract
  (`apps/common/xanadu/vortex/mesh_value.hpp:96–120`). No claim that `ActorDeliverWriteSet` is
  unsolicited ingress; current receive logic correlates it to a pending delivery
  (`apps/common/xanadu/vortex/mesh_actor.cpp:422–449`).
- **U-A5 commitment:** The explicit drop-before-success precedence is grounded in vagent’s stated
  `continue(i)` order (`design/vagent-agent-language.md:805–808`) and is confined to preserving
  current semantics, with modifiers deferred (proposal lines 130–145).
- **U-A8 replay:** Closed. It makes canonical semantic comparison a harness prerequisite and
  excludes allocation refs/readers/cache representation (proposal lines 178–204). It limits initial
  replay to an agent’s ordered turn stream and explicitly defers whole-node timing until global
  actor order and host/transport decisions are captured (lines 183–192). That correctly accounts for
  current `pumpOne`’s actor-map scan and mailbox-head selection
  (`apps/common/xanadu/vortex/mesh_actor.cpp:459–476`) while not claiming the unimplemented
  fair-pump proposal is already present.
- **U-A9 StoreWriter/R8:** Closed at recommendation scope. StoreWriter remains unimplemented
  initially, and delegated authorship remains a proposed R8 interpretation rather than an amendment
  or consequence of P1–P14 (proposal lines 206–243). The stable id, atomic commit/dedup or
  equivalent recovery protocol, and explicit outcome after a lost completion address the future
  failure boundary. Current sends do deduplicate requests in memory
  (`apps/common/xanadu/vortex/mesh_actor.cpp:328–353`) and retry those messages
  (`mesh_actor.cpp:513–542`); that does not provide a durable transaction across process failure, as
  R2 correctly distinguishes.
- **U-A11 ordinary scheduler contract:** Improved. It removes the adapter-only wire fast-lane
  premise and requires the ordinary P4 host-delivery/ready-intention ordering, wake budget
  accounting, and due-deadline service before any urgency extension (proposal lines 264–287). This
  is consistent with the spec’s distinction between message and wake turns, where host deliveries
  and due timers are wake work (`design/vagent-agent-language.md:300–305`), and its adapter
  responsibilities (`design/vagent-agent-language.md:1523–1529`). `pumpOne` itself remains
  mailbox-only (`apps/common/xanadu/vortex/mesh_actor.cpp:459–505`), so R2 does not misstate current
  runtime behavior.
- **U-A13 protocol details:** The same-owner and typed-event boundaries are appropriate, and the
  sender check matches vagent’s implicit `source: Opt<Agent>` event field
  (`design/vagent-agent-language.md:549–558`). The correlation capacity is clearly a prerequisite,
  not a built feature. Add the explicit CFP expiry as noted above. Cancellation remains best effort
  for already-started effects, consistent with the spec (`design/vagent-agent-language.md:520–527`)
  and R2 (lines 372–376).
- **U-A14 shared program:** Correctly deferred until measurement and a separate runtime mechanism.
  Current `ArenaManifold` supports a non-owning immutable base with private shadows
  (`apps/common/xanadu/zigzag/arena_manifold.hpp:251–267`), while `WorkerContext` takes a borrowed
  base/reader pointer and is nonmovable (`apps/common/xanadu/vortex/worker_context.hpp:51–67`). R2’s
  per-actor base/reader pinning and private output/cursor/host state (proposal lines 319–336)
  address those constraints without claiming the existing actor runtime installs a shared base.

## U-A1–U-A14 dispositions

| U-A | Disposition           | Review note                                                                                         |
| --- | --------------------- | --------------------------------------------------------------------------------------------------- |
| 1   | **Accept**            | Root inventory, source lifetime, atomic replacement, and initial boundary are explicit.             |
| 2   | **Accept**            | Keeps first release single-owner; authorization is a separate extension.                            |
| 3   | **Accept**            | Handle/resolver is deferred; lease and unresolved-view lifecycle is specified.                      |
| 4   | **Accept**            | Retains host relay and separates discovery from membership/delivery semantics.                      |
| 5   | **Accept**            | Keeps current commitment and makes drop-first precedence explicit.                                  |
| 6   | **Accept**            | Recommends logical colon names while scoping migration separately.                                  |
| 7   | **Accept**            | Retains restricted suspension; continuation persistence is a distinct language change.              |
| 8   | **Accept**            | Separates per-agent replay from whole-node replay and pins canonical comparison as prerequisite.    |
| 9   | **Accept**            | StoreWriter remains unavailable pending authority, R8, commit, and recovery decisions.              |
| 10  | **Accept**            | Specifies stable state comparison, ordered staged outbox, manual clock, and integration coverage.   |
| 11  | **Accept**            | Corrects the wire-priority claim and requires baseline arbitration before any fast lane.            |
| 12  | **Accept**            | Keeps current policies fixed; the optional plan selector has safe eligibility/fallback constraints. |
| 13  | **Accept with minor** | Sound same-owner library direction; make CFP expiry explicit in the event contract.                 |
| 14  | **Accept**            | Treats shared immutable base as measured optimization with ownership and ABI prerequisites.         |

## Incorporation order

R2’s order is sound: specify the U-A10 harness and initial boundaries; then detail U-A1 reclamation,
U-A3 content lifetimes, U-A11 ordinary P4 arbitration, and U-A8 per-agent debug replay. Treat U-A9’s
R8 interpretation and StoreWriter contract separately. Measure before adopting U-A14; demonstrate
the U-A13 library and a need for U-A12 before adding syntax or selector hooks. Add the CFP expiry to
U-A13’s worked contract before adopting or presenting it as a complete library protocol.

This is a specification review only. No builds or tests were run, and no implementation or approval
is implied.

# Vortex runtime review: vagent open questions

## Overall verdict

The recommendations correctly treat U-A1–U-A14 as proposals, preserve the approved P1–P14 register,
and ground the main runtime boundaries in the code. The release boundaries for U-A2, U-A4, U-A5,
U-A7, and U-A11 are sound.

Revise U-A1 to inventory every live root and borrowed dependency before context replacement. Keep
U-A9 explicitly pending owner interpretation of R8 and specify reconciliation for a persistent write
whose completion is lost. U-A8 should make replay’s global scheduling order explicit. No
recommendation is blocked, and none is implementation evidence.

## Findings

1. **[MAJOR] U-A1 needs a complete root and dependency inventory.** The proposal calls for a
   “complete ref map” and asks the adapter to remap “any other live references” (proposal, lines
   20–30). But live roots sit outside the arena: `Actor` stores `cursor` and `stateRoot` separately
   (`apps/common/xanadu/vortex/mesh_actor.cpp:31–46`); `pumpOne` passes both into the executor
   (`apps/common/xanadu/vortex/mesh_actor.cpp:467–476`). The arena also holds non-owning base,
   store, and span-reader pointers, plus attached `Space` metadata
   (`apps/common/xanadu/zigzag/arena_manifold.hpp:251–271`, `741–803`). A copy that maps the state
   graph but leaves `actor.cursor` pointing into the old context can appear to succeed, then execute
   against a dead or mismatched arena. **Correction:** require a written root inventory covering
   actor and VM cursor roots, state root, pending continuations, external callback tokens, arena
   attachment metadata, and span-reader/base lifetimes; swap only after all have valid replacement
   mappings. The proposal’s failure-injection gate should assert these roots remain usable.

1. **[MAJOR] U-A9 needs retry/crash reconciliation and an explicit R8 decision point.** The proposal
   correctly labels delegated authorship as a proposed interpretation, since R8 says only
   user-generated updates persist (proposal, lines 150–164;
   `design/store-slice-convergence.md:363`). Mesh delivery retries resend the same request until
   acknowledged (`apps/common/xanadu/vortex/mesh_actor.cpp:513–542`), with actor-side request
   deduplication in `mesh_actor.cpp:328–353`. This is not evidence that the current mesh duplicate
   suppression fails. The open issue is the future StoreWriter effect boundary: if the store commit
   succeeds but the process crashes or loses its completion before recording/replaying that outcome,
   retrying may repeat the write unless commit and effect identity can be reconciled.
   **Correction:** make the delegation/effect identifier an idempotency key checked atomically with
   the target precondition, or specify a durable commit/outbox reconciliation protocol with
   equivalent semantics. Also specify when revocation is checked for already-staged or in-flight
   writes. Incorporate the R8 boundary as a pending owner decision, not as a settled rule.

1. **[MINOR] U-A8 should pin global scheduling order in replay.** The proposal records selected
   inputs and lifecycle decisions and pins runtime semantics (proposal, lines 130–146). For a
   node-level replay, explicitly record or reproduce the order in which actors are pumped: `pumpOne`
   scans the actor map and executes the first actor with mailbox work
   (`apps/common/xanadu/vortex/mesh_actor.cpp:459–476`). Interleaving can change when replies and
   effects become visible. **Correction:** define the replay unit as a globally ordered node
   execution trace, or state that per-agent replay omits cross-agent timing and cannot claim
   whole-node equivalence.

## U-A1–U-A14 dispositions

| Question                    | Disposition | Review note                                                                          |
| --------------------------- | ----------- | ------------------------------------------------------------------------------------ |
| U-A1 Reclaim busy arena     | **Revise**  | Root, cursor, attachment, and borrowed-reader mapping must be explicit.              |
| U-A2 Cross-owner spaces     | **Accept**  | Correctly keeps first release single-owner and separates admission design.           |
| U-A3 Large payloads         | **Accept**  | Correctly distinguishes handles from available content and keeps fetch asynchronous. |
| U-A4 Hostless swarm spaces  | **Accept**  | Correctly separates discovery from membership and delivery semantics.                |
| U-A5 Commitment strategies  | **Accept**  | Good first-release boundary and sensible conditions for later modifiers.             |
| U-A6 Module paths           | **Accept**  | Colon names are scoped to logical names; migration remains separate.                 |
| U-A7 Suspension anywhere    | **Accept**  | Correctly treats continuation reification as a language/runtime change.              |
| U-A8 Recorded streams       | **Revise**  | Make node-wide actor scheduling order explicit for whole-node replay.                |
| U-A9 Agents and R8          | **Revise**  | Keep R8 interpretation pending; specify idempotent writes and revocation timing.     |
| U-A10 Tests                 | **Accept**  | Separates one-turn harness from runtime integration tests.                           |
| U-A11 Fast lane             | **Accept**  | Correctly identifies FIFO mailbox selection in `pumpOne`; no interrupt guarantee.    |
| U-A12 Selection functions   | **Accept**  | Safely limits the proposed hook to eligible plans and defers scheduling hooks.       |
| U-A13 Protocol declarations | **Accept**  | Good desugaring-first approach; preserves transport’s weaker guarantees.             |
| U-A14 Shared program        | **Accept**  | Correctly accounts for private indexes/registrations and pins shared bases/readers.  |

## Incorporation order

Keep the proposed order with one adjustment: incorporate U-A9’s **boundary and unresolved R8
decision** early, but defer any rule that treats delegated agent writes as user-generated until the
owner decides that interpretation. Add the U-A1 root/dependency inventory and U-A8 global replay
ordering before specifying or pricing those extensions. The remaining order is reasonable: clarify
first-release boundaries and the test contract, then detail reclamation, payloads, and replay;
measure the private-program baseline before deciding U-A14; keep U-A12 and U-A13 as later
enhancements.

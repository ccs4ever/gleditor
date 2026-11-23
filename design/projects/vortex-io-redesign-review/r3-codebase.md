# Round 3 critique: Codebase Expert (vortex-io-redesign.md)

Paths below are relative to the repo root. Target SHA256:
`562373ff5f8e53c1ce94e0e0dc61a31ad4067bd90e261472bfb81669ddad2b02`. Tally: 1 BLOCKING, 1 MAJOR, 0
MINOR.

## Status of previous BLOCKING/MAJOR findings

- R2.1 (U-4 system-settings proposal): OPEN — the frozen target still has only P1–P4 in its
  register; U-4 still requires adding settings with schema/default/notes, but no P6 names the
  settings emitter (`design/vortex-io-redesign.md:651–660, 669`).

## BLOCKING

### 1. (§4, lines 225–241) Actor I/O changes are specified without the promised P5

**Problem:** The new actor contract requires refusing inline await before request admission,
admitting VAgent post-commit work asynchronously, delivering completions in later owner turns,
enforcing per-actor I/O grants, refusing checkpoints while service work is unresolved, and
reconciling late old-generation completions. These are changes to the existing `MeshActorRuntime`
and VAgent adapter path, but the frozen Proposal Register contains only P1–P4. This leaves a
required existing runtime change outside a numbered proposal with cost and fallback.

**Evidence:** Section 4 says an actor callback “may not inline-await” and “the actor bridge refuses
that placement with a typed outcome before admitting the request”; it also requires later-turn
completion, new grant limits, and `NotQuiescent` checkpoints for unresolved I/O
(`design/vortex-io-redesign.md:225–241`). The current `MeshActorRuntime::pumpOne` invokes the
executor synchronously inside the one message/wake turn
(`apps/common/xanadu/vortex/mesh_actor.hpp:244–250`;
`apps/common/xanadu/vortex/mesh_actor.cpp:611–619`). `ActorGrant` has instruction, cell, output,
solution, and mailbox limits, but no I/O byte or request limit
(`apps/common/xanadu/vortex/mesh_actor.hpp:60–74`). `checkpointView` checks actor execution,
mailbox/wake/choice/mark state, and delivery records, but has no IoService-request condition
(`apps/common/xanadu/vortex/mesh_actor.cpp:899–912`). The target register lists P1–P4 only and then
defers any additional existing runtime file to a later gate
(`design/vortex-io-redesign.md:651–660`).

**Fix:** Add the promised P5 with exact actor runtime, grant/wire, and VAgent adapter files that the
implementation requires; explain why the owner cannot implement this in a caller without violating
the one-turn runtime contract; price queued completions, retained request state, grant metadata, and
checkpoint checks; and state a fallback that leaves actor I/O unavailable until P5 is approved. Keep
completion delivery at message boundaries and preserve existing VAgent post-commit admission.

## MAJOR

### 1. (§8.3, §13 U-4) User-adjustable budget settings remain unregistered

**Problem:** The v0.4 target did not incorporate the proposed P6 described in the round-two
disposition. Its U-4 gate still requires adding system settings, so budget configurability has no
numbered proposal for the existing settings implementation. The generic promise to price additional
files before implementation avoids silent edits but does not make this already-required change
reviewable now.

**Evidence:** The target says each user-adjustable budget class needs a schema/default/notes entry
and U-4 says the experiments “then add system settings with schema/default/notes”
(`design/vortex-io-redesign.md:465–469, 669`). The settings emitter is `defaultSettingSpecs`
(`apps/common/xanadu/system_docs.cpp:538–545`) and settings are materialized through
`ensureAllSettings` (`apps/common/xanadu/system_docs.cpp:2356–2367`). No P6 appears in the target
register (`design/vortex-io-redesign.md:651–660`).

**Fix:** Add P6 naming `system_docs.hpp/.cpp` and `vortex_host.hpp/.cpp`, with the settings covered,
schema/default/notes and migration cost, and the reason host-only configuration is insufficient for
user-adjustable limits. Until that proposal is approved, retain U-4's immutable host/service
snapshot and named backend invariants as the fallback.

## MINOR

None.

VERDICT: NOT CONVERGED

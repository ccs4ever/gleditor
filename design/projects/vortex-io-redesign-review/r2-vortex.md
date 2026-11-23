# Round 2 critique: Vortex & Spool-Mesh Expert (vortex-io-redesign.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 1 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R1.1 (Pending execution has no scoped core proposal): RESOLVED — v0.3 adds P1 for typed core
  status, parked VM continuation, and owner pumping, plus P4 for shared language bridge migration
  and explicit no-feature fallbacks (§12, lines 625–640). The design now explains why this cannot be
  implemented in each caller and identifies costs to measure.

## BLOCKING

None.

## MAJOR

### 1. (§4, §7.1, §12) Pending actor I/O lacks a turn/checkpoint contract

**Problem:** The shared language contract and P4 make VAgent one of the bridges receiving async
submit/await, but the proposal does not decide how an await that suspends inside a Spool-Mesh actor
interacts with its mailbox turn, VAgent execution, actor grants, and checkpoint boundary. The
existing `MeshActorRuntime` invokes its executor synchronously from `pumpOne`; its result type has
only completed output or error. An error stops the actor and emits `ActorDown`, so it cannot
represent Pending. Keeping that call open while awaiting would hold the owner-thread pump. Returning
Pending without actor integration would leave the consumed message/turn and actor continuation
without specified treatment. The portable actor continuation explicitly excludes VM PCs and native
callbacks, so a pending turn cannot be assumed to survive checkpoint/resume. P1 names Vortex
core/VM/host files, and P4 names the VAgent adapter, but neither includes nor defines the
`MeshActorRuntime` adapter boundary. This leaves a supported language bridge with materially
different effect and recovery semantics.

**Evidence:** The shared contract says each bridge shares submit/await rules (§4, lines 203–223),
and the bridge table says VAgent preserves post-commit admission and correlation (§4, lines
180–187). Pending await is a distinct execution outcome with retained continuation (§7.1, lines
363–374). In current code, `MeshActorRuntime::Executor` returns only
`std::expected<std::string, ActorError>` (`apps/common/xanadu/vortex/mesh_actor.hpp:165-168`);
`pumpOne` invokes it inline and treats a failed result as terminal, setting the actor stopped
(`apps/common/xanadu/vortex/mesh_actor.cpp:611-624`, `682-686`). The public `pumpOne` contract
executes at most one mailbox/wake turn (`apps/common/xanadu/vortex/mesh_actor.hpp:244-250`).
Portable `ActorContinuation` is only a message-boundary record; VM PCs and native callbacks stay
local (`apps/common/xanadu/vortex/mesh_actor.hpp:144-152`). P1 scopes core, VM, and host; P4 scopes
VAgent compiler/executor/adapter, with no `mesh_actor.hpp/.cpp` integration or actor-specific
fallback (§12, lines 634–640).

**Fix:** Decide and state whether actor VAgent programs may submit/await service I/O. If supported,
add a separate proposal for the exact `apps/common/xanadu/vortex/mesh_actor.hpp/.cpp` and
actor-adapter files that must represent a parked turn. Specify that `pumpOne` returns without
blocking, retains the consumed message and same actor continuation, does not run another turn in its
place, and resumes only the same request on the actor's owner. Define how actor grants authorize and
bound such work, how completion and late completion are tied to actor/endpoint generations without
cross-arena `CellRef`s, and whether checkpoint is refused or deferred until the actor reaches the
existing message boundary. On restart, a pending external effect must remain an unknown/terminal
observation or otherwise reconcile its original request; checkpoint restore cannot submit it again.
If actor I/O is out of scope, say so explicitly and make actor VAgent submit fail with a typed
refusal; do not let the generic VAgent bridge imply support. Price retained message/continuation and
completion bytes against actor grant/runtime bounds, and state the no-blocking/no-resubmit fallback.

## MINOR

None.

VERDICT: NOT CONVERGED
